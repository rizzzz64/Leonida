// Scratch buffers that live until the submission that used them has completed: device-local chunks handed out
// while a list is prepared, retired with the submission's timeline value, and reused (or freed) once the GPU is past
// it. The replay stages copies Vulkan cannot do in one command through them (vulkan_replay.h, depth <-> colour
// copies). Single-threaded: the caller holds the context's lock. The same life cycle as the depth24 converter's own
// scratch (vulkan_depth24.h), which also needs descriptor pools and exists only on GPUs without D24.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace leonida {

class ScratchBuffers {
public:
    // A device-local allocation for a chunk; null on failure.
    using Allocate = std::function<VkDeviceMemory(const VkMemoryRequirements&)>;

    ScratchBuffers(PFN_vkGetDeviceProcAddr get, VkDevice device, Allocate allocate)
        : device_(device), allocate_(std::move(allocate)) {
        loaded_ = vk_.load(get, device);
    }

    ~ScratchBuffers() {
        if (!loaded_) return;
        vk_.vkDeviceWaitIdle(device_);
        release(current_);
        for (auto& batch : retired_) release(batch);
        for (auto& chunk : spare_) destroy(chunk);
    }

    ScratchBuffers(const ScratchBuffers&) = delete;
    ScratchBuffers& operator=(const ScratchBuffers&) = delete;

    bool loaded() const { return loaded_; }

    // `bytes` of transfer scratch for the next submission, at a 256-byte aligned offset (more than any copy needs).
    bool get(std::uint64_t bytes, VkBuffer& buffer, VkDeviceSize& offset) {
        constexpr VkDeviceSize alignment = 256, minimum = 4u << 20;
        if (!loaded_ || !bytes) return false;
        for (auto& chunk : current_.chunks) {
            const VkDeviceSize padding = (alignment - chunk.used % alignment) % alignment;
            if (padding > chunk.size - chunk.used) continue;
            const VkDeviceSize start = chunk.used + padding;
            if (bytes <= chunk.size - start) {
                chunk.used = start + bytes;
                buffer = chunk.buffer;
                offset = start;
                return true;
            }
        }
        // Prepare ownership bookkeeping before acquiring a buffer. In particular, reserve the
        // retirement slot before any commands can reference this batch: retire() also runs in a
        // destructor and after vkQueueSubmit, where an allocation failure is too late.
        try {
            if (current_.chunks.empty()) reserve_one(retired_);
            reserve_one(current_.chunks);
        } catch (const std::bad_alloc&) {
            return false;
        } catch (const std::length_error&) {
            return false;
        }

        Chunk chunk;
        for (std::size_t i = 0; i < spare_count_; ++i)
            if (spare_[i].size >= bytes) {
                chunk = spare_[i];
                for (std::size_t j = i + 1; j < spare_count_; ++j) spare_[j - 1] = spare_[j];
                spare_[--spare_count_] = {};
                break;
            }
        if (!chunk.buffer) {
            try {
                if (!make_chunk(std::max<std::uint64_t>(bytes, minimum), chunk)) return false;
            } catch (const std::bad_alloc&) {
                return false;
            }
        }
        chunk.used = bytes;
        current_.chunks.push_back(chunk);  // capacity was reserved above
        buffer = chunk.buffer;
        offset = 0;
        return true;
    }

    // The chunks handed out since the last call belong to a submission that `timeline` reaches `ticket` for
    // (ticket 0: nothing was submitted, they are free once `collect` runs).
    void retire(VkSemaphore timeline, std::uint64_t ticket) noexcept {
        if (current_.chunks.empty()) return;
        current_.timeline = timeline;
        current_.ticket = ticket;
        retired_.push_back(std::move(current_));
        current_ = {};
    }

    // The submissions on `timeline` are over and the semaphore is about to go (its queue is idle).
    void forget(VkSemaphore timeline) {
        for (std::size_t i = 0; i < retired_.size();) {
            if (retired_[i].timeline != timeline) {
                ++i;
                continue;
            }
            release(retired_[i]);
            retired_.erase(retired_.begin() + std::ptrdiff_t(i));
        }
    }

    // Keeps for reuse (or frees) the chunks of finished submissions.
    void collect() {
        for (std::size_t i = 0; i < retired_.size();) {
            if (retired_[i].ticket != 0) {
                std::uint64_t done = 0;
                if (!retired_[i].timeline ||
                    vk_.vkGetSemaphoreCounterValue(device_, retired_[i].timeline, &done) != VK_SUCCESS ||
                    done < retired_[i].ticket) {
                    ++i;
                    continue;
                }
            }
            for (auto& chunk : retired_[i].chunks) {
                chunk.used = 0;
                if (spare_count_ < spare_.size()) {
                    spare_[spare_count_++] = chunk;
                    chunk = {};
                } else destroy(chunk);
            }
            retired_[i].chunks.clear();
            retired_.erase(retired_.begin() + std::ptrdiff_t(i));
        }
    }

private:
    template<class T>
    static void reserve_one(std::vector<T>& entries) {
        if (entries.size() < entries.capacity()) return;
        if (entries.size() == entries.max_size()) throw std::length_error("scratch batch capacity exhausted");
        const auto extra = std::max<std::size_t>(1, entries.size() / 2);
        entries.reserve(entries.size() + std::min(extra, entries.max_size() - entries.size()));
    }

    struct Chunk {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0, used = 0;
    };
    struct Batch {
        std::vector<Chunk> chunks;
        VkSemaphore timeline = VK_NULL_HANDLE;
        std::uint64_t ticket = 0;
    };

    struct Functions {
#define LEONIDA_SCRATCH_FUNCTIONS(X)                                                                           \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkFreeMemory) \
    X(vkGetSemaphoreCounterValue) X(vkDeviceWaitIdle)
#define LEONIDA_SCRATCH_MEMBER(name) PFN_##name name = nullptr;
        LEONIDA_SCRATCH_FUNCTIONS(LEONIDA_SCRATCH_MEMBER)
#undef LEONIDA_SCRATCH_MEMBER

        bool load(PFN_vkGetDeviceProcAddr get, VkDevice device) {
            bool all = true;
#define LEONIDA_SCRATCH_LOAD(name)                              \
    name = reinterpret_cast<PFN_##name>(get(device, #name));    \
    all = all && name != nullptr;
            LEONIDA_SCRATCH_FUNCTIONS(LEONIDA_SCRATCH_LOAD)
#undef LEONIDA_SCRATCH_LOAD
#undef LEONIDA_SCRATCH_FUNCTIONS
            return all;
        }
    };

    bool make_chunk(std::uint64_t size, Chunk& out) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vk_.vkCreateBuffer(device_, &info, nullptr, &out.buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vk_.vkGetBufferMemoryRequirements(device_, out.buffer, &requirements);
        try {
            out.memory = allocate_ ? allocate_(requirements) : VK_NULL_HANDLE;
        } catch (...) {
            destroy(out);
            throw;
        }
        if (!out.memory || vk_.vkBindBufferMemory(device_, out.buffer, out.memory, 0) != VK_SUCCESS) {
            destroy(out);
            return false;
        }
        out.size = size;
        return true;
    }
    void destroy(Chunk& chunk) {
        if (chunk.buffer) vk_.vkDestroyBuffer(device_, chunk.buffer, nullptr);
        if (chunk.memory) vk_.vkFreeMemory(device_, chunk.memory, nullptr);
        chunk = {};
    }
    void release(Batch& batch) {
        for (auto& chunk : batch.chunks) destroy(chunk);
        batch.chunks.clear();
    }

    VkDevice device_;
    Allocate allocate_;
    Functions vk_;
    bool loaded_ = false;
    Batch current_;               // handed out, not yet submitted
    std::vector<Batch> retired_;  // submitted, waiting for the GPU
    std::array<Chunk, 4> spare_{};  // finished chunks kept for reuse, no allocations in collect()
    std::size_t spare_count_ = 0;
};

}  // namespace leonida

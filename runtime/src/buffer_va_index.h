#pragma once

// Which registered buffer a console VA falls in (VulkanContext::buffer_at, for vertex and index views and
// WriteBufferImmediate). No Vulkan or Windows dependency; the caller guards this index with resource_mutex_.
// D3D12 placed buffers can alias. Prefer the greatest (start, id) containing the whole requested span,
// or the greatest (start, id) containing its first byte if none fits. A zero byte request counts as one.
// An AVL tree caches each subtree's greatest inclusive endpoint: unlike a global longest-range bound,
// one large live buffer cannot force a walk over thousands of unrelated short buffers. Lookup and updates
// are O(log n), including overlapping ranges. Endpoints use 65 bits to preserve the old subtraction-based
// containment rule even for ranges whose mathematical end exceeds UINT64_MAX.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace leonida {

template<typename Id>
class BufferVaIndex {
public:
    BufferVaIndex() = default;
    BufferVaIndex(const BufferVaIndex& other) : root_(clone(other.root_.get())) {}
    BufferVaIndex& operator=(const BufferVaIndex& other) {
        if (this != &other) root_ = clone(other.root_.get());
        return *this;
    }
    BufferVaIndex(BufferVaIndex&&) noexcept = default;
    BufferVaIndex& operator=(BufferVaIndex&&) noexcept = default;

    void insert(std::uint64_t va, std::uint64_t size, Id id) {
        if (va && size) insert_node(root_, {va, id}, size);
    }

    void erase(std::uint64_t va, Id id) { erase_node(root_, {va, id}); }

    struct Hit {
        Id id;
        std::uint64_t start;
        std::uint64_t size;
    };

    bool find(std::uint64_t va, Hit& out, std::uint64_t bytes = 1) const {
        if (!bytes) bytes = 1;
        const Node* hit = search(root_.get(), va, endpoint(va, bytes));
        if (!hit && bytes != 1) hit = search(root_.get(), va, endpoint(va, 1));
        if (!hit) return false;
        out = {hit->key.second, hit->key.first, hit->size};
        return true;
    }

    std::size_t size() const { return root_ ? root_->count : 0; }
    std::uint64_t longest() const { return root_ ? root_->longest : 0; }

private:
    using Key = std::pair<std::uint64_t, Id>;
    using End = std::pair<bool, std::uint64_t>;
    static End endpoint(std::uint64_t start, std::uint64_t size) {
        const auto last = start + (size - 1);
        return {last < start, last};
    }

    struct Node {
        Node(Key k, std::uint64_t s) : key(k), size(s), last(endpoint(k.first, s)), longest(s) {}
        Key key;
        std::uint64_t size;
        End last;
        std::uint64_t longest;
        std::size_t count = 1;
        int height = 1;
        std::unique_ptr<Node> left, right;
    };
    using Ptr = std::unique_ptr<Node>;

    static int height(const Ptr& n) { return n ? n->height : 0; }
    static int balance(const Ptr& n) { return height(n->left) - height(n->right); }

    static void refresh(Node& n) {
        n.last = endpoint(n.key.first, n.size);
        n.longest = n.size;
        n.count = 1;
        n.height = 1 + std::max(height(n.left), height(n.right));
        for (const Node* child : {n.left.get(), n.right.get()}) {
            if (!child) continue;
            n.last = std::max(n.last, child->last);
            n.longest = std::max(n.longest, child->longest);
            n.count += child->count;
        }
    }

    static void rotate_left(Ptr& n) {
        Ptr pivot = std::move(n->right);
        n->right = std::move(pivot->left);
        refresh(*n);
        pivot->left = std::move(n);
        refresh(*pivot);
        n = std::move(pivot);
    }

    static void rotate_right(Ptr& n) {
        Ptr pivot = std::move(n->left);
        n->left = std::move(pivot->right);
        refresh(*n);
        pivot->right = std::move(n);
        refresh(*pivot);
        n = std::move(pivot);
    }

    static void rebalance(Ptr& n) {
        if (!n) return;
        refresh(*n);
        if (balance(n) > 1) {
            if (balance(n->left) < 0) rotate_left(n->left);
            rotate_right(n);
        } else if (balance(n) < -1) {
            if (balance(n->right) > 0) rotate_right(n->right);
            rotate_left(n);
        }
    }

    static void insert_node(Ptr& n, Key key, std::uint64_t size) {
        if (!n) {
            // Allocate before changing the tree; a failed allocation leaves all ancestors untouched.
            n = std::make_unique<Node>(key, size);
            return;
        }
        if (key < n->key) insert_node(n->left, key, size);
        else if (n->key < key) insert_node(n->right, key, size);
        else n->size = size;  // replacement is allocation-free
        rebalance(n);
    }

    static void erase_node(Ptr& n, Key key) {
        if (!n) return;
        if (key < n->key) erase_node(n->left, key);
        else if (n->key < key) erase_node(n->right, key);
        else {
            if (!n->left || !n->right) {
                // Keep the old node alive until its child has been moved out.
                Ptr old = std::move(n);
                n = old->left ? std::move(old->left) : std::move(old->right);
                return;
            }
            const Node* successor = n->right.get();
            while (successor->left) successor = successor->left.get();
            n->key = successor->key;
            n->size = successor->size;
            erase_node(n->right, n->key);
        }
        rebalance(n);
    }

    // Reverse in-order search of the prefix start <= va. An endpoint bound discards a whole subtree
    // if even its longest-reaching range ends before the requested final byte.
    static const Node* search(const Node* n, std::uint64_t va, End last) {
        if (!n || n->last < last) return nullptr;
        if (n->key.first > va) return search(n->left.get(), va, last);
        if (const Node* right = search(n->right.get(), va, last)) return right;
        if (endpoint(n->key.first, n->size) >= last) return n;
        return search(n->left.get(), va, last);
    }

    static Ptr clone(const Node* n) {
        if (!n) return {};
        Ptr copy = std::make_unique<Node>(n->key, n->size);
        copy->left = clone(n->left.get());
        copy->right = clone(n->right.get());
        refresh(*copy);
        return copy;
    }

    Ptr root_;
};

} // namespace leonida

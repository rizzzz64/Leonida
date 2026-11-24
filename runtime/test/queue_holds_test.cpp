// CPU-only regression for per-device queue pumping and exception unwinding.
#include "../src/queue_holds.h"
#include <cstdio>
#include <stdexcept>
#include <vector>

using Holds = leonida::QueueHolds<unsigned>;

static bool nested_devices() {
    Holds a, b;
    std::vector<int> order;
    a.after_held(&a, [&] {
        order.push_back(1);
        b.after_held(&b, [&] {
            order.push_back(2);
            a.after_held(&a, [&] { order.push_back(4); });
        });
        order.push_back(3);
    });
    return order == std::vector<int>{1, 2, 3, 4};
}

static bool same_device_reentry() {
    Holds a;
    std::vector<int> order;
    a.after_held(&a, [&] {
        order.push_back(1);
        a.after_held(&a, [&] { order.push_back(3); });
        order.push_back(2);
    });
    return order == std::vector<int>{1, 2, 3};
}

static bool callback_exception() {
    Holds a;
    bool caught = false, ran = false;
    try {
        a.after_held(&a, [] { throw std::runtime_error("synthetic failure"); });
    } catch (const std::runtime_error&) { caught = true; }
    a.after_held(&a, [&] { ran = true; });
    return caught && ran;
}

struct Held final : Holds::Holder {
    Holds& owner;
    std::uint64_t seq;
    bool discarded = false;
    bool throw_on_discard = false;
    explicit Held(Holds& a) : owner(a), seq(a.next_seq()) {
        owner.attach(this);
        owner.held_changed(1);
    }
    ~Held() {
        if (seq) owner.held_changed(-1);
        owner.detach(this);
    }
    bool release_ready() override { return false; }
    std::uint64_t oldest_held_seq() override { return seq; }
    void discard_waiting_on(unsigned sem) override {
        if (sem != 7 || !seq) return;
        if (throw_on_discard) throw std::runtime_error("synthetic discard failure");
        discarded = true;
        seq = 0;
        owner.held_changed(-1);
    }
};

static bool cross_device_discard() {
    Holds a, b;
    b.add_fence(7, 0);
    bool ran = false;
    Held held(b);
    b.after_held(&b, [&] { ran = true; });
    a.after_held(&a, [&] { b.remove_fence(7); });
    return held.discarded && ran;
}

static bool discard_exception() {
    Holds a;
    a.add_fence(7, 0);
    bool caught = false, ran = false;
    Held held(a);
    held.throw_on_discard = true;
    a.after_held(&a, [&] { ran = true; });
    try { a.remove_fence(7); }
    catch (const std::runtime_error&) { caught = true; }
    held.throw_on_discard = false;
    a.remove_fence(7);
    return caught && held.discarded && ran;
}

int main(int argc, char** argv) {
    // Run one case per process so the old sticky thread-local flag cannot contaminate other tests.
    if (argc != 2) return 2;
    const int i = argv[1][0] - '0';
    using Test = bool(*)();
    const Test tests[]{nested_devices, same_device_reentry, callback_exception, cross_device_discard, discard_exception};
    if (i < 0 || i >= 5) return 2;
    const bool ok = tests[i]();
    std::printf("queue_holds case %d: %s\n", i, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

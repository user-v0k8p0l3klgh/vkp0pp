// Bounded single-producer/single-consumer ring. The render thread produces
// frame records, the diagnostics thread consumes them. When full, push fails
// and the producer counts a drop instead of blocking.
#pragma once
#include <array>
#include <atomic>
#include <cstddef>

namespace bll {

template <typename T, size_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "capacity must be a power of two");

public:
    bool push(const T& v) {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_acquire);
        if (h - t == N) return false;
        slots_[h & (N - 1)] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t h = head_.load(std::memory_order_acquire);
        if (t == h) return false;
        out = slots_[t & (N - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    size_t size() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

private:
    std::array<T, N> slots_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

} // namespace bll

// Fixed-capacity rolling sample window with order statistics.
// push() is O(1) and allocation-free; the summary functions copy and sort and
// are meant for the diagnostics thread, not the render thread.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace bll {

template <typename T, size_t N>
class RollingWindow {
public:
    void push(T v) {
        data_[head_] = v;
        head_ = (head_ + 1) % N;
        if (count_ < N) ++count_;
    }
    void clear() { head_ = count_ = 0; }
    size_t size() const { return count_; }
    bool full() const { return count_ == N; }
    static constexpr size_t capacity() { return N; }

    double mean() const {
        if (!count_) return 0.0;
        double s = 0;
        for (size_t i = 0; i < count_; ++i) s += static_cast<double>(data_[i]);
        return s / static_cast<double>(count_);
    }

    double stddev() const {
        if (count_ < 2) return 0.0;
        const double m = mean();
        double s = 0;
        for (size_t i = 0; i < count_; ++i) {
            const double d = static_cast<double>(data_[i]) - m;
            s += d * d;
        }
        return std::sqrt(s / static_cast<double>(count_ - 1));
    }

    // Nearest-rank percentile, p in [0, 100].
    T percentile(double p) const {
        if (!count_) return T{};
        std::array<T, N> tmp;
        std::copy_n(data_.begin(), count_, tmp.begin());
        const double rank = std::ceil(p / 100.0 * static_cast<double>(count_));
        size_t k = rank < 1.0 ? 0 : static_cast<size_t>(rank) - 1;
        if (k >= count_) k = count_ - 1;
        std::nth_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(k),
                         tmp.begin() + static_cast<std::ptrdiff_t>(count_));
        return tmp[k];
    }

    T max() const {
        return count_ ? *std::max_element(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(count_)) : T{};
    }

private:
    std::array<T, N> data_{};
    size_t head_ = 0;
    size_t count_ = 0;
};

} // namespace bll

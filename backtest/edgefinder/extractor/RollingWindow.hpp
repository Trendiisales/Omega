#pragma once
// =============================================================================
// RollingWindow.hpp -- ring buffers and rolling stat primitives for the
// extractor's hot path. Header-only, no allocations after construction.
// =============================================================================
//
// All windows are sized as power-of-two for fast modulo via bitmask.
// All operations are O(1) per push.
//
// Provided types:
//   Ring<T,N>          -- fixed-cap circular buffer with random access
//   MinMaxRing<T,N>    -- O(1) amortised running min/max
//   RollingMean<N>     -- running mean, O(1)
//   RollingVar<N>      -- running variance via Welford-on-window, O(1)
//   RollingLinReg<N>   -- least-squares slope on (x=0..N-1, y=samples), O(1)
//
// Naming:
//   * size() returns current count, capacity() returns N.
//   * `back()` is most recently pushed sample, `front()` is oldest.
//   * `operator[](i)` is i-th oldest (i=0 is front).
//
// All numerics in double. Templates avoid runtime allocation by using std::array.
// =============================================================================

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace edgefinder {

template <typename T, std::size_t N>
class Ring {
    static_assert(N > 0 && (N & (N-1)) == 0, "N must be a power of two");
    std::array<T, N> buf_{};
    std::size_t      head_  = 0;     // index of oldest valid element
    std::size_t      count_ = 0;
    static constexpr std::size_t MASK = N - 1;
public:
    void push_back(const T& v) noexcept {
        buf_[(head_ + count_) & MASK] = v;
        if (count_ < N) ++count_;
        else            head_ = (head_ + 1) & MASK;
    }
    std::size_t size()     const noexcept { return count_; }
    std::size_t capacity() const noexcept { return N; }
    bool        empty()    const noexcept { return count_ == 0; }
    bool        full()     const noexcept { return count_ == N; }
    const T&    back()     const noexcept { return buf_[(head_ + count_ - 1) & MASK]; }
    const T&    front()    const noexcept { return buf_[head_]; }
    const T&    operator[](std::size_t i) const noexcept { return buf_[(head_ + i) & MASK]; }
    void        clear() noexcept { head_ = 0; count_ = 0; }
};

// Monotone-deque running min/max. Each push is O(1) amortised.
template <typename T, std::size_t N>
class MinMaxRing {
    static_assert(N > 0 && (N & (N-1)) == 0, "N must be a power of two");
    std::array<T, N>            buf_{};
    std::array<std::size_t,2*N> min_idx_{}, max_idx_{};
    std::size_t abs_pos_ = 0, count_ = 0;
    std::size_t min_h_ = 0, min_t_ = 0, max_h_ = 0, max_t_ = 0;
    static constexpr std::size_t MASK  = N - 1;
    static constexpr std::size_t IMASK = 2*N - 1;
public:
    void push_back(const T& v) noexcept {
        const std::size_t slot = abs_pos_ & MASK;
        buf_[slot] = v;
        if (count_ == N) {
            const std::size_t evict_slot = (abs_pos_ - N) & MASK;
            if (min_h_ != min_t_ && min_idx_[min_h_ & IMASK] == evict_slot) ++min_h_;
            if (max_h_ != max_t_ && max_idx_[max_h_ & IMASK] == evict_slot) ++max_h_;
        }
        while (min_h_ != min_t_ && buf_[min_idx_[(min_t_-1) & IMASK]] >= v) --min_t_;
        min_idx_[min_t_++ & IMASK] = slot;
        while (max_h_ != max_t_ && buf_[max_idx_[(max_t_-1) & IMASK]] <= v) --max_t_;
        max_idx_[max_t_++ & IMASK] = slot;
        ++abs_pos_;
        if (count_ < N) ++count_;
    }
    std::size_t size()     const noexcept { return count_; }
    std::size_t capacity() const noexcept { return N; }
    bool        empty()    const noexcept { return count_ == 0; }
    bool        full()     const noexcept { return count_ == N; }
    T           min()      const noexcept { return buf_[min_idx_[min_h_ & IMASK]]; }
    T           max()      const noexcept { return buf_[max_idx_[max_h_ & IMASK]]; }
    void        clear() noexcept {
        count_ = 0; abs_pos_ = 0;
        min_h_ = min_t_ = max_h_ = max_t_ = 0;
    }
};

// Running mean over the last N samples in O(1). NaN until first push.
template <std::size_t N>
class RollingMean {
    static_assert(N > 0 && (N & (N-1)) == 0, "N must be a power of two");
    std::array<double, N> buf_{};
    std::size_t           pos_ = 0, count_ = 0;
    double                sum_ = 0.0;
    static constexpr std::size_t MASK = N - 1;
public:
    void push(double v) noexcept {
        if (count_ < N) {
            buf_[pos_ & MASK] = v;
            sum_ += v;
            ++count_;
        } else {
            const double evict = buf_[pos_ & MASK];
            buf_[pos_ & MASK] = v;
            sum_ += (v - evict);
        }
        ++pos_;
    }
    std::size_t size()     const noexcept { return count_; }
    std::size_t capacity() const noexcept { return N; }
    bool        full()     const noexcept { return count_ == N; }
    double      mean()     const noexcept {
        return (count_ == 0) ? std::nan("") : sum_ / static_cast<double>(count_);
    }
};

// Running mean + variance over last N samples in O(1) (sum and sum-of-squares
// maintenance; numerically OK for moderate N; NOT Welford). Adequate for ATR/vol.
template <std::size_t N>
class RollingVar {
    static_assert(N > 0 && (N & (N-1)) == 0, "N must be a power of two");
    std::array<double, N> buf_{};
    std::size_t           pos_ = 0, count_ = 0;
    double                sum_ = 0.0, sumsq_ = 0.0;
    static constexpr std::size_t MASK = N - 1;
public:
    void push(double v) noexcept {
        if (count_ < N) {
            buf_[pos_ & MASK] = v;
            sum_   += v;
            sumsq_ += v * v;
            ++count_;
        } else {
            const double evict = buf_[pos_ & MASK];
            buf_[pos_ & MASK] = v;
            sum_   += (v - evict);
            sumsq_ += (v*v - evict*evict);
        }
        ++pos_;
    }
    std::size_t size() const noexcept { return count_; }
    bool        full() const noexcept { return count_ == N; }
    double      mean() const noexcept {
        return (count_ == 0) ? std::nan("") : sum_ / static_cast<double>(count_);
    }
    // Sample variance, n-1 denominator. NaN if count<2.
    double      var() const noexcept {
        if (count_ < 2) return std::nan("");
        const double n  = static_cast<double>(count_);
        const double mu = sum_ / n;
        const double s  = sumsq_ - n * mu * mu;
        return (s > 0.0) ? (s / (n - 1.0)) : 0.0;
    }
    double      stddev() const noexcept {
        const double v = var();
        return std::isnan(v) ? v : std::sqrt(v);
    }
};

// Slope of least-squares fit y = a + b*x, x = 0..N-1 (oldest..newest).
// Maintains running sum of y_i and sum of i*y_i; sum(x) and sum(x^2) are constants.
// The exact closed form for slope is:
//   b = (N * Σ(x*y) - Σx * Σy) / (N * Σ(x^2) - (Σx)^2)
// We use a simpler equivalent: with x = 0..N-1, mean_x = (N-1)/2, var_x = (N^2-1)/12.
// This gives O(1) per sample after the first N.
template <std::size_t N>
class RollingLinReg {
    static_assert(N > 0 && (N & (N-1)) == 0, "N must be a power of two");
    std::array<double, N> buf_{};
    std::size_t           pos_ = 0, count_ = 0;
    static constexpr std::size_t MASK = N - 1;
public:
    void push(double v) noexcept {
        buf_[pos_ & MASK] = v;
        ++pos_;
        if (count_ < N) ++count_;
    }
    std::size_t size() const noexcept { return count_; }
    bool        full() const noexcept { return count_ == N; }
    // Slope per step (in y units / 1 bar).
    double      slope() const noexcept {
        if (count_ < 2) return std::nan("");
        const std::size_t n = count_;
        // index 0 is oldest, index n-1 newest
        double sum_y = 0.0, sum_xy = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double y = buf_[(pos_ - n + i) & MASK];
            sum_y  += y;
            sum_xy += static_cast<double>(i) * y;
        }
        const double dn  = static_cast<double>(n);
        const double sx  = (dn - 1.0) * dn / 2.0;
        const double sxx = (dn - 1.0) * dn * (2.0 * dn - 1.0) / 6.0;
        const double denom = dn * sxx - sx * sx;
        if (denom <= 0.0) return std::nan("");
        return (dn * sum_xy - sx * sum_y) / denom;
    }
};

} // namespace edgefinder

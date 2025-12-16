#pragma once
#include <array>
#include <cstdint>
#include <cmath>
#include "MicroEngineBase.hpp"

namespace Omega {

// =============================================================================
// MicroEngineReversion - HFT Optimized with CRTP
// YOUR exact logic preserved, mutex removed
// =============================================================================
class MicroEngineReversion : public MicroEngineBase<MicroEngineReversion> {
    friend class MicroEngineBase<MicroEngineReversion>;
    static constexpr size_t MAX_WINDOW = 128;

public:
    MicroEngineReversion() : window_(30), bandWidth_(2.0), count_(0), head_(0), sum_(0.0) {
        mids_.fill(0.0);
        tss_.fill(0);
    }
    ~MicroEngineReversion() = default;

    inline void setWindow(size_t n) { window_ = (n > MAX_WINDOW) ? MAX_WINDOW : n; }
    inline void setBand(double b) { bandWidth_ = b; }

private:
    inline void onTickImpl(const UnifiedTick& t) {
        double mid = (t.bid + t.ask) * 0.5;
        
        // Remove old value from running sum if buffer full
        if (count_ == window_) {
            size_t oldest = (head_ + MAX_WINDOW - window_) % MAX_WINDOW;
            sum_ -= mids_[oldest];
        }
        
        mids_[head_] = mid;
        tss_[head_] = t.tsLocal;
        sum_ += mid;
        head_ = (head_ + 1) % MAX_WINDOW;
        
        if (count_ < window_) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        if (count_ < 5) return s;
        
        // Mean from running sum
        double mean = sum_ / count_;
        
        // Calculate stddev (iterate over valid data)
        double var = 0.0;
        for (size_t i = 0; i < count_; ++i) {
            size_t idx = (head_ + MAX_WINDOW - count_ + i) % MAX_WINDOW;
            double d = mids_[idx] - mean;
            var += d * d;
        }
        var /= count_;
        double sigma = std::sqrt(var);
        
        // Get last value
        size_t lastIdx = (head_ + MAX_WINDOW - 1) % MAX_WINDOW;
        double last = mids_[lastIdx];
        double zscore = (last - mean) / (sigma + 1e-9);
        
        // YOUR exact reversion logic
        if (zscore > bandWidth_) {
            s.value = -zscore;
            s.confidence = std::min(1.0, (zscore - bandWidth_) / bandWidth_);
        } else if (zscore < -bandWidth_) {
            s.value = -zscore;
            s.confidence = std::min(1.0, (-zscore - bandWidth_) / bandWidth_);
        } else {
            s.value = -zscore * 0.5;
            s.confidence = std::fabs(zscore) / bandWidth_ * 0.3;
        }
        
        s.ts = tss_[lastIdx];
        return s;
    }
    
    inline void resetImpl() {
        count_ = 0;
        head_ = 0;
        sum_ = 0.0;
        mids_.fill(0.0);
        tss_.fill(0);
    }

private:
    size_t window_;
    double bandWidth_;
    size_t count_;
    size_t head_;
    double sum_;  // Running sum for O(1) mean
    std::array<double, MAX_WINDOW> mids_;
    std::array<uint64_t, MAX_WINDOW> tss_;
};

} // namespace Omega

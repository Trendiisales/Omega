#pragma once
#include <array>
#include <cstdint>
#include <cmath>
#include "MicroEngineBase.hpp"

namespace Omega {

// =============================================================================
// MicroEngineVolumeShock - HFT Optimized with CRTP
// YOUR exact logic preserved
// =============================================================================
class MicroEngineVolumeShock : public MicroEngineBase<MicroEngineVolumeShock> {
    friend class MicroEngineBase<MicroEngineVolumeShock>;
    static constexpr size_t MAX_WINDOW = 128;

public:
    MicroEngineVolumeShock() : window_(20), threshold_(2.0), count_(0), head_(0), sum_(0.0) {
        vol_.fill(0.0);
        deltas_.fill(0.0);
        tss_.fill(0);
    }
    ~MicroEngineVolumeShock() = default;

    inline void setWindow(size_t n) { window_ = (n > MAX_WINDOW) ? MAX_WINDOW : n; }
    inline void setThreshold(double t) { threshold_ = t; }

private:
    inline void onTickImpl(const UnifiedTick& t) {
        double volume = t.buyVol + t.sellVol;
        double delta = t.buyVol - t.sellVol;
        
        // Remove old from sum if full
        if (count_ == window_) {
            size_t oldest = (head_ + MAX_WINDOW - window_) % MAX_WINDOW;
            sum_ -= vol_[oldest];
        }
        
        vol_[head_] = volume;
        deltas_[head_] = delta;
        tss_[head_] = t.tsLocal;
        sum_ += volume;
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < window_) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        if (count_ < 5) return s;
        
        double mean = sum_ / count_;
        
        // Calculate stddev
        double var = 0.0;
        for (size_t i = 0; i < count_; ++i) {
            size_t idx = (head_ + MAX_WINDOW - count_ + i) % MAX_WINDOW;
            double d = vol_[idx] - mean;
            var += d * d;
        }
        var /= count_;
        double sigma = std::sqrt(var);
        
        size_t lastIdx = (head_ + MAX_WINDOW - 1) % MAX_WINDOW;
        double lastVol = vol_[lastIdx];
        double lastDelta = deltas_[lastIdx];
        
        // YOUR exact shock logic
        double shock = (lastVol - mean) / (sigma + 1e-9);
        double direction = lastDelta / (lastVol + 1e-9);
        
        s.value = shock * direction;
        s.confidence = std::min(1.0, std::fabs(shock) / threshold_) * 0.5;
        s.ts = tss_[lastIdx];
        return s;
    }
    
    inline void resetImpl() {
        count_ = 0;
        head_ = 0;
        sum_ = 0.0;
        vol_.fill(0.0);
        deltas_.fill(0.0);
        tss_.fill(0);
    }

private:
    size_t window_;
    double threshold_;
    size_t count_;
    size_t head_;
    double sum_;
    std::array<double, MAX_WINDOW> vol_;
    std::array<double, MAX_WINDOW> deltas_;
    std::array<uint64_t, MAX_WINDOW> tss_;
};

} // namespace Omega

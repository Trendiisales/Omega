#pragma once
#include <array>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include "MicroEngineBase.hpp"

namespace Omega {

// =============================================================================
// MicroEngineBreakout - HFT Optimized with CRTP
// YOUR exact logic preserved
// =============================================================================
class MicroEngineBreakout : public MicroEngineBase<MicroEngineBreakout> {
    friend class MicroEngineBase<MicroEngineBreakout>;
    static constexpr size_t MAX_WINDOW = 128;

public:
    MicroEngineBreakout() : window_(25), count_(0), head_(0) {
        highs_.fill(0.0);
        lows_.fill(0.0);
        tss_.fill(0);
    }
    ~MicroEngineBreakout() = default;

    inline void setWindow(size_t n) { window_ = (n > MAX_WINDOW) ? MAX_WINDOW : n; }

private:
    inline void onTickImpl(const UnifiedTick& t) {
        double mid = (t.bid + t.ask) / 2.0;
        double halfSpread = t.spread / 2.0;
        
        highs_[head_] = mid + halfSpread;
        lows_[head_] = mid - halfSpread;
        tss_[head_] = t.tsLocal;
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < window_) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        if (count_ < 3) return s;
        
        size_t lastIdx = (head_ + MAX_WINDOW - 1) % MAX_WINDOW;
        double recentHigh = highs_[lastIdx];
        double recentLow = lows_[lastIdx];
        
        // Find max/min excluding last value
        double maxPrev = recentHigh;
        double minPrev = recentLow;
        
        for (size_t i = 0; i < count_ - 1; ++i) {
            size_t idx = (head_ + MAX_WINDOW - count_ + i) % MAX_WINDOW;
            maxPrev = std::max(maxPrev, highs_[idx]);
            minPrev = std::min(minPrev, lows_[idx]);
        }
        
        double breakout = 0.0;
        
        // YOUR exact breakout logic
        if (recentHigh > maxPrev) {
            breakout = +(recentHigh - maxPrev);
        } else if (recentLow < minPrev) {
            breakout = -(minPrev - recentLow);
        }
        
        s.value = breakout;
        s.confidence = std::fabs(breakout) * 0.12;
        s.ts = tss_[lastIdx];
        return s;
    }
    
    inline void resetImpl() {
        count_ = 0;
        head_ = 0;
        highs_.fill(0.0);
        lows_.fill(0.0);
        tss_.fill(0);
    }

private:
    size_t window_;
    size_t count_;
    size_t head_;
    std::array<double, MAX_WINDOW> highs_;
    std::array<double, MAX_WINDOW> lows_;
    std::array<uint64_t, MAX_WINDOW> tss_;
};

} // namespace Omega

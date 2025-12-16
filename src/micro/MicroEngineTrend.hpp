#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <atomic>
#include "MicroEngineBase.hpp"

namespace Omega {

// =============================================================================
// MicroEngineTrend - HFT Optimized with CRTP
// NO mutex - single-threaded hot path
// Fixed array - NO heap allocation
// =============================================================================
class MicroEngineTrend : public MicroEngineBase<MicroEngineTrend> {
    friend class MicroEngineBase<MicroEngineTrend>;
    static constexpr size_t MAX_WINDOW = 128;

public:
    MicroEngineTrend() : window_(20), count_(0), head_(0) {
        mids_.fill(0.0);
        tss_.fill(0);
    }
    ~MicroEngineTrend() = default;

    inline void setWindow(size_t n) {
        window_ = (n > MAX_WINDOW) ? MAX_WINDOW : n;
    }

private:
    // CRTP Implementation - called by base class
    inline void onTickImpl(const UnifiedTick& t) {
        double mid = (t.bid + t.ask) * 0.5;
        
        mids_[head_] = mid;
        tss_[head_] = t.tsLocal;
        head_ = (head_ + 1) % MAX_WINDOW;
        
        if (count_ < window_) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        if (count_ < 3) return s;
        
        // Get last 3 values using circular buffer math
        size_t idx3 = (head_ + MAX_WINDOW - 1) % MAX_WINDOW;
        size_t idx2 = (head_ + MAX_WINDOW - 2) % MAX_WINDOW;
        size_t idx1 = (head_ + MAX_WINDOW - 3) % MAX_WINDOW;
        
        double p1 = mids_[idx1];
        double p2 = mids_[idx2];
        double p3 = mids_[idx3];
        
        double slope = (p3 - p1);
        double accel = (p3 - p2) - (p2 - p1);
        
        s.value = slope;
        s.confidence = std::fabs(accel) * 0.1;
        s.ts = tss_[idx3];
        
        return s;
    }
    
    inline void resetImpl() {
        count_ = 0;
        head_ = 0;
        mids_.fill(0.0);
        tss_.fill(0);
    }

private:
    size_t window_;
    size_t count_;
    size_t head_;
    std::array<double, MAX_WINDOW> mids_;
    std::array<uint64_t, MAX_WINDOW> tss_;
};

} // namespace Omega

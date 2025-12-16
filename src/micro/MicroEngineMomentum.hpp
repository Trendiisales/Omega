#pragma once
#include <array>
#include <cstdint>
#include <cmath>
#include "MicroEngineBase.hpp"

namespace Omega {

// =============================================================================
// MicroEngineMomentum - HFT Optimized with CRTP
// YOUR exact logic preserved
// =============================================================================
class MicroEngineMomentum : public MicroEngineBase<MicroEngineMomentum> {
    friend class MicroEngineBase<MicroEngineMomentum>;
    static constexpr size_t MAX_WINDOW = 128;

public:
    MicroEngineMomentum() : window_(15), count_(0), head_(0) {
        mids_.fill(0.0);
        tss_.fill(0);
    }
    ~MicroEngineMomentum() = default;

    inline void setWindow(size_t n) { window_ = (n > MAX_WINDOW) ? MAX_WINDOW : n; }

private:
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
        
        size_t idx3 = (head_ + MAX_WINDOW - 1) % MAX_WINDOW;
        size_t idx2 = (head_ + MAX_WINDOW - 2) % MAX_WINDOW;
        size_t idx1 = (head_ + MAX_WINDOW - 3) % MAX_WINDOW;
        
        double p1 = mids_[idx1];
        double p2 = mids_[idx2];
        double p3 = mids_[idx3];
        
        double v1 = (p2 - p1);
        double v2 = (p3 - p2);
        
        // YOUR exact momentum logic
        double mom = v2 * 2.0 - v1;
        
        s.value = mom;
        s.confidence = std::fabs(mom) * 0.08;
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

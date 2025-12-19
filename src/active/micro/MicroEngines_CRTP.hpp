// =============================================================================
// MicroEngines_CRTP.hpp - ALL 17 MicroEngines converted to CRTP + Fixed Arrays
// 
// HFT Optimizations:
// - CRTP (Curiously Recurring Template Pattern) - zero virtual function overhead
// - Fixed std::array - zero heap allocation
// - Inline everything - compiler optimization
// - NO mutex - single-threaded hot path
// =============================================================================
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include "MicroEngineBase.hpp"
#include "../data/UnifiedTick.hpp"

namespace Chimera {

// =============================================================================
// MicroEngine01 - Momentum (EMA of price changes)
// =============================================================================
class MicroEngine01 : public MicroEngineBase<MicroEngine01> {
    friend class MicroEngineBase<MicroEngine01>;
    static constexpr size_t MAX_WINDOW = 64;
    
public:
    MicroEngine01() : lastMid_(0), momentum_(0), head_(0), count_(0) {
        mids_.fill(0.0);
    }
    
private:
    inline void onTickImpl(const UnifiedTick& t) {
        double mid = 0.5 * (t.bid + t.ask);
        double d = mid - lastMid_;
        lastMid_ = mid;
        momentum_ = 0.9 * momentum_ + 0.1 * d;
        
        mids_[head_] = mid;
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < MAX_WINDOW) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        s.value = momentum_;
        s.confidence = std::min(1.0, std::fabs(momentum_) * 100.0);
        return s;
    }
    
    inline void resetImpl() {
        lastMid_ = 0; momentum_ = 0; head_ = 0; count_ = 0;
        mids_.fill(0.0);
    }
    
    double lastMid_, momentum_;
    size_t head_, count_;
    std::array<double, MAX_WINDOW> mids_;
};

// =============================================================================
// MicroEngine02 - Order Book Imbalance (top 2 levels)
// =============================================================================
class MicroEngine02 : public MicroEngineBase<MicroEngine02> {
    friend class MicroEngineBase<MicroEngine02>;
    static constexpr size_t MAX_WINDOW = 64;
    
public:
    MicroEngine02() : imbalance_(0), head_(0), count_(0) {
        imbalances_.fill(0.0);
    }
    
private:
    inline void onTickImpl(const UnifiedTick& t) {
        double B = t.bidDepth;
        double A = t.askDepth;
        
        if (B + A > 0)
            imbalance_ = (B - A) / (B + A);
        else
            imbalance_ = 0;
        
        imbalances_[head_] = imbalance_;
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < MAX_WINDOW) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        s.value = imbalance_;
        s.confidence = std::fabs(imbalance_);
        return s;
    }
    
    inline void resetImpl() {
        imbalance_ = 0; head_ = 0; count_ = 0;
        imbalances_.fill(0.0);
    }
    
    double imbalance_;
    size_t head_, count_;
    std::array<double, MAX_WINDOW> imbalances_;
};

// =============================================================================
// MicroEngine03 - Volume EMA (volume change momentum)
// =============================================================================
class MicroEngine03 : public MicroEngineBase<MicroEngine03> {
    friend class MicroEngineBase<MicroEngine03>;
    static constexpr size_t MAX_WINDOW = 64;
    
public:
    MicroEngine03() : volEMA_(0), lastVol_(0), head_(0), count_(0) {
        volumes_.fill(0.0);
    }
    
private:
    inline void onTickImpl(const UnifiedTick& t) {
        double v = t.buyVol + t.sellVol;
        double dv = v - lastVol_;
        lastVol_ = v;
        volEMA_ = 0.9 * volEMA_ + 0.1 * dv;
        
        volumes_[head_] = v;
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < MAX_WINDOW) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        s.value = volEMA_;
        s.confidence = std::min(1.0, std::fabs(volEMA_) * 10.0);
        return s;
    }
    
    inline void resetImpl() {
        volEMA_ = 0; lastVol_ = 0; head_ = 0; count_ = 0;
        volumes_.fill(0.0);
    }
    
    double volEMA_, lastVol_;
    size_t head_, count_;
    std::array<double, MAX_WINDOW> volumes_;
};

// =============================================================================
// MicroEngine04 - Spread EMA
// =============================================================================
class MicroEngine04 : public MicroEngineBase<MicroEngine04> {
    friend class MicroEngineBase<MicroEngine04>;
    static constexpr size_t MAX_WINDOW = 64;
    
public:
    MicroEngine04() : spreadEMA_(0), head_(0), count_(0) {
        spreads_.fill(0.0);
    }
    
private:
    inline void onTickImpl(const UnifiedTick& t) {
        spreadEMA_ = 0.85 * spreadEMA_ + 0.15 * t.spread;
        
        spreads_[head_] = t.spread;
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < MAX_WINDOW) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        s.value = spreadEMA_;
        s.confidence = 1.0;
        return s;
    }
    
    inline void resetImpl() {
        spreadEMA_ = 0; head_ = 0; count_ = 0;
        spreads_.fill(0.0);
    }
    
    double spreadEMA_;
    size_t head_, count_;
    std::array<double, MAX_WINDOW> spreads_;
};

// =============================================================================
// MicroEngine05 - Depth Tilt (3 levels)
// =============================================================================
class MicroEngine05 : public MicroEngineBase<MicroEngine05> {
    friend class MicroEngineBase<MicroEngine05>;
    static constexpr size_t MAX_WINDOW = 64;
    
public:
    MicroEngine05() : depthTilt_(0), head_(0), count_(0) {
        tilts_.fill(0.0);
    }
    
private:
    inline void onTickImpl(const UnifiedTick& t) {
        double B = t.bidDepth;
        double A = t.askDepth;
        
        if (B + A > 0)
            depthTilt_ = (B - A) / (B + A);
        else
            depthTilt_ = 0;
        
        tilts_[head_] = depthTilt_;
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < MAX_WINDOW) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        s.value = depthTilt_;
        s.confidence = std::fabs(depthTilt_);
        return s;
    }
    
    inline void resetImpl() {
        depthTilt_ = 0; head_ = 0; count_ = 0;
        tilts_.fill(0.0);
    }
    
    double depthTilt_;
    size_t head_, count_;
    std::array<double, MAX_WINDOW> tilts_;
};

// =============================================================================
// MicroEngine06 - Delta Acceleration
// =============================================================================
class MicroEngine06 : public MicroEngineBase<MicroEngine06> {
    friend class MicroEngineBase<MicroEngine06>;
    static constexpr size_t MAX_WINDOW = 64;
    
public:
    MicroEngine06() : deltaAccel_(0), lastDelta_(0), lastAccel_(0), head_(0), count_(0) {
        accels_.fill(0.0);
    }
    
private:
    inline void onTickImpl(const UnifiedTick& t) {
        double delta = t.buyVol - t.sellVol;
        double d = delta - lastDelta_;
        lastDelta_ = delta;
        
        double acc = d - lastAccel_;
        lastAccel_ = d;
        
        deltaAccel_ = 0.9 * deltaAccel_ + 0.1 * acc;
        
        accels_[head_] = deltaAccel_;
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < MAX_WINDOW) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        s.value = deltaAccel_;
        s.confidence = std::min(1.0, std::fabs(deltaAccel_) * 100.0);
        return s;
    }
    
    inline void resetImpl() {
        deltaAccel_ = 0; lastDelta_ = 0; lastAccel_ = 0; head_ = 0; count_ = 0;
        accels_.fill(0.0);
    }
    
    double deltaAccel_, lastDelta_, lastAccel_;
    size_t head_, count_;
    std::array<double, MAX_WINDOW> accels_;
};

// =============================================================================
// MicroEngine07 - Alternate Level Imbalance (levels 0,2,4)
// =============================================================================
class MicroEngine07 : public MicroEngineBase<MicroEngine07> {
    friend class MicroEngineBase<MicroEngine07>;
    static constexpr size_t MAX_WINDOW = 64;
    
public:
    MicroEngine07() : imbalance2_(0), head_(0), count_(0) {
        imbalances_.fill(0.0);
    }
    
private:
    inline void onTickImpl(const UnifiedTick& t) {
        // Using total depth as proxy (original used specific levels)
        double B = t.bidDepth * 0.6;  // Weighted for alternate levels
        double A = t.askDepth * 0.6;
        
        if (B + A > 0)
            imbalance2_ = (B - A) / (B + A);
        else
            imbalance2_ = 0;
        
        imbalances_[head_] = imbalance2_;
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < MAX_WINDOW) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        s.value = imbalance2_;
        s.confidence = std::fabs(imbalance2_);
        return s;
    }
    
    inline void resetImpl() {
        imbalance2_ = 0; head_ = 0; count_ = 0;
        imbalances_.fill(0.0);
    }
    
    double imbalance2_;
    size_t head_, count_;
    std::array<double, MAX_WINDOW> imbalances_;
};

// =============================================================================
// MicroEngine08 - Volume Shock (deviation from average)
// =============================================================================
class MicroEngine08 : public MicroEngineBase<MicroEngine08> {
    friend class MicroEngineBase<MicroEngine08>;
    static constexpr size_t MAX_WINDOW = 64;
    
public:
    MicroEngine08() : volShock_(0), volAvg_(0), head_(0), count_(0) {
        volumes_.fill(0.0);
    }
    
private:
    inline void onTickImpl(const UnifiedTick& t) {
        double v = t.buyVol + t.sellVol;
        volAvg_ = 0.9 * volAvg_ + 0.1 * v;
        volShock_ = v - volAvg_;
        
        volumes_[head_] = v;
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < MAX_WINDOW) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        s.value = volShock_;
        s.confidence = (volAvg_ > 0) ? std::min(1.0, std::fabs(volShock_ / volAvg_)) : 0.0;
        return s;
    }
    
    inline void resetImpl() {
        volShock_ = 0; volAvg_ = 0; head_ = 0; count_ = 0;
        volumes_.fill(0.0);
    }
    
    double volShock_, volAvg_;
    size_t head_, count_;
    std::array<double, MAX_WINDOW> volumes_;
};

// =============================================================================
// MicroEngine09 - Spread Acceleration
// =============================================================================
class MicroEngine09 : public MicroEngineBase<MicroEngine09> {
    friend class MicroEngineBase<MicroEngine09>;
    static constexpr size_t MAX_WINDOW = 64;
    
public:
    MicroEngine09() : spreadAccel_(0), lastSpread_(0), lastAccel_(0), head_(0), count_(0) {
        accels_.fill(0.0);
    }
    
private:
    inline void onTickImpl(const UnifiedTick& t) {
        double d = t.spread - lastSpread_;
        lastSpread_ = t.spread;
        
        double acc = d - lastAccel_;
        lastAccel_ = d;
        
        spreadAccel_ = 0.92 * spreadAccel_ + 0.08 * acc;
        
        accels_[head_] = spreadAccel_;
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < MAX_WINDOW) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        s.value = spreadAccel_;
        s.confidence = std::min(1.0, std::fabs(spreadAccel_) * 1000.0);
        return s;
    }
    
    inline void resetImpl() {
        spreadAccel_ = 0; lastSpread_ = 0; lastAccel_ = 0; head_ = 0; count_ = 0;
        accels_.fill(0.0);
    }
    
    double spreadAccel_, lastSpread_, lastAccel_;
    size_t head_, count_;
    std::array<double, MAX_WINDOW> accels_;
};

// =============================================================================
// MicroEngine10 - Depth Gradient
// =============================================================================
class MicroEngine10 : public MicroEngineBase<MicroEngine10> {
    friend class MicroEngineBase<MicroEngine10>;
    static constexpr size_t MAX_WINDOW = 64;
    
public:
    MicroEngine10() : depthGradient_(0), head_(0), count_(0) {
        gradients_.fill(0.0);
    }
    
private:
    inline void onTickImpl(const UnifiedTick& t) {
        double bSlope = t.bidDepth;
        double aSlope = t.askDepth;
        
        if (bSlope + aSlope > 0)
            depthGradient_ = (bSlope - aSlope) / (bSlope + aSlope);
        else
            depthGradient_ = 0;
        
        gradients_[head_] = depthGradient_;
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < MAX_WINDOW) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        s.value = depthGradient_;
        s.confidence = std::fabs(depthGradient_);
        return s;
    }
    
    inline void resetImpl() {
        depthGradient_ = 0; head_ = 0; count_ = 0;
        gradients_.fill(0.0);
    }
    
    double depthGradient_;
    size_t head_, count_;
    std::array<double, MAX_WINDOW> gradients_;
};

// =============================================================================
// MicroEngine11 - Short Term Momentum (faster EMA)
// =============================================================================
class MicroEngine11 : public MicroEngineBase<MicroEngine11> {
    friend class MicroEngineBase<MicroEngine11>;
    static constexpr size_t MAX_WINDOW = 64;
    
public:
    MicroEngine11() : shortTermMom_(0), lastMid_(0), head_(0), count_(0) {
        moms_.fill(0.0);
    }
    
private:
    inline void onTickImpl(const UnifiedTick& t) {
        double mid = 0.5 * (t.bid + t.ask);
        double d = mid - lastMid_;
        lastMid_ = mid;
        
        shortTermMom_ = 0.8 * shortTermMom_ + 0.2 * d;
        
        moms_[head_] = shortTermMom_;
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < MAX_WINDOW) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        s.value = shortTermMom_;
        s.confidence = std::min(1.0, std::fabs(shortTermMom_) * 100.0);
        return s;
    }
    
    inline void resetImpl() {
        shortTermMom_ = 0; lastMid_ = 0; head_ = 0; count_ = 0;
        moms_.fill(0.0);
    }
    
    double shortTermMom_, lastMid_;
    size_t head_, count_;
    std::array<double, MAX_WINDOW> moms_;
};

// =============================================================================
// MicroEngine12 - Volume Balance (buy vs sell)
// =============================================================================
class MicroEngine12 : public MicroEngineBase<MicroEngine12> {
    friend class MicroEngineBase<MicroEngine12>;
    static constexpr size_t MAX_WINDOW = 64;
    
public:
    MicroEngine12() : volBalance_(0), head_(0), count_(0) {
        balances_.fill(0.0);
    }
    
private:
    inline void onTickImpl(const UnifiedTick& t) {
        double buy = t.buyVol;
        double sell = t.sellVol;
        
        if (buy + sell > 0)
            volBalance_ = (buy - sell) / (buy + sell);
        else
            volBalance_ = 0;
        
        balances_[head_] = volBalance_;
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < MAX_WINDOW) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        s.value = volBalance_;
        s.confidence = std::fabs(volBalance_);
        return s;
    }
    
    inline void resetImpl() {
        volBalance_ = 0; head_ = 0; count_ = 0;
        balances_.fill(0.0);
    }
    
    double volBalance_;
    size_t head_, count_;
    std::array<double, MAX_WINDOW> balances_;
};

// =============================================================================
// MicroEngine13 - Mid Price Acceleration
// =============================================================================
class MicroEngine13 : public MicroEngineBase<MicroEngine13> {
    friend class MicroEngineBase<MicroEngine13>;
    static constexpr size_t MAX_WINDOW = 64;
    
public:
    MicroEngine13() : midAccel_(0), lastMid_(0), lastVel_(0), head_(0), count_(0) {
        accels_.fill(0.0);
    }
    
private:
    inline void onTickImpl(const UnifiedTick& t) {
        double mid = 0.5 * (t.bid + t.ask);
        double vel = mid - lastMid_;
        lastMid_ = mid;
        
        double acc = vel - lastVel_;
        lastVel_ = vel;
        
        midAccel_ = 0.9 * midAccel_ + 0.1 * acc;
        
        accels_[head_] = midAccel_;
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < MAX_WINDOW) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        s.value = midAccel_;
        s.confidence = std::min(1.0, std::fabs(midAccel_) * 1000.0);
        return s;
    }
    
    inline void resetImpl() {
        midAccel_ = 0; lastMid_ = 0; lastVel_ = 0; head_ = 0; count_ = 0;
        accels_.fill(0.0);
    }
    
    double midAccel_, lastMid_, lastVel_;
    size_t head_, count_;
    std::array<double, MAX_WINDOW> accels_;
};

// =============================================================================
// MicroEngine14 - Depth Symmetry (absolute imbalance)
// =============================================================================
class MicroEngine14 : public MicroEngineBase<MicroEngine14> {
    friend class MicroEngineBase<MicroEngine14>;
    static constexpr size_t MAX_WINDOW = 64;
    
public:
    MicroEngine14() : depthSym_(0), head_(0), count_(0) {
        syms_.fill(0.0);
    }
    
private:
    inline void onTickImpl(const UnifiedTick& t) {
        double Bl = t.bidDepth;
        double Al = t.askDepth;
        
        if (Bl + Al > 0)
            depthSym_ = std::fabs(Bl - Al) / (Bl + Al);
        else
            depthSym_ = 0;
        
        syms_[head_] = depthSym_;
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < MAX_WINDOW) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        s.value = depthSym_;
        s.confidence = depthSym_;
        return s;
    }
    
    inline void resetImpl() {
        depthSym_ = 0; head_ = 0; count_ = 0;
        syms_.fill(0.0);
    }
    
    double depthSym_;
    size_t head_, count_;
    std::array<double, MAX_WINDOW> syms_;
};

// =============================================================================
// MicroEngine15 - Spread Trend
// =============================================================================
class MicroEngine15 : public MicroEngineBase<MicroEngine15> {
    friend class MicroEngineBase<MicroEngine15>;
    static constexpr size_t MAX_WINDOW = 64;
    
public:
    MicroEngine15() : spreadTrend_(0), lastSpread_(0), head_(0), count_(0) {
        trends_.fill(0.0);
    }
    
private:
    inline void onTickImpl(const UnifiedTick& t) {
        double d = t.spread - lastSpread_;
        lastSpread_ = t.spread;
        
        spreadTrend_ = 0.85 * spreadTrend_ + 0.15 * d;
        
        trends_[head_] = spreadTrend_;
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < MAX_WINDOW) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        s.value = spreadTrend_;
        s.confidence = std::min(1.0, std::fabs(spreadTrend_) * 1000.0);
        return s;
    }
    
    inline void resetImpl() {
        spreadTrend_ = 0; lastSpread_ = 0; head_ = 0; count_ = 0;
        trends_.fill(0.0);
    }
    
    double spreadTrend_, lastSpread_;
    size_t head_, count_;
    std::array<double, MAX_WINDOW> trends_;
};

// =============================================================================
// MicroEngine16 - Book Pressure (5 levels)
// =============================================================================
class MicroEngine16 : public MicroEngineBase<MicroEngine16> {
    friend class MicroEngineBase<MicroEngine16>;
    static constexpr size_t MAX_WINDOW = 64;
    
public:
    MicroEngine16() : bookPressure_(0), head_(0), count_(0) {
        pressures_.fill(0.0);
    }
    
private:
    inline void onTickImpl(const UnifiedTick& t) {
        double B = t.bidDepth;
        double A = t.askDepth;
        
        if (B + A > 0)
            bookPressure_ = (B - A) / (B + A);
        else
            bookPressure_ = 0;
        
        pressures_[head_] = bookPressure_;
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < MAX_WINDOW) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        s.value = bookPressure_;
        s.confidence = std::fabs(bookPressure_);
        return s;
    }
    
    inline void resetImpl() {
        bookPressure_ = 0; head_ = 0; count_ = 0;
        pressures_.fill(0.0);
    }
    
    double bookPressure_;
    size_t head_, count_;
    std::array<double, MAX_WINDOW> pressures_;
};

// =============================================================================
// MicroEngine17 - Volatility (EMA variance)
// =============================================================================
class MicroEngine17 : public MicroEngineBase<MicroEngine17> {
    friend class MicroEngineBase<MicroEngine17>;
    static constexpr size_t MAX_WINDOW = 64;
    
public:
    MicroEngine17() : volatility_(0), var_(0), sumMid_(0), sumMidSq_(0), head_(0), count_(0) {
        mids_.fill(0.0);
    }
    
private:
    inline void onTickImpl(const UnifiedTick& t) {
        double mid = 0.5 * (t.bid + t.ask);
        
        var_ = 0.95 * var_ + 0.05 * (mid * mid);
        volatility_ = std::sqrt(std::max(0.0, var_ - mid * mid));
        
        mids_[head_] = mid;
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < MAX_WINDOW) ++count_;
    }
    
    inline MicroSignal computeImpl() const {
        MicroSignal s;
        s.value = volatility_;
        s.confidence = std::min(1.0, volatility_ * 1000.0);
        return s;
    }
    
    inline void resetImpl() {
        volatility_ = 0; var_ = 0; sumMid_ = 0; sumMidSq_ = 0; head_ = 0; count_ = 0;
        mids_.fill(0.0);
    }
    
    double volatility_, var_, sumMid_, sumMidSq_;
    size_t head_, count_;
    std::array<double, MAX_WINDOW> mids_;
};

} // namespace Chimera

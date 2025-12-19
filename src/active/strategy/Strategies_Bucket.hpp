// =============================================================================
// Strategies_Bucket.hpp - 10-Bucket Strategy System
// =============================================================================
// ARCHITECTURE:
//   - Each bucket owns ONE strategy category
//   - Buckets vote independently
//   - Final decision = weighted sum of bucket winners
//   - Risk scaling driven by specific bucket outputs
//
// BUCKETS:
//   B1 - Order Flow (OFI)            - Who is in control right now
//   B2 - Momentum (Micro-Trend)      - Directional pressure
//   B3 - Liquidity (Vacuum)          - Structural weakness
//   B4 - Reversion                   - Counter-move capture
//   B5 - Spread Regime               - Market quality
//   B6 - Aggression (Burst)          - Real intent
//   B7 - Volatility                  - Energy state
//   B8 - Execution Safety (Latency)  - Latency sanity
//   B9 - Session Bias                - Time edge
//   B10 - Confirmation               - Final gate
//
// HFT OPTIMIZATIONS:
//   - CRTP (zero virtual overhead)
//   - Fixed arrays (zero heap)
//   - Inline everything
//   - NO mutex
//   - Cache-line aligned
// =============================================================================
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include "../data/UnifiedTick.hpp"
#include "../micro/CentralMicroEngine.hpp"

namespace Chimera {

// =============================================================================
// Bucket IDs
// =============================================================================
enum class BucketId : uint8_t {
    ORDER_FLOW      = 0,   // B1
    MOMENTUM        = 1,   // B2
    LIQUIDITY       = 2,   // B3
    REVERSION       = 3,   // B4
    SPREAD_REGIME   = 4,   // B5
    AGGRESSION      = 5,   // B6
    VOLATILITY      = 6,   // B7
    EXEC_SAFETY     = 7,   // B8
    SESSION_BIAS    = 8,   // B9
    CONFIRMATION    = 9,   // B10
    NUM_BUCKETS     = 10
};

// =============================================================================
// Bucket Signal - Output from each strategy
// =============================================================================
struct BucketSignal {
    double score = 0.0;          // Score (-1 to +1)
    double confidence = 0.0;     // Confidence (0 to 1)
    int8_t direction = 0;        // -1 sell, 0 neutral, +1 buy
    BucketId bucket;
    uint64_t ts = 0;
    
    inline bool isBuy() const noexcept { return score > 0.1 && confidence > 0.3; }
    inline bool isSell() const noexcept { return score < -0.1 && confidence > 0.3; }
    inline bool isActive() const noexcept { return confidence > 0.1; }
};

// =============================================================================
// Bucket Vote - Weighted contribution to final decision
// =============================================================================
struct BucketVote {
    double weight = 1.0;         // Bucket weight in final decision
    double riskMultiplier = 1.0; // Impact on position sizing
    bool canVeto = false;        // Can this bucket block trading?
    BucketSignal signal;
};

// =============================================================================
// Aggregated Bucket Decision
// =============================================================================
struct BucketDecision {
    double totalScore = 0.0;
    double avgConfidence = 0.0;
    double riskMultiplier = 1.0;
    int8_t consensus = 0;        // -1, 0, +1
    int buyVotes = 0;
    int sellVotes = 0;
    int neutralVotes = 0;
    bool vetoed = false;
    uint64_t ts = 0;
    
    inline bool shouldBuy() const noexcept { 
        return !vetoed && consensus == 1 && avgConfidence > 0.4; 
    }
    inline bool shouldSell() const noexcept { 
        return !vetoed && consensus == -1 && avgConfidence > 0.4; 
    }
    inline bool hasConsensus() const noexcept {
        return consensus != 0 && !vetoed;
    }
};

// =============================================================================
// CRTP Strategy Base Class - Zero virtual function overhead
// =============================================================================
template<typename Derived, BucketId BUCKET>
class BucketStrategyBase {
public:
    static constexpr BucketId BUCKET_ID = BUCKET;
    
    BucketStrategyBase() : enabled_(true) {}
    ~BucketStrategyBase() = default;
    
    // CRTP dispatch - resolved at compile-time
    inline BucketSignal compute(const UnifiedTick& t, const MicrostructureSignals& signals) {
        if (!enabled_) return BucketSignal{};
        BucketSignal sig = static_cast<Derived*>(this)->computeImpl(t, signals);
        sig.bucket = BUCKET;
        return sig;
    }
    
    inline void reset() {
        static_cast<Derived*>(this)->resetImpl();
    }
    
    inline void enable(bool e) { enabled_ = e; }
    inline bool isEnabled() const { return enabled_; }
    
protected:
    bool enabled_;
};

// =============================================================================
// B1: Order Flow Imbalance Strategy (OFI)
// Purpose: Detect who is in control right now
// =============================================================================
class OFIStrategy : public BucketStrategyBase<OFIStrategy, BucketId::ORDER_FLOW> {
    friend class BucketStrategyBase<OFIStrategy, BucketId::ORDER_FLOW>;
    static constexpr size_t WINDOW = 32;
    
public:
    OFIStrategy() : lastBuyVol_(0), lastSellVol_(0), ofiEma_(0), idx_(0), count_(0) {
        history_.fill(0.0);
    }
    
private:
    inline BucketSignal computeImpl(const UnifiedTick& t, const MicrostructureSignals& sig) {
        // Delta order flow
        double buyFlow = t.buyVol - lastBuyVol_;
        double sellFlow = t.sellVol - lastSellVol_;
        lastBuyVol_ = t.buyVol;
        lastSellVol_ = t.sellVol;
        
        // OFI = net flow * price impact direction
        double ofi = (buyFlow - sellFlow);
        
        // EMA smoothing
        ofiEma_ = 0.85 * ofiEma_ + 0.15 * ofi;
        
        history_[idx_] = ofiEma_;
        idx_ = (idx_ + 1) % WINDOW;
        if (count_ < WINDOW) ++count_;
        
        // Combine with microstructure signals
        double depthBias = sig.depthImbalance;
        double flowToxicity = sig.vpin;
        
        BucketSignal s;
        s.score = ofiEma_ * 0.0001 * 0.5 + depthBias * 0.3 + flowToxicity * 0.2;
        s.confidence = std::min(1.0, std::fabs(s.score) * 5.0);
        s.direction = (s.score > 0.05) ? 1 : (s.score < -0.05) ? -1 : 0;
        s.ts = t.tsLocal;
        return s;
    }
    
    inline void resetImpl() {
        lastBuyVol_ = 0; lastSellVol_ = 0; ofiEma_ = 0;
        idx_ = 0; count_ = 0;
        history_.fill(0.0);
    }
    
    double lastBuyVol_, lastSellVol_, ofiEma_;
    size_t idx_, count_;
    std::array<double, WINDOW> history_;
};

// =============================================================================
// B2: Micro-Trend Breakout Strategy
// Purpose: Detect directional pressure
// =============================================================================
class MicroTrendStrategy : public BucketStrategyBase<MicroTrendStrategy, BucketId::MOMENTUM> {
    friend class BucketStrategyBase<MicroTrendStrategy, BucketId::MOMENTUM>;
    static constexpr size_t WINDOW = 32;
    
public:
    MicroTrendStrategy() : lastMid_(0), trend_(0), idx_(0), count_(0) {
        history_.fill(0.0);
    }
    
private:
    inline BucketSignal computeImpl(const UnifiedTick& t, const MicrostructureSignals& sig) {
        double mid = 0.5 * (t.bid + t.ask);
        double delta = mid - lastMid_;
        lastMid_ = mid;
        
        // EMA trend
        trend_ = 0.92 * trend_ + 0.08 * delta;
        
        history_[idx_] = trend_;
        idx_ = (idx_ + 1) % WINDOW;
        if (count_ < WINDOW) ++count_;
        
        // Combine with momentum signals
        double momentum = sig.momentum;
        double trendStr = sig.trendStrength;
        double depthTilt = sig.depthImbalance;
        
        BucketSignal s;
        s.score = trend_ * 0.4 + delta * 0.25 + momentum * 0.2 + trendStr * depthTilt * 0.15;
        s.confidence = std::min(1.0, std::fabs(s.score) * 10.0);
        s.direction = (s.score > 0.001) ? 1 : (s.score < -0.001) ? -1 : 0;
        s.ts = t.tsLocal;
        return s;
    }
    
    inline void resetImpl() {
        lastMid_ = 0; trend_ = 0;
        idx_ = 0; count_ = 0;
        history_.fill(0.0);
    }
    
    double lastMid_, trend_;
    size_t idx_, count_;
    std::array<double, WINDOW> history_;
};

// =============================================================================
// B3: Liquidity Vacuum Strategy
// Purpose: Detect structural weakness / liquidity gaps
// =============================================================================
class LiquidityVacuumStrategy : public BucketStrategyBase<LiquidityVacuumStrategy, BucketId::LIQUIDITY> {
    friend class BucketStrategyBase<LiquidityVacuumStrategy, BucketId::LIQUIDITY>;
    static constexpr size_t WINDOW = 32;
    
public:
    LiquidityVacuumStrategy() : lastSpread_(0), avgSpread_(0), idx_(0), count_(0) {
        history_.fill(0.0);
    }
    
private:
    inline BucketSignal computeImpl(const UnifiedTick& t, const MicrostructureSignals& sig) {
        // Spread change detection
        double spreadDelta = t.spread - lastSpread_;
        lastSpread_ = t.spread;
        
        // Average spread for deviation
        avgSpread_ = 0.95 * avgSpread_ + 0.05 * t.spread;
        double spreadDev = t.spread - avgSpread_;
        
        history_[idx_] = spreadDev;
        idx_ = (idx_ + 1) % WINDOW;
        if (count_ < WINDOW) ++count_;
        
        // Liquidity vacuum: spread expansion + depth imbalance
        double depthPressure = sig.depthImbalance;
        double intensity = sig.tradeIntensity * 0.01;
        
        // Positive = liquidity vacuum (spread widening + imbalance)
        BucketSignal s;
        s.score = spreadDev * 0.35 + spreadDelta * 0.25 + depthPressure * 0.25 + intensity * 0.15;
        s.confidence = std::min(1.0, std::fabs(s.score) * 100.0);
        s.direction = (depthPressure > 0.2) ? 1 : (depthPressure < -0.2) ? -1 : 0;
        s.ts = t.tsLocal;
        return s;
    }
    
    inline void resetImpl() {
        lastSpread_ = 0; avgSpread_ = 0;
        idx_ = 0; count_ = 0;
        history_.fill(0.0);
    }
    
    double lastSpread_, avgSpread_;
    size_t idx_, count_;
    std::array<double, WINDOW> history_;
};

// =============================================================================
// B4: Micro Mean Reversion Strategy
// Purpose: Counter-move capture
// =============================================================================
class MeanReversionStrategy : public BucketStrategyBase<MeanReversionStrategy, BucketId::REVERSION> {
    friend class BucketStrategyBase<MeanReversionStrategy, BucketId::REVERSION>;
    static constexpr size_t WINDOW = 32;
    
public:
    MeanReversionStrategy() : ema_(0), fastEma_(0), idx_(0), count_(0) {
        history_.fill(0.0);
    }
    
private:
    inline BucketSignal computeImpl(const UnifiedTick& t, const MicrostructureSignals& sig) {
        double mid = 0.5 * (t.bid + t.ask);
        
        // Dual EMA for mean detection
        ema_ = 0.95 * ema_ + 0.05 * mid;
        fastEma_ = 0.8 * fastEma_ + 0.2 * mid;
        
        // Deviation from mean
        double dev = mid - ema_;
        double fastDev = fastEma_ - ema_;
        
        history_[idx_] = dev;
        idx_ = (idx_ + 1) % WINDOW;
        if (count_ < WINDOW) ++count_;
        
        // Mean reversion: negative deviation = buy opportunity
        double skew = sig.depthImbalance;
        double vol = sig.realizedVolatility;
        
        BucketSignal s;
        // Negative score means price is extended, signal opposite direction
        s.score = -dev * 0.45 + skew * 0.35 - fastDev * 0.1 + vol * 0.1;
        s.confidence = std::min(1.0, std::fabs(dev) * 100.0);
        s.direction = (s.score > 0.001) ? 1 : (s.score < -0.001) ? -1 : 0;
        s.ts = t.tsLocal;
        return s;
    }
    
    inline void resetImpl() {
        ema_ = 0; fastEma_ = 0;
        idx_ = 0; count_ = 0;
        history_.fill(0.0);
    }
    
    double ema_, fastEma_;
    size_t idx_, count_;
    std::array<double, WINDOW> history_;
};

// =============================================================================
// B5: Spread Expansion Strategy
// Purpose: Market quality regime detection
// =============================================================================
class SpreadExpansionStrategy : public BucketStrategyBase<SpreadExpansionStrategy, BucketId::SPREAD_REGIME> {
    friend class BucketStrategyBase<SpreadExpansionStrategy, BucketId::SPREAD_REGIME>;
    static constexpr size_t WINDOW = 32;
    
public:
    SpreadExpansionStrategy() : prevBid_(0), prevAsk_(0), spreadEma_(0), idx_(0), count_(0) {
        history_.fill(0.0);
    }
    
private:
    inline BucketSignal computeImpl(const UnifiedTick& t, const MicrostructureSignals& sig) {
        // Bid/ask changes
        double bidDelta = t.bid - prevBid_;
        double askDelta = t.ask - prevAsk_;
        prevBid_ = t.bid;
        prevAsk_ = t.ask;
        
        // Spread regime
        spreadEma_ = 0.9 * spreadEma_ + 0.1 * t.spread;
        double spreadExpansion = t.spread - spreadEma_;
        
        history_[idx_] = spreadExpansion;
        idx_ = (idx_ + 1) % WINDOW;
        if (count_ < WINDOW) ++count_;
        
        // Combine bid/ask movement with spread
        double momentum = sig.momentum;
        double trendStr = sig.trendStrength;
        
        BucketSignal s;
        s.score = bidDelta * 0.3 + askDelta * 0.3 + spreadExpansion * 0.25 + momentum * trendStr * 0.15;
        s.confidence = std::min(1.0, std::fabs(s.score) * 10.0);
        s.direction = (s.score > 0.001) ? 1 : (s.score < -0.001) ? -1 : 0;
        s.ts = t.tsLocal;
        return s;
    }
    
    inline void resetImpl() {
        prevBid_ = 0; prevAsk_ = 0; spreadEma_ = 0;
        idx_ = 0; count_ = 0;
        history_.fill(0.0);
    }
    
    double prevBid_, prevAsk_, spreadEma_;
    size_t idx_, count_;
    std::array<double, WINDOW> history_;
};

// =============================================================================
// B6: Aggressor Burst Strategy
// Purpose: Detect real intent through volume bursts
// =============================================================================
class AggressorBurstStrategy : public BucketStrategyBase<AggressorBurstStrategy, BucketId::AGGRESSION> {
    friend class BucketStrategyBase<AggressorBurstStrategy, BucketId::AGGRESSION>;
    static constexpr size_t WINDOW = 32;
    
public:
    AggressorBurstStrategy() : avgVol_(0), idx_(0), count_(0) {
        history_.fill(0.0);
    }
    
private:
    inline BucketSignal computeImpl(const UnifiedTick& t, const MicrostructureSignals& sig) {
        double vol = t.buyVol + t.sellVol;
        avgVol_ = 0.9 * avgVol_ + 0.1 * vol;
        
        // Volume burst detection
        double burst = vol - avgVol_;
        double burstRatio = (avgVol_ > 0) ? (vol / avgVol_) : 1.0;
        
        history_[idx_] = burst;
        idx_ = (idx_ + 1) % WINDOW;
        if (count_ < WINDOW) ++count_;
        
        // Direction from order flow
        double imbalance = sig.orderFlowImbalance;
        double toxicity = sig.toxicity;
        double vpin = sig.vpin;
        
        // Burst + direction = aggressive intent
        BucketSignal s;
        double burstScore = burst * 0.0001;
        s.score = burstScore * imbalance * 0.4 + imbalance * 0.35 + (vpin + toxicity) * 0.25;
        s.confidence = std::min(1.0, burstRatio * 0.3 + std::fabs(imbalance) * 0.7);
        s.direction = (imbalance > 0.15) ? 1 : (imbalance < -0.15) ? -1 : 0;
        s.ts = t.tsLocal;
        return s;
    }
    
    inline void resetImpl() {
        avgVol_ = 0;
        idx_ = 0; count_ = 0;
        history_.fill(0.0);
    }
    
    double avgVol_;
    size_t idx_, count_;
    std::array<double, WINDOW> history_;
};

// =============================================================================
// B7: Volatility Expansion Strategy
// Purpose: Detect energy state / volatility regime
// =============================================================================
class VolatilityExpansionStrategy : public BucketStrategyBase<VolatilityExpansionStrategy, BucketId::VOLATILITY> {
    friend class BucketStrategyBase<VolatilityExpansionStrategy, BucketId::VOLATILITY>;
    static constexpr size_t WINDOW = 32;
    
public:
    VolatilityExpansionStrategy() : lastPrice_(0), vol_(0), avgVol_(0), idx_(0), count_(0) {
        returns_.fill(0.0);
    }
    
private:
    inline BucketSignal computeImpl(const UnifiedTick& t, const MicrostructureSignals& sig) {
        double mid = 0.5 * (t.bid + t.ask);
        
        if (lastPrice_ > 0.0) {
            double r = mid - lastPrice_;
            returns_[idx_] = r;
            idx_ = (idx_ + 1) % WINDOW;
            if (count_ < WINDOW) ++count_;
        }
        lastPrice_ = mid;
        
        if (count_ < WINDOW) {
            vol_ = 0.0;
            BucketSignal s;
            s.score = 0.0;
            s.confidence = 0.0;
            s.direction = 0;
            s.ts = t.tsLocal;
            return s;
        }
        
        // Calculate realized volatility
        double sum = 0.0;
        for (size_t i = 0; i < WINDOW; ++i) {
            sum += returns_[i] * returns_[i];
        }
        vol_ = std::sqrt(sum / WINDOW);
        
        // EMA of volatility for regime detection
        avgVol_ = 0.95 * avgVol_ + 0.05 * vol_;
        double volExpansion = vol_ - avgVol_;
        
        // Volatility expansion unlocks size, not direction
        // Direction comes from other signals
        BucketSignal s;
        s.score = volExpansion * 100.0;  // Scale for comparison
        s.confidence = std::min(1.0, vol_ * 50.0);
        s.direction = 0;  // Volatility doesn't determine direction
        s.ts = t.tsLocal;
        return s;
    }
    
    inline void resetImpl() {
        lastPrice_ = 0; vol_ = 0; avgVol_ = 0;
        idx_ = 0; count_ = 0;
        returns_.fill(0.0);
    }
    
    double lastPrice_, vol_, avgVol_;
    size_t idx_, count_;
    std::array<double, WINDOW> returns_;
};

// =============================================================================
// B8: Latency-Aware Filter Strategy
// Purpose: Suppress signals when internal latency makes them stale
// NOTE: This does NOT block trading globally - it votes negatively when latency is too high
// =============================================================================
class LatencyAwareFilterStrategy : public BucketStrategyBase<LatencyAwareFilterStrategy, BucketId::EXEC_SAFETY> {
    friend class BucketStrategyBase<LatencyAwareFilterStrategy, BucketId::EXEC_SAFETY>;
    
public:
    LatencyAwareFilterStrategy() : lastTickTs_(0), avgLatencyNs_(0), penalty_(0.0) {}
    
    // Called externally to update execution latency
    void updateExecLatency(uint64_t execLatencyNs) {
        avgLatencyNs_ = 0.9 * avgLatencyNs_ + 0.1 * static_cast<double>(execLatencyNs);
    }
    
private:
    inline BucketSignal computeImpl(const UnifiedTick& t, const MicrostructureSignals& sig) {
        // Calculate tick-to-tick latency
        uint64_t tickLatency = 0;
        if (lastTickTs_ > 0 && t.tsLocal > lastTickTs_) {
            tickLatency = t.tsLocal - lastTickTs_;
        }
        lastTickTs_ = t.tsLocal;
        
        // Combined latency assessment
        double execNs = avgLatencyNs_;
        
        // Latency tiers:
        // < 50us  = excellent, no penalty
        // 50-150us = acceptable, slight penalty
        // > 150us = stale signals, heavy penalty
        if (execNs < 50'000) {
            penalty_ = 0.0;
        } else if (execNs < 150'000) {
            penalty_ = -0.3 * ((execNs - 50'000) / 100'000);
        } else {
            penalty_ = -1.0;
        }
        
        BucketSignal s;
        s.score = penalty_;
        s.confidence = 1.0;  // Always confident about latency state
        s.direction = 0;     // Latency doesn't determine direction
        s.ts = t.tsLocal;
        return s;
    }
    
    inline void resetImpl() {
        lastTickTs_ = 0;
        avgLatencyNs_ = 0;
        penalty_ = 0.0;
    }
    
    uint64_t lastTickTs_;
    double avgLatencyNs_;
    double penalty_;
};

// =============================================================================
// B9: Time-of-Session Bias Strategy
// Purpose: Apply deterministic session bias with zero runtime indicators
// =============================================================================
class TimeOfSessionBiasStrategy : public BucketStrategyBase<TimeOfSessionBiasStrategy, BucketId::SESSION_BIAS> {
    friend class BucketStrategyBase<TimeOfSessionBiasStrategy, BucketId::SESSION_BIAS>;
    
public:
    TimeOfSessionBiasStrategy() : bias_(0.0), sessionMultiplier_(1.0) {}
    
private:
    inline BucketSignal computeImpl(const UnifiedTick& t, const MicrostructureSignals& sig) {
        // Extract hour from nanosecond timestamp (UTC)
        int hour = static_cast<int>((t.tsLocal / 3'600'000'000'000ULL) % 24);
        
        // Session windows (UTC):
        // 07:00-10:00 = London open  (moderate activity)
        // 13:00-16:00 = NY open      (high activity)
        // 21:00-00:00 = Asia open    (crypto active)
        // Others = reduced activity
        
        if (hour >= 7 && hour <= 10) {
            // London session
            bias_ = 0.4;
            sessionMultiplier_ = 1.2;
        } else if (hour >= 13 && hour <= 16) {
            // NY session - peak liquidity
            bias_ = 0.6;
            sessionMultiplier_ = 1.5;
        } else if (hour >= 21 || hour <= 0) {
            // Asia session - good for crypto
            bias_ = 0.3;
            sessionMultiplier_ = 1.1;
        } else {
            // Off-hours
            bias_ = 0.0;
            sessionMultiplier_ = 0.8;
        }
        
        BucketSignal s;
        s.score = bias_;
        s.confidence = sessionMultiplier_;  // Use confidence to pass multiplier
        s.direction = 0;  // Session doesn't determine direction
        s.ts = t.tsLocal;
        return s;
    }
    
    inline void resetImpl() {
        bias_ = 0.0;
        sessionMultiplier_ = 1.0;
    }
    
    double bias_;
    double sessionMultiplier_;
};

// =============================================================================
// B10: Price Action Confirmation Strategy
// Purpose: Final gate - confirm signal with price action
// =============================================================================
class PriceConfirmStrategy : public BucketStrategyBase<PriceConfirmStrategy, BucketId::CONFIRMATION> {
    friend class BucketStrategyBase<PriceConfirmStrategy, BucketId::CONFIRMATION>;
    static constexpr size_t WINDOW = 32;
    
public:
    PriceConfirmStrategy() : emaImpulse_(0), lastMid_(0), lastDelta_(0), idx_(0), count_(0) {
        history_.fill(0.0);
    }
    
private:
    inline BucketSignal computeImpl(const UnifiedTick& t, const MicrostructureSignals& sig) {
        double mid = 0.5 * (t.bid + t.ask);
        double dMid = mid - lastMid_;
        lastMid_ = mid;
        
        double delta = t.buyVol - t.sellVol;
        double dDelta = delta - lastDelta_;
        lastDelta_ = delta;
        
        // Impulse = price move confirmed by volume
        double impulse = dMid + dDelta * 0.0001;
        emaImpulse_ = 0.9 * emaImpulse_ + 0.1 * impulse;
        
        history_[idx_] = emaImpulse_;
        idx_ = (idx_ + 1) % WINDOW;
        if (count_ < WINDOW) ++count_;
        
        // Combine multiple confirmation signals
        double bookSlope = sig.depthImbalance;
        double momentum = sig.momentum;
        double trendStr = sig.trendStrength;
        double accel = sig.acceleration;
        
        // Strong confirmation when all align
        double microAlign = (momentum + accel + trendStr) / 3.0;
        
        BucketSignal s;
        s.score = emaImpulse_ * 0.35 + impulse * 0.25 + bookSlope * 0.25 + microAlign * 0.15;
        s.confidence = std::min(1.0, std::fabs(s.score) * 10.0);
        s.direction = (s.score > 0.001) ? 1 : (s.score < -0.001) ? -1 : 0;
        s.ts = t.tsLocal;
        return s;
    }
    
    inline void resetImpl() {
        emaImpulse_ = 0; lastMid_ = 0; lastDelta_ = 0;
        idx_ = 0; count_ = 0;
        history_.fill(0.0);
    }
    
    double emaImpulse_, lastMid_, lastDelta_;
    size_t idx_, count_;
    std::array<double, WINDOW> history_;
};

// =============================================================================
// Bucket Weights Configuration
// =============================================================================
struct BucketWeights {
    // Signal weights (for direction decision)
    std::array<double, 10> signalWeights = {{
        1.0,   // B1 - Order Flow
        1.0,   // B2 - Momentum
        0.8,   // B3 - Liquidity
        0.7,   // B4 - Reversion
        0.6,   // B5 - Spread Regime
        0.9,   // B6 - Aggression
        0.0,   // B7 - Volatility (doesn't vote direction)
        0.0,   // B8 - Exec Safety (doesn't vote direction)
        0.0,   // B9 - Session (doesn't vote direction)
        1.0    // B10 - Confirmation
    }};
    
    // Risk multiplier weights (for position sizing)
    std::array<double, 10> riskWeights = {{
        0.3,   // B1 - Order Flow increases risk
        0.2,   // B2 - Momentum increases risk
        -0.2,  // B3 - Liquidity stress reduces risk
        0.0,   // B4 - Reversion neutral
        -0.3,  // B5 - Spread expansion reduces risk
        0.3,   // B6 - Aggression increases risk
        0.4,   // B7 - Volatility increases risk
        -0.5,  // B8 - Latency reduces risk (always)
        0.2,   // B9 - Good session increases risk
        0.1    // B10 - Confirmation increases risk
    }};
    
    // Can these buckets veto trading?
    std::array<bool, 10> canVeto = {{
        false,  // B1 - Order Flow
        false,  // B2 - Momentum
        false,  // B3 - Liquidity
        false,  // B4 - Reversion
        true,   // B5 - Spread (wide spread = veto)
        false,  // B6 - Aggression
        false,  // B7 - Volatility
        true,   // B8 - Latency (high latency = veto)
        false,  // B9 - Session
        false   // B10 - Confirmation
    }};
};

// =============================================================================
// Bucket Aggregator - Combines all bucket votes into final decision
// =============================================================================
class BucketAggregator {
public:
    BucketAggregator() : weights_() {}
    
    inline BucketDecision aggregate(
        const std::array<BucketSignal, 10>& signals,
        uint64_t ts
    ) {
        BucketDecision decision;
        decision.ts = ts;
        
        double weightedScore = 0.0;
        double totalWeight = 0.0;
        double totalConfidence = 0.0;
        double riskMultiplier = 1.0;
        
        for (size_t i = 0; i < 10; ++i) {
            const auto& sig = signals[i];
            double signalWeight = weights_.signalWeights[i];
            double riskWeight = weights_.riskWeights[i];
            
            // Direction voting (only buckets with signal weights)
            if (signalWeight > 0.0 && sig.isActive()) {
                weightedScore += sig.score * signalWeight;
                totalWeight += signalWeight;
                totalConfidence += sig.confidence;
                
                if (sig.direction > 0) decision.buyVotes++;
                else if (sig.direction < 0) decision.sellVotes++;
                else decision.neutralVotes++;
            }
            
            // Risk scaling (all buckets contribute)
            if (sig.isActive()) {
                riskMultiplier *= (1.0 + riskWeight * sig.score);
            }
            
            // Veto check
            if (weights_.canVeto[i]) {
                if (i == static_cast<size_t>(BucketId::SPREAD_REGIME) && sig.score > 0.5) {
                    decision.vetoed = true;  // Spread too wide
                }
                if (i == static_cast<size_t>(BucketId::EXEC_SAFETY) && sig.score < -0.5) {
                    decision.vetoed = true;  // Latency too high
                }
            }
        }
        
        // Compute final values
        if (totalWeight > 0.0) {
            decision.totalScore = weightedScore / totalWeight;
        }
        
        int votingBuckets = decision.buyVotes + decision.sellVotes + decision.neutralVotes;
        if (votingBuckets > 0) {
            decision.avgConfidence = totalConfidence / votingBuckets;
        }
        
        // Consensus requires clear majority
        int majority = votingBuckets / 2 + 1;
        if (decision.buyVotes >= majority && decision.buyVotes > decision.sellVotes + 2) {
            decision.consensus = 1;
        } else if (decision.sellVotes >= majority && decision.sellVotes > decision.buyVotes + 2) {
            decision.consensus = -1;
        }
        
        // Clamp risk multiplier
        decision.riskMultiplier = std::clamp(riskMultiplier, 0.1, 3.0);
        
        return decision;
    }
    
    void setWeights(const BucketWeights& w) { weights_ = w; }
    const BucketWeights& getWeights() const { return weights_; }
    
private:
    BucketWeights weights_;
};

// =============================================================================
// Strategy Pack - All 10 strategies in one place
// =============================================================================
struct StrategyPack {
    OFIStrategy                  ofi;           // B1
    MicroTrendStrategy           microTrend;    // B2
    LiquidityVacuumStrategy      liquidityVac;  // B3
    MeanReversionStrategy        meanRevert;    // B4
    SpreadExpansionStrategy      spreadExp;     // B5
    AggressorBurstStrategy       aggrBurst;     // B6
    VolatilityExpansionStrategy  volExpand;     // B7
    LatencyAwareFilterStrategy   latencyFilter; // B8
    TimeOfSessionBiasStrategy    sessionBias;   // B9
    PriceConfirmStrategy         priceConfirm;  // B10
    
    BucketAggregator             aggregator;
    
    // Compute all strategies and aggregate
    inline BucketDecision compute(const UnifiedTick& t, const MicrostructureSignals& sig) {
        std::array<BucketSignal, 10> signals;
        
        signals[0] = ofi.compute(t, sig);
        signals[1] = microTrend.compute(t, sig);
        signals[2] = liquidityVac.compute(t, sig);
        signals[3] = meanRevert.compute(t, sig);
        signals[4] = spreadExp.compute(t, sig);
        signals[5] = aggrBurst.compute(t, sig);
        signals[6] = volExpand.compute(t, sig);
        signals[7] = latencyFilter.compute(t, sig);
        signals[8] = sessionBias.compute(t, sig);
        signals[9] = priceConfirm.compute(t, sig);
        
        return aggregator.aggregate(signals, t.tsLocal);
    }
    
    // Update execution latency for latency filter
    inline void updateExecLatency(uint64_t ns) {
        latencyFilter.updateExecLatency(ns);
    }
    
    // Reset all strategies
    inline void reset() {
        ofi.reset();
        microTrend.reset();
        liquidityVac.reset();
        meanRevert.reset();
        spreadExp.reset();
        aggrBurst.reset();
        volExpand.reset();
        latencyFilter.reset();
        sessionBias.reset();
        priceConfirm.reset();
    }
};

} // namespace Chimera

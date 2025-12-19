// =============================================================================
// Arbiter.hpp - Venue-Agnostic Routing Decision Engine v6.4
// =============================================================================
// DESIGN PRINCIPLES (per spec):
//   - No locks
//   - No IO
//   - No allocation
//   - Deterministic O(1)
//   - Pure atomic reads
//   - Stateful (anti-flap)
//   - Backpressure-aware
//   - PROTOCOL-AGNOSTIC (no Binance/FIX knowledge)
//
// v6.4 UNIFIED MODEL (per spec):
//   - Arbiter NEVER checks "is_binance" or "is_fix"
//   - Arbiter ONLY sees VenueHealth + Intent
//   - Single canonical VenueState: HEALTHY/DEGRADED/UNAVAILABLE
//   - Urgency-gated filtering per venue threshold
//   - Protocol logic stays in engines, not Arbiter
//
// ARBITER CONTRACT:
//   Input:  VenueHealth (per venue) + Intent
//   Output: ArbiterDecision (venue selection + allow flag)
//   Rule:   No protocol knowledge leaks into Arbiter
// =============================================================================
#pragma once

#include "Intent.hpp"
#include "ArbiterDecision.hpp"
#include "../venue/VenueHealth.hpp"
#include <atomic>
#include <cstdint>
#include <chrono>

namespace Chimera {

// =============================================================================
// Arbiter - Hot-Path Safe Venue Router (VENUE-AGNOSTIC) v6.4
// =============================================================================
// The Arbiter NEVER checks protocol-specific conditions.
// It only evaluates VenueHealth state for each venue.
// =============================================================================
class Arbiter {
public:
    // =========================================================================
    // Configurable thresholds
    // =========================================================================
    static constexpr uint64_t MAX_LATENCY_NS     = 5'000'000;    // 5ms EWMA threshold
    static constexpr uint64_t MAX_TAIL_NS        = 15'000'000;   // 15ms tail threshold (p99)
    static constexpr uint64_t MIN_HOLD_NS        = 10'000'000;   // 10ms venue hold (anti-flap)
    static constexpr uint64_t MAX_INTENT_AGE_NS  = 50'000'000;   // 50ms intent freshness
    static constexpr double   MIN_CONFIDENCE     = 0.55;
    static constexpr double   MIN_URGENCY        = 0.0;
    
    // Per-venue urgency thresholds for DEGRADED state (configurable per venue)
    // These are configured by the engine, not hardcoded protocol knowledge
    static constexpr double   DEFAULT_DEGRADED_URGENCY_THRESHOLD = 0.3;
    
    // =========================================================================
    // Constructor - Takes venue health references (VENUE-AGNOSTIC)
    // =========================================================================
    // Note: venue1/venue2 are generic - could be any venue type
    // The Arbiter doesn't know or care what protocol they use
    // =========================================================================
    Arbiter(const VenueHealth& venue1,
            const VenueHealth& venue2,
            const std::atomic<bool>& global_kill)
        : venue1_(venue1)
        , venue2_(venue2)
        , global_kill_(global_kill)
        , max_latency_ns_(MAX_LATENCY_NS)
        , max_tail_ns_(MAX_TAIL_NS)
        , min_hold_ns_(MIN_HOLD_NS)
        , max_intent_age_ns_(MAX_INTENT_AGE_NS)
        , min_confidence_(MIN_CONFIDENCE)
        , venue1_degraded_urgency_(DEFAULT_DEGRADED_URGENCY_THRESHOLD)
        , venue2_degraded_urgency_(DEFAULT_DEGRADED_URGENCY_THRESHOLD)
        , last_venue_(ArbiterVenue::None)
        , last_switch_ts_(0)
    {}
    
    // =========================================================================
    // Configuration: Set per-venue urgency thresholds
    // =========================================================================
    // This allows engines to configure venue-specific behavior without
    // leaking protocol knowledge into the Arbiter
    // =========================================================================
    void setVenue1DegradedUrgency(double threshold) noexcept {
        venue1_degraded_urgency_ = threshold;
    }
    
    void setVenue2DegradedUrgency(double threshold) noexcept {
        venue2_degraded_urgency_ = threshold;
    }
    
    void setMaxLatency(uint64_t ns) noexcept { max_latency_ns_ = ns; }
    void setMaxTail(uint64_t ns) noexcept { max_tail_ns_ = ns; }
    void setMinHold(uint64_t ns) noexcept { min_hold_ns_ = ns; }
    void setMaxIntentAge(uint64_t ns) noexcept { max_intent_age_ns_ = ns; }
    void setMinConfidence(double c) noexcept { min_confidence_ = c; }
    
    // =========================================================================
    // Main decision function (stateful, venue-agnostic) v6.4
    // =========================================================================
    inline ArbiterDecision decide(const Intent& intent) noexcept {
        // 1. Global kill switch
        if (global_kill_.load(std::memory_order_relaxed)) {
            return { ArbiterVenue::None, false };
        }
        
        // 2. Confidence threshold
        if (intent.confidence < min_confidence_) {
            return { ArbiterVenue::None, false };
        }
        
        // 3. Intent freshness check
        const uint64_t now = nowNs();
        if (intent.ts_ns > 0 && (now - intent.ts_ns) > max_intent_age_ns_) {
            return { ArbiterVenue::None, false };
        }
        
        // 4. Check venue health (VENUE-AGNOSTIC - uses unified venue_allows)
        bool venue1_ok = venue_allows(venue1_, intent.urgency, venue1_degraded_urgency_, now, max_latency_ns_);
        bool venue2_ok = venue_allows(venue2_, intent.urgency, venue2_degraded_urgency_, now, max_latency_ns_);
        
        // 5. No venue available
        if (!venue1_ok && !venue2_ok) {
            return { ArbiterVenue::None, false };
        }
        
        // 6. Enforce venue hold (anti-flap) - STATEFUL
        const ArbiterVenue last = last_venue_.load(std::memory_order_relaxed);
        const uint64_t last_ts = last_switch_ts_.load(std::memory_order_relaxed);
        
        if (last != ArbiterVenue::None && (now - last_ts) < min_hold_ns_) {
            // Within hold period - stick with last venue if still healthy
            if (last == ArbiterVenue::Binance && venue1_ok) {
                return { ArbiterVenue::Binance, true };
            }
            if (last == ArbiterVenue::CTrader && venue2_ok) {
                return { ArbiterVenue::CTrader, true };
            }
            // Last venue unhealthy - force switch (allowed even during hold)
        }
        
        // 7. Route based on intent venue preference and availability
        ArbiterVenue selected = selectVenue(intent, venue1_ok, venue2_ok, now);
        
        // 8. Update state if venue changed
        if (selected != last) {
            last_venue_.store(selected, std::memory_order_relaxed);
            last_switch_ts_.store(now, std::memory_order_relaxed);
        }
        
        return { selected, selected != ArbiterVenue::None };
    }
    
    // =========================================================================
    // Single-venue check (for engine-specific use)
    // =========================================================================
    inline bool checkVenue1Ok(const Intent& intent) const noexcept {
        return venue_allows(venue1_, intent.urgency, venue1_degraded_urgency_, nowNs(), max_latency_ns_);
    }
    
    inline bool checkVenue2Ok(const Intent& intent) const noexcept {
        return venue_allows(venue2_, intent.urgency, venue2_degraded_urgency_, nowNs(), max_latency_ns_);
    }
    
    // =========================================================================
    // Legacy compatibility aliases
    // =========================================================================
    inline bool checkBinanceOk(const Intent& intent) const noexcept {
        return checkVenue1Ok(intent);
    }
    
    inline bool checkCTraderOk(const Intent& intent) const noexcept {
        return checkVenue2Ok(intent);
    }
    
    // =========================================================================
    // State accessors (for monitoring/metrics)
    // =========================================================================
    inline VenueState getVenue1State() const noexcept {
        return venue1_.state.load(std::memory_order_relaxed);
    }
    
    inline VenueState getVenue2State() const noexcept {
        return venue2_.state.load(std::memory_order_relaxed);
    }
    
    inline const char* getVenue1StateStr() const noexcept {
        return venue1_.stateStr();
    }
    
    inline const char* getVenue2StateStr() const noexcept {
        return venue2_.stateStr();
    }
    
    // Legacy aliases
    inline VenueState getBinanceState() const noexcept { return getVenue1State(); }
    inline VenueState getCTraderState() const noexcept { return getVenue2State(); }
    inline const char* getBinanceStateStr() const noexcept { return getVenue1StateStr(); }
    inline const char* getCTraderStateStr() const noexcept { return getVenue2StateStr(); }
    
    inline uint64_t getVenue1LatencyNs() const noexcept {
        return venue1_.latency_ewma_ns.load(std::memory_order_relaxed);
    }
    
    inline uint64_t getVenue2LatencyNs() const noexcept {
        return venue2_.latency_ewma_ns.load(std::memory_order_relaxed);
    }
    
    inline uint64_t getVenue1TailNs() const noexcept {
        return venue1_.latency_max_window_ns.load(std::memory_order_relaxed);
    }
    
    inline uint64_t getVenue2TailNs() const noexcept {
        return venue2_.latency_max_window_ns.load(std::memory_order_relaxed);
    }
    
    // Legacy aliases
    inline uint64_t getBinanceLatencyNs() const noexcept { return getVenue1LatencyNs(); }
    inline uint64_t getCTraderLatencyNs() const noexcept { return getVenue2LatencyNs(); }
    inline uint64_t getBinanceTailNs() const noexcept { return getVenue1TailNs(); }
    inline uint64_t getCTraderTailNs() const noexcept { return getVenue2TailNs(); }
    
    inline bool isVenue1Alive() const noexcept {
        return venue1_.state.load(std::memory_order_relaxed) == VenueState::HEALTHY;
    }
    
    inline bool isVenue2Alive() const noexcept {
        return venue2_.state.load(std::memory_order_relaxed) == VenueState::HEALTHY;
    }
    
    // Legacy aliases
    inline bool isBinanceAlive() const noexcept { return isVenue1Alive(); }
    inline bool isCTraderAlive() const noexcept { return isVenue2Alive(); }
    
    inline ArbiterVenue getLastVenue() const noexcept {
        return last_venue_.load(std::memory_order_relaxed);
    }
    
    inline uint64_t getLastSwitchTs() const noexcept {
        return last_switch_ts_.load(std::memory_order_relaxed);
    }
    
    inline uint32_t getVenue1Pending() const noexcept {
        return venue1_.pending_orders.load(std::memory_order_relaxed);
    }
    
    inline uint32_t getVenue2Pending() const noexcept {
        return venue2_.pending_orders.load(std::memory_order_relaxed);
    }
    
    inline uint32_t getVenue1Rejects() const noexcept {
        return venue1_.recent_rejects.load(std::memory_order_relaxed);
    }
    
    inline uint32_t getVenue2Rejects() const noexcept {
        return venue2_.recent_rejects.load(std::memory_order_relaxed);
    }
    
    // Legacy aliases
    inline uint32_t getBinancePending() const noexcept { return getVenue1Pending(); }
    inline uint32_t getCTraderPending() const noexcept { return getVenue2Pending(); }
    inline uint32_t getBinanceRejects() const noexcept { return getVenue1Rejects(); }
    inline uint32_t getCTraderRejects() const noexcept { return getVenue2Rejects(); }
    
    // =========================================================================
    // Reset state (for testing/recovery)
    // =========================================================================
    void reset() noexcept {
        last_venue_.store(ArbiterVenue::None, std::memory_order_relaxed);
        last_switch_ts_.store(0, std::memory_order_relaxed);
    }

private:
    // =========================================================================
    // Venue selection logic (intent-based routing)
    // =========================================================================
    inline ArbiterVenue selectVenue(
        const Intent& intent,
        bool venue1_ok,
        bool venue2_ok,
        uint64_t now
    ) const noexcept {
        // If intent specifies a venue, honor it if available
        if (intent.venue != Venue::UNKNOWN) {
            if (intent.venue == Venue::BINANCE && venue1_ok) {
                return ArbiterVenue::Binance;
            }
            if (intent.venue == Venue::CTRADER && venue2_ok) {
                return ArbiterVenue::CTrader;
            }
        }
        
        // Both available - prefer based on latency
        if (venue1_ok && venue2_ok) {
            uint64_t lat1 = venue1_.latency_ewma_ns.load(std::memory_order_relaxed);
            uint64_t lat2 = venue2_.latency_ewma_ns.load(std::memory_order_relaxed);
            
            // Prefer lower latency, with hysteresis to avoid flapping
            // Only switch if difference is significant (>20%)
            if (lat1 > 0 && lat2 > 0) {
                if (lat1 < lat2 * 0.8) return ArbiterVenue::Binance;
                if (lat2 < lat1 * 0.8) return ArbiterVenue::CTrader;
            }
            
            // Default to venue1 (Binance) if roughly equal
            return ArbiterVenue::Binance;
        }
        
        // Only one available
        if (venue1_ok) return ArbiterVenue::Binance;
        if (venue2_ok) return ArbiterVenue::CTrader;
        
        return ArbiterVenue::None;
    }
    
    // Inline clock read (HFT-safe)
    inline uint64_t nowNs() const noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

private:
    // Venue health references (venue-agnostic)
    const VenueHealth& venue1_;  // e.g., Binance
    const VenueHealth& venue2_;  // e.g., cTrader/FIX
    const std::atomic<bool>& global_kill_;
    
    // Configurable thresholds
    uint64_t max_latency_ns_;
    uint64_t max_tail_ns_;
    uint64_t min_hold_ns_;
    uint64_t max_intent_age_ns_;
    double min_confidence_;
    
    // Per-venue urgency thresholds (configured by engines)
    double venue1_degraded_urgency_;
    double venue2_degraded_urgency_;
    
    // Stateful routing (anti-flap)
    std::atomic<ArbiterVenue> last_venue_;
    std::atomic<uint64_t> last_switch_ts_;
};

} // namespace Chimera

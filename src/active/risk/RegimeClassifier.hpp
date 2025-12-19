// =============================================================================
// RegimeClassifier.hpp - RESTORED ORIGINAL API (LOCKED)
// =============================================================================
// DO NOT MODIFY - This is the locked API surface
// Uses MicroMetrics from pipeline (original design)
// =============================================================================
#pragma once
#include "../pipeline/MicroMetrics.hpp"
#include <cstdint>

namespace Chimera {

// Original regime enum (LOCKED)
enum class Regime {
    Quiet    = 0,
    Trend    = 1,
    Volatile = 2,
    Shocked  = 3
};

// Compatibility mapping for HFT layer
// Maps: STABLE→Quiet, NORMAL→Trend, VOLATILE→Volatile, HALTED→Shocked
enum class MarketRegime : uint8_t {
    STABLE   = 0,  // = Quiet
    NORMAL   = 1,  // = Trend
    VOLATILE = 2,  // = Volatile
    HALTED   = 3   // = Shocked
};

class RegimeClassifier {
public:
    // Original API (MicroMetrics-based) - LOCKED
    static Regime classify(const MicroMetrics& m) noexcept;
    
    // Compatibility API for HFT hardening layer
    MarketRegime classify(
        double volatility,
        double spread,
        double liquidity,
        uint64_t latency_ns) noexcept;

    // Get string representation
    static const char* regime_str(Regime r) noexcept {
        switch (r) {
            case Regime::Quiet:    return "QUIET";
            case Regime::Trend:    return "TREND";
            case Regime::Volatile: return "VOLATILE";
            case Regime::Shocked:  return "SHOCKED";
            default:               return "UNKNOWN";
        }
    }
    
    static const char* regime_str(MarketRegime r) noexcept {
        switch (r) {
            case MarketRegime::STABLE:   return "STABLE";
            case MarketRegime::NORMAL:   return "NORMAL";
            case MarketRegime::VOLATILE: return "VOLATILE";
            case MarketRegime::HALTED:   return "HALTED";
            default:                     return "UNKNOWN";
        }
    }

private:
    static constexpr double trendThresh = 1.0;
    static constexpr double volThresh = 0.02;
    
    // HFT layer thresholds
    double high_vol_threshold = 0.02;
    double tight_spread_threshold = 0.0005;
    double deep_liquidity_threshold = 100000;
    uint64_t max_latency_ns = 5000000;
};

} // namespace Chimera

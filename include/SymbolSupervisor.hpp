#pragma once
// ==============================================================================
// SymbolSupervisor — per-symbol regime classifier + engine permission layer
//
// Architecture:
//   market data → RegimeClassifier → supervisor scorecard → engine permissions
//
// Regimes:
//   QUIET_COMPRESSION    — tight range, ambiguous direction → bracket preferred
//   EXPANSION_BREAKOUT   — vol expanding, momentum aligning → breakout preferred
//   TREND_CONTINUATION   — direction established, follow-through likely → breakout
//   CHOP_REVERSAL        — repeated false breaks, noisy → both blocked
//   HIGH_RISK_NO_TRADE   — spread/slippage/session bad → all blocked
//
// One supervisor per symbol. Call update() on every tick.
// Read allow_bracket / allow_breakout before arming either engine.
// ==============================================================================

#include <string>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <deque>

namespace omega {

// ── Regime enum ───────────────────────────────────────────────────────────────
enum class Regime : uint8_t {
    QUIET_COMPRESSION  = 0,
    EXPANSION_BREAKOUT = 1,
    TREND_CONTINUATION = 2,
    CHOP_REVERSAL      = 3,
    HIGH_RISK_NO_TRADE = 4,
    UNKNOWN            = 5,
};

inline const char* regime_name(Regime r) noexcept {
    switch (r) {
        case Regime::QUIET_COMPRESSION:  return "QUIET_COMPRESSION";
        case Regime::EXPANSION_BREAKOUT: return "EXPANSION_BREAKOUT";
        case Regime::TREND_CONTINUATION: return "TREND_CONTINUATION";
        case Regime::CHOP_REVERSAL:      return "CHOP_REVERSAL";
        case Regime::HIGH_RISK_NO_TRADE: return "HIGH_RISK_NO_TRADE";
        default:                         return "UNKNOWN";
    }
}

// ── Supervisor config — set per symbol from symbols.ini ──────────────────────
struct SupervisorConfig {
    bool   allow_bracket           = true;
    bool   allow_breakout          = true;
    double min_regime_confidence   = 0.55;  // below this → HIGH_RISK_NO_TRADE
    double min_engine_win_margin   = 0.10;  // bracket/breakout scores must differ by this to pick a winner
    int    max_false_breaks        = 2;     // false breaks in window before CHOP block
    double max_spread_pct          = 0.10;  // above this → HIGH_RISK_NO_TRADE
    double compression_thresh      = 0.60;  // recent_vol/base_vol ratio below this = compression
    double expansion_thresh        = 0.80;  // recent_vol/base_vol ratio above this = expansion
    double momentum_trend_thresh   = 0.015; // |momentum| above this = directional
    bool   bracket_in_quiet_comp   = true;  // prefer bracket during QUIET_COMPRESSION
    bool   breakout_in_trend       = true;  // prefer breakout during TREND_CONTINUATION
};

// ── Supervisor decision output ────────────────────────────────────────────────
struct SupervisorDecision {
    Regime      regime           = Regime::UNKNOWN;
    double      confidence       = 0.0;
    double      bracket_score    = 0.0;
    double      breakout_score   = 0.0;
    bool        allow_bracket    = false;
    bool        allow_breakout   = false;
    const char* winner           = "NONE";
    const char* reason           = "";
};

// ── SymbolSupervisor ──────────────────────────────────────────────────────────
class SymbolSupervisor {
public:
    std::string        symbol;
    SupervisorConfig   cfg;

    // Call on every tick. Returns the current decision.
    SupervisorDecision update(
        double bid, double ask,
        double recent_vol_pct,   // short-window vol as % of price
        double base_vol_pct,     // long-window baseline vol
        double momentum_pct,     // price_now vs price_20_ago as %
        double comp_high,        // current compression high (0 if not in compression)
        double comp_low,         // current compression low
        bool   in_compression,   // engine is in COMPRESSION phase
        int    false_break_count // recent false break count (tracked by caller)
    ) noexcept
    {
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        if (mid <= 0.0) return last_;

        const double spread_pct = spread / mid * 100.0;
        const double vol_ratio  = (base_vol_pct > 0.0) ? (recent_vol_pct / base_vol_pct) : 1.0;
        const double comp_range = (comp_high > comp_low) ? (comp_high - comp_low) : 0.0;
        const double comp_pct   = (mid > 0.0 && comp_range > 0.0) ? (comp_range / mid) : 0.0;
        const double mom_abs    = std::fabs(momentum_pct);

        // ── Score each feature ─────────────────────────────────────────────────

        // Compression score: high when range is tight relative to price and vol is suppressed
        double compression_score = 0.0;
        if (in_compression && comp_pct > 0.0) {
            const double tightness  = std::max(0.0, 1.0 - comp_pct * 200.0); // tighter = higher
            const double vol_suppression = std::max(0.0, 1.0 - vol_ratio);
            compression_score = (tightness + vol_suppression) * 0.5;
        }

        // Expansion score: high when vol is expanding beyond baseline
        const double expansion_score = std::max(0.0, (vol_ratio - cfg.expansion_thresh)
                                                      / (1.0 - cfg.expansion_thresh + 0.001));

        // Directional confidence: momentum aligned and above noise threshold
        const double dir_score = std::min(1.0, mom_abs / (cfg.momentum_trend_thresh * 2.0 + 0.001));

        // Execution quality: spread relative to max allowed
        const double exec_score = std::max(0.0, 1.0 - (spread_pct / cfg.max_spread_pct));

        // False break risk: penalise if we've seen repeated failures
        const double trap_risk = std::min(1.0, static_cast<double>(false_break_count)
                                               / static_cast<double>(cfg.max_false_breaks + 1));

        // ── Classify regime ────────────────────────────────────────────────────

        Regime regime     = Regime::UNKNOWN;
        double confidence = 0.0;
        const char* reason = "";

        if (spread_pct > cfg.max_spread_pct || exec_score < 0.1) {
            regime     = Regime::HIGH_RISK_NO_TRADE;
            confidence = exec_score < 0.1 ? 0.9 : (spread_pct / cfg.max_spread_pct);
            confidence = std::min(1.0, confidence);
            reason     = "spread_too_wide";
        } else if (trap_risk >= 0.8) {
            regime     = Regime::CHOP_REVERSAL;
            confidence = trap_risk;
            reason     = "repeated_false_breaks";
        } else if (in_compression && vol_ratio < cfg.compression_thresh && comp_score_ok(comp_pct)) {
            regime     = Regime::QUIET_COMPRESSION;
            confidence = compression_score * exec_score;
            reason     = "tight_range_low_vol";
        } else if (expansion_score > 0.3 && dir_score > 0.4) {
            if (dir_score > 0.7 && vol_ratio > 1.2) {
                regime     = Regime::TREND_CONTINUATION;
                confidence = (dir_score + expansion_score) * 0.5 * exec_score;
                reason     = "strong_momentum_expansion";
            } else {
                regime     = Regime::EXPANSION_BREAKOUT;
                confidence = (expansion_score + dir_score) * 0.5 * exec_score;
                reason     = "vol_expanding_direction_emerging";
            }
        } else if (trap_risk > 0.4) {
            regime     = Regime::CHOP_REVERSAL;
            confidence = trap_risk * exec_score;
            reason     = "weak_follow_through";
        } else {
            // Low confidence — default to conservative
            regime     = Regime::HIGH_RISK_NO_TRADE;
            confidence = 0.5;
            reason     = "low_confidence";
        }

        // ── Score engines for this regime ──────────────────────────────────────

        double bracket_score  = 0.0;
        double breakout_score = 0.0;

        switch (regime) {
            case Regime::QUIET_COMPRESSION:
                bracket_score  = compression_score * 0.8 + exec_score * 0.2;
                breakout_score = expansion_score   * 0.6 + dir_score  * 0.4;
                break;
            case Regime::EXPANSION_BREAKOUT:
                bracket_score  = compression_score * 0.3 + exec_score * 0.2;
                breakout_score = expansion_score   * 0.5 + dir_score  * 0.3 + exec_score * 0.2;
                break;
            case Regime::TREND_CONTINUATION:
                bracket_score  = 0.1;
                breakout_score = dir_score * 0.6 + expansion_score * 0.3 + exec_score * 0.1;
                break;
            case Regime::CHOP_REVERSAL:
            case Regime::HIGH_RISK_NO_TRADE:
                bracket_score  = 0.0;
                breakout_score = 0.0;
                break;
            default:
                break;
        }

        // ── Determine permissions ──────────────────────────────────────────────

        SupervisorDecision d{};
        d.regime        = regime;
        d.confidence    = confidence;
        d.bracket_score = bracket_score;
        d.breakout_score= breakout_score;
        d.reason        = reason;

        const bool regime_ok = (confidence >= cfg.min_regime_confidence)
                            && (regime != Regime::CHOP_REVERSAL)
                            && (regime != Regime::HIGH_RISK_NO_TRADE);

        if (!regime_ok) {
            d.allow_bracket  = false;
            d.allow_breakout = false;
            d.winner = "NONE";
        } else {
            const double margin = bracket_score - breakout_score;
            if (std::fabs(margin) < cfg.min_engine_win_margin) {
                // Tie — use config preference
                if (regime == Regime::QUIET_COMPRESSION && cfg.bracket_in_quiet_comp) {
                    d.allow_bracket  = cfg.allow_bracket;
                    d.allow_breakout = false;
                    d.winner = "BRACKET";
                } else {
                    d.allow_bracket  = false;
                    d.allow_breakout = cfg.allow_breakout;
                    d.winner = "BREAKOUT";
                }
            } else if (margin > 0) {
                d.allow_bracket  = cfg.allow_bracket;
                d.allow_breakout = false;
                d.winner = "BRACKET";
            } else {
                d.allow_bracket  = false;
                d.allow_breakout = cfg.allow_breakout;
                d.winner = "BREAKOUT";
            }
        }

        // Log on regime change
        if (d.regime != last_.regime || d.winner != last_.winner) {
            std::cout << "[SUPERVISOR-" << symbol << "]"
                      << " regime=" << regime_name(d.regime)
                      << " conf="   << std::fixed << std::setprecision(2) << d.confidence
                      << " bracket=" << d.bracket_score
                      << " breakout=" << d.breakout_score
                      << " winner=" << d.winner
                      << " allow_trade=" << (d.allow_bracket || d.allow_breakout ? 1 : 0)
                      << " reason=" << d.reason << "\n";
            std::cout.flush();
        }

        last_ = d;
        return d;
    }

    const SupervisorDecision& last() const noexcept { return last_; }

private:
    SupervisorDecision last_;

    static bool comp_score_ok(double comp_pct) noexcept {
        // Compression is meaningful only when range is non-trivial
        return comp_pct > 0.00005; // at least 0.005% of price
    }
};

} // namespace omega

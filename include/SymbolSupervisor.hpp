#pragma once
// ==============================================================================
// SymbolSupervisor — per-symbol regime classifier + engine permission layer
//
// Fixes applied vs original:
//   1. Hard winner score floor: min_winner_score — blocks 0.12 vs 0.09 noise
//   2. Explicit no-trade dominance: low confidence gives real score, not 0.5
//   3. Bracket eagerness guard: min_bracket_score — higher floor for bracket
//   4. Edge quality feedback: net_edge_pct param boosts breakout score
//   5. Slippage is dynamic in edge model — not supervisor's domain
//   6. Bad-regime memory: cooldown after N consecutive blocked decisions
// ==============================================================================
#include <string>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <chrono>

namespace omega {

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

struct SupervisorConfig {
    bool   allow_bracket           = true;
    bool   allow_breakout          = true;
    double min_regime_confidence   = 0.55;
    double min_engine_win_margin   = 0.10;
    // Fix 1+2: absolute winner score floor — both engines scoring low blocks the trade
    double min_winner_score        = 0.25;
    // Fix 3: bracket-specific floor — higher because bracket places two live orders
    double min_bracket_score       = 0.35;
    int    max_false_breaks        = 2;
    double max_spread_pct          = 0.10;
    double compression_thresh      = 0.55;  // vol_ratio below this = compression
    double expansion_thresh        = 0.85;  // vol_ratio above this = expansion
    double momentum_trend_thresh   = 0.015;
    bool   bracket_in_quiet_comp   = true;
    bool   breakout_in_trend       = true;
    // Fix 6: bad-regime memory — raised threshold to 20 (was 3, fired constantly)
    int     cooldown_fail_threshold = 20;
    int64_t cooldown_duration_ms    = 120000; // 2 minutes
};

struct SupervisorDecision {
    Regime      regime           = Regime::UNKNOWN;
    double      confidence       = 0.0;
    double      bracket_score    = 0.0;
    double      breakout_score   = 0.0;
    bool        allow_bracket    = false;
    bool        allow_breakout   = false;
    const char* winner           = "NONE";
    const char* reason           = "";
    bool        in_cooldown      = false;
};

class SymbolSupervisor {
public:
    std::string      symbol;
    SupervisorConfig cfg;

    // net_edge_pct = edge.net_edge / mid from last EdgeResult (0 if unavailable)
    SupervisorDecision update(
        double bid, double ask,
        double recent_vol_pct,
        double base_vol_pct,
        double momentum_pct,
        double comp_high,
        double comp_low,
        bool   in_compression,
        int    false_break_count,
        double net_edge_pct = 0.0
    ) noexcept
    {
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        if (mid <= 0.0) return last_;

        // Fix 6: cooldown check — block everything during penalty period
        // Early exit allowed if signal is very strong (top_score > 0.45) — market
        // may have become genuinely good during cooldown window
        const int64_t now_ms = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (now_ms < m_cooldown_until_ms) {
            // Peek at scores to check for early-exit condition.
            // We cannot use the scores computed later (not yet computed here),
            // so use a fast proxy: if vol is clearly expanding + momentum strong,
            // break cooldown early. This avoids missing the best entry.
            const double fast_vol_ratio = (base_vol_pct > 0.0)
                                          ? (recent_vol_pct / base_vol_pct) : 1.0;
            const double fast_dir       = std::fabs(momentum_pct)
                                          / (cfg.momentum_trend_thresh * 2.0 + 0.001);
            const double fast_edge      = std::min(0.15, net_edge_pct * 8.0);
            const double fast_score     = std::min(1.0,
                fast_vol_ratio * 0.4 + fast_dir * 0.4 + fast_edge * 3.0);
            const bool early_exit = (fast_score > 0.45);
            if (!early_exit) {
                SupervisorDecision b{};
                b.regime        = Regime::HIGH_RISK_NO_TRADE;
                b.confidence    = 1.0;
                b.winner        = "NONE";
                b.reason        = "symbol_in_cooldown";
                b.in_cooldown   = true;
                if (b.regime != last_.regime || b.in_cooldown != last_.in_cooldown) {
                    std::cout << "[SUPERVISOR-" << symbol << "] IN_COOLDOWN until +"
                              << (m_cooldown_until_ms - now_ms) / 1000 << "s\n";
                    std::cout.flush();
                }
                last_ = b;
                return b;
            }
            // Strong signal — break cooldown early
            m_cooldown_until_ms = 0;
            m_consecutive_blocks = 0;
            std::cout << "[SUPERVISOR-" << symbol << "] COOLDOWN EARLY EXIT"
                      << " fast_score=" << fast_score << "\n";
            std::cout.flush();
        }

        const double spread_pct = spread / mid * 100.0;
        const double vol_ratio  = (base_vol_pct > 0.0)
                                  ? (recent_vol_pct / base_vol_pct) : 1.0;
        const double comp_range = (comp_high > comp_low) ? (comp_high - comp_low) : 0.0;
        const double comp_pct   = (mid > 0.0 && comp_range > 0.0) ? (comp_range / mid) : 0.0;
        const double mom_abs    = std::fabs(momentum_pct);

        // ── Feature scores ─────────────────────────────────────────────────────
        double compression_score = 0.0;
        if (in_compression && comp_pct > 0.0) {
            const double tightness       = std::max(0.0, 1.0 - comp_pct * 200.0);
            const double vol_suppression = std::max(0.0, 1.0 - vol_ratio);
            compression_score = (tightness + vol_suppression) * 0.5;
        }
        const double expansion_score = std::max(0.0,
            (vol_ratio - cfg.expansion_thresh) / (1.0 - cfg.expansion_thresh + 0.001));
        const double dir_score = std::min(1.0,
            mom_abs / (cfg.momentum_trend_thresh * 2.0 + 0.001));
        const double exec_score = std::max(0.0,
            1.0 - (spread_pct / cfg.max_spread_pct));
        const double trap_risk = std::min(1.0,
            static_cast<double>(false_break_count)
            / static_cast<double>(cfg.max_false_breaks + 1));

        // Fix 4: edge quality boost — capped lower (0.15) and scaled conservatively (×8)
        // to prevent weak structure + high edge dominating the score
        const double edge_boost = std::min(0.15, std::max(0.0, net_edge_pct * 8.0));

        // ── Regime classification ──────────────────────────────────────────────
        Regime      regime     = Regime::UNKNOWN;
        double      confidence = 0.0;
        const char* reason     = "";

        if (spread_pct > cfg.max_spread_pct || exec_score < 0.1) {
            regime     = Regime::HIGH_RISK_NO_TRADE;
            confidence = std::min(1.0, spread_pct / cfg.max_spread_pct);
            reason     = "spread_too_wide";
        } else if (trap_risk >= 0.8) {
            regime     = Regime::CHOP_REVERSAL;
            confidence = trap_risk;
            reason     = "repeated_false_breaks";
        } else if (in_compression && vol_ratio < cfg.compression_thresh && comp_pct > 0.00005) {
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
            // Fix 2: genuine low-confidence — compute real score, don't hardcode 0.5
            regime     = Regime::HIGH_RISK_NO_TRADE;
            confidence = compression_score * 0.3 + expansion_score * 0.3 + dir_score * 0.4;
            reason     = "no_dominant_regime";
        }

        // ── Engine scores ──────────────────────────────────────────────────────
        double bracket_score  = 0.0;
        double breakout_score = 0.0;

        switch (regime) {
            case Regime::QUIET_COMPRESSION:
                bracket_score  = compression_score * 0.8 + exec_score * 0.2;
                breakout_score = expansion_score   * 0.6 + dir_score  * 0.4 + edge_boost;
                break;
            case Regime::EXPANSION_BREAKOUT:
                bracket_score  = compression_score * 0.3 + exec_score * 0.2;
                breakout_score = expansion_score   * 0.5 + dir_score  * 0.3
                               + exec_score * 0.2 + edge_boost;
                break;
            case Regime::TREND_CONTINUATION:
                bracket_score  = 0.05;
                breakout_score = dir_score * 0.6 + expansion_score * 0.3
                               + exec_score * 0.1 + edge_boost;
                break;
            case Regime::CHOP_REVERSAL:
            case Regime::HIGH_RISK_NO_TRADE:
            default:
                bracket_score  = 0.0;
                breakout_score = 0.0;
                break;
        }

        // ── Regime hysteresis ─────────────────────────────────────────────────
        // Key insight from live session: HIGH_RISK_NO_TRADE was resetting the
        // candidate counter, so tradeable regimes could never accumulate the
        // required hold ticks. A single noisy HIGH_RISK tick wiped all progress.
        //
        // Rules:
        //   CHOP/HIGH_RISK: block trading immediately but do NOT reset the
        //     candidate counter for a previously-building tradeable regime.
        //     The candidate keeps building — we just don't trade yet.
        //   Tradeable regimes: must hold REGIME_HOLD_TICKS consecutive ticks.
        //     A single HIGH_RISK tick between two EXPANSION ticks is noise.
        //   Regime change to different tradeable regime: reset counter.
        const bool is_blocking_regime = (regime == Regime::HIGH_RISK_NO_TRADE
                                      || regime == Regime::CHOP_REVERSAL);
        if (!is_blocking_regime) {
            if (regime == m_candidate_regime) {
                ++m_candidate_count;
            } else {
                // Switched to a different tradeable regime — reset
                m_candidate_regime = regime;
                m_candidate_count  = 1;
            }
        }
        // Stable regime: use candidate if held long enough, else keep last stable
        // (not HIGH_RISK — that was the bug). Fall back to UNKNOWN if no candidate.
        const bool candidate_stable = (m_candidate_count >= REGIME_HOLD_TICKS)
                                   && !is_blocking_regime;
        const Regime stable_regime  = candidate_stable
                                      ? m_candidate_regime
                                      : (is_blocking_regime ? Regime::HIGH_RISK_NO_TRADE
                                                            : last_.regime);

        // Recompute scores for the stable regime
        double stable_bracket  = 0.0;
        double stable_breakout = 0.0;
        switch (stable_regime) {
            case Regime::QUIET_COMPRESSION:
                stable_bracket  = bracket_score;
                stable_breakout = breakout_score;
                break;
            case Regime::EXPANSION_BREAKOUT:
            case Regime::TREND_CONTINUATION:
                stable_bracket  = bracket_score;
                stable_breakout = breakout_score;
                break;
            default:
                break;
        }

        // ── Permission decision — 3-layer gate ────────────────────────────────
        SupervisorDecision d{};
        d.regime         = stable_regime;
        d.confidence     = confidence;
        d.bracket_score  = stable_bracket;
        d.breakout_score = stable_breakout;
        d.reason         = reason;

        const bool regime_ok     = (stable_regime != Regime::CHOP_REVERSAL)
                                && (stable_regime != Regime::HIGH_RISK_NO_TRADE);
        const bool confidence_ok = (confidence >= cfg.min_regime_confidence);
        const double top_score   = std::max(stable_bracket, stable_breakout);
        const bool   score_ok    = (top_score >= cfg.min_winner_score);

        if (!regime_ok || !confidence_ok || !score_ok) {
            d.allow_bracket  = false;
            d.allow_breakout = false;
            // Distinguish "no setup" from "good setup but engine unavailable"
            // This is purely for log clarity — both result in NONE
            if (regime_ok && confidence_ok && top_score >= cfg.min_winner_score) {
                d.winner = "NONE";
                d.reason = "bracket_unavailable"; // regime good, no engine can act
            } else {
                d.winner = "NONE";
            }
            // Cooldown only counts CHOP or repeated false-break conditions —
            // NOT plain inactivity (no_dominant_regime).
            // Punishing quiet symbols for having no setup causes missed entries
            // when those symbols eventually move.
            if (regime == Regime::CHOP_REVERSAL
                    || (regime == Regime::HIGH_RISK_NO_TRADE
                        && reason != std::string("no_dominant_regime"))) {
                ++m_consecutive_blocks;
                if (m_consecutive_blocks >= cfg.cooldown_fail_threshold) {
                    m_cooldown_until_ms  = now_ms + cfg.cooldown_duration_ms;
                    m_consecutive_blocks = 0;
                    std::cout << "[SUPERVISOR-" << symbol << "] COOLDOWN triggered"
                              << " after " << cfg.cooldown_fail_threshold << " blocks"
                              << " for " << cfg.cooldown_duration_ms / 1000 << "s"
                              << " reason=" << reason << "\n";
                    std::cout.flush();
                }
            }
        } else {
            m_consecutive_blocks = 0;

            const double margin  = stable_bracket - stable_breakout;
            const bool margin_ok = std::fabs(margin) >= cfg.min_engine_win_margin;

            // Breakout only valid in EXPANSION or TREND — not in QUIET_COMPRESSION.
            // In compression the market hasn't moved yet; granting breakout here
            // produces allow=1 with no signal because the engine blocks on vol_gate.
            const bool breakout_regime_ok = (stable_regime == Regime::EXPANSION_BREAKOUT
                                          || stable_regime == Regime::TREND_CONTINUATION);
            const bool breakout_qualifies = breakout_regime_ok
                                         && (stable_breakout >= cfg.min_winner_score)
                                         && cfg.allow_breakout;
            const bool bracket_qualifies  = (stable_bracket >= cfg.min_bracket_score)
                                         && cfg.allow_bracket;

            if (!margin_ok) {
                if (stable_regime == Regime::QUIET_COMPRESSION
                        && cfg.bracket_in_quiet_comp && bracket_qualifies) {
                    d.allow_bracket = true;
                    d.winner        = "BRACKET";
                } else if (breakout_qualifies) {
                    d.allow_breakout = true;
                    d.winner         = "BREAKOUT";
                }
            } else if (margin > 0) {
                if (bracket_qualifies) {
                    d.allow_bracket = true;
                    d.winner        = "BRACKET";
                } else if (breakout_qualifies) {
                    d.allow_breakout = true;
                    d.winner         = "BREAKOUT";
                }
            } else {
                if (breakout_qualifies) {
                    d.allow_breakout = true;
                    d.winner         = "BREAKOUT";
                }
            }

            if (!d.allow_bracket && !d.allow_breakout)
                d.winner = "NONE";
        }

        // Log on change — includes top_score and threshold for tuning visibility
        if (d.regime != last_.regime || d.winner != last_.winner
                || d.in_cooldown != last_.in_cooldown) {
            std::cout << "[SUPERVISOR-" << symbol << "]"
                      << " regime="    << regime_name(d.regime)
                      << " conf="      << std::fixed << std::setprecision(2) << d.confidence
                      << " bracket="   << d.bracket_score
                      << " breakout="  << d.breakout_score
                      << " top_score=" << top_score
                      << " threshold=" << cfg.min_winner_score
                      << " winner="    << d.winner
                      << " allow="     << (d.allow_bracket || d.allow_breakout ? 1 : 0)
                      << " cooldown="  << d.in_cooldown
                      << " reason="    << d.reason << "\n";
            std::cout.flush();
        }

        last_ = d;
        return d;
    }

    const SupervisorDecision& last() const noexcept { return last_; }

    // Call after a winning trade — decay failure counter
    void on_trade_success() noexcept {
        m_consecutive_blocks = std::max(0, m_consecutive_blocks - 1);
    }

private:
    SupervisorDecision last_;
    int     m_consecutive_blocks  = 0;
    int64_t m_cooldown_until_ms   = 0;
    // Hysteresis: candidate regime must hold for this many ticks before switching
    Regime  m_candidate_regime    = Regime::UNKNOWN;
    int     m_candidate_count     = 0;
    static constexpr int REGIME_HOLD_TICKS = 2;  // 2 consecutive ticks before regime is stable
};

} // namespace omega

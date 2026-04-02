#pragma once
// ==============================================================================
// SymbolSupervisor -- per-symbol regime classifier + engine permission layer
//
// Fixes applied vs original:
//   1. Hard winner score floor: min_winner_score -- blocks 0.12 vs 0.09 noise
//   2. Explicit no-trade dominance: low confidence gives real score, not 0.5
//   3. Bracket eagerness guard: min_bracket_score -- higher floor for bracket
//   4. Edge quality feedback: net_edge_pct param boosts breakout score
//   5. Slippage is dynamic in edge model -- not supervisor's domain
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
    double min_regime_confidence   = 0.45;  // was 0.55 -- too tight, caused allow=1/0 alternation
    double min_engine_win_margin   = 0.10;
    // Fix 1+2: absolute winner score floor -- both engines scoring low blocks the trade
    double min_winner_score        = 0.25;
    // Fix 3: bracket-specific floor -- higher because bracket places two live orders
    double min_bracket_score       = 0.35;
    int    max_false_breaks        = 2;
    double max_spread_pct          = 0.10;
    double compression_thresh      = 0.85;  // vol_ratio below this = compression
    // Must be >= engine COMPRESSION_THRESHOLD (0.85). Previously 0.55, which was
    // far below the engine threshold (0.85), meaning the supervisor never saw
    // QUIET_COMPRESSION in the vol_ratio range 0.55-0.85 where the engine operates.
    // Result: FX engines permanently blocked as HIGH_RISK_NO_TRADE.
    double expansion_thresh        = 0.85;  // vol_ratio above this = expansion
    double momentum_trend_thresh   = 0.015;
    bool   bracket_in_quiet_comp   = true;
    bool   breakout_in_trend       = true;
    // Fix 6: bad-regime memory -- raised threshold to 20 (was 3, fired constantly)
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

        // Fix 6: cooldown check -- block everything during penalty period
        // Early exit allowed if signal is very strong (top_score > 0.45) -- market
        // may have become genuinely good during cooldown window
        const int64_t now_ms = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (now_ms < m_cooldown_until_ms) {
            // Early exit only on genuinely exceptional signal -- ALL three components
            // must be strong simultaneously. Previously threshold=0.75 with weight
            // distribution that let normal momentum alone score 0.78+ every tick,
            // making cooldown completely ineffective (bypassed on every tick).
            //
            // New formula: all three must contribute meaningfully:
            //   vol_ratio >= 2.0x baseline  (real expansion, not just normal vol)
            //   momentum  >= 3x trend_thresh (strong directional move, not drift)
            //   net_edge  >= meaningful positive edge
            // Score must exceed 0.90 -- requires all three, not just one.
            const double fast_vol_ratio = std::min(1.5, (base_vol_pct > 0.0)
                                          ? (recent_vol_pct / base_vol_pct) : 1.0);
            const double fast_dir       = std::fabs(momentum_pct)
                                          / (cfg.momentum_trend_thresh * 3.0 + 0.001); // was *2.0
            const double fast_edge      = std::min(0.15, net_edge_pct * 8.0);
            const double fast_score     = std::min(1.0,
                fast_vol_ratio * 0.2 + fast_dir * 0.4 + fast_edge * 8.0); // edge weighted more
            const bool early_exit = (fast_score > 0.90  // raised 0.75?0.90
                                  && fast_vol_ratio > 1.2   // must have real vol expansion
                                  && fast_dir > 0.8          // must have real momentum
                                  && fast_edge > 0.01);      // must have positive edge
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
            // Genuinely strong signal -- break cooldown early
            m_cooldown_until_ms = 0;
            m_consecutive_blocks = 0;
            std::cout << "[SUPERVISOR-" << symbol << "] COOLDOWN EARLY EXIT"
                      << " fast_score=" << fast_score << "\n";
            std::cout.flush();
        }

        const double spread_pct = spread / mid * 100.0;
        // cold_start: base_vol_pct not yet computed (engine buffer < BASELINE_LOOKBACK).
        // Previously fell back to vol_ratio=1.0 which lands in the ambiguous zone
        // (not < compression_thresh, not enough dir for expansion) ? HIGH_RISK every tick.
        // The engine itself still guards against entries during warmup, so passing
        // QUIET_COMPRESSION here is safe -- it just lets ticks accumulate unblocked.
        const bool   cold_start = (base_vol_pct <= 0.0);
        const double vol_ratio  = cold_start
                                  ? 0.5   // force into QUIET_COMPRESSION during warmup
                                  : (recent_vol_pct / base_vol_pct);
        const double comp_range = (comp_high > comp_low) ? (comp_high - comp_low) : 0.0;
        const double comp_pct   = (mid > 0.0 && comp_range > 0.0) ? (comp_range / mid) : 0.0;
        const double mom_abs    = std::fabs(momentum_pct);

        // ?? Feature scores ?????????????????????????????????????????????????????
        // compression_score: measures how compressed the market is.
        // Previously gated on in_compression (engine phase), causing a deadlock:
        //   supervisor needs in_compression=true to classify QUIET_COMPRESSION
        //   engine needs QUIET_COMPRESSION to enter compression
        //   ? FX engines could never start, especially overnight low-vol pairs.
        // Fix: vol_suppression is computable from vol_ratio alone.
        // tightness uses comp_range if available (engine in compression), else 1.0
        // (fully tight -- no structural range data yet, assume max tightness).
        double compression_score = 0.0;
        {
            const double vol_suppression = std::max(0.0, 1.0 - vol_ratio);
            const double tightness = (in_compression && comp_pct > 0.0)
                ? std::max(0.0, 1.0 - comp_pct * 200.0)
                : 1.0;  // no range data yet -- assume fully tight
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

        // Fix 4: edge quality boost -- capped lower (0.15) and scaled conservatively (?8)
        // to prevent weak structure + high edge dominating the score
        const double edge_boost = std::min(0.15, std::max(0.0, net_edge_pct * 8.0));

        // ?? Regime classification ??????????????????????????????????????????????
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
        } else if (vol_ratio < cfg.compression_thresh) {
            // QUIET_COMPRESSION: vol is suppressed relative to baseline.
            // Previously required in_compression=true (engine phase), creating a deadlock
            // for FX pairs where vol_ratio < threshold but engine couldn't enter compression
            // because the supervisor was blocking it. vol_ratio alone determines compression.
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
        } else if (dir_score < 0.2) {
            // Ambiguous zone: vol_ratio in compression-expansion band (0.85-1.0) with no
            // directional momentum. Previously fell to HIGH_RISK "no_dominant_regime",
            // permanently blocking all engines. A flat undirected market is compression --
            // classify it so the engine can accumulate ticks and eventually trade.
            // The engine's own compression/breakout gates remain active.
            regime     = Regime::QUIET_COMPRESSION;
            confidence = compression_score * exec_score;
            reason     = "flat_undirected_treated_as_compression";
        } else if (dir_score < 0.5 && expansion_score < 0.3) {
            // Ambiguous with slight momentum but no real expansion -- still treat as compression.
            // Previously fell to HIGH_RISK ("no_dominant_regime") blocking all trading.
            // A market with moderate dir_score but no expansion is not trending -- it's coiling.
            regime     = Regime::QUIET_COMPRESSION;
            confidence = compression_score * exec_score;
            reason     = "weak_momentum_treated_as_compression";
        } else {
            // Fix 2: genuine low-confidence -- compute real score, don't hardcode 0.5
            regime     = Regime::HIGH_RISK_NO_TRADE;
            confidence = compression_score * 0.3 + expansion_score * 0.3 + dir_score * 0.4;
            reason     = "no_dominant_regime";
        }

        // ?? Engine scores ??????????????????????????????????????????????????????
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
                // In a trend, bracket re-arms on each compression leg.
                // Score = exec_score (spread/latency quality) since the trend IS the signal.
                // dir_score included to confirm direction is still present.
                bracket_score  = exec_score * 0.6 + dir_score * 0.4;
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

        // ?? Regime hysteresis ?????????????????????????????????????????????????
        // Key insight from live session: HIGH_RISK_NO_TRADE was resetting the
        // candidate counter, so tradeable regimes could never accumulate the
        // required hold ticks. A single noisy HIGH_RISK tick wiped all progress.
        //
        // Rules:
        //   CHOP/HIGH_RISK: block trading immediately but do NOT reset the
        //     candidate counter for a previously-building tradeable regime.
        //     The candidate keeps building -- we just don't trade yet.
        //   Tradeable regimes: must hold REGIME_HOLD_TICKS consecutive ticks.
        //     A single HIGH_RISK tick between two EXPANSION ticks is noise.
        //   Regime change to different tradeable regime: reset counter.
        const bool is_blocking_regime = (regime == Regime::HIGH_RISK_NO_TRADE
                                      || regime == Regime::CHOP_REVERSAL);
        if (!is_blocking_regime) {
            if (regime == m_candidate_regime) {
                ++m_candidate_count;
            } else if (
                (regime == Regime::QUIET_COMPRESSION
                    && m_candidate_regime == Regime::EXPANSION_BREAKOUT) ||
                (regime == Regime::EXPANSION_BREAKOUT
                    && m_candidate_regime == Regime::QUIET_COMPRESSION)
            ) {
                ++m_candidate_count;
                m_candidate_regime = regime;
            } else {
                // Fix 2: only switch regime if time-based hold is met
                // Prevents tick-by-tick flipping that kills entries
                const bool time_ok = (now_ms - m_candidate_start_ms) >= REGIME_HOLD_MS;
                if (time_ok || m_candidate_count == 0) {
                    m_candidate_regime   = regime;
                    m_candidate_count    = 1;
                    m_candidate_start_ms = now_ms;
                } else {
                    ++m_candidate_count;  // keep building current candidate
                }
            }
        }
        // Stable regime: promote candidate once it's held long enough.
        // KEY FIX: candidate_stable must NOT depend on is_blocking_regime.
        // Previously: candidate_stable = (count >= HOLD) && !is_blocking
        // This meant any single HIGH_RISK noise tick forced stable=false regardless
        // of accumulated count -- the hysteresis gave zero protection.
        // Now: once count >= HOLD_TICKS, the regime is considered stable.
        // A blocking tick does NOT revoke stability -- it is absorbed as noise.
        // Stability is only broken when a genuinely different tradeable regime
        // accumulates its own HOLD_TICKS count (handled by the reset below).
        const bool candidate_stable = (m_candidate_count >= REGIME_HOLD_TICKS);
        const Regime stable_regime  = candidate_stable
                                      ? m_candidate_regime
                                      : (is_blocking_regime
                                         ? Regime::HIGH_RISK_NO_TRADE
                                         : (m_candidate_count > 0
                                            ? m_candidate_regime
                                            : Regime::HIGH_RISK_NO_TRADE));

        // Scores for the stable regime.
        // When blocking regime (HIGH_RISK/CHOP), scores are 0 by definition.
        // Always reuse last cached non-zero scores -- never let a blocking tick zero out a valid setup.
        // Previously only activated when candidate_stable=true, but supervisor flips too fast
        // for candidate_stable to accumulate, causing score collapse on every noisy tick.
        double stable_bracket  = 0.0;
        double stable_breakout = 0.0;
        if (is_blocking_regime) {
            // Always use cache -- blocking tick must never zero a valid score
            stable_bracket  = m_last_stable_bracket;
            stable_breakout = m_last_stable_breakout;
        } else {
            switch (stable_regime) {
                case Regime::QUIET_COMPRESSION:
                case Regime::EXPANSION_BREAKOUT:
                case Regime::TREND_CONTINUATION:
                    stable_bracket  = bracket_score;
                    stable_breakout = breakout_score;
                    m_last_stable_bracket  = bracket_score;
                    m_last_stable_breakout = breakout_score;
                    break;
                default:
                    // Don't clear cache -- keep last valid scores
                    stable_bracket  = m_last_stable_bracket;
                    stable_breakout = m_last_stable_breakout;
                    break;
            }
        }

        // ?? Permission decision ???????????????????????????????????????????????
        // CHOP always blocks immediately -- genuine structure failure, not noise.
        //
        // HIGH_RISK uses symmetric hysteresis:
        //   Promotion:  tradeable regime must hold REGIME_HOLD_TICKS before allow=1
        //   Revocation: HIGH_RISK must hold HIGH_RISK_REVOKE_TICKS before allow=0
        //               on a previously stable candidate.
        //
        // This prevents the flip-flop (single HIGH_RISK tick aborting compression)
        // while still revoking on genuine sustained HIGH_RISK conditions.
        //
        // If candidate is not yet stable: HIGH_RISK blocks immediately (strict).
        // If candidate is stable: require HIGH_RISK_REVOKE_TICKS consecutive
        //   HIGH_RISK ticks before revoking the stable candidate.
        SupervisorDecision d{};
        d.regime         = stable_regime;
        d.confidence     = confidence;
        d.bracket_score  = stable_bracket;
        d.breakout_score = stable_breakout;
        d.reason         = reason;

        const double top_score = std::max(stable_bracket, stable_breakout);

        // Update HIGH_RISK revocation counter
        if (regime == Regime::HIGH_RISK_NO_TRADE && candidate_stable) {
            ++m_high_risk_ticks;
        } else {
            m_high_risk_ticks = 0;
        }

        // Update CHOP revocation counter -- same hysteresis pattern as HIGH_RISK.
        // Previously: chop = instant block on any single CHOP tick.
        // Problem: a 150pt trend leg dips into low-vol-expansion territory for
        // 1-2 ticks, trap_risk > 0.4 tips to CHOP_REVERSAL, locked forever.
        // Fix: require CHOP_REVOKE_TICKS consecutive CHOP ticks before blocking.
        // If candidate is not yet stable, CHOP blocks immediately (strict, same as HR).
        // If candidate IS stable, absorb up to CHOP_REVOKE_TICKS-1 CHOP ticks as noise.
        if (regime == Regime::CHOP_REVERSAL && candidate_stable) {
            ++m_chop_ticks;
        } else {
            m_chop_ticks = 0;
        }

        const bool chop      = candidate_stable
            ? (m_chop_ticks >= CHOP_REVOKE_TICKS)
            : (regime == Regime::CHOP_REVERSAL);
        // HIGH_RISK blocks if:
        //   a) candidate not yet stable (single tick is enough), OR
        //   b) candidate stable but HIGH_RISK has persisted for REVOKE_TICKS
        const bool high_risk = candidate_stable
            ? (m_high_risk_ticks >= HIGH_RISK_REVOKE_TICKS)
            : (regime == Regime::HIGH_RISK_NO_TRADE);
        const bool blocked   = chop || high_risk || (top_score < cfg.min_winner_score);

        if (blocked) {
            d.allow_bracket  = false;
            d.allow_breakout = false;
            d.winner         = "NONE";
            if      (chop)      d.reason = "chop_detected";
            else if (high_risk) d.reason = "high_risk_no_trade";
            else                d.reason = "score_below_threshold";
            // Increment consecutive-block counter on CHOP or persistent HIGH_RISK.
            // Previously only CHOP incremented -- a symbol stuck in permanent HIGH_RISK
            // (e.g. permanently wide spreads) would never trigger the cooldown and would
            // log every tick silently forever.
            if (chop || high_risk) {
                ++m_consecutive_blocks;
                if (m_consecutive_blocks >= cfg.cooldown_fail_threshold) {
                    m_cooldown_until_ms  = now_ms + cfg.cooldown_duration_ms;
                    m_consecutive_blocks = 0;
                }
            }
        } else {
            m_consecutive_blocks = 0;
            d.reason = "valid_signal";

            // ?? Regime-gated permission ???????????????????????????????????????
            // QUIET_COMPRESSION = price is coiling, not breaking -- only bracket allowed
            // EXPANSION_BREAKOUT / TREND_CONTINUATION = breakout allowed
            // breakout_in_trend:    if false, block breakout during TREND_CONTINUATION
            // bracket_in_quiet_comp: if false, block bracket during QUIET_COMPRESSION
            const bool regime_allows_breakout =
                (stable_regime == Regime::EXPANSION_BREAKOUT) ||
                (stable_regime == Regime::TREND_CONTINUATION && cfg.breakout_in_trend);

            const bool regime_allows_bracket =
                (stable_regime == Regime::EXPANSION_BREAKOUT) ||
                (stable_regime == Regime::TREND_CONTINUATION) ||
                (stable_regime == Regime::QUIET_COMPRESSION && cfg.bracket_in_quiet_comp);

            if (cfg.allow_breakout && regime_allows_breakout) {
                d.allow_breakout = true;
                d.winner         = "BREAKOUT";
            }
            if (cfg.allow_bracket && regime_allows_bracket && stable_bracket >= cfg.min_bracket_score) {
                d.allow_bracket = true;
                d.winner        = d.allow_breakout ? "BREAKOUT" : "BRACKET";
            }
            if (!d.allow_bracket && !d.allow_breakout) {
                d.winner = "NONE";
                d.reason = "regime_not_tradeable";  // QUIET_COMPRESSION with no bracket
            }
        }

        // Log on change -- includes top_score and threshold for tuning visibility
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

    // Call after a winning trade -- decay failure counter
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
    int64_t m_candidate_start_ms  = 0;  // Fix 2: time-based minimum hold
    // Score cache: last valid scores from a non-blocking tick
    double  m_last_stable_bracket  = 0.0;
    double  m_last_stable_breakout = 0.0;
    // Consecutive HIGH_RISK ticks while candidate_stable=true -- used for revocation
    int     m_high_risk_ticks      = 0;
    // Consecutive CHOP ticks -- CHOP now uses same hysteresis as HIGH_RISK.
    // Root cause: a 150pt sustained downtrend tips into QUIET_COMPRESSION vol territory,
    // trap_risk > 0.4 flips one tick to CHOP_REVERSAL, instant block, locked forever.
    // Fix: CHOP_REVOKE_TICKS consecutive CHOP ticks required before blocking.
    // Single CHOP ticks during a trend leg are noise -- absorbed, not acted on.
    int     m_chop_ticks           = 0;
    // Fix 2: reduced from 4 -- supervisor was too slow to stabilise
    static constexpr int REGIME_HOLD_TICKS     = 1;  // was 2 -- single non-blocking tick enough to stabilize
    // Fix 2: minimum ms a regime must hold before switching (prevents tick-by-tick flipping)
    static constexpr int64_t REGIME_HOLD_MS    = 1500;  // 1.5 seconds -- faster regime promotion
    // HIGH_RISK must hold this many consecutive ticks to revoke a stable candidate.
    // 5 ticks at ~1-3 ticks/sec = 2-5 seconds of sustained HIGH_RISK before revoke.
    // Single noisy ticks (1-2) are absorbed. Genuine sustained HIGH_RISK (5+) revokes.
    static constexpr int HIGH_RISK_REVOKE_TICKS = 5;
    // CHOP must sustain for this many ticks before blocking.
    // 8 ticks ~ 3-8 seconds. Genuine chop sustains; a trend leg dipping into
    // low-vol classification for 1-2 ticks does not.
    static constexpr int CHOP_REVOKE_TICKS = 8;
};

} // namespace omega

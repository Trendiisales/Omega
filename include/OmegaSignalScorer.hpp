#pragma once
// =============================================================================
// OmegaSignalScorer.hpp -- Composite signal scoring for GoldFlow entry quality
//
// Replaces the sequential hard-gate model with a weighted point system.
// Each condition contributes points toward an entry score.
// Entry is allowed when score >= SCORE_MIN_ENTRY (5 out of 13 max).
//
// HARD GATES (unchanged -- these are risk controls, not signal quality):
//   NY_CLOSE_NOISE, LONDON_OPEN_NOISE, BARS_NOT_READY, SPREAD_ANOMALY,
//   HIGH_IMPACT_WINDOW, COST_GATE, COMPRESSION_NO_VOL, RSI extreme
//   (non-momentum regime), COUNTER_TREND (non-momentum regime)
//
// SCORED CONDITIONS (replaces remaining soft gates):
//   Points  Condition
//   +2      EMA trend aligned (M1 EMA9 > EMA50 for LONG, < for SHORT)
//   +2      ADX trending (>= 25) -- confirmed directional move
//   +2      L2 imbalance strong (>0.75 LONG / <0.25 SHORT)
//   +1      VWAP direction aligned with signal
//   +1      Microprice bias aligned with signal (>0 LONG / <0 SHORT)
//   +1      ATR expanding -- volatility supporting a move
//   +1      Vol delta aligned (rolling buy/sell pressure)
//   +1      RSI in healthy zone: 30-70 (not stretched, not diverging)
//   +1      BBW squeeze releasing (squeeze was active, now releasing)
//   +1      ADX strong (>= 40) -- high conviction trend
//   ----
//   13 max  SCORE_MIN_ENTRY = 5 (38% of max -- deliberately low to restore trades)
//
// The score is exposed as an atomic int for GUI/logging.
// log_score() prints full breakdown on every scored tick.
//
// Thread safety: all reads are from atomics (relaxed). score() is const and
// reads only atomics -- safe to call from any thread.
// =============================================================================

#include <atomic>
#include <cstdio>
#include "OHLCBarEngine.hpp"

// Minimum score threshold -- defined here so ScoreResult can reference it
// without a forward declaration.
static constexpr int OMEGA_SCORE_MIN_ENTRY = 5;

// =============================================================================
// ScoreResult -- breakdown of a single scoring evaluation
// =============================================================================
struct ScoreResult {
    int total      = 0;    // total points scored
    int max_points = 13;   // maximum achievable

    // Individual component scores (0 or their point value)
    int pts_ema_trend    = 0;  // +2: EMA9 vs EMA50 aligned
    int pts_adx_trend    = 0;  // +2: ADX >= 25
    int pts_l2_strong    = 0;  // +2: L2 imbalance strongly aligned
    int pts_vwap_align   = 0;  // +1: VWAP direction aligned
    int pts_microprice   = 0;  // +1: microprice bias aligned
    int pts_atr_expand   = 0;  // +1: ATR expanding
    int pts_vol_delta    = 0;  // +1: vol delta (buy/sell pressure) aligned
    int pts_rsi_healthy  = 0;  // +1: RSI in 30-70 zone
    int pts_bbw_release  = 0;  // +1: BBW squeeze releasing
    int pts_adx_strong   = 0;  // +1: ADX >= 40

    bool passes() const { return total >= OMEGA_SCORE_MIN_ENTRY; }
};

// =============================================================================
// OmegaSignalScorer
// =============================================================================
class OmegaSignalScorer {
public:
    // Minimum score to allow entry -- see OMEGA_SCORE_MIN_ENTRY above
    static constexpr int SCORE_MIN_ENTRY = OMEGA_SCORE_MIN_ENTRY;

    // L2 imbalance thresholds (mirror GFE_LONG_THRESHOLD)
    static constexpr double L2_STRONG_LONG  = 0.75;
    static constexpr double L2_STRONG_SHORT = 0.25;

    // Microprice bias threshold -- above this = bid pressure, below = ask pressure
    static constexpr double MICROPRICE_BIAS_THR = 0.003;  // 0.3 pts for XAUUSD

    // Vol delta alignment threshold -- rolling buy/sell pressure
    static constexpr double VOL_DELTA_THR = 0.10;  // 10% net buy/sell bias

    // RSI healthy zone -- avoid stretched extremes and divergence territory
    static constexpr double RSI_HEALTHY_LOW  = 30.0;
    static constexpr double RSI_HEALTHY_HIGH = 70.0;

    // Last computed score -- exposed for GUI / watchdog
    std::atomic<int> last_score{0};
    std::atomic<bool> last_passed{false};

    // =========================================================================
    // score()
    // Evaluate all conditions for a given direction (is_long) and return
    // a ScoreResult with the total and per-component breakdown.
    //
    // Parameters:
    //   ind          : M1 bar indicators from OHLCBarEngine
    //   is_long      : true = evaluating a LONG signal, false = SHORT
    //   l2_imbalance : current gold L2 imbalance (0..1, >0.5 = bid heavy)
    //   microprice   : gold microprice bias (signed, pts)
    //
    // Returns ScoreResult. Call result.passes() to check entry threshold.
    // =========================================================================
    ScoreResult score(const BarIndicators& ind,
                      bool   is_long,
                      double l2_imbalance,
                      double microprice) const noexcept
    {
        ScoreResult r;

        // ?? +2: EMA trend aligned ????????????????????????????????????????????
        // M1 EMA9 vs EMA50 crossover. Only scored when live (m1_ema_live=true).
        // Stale disk-loaded EMAs are not trusted for direction.
        if (ind.m1_ema_live.load(std::memory_order_relaxed)) {
            const double e9  = ind.ema9 .load(std::memory_order_relaxed);
            const double e50 = ind.ema50.load(std::memory_order_relaxed);
            if (e9 > 0.0 && e50 > 0.0) {
                const bool aligned = is_long ? (e9 > e50) : (e9 < e50);
                if (aligned) r.pts_ema_trend = 2;
            }
        }

        // ?? +2: ADX trending (>= 25) ???????????????????????????????????????????
        if (ind.adx_trending.load(std::memory_order_relaxed)) {
            r.pts_adx_trend = 2;
        }

        // ?? +2: L2 imbalance strongly aligned ?????????????????????????????????????
        // >0.75 = strong bid pressure -> LONG; <0.25 = strong ask pressure -> SHORT
        {
            const bool l2_long  = (l2_imbalance >= L2_STRONG_LONG);
            const bool l2_short = (l2_imbalance <= L2_STRONG_SHORT);
            if ((is_long && l2_long) || (!is_long && l2_short)) {
                r.pts_l2_strong = 2;
            }
        }

        // ?? +1: VWAP direction aligned ?????????????????????????????????????????
        {
            const int vwap_dir = ind.vwap_direction.load(std::memory_order_relaxed);
            if ((is_long && vwap_dir == +1) || (!is_long && vwap_dir == -1)) {
                r.pts_vwap_align = 1;
            }
        }

        // ?? +1: Microprice bias aligned ????????????????????????????????????????
        // Microprice = weighted mid from L2, bias = microprice - mid.
        // Positive = bids heavier at top of book -> bullish pressure.
        {
            const bool mp_long  = (microprice >  MICROPRICE_BIAS_THR);
            const bool mp_short = (microprice < -MICROPRICE_BIAS_THR);
            if ((is_long && mp_long) || (!is_long && mp_short)) {
                r.pts_microprice = 1;
            }
        }

        // ?? +1: ATR expanding ?????????????????????????????????????????????????
        if (ind.atr_expanding.load(std::memory_order_relaxed)) {
            r.pts_atr_expand = 1;
        }

        // ?? +1: Vol delta aligned (rolling buy/sell pressure) ????????????????
        // vol_delta_ratio: 100-tick rolling average, -1..+1
        // +0.10 = net buying pressure -> supports LONG
        // -0.10 = net selling pressure -> supports SHORT
        {
            const double vd = ind.vol_delta_ratio.load(std::memory_order_relaxed);
            if ((is_long && vd > VOL_DELTA_THR) || (!is_long && vd < -VOL_DELTA_THR)) {
                r.pts_vol_delta = 1;
            }
        }

        // ?? +1: RSI in healthy zone (30-70) ????????????????????????????????????
        // Avoids stretched RSI at extremes where mean-reversion risk is highest.
        // Note: momentum regime (ADX>=25 + ATR expanding) handles RSI extremes
        // separately via is_momentum_regime() -- this is the non-momentum case.
        {
            const double rsi = ind.rsi14.load(std::memory_order_relaxed);
            if (rsi >= RSI_HEALTHY_LOW && rsi <= RSI_HEALTHY_HIGH) {
                r.pts_rsi_healthy = 1;
            }
        }

        // ?? +1: BBW squeeze releasing ?????????????????????????????????????????
        // Squeeze was active (coiling) and is now releasing -> breakout energy.
        // Score when: squeeze was detected (bb_squeeze_bars >= 1) AND
        //             current BBW > bb_width_min * 1.10 (bands starting to expand)
        {
            const int    sq_bars = ind.bb_squeeze_bars.load(std::memory_order_relaxed);
            const double bb_w    = ind.bb_width    .load(std::memory_order_relaxed);
            const double bb_wmin = ind.bb_width_min.load(std::memory_order_relaxed);
            const bool releasing = (sq_bars >= 1) && (bb_wmin > 0.0)
                                && (bb_w > bb_wmin * 1.10);
            if (releasing) r.pts_bbw_release = 1;
        }

        // ?? +1: ADX strong (>= 40) -- high conviction trend ?????????????????
        if (ind.adx_strong.load(std::memory_order_relaxed)) {
            r.pts_adx_strong = 1;
        }

        // Sum all components
        r.total = r.pts_ema_trend
                + r.pts_adx_trend
                + r.pts_l2_strong
                + r.pts_vwap_align
                + r.pts_microprice
                + r.pts_atr_expand
                + r.pts_vol_delta
                + r.pts_rsi_healthy
                + r.pts_bbw_release
                + r.pts_adx_strong;

        return r;
    }

    // =========================================================================
    // score_and_store()
    // Calls score() and stores result in last_score / last_passed atomics.
    // Use this variant in the hot path so GUI/watchdog can read current score.
    // =========================================================================
    ScoreResult score_and_store(const BarIndicators& ind,
                                bool   is_long,
                                double l2_imbalance,
                                double microprice) noexcept
    {
        const ScoreResult r = score(ind, is_long, l2_imbalance, microprice);
        last_score .store(r.total,     std::memory_order_relaxed);
        last_passed.store(r.passes(),  std::memory_order_relaxed);
        return r;
    }

    // =========================================================================
    // log_score()
    // Full diagnostic breakdown -- call on every entry attempt.
    // Format is single line for grep-ability.
    // =========================================================================
    void log_score(const ScoreResult& r, bool is_long) const noexcept {
        printf("[SCORER] %s score=%d/%d %s | "
               "ema=%d adx_trend=%d l2=%d vwap=%d mp=%d "
               "atr_exp=%d vdelta=%d rsi=%d bbw_rel=%d adx_str=%d\n",
               is_long ? "LONG" : "SHORT",
               r.total, r.max_points,
               r.passes() ? "PASS" : "FAIL",
               r.pts_ema_trend, r.pts_adx_trend, r.pts_l2_strong,
               r.pts_vwap_align, r.pts_microprice,
               r.pts_atr_expand, r.pts_vol_delta, r.pts_rsi_healthy,
               r.pts_bbw_release, r.pts_adx_strong);
        fflush(stdout);
    }
};

#pragma once
// =============================================================================
// OmegaSignalScorer.hpp -- Composite signal scoring for GoldFlow entry quality
//
// Replaces the sequential hard-gate model with a weighted point system.
// Each condition contributes points toward an entry score.
// Entry is allowed when score >= SCORE_MIN_ENTRY (5 out of 16 max).
//
// HARD GATES (unchanged -- these are risk controls, not signal quality):
//   NY_CLOSE_NOISE, LONDON_OPEN_NOISE, BARS_NOT_READY, SPREAD_ANOMALY,
//   HIGH_IMPACT_WINDOW, COST_GATE, COMPRESSION_NO_VOL, RSI extreme
//   (non-momentum regime), COUNTER_TREND (non-momentum regime)
//
// SCORED CONDITIONS:
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
//   --- Cross-asset (RenTec #3) ---
//   +1      Macro regime aligned (RISK_OFF = gold LONG tailwind; RISK_ON = SHORT)
//   +1      DXY momentum aligned (falling DXY = gold LONG; rising = SHORT)
//   +1      SPX direction confirms gold signal (SPX down = safe haven LONG)
//   ----
//   16 max  SCORE_MIN_ENTRY = 5 (31% of max)
//
// score() takes cross-asset params as plain doubles -- caller reads from
// g_macroDetector and g_bars_sp. No direct dependency on those globals here,
// keeping the scorer testable and self-contained.
//
// Thread safety: all reads are from atomics (relaxed). score() is const and
// reads only atomics + passed-in values -- safe to call from any thread.
// =============================================================================

#include <atomic>
#include <cstdio>
#include <string>
#include "OHLCBarEngine.hpp"

// Minimum score threshold -- defined before ScoreResult so it can reference it.
static constexpr int OMEGA_SCORE_MIN_ENTRY = 5;

// =============================================================================
// ScoreResult -- breakdown of a single scoring evaluation
// =============================================================================
struct ScoreResult {
    int total      = 0;    // total points scored
    int max_points = 16;   // maximum achievable

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
    int pts_macro_regime = 0;  // +1: macro regime aligned with gold direction
    int pts_dxy_aligned  = 0;  // +1: DXY momentum aligned
    int pts_spx_aligned  = 0;  // +1: SPX direction confirms gold signal

    bool passes() const { return total >= OMEGA_SCORE_MIN_ENTRY; }
};

// =============================================================================
// OmegaSignalScorer
// =============================================================================
class OmegaSignalScorer {
public:
    static constexpr int    SCORE_MIN_ENTRY    = OMEGA_SCORE_MIN_ENTRY;
    static constexpr double L2_STRONG_LONG     = 0.75;
    static constexpr double L2_STRONG_SHORT    = 0.25;
    static constexpr double MICROPRICE_BIAS_THR = 0.003;  // ~0.3pts for XAUUSD
    static constexpr double VOL_DELTA_THR      = 0.10;
    static constexpr double RSI_HEALTHY_LOW    = 30.0;
    static constexpr double RSI_HEALTHY_HIGH   = 70.0;
    static constexpr double DXY_ALIGNED_THR    = 0.0010;  // 10bp DXY move
    static constexpr double SPX_RETURN_THR     = 0.0020;  // 20bp SPX move

    // Last computed score -- exposed for GUI / watchdog
    std::atomic<int>  last_score {0};
    std::atomic<bool> last_passed{false};

    // =========================================================================
    // score()
    // Parameters:
    //   ind          : gold M1 bar indicators
    //   is_long      : direction being evaluated
    //   l2_imbalance : gold L2 imbalance (0..1)
    //   microprice   : gold microprice bias (signed, pts)
    //   macro_regime : "RISK_ON" / "RISK_OFF" / "NEUTRAL"
    //   dxy_return   : fractional DXY return over window (e.g. 0.0015)
    //   spx_return   : fractional US500 return over window (e.g. -0.003)
    // =========================================================================
    ScoreResult score(const BarIndicators& ind,
                      bool               is_long,
                      double             l2_imbalance,
                      double             microprice,
                      const std::string& macro_regime,
                      double             dxy_return,
                      double             spx_return) const noexcept
    {
        ScoreResult r;

        // ?? +2: EMA trend aligned ????????????????????????????????????????????
        if (ind.m1_ema_live.load(std::memory_order_relaxed)) {
            const double e9  = ind.ema9 .load(std::memory_order_relaxed);
            const double e50 = ind.ema50.load(std::memory_order_relaxed);
            if (e9 > 0.0 && e50 > 0.0) {
                if (is_long ? (e9 > e50) : (e9 < e50)) r.pts_ema_trend = 2;
            }
        }

        // ?? +2: ADX trending (>= 25) ???????????????????????????????????????????
        if (ind.adx_trending.load(std::memory_order_relaxed)) r.pts_adx_trend = 2;

        // ?? +2: L2 imbalance strongly aligned ?????????????????????????????????????
        {
            const bool l2_long  = (l2_imbalance >= L2_STRONG_LONG);
            const bool l2_short = (l2_imbalance <= L2_STRONG_SHORT);
            if ((is_long && l2_long) || (!is_long && l2_short)) r.pts_l2_strong = 2;
        }

        // ?? +1: VWAP direction aligned ?????????????????????????????????????????
        {
            const int vwap_dir = ind.vwap_direction.load(std::memory_order_relaxed);
            if ((is_long && vwap_dir == +1) || (!is_long && vwap_dir == -1))
                r.pts_vwap_align = 1;
        }

        // ?? +1: Microprice bias aligned ????????????????????????????????????????
        {
            if ((is_long  && microprice >  MICROPRICE_BIAS_THR) ||
                (!is_long && microprice < -MICROPRICE_BIAS_THR))
                r.pts_microprice = 1;
        }

        // ?? +1: ATR expanding ?????????????????????????????????????????????????
        if (ind.atr_expanding.load(std::memory_order_relaxed)) r.pts_atr_expand = 1;

        // ?? +1: Vol delta aligned ????????????????????????????????????????????
        {
            const double vd = ind.vol_delta_ratio.load(std::memory_order_relaxed);
            if ((is_long && vd > VOL_DELTA_THR) || (!is_long && vd < -VOL_DELTA_THR))
                r.pts_vol_delta = 1;
        }

        // ?? +1: RSI in healthy zone (30-70) ????????????????????????????????????
        {
            const double rsi = ind.rsi14.load(std::memory_order_relaxed);
            if (rsi >= RSI_HEALTHY_LOW && rsi <= RSI_HEALTHY_HIGH) r.pts_rsi_healthy = 1;
        }

        // ?? +1: BBW squeeze releasing ?????????????????????????????????????????
        {
            const int    sq_bars = ind.bb_squeeze_bars.load(std::memory_order_relaxed);
            const double bb_w    = ind.bb_width    .load(std::memory_order_relaxed);
            const double bb_wmin = ind.bb_width_min.load(std::memory_order_relaxed);
            if ((sq_bars >= 1) && (bb_wmin > 0.0) && (bb_w > bb_wmin * 1.10))
                r.pts_bbw_release = 1;
        }

        // ?? +1: ADX strong (>= 40) ????????????????????????????????????????????
        if (ind.adx_strong.load(std::memory_order_relaxed)) r.pts_adx_strong = 1;

        // =====================================================================
        // Cross-asset conditions (RenTec #3)
        // =====================================================================

        // ?? +1: Macro regime aligned ?????????????????????????????????????????
        // RISK_OFF: flight-to-safety = gold LONG tailwind
        // RISK_ON:  risk appetite kills gold bid = gold SHORT tailwind
        {
            if ((is_long  && macro_regime == "RISK_OFF") ||
                (!is_long && macro_regime == "RISK_ON"))
                r.pts_macro_regime = 1;
        }

        // ?? +1: DXY momentum aligned ??????????????????????????????????????????
        // Gold/USD inverse correlation (~-0.7).
        // Falling DXY (negative return) = USD weak = gold LONG tailwind.
        // Rising DXY  (positive return) = USD strong = gold SHORT tailwind.
        {
            if ((is_long  && dxy_return < -DXY_ALIGNED_THR) ||
                (!is_long && dxy_return >  DXY_ALIGNED_THR))
                r.pts_dxy_aligned = 1;
        }

        // ?? +1: SPX direction confirms gold signal ????????????????????????????????????
        // Falling SPX = risk-off = safe-haven gold bid = LONG confirmation.
        // Rising SPX  = risk appetite = gold headwind = SHORT confirmation.
        // Soft bonus (not a block) -- gold can rally with equities on inflation.
        {
            if ((is_long  && spx_return < -SPX_RETURN_THR) ||
                (!is_long && spx_return >  SPX_RETURN_THR))
                r.pts_spx_aligned = 1;
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
                + r.pts_adx_strong
                + r.pts_macro_regime
                + r.pts_dxy_aligned
                + r.pts_spx_aligned;

        return r;
    }

    // =========================================================================
    // score_and_store()
    // =========================================================================
    ScoreResult score_and_store(const BarIndicators& ind,
                                bool               is_long,
                                double             l2_imbalance,
                                double             microprice,
                                const std::string& macro_regime,
                                double             dxy_return,
                                double             spx_return) noexcept
    {
        const ScoreResult r = score(ind, is_long, l2_imbalance, microprice,
                                    macro_regime, dxy_return, spx_return);
        last_score .store(r.total,    std::memory_order_relaxed);
        last_passed.store(r.passes(), std::memory_order_relaxed);
        return r;
    }

    // =========================================================================
    // log_score() -- single-line breakdown for grep-ability
    // =========================================================================
    void log_score(const ScoreResult& r, bool is_long) const noexcept {
        printf("[SCORER] %s score=%d/%d %s | "
               "ema=%d adx_tr=%d l2=%d vwap=%d mp=%d "
               "atr_exp=%d vd=%d rsi=%d bbw=%d adx_str=%d "
               "macro=%d dxy=%d spx=%d\n",
               is_long ? "LONG" : "SHORT",
               r.total, r.max_points,
               r.passes() ? "PASS" : "FAIL",
               r.pts_ema_trend, r.pts_adx_trend, r.pts_l2_strong,
               r.pts_vwap_align, r.pts_microprice,
               r.pts_atr_expand, r.pts_vol_delta, r.pts_rsi_healthy,
               r.pts_bbw_release, r.pts_adx_strong,
               r.pts_macro_regime, r.pts_dxy_aligned, r.pts_spx_aligned);
        fflush(stdout);
    }
};

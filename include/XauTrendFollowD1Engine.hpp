#pragma once
// =============================================================================
//  XauTrendFollowD1Engine.hpp -- XAU D1 trend-follow ensemble (S33e 2026-05-11)
// =============================================================================
//
//  PROVENANCE
//
//  Built 2026-05-11 from edge_hunt v3 Pass-1 results. D1 was untested
//  before this pass; it surfaced three positive trend-follow cells on
//  3 years of Dukascopy (2023-Q4 → 2025-Q3, ~570 daily bars). Realistic
//  bid/ask fill simulation, $0.06/RT cost subtracted, 0.01 lot.
//
//  Cells:
//      [A] Momentum lookback=20 sl2.0tp4.0   n=31  net=+$1134  $36.6/trade
//      [B] Keltner  K=2.0       sl2.0tp4.0   n=15  net=+$911   $60.7/trade
//      [C] ADX_Mom  mom=20 adx>25 sl2.0tp4.0 n=20  net=+$737   $36.9/trade
//
//  Yearly breakdown (Momentum lookback=20, the strongest):
//      2023 Q4 (3 mo):  n=10, net +$178
//      2024 (full yr):  n=11, net +$107
//      2025 Jan-Sep:    n=10, net +$643
//
//  CAVEATS — different beast from the 4h ensemble:
//      - Sample size is small per cell (15-31 trades over 30 months).
//      - 2/3 years positive (not 3/3 like 4h ensemble).
//      - Per-trade values are the HIGHEST in the project ($36-60), which
//        is the upside but also means single trades matter a lot.
//      - Very low cadence: combined ~2 trades/month across all three cells.
//
//  ARCHITECTURE
//
//  Internally aggregates D1 bars from the H4 stream that tick_gold.hpp
//  already publishes. Each H4 close updates the engine; the engine groups
//  by UTC date and emits D1 bars when the date advances. No new bar
//  aggregation needed in tick_gold.hpp -- just call on_h4_bar() alongside
//  the 4h engine.
//
//  SAFETY
//      - shadow_mode = true by default; promotion to live requires
//        operator authorisation AND a minimum 3-month shadow period.
//      - 0.01 lot per cell, max 3 concurrent positions.
//      - No protected file touched.
//      - DOES NOT share state with XauTrendFollow4hEngine; both can run
//        concurrently with their own positions.
//
//  USAGE
//
//      // globals.hpp:
//      static omega::XauTrendFollowD1Engine g_xau_tf_d1;
//
//      // engine_init.hpp:
//      g_xau_tf_d1.shadow_mode = kShadowDefault;
//      g_xau_tf_d1.enabled     = true;
//      g_xau_tf_d1.lot         = 0.01;
//      g_xau_tf_d1.max_spread  = 1.0;
//      g_xau_tf_d1.init();
//
//      // tick_gold.hpp (in the H4-close dispatch block, alongside g_xau_tf_4h):
//      omega::XauTfD1Bar tf_h4{};
//      tf_h4.bar_start_ms = s_bar_h4_ms;
//      tf_h4.open  = s_cur_h4.open;
//      tf_h4.high  = s_cur_h4.high;
//      tf_h4.low   = s_cur_h4.low;
//      tf_h4.close = s_cur_h4.close;
//      g_xau_tf_d1.on_h4_bar(tf_h4, bid, ask, now_ms_g, bracket_on_close);
//
//      // tick_gold.hpp every tick (alongside g_xau_tf_4h.on_tick):
//      g_xau_tf_d1.on_tick(bid, ask, now_ms_g, bracket_on_close);
//
//  S37 Phase H STAGE-TRAIL TOMBSTONE-BY-ANALOGY (2026-05-27b).
//  Multi-cell engine; cells use TPs at 3-6*ATR. Predicted NEGATIVE by analogy
//  to XauPullbackContH4 (trail -36% Sharpe at TP=5N). D1 timeframe makes the
//  trail-vs-multi-day-trend collision sharper -- a D1 winner that touches 2N
//  MFE on day 1 would be trail-cut at +0.5N before the rest of the multi-day
//  trend unfolds. Trail STAYS OFF and is NOT implemented to keep diff small.
// =============================================================================
//
//  S34 P1 FIXES (2026-05-12) -- close-path bug class from HANDOFF_S34.md §3.2,
//  §4.2. See XauTrendFollow4hEngine.hpp for full diagnosis. Bug numbers
//  below match the table in §3.2.
//
//    #1 tr.pnl never assigned in _close. Fix: pts_move signed by direction,
//       tr.pnl = pts_move * lot. tick_value_multiplier applied downstream.
//    #3 tr.engine was bare "XauTrendFollowD1" for all 3 cells. Fix:
//       tr.engine = "XauTrendFollowD1_" + cell.name.
//    #4 tr.side was BUY/SELL. Fix: LONG/SHORT.
//    #5 MFE/MAE never tracked. Fix: mfe/mae on XauTfD1Pos, updated per tick
//       in _manage_open with mid = (bid+ask)/2, propagated to TradeRecord.
//
//  Bug #2 (symbol key) NOT applicable per handoff §4.2 -- "XAUUSD" is
//  the correct sizing-table key. S34-B guards NOT carried over -- PROVE_IT
//  (90 seconds, 4 pt favourable) is nonsensical on a daily timeframe.
// =============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"
#include "GoldWaveTrend.hpp"   // S-2026-06-03 momentum-confirm gate (omega::gold_wt())
#include "GoldD1TrendState.hpp"  // D1 regime gate (added 2026-05-21)
#include "RegimeState.hpp"       // 2026-06-21: macro-hostile long-block (BearProtect coverage)
#include "OpenPositionRegistry.hpp"  // S-2026-06-03 PositionSnapshot persist/restore
#include "GoldTrendMimicLadder.hpp" // one-way mimic trigger (fire-and-forget on open)
#include <vector>
#include <cstdlib>

namespace omega {

// Input bar: H4 (we synthesise D1 internally by UTC date).
struct XauTfD1Bar {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
};

// Internal daily bar (synthesised from H4 input)
struct XauTfD1DailyBar {
    long long date_id = 0;          // YYYYMMDD as integer
    double    open    = 0.0;
    double    high    = 0.0;
    double    low     = 0.0;
    double    close   = 0.0;
    int       h4_count = 0;
};

struct XauTfD1Pos {
    bool        active             = false;
    bool        is_long            = false;
    double      entry_px           = 0.0;
    double      tp_px              = 0.0;
    double      sl_px              = 0.0;
    double      atr_at_entry       = 0.0;
    int64_t     entry_ts_ms        = 0;
    int         bars_held          = 0;
    int         cooldown_bars      = 0;
    // S34 P1 fix #5: per-position MFE/MAE in price units (>=0).
    double      mfe                = 0.0;
    double      mae                = 0.0;
    double      size_mult          = 1.0;   // S-2026-06-03: regression-slope sizing overlay
    std::string broker_position_id;
    std::string entry_clOrdId;
};

enum class XauTfD1Family { Momentum20, Keltner20, AdxMom20, Donchian5 };
struct XauTfD1CellConfig {
    XauTfD1Family family;
    double        sl_mult;
    double        tp_mult;
    const char*   name;
};

// Three validated D1 cells (3-year Duka, 2/3 years +ve each, biggest
// per-trade edges in the project).
// S33h 2026-05-11: D1 Keltner R:R upgrade from Pass-5 deep_dive v5.
// Switching TP from 4.0*ATR to 6.0*ATR (R:R 2:1 -> 3:1 given SL=2.0)
// lifts net from $911 to $1278 over 30 months (+$367, BE jumps to $106/RT).
static constexpr XauTfD1CellConfig kXauTfD1Cells[] = {
    { XauTfD1Family::Momentum20, 2.0, 4.0, "D1_Momentum_lb20_sl2.0tp4.0"        },
    { XauTfD1Family::Keltner20,  2.0, 6.0, "D1_Keltner_K2_sl2.0tp6.0_RR3to1"    },  // was 2.0, 4.0
    { XauTfD1Family::AdxMom20,   2.0, 4.0, "D1_ADX_Mom_adx25_sl2.0tp4.0"        },
    // S42 (2026-05-31): Donchian-N5 vol-target-style breakout, NO-TP runner.
    // From the S41 D1 deep test (backtest probe + XauTrendFollowD1Backtest):
    // voldon N5 / bull_LB30 / sl2.0 was the strongest + most robust D1 cell
    // (ROBUST 5/6 across lb{20,30,50}, g~+4800bps n=20). Entry: close > prior
    // 5-day Donchian high with a 30-day bull gate. Exit: close < prior 5-day
    // Donchian low (bar-close, handled in _finalise_day_and_evaluate) OR the
    // 2.0*ATR stop. tp_mult=20.0 == effectively-no-TP (same convention as the
    // 1h Donchian40 + 4h KeltnerEma50 cells). Bit 3 in cell mask; the engine
    // has no per-cell enable mask, so this cell runs whenever the engine is
    // enabled -- it is added deliberately as a 4th always-on D1 cell.
    { XauTfD1Family::Donchian5,  2.0, 20.0, "D1_Donchian_N5_lb30_sl2.0_S42"     },
};
static constexpr int kXauTfD1NumCells =
    static_cast<int>(sizeof(kXauTfD1Cells) / sizeof(kXauTfD1Cells[0]));

struct XauTrendFollowD1Engine {
public:
    bool   shadow_mode = true;
    bool   enabled     = false;
    // S88-followup (2026-05-27): ATR-percentile vol-band gate (see
    // XauThreeBar30m results: PF 1.55 -> 2.23 on 6mo PKL). Default OFF.
    bool   use_vol_band_gate = false;
    // S-2026-06-22 IMPULSE FILTER (fleet-audit): signal-day bar must thrust
    // >= min_impulse_atr*ATR14 (|close-open|). 0.5 improves BOTH regimes on the
    // D1 ensemble (bull 1.55->1.60, bear 1.38->1.52, maxDD -16%); 1.0 over-filters
    // (cuts Keltner/Donchian winners — rejected). 0 = OFF (byte-identical).
    // Activated to 0.5 on g_xau_tf_d1 in engine_init. Mirrors the 1h engine filter.
    double min_impulse_atr   = 0.0;
    double vol_band_low_pct  = 0.30;
    double vol_band_high_pct = 0.85;
    std::deque<double> atr_vol_window_;
    // S88-followup per-cell mask (default all-ones).
    uint32_t cell_vol_band_mask = 0xFFFFFFFF;
    double lot         = 0.01;
    // S-2026-06-03: regression-slope sizing overlay. Default OFF = byte-identical
    // behaviour. When ON: down-size to reg_size_floor when the 128-bar regression
    // slope of close is negative (breakout against the macro trend-fit).
    bool   reg_slope_size = true;   // S-2026-06-03: ENABLED (shadow engines -> live shadow validation of the prod-validated overlay)
    double reg_size_floor = 0.4;
    double max_spread  = 1.0;

    // S63 2026-05-14 (part W): VWR-pattern in-flight protection (full trio).
    //   Defaults to ALL ZERO -- S63 is OFF on production until per-instrument
    //   backtest evidence justifies enabling. The XauTrendFollow trio
    //   (2h/4h/D1) is in STATE B (hooks declared + mgmt-path implemented +
    //   deliberately zeroed at class default) until the planned Phase 3
    //   walk-forward sweep (scripts/xtf_*_wf_t1.py + the dedicated
    //   XauTrendFollowBacktest harness) produces baseline-vs-tuned evidence.
    //
    //   D1 CAVEAT: this engine has very low cadence (~2 trades/month
    //   combined across 3 cells, n=15-31 per cell over 30 months). S63
    //   evaluation will have wide error bars. A favourable S63 cell on
    //   D1 should be triple-checked against the small-n risk before
    //   activating, and the pass criterion may want to be tightened
    //   (e.g. min trade count per window) when the sweep runs.
    //
    //   LOSS_CUT_PCT  -- cut when adverse >= entry*pct/100. Phase 2 (cold-loss).
    //   BE_ARM_PCT    -- mfe % of entry that arms BE ratchet. Phase 1 (giveback).
    //   BE_BUFFER_PCT -- BE_CUT triggers when move <= entry*pct/100 after arm.
    //   Override via the backtest harness CLI for the sweep; set _PCT = 0.0
    //   to disable a phase entirely.
    // ADVERSE-PROTECTION: (backtested verdicts; header tag added S-2026-07-17u
    //   when the ENTRY_RE widening exposed this file as un-tagged.)
    //   Verdict = COLD-LOSS CUT + WIDE-ARM BE RATCHET (both set in engine_init):
    //   * LOSS_CUT_PCT=1.0 (S-2026-06-17, losscut_batch_b.py): net flat,
    //     maxDD -68% (-341 -> -110), worst -174 -> -53.
    //   * BE_ARM_PCT=5.0 / BE_BUFFER_PCT=1.0 (S-2026-06-25 re-sweep,
    //     xau_tf_d1_bearm_sweep, real engine, 2yr H4, bull+bear, WF): PF1.58
    //     +$4160 maxDD $1553, both-WF-halves+; arm1/2 tight-lock GUTS the edge
    //     (PF<1) -- wide-arm mandatory, plateau arm3-5.
    double LOSS_CUT_PCT  = 0.0;
    double BE_ARM_PCT    = 0.0;
    double BE_BUFFER_PCT = 0.0;

    std::array<XauTfD1Pos, kXauTfD1NumCells> pos{};

    // Daily bar history (last 40 days; enough for Momentum20 + ADX + Keltner)
    static constexpr int kDailyHistory = 40;
    std::deque<XauTfD1DailyBar> daily_;
    XauTfD1DailyBar              cur_day_{};
    bool                         cur_day_open_ = false;

    // Rolling indicators on daily bars
    static constexpr int kAtrPeriod = 14;
    double atr14_ = 0.0;
    int    atr_warmup_count_ = 0;

    static constexpr int kEmaPeriod = 20;
    double ema20_ = 0.0;
    bool   ema_initialised_ = false;

    static constexpr int kAdxPeriod = 14;
    double adx14_ = 0.0;
    double adx_atr_sum_ = 0.0;
    double adx_pdm_sum_ = 0.0;
    double adx_mdm_sum_ = 0.0;
    double adx_dx_sum_  = 0.0;
    int    adx_warmup_count_ = 0;
    int    adx_dx_count_     = 0;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    // S102: warmup entry guard — see XauTrendFollow2hEngine.hpp for root cause.
    bool warmup_active_ = false;

    void init() noexcept {
        daily_.clear();
        cur_day_ = {};
        cur_day_open_ = false;
        atr14_ = 0.0;
        atr_warmup_count_ = 0;
        ema20_ = 0.0;
        ema_initialised_ = false;
        adx14_ = 0.0;
        adx_atr_sum_ = adx_pdm_sum_ = adx_mdm_sum_ = adx_dx_sum_ = 0.0;
        adx_warmup_count_ = 0;
        adx_dx_count_ = 0;
        warmup_active_ = false;
        for (auto& p : pos) p = {};
    }

    bool any_open() const noexcept {
        for (const auto& p : pos) if (p.active) return true;
        return false;
    }
    int open_count() const noexcept {
        int n = 0; for (const auto& p : pos) if (p.active) ++n; return n;
    }

    // ============================================================
    //  S-2026-06-03 open-position PERSISTENCE (save/restore).
    //  One snapshot per ACTIVE cell, tagged "<base_engine>#<cellIdx>".
    // ============================================================
    void persist_save_all(const char* base_engine, const char* sym,
                          std::vector<omega::PositionSnapshot>& out) const {
        for (int ci = 0; ci < kXauTfD1NumCells; ++ci) {
            const auto& p = pos[ci];
            if (!p.active) continue;
            omega::PositionSnapshot ps;
            ps.engine   = std::string(base_engine) + "#" + std::to_string(ci);
            ps.symbol   = sym;
            ps.side     = p.is_long ? "LONG" : "SHORT";
            ps.size     = lot;
            ps.entry    = p.entry_px;
            ps.sl       = p.sl_px;
            ps.tp       = p.tp_px;
            ps.entry_ts = p.entry_ts_ms / 1000;
            out.push_back(ps);
        }
    }

    bool persist_restore(const omega::PositionSnapshot& ps) {
        const auto hash = ps.engine.rfind('#');
        if (hash == std::string::npos) return false;
        const int ci = std::atoi(ps.engine.c_str() + hash + 1);
        if (ci < 0 || ci >= kXauTfD1NumCells) return false;
        auto& p = pos[ci];
        if (p.active) return true;   // already holding -- don't double
        p.active        = true;
        p.is_long       = (ps.side == "LONG");
        p.entry_px      = ps.entry;
        p.sl_px         = ps.sl;
        p.tp_px         = ps.tp;
        p.entry_ts_ms   = ps.entry_ts * 1000;
        p.bars_held     = 0;
        p.mfe           = 0.0;
        p.mae           = 0.0;
        p.size_mult     = 1.0;
        // one-way mimic notify — RESTORE path (S-2026-07-18 bounded catch-up): the old
        // unconditional on_trend_open re-fire here spawned duplicate stale-trigger legs
        // on every restart while holding (no age bound, original entry px). The registry
        // restore route spawns ONLY under the certified catch-up conditions.
        omega::gold_trend_mimic().on_trend_restore("XauTfD1", p.is_long ? 1 : -1, p.entry_px, ps.entry_ts);
        return true;
    }

    // ============================================================
    //  on_h4_bar -- aggregates 6 H4 bars into 1 D1 bar; fires on
    //               D1 close only.
    // ============================================================
    void on_h4_bar(const XauTfD1Bar& bar,
                   double bid, double ask,
                   int64_t now_ms,
                   OnCloseFn on_close) noexcept {
        if (!enabled) return;
        // SPECIFIC FEED: drive this engine's mimic book on the H4 bar (the cadence the XauTfD1
        // mimic was backtested on -- clip_path d1 emits the H4 path). One-way; no-op if unregistered.
        omega::gold_trend_mimic().on_bar("XauTfD1", bar.high, bar.low, bar.close, now_ms / 1000);
        long long date_id = _date_id_from_ms(bar.bar_start_ms);

        if (!cur_day_open_) {
            cur_day_ = {};
            cur_day_.date_id = date_id;
            cur_day_.open = bar.open;
            cur_day_.high = bar.high;
            cur_day_.low  = bar.low;
            cur_day_.close = bar.close;
            cur_day_.h4_count = 1;
            cur_day_open_ = true;
            return;
        }

        if (date_id != cur_day_.date_id) {
            // Day rollover: finalise previous day, then start new day with
            // the just-received H4 as the first bar of the new day.
            _finalise_day_and_evaluate(bid, ask, now_ms, on_close);

            cur_day_ = {};
            cur_day_.date_id = date_id;
            cur_day_.open = bar.open;
            cur_day_.high = bar.high;
            cur_day_.low  = bar.low;
            cur_day_.close = bar.close;
            cur_day_.h4_count = 1;
            cur_day_open_ = true;
            return;
        }

        // Same day -- extend current day's OHLC.
        if (bar.high > cur_day_.high) cur_day_.high = bar.high;
        if (bar.low  < cur_day_.low)  cur_day_.low  = bar.low;
        cur_day_.close = bar.close;
        ++cur_day_.h4_count;
    }

    // ============================================================
    //  on_tick -- per-tick position management for any open cell
    // ============================================================
    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseFn on_close) noexcept {
        if (!enabled) return;
        for (int ci = 0; ci < kXauTfD1NumCells; ++ci) {
            if (!pos[ci].active) continue;
            _manage_open(ci, bid, ask, now_ms, on_close);
        }
    }

    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseFn on_close, const char* reason) noexcept {
        for (int ci = 0; ci < kXauTfD1NumCells; ++ci) {
            if (!pos[ci].active) continue;
            double xp = pos[ci].is_long ? bid : ask;
            _close(ci, xp, reason ? reason : "FORCE_CLOSE", now_ms, on_close);
        }
    }

private:
    static long long _date_id_from_ms(int64_t bar_start_ms) noexcept {
        std::time_t tt = (std::time_t)(bar_start_ms / 1000);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &tt);
#else
        gmtime_r(&tt, &tm);
#endif
        return (long long)(tm.tm_year + 1900) * 10000LL
             + (long long)(tm.tm_mon + 1)     * 100LL
             + (long long)tm.tm_mday;
    }

    // Day closed: push to history, update indicators, then evaluate cells.
    void _finalise_day_and_evaluate(double bid, double ask,
                                    int64_t now_ms,
                                    OnCloseFn on_close) noexcept {
        if (!cur_day_open_) return;
        daily_.push_back(cur_day_);
        while ((int)daily_.size() > kDailyHistory) daily_.pop_front();

        _update_atr14();
        _update_ema20();
        _update_adx14();

        for (auto& p : pos) {
            if (p.cooldown_bars > 0) --p.cooldown_bars;
            if (p.active) ++p.bars_held;
        }

        if ((int)daily_.size() < 22) return;   // need 20-bar momentum + warmup
        if (atr14_ <= 0.0) return;
        if (ask - bid > max_spread) return;

        // S88-followup: rolling ATR window for vol-band gate.
        if (use_vol_band_gate && atr14_ > 0.0) {
            atr_vol_window_.push_back(atr14_);
            if ((int)atr_vol_window_.size() > 200) atr_vol_window_.pop_front();
        }

        // S42: Donchian5 no-TP runner exits on a close back below the prior
        // 5-day Donchian low (bar-close exit), mirroring the 1h Donchian40
        // cell. Runs BEFORE new-entry decisions. The 2.0*ATR stop still fires
        // intra-bar via _manage_open. Only this cell (family Donchian5) uses
        // the bar-close low exit; all other cells exit purely on SL/TP.
        for (int ci = 0; ci < kXauTfD1NumCells; ++ci) {
            if (kXauTfD1Cells[ci].family != XauTfD1Family::Donchian5) continue;
            if (!pos[ci].active) continue;
            const int sz = (int)daily_.size();
            if (sz < 7) continue;
            double lo = 1e18;
            for (int k = sz - 1 - 5; k <= sz - 2; ++k) if (daily_[k].low < lo) lo = daily_[k].low;
            if (daily_[sz - 1].close < lo) {
                _close(ci, daily_[sz - 1].close, "DONCH_LOW_EXIT", now_ms, on_close);
            }
        }

        for (int ci = 0; ci < kXauTfD1NumCells; ++ci) {
            if (pos[ci].active) continue;
            if (pos[ci].cooldown_bars > 0) continue;
            int side = _evaluate_signal(ci);
            if (side == 0) continue;
            // S-2026-06-22 IMPULSE FILTER (fleet-audit): the signal-day bar must
            // thrust >= min_impulse_atr*ATR14 (|close-open|). 0=OFF. Applies to all
            // D1 cells (all are breakout/momentum; no dip-buy family here).
            if (min_impulse_atr > 0.0 && atr14_ > 0.0 && !daily_.empty()) {
                const auto& sd = daily_.back();
                if (std::fabs(sd.close - sd.open) < min_impulse_atr * atr14_) continue;
            }
            // S88-followup vol-band gate (per-cell mask)
            if (use_vol_band_gate && (cell_vol_band_mask & (1u << ci))
                && (int)atr_vol_window_.size() >= 200) {
                int below = 0;
                const int n = (int)atr_vol_window_.size();
                for (int i = 0; i < n; ++i) if (atr_vol_window_[i] < atr14_) ++below;
                const double pct = static_cast<double>(below) / n;
                if (pct < vol_band_low_pct || pct > vol_band_high_pct) {
                    continue;  // VOL_BAND_OUT
                }
            }
            _fire_entry(ci, side, bid, ask, now_ms);
        }
        (void)on_close;
    }

    void _update_atr14() noexcept {
        if (daily_.size() < 2) { atr14_ = 0.0; return; }
        const auto& cur  = daily_.back();
        const auto& prev = daily_[daily_.size() - 2];
        double tr = std::max(cur.high - cur.low,
                             std::max(std::abs(cur.high - prev.close),
                                      std::abs(cur.low  - prev.close)));
        if (atr_warmup_count_ < kAtrPeriod) {
            atr14_ = (atr14_ * atr_warmup_count_ + tr) / (atr_warmup_count_ + 1);
            ++atr_warmup_count_;
        } else {
            atr14_ = (atr14_ * (kAtrPeriod - 1) + tr) / kAtrPeriod;
        }
    }

    void _update_ema20() noexcept {
        if (daily_.empty()) return;
        double c = daily_.back().close;
        if (!ema_initialised_) { ema20_ = c; ema_initialised_ = true; }
        else {
            double a = 2.0 / (kEmaPeriod + 1);
            ema20_ = a * c + (1.0 - a) * ema20_;
        }
    }

    void _update_adx14() noexcept {
        if (daily_.size() < 2) return;
        const auto& cur  = daily_.back();
        const auto& prev = daily_[daily_.size() - 2];
        double up = cur.high - prev.high;
        double dn = prev.low - cur.low;
        double pdm = (up > dn && up > 0) ? up : 0.0;
        double mdm = (dn > up && dn > 0) ? dn : 0.0;
        double tr  = std::max(cur.high - cur.low,
                              std::max(std::abs(cur.high - prev.close),
                                       std::abs(cur.low  - prev.close)));
        if (adx_warmup_count_ < kAdxPeriod) {
            adx_atr_sum_ += tr;
            adx_pdm_sum_ += pdm;
            adx_mdm_sum_ += mdm;
            ++adx_warmup_count_;
        } else {
            adx_atr_sum_ = adx_atr_sum_ - adx_atr_sum_ / kAdxPeriod + tr;
            adx_pdm_sum_ = adx_pdm_sum_ - adx_pdm_sum_ / kAdxPeriod + pdm;
            adx_mdm_sum_ = adx_mdm_sum_ - adx_mdm_sum_ / kAdxPeriod + mdm;
        }
        if (adx_warmup_count_ >= kAdxPeriod && adx_atr_sum_ > 1e-12) {
            double pdi = 100.0 * adx_pdm_sum_ / adx_atr_sum_;
            double mdi = 100.0 * adx_mdm_sum_ / adx_atr_sum_;
            double sum = pdi + mdi;
            double dx  = (sum > 1e-12) ? 100.0 * std::abs(pdi - mdi) / sum : 0.0;
            if (adx_dx_count_ < kAdxPeriod) {
                adx_dx_sum_ += dx;
                ++adx_dx_count_;
                if (adx_dx_count_ == kAdxPeriod) adx14_ = adx_dx_sum_ / kAdxPeriod;
            } else {
                adx14_ = (adx14_ * (kAdxPeriod - 1) + dx) / kAdxPeriod;
            }
        }
    }

    int _evaluate_signal(int ci) const noexcept {
        switch (kXauTfD1Cells[ci].family) {
            case XauTfD1Family::Momentum20: return _sig_momentum(20);
            case XauTfD1Family::Keltner20:  return _sig_keltner();
            case XauTfD1Family::AdxMom20:   return _sig_adx_momentum(25.0, 20);
            case XauTfD1Family::Donchian5:  return _sig_donchian5();
        }
        return 0;
    }

    // S42: long-only Donchian-N5 breakout with a 30-day bull gate. EXACT port
    // of the validated D1 voldon edge. Returns +1 when close > prior 5-day
    // Donchian high AND close > close 30 days ago. Exit is the close<5-day-low
    // bar-close path in _finalise_day_and_evaluate, or the 2.0*ATR stop.
    int _sig_donchian5() const noexcept {
        constexpr int N = 5;
        constexpr int BULL_LB = 30;
        const int sz = (int)daily_.size();
        if (sz < BULL_LB + 2) return 0;
        if (!(daily_[sz - 1].close > daily_[sz - 1 - BULL_LB].close)) return 0;  // bull gate
        double hi = -1e18;
        for (int k = sz - 1 - N; k <= sz - 2; ++k) if (daily_[k].high > hi) hi = daily_[k].high;
        if (daily_[sz - 1].close > hi) return +1;
        return 0;
    }

    int _sig_momentum(int N) const noexcept {
        if ((int)daily_.size() < N + 2) return 0;
        const int last = (int)daily_.size() - 1;
        double cur  = daily_[last].close;
        double prev = daily_[last - N].close;
        if (cur > prev * 1.001) return +1;
        if (cur < prev * 0.999) return -1;
        return 0;
    }

    int _sig_keltner() const noexcept {
        if (!ema_initialised_ || atr14_ <= 0.0) return 0;
        if (daily_.size() < 22) return 0;
        const auto& cur = daily_.back();
        double up = ema20_ + 2.0 * atr14_;
        double lo = ema20_ - 2.0 * atr14_;
        if (cur.close > up) return +1;
        if (cur.close < lo) return -1;
        return 0;
    }

    int _sig_adx_momentum(double adx_thr, int N) const noexcept {
        if (adx_dx_count_ < kAdxPeriod) return 0;
        if (adx14_ < adx_thr) return 0;
        if ((int)daily_.size() < N + 2) return 0;
        const int last = (int)daily_.size() - 1;
        double cur  = daily_[last].close;
        double prev = daily_[last - N].close;
        if (cur > prev * 1.001) return +1;
        if (cur < prev * 0.999) return -1;
        return 0;
    }

    void _fire_entry(int ci, int side, double bid, double ask, int64_t now_ms) noexcept {
        // S102: block entries during warmup — warmup primes indicators only.
        if (warmup_active_) return;
        // 2026-05-21: D1 EMA200 regime gate (chokepoint for all D1 cells).
        if (side > 0 && !omega::gold_d1_trend().long_allowed())  return;
        if (side < 0 && !omega::gold_d1_trend().short_allowed()) return;
        // 2026-06-21: macro-hostile long-block (BearProtect coverage; sibling 1h/4h
        // already wired). Fail-safe false when MacroGoldGate feed missing/stale.
        if (side > 0 && omega::gold_regime().long_blocked()) return;
        const auto& cfg = kXauTfD1Cells[ci];
        double entry = (side > 0) ? ask : bid;
        if (entry <= 0.0 || atr14_ <= 0.0) return;
        double sl_dist = cfg.sl_mult * atr14_;
        double tp_dist = cfg.tp_mult * atr14_;
        // 2026-05-12 cost gate -- see outputs/PLAN_A_B_REPORT.md
        {
            const double spread_pts = ask - bid;
            if (!ExecutionCostGuard::is_viable(
                    "XAUUSD", spread_pts, tp_dist, lot, 1.5))
            {
                return;
            }
        }

        // Momentum-confirm gate (S-2026-06-03): gold-validated WaveTrend filter.
        if (omega::gold_wt().gate_enabled && !omega::gold_wt().confirms(side > 0)) {
            omega::gold_wt().record_skip();
            printf("[GOLD-MOMGATE] XauTFD1 cell=%d SKIP %s (no momentum confirm) "
                   "wt1=%.1f regime_up=%d bars=%ld\n", ci, side > 0 ? "long" : "short",
                   omega::gold_wt().wt1(), (int)omega::gold_wt().regime_up(),
                   omega::gold_wt().bars_seen());
            fflush(stdout);
            return;
        }
        if (omega::gold_wt().gate_enabled) omega::gold_wt().record_pass();

        auto& p = pos[ci];
        p.active        = true;
        p.is_long       = (side > 0);
        p.entry_px      = entry;
        p.sl_px         = (side > 0) ? entry - sl_dist : entry + sl_dist;
        p.tp_px         = (side > 0) ? entry + tp_dist : entry - tp_dist;
        p.atr_at_entry  = atr14_;
        p.entry_ts_ms   = now_ms;
        p.bars_held     = 0;
        p.cooldown_bars = 0;
        // S34 P1 fix #5: reset MFE/MAE per new entry.
        p.mfe           = 0.0;
        p.mae           = 0.0;
        // S-2026-06-03: regression-slope sizing overlay (default OFF). Down-size
        // when the 128-bar regression slope of close is negative. Bar deque here
        // is daily_ (XauTfD1DailyBar), not bars_.
        p.size_mult = 1.0;
        if (reg_slope_size) {
            const int W = 128;
            if ((int)daily_.size() >= W) {
                const int s0 = (int)daily_.size() - W;
                double Sx=0, Sy=0, Sxx=0, Sxy=0;
                for (int k=0; k<W; ++k) { double x=k, y=daily_[s0+k].close; Sx+=x; Sy+=y; Sxx+=x*x; Sxy+=x*y; }
                const double den = (double)W*Sxx - Sx*Sx;
                const double slope = (den != 0.0) ? ((double)W*Sxy - Sx*Sy)/den : 0.0;
                if (slope < 0.0) p.size_mult = reg_size_floor;
            }
        }
        p.broker_position_id.clear();
        p.entry_clOrdId.clear();
        // GoldTrendMimicLadder one-way notify (fire-and-forget; never reads/touches this
        // position). S-2026-07-17q GAP FIX (same as XauTf4h): since 11e2b0fe the only fire
        // sat in persist_restore — live opens never spawned XauTfD1 mimic legs. Book was
        // certified on the FULL entry stream; this restores that contract. Pre-arm seed
        // opens can't fire (enabled=false during warmup + registry armed_ latch).
        omega::gold_trend_mimic().on_trend_open("XauTfD1", p.is_long ? 1 : -1, p.entry_px, now_ms / 1000);
    }

    void _manage_open(int ci, double bid, double ask, int64_t now_ms,
                      OnCloseFn on_close) noexcept {
        auto& p = pos[ci];

        // S34 P1 fix #5: update MFE/MAE on every tick using mid price.
        const double mid = (bid + ask) * 0.5;
        const double favourable = p.is_long ? (mid - p.entry_px)
                                            : (p.entry_px - mid);
        if (favourable > p.mfe) p.mfe = favourable;
        if (-favourable > p.mae) p.mae = -favourable;

        // S63 2026-05-14 (part W): VWR-pattern in-flight protection. Runs
        //   BEFORE the SL/TP exit check so cold-loss/giveback cuts take
        //   priority. Mirrors IndexMacroCrashEngine pattern at
        //   IndexFlowEngine.hpp:1123-1150. Both phases gated on their _PCT
        //   field being > 0.0 so defaulting all three to 0.0 (state B) is
        //   a structural no-op.
        // Phase 1: BE_RATCHET -- after mfe crosses arm threshold, exit at BE
        //          if current move falls back to within buffer.
        if (BE_ARM_PCT > 0.0 && BE_BUFFER_PCT >= 0.0 && p.entry_px > 0.0) {
            const double arm_pts    = p.entry_px * BE_ARM_PCT    / 100.0;
            const double buffer_pts = p.entry_px * BE_BUFFER_PCT / 100.0;
            if (p.mfe >= arm_pts && favourable <= buffer_pts) {
                const double exit_px_be = p.is_long ? bid : ask;
                _close(ci, exit_px_be, "BE_CUT", now_ms, on_close);
                return;
            }
        }
        // Phase 2: LOSS_CUT -- cold-loss cut for trades that go straight
        //          adverse without ever reaching the ATR-based SL.
        if (LOSS_CUT_PCT > 0.0 && p.entry_px > 0.0) {
            const double adverse       = -favourable;
            const double loss_cut_dist = p.entry_px * LOSS_CUT_PCT / 100.0;
            if (adverse >= loss_cut_dist) {
                const double exit_px_lc = p.is_long ? bid : ask;
                _close(ci, exit_px_lc, "LOSS_CUT", now_ms, on_close);
                return;
            }
        }

        bool hit_tp = false, hit_sl = false;
        double xp = 0;
        if (p.is_long) {
            if (bid <= p.sl_px) { xp = p.sl_px; hit_sl = true; }
            else if (bid >= p.tp_px) { xp = p.tp_px; hit_tp = true; }
        } else {
            if (ask >= p.sl_px) { xp = p.sl_px; hit_sl = true; }
            else if (ask <= p.tp_px) { xp = p.tp_px; hit_tp = true; }
        }
        if (!hit_tp && !hit_sl) return;
        _close(ci, xp, hit_tp ? "TP_HIT" : "SL_HIT", now_ms, on_close);
    }

    void _close(int ci, double exit_px, const char* reason,
                int64_t now_ms, OnCloseFn on_close) noexcept {
        auto& p = pos[ci];
        if (!p.active) return;

        // S34 P1 fix #1: pts_move signed so profit is +ve.
        const double pts_move = p.is_long ? (exit_px - p.entry_px)
                                          : (p.entry_px - exit_px);

        omega::TradeRecord tr;
        tr.symbol     = "XAUUSD";
        // S34 P1 fix #3: per-cell engine string.
        tr.engine     = std::string("XauTrendFollowD1_") + kXauTfD1Cells[ci].name;
        // S34 P1 fix #4: LONG/SHORT, not BUY/SELL.
        tr.side       = p.is_long ? "LONG" : "SHORT";
        tr.entryPrice = p.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = p.tp_px;
        tr.sl         = p.sl_px;
        tr.size       = lot * p.size_mult;   // S-2026-06-03: regression-slope sizing overlay
        tr.entryTs    = p.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = kXauTfD1Cells[ci].name;
        tr.shadow     = shadow_mode;
        // S34 P1 fix #1: gross pnl in price-points*lot; USD via tick_mult.
        tr.pnl        = pts_move * lot * p.size_mult;   // S-2026-06-03: regression-slope sizing overlay
        // S34 P1 fix #5: propagate MFE/MAE.
        tr.mfe        = p.mfe;
        tr.mae        = p.mae;

        if (on_close) on_close(tr);

        p.active        = false;
        p.broker_position_id.clear();
        p.entry_clOrdId.clear();
        p.cooldown_bars = 1;
    }

public:
    // S101: warmup from pre-built H4 bar CSV. Engine synthesizes D1 bars
    //   internally from H4 input so we feed H4 bars, same as live path.
    std::string warmup_csv_path;

    int warmup_from_csv(const std::string& path) noexcept {
        if (!enabled) { printf("[XauTFD1-WARMUP] skipped -- disabled\n"); fflush(stdout); return 0; }
        if (path.empty()) { printf("[XauTFD1-WARMUP] skipped -- no path (cold start)\n"); fflush(stdout); return 0; }
        std::ifstream f(path);
        if (!f.is_open()) { printf("[XauTFD1-WARMUP] FAIL -- cannot open '%s'\n", path.c_str()); fflush(stdout); return 0; }
        warmup_active_ = true;  // S102: block entries during indicator priming

        int fed = 0;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#' || line[0] == 'b') continue;
            char* p1; long long ms = std::strtoll(line.c_str(), &p1, 10);
            if (!p1 || *p1 != ',') continue;
            char* p2; double o = std::strtod(p1+1, &p2); if (!p2 || *p2 != ',') continue;
            char* p3; double h = std::strtod(p2+1, &p3); if (!p3 || *p3 != ',') continue;
            char* p4; double l = std::strtod(p3+1, &p4); if (!p4 || *p4 != ',') continue;
            char* p5; double c = std::strtod(p4+1, &p5);
            if (!std::isfinite(o) || !std::isfinite(h) || !std::isfinite(l) || !std::isfinite(c)) continue;

            XauTfD1Bar bar; bar.bar_start_ms = ms; bar.open = o; bar.high = h; bar.low = l; bar.close = c;
            on_h4_bar(bar, c, c, ms + 14400LL*1000, OnCloseFn{});
            ++fed;
        }
        warmup_active_ = false;  // S102: live entries now permitted
        printf("[XauTFD1-WARMUP] fed=%d H4 bars, atr=%.4f daily_size=%d path='%s'\n",
               fed, atr14_, (int)daily_.size(), path.c_str());
        fflush(stdout);
        return fed;
    }
};

} // namespace omega

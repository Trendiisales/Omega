#pragma once
// =============================================================================
//  XauTrendFollow4hEngine.hpp -- XAU 4h trend-follow ensemble (S33d 2026-05-11)
// =============================================================================
//
//  PROVENANCE
//
//  Built 2026-05-11 from edge_hunt.cpp + top_cells_monthly.cpp results on
//  the full corpus (3-year Dukascopy 2023-09-27 -> 2025-09-26 + 1-month
//  XAU L2 2026-04-09 -> 2026-05-08). Realistic bid/ask fill simulation,
//  $0.06/RT cost subtracted, 0.01 lot.
//
//  Cells (each independently positive in 3/3 Dukascopy years):
//
//      [A] Donchian N=20 sl1.5tp3.0      n=134  WR=47%  net=+$1332  $9.94/trade
//                                         20/26 months +ve, 2-mo max streak
//      [B] InsideBar    sl2.0tp4.0       n=138  WR=44%  net=+$1124  $8.15/trade
//                                         19/26 months +ve, 1-mo max streak  ★
//      [C] ER0.20 mom=20 sl1.5tp3.0      n=214  WR=40%  net=+$840   $3.92/trade
//                                         19/26 months +ve, 2-mo max streak
//
//  Three uncorrelated signal mechanics, all positive on the same regime
//  (XAU 4h trend-follow). Combined expected ~25-30 trades/month at the
//  full ensemble across all three cells. Each cell holds at most one
//  position; three max concurrent positions across the ensemble.
//
//  SAFETY
//
//      - shadow_mode = true by default; promotion to live requires explicit
//        operator authorisation in engine_init.hpp + a 2-week+ shadow
//        period matching backtest expectancy within 50%.
//      - DOES NOT touch ANY protected core engine file.
//      - 0.01 lot cap per cell; max 3 concurrent positions.
//      - Spread cap = 1.0 USD; engines refuse new entries when spread
//        exceeds this (avoids fill quality issues at session boundaries).
//      - Cooldown per cell = 1 bar (4h) after each trade closes; this
//        matches the backtest harness exactly.
//      - All entries use the broker-aware fill side: long on ask, short
//        on bid (matches the realistic-fill backtest that produced the
//        edges above).
//
//  USAGE
//
//      // globals.hpp:
//      static omega::XauTrendFollow4hEngine g_xau_tf_4h;
//
//      // engine_init.hpp (after g_donchian init):
//      g_xau_tf_4h.shadow_mode = kShadowDefault;
//      g_xau_tf_4h.enabled     = true;
//      g_xau_tf_4h.lot         = 0.01;
//      g_xau_tf_4h.max_spread  = 1.0;
//      g_xau_tf_4h.init();
//
//      // tick_gold.hpp inside H4-close branch (after g_minimal_h4_gold.on_h4_bar):
//      omega::XauTfBar tf_h4{};
//      tf_h4.bar_start_ms = s_bar_h4_ms;
//      tf_h4.open  = s_cur_h4.open;
//      tf_h4.high  = s_cur_h4.high;
//      tf_h4.low   = s_cur_h4.low;
//      tf_h4.close = s_cur_h4.close;
//      g_xau_tf_4h.on_h4_bar(tf_h4, bid, ask,
//          g_bars_gold.h4.ind.atr14.load(std::memory_order_relaxed),
//          now_ms_g, bracket_on_close);
//
//      // tick_gold.hpp on every tick (alongside g_donchian.on_tick):
//      g_xau_tf_4h.on_tick(bid, ask, now_ms_g, bracket_on_close);
//
//  S37 Phase H STAGE-TRAIL TOMBSTONE-BY-ANALOGY for cells [A]/[B]/[C]/[D]/[E]/[F]
//  (Donchian20/InsideBar/ER0.20/Keltner20/AdxMom20/RangeExpand) (2026-05-27b).
//  NOT empirically tested. Predicted NEGATIVE by analogy to XauPullbackContH4
//  (trail -36% Sharpe at TP=5N). These cells have TP at 3-6*ATR; stage1 arm
//  at 2*ATR catches winners at +0.5N. Trail STAYS OFF for default cells.
//
//  S37-H QUEUED EMPIRICAL TEST for cell [G] EmaCross8_21 (bit 6, default off).
//  This cell has tp_mult=20.0 = effectively-no-TP. Trail MAY help here
//  because there is no fixed TP to lose. Test deferred: cell is OFF in prod
//  by default (cell_enable_mask=0x3F excludes bit 6); when operator activates
//  bit 6, route an empirical baseline-vs-trail comparison through the
//  XauTrendFollowBacktest harness before shipping.
// =============================================================================
//
//  S34 P1 FIXES (2026-05-12) -- close-path bug class from HANDOFF_S34.md §3.2,
//  §4.2. Same pattern as the UstecTrendFollow5mEngine fixes shipped in
//  commit b1932d2. Bug numbers below match the table in §3.2.
//
//    #1 tr.pnl was never assigned in _close(); the trailing comment claimed
//       handle_closed_trade would compute it, but it does not -- it only
//       multiplies tr.pnl by tick_value_multiplier(symbol). Result: every
//       closed trade shipped PnL=$0 to the ledger. Fix: compute
//       pts_move = (long ? exit-entry : entry-exit), assign
//       tr.pnl = pts_move * lot. Matches BracketEngine.hpp:1216-1218.
//
//    #3 tr.engine was the bare string "XauTrendFollow4h" for all six cells;
//       ledger queries grouping by engine could not distinguish cells. Fix:
//       tr.engine = "XauTrendFollow4h_" + cell.name. (Bug #2 from §3.2 --
//       wrong symbol key -- is NOT present per handoff §4.2: "XAUUSD" is
//       the correct sizing-table key for these engines.)
//
//    #4 tr.side was "BUY"/"SELL"; ledger convention is "LONG"/"SHORT".
//
//    #5 MFE/MAE never tracked; tr.mfe and tr.mae shipped as 0. Fix: add
//       mfe/mae fields to XauTfPos, update each tick in _manage_open using
//       mid = (bid+ask)/2, propagate to TradeRecord in _close.
//
//  Bug #2 (symbol key) deliberately NOT changed -- "XAUUSD" matches the
//  existing sizing table per handoff §4.2 which omits #2 from the
//  XAU-engine bug list.
//
//  S34-B structural guards (PROVE_IT exit, cell mutual exclusion,
//  MIN_ATR floor) NOT carried over -- they are calibrated for 5m USTEC and
//  meaningless on a 4h timeframe.
// =============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "OmegaTradeLedger.hpp"  // omega::TradeRecord
#include "OmegaCostGuard.hpp"
#include "GoldWaveTrend.hpp"   // S-2026-06-03 momentum-confirm gate (omega::gold_wt())
#include "GoldD1TrendState.hpp"  // D1 regime gate (added 2026-05-21)
#include "L2Globals.hpp"         // 2026-05-30: AtomicL2 + g_l2_<sym> globals
#include "L2LeverageState.hpp"   // 2026-05-30: L2 sizing + L2-trail flip helper
#include "OpenPositionRegistry.hpp"  // S-2026-06-03 PositionSnapshot persist/restore
#include "RegimeState.hpp"       // 2026-06-12 Tier-2 re-opt: gold_regime() long-gate
#include "GoldTrendMimicLadder.hpp" // one-way mimic trigger (fire-and-forget on open)
#include <cstdlib>

namespace omega {

// ---------- Bar input (mirrors DonchianBar so the call site is familiar)
struct XauTfBar {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
};

// ---------- Per-cell position state
struct XauTfPos {
    bool        active             = false;
    bool        is_long            = false;
    double      entry_px           = 0.0;
    double      tp_px              = 0.0;
    double      sl_px              = 0.0;
    double      atr_at_entry       = 0.0;
    int64_t     entry_ts_ms        = 0;
    int         bars_held          = 0;
    int         cooldown_bars      = 0;
    double      size_mult          = 1.0;   // S-2026-06-03: regression-slope sizing overlay (1.0 = full)
    // S34 P1 fix #5: per-position MFE/MAE in price units. Both stored as
    // POSITIVE distances from entry. Updated per tick in _manage_open using
    // mid = (bid+ask)/2; propagated to TradeRecord.mfe / TradeRecord.mae in
    // _close.
    double      mfe                = 0.0;   // max favourable excursion (>=0)
    double      mae                = 0.0;   // max adverse  excursion (>=0)
    std::string broker_position_id;
    std::string entry_clOrdId;
};

// ---------- Cell config (signal selector + bracket geometry)
//
// 2026-05-11 S33d extension: added Keltner20 and AdxMom20 cells based on
// Pass-1 edge_hunt results showing 3/3-year positive PnL convergence on
// XAU 4h (Keltner $687, ADX_Mom $648). Both use the same realistic-fill
// bracket as the original three cells.
// S116 (2026-05-19): EmaCross8_21 family added for the S114 long-trend
// ensemble research result.  Long-only, EMA(8)>EMA(21) cross + slow-rising
// filter, ATR(14)x2.5 stop, effectively-no-TP (tp_mult=20.0).  Bit 6 in
// cell_enable_mask; off by default (mask stays 0x3F so existing prod
// behaviour is unchanged).  Engine_init.hpp must explicitly set bit 6
// (mask |= 0x40) to activate.  Python+C++ evidence:
//   Python (S114): +$30,966 (+30.97%) Sharpe +1.96 trades 103 over 25mo
//   C++    (S115): +$32,025 (+32.03%) trades  95 over 25mo (Δ$1,059 / 3.4%)
enum class XauTfFamily { Donchian20, InsideBar, ErTrend020, Keltner20, AdxMom20, RangeExpand, EmaCross8_21, KeltnerEma50 };
struct XauTfCellConfig {
    XauTfFamily family;
    double      sl_mult;   // SL = sl_mult * ATR
    double      tp_mult;   // TP = tp_mult * ATR
    const char* name;      // e.g. "Donchian_N20_sl1.5tp3.0"
};

// Five validated survivor cells -- all 3/3 Duka years +ve, realistic fills.
//
// 2026-05-11 S33g R:R OPTIMISATION: Pass-4 deep_dive showed two cells were
// using sub-optimal TP multipliers. Switching to TP=6.0*ATR (R:R 4:1 with
// SL=1.5*ATR) lifts net materially:
//   InsideBar:   sl=2.0/tp=4.0 ($657)  ->  sl=1.5/tp=6.0 ($1455)  +$797
//   ER0.20:      sl=1.5/tp=3.0 ($805)  ->  sl=1.5/tp=6.0 ($1155)  +$351
//
// The other three cells (Donchian, Keltner, ADX_Mom) are already at
// their optimal R:R per the deep_dive sweep -- left unchanged.
// S33i 2026-05-11: SL-multiplier optimisation from Pass-6 deep_dive v6.
// SL sweep at fixed TP=6.0 showed each cell has a different optimal SL:
//   InsideBar:  sl=2.0   n=110  net=+$1680  3/3 yrs  (vs sl=1.5: $1455 -- +$225)
//   ER0.20:     sl=0.75  n=210  net=+$1279  3/3 yrs  (vs sl=1.5: $1155 -- +$124)
// Different mechanics want different stops. Lock the optima in.
static constexpr XauTfCellConfig kXauTfCells[] = {
    { XauTfFamily::Donchian20,   1.5, 3.0,  "Donchian_N20_sl1.5tp3.0"       },
    { XauTfFamily::InsideBar,    2.0, 6.0,  "InsideBar_sl2.0tp6.0_S33i"     },
    { XauTfFamily::ErTrend020,   0.75, 6.0, "ER0.20_sl0.75tp6.0_S33i"       },
    { XauTfFamily::Keltner20,    1.5, 3.0,  "Keltner_K2_sl1.5tp3.0"         },
    { XauTfFamily::AdxMom20,     2.0, 4.0,  "ADX_Mom_adx25_sl2.0tp4.0"      },
    // S33h Pass-5: RangeExpansion. Fires when bar TR > 1.5*ATR, trades
    // direction of bar. n=128, net=+$574 across 3 Duka years (all +ve).
    { XauTfFamily::RangeExpand,  1.5, 6.0,  "RangeExpand_K1.5_sl1.5tp6.0"   },
    // S116 (2026-05-19): EmaCross8_21 -- long-only EMA(8,21) cross + slow
    // rising filter.  tp_mult=20.0 is effectively-no-TP (TP at 20*ATR is
    // ~$700 above entry on H4 gold, almost never hit); exits run on stop
    // or signal flip in _evaluate_signal (returns 0 when fast<=slow).
    // Bit 6 in cell_enable_mask -- off by default.
    { XauTfFamily::EmaCross8_21, 2.5, 20.0, "EmaCross_8_21_sl2.5_S116"      },
    // S41 (2026-05-30): Keltner EMA50 channel breakout, no-TP runner. From the
    // XAU deep-edge test (backtest/xau_edge_deep.cpp + edge_validate_s41.cpp):
    // on H4, keltner EMA50 is ROBUST across the ENTIRE k{1.5,2.0,2.5} x
    // sl{2.5,3.0,3.5} grid (PF 2.0-3.8, all 5/6 or 6/6 blocks) AND cost-stress
    // 1x/2x/3x (PF 2.79/2.76/2.72). Entry: close > EMA50 + 2.0*ATR with a slow
    // bull gate (close > close 30 H4 bars ago). Exit: close < EMA50 (bar-close,
    // handled in on_h4_bar) OR stop at 3.0*ATR. tp_mult=20.0 == effectively-
    // no-TP (same convention as the EmaCross8_21 cell). Bit 7 in
    // cell_enable_mask; OFF by default (mask stays 0x3F) -- engine_init must
    // OR in 0x80 to activate.
    { XauTfFamily::KeltnerEma50, 3.0, 20.0, "Keltner_EMA50_k2.0_sl3.0_S41"  },
};
static constexpr int kXauTfNumCells =
    static_cast<int>(sizeof(kXauTfCells) / sizeof(kXauTfCells[0]));

// =============================================================================
//  XauTrendFollow4hEngine
// =============================================================================
struct XauTrendFollow4hEngine {
public:
    // 2026-05-30 L2 leverage state (XAU only).
    L2LeverageState l2_;

    // Public knobs (set by engine_init.hpp before init()).
    bool   shadow_mode = true;
    bool   enabled     = false;
    double lot         = 0.01;
    // S-2026-06-03: regression-slope sizing overlay (prod-validated risk-adjusted
    // lift; rDD +22%, both-halves+). Default OFF = byte-identical behaviour.
    // When ON: down-size to reg_size_floor when the 128-bar regression slope of
    // close is negative (breakout against the macro trend-fit). See memory
    // omega-gbb-indicators-eval.
    bool   reg_slope_size = true;   // S-2026-06-03: ENABLED (shadow engines -> live shadow validation of the prod-validated overlay)
    double reg_size_floor = 0.4;
    double max_spread  = 1.0;  // USD; refuse entries above this

    // S96: per-cell enable bitmask. Bit i controls kXauTfCells[i].
    // Default 0x3F = original 6 cells enabled. Set in engine_init.hpp.
    // Backtest v2 showed 4h_InsideBar(1)/4h_ER20(2)/4h_ADX_Mom(4) PF<1.0 →
    // disable those 3 to concentrate on profitable cells.
    // S116: bit 6 added for EmaCross8_21 cell; default still 0x3F to keep
    // production behaviour unchanged.  Engine_init.hpp must explicitly OR
    // in 0x40 to activate the new cell.
    uint32_t cell_enable_mask = 0x3F;  // bits 0-5, original 6 cells

    // 2026-06-12 Tier-2 re-opt: skip LONG entries while gold_regime() reports a
    // sustained H1 bear. TESTED AND REJECTED -- leave OFF. A/B on XAU2022_bear
    // (backtest/xau_tf4h_cell_sweep.cpp): gate changed bear net -0.69->-0.90
    // (Donchian), EmaCross 0.42->0.42 PF (its 2022 bleed is false-cross chop
    // OUTSIDE sustained-bear windows, so the gate never fires on those losers),
    // and cost bull-tape PF 1.58->1.55. Kept as an inert flag so the negative
    // result is discoverable and nobody re-chases it.
    bool use_regime_long_gate = false;

    // S88-followup (2026-05-27): ATR-percentile vol-band gate (proven on
    // XauThreeBar30m: PF 1.55 -> 2.23, MaxDD -56% on 6mo PKL). Skip entries
    // when current ATR is outside the [low, high] percentile band of the
    // rolling 200-bar ATR distribution. Removes dead-vol (no follow-through)
    // and crisis-vol (gap-through-SL) entries. Per-cell; ANDed with the
    // existing cell_enable_mask. Default OFF (regression-safe).
    bool   use_vol_band_gate = false;
    double vol_band_low_pct  = 0.30;
    double vol_band_high_pct = 0.85;
    std::deque<double> atr_vol_window_;
    // S-2026-06-29 IMPULSE FILTER (ported from XauTrendFollow1h/D1): the entry bar
    // must thrust >= min_impulse_atr*ATR14 ((bar.high - prior close) >= mult*ATR).
    // Filters weak/stalling breakouts. 4h has no dip-buy family so it applies to all
    // cells (mirrors D1 "applies to all"). 0 = OFF (byte-identical to pre-change).
    double min_impulse_atr   = 0.0;
    // S-2026-06-30 ADX CHOP-GATE (study): block ALL cell entries when the
    // engine's own Wilder ADX14 is below this floor (= ranging/chop). Targets
    // the EMA-cross/breakout whipsaw-into-a-range failure. 0 = OFF (byte-
    // identical to pre-change). Applies on top of each cell's own gates.
    double min_adx_entry     = 0.0;
    // S88-followup post-sweep 2026-05-27: per-cell gate mask (default
    // all-ones = regression-safe). Operator can target specific cells
    // when their preferred gate differs (see 2h Donch50 ADX-hurts pattern).
    uint32_t cell_vol_band_mask = 0xFFFFFFFF;  // rolling ATR(14) values for percentile

    // S63 2026-05-14 (part W): VWR-pattern in-flight protection (full trio).
    //   Defaults to ALL ZERO -- S63 is OFF on production until per-instrument
    //   backtest evidence justifies enabling. The XauTrendFollow trio
    //   (2h/4h/D1) is in STATE B (hooks declared + mgmt-path implemented +
    //   deliberately zeroed at class default) until the planned Phase 3
    //   walk-forward sweep (scripts/xtf_*_wf_t1.py + the dedicated
    //   XauTrendFollowBacktest harness) produces baseline-vs-tuned evidence.
    //   The S63-adverse pattern from VWR USTEC (S71 Phase 3) and UTF5m
    //   USTEC (S73 Phase 3) is the prior; the XTF trio sweep is the test
    //   of whether the pattern generalises to XAU trend-follow at higher
    //   timeframes, or whether XAU's tail shape flips the sign. Pre-commit
    //   pass criterion: aggregate PF >= 1.20 AND >=3/4 windows pass the
    //   per-window avg_pnl >= +0.001 threshold (same protocol as UTF5m S73).
    //
    //   LOSS_CUT_PCT  -- cut when adverse >= entry*pct/100. Phase 2 (cold-loss).
    //   BE_ARM_PCT    -- mfe % of entry that arms BE ratchet. Phase 1 (giveback).
    //   BE_BUFFER_PCT -- BE_CUT triggers when move <= entry*pct/100 after arm.
    //   Override via the backtest harness CLI for the sweep; set _PCT = 0.0
    //   to disable a phase entirely.
    //
    // ADVERSE-PROTECTION: (S-2026-06-19 — backtested verdict, MANDATORY per CLAUDE.md gate)
    //   Verdict = TRAIL-ONLY (no profit-banking). The give-back of open profit is the
    //   INTRINSIC cost of the trend-runner edge. Deep-BE profit-lock swept 10 configs
    //   (xau_tf4h_cell_sweep.cpp --be-arm/--be-buf) across BOTH regimes:
    //     BULL  (gold 2024-26): baseline PF1.58 net+57.7 -> every BE config 1.3-1.44, net DOWN.
    //     BEAR  (gold 2022-23, metal -22%): baseline PF1.13 net+4.5 (engine is cross-regime
    //           positive — NOT bull-beta) -> BE neutral-to-worse.
    //   No config improves net OR reduces the worst trade in either regime; banking clips the
    //   runners that ARE the edge. So LOSS_CUT/BE stay 0.0 by design (a recorded "trail-only,
    //   cut hurts net" verdict — valid per the standing rule). Re-test only with new evidence.
    //   See [[omega-runner-profit-protect-regime]].
    double LOSS_CUT_PCT  = 0.0;
    double BE_ARM_PCT    = 0.0;
    double BE_BUFFER_PCT = 0.0;

    // Per-cell state. Indexed by kXauTfCells.
    std::array<XauTfPos, kXauTfNumCells> pos{};

    // Bar history (last N=64 bars; enough for Donchian20 + ER lookback + warmup).
    static constexpr int kBarHistory = 64;
    std::deque<XauTfBar> bars_;

    // Rolling Wilder ATR14 (mirrors what g_bars_gold.h4.ind.atr14 publishes,
    // but recomputed locally so the engine is self-contained).
    static constexpr int kAtrPeriod = 14;
    double atr14_ = 0.0;
    int    atr_warmup_count_ = 0;

    // 2026-05-11 S33d-ext: rolling EMA20 (for Keltner channel) and ADX14
    // (for AdxMom cell).  Updated on each on_h4_bar call.
    static constexpr int kEmaPeriod = 20;
    double ema20_ = 0.0;
    bool   ema_initialised_ = false;

    // S41: EMA50 for the KeltnerEma50 cell (validated H4 channel mid-line).
    static constexpr int kEmaPeriod50 = 50;
    double ema50_ = 0.0;
    bool   ema50_initialised_ = false;

    // S116: rolling EMA8 + EMA21 for EmaCross8_21 cell.  Slow-rising filter
    // checks ema21_ against its value 3 bars ago, so we keep the last
    // ~6 closes of ema21 in a small ring.
    static constexpr int kEmaFastFor8_21 = 8;
    static constexpr int kEmaSlowFor8_21 = 21;
    static constexpr int kEmaSlowRiseLookback = 3;
    double ema8_  = 0.0;
    double ema21_ = 0.0;
    bool   ema8_21_initialised_ = false;
    std::deque<double> ema21_hist_;  // last ~8 values for rising check

    // ADX Wilder state -- accumulators across bars.
    static constexpr int kAdxPeriod = 14;
    double adx14_ = 0.0;
    double adx_atr_sum_ = 0.0;
    double adx_pdm_sum_ = 0.0;
    double adx_mdm_sum_ = 0.0;
    double adx_dx_sum_  = 0.0;
    int    adx_warmup_count_ = 0;
    int    adx_dx_count_     = 0;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    // S102: warmup entry guard. True while warmup_from_csv is running.
    // Blocks _fire_entry so indicator-priming bars don't open stale
    // positions at historical price levels. See XauTrendFollow2hEngine.hpp
    // S102 comment for the full root-cause write-up.
    bool warmup_active_ = false;

    // S-2026-07-02 KILL-THE-4H-WAIT (operator): after a restart the engine could
    // only evaluate a new entry at the NEXT live H4 close (up to 4h idle), because
    // the bundled warm-seed CSV ends ~60d stale and nothing replays recent bars.
    // append_fresh_h4() primes indicators from the live H4 dump and stashes the last
    // CLOSED bar here; try_boot_fire() (first live tick) evaluates it for entry so a
    // valid signal is taken on boot. Guarded so a stale bar isn't filled at a moved price.
    bool     boot_fire_armed_ = false;
    bool     has_boot_last_   = false;
    XauTfBar boot_last_bar_{};

    void init() noexcept {
        bars_.clear();
        atr14_ = 0.0;
        atr_warmup_count_ = 0;
        ema20_ = 0.0;
        ema_initialised_ = false;
        ema50_ = 0.0;
        ema50_initialised_ = false;
        ema8_ = ema21_ = 0.0;
        ema8_21_initialised_ = false;
        ema21_hist_.clear();
        adx14_ = 0.0;
        adx_atr_sum_ = adx_pdm_sum_ = adx_mdm_sum_ = adx_dx_sum_ = 0.0;
        adx_warmup_count_ = 0;
        adx_dx_count_ = 0;
        warmup_active_ = false;
        boot_fire_armed_ = false;
        has_boot_last_ = false;
        boot_last_bar_ = {};
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
    //  XauTfPos carries a regression-sizing size_mult; on restore it is
    //  reset to 1.0 (the overlay re-evaluates on the next fresh entry).
    // ============================================================
    void persist_save_all(const char* base_engine, const char* sym,
                          std::vector<omega::PositionSnapshot>& out) const {
        for (int ci = 0; ci < kXauTfNumCells; ++ci) {
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
        if (ci < 0 || ci >= kXauTfNumCells) return false;
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
        p.size_mult     = 1.0;       // preserve regression-sizing field at full size
        // one-way mimic notify (fire-and-forget; never reads/touches this position)
        omega::gold_trend_mimic().on_trend_open(mimic_tag, p.is_long ? 1 : -1, p.entry_px, ps.entry_ts);
        return true;
    }

    // ============================================================
    //  on_h4_bar -- called by tick_gold.hpp when an H4 bar closes
    // ============================================================
    void on_h4_bar(const XauTfBar& bar,
                   double bid, double ask,
                   double atr14_external,    // from g_bars_gold.h4.ind.atr14
                   int64_t now_ms,
                   OnCloseFn on_close) noexcept {
        if (!enabled) return;

        // Append bar to history.
        bars_.push_back(bar);
        while ((int)bars_.size() > kBarHistory) bars_.pop_front();

        // SPECIFIC FEED: drive this engine's mimic book's leg management on the H4 bar (the
        // cadence GoldTrendMimicLadder backtested XauTf4h on). One-way; no-op if the book/tag
        // is not registered or has no open legs. Per-instance mimic_tag (S-17q): the MGC
        // instance feeds ITS book, never the spot book (was cross-feeding via the literal).
        omega::gold_trend_mimic().on_bar(mimic_tag, bar.high, bar.low, bar.close, now_ms / 1000);

        // Use external ATR if provided (preferred -- matches the rest of
        // the stack); otherwise compute locally.
        if (atr14_external > 0.0) {
            atr14_ = atr14_external;
        } else {
            _update_local_atr();
        }

        // S33d-ext: update EMA20 (for Keltner) and ADX14 (for AdxMom).
        _update_ema20();
        _update_ema50();   // S41: KeltnerEma50 channel mid-line
        _update_adx14();
        // S116: update EMA8/EMA21 for EmaCross8_21 cell.
        _update_ema8_21();

        // Advance cell cooldowns and increment bars_held on open positions.
        for (auto& p : pos) {
            if (p.cooldown_bars > 0) --p.cooldown_bars;
            if (p.active) ++p.bars_held;
        }

        // S41: KeltnerEma50 is a no-TP runner that exits on a close back below
        // EMA50 (channel mid). This is a bar-close exit -- the only one in this
        // engine -- mirroring the 1h Donchian-low exit. All other cells still
        // exit purely intra-bar via on_tick. Runs before new-entry decisions.
        if (ema50_initialised_) {
            for (int ci = 0; ci < kXauTfNumCells; ++ci) {
                if (kXauTfCells[ci].family == XauTfFamily::KeltnerEma50
                    && pos[ci].active && bar.close < ema50_) {
                    _close(ci, bar.close, "KELT50_MID_EXIT", now_ms, on_close);
                }
            }
        }

        // Decide entries cell-by-cell. We do NOT re-check exits here --
        // exits run intra-bar on every tick via on_tick (so SL/TP can hit
        // within the bar). The bar-close is only for new-entry decisions.
        if ((int)bars_.size() < 32) return;  // warmup
        if (atr14_ <= 0.0) return;
        if (ask - bid > max_spread) {
            // S-2026-07-02 observability: silently zeroed ALL entries for the bar
            // (zero-trades post-mortem rule: every total-veto must log).
            std::printf("[XTF4H-BLOCK] spread %.2f > max %.2f -- whole-bar entry skip\n",
                        ask - bid, max_spread);
            return;
        }

        // S88-followup: maintain rolling ATR window for vol-band gate.
        if (use_vol_band_gate && atr14_ > 0.0) {
            atr_vol_window_.push_back(atr14_);
            if ((int)atr_vol_window_.size() > 200) atr_vol_window_.pop_front();
        }

        // S-2026-07-10 log de-spam: aggregate per-cell/-bar skip reasons into ONE
        // summary line per bar (below), instead of a printf per skipped cell. A quiet
        // engine could otherwise emit 6+ [XTF4H-BLOCK] lines every bar and dominate
        // the log (the impulse gate alone logged ~165 times over a 2yr BT while the
        // trade count was UNCHANGED -- those blocked cells re-enter later). Pure
        // logging change, zero effect on entry logic.
        int nskip_impulse = 0, nskip_adx_chop = 0, nskip_adx_cold = 0;
        for (int ci = 0; ci < kXauTfNumCells; ++ci) {
            if (!(cell_enable_mask & (1u << ci))) continue;  // S96: per-cell gate
            if (pos[ci].active) continue;
            if (pos[ci].cooldown_bars > 0) continue;
            int side = _evaluate_signal(ci);
            if (side == 0) continue;
            // S-2026-06-29 IMPULSE FILTER: entry bar must thrust >= min_impulse_atr*ATR14.
            // S-2026-07-02 BUG FIX (zero-trades post-mortem): the original formula
            // measured (bar.high - prev_close) for BOTH sides -- upward thrust only.
            // A short-breakout bar has high ~= prev_close -> thrust ~= 0 -> EVERY
            // short signal was vetoed since 2026-06-29. With longs blocked by the D1
            // bear gate, that left the engine dead in both directions. Thrust is now
            // direction-aware: longs measure the up-thrust, shorts the down-thrust.
            if (min_impulse_atr > 0.0 && atr14_ > 0.0 && (int)bars_.size() >= 2) {
                const double prev_close = bars_[bars_.size()-2].close;
                const double thrust = (side > 0) ? (bar.high - prev_close)
                                                 : (prev_close - bar.low);
                if (thrust < min_impulse_atr * atr14_) { ++nskip_impulse; continue; }
            }
            // S-2026-06-30 ADX CHOP-GATE (study): skip entries when ADX14 < floor (ranging).
            if (min_adx_entry > 0.0) {
                if (adx_dx_count_ < kAdxPeriod) { ++nskip_adx_cold; continue; }  // ADX not warm
                if (adx14_ < min_adx_entry)    { ++nskip_adx_chop; continue; }  // chop -> skip
            }
            // 2026-06-12 regime gate: no new longs in sustained gold bear.
            if (use_regime_long_gate && side > 0
                && omega::gold_regime().long_blocked()) continue;
            // S88-followup vol-band gate (per-cell mask)
            if (use_vol_band_gate && (cell_vol_band_mask & (1u << ci))
                && (int)atr_vol_window_.size() >= 200) {
                int below = 0;
                const int n = (int)atr_vol_window_.size();
                for (int i = 0; i < n; ++i) if (atr_vol_window_[i] < atr14_) ++below;
                const double pct = static_cast<double>(below) / n;
                if (pct < vol_band_low_pct || pct > vol_band_high_pct) {
                    continue;  // VOL_BAND_OUT (S88-followup)
                }
            }
            _fire_entry(ci, side, bid, ask, now_ms);
        }
        // S-2026-07-10 de-spammed: ONE aggregated skip line per bar (was a printf
        // per skipped cell). impulse= weak-breakout skips, adx_chop= ADX<floor,
        // adx_cold= ADX not warm. Prints only when at least one cell was skipped.
        if (nskip_impulse | nskip_adx_chop | nskip_adx_cold) {
            std::printf("[XTF4H-SKIP] cells skipped this bar: impulse=%d adx_chop=%d(<%.0f) adx_cold=%d\n",
                        nskip_impulse, nskip_adx_chop, min_adx_entry, nskip_adx_cold);
        }
        (void)on_close; // signature parity; we only emit on exit (in on_tick)
    }

    // ============================================================
    //  on_tick -- runs every tick to manage open positions
    // ============================================================
    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseFn on_close) noexcept {
        l2_.push(g_l2_gold);  // 2026-05-30: maintain L2 ring buffer always
        if (!enabled) return;
        for (int ci = 0; ci < kXauTfNumCells; ++ci) {
            if (!pos[ci].active) continue;
            // 2026-05-30 L2-trail flip: close cell if mic_avg flips against
            // side AND mfe >= 1R. (XAU only -- this engine is XAU-specific.)
            auto& p = pos[ci];
            const double sl_dist = std::fabs(p.entry_px - p.sl_px);
            const int side = p.is_long ? +1 : -1;
            if (sl_dist > 0 && l2_.check_trail_flip(side, p.mfe, sl_dist)) {
                const double exit_px = p.is_long ? bid : ask;
                _close(ci, exit_px, "L2_FLIP", now_ms, on_close);
                continue;
            }
            _manage_open(ci, bid, ask, now_ms, on_close);
        }
    }

    // ============================================================
    //  force_close -- end-of-day or shutdown flatten
    // ============================================================
    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseFn on_close, const char* reason) noexcept {
        for (int ci = 0; ci < kXauTfNumCells; ++ci) {
            if (!pos[ci].active) continue;
            double exit_px = pos[ci].is_long ? bid : ask;
            _close(ci, exit_px, reason ? reason : "FORCE_CLOSE", now_ms, on_close);
        }
    }

private:
    // ---------- Local ATR computation (Wilder) over bars_
    void _update_local_atr() noexcept {
        if ((int)bars_.size() < 2) { atr14_ = 0.0; return; }
        const auto& cur  = bars_.back();
        const auto& prev = bars_[bars_.size() - 2];
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

    // ---------- Indicator updaters (S33d-ext: EMA20 + ADX14)
    void _update_ema20() noexcept {
        if (bars_.empty()) return;
        double c = bars_.back().close;
        if (!ema_initialised_) {
            ema20_ = c;
            ema_initialised_ = true;
        } else {
            double alpha = 2.0 / (kEmaPeriod + 1);
            ema20_ = alpha * c + (1.0 - alpha) * ema20_;
        }
    }

    // S41: EMA50 updater for the KeltnerEma50 cell.
    void _update_ema50() noexcept {
        if (bars_.empty()) return;
        const double c = bars_.back().close;
        if (!ema50_initialised_) { ema50_ = c; ema50_initialised_ = true; }
        else { const double a = 2.0 / (kEmaPeriod50 + 1); ema50_ = a * c + (1.0 - a) * ema50_; }
    }

    void _update_adx14() noexcept {
        if (bars_.size() < 2) return;
        const auto& cur  = bars_[bars_.size() - 1];
        const auto& prev = bars_[bars_.size() - 2];
        double up = cur.high - prev.high;
        double dn = prev.low - cur.low;
        double pdm = (up > dn && up > 0) ? up : 0.0;
        double mdm = (dn > up && dn > 0) ? dn : 0.0;
        double tr  = std::max(cur.high - cur.low,
                              std::max(std::abs(cur.high - prev.close),
                                       std::abs(cur.low  - prev.close)));
        // Wilder warmup: simple sum for first kAdxPeriod bars
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

    // S116: update EMA8 + EMA21 + slow-EMA history for EmaCross8_21 cell.
    void _update_ema8_21() noexcept {
        if (bars_.empty()) return;
        const double c = bars_.back().close;
        if (!ema8_21_initialised_) {
            ema8_ = ema21_ = c;
            ema8_21_initialised_ = true;
        } else {
            const double alpha8  = 2.0 / (kEmaFastFor8_21 + 1);
            const double alpha21 = 2.0 / (kEmaSlowFor8_21 + 1);
            ema8_  = alpha8  * c + (1.0 - alpha8)  * ema8_;
            ema21_ = alpha21 * c + (1.0 - alpha21) * ema21_;
        }
        ema21_hist_.push_back(ema21_);
        while ((int)ema21_hist_.size() > (kEmaSlowRiseLookback + 4)) {
            ema21_hist_.pop_front();
        }
    }

    // ---------- Signal evaluators
    int _evaluate_signal(int cell_idx) const noexcept {
        switch (kXauTfCells[cell_idx].family) {
            case XauTfFamily::Donchian20:   return _sig_donchian20();
            case XauTfFamily::InsideBar:    return _sig_inside_bar();
            case XauTfFamily::ErTrend020:   return _sig_er_trend(0.20, 20);
            case XauTfFamily::Keltner20:    return _sig_keltner();
            case XauTfFamily::AdxMom20:     return _sig_adx_momentum(25.0, 20);
            case XauTfFamily::RangeExpand:  return _sig_range_expansion(1.5);
            case XauTfFamily::EmaCross8_21: return _sig_ema_cross_8_21();
            case XauTfFamily::KeltnerEma50: return _sig_keltner_ema50();
        }
        return 0;
    }

    // S41: Keltner EMA50 upper-channel breakout, long-only, with a self-
    // contained slow-bull gate (close > close 30 H4 bars ago, ~1 trading week).
    // EXACT port of the validated edge (edge_validate_s41.cpp keltner_e50_k2.0).
    // The engine's D1 EMA200 gate in _fire_entry applies on top. Exit is the
    // close<EMA50 bar-close path in on_h4_bar, or the 3.0*ATR stop.
    int _sig_keltner_ema50() const noexcept {
        constexpr int    BULL_LB = 30;
        constexpr double K       = 2.0;
        if (!ema50_initialised_ || atr14_ <= 0.0) return 0;
        const int n = (int)bars_.size();
        if (n < BULL_LB + 2) return 0;
        const double c = bars_[n - 1].close;
        if (!(c > bars_[n - 1 - BULL_LB].close)) return 0;   // slow bull gate
        if (c > ema50_ + K * atr14_) return +1;              // upper channel break
        return 0;
    }

    // S116: long-only EMA8>EMA21 cross with slow-rising filter.
    // Returns +1 when EMA8 > EMA21 AND EMA21 has risen vs. the value
    // kEmaSlowRiseLookback bars ago.  Never returns -1 (long-only by
    // design per S114 research).  Exit on signal-flat happens through
    // the standard bracket -- when the EMA condition fails on next bar
    // and we are flat, the cell simply doesn't re-enter; existing open
    // positions exit on the bracketed stop (SL=2.5*ATR) or the very
    // distant TP (tp_mult=20.0).  S116 does NOT add a signal-flip exit
    // path -- that's deferred to S117 if the shadow data shows runners
    // sticking around past the trend.
    int _sig_ema_cross_8_21() const noexcept {
        if (!ema8_21_initialised_) return 0;
        if ((int)ema21_hist_.size() <= kEmaSlowRiseLookback) return 0;
        if ((int)bars_.size() < kEmaSlowFor8_21 + 2) return 0;
        const double slow_now  = ema21_hist_.back();
        const double slow_then = ema21_hist_[ema21_hist_.size() - 1 - kEmaSlowRiseLookback];
        const bool fast_above  = (ema8_ > ema21_);
        const bool slow_rising = (slow_now > slow_then);
        if (fast_above && slow_rising) return +1;
        return 0;  // long-only: never short
    }

    // S33h-ext: range-expansion bar entry. When current bar's true range
    // exceeds K*ATR14, trade direction of the bar (close vs open).
    int _sig_range_expansion(double K) const noexcept {
        if (atr14_ <= 0.0 || bars_.size() < 16) return 0;
        const auto& cur = bars_.back();
        double tr = cur.high - cur.low;
        if (tr < K * atr14_) return 0;
        if (cur.close > cur.open) return +1;
        if (cur.close < cur.open) return -1;
        return 0;
    }

    // S33d-ext: Keltner channel break. EMA20 +/- 2*ATR14 envelope.
    int _sig_keltner() const noexcept {
        if (!ema_initialised_ || atr14_ <= 0.0) return 0;
        if (bars_.size() < 22) return 0;
        const auto& cur = bars_.back();
        double up = ema20_ + 2.0 * atr14_;
        double lo = ema20_ - 2.0 * atr14_;
        if (cur.close > up) return +1;
        if (cur.close < lo) return -1;
        return 0;
    }

    // S33d-ext: ADX-gated momentum.  Only fire when ADX >= adx_thr.
    int _sig_adx_momentum(double adx_thr, int N) const noexcept {
        if (adx_dx_count_ < kAdxPeriod) return 0;     // ADX not warm yet
        if (adx14_ < adx_thr) return 0;
        if ((int)bars_.size() < N + 2) return 0;
        const int last = (int)bars_.size() - 1;
        double cur  = bars_[last].close;
        double prev = bars_[last - N].close;
        if (cur > prev * 1.001) return +1;
        if (cur < prev * 0.999) return -1;
        return 0;
    }

    int _sig_donchian20() const noexcept {
        const int N = 20;
        if ((int)bars_.size() < N + 1) return 0;
        const int last = (int)bars_.size() - 1;  // current closed bar
        double hi = bars_[last - N].high, lo = bars_[last - N].low;
        for (int k = last - N + 1; k <= last - 1; ++k) {
            if (bars_[k].high > hi) hi = bars_[k].high;
            if (bars_[k].low  < lo) lo = bars_[k].low;
        }
        // bar[last-1] is the "prior_high/low"; bar[last] is the close that fires.
        if (bars_[last].close > hi) return +1;
        if (bars_[last].close < lo) return -1;
        return 0;
    }

    int _sig_inside_bar() const noexcept {
        if ((int)bars_.size() < 3) return 0;
        const int last = (int)bars_.size() - 1;
        const auto& a = bars_[last - 2];  // mother bar
        const auto& b = bars_[last - 1];  // inside bar
        const auto& c = bars_[last];      // breakout bar
        if (!(b.high < a.high && b.low > a.low)) return 0;
        if (c.close > b.high) return +1;
        if (c.close < b.low)  return -1;
        return 0;
    }

    // Kaufman Efficiency Ratio over last er_n bars, gate momentum at mom_n
    int _sig_er_trend(double er_thr, int N) const noexcept {
        if ((int)bars_.size() < N + 2) return 0;
        const int last = (int)bars_.size() - 1;
        double num = std::abs(bars_[last].close - bars_[last - N].close);
        double den = 0.0;
        for (int k = last - N + 1; k <= last; ++k) {
            den += std::abs(bars_[k].close - bars_[k - 1].close);
        }
        if (den < 1e-12) return 0;
        double er = num / den;
        if (er < er_thr) return 0;
        // momentum direction
        if (bars_[last].close > bars_[last - N].close) return +1;
        if (bars_[last].close < bars_[last - N].close) return -1;
        return 0;
    }

    // ---------- Entry / exit
    void _fire_entry(int ci, int side, double bid, double ask, int64_t now_ms) noexcept {
        // S102: block entries during warmup — warmup primes indicators only.
        if (warmup_active_) return;
        // 2026-05-21: D1 EMA200 regime gate (chokepoint for all 4h cells).
        // S-2026-07-02: vetoes now LOG (zero-trades post-mortem: silent chokepoints
        // suppressed the whole gold book for weeks with no way to see which gate).
        if (side > 0 && !omega::gold_d1_trend().long_allowed()) {
            std::printf("[XTF4H-BLOCK] cell=%d D1-EMA200 gate: long not allowed -- vetoed\n", ci);
            return;
        }
        if (side < 0 && !omega::gold_d1_trend().short_allowed()) {
            std::printf("[XTF4H-BLOCK] cell=%d D1-EMA200 gate: short not allowed -- vetoed\n", ci);
            return;
        }
        // 2026-06-13: ALSO gate shorts on the price-based sustained-bull regime
        // (gold_regime().short_blocked() = is_bull). The laggy D1 EMA200 gate let
        // SHORT cells fill into the gold bull (XAU_4h_DonchN20/N100 SHORT 4196 ->
        // stopped 4203 over the weekend). RegimeState releases the moment price
        // loses EMA200, so it never blocks a genuine bear short. Strictly
        // subtractive: can only reject shorts during a confirmed uptrend.
        if (side < 0 && omega::gold_regime().short_blocked()) {
            std::printf("[XTF4H-BLOCK] cell=%d regime short_blocked (%s) -- short vetoed\n",
                        ci, omega::gold_regime().regime_name());
            return;
        }
        const auto& cfg = kXauTfCells[ci];
        double entry = (side > 0) ? ask : bid;
        if (entry <= 0.0 || atr14_ <= 0.0) return;
        double sl_dist = cfg.sl_mult * atr14_;
        double tp_dist = cfg.tp_mult * atr14_;
        double sl_px = (side > 0) ? entry - sl_dist : entry + sl_dist;
        double tp_px = (side > 0) ? entry + tp_dist : entry - tp_dist;

        // 2026-05-12 cost gate -- see outputs/PLAN_A_B_REPORT.md
        // S-2026-07-11 GOLD PHASE 1: gate on ledger_symbol (spot instances =
        // "XAUUSD" row, unchanged; MGC venue instances = the explicit MGC row
        // instead of the ~10x-misscaled spot proxy).
        {
            const double spread_pts = ask - bid;
            if (!ExecutionCostGuard::is_viable(
                    ledger_symbol.c_str(), spread_pts, tp_dist, lot, 1.5))
            {
                return;
            }
        }

        // Momentum-confirm gate (S-2026-06-03): gold-validated WaveTrend filter.
        if (omega::gold_wt().gate_enabled && !omega::gold_wt().confirms(side > 0)) {
            omega::gold_wt().record_skip();
            printf("[GOLD-MOMGATE] XauTF4h cell=%d SKIP %s (no momentum confirm) "
                   "wt1=%.1f regime_up=%d bars=%ld\n", ci, side > 0 ? "long" : "short",
                   omega::gold_wt().wt1(), (int)omega::gold_wt().regime_up(),
                   omega::gold_wt().bars_seen());
            fflush(stdout);
            return;
        }
        if (omega::gold_wt().gate_enabled) omega::gold_wt().record_pass();

        // ── L2 entry gate (2026-05-30) ──
        {
            const double mic = g_l2_gold.microprice_bias.load(std::memory_order_relaxed);
            const double imb = g_l2_gold.imbalance.load(std::memory_order_relaxed);
            if (side > 0  && mic < -0.10) return;
            if (side < 0  && mic >  0.10) return;
            if (std::fabs(imb - 0.5) > 0.01) {
                if (side > 0 && imb < 0.40) return;
                if (side < 0 && imb > 0.60) return;
            }
        }

        // 2026-05-30 L2 sizing skipped here -- XauTfPos lacks per-position size
        // field (engine ships uniform `lot` to all cells). Sizing would
        // require pos struct extension; deferred to next iteration.
        auto& p = pos[ci];
        p.active        = true;
        p.is_long       = (side > 0);
        p.entry_px      = entry;
        p.sl_px         = sl_px;
        p.tp_px         = tp_px;
        p.atr_at_entry  = atr14_;
        p.entry_ts_ms   = now_ms;
        p.bars_held     = 0;
        p.cooldown_bars = 0;
        // S-2026-06-03: regression-slope sizing overlay (default OFF). Down-size
        // when the 128-bar regression slope of close is negative (breakout against
        // the macro trend-fit). Prod-validated risk-adjusted lift; see memory.
        p.size_mult = 1.0;
        if (reg_slope_size) {
            const int W = 128;
            if ((int)bars_.size() >= W) {
                const int s0 = (int)bars_.size() - W;
                double Sx=0, Sy=0, Sxx=0, Sxy=0;
                for (int k=0; k<W; ++k) { double x=k, y=bars_[s0+k].close; Sx+=x; Sy+=y; Sxx+=x*x; Sxy+=x*y; }
                const double den = (double)W*Sxx - Sx*Sx;
                const double slope = (den != 0.0) ? ((double)W*Sxy - Sx*Sy)/den : 0.0;
                if (slope < 0.0) p.size_mult = reg_size_floor;
            }
        }
        // S34 P1 fix #5: reset MFE/MAE per new entry.
        p.mfe           = 0.0;
        p.mae           = 0.0;
        p.broker_position_id.clear();
        p.entry_clOrdId.clear();

        // GoldTrendMimicLadder one-way notify (fire-and-forget; never reads/touches this
        // position). S-2026-07-17q GAP FIX: since 11e2b0fe the ONLY fire in this class sat
        // in persist_restore — live opens never spawned mimic legs, so the book only saw
        // entries that happened to survive a restart. The books were certified on the FULL
        // parent entry stream (clip_path harnesses drive every open); this restores that
        // contract. Pre-arm (seed replay) opens can't fire: enabled=false during warmup.
        omega::gold_trend_mimic().on_trend_open(mimic_tag, p.is_long ? 1 : -1, p.entry_px, now_ms / 1000);

        // Live order dispatch is owned by the caller of this engine via
        // engine_init.hpp's fire hook (mirrors what GoldMicroScalper used).
        // The engine itself stays broker-agnostic; the host code is
        // responsible for translating this fire into a real order when
        // shadow_mode == false.
    }

    void _manage_open(int ci, double bid, double ask, int64_t now_ms,
                      OnCloseFn on_close) noexcept {
        auto& p = pos[ci];
        if (!p.active) return;

        // S34 P1 fix #5: update MFE/MAE on every tick using mid price.
        // Both stored as positive distances from entry.
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

        // Long: exit at bid when bid touches sl/tp. Short: exit at ask.
        bool hit_tp = false, hit_sl = false;
        double exit_px = 0.0;
        if (p.is_long) {
            if (bid <= p.sl_px) { exit_px = p.sl_px; hit_sl = true; }
            else if (bid >= p.tp_px) { exit_px = p.tp_px; hit_tp = true; }
        } else {
            if (ask >= p.sl_px) { exit_px = p.sl_px; hit_sl = true; }
            else if (ask <= p.tp_px) { exit_px = p.tp_px; hit_tp = true; }
        }
        if (!hit_tp && !hit_sl) return;

        const char* reason = hit_tp ? "TP_HIT" : "SL_HIT";
        _close(ci, exit_px, reason, now_ms, on_close);
    }

    void _close(int ci, double exit_px, const char* reason,
                int64_t now_ms, OnCloseFn on_close) noexcept {
        auto& p = pos[ci];
        if (!p.active) return;

        // S34 P1 fix #1: compute pts_move and assign tr.pnl.
        // pts_move is signed against position direction so that profitable
        // trades produce a positive number. handle_closed_trade applies the
        // symbol-specific tick_value_multiplier downstream.
        const double pts_move = p.is_long ? (exit_px - p.entry_px)
                                          : (p.entry_px - exit_px);

        // Build TradeRecord.  Caller's on_close (e.g. bracket_on_close)
        // handles ledger ingest + PnL math.
        omega::TradeRecord tr;
        tr.symbol     = ledger_symbol;
        // S34 P1 fix #3: per-cell engine string so ledger queries grouping
        // by engine can distinguish cells. Cell name is unique per row.
        tr.engine     = ledger_prefix + kXauTfCells[ci].name;
        // S34 P1 fix #4: ledger convention is LONG/SHORT, not BUY/SELL.
        tr.side       = p.is_long ? "LONG" : "SHORT";
        tr.entryPrice = p.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = p.tp_px;
        tr.sl         = p.sl_px;
        tr.size       = lot * p.size_mult;   // S-2026-06-03: regression-slope sizing overlay
        tr.entryTs    = p.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = kXauTfCells[ci].name;  // cell name -> regime field (unchanged)
        tr.shadow     = shadow_mode;
        // S34 P1 fix #1: assign gross pnl. tick_value_multiplier(symbol) is
        // applied downstream by handle_closed_trade to get USD.
        tr.pnl        = pts_move * lot * p.size_mult;   // S-2026-06-03: regression-slope sizing overlay
        // S34 P1 fix #5: propagate MFE/MAE accumulated during the position
        // life. Both in price units (XAU points).
        tr.mfe        = p.mfe;
        tr.mae        = p.mae;

        if (on_close) on_close(tr);

        // Reset cell state, apply cooldown (1 bar = 4h)
        p.active        = false;
        p.broker_position_id.clear();
        p.entry_clOrdId.clear();
        p.cooldown_bars = 1;
        // (mfe/mae reset on next _fire_entry; harmless to leave residual)
    }

public:
    // S101: warmup from pre-built H4 bar CSV. Mirrors Tsmom pattern.
    //   Feeds historical bars through on_h4_bar with noop close callback
    //   so ATR, Donchian lookback, ADX, EMA are all primed before first
    //   live tick arrives. CSV format: bar_start_ms,open,high,low,close
    std::string warmup_csv_path;

    // S-2026-07-07 MGC venue port: a second instance of this class runs on the
    // MGC futures feed (MgcFastDonchianFeed.hpp). These give that instance a
    // distinct ledger tag + symbol; defaults keep the spot instance byte-identical.
    std::string ledger_prefix = "XauTrendFollow4h_";
    std::string ledger_symbol = "XAUUSD";
    // S-2026-07-17q: per-INSTANCE mimic tag (was the hardcoded literal "XauTf4h" in both
    // the on_trend_open fire and the on_bar feed). With the literal, the MGC instance
    // (g_mgc_tf_4h) cross-fed the SPOT XauTf4h mimic book with MGC H4 bars (double
    // cadence + futures basis) and an MGC persist_restore would spawn a leg in the spot
    // book at an MGC price. Spot default keeps the spot instance byte-identical; the MGC
    // instance sets "MgcTF4h" (its own certified book, MGC_VENUE_MIMIC_FINDINGS).
    std::string mimic_tag = "XauTf4h";

    int warmup_from_csv(const std::string& path) noexcept {
        if (!enabled) { printf("[XauTF4h-WARMUP] skipped -- disabled\n"); fflush(stdout); return 0; }
        if (path.empty()) { printf("[XauTF4h-WARMUP] skipped -- no path (cold start)\n"); fflush(stdout); return 0; }
        std::ifstream f(path);
        if (!f.is_open()) { printf("[XauTF4h-WARMUP] FAIL -- cannot open '%s'\n", path.c_str()); fflush(stdout); return 0; }
        warmup_active_ = true;  // S102: block entries during indicator priming

        int fed = 0;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#' || line[0] == 'b') continue; // skip header/comments
            char* p1; long long ms = std::strtoll(line.c_str(), &p1, 10);
            if (!p1 || *p1 != ',') continue;
            char* p2; double o = std::strtod(p1+1, &p2); if (!p2 || *p2 != ',') continue;
            char* p3; double h = std::strtod(p2+1, &p3); if (!p3 || *p3 != ',') continue;
            char* p4; double l = std::strtod(p3+1, &p4); if (!p4 || *p4 != ',') continue;
            char* p5; double c = std::strtod(p4+1, &p5);
            if (!std::isfinite(o) || !std::isfinite(h) || !std::isfinite(l) || !std::isfinite(c)) continue;
            // Seconds-vs-milliseconds guard (see XauTrendFollow2hEngine warmup: VPS
            // seed_refresh can regenerate warmup CSVs in seconds). Normalise to ms.
            if (ms > 0 && ms < 100000000000LL) ms *= 1000LL;

            XauTfBar bar; bar.bar_start_ms = ms; bar.open = o; bar.high = h; bar.low = l; bar.close = c;
            on_h4_bar(bar, c, c, 0.0, ms + 14400LL*1000, OnCloseFn{});
            ++fed;
        }
        warmup_active_ = false;  // S102: live entries now permitted
        printf("[XauTF4h-WARMUP] fed=%d bars, atr=%.4f bars_size=%d path='%s'\n",
               fed, atr14_, (int)bars_.size(), path.c_str());
        fflush(stdout);
        return fed;
    }

    // S-2026-07-02 KILL-THE-4H-WAIT: append fresh live H4 bars on top of the bundled
    // seed so indicators/price reflect NOW (the bundled CSV ends ~60d stale). Feeds
    // all-but-last with entries blocked (indicator prime only) and stashes the last
    // CLOSED bar for a guarded live evaluation on the first tick (try_boot_fire()).
    // Called from init_engines() -- pos[] is still empty here (restore runs later), so
    // replaying historical bars cannot spuriously touch a resumed position.
    int append_fresh_h4(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) {
            printf("[XauTF4h-FRESH] no live dump '%s' -- bundled seed only (fire-on-boot disarmed)\n", path.c_str());
            fflush(stdout); return 0;
        }
        std::vector<XauTfBar> v;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#' || line[0] == 'b') continue;
            char* p1; long long ms = std::strtoll(line.c_str(), &p1, 10); if (!p1 || *p1 != ',') continue;
            char* p2; double o = std::strtod(p1+1, &p2); if (!p2 || *p2 != ',') continue;
            char* p3; double h = std::strtod(p2+1, &p3); if (!p3 || *p3 != ',') continue;
            char* p4; double l = std::strtod(p3+1, &p4); if (!p4 || *p4 != ',') continue;
            char* p5; double c = std::strtod(p4+1, &p5);
            if (!std::isfinite(o) || !std::isfinite(h) || !std::isfinite(l) || !std::isfinite(c)) continue;
            XauTfBar b; b.bar_start_ms = ms; b.open = o; b.high = h; b.low = l; b.close = c;
            v.push_back(b);
        }
        if (v.empty()) {
            printf("[XauTF4h-FRESH] live dump '%s' empty -- bundled seed only\n", path.c_str());
            fflush(stdout); return 0;
        }
        warmup_active_ = true;                       // indicator-prime only, no entries
        for (size_t i = 0; i + 1 < v.size(); ++i)
            on_h4_bar(v[i], v[i].close, v[i].close, 0.0, v[i].bar_start_ms + 14400LL*1000, OnCloseFn{});
        warmup_active_ = false;
        boot_last_bar_   = v.back();
        has_boot_last_   = true;
        boot_fire_armed_ = true;
        printf("[XauTF4h-FRESH] appended %d fresh H4 bars (last_close=%.2f atr=%.2f bars_size=%d) -- fire-on-boot armed\n",
               (int)v.size(), boot_last_bar_.close, atr14_, (int)bars_.size());
        fflush(stdout);
        return (int)v.size();
    }

    // S-2026-07-02 KILL-THE-4H-WAIT: first-tick guarded fire-on-boot. Evaluates the last
    // CLOSED live H4 bar for entry so a restart doesn't skip a valid signal and idle up to
    // 4h for the next close. One-shot (self-disarms). GUARD: only fire if the live mid is
    // within tol_atr*ATR of that bar's close -- if gold moved sharply since the close the
    // edge is stale, so seed the bar for indicator completeness but do NOT enter.
    void try_boot_fire(double bid, double ask, int64_t now_ms, OnCloseFn cb,
                       double tol_atr = 0.5) noexcept {
        if (!boot_fire_armed_) return;
        boot_fire_armed_ = false;                    // one-shot, whatever the outcome
        if (!has_boot_last_ || atr14_ <= 0.0 || bid <= 0.0 || ask <= 0.0) return;
        const double live_mid = 0.5 * (bid + ask);
        const double gap      = std::fabs(live_mid - boot_last_bar_.close);
        if (gap > tol_atr * atr14_) {
            warmup_active_ = true;                    // seed-only: complete indicators, no entry
            on_h4_bar(boot_last_bar_, boot_last_bar_.close, boot_last_bar_.close, 0.0,
                      boot_last_bar_.bar_start_ms + 14400LL*1000, OnCloseFn{});
            warmup_active_ = false;
            printf("[XTF4H-BOOT] fire-on-boot SKIP: live %.2f vs last_close %.2f gap %.2f > %.2f (%.1fxATR) -- wait next H4 close\n",
                   live_mid, boot_last_bar_.close, gap, tol_atr * atr14_, tol_atr);
            fflush(stdout);
            return;
        }
        // Guard passed -> evaluate the last closed bar live (entries enabled, live
        // bid/ask + cost gate). Cells already holding a resumed position self-skip.
        on_h4_bar(boot_last_bar_, bid, ask, 0.0, now_ms, cb);
        printf("[XTF4H-BOOT] fire-on-boot: live %.2f within %.2f (%.1fxATR) of last_close %.2f -- last bar evaluated for entry\n",
               live_mid, tol_atr * atr14_, tol_atr, boot_last_bar_.close);
        fflush(stdout);
    }
};

} // namespace omega

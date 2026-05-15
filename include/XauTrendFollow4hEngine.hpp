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
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "OmegaTradeLedger.hpp"  // omega::TradeRecord
#include "OmegaCostGuard.hpp"

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
enum class XauTfFamily { Donchian20, InsideBar, ErTrend020, Keltner20, AdxMom20, RangeExpand };
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
};
static constexpr int kXauTfNumCells =
    static_cast<int>(sizeof(kXauTfCells) / sizeof(kXauTfCells[0]));

// =============================================================================
//  XauTrendFollow4hEngine
// =============================================================================
struct XauTrendFollow4hEngine {
public:
    // Public knobs (set by engine_init.hpp before init()).
    bool   shadow_mode = true;
    bool   enabled     = false;
    double lot         = 0.01;
    double max_spread  = 1.0;  // USD; refuse entries above this

    // S96: per-cell enable bitmask. Bit i controls kXauTfCells[i].
    // Default 0x3F = all 6 cells enabled. Set in engine_init.hpp.
    // Backtest v2 showed 4h_InsideBar(1)/4h_ER20(2)/4h_ADX_Mom(4) PF<1.0 →
    // disable those 3 to concentrate on profitable cells.
    uint32_t cell_enable_mask = 0x3F;  // bits 0-5, all on by default

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

    void init() noexcept {
        bars_.clear();
        atr14_ = 0.0;
        atr_warmup_count_ = 0;
        ema20_ = 0.0;
        ema_initialised_ = false;
        adx14_ = 0.0;
        adx_atr_sum_ = adx_pdm_sum_ = adx_mdm_sum_ = adx_dx_sum_ = 0.0;
        adx_warmup_count_ = 0;
        adx_dx_count_ = 0;
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

        // Use external ATR if provided (preferred -- matches the rest of
        // the stack); otherwise compute locally.
        if (atr14_external > 0.0) {
            atr14_ = atr14_external;
        } else {
            _update_local_atr();
        }

        // S33d-ext: update EMA20 (for Keltner) and ADX14 (for AdxMom).
        _update_ema20();
        _update_adx14();

        // Advance cell cooldowns and increment bars_held on open positions.
        for (auto& p : pos) {
            if (p.cooldown_bars > 0) --p.cooldown_bars;
            if (p.active) ++p.bars_held;
        }

        // Decide entries cell-by-cell. We do NOT re-check exits here --
        // exits run intra-bar on every tick via on_tick (so SL/TP can hit
        // within the bar). The bar-close is only for new-entry decisions.
        if ((int)bars_.size() < 32) return;  // warmup
        if (atr14_ <= 0.0) return;
        if (ask - bid > max_spread) return;

        for (int ci = 0; ci < kXauTfNumCells; ++ci) {
            if (!(cell_enable_mask & (1u << ci))) continue;  // S96: per-cell gate
            if (pos[ci].active) continue;
            if (pos[ci].cooldown_bars > 0) continue;
            int side = _evaluate_signal(ci);
            if (side == 0) continue;
            _fire_entry(ci, side, bid, ask, now_ms);
        }
        (void)on_close; // signature parity; we only emit on exit (in on_tick)
    }

    // ============================================================
    //  on_tick -- runs every tick to manage open positions
    // ============================================================
    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseFn on_close) noexcept {
        if (!enabled) return;
        for (int ci = 0; ci < kXauTfNumCells; ++ci) {
            if (!pos[ci].active) continue;
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

    // ---------- Signal evaluators
    int _evaluate_signal(int cell_idx) const noexcept {
        switch (kXauTfCells[cell_idx].family) {
            case XauTfFamily::Donchian20:  return _sig_donchian20();
            case XauTfFamily::InsideBar:   return _sig_inside_bar();
            case XauTfFamily::ErTrend020:  return _sig_er_trend(0.20, 20);
            case XauTfFamily::Keltner20:   return _sig_keltner();
            case XauTfFamily::AdxMom20:    return _sig_adx_momentum(25.0, 20);
            case XauTfFamily::RangeExpand: return _sig_range_expansion(1.5);
        }
        return 0;
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
        const auto& cfg = kXauTfCells[ci];
        double entry = (side > 0) ? ask : bid;
        if (entry <= 0.0 || atr14_ <= 0.0) return;
        double sl_dist = cfg.sl_mult * atr14_;
        double tp_dist = cfg.tp_mult * atr14_;
        double sl_px = (side > 0) ? entry - sl_dist : entry + sl_dist;
        double tp_px = (side > 0) ? entry + tp_dist : entry - tp_dist;

        // 2026-05-12 cost gate -- see outputs/PLAN_A_B_REPORT.md
        {
            const double spread_pts = ask - bid;
            if (!ExecutionCostGuard::is_viable(
                    "XAUUSD", spread_pts, tp_dist, lot, 1.5))
            {
                return;
            }
        }

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
        // S34 P1 fix #5: reset MFE/MAE per new entry.
        p.mfe           = 0.0;
        p.mae           = 0.0;
        p.broker_position_id.clear();
        p.entry_clOrdId.clear();

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
        tr.symbol     = "XAUUSD";
        // S34 P1 fix #3: per-cell engine string so ledger queries grouping
        // by engine can distinguish cells. Cell name is unique per row.
        tr.engine     = std::string("XauTrendFollow4h_") + kXauTfCells[ci].name;
        // S34 P1 fix #4: ledger convention is LONG/SHORT, not BUY/SELL.
        tr.side       = p.is_long ? "LONG" : "SHORT";
        tr.entryPrice = p.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = p.tp_px;
        tr.sl         = p.sl_px;
        tr.size       = lot;
        tr.entryTs    = p.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = kXauTfCells[ci].name;  // cell name -> regime field (unchanged)
        tr.shadow     = shadow_mode;
        // S34 P1 fix #1: assign gross pnl. tick_value_multiplier(symbol) is
        // applied downstream by handle_closed_trade to get USD.
        tr.pnl        = pts_move * lot;
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
};

} // namespace omega

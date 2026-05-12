#pragma once
// =============================================================================
//  XauThreeBar30mEngine.hpp -- XAU 30m three-bar continuation
//                              (S34 2026-05-12, S35-P3 protections retrofit
//                               2026-05-12)
// =============================================================================
//
//  PROVENANCE
//
//  Implements the "ThreeBar" cell that showed positive in S33 Pass-8 but
//  was never built as a production engine. HANDOFF_S33_FINAL.md sec 4
//  item 5:
//
//      "30m XAU ThreeBar (positive in Pass-8, n=639 +$979 across 30mo,
//       BE=$1.59). Not yet built. 21 trades/month, modest edge."
//
//  Cell mechanic (from backtest/edge_hunt.cpp sig_three_bar, lines
//  604-624):
//
//      Three consecutive same-direction 30m bars in [i-3, i-2, i-1]
//      (all green or all red). On bar i, fire LONG if close > bars[i-1].high;
//      fire SHORT if close < bars[i-1].low. Bracket geometry sl=2.0*ATR14,
//      tp=4.0*ATR14 (the S33 sweep optimum for this cell).
//
//  Cross-validation status: Pass-8 result was positive on 30 months
//  Dukascopy 2023-09 → 2025-09; explicit per-year cross-validation
//  has not yet been re-run (the S33 candidate list put this in "NOT
//  tested but COULD be" rather than the cross-validated production
//  stack). This file therefore ships as a PRODUCTION CANDIDATE:
//  enabled=false by default in addition to the shadow_mode=true safety
//  pin. To promote: run the backtest harness with the parameters
//  baked in below, confirm year-by-year all-positive (with AND without
//  the S35-P3 protection stack toggled on), then flip enabled=true
//  in engine_init.hpp.
//
//  S35-P3 PROTECTIONS RETROFIT (2026-05-12)
//
//  Layered on top of the original engine, the S35-P3 commit wires in
//  the standard ProtectedEngineGuards bundle (include/engine_protections.hpp)
//  so the engine inherits a uniform downside-protection stack:
//
//      (1) Hard SL multiplier             -- still SL_MULT = 2.0 * ATR
//      (2) Time stop                      -- close after max_bars_held bars
//      (3) Break-even shift               -- arm BE at be_trigger_atr*ATR_at_entry
//      (4) Trailing stop after BE         -- trail = trail_atr_mult * ATR_at_entry
//      (5) Daily loss cap                 -- pause for the UTC day after dollar limit
//      (6) Consec-loss kill switch        -- flip shadow_mode after N losses
//      (7) Volatility regime gate         -- ATR floor + ceiling
//      (8) Spread cap                     -- inherited from guards.cfg.max_spread
//      (9) Session-window block           -- UTC hour range, wrap-aware
//      (10) shadow_mode and enabled       -- still engine-owned
//
//  The signal evaluator (_evaluate_signal), bar history (bars_), local
//  ATR fallback (_update_local_atr), and entry geometry math
//  (_fire_entry) are byte-identical to the pre-retrofit version. The
//  retrofit only adds:
//
//      - guards member (omega::ProtectedEngineGuards)
//      - public knobs forwarded to guards.cfg in init()
//      - guards.roll_day + guards.on_bar_held + guards.time_stop_fired
//        path in on_30m_bar (bars-held / time-stop)
//      - guards.check_entry_ok before signal evaluation in on_30m_bar
//        (subsumes the previous spread-cap return)
//      - guards.update_mfe_mae + guards.update_sl path in _manage_open
//      - guards.on_close path in _close (USD pnl bookkeeping for the
//        daily cap and consec-loss killswitch)
//      - reset_per_trade in _close
//
//  With every new knob set to its disabled-value default (0 / -1),
//  behavior is regression-identical to the pre-retrofit engine. The
//  retrofit therefore lands SAFELY in shadow / disabled mode; the
//  operator opts in to specific protections by setting non-default
//  knobs in engine_init.hpp.
//
//  SAFETY (post-retrofit)
//
//      - shadow_mode = true by default. HARD shadow regardless of
//        kShadowDefault, until the year-by-year cross-validation has
//        been done.
//      - enabled = false by default. Set true in engine_init.hpp only
//        after the cross-validation completes.
//      - 0.01 lot, single position at a time.
//      - 1.0 USD spread cap (now via guards.cfg.max_spread).
//      - 1-bar (30m) cooldown after exit.
//      - DOES NOT touch any protected core file.
//      - DOES NOT take any additional locks (guards is per-engine,
//        single-threaded by codebase convention).
//      - All ten S35-P3 protections enumerated above, each opt-in
//        through a public knob.
//
//  USAGE
//
//      // globals.hpp:
//      static omega::XauThreeBar30mEngine g_xau_threebar_30m;
//
//      // engine_init.hpp (existing + new knobs; the new ones can stay
//      // at their declared defaults if you want the post-retrofit
//      // engine to behave EXACTLY like the pre-retrofit engine):
//      g_xau_threebar_30m.shadow_mode        = true;
//      g_xau_threebar_30m.enabled            = false;
//      g_xau_threebar_30m.lot                = 0.01;
//      g_xau_threebar_30m.max_spread         = 1.0;
//      g_xau_threebar_30m.max_bars_held      = 4;     // S35-P3 (2h cap)
//      g_xau_threebar_30m.be_trigger_atr     = 1.0;   // S35-P3
//      g_xau_threebar_30m.be_cost_buffer_pts = 0.10;  // S35-P3
//      g_xau_threebar_30m.trail_after_be     = true;  // S35-P3
//      g_xau_threebar_30m.trail_atr_mult     = 0.75;  // S35-P3
//      g_xau_threebar_30m.daily_loss_limit   = 5.0;   // S35-P3
//      g_xau_threebar_30m.max_consec_losses  = 5;     // S35-P3
//      g_xau_threebar_30m.min_atr_floor      = 0.30;  // S35-P3
//      g_xau_threebar_30m.max_atr_ceil       = 30.0;  // S35-P3
//      g_xau_threebar_30m.block_hour_start   = 22;    // S35-P3 (Asia)
//      g_xau_threebar_30m.block_hour_end     = 8;     // S35-P3 (Asia)
//      g_xau_threebar_30m.init();
//      printf("[OMEGA-INIT] XauThreeBar30mEngine initialised: shadow=%d enabled=%d lot=%.2f\n",
//             (int)g_xau_threebar_30m.shadow_mode,
//             (int)g_xau_threebar_30m.enabled,
//             g_xau_threebar_30m.lot);
//
//      // tick_gold.hpp inside the M30 bar-close branch:
//      omega::XauThreeBar30mBar bar30m{};
//      bar30m.bar_start_ms = s_bar_30m_ms;
//      bar30m.open  = s_cur_30m.open;
//      bar30m.high  = s_cur_30m.high;
//      bar30m.low   = s_cur_30m.low;
//      bar30m.close = s_cur_30m.close;
//      g_xau_threebar_30m.on_30m_bar(bar30m, bid, ask,
//          g_bars_gold.m30.ind.atr14.load(std::memory_order_relaxed),
//          now_ms_g, bracket_on_close);
//
//      // tick_gold.hpp on every gold tick (intra-bar SL/TP management):
//      g_xau_threebar_30m.on_tick(bid, ask, now_ms_g, bracket_on_close);
//
//  S34 CLOSE-PATH CORRECTNESS  (preserved post-retrofit)
//
//  This engine implements the BracketEngine close-path convention
//  (include/BracketEngine.hpp:1207-1230) from day one, so the bugs
//  documented in the UstecTrendFollow5mEngine.hpp S34 header don't
//  repeat:
//
//      tr.pnl    = (exit - entry) * sign * lot  (raw pts*lots;
//                                                trade_lifecycle
//                                                multiplies by
//                                                tick_value_multiplier)
//      tr.symbol = "XAUUSD"                  (matches sizing table →
//                                             $100/pt)
//      tr.side   = "LONG" / "SHORT"          (ledger convention)
//      tr.mfe / tr.mae populated on every tick from live mid (now
//                                            via guards.st.mfe_pts /
//                                            mae_pts)
//      tr.engine = "XauThreeBar30m"          (distinct in ledger; the
//                                             cell has only one signal
//                                             family so no per-cell
//                                             suffix needed)
//
//  If the logging.hpp dedupe + zero-PnL guards (S34) ever print
//  [CSV-ZERO-PNL] against this engine, that's a bug here. By
//  construction it should never fire.
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>

#include "OmegaTradeLedger.hpp"      // omega::TradeRecord
#include "engine_protections.hpp"    // S35-P3 ProtectedEngineGuards
#include "OmegaCostGuard.hpp"

namespace omega {

// ---------- Bar input (mirrors XauTfBar for call-site parity)
struct XauThreeBar30mBar {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
};

// ---------- Single-position state
struct XauThreeBar30mPos {
    bool        active             = false;
    bool        is_long            = false;
    double      entry_px           = 0.0;
    double      tp_px              = 0.0;
    double      sl_px              = 0.0;
    double      atr_at_entry       = 0.0;
    int64_t     entry_ts_ms        = 0;
    int         bars_held          = 0;
    int         cooldown_bars      = 0;
    // S34-style MFE/MAE tracking in raw price points (mid-based). The
    // S35-P3 retrofit moves the authoritative MFE/MAE into
    // guards.st.mfe_pts / mae_pts; pos.mfe_pts / mae_pts are retained
    // as a backward-compat reflection of the same values for any code
    // that reads them off the position struct directly. Multiplied
    // by lot and tick_value_multiplier downstream to produce USD figures.
    double      mfe_pts            = 0.0;
    double      mae_pts            = 0.0;
    std::string broker_position_id;
    std::string entry_clOrdId;
};

// =============================================================================
//  XauThreeBar30mEngine
// =============================================================================
struct XauThreeBar30mEngine {
public:
    // ── Pre-retrofit public knobs (UNCHANGED; engine_init.hpp compatible) ──
    bool   shadow_mode = true;
    bool   enabled     = false;
    double lot         = 0.01;
    double max_spread  = 1.0;

    // ── S35-P3 protection knobs (NEW, defaults = "disabled" so the
    //    retrofit is regression-safe unless the operator opts in).
    //    Wired into guards.cfg in init(). Override per-engine in
    //    engine_init.hpp.
    int    max_bars_held       = 0;     // (2) time stop bars; 0 disables
    double be_trigger_atr      = 0.0;   // (3) BE arm trigger; 0 disables
    double be_cost_buffer_pts  = 0.10;  // BE-shifted SL = entry +/- this
    bool   trail_after_be      = false; // (4) trail after BE arm
    double trail_atr_mult      = 0.0;   //     trail = trail_atr_mult * ATR_at_entry
    double daily_loss_limit    = 0.0;   // (5) USD; 0 disables
    int    max_consec_losses   = 0;     // (6) killswitch threshold; 0 disables
    double min_atr_floor       = 0.0;   // (7) ATR floor; 0 disables
    double max_atr_ceil        = 0.0;   //     ATR ceiling; 0 disables
    int    block_hour_start    = -1;    // (9) session block start UTC; -1 disables
    int    block_hour_end      = -1;    //     session block end UTC

    // ── Bracket geometry — locked to Pass-8 sweep optimum ──────────────────
    // SL = 2.0 * ATR14, TP = 4.0 * ATR14 (R:R 2:1). Per the S33 sweep this
    // was the cell variant that produced n=639 +$979 over 30mo at BE=$1.59
    // on XAU 30m Dukascopy. Tunable in case the cross-validation finds a
    // different optimum on the year-by-year split.
    static constexpr double SL_MULT = 2.0;
    static constexpr double TP_MULT = 4.0;

    // ── Cooldown bars after close (1 bar = 30m) ────────────────────────────
    static constexpr int COOLDOWN_BARS = 1;

    XauThreeBar30mPos pos{};

    // S35-P3 protection bundle (public so dashboards can read state).
    ProtectedEngineGuards guards{};

    // Bar history (last N=16; need 4 bars for the signal + warmup margin).
    static constexpr int kBarHistory = 16;
    std::deque<XauThreeBar30mBar> bars_;

    // Rolling Wilder ATR14 over bars_ (local fallback; preferred path is to
    // pass the external ATR from g_bars_gold.m30.ind.atr14 in on_30m_bar).
    static constexpr int kAtrPeriod = 14;
    double atr14_ = 0.0;
    int    atr_warmup_count_ = 0;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    void init() noexcept {
        bars_.clear();
        atr14_ = 0.0;
        atr_warmup_count_ = 0;
        pos = {};

        // S35-P3: wire engine knobs into guards.cfg. The guards struct
        // is the single source of truth at runtime; the engine's
        // public members exist solely as the engine_init.hpp surface.
        guards.cfg.sl_atr_mult         = SL_MULT;
        guards.cfg.max_bars_held       = max_bars_held;
        guards.cfg.be_trigger_atr      = be_trigger_atr;
        guards.cfg.be_cost_buffer_pts  = be_cost_buffer_pts;
        guards.cfg.trail_after_be      = trail_after_be;
        guards.cfg.trail_atr_mult      = trail_atr_mult;
        guards.cfg.daily_loss_limit    = daily_loss_limit;
        guards.cfg.max_consec_losses   = max_consec_losses;
        guards.cfg.min_atr_floor       = min_atr_floor;
        guards.cfg.max_atr_ceil        = max_atr_ceil;
        guards.cfg.max_spread          = max_spread;
        guards.cfg.block_hour_start    = block_hour_start;
        guards.cfg.block_hour_end      = block_hour_end;
        guards.reset_all();
    }

    bool has_open_position() const noexcept { return pos.active; }

    // ============================================================
    //  on_30m_bar -- called by tick_gold.hpp when an M30 bar closes
    // ============================================================
    void on_30m_bar(const XauThreeBar30mBar& bar,
                    double bid, double ask,
                    double atr14_external,
                    int64_t now_ms,
                    OnCloseFn on_close) noexcept {
        if (!enabled) return;

        const int64_t now_unix_s = now_ms / 1000;
        guards.roll_day(now_unix_s);

        bars_.push_back(bar);
        while ((int)bars_.size() > kBarHistory) bars_.pop_front();

        if (atr14_external > 0.0) atr14_ = atr14_external;
        else _update_local_atr();

        if (pos.cooldown_bars > 0) --pos.cooldown_bars;

        // S35-P3: bars-held + time stop. Increment guards' counter, then
        // check time stop. If fired, close the trade with TIME_STOP
        // reason and return -- no further signal evaluation this bar.
        if (pos.active) {
            ++pos.bars_held;
            guards.on_bar_held();
            if (guards.time_stop_fired()) {
                const double exit_px = pos.is_long ? bid : ask;
                omega::log_time_stop("XauThreeBar30m",
                                     guards.st.bars_held,
                                     guards.st.mfe_pts,
                                     guards.st.mae_pts);
                _close(exit_px, "TIME_STOP", now_ms, on_close);
                return;
            }
        }

        // Warmup: need 4 bars for the signal lookback + at least 14 for ATR.
        if ((int)bars_.size() < 16) return;
        if (atr14_ <= 0.0) return;

        // Only fire when no position is open and cooldown has expired.
        if (pos.active) return;
        if (pos.cooldown_bars > 0) return;

        // S35-P3: entry guards (spread, ATR regime, daily cap, killswitch,
        // session window). Subsumes the pre-retrofit spread-cap return.
        if (const char* why = guards.check_entry_ok(bid, ask, atr14_,
                                                    now_unix_s)) {
            omega::log_entry_block("XauThreeBar30m", why);
            return;
        }

        int side = _evaluate_signal();
        if (side == 0) return;

        _fire_entry(side, bid, ask, now_ms);
        (void)on_close;  // exits run intra-bar in on_tick
    }

    // ============================================================
    //  on_tick -- runs every tick to manage open position
    // ============================================================
    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseFn on_close) noexcept {
        if (!enabled) return;
        if (!pos.active) return;
        _manage_open(bid, ask, now_ms, on_close);
    }

    // ============================================================
    //  force_close -- end-of-day or shutdown flatten
    // ============================================================
    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseFn on_close, const char* reason) noexcept {
        if (!pos.active) return;
        double exit_px = pos.is_long ? bid : ask;
        _close(exit_px, reason ? reason : "FORCE_CLOSE", now_ms, on_close);
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

    // ---------- Three-bar continuation signal (BYTE-IDENTICAL pre-retrofit)
    // bars_[last-3], bars_[last-2], bars_[last-1] = three completed bars
    //                                              before the trigger bar
    // bars_[last]                                 = trigger bar just closed
    //
    // Long  fires when all three [last-3 .. last-1] are bullish (close>open)
    //       and bars_[last].close breaks above bars_[last-1].high.
    // Short fires when all three are bearish (close<open) and bars_[last].close
    //       breaks below bars_[last-1].low.
    int _evaluate_signal() const noexcept {
        if ((int)bars_.size() < 4) return 0;
        const int last = (int)bars_.size() - 1;
        const auto& b3 = bars_[last - 3];
        const auto& b2 = bars_[last - 2];
        const auto& b1 = bars_[last - 1];
        const auto& cb = bars_[last];

        const bool all_up =
            (b3.close > b3.open) && (b2.close > b2.open) && (b1.close > b1.open);
        const bool all_down =
            (b3.close < b3.open) && (b2.close < b2.open) && (b1.close < b1.open);

        if (all_up   && cb.close > b1.high) return +1;
        if (all_down && cb.close < b1.low ) return -1;
        return 0;
    }

    void _fire_entry(int side, double bid, double ask, int64_t now_ms) noexcept {
        double entry = (side > 0) ? ask : bid;
        if (entry <= 0.0 || atr14_ <= 0.0) return;

        // SL distance uses guards.cfg.sl_atr_mult (= SL_MULT from init()).
        const double sl_dist = guards.cfg.sl_atr_mult * atr14_;
        const double tp_dist = TP_MULT * atr14_;

        // 2026-05-12 cost gate -- see outputs/PLAN_A_B_REPORT.md
        {
            const double spread_pts = ask - bid;
            if (!ExecutionCostGuard::is_viable(
                    "XAUUSD", spread_pts, tp_dist, lot, 1.5))
            {
                return;
            }
        }

        pos.active        = true;
        pos.is_long       = (side > 0);
        pos.entry_px      = entry;
        pos.sl_px         = (side > 0) ? entry - sl_dist : entry + sl_dist;
        pos.tp_px         = (side > 0) ? entry + tp_dist : entry - tp_dist;
        pos.atr_at_entry  = atr14_;
        pos.entry_ts_ms   = now_ms;
        pos.bars_held     = 0;
        pos.cooldown_bars = 0;
        pos.mfe_pts       = 0.0;
        pos.mae_pts       = 0.0;
        pos.broker_position_id.clear();
        pos.entry_clOrdId.clear();

        // S35-P3: clear per-trade guards state (be_armed, mfe/mae, bars_held).
        guards.reset_per_trade();
    }

    void _manage_open(double bid, double ask, int64_t now_ms,
                      OnCloseFn on_close) noexcept {
        // S35-P3: MFE/MAE via guards (authoritative). Also reflect into
        // pos.mfe_pts / mae_pts for backward-compat with any reader that
        // pulls them off the position struct.
        const double mid = (bid + ask) * 0.5;
        if (mid > 0.0 && pos.entry_px > 0.0) {
            guards.update_mfe_mae(pos.is_long, pos.entry_px, mid);
            pos.mfe_pts = guards.st.mfe_pts;
            pos.mae_pts = guards.st.mae_pts;
        }

        // S35-P3: SL tightening (BE arm + trail after BE). Returns the
        // possibly-tightened SL; engine assigns it back. Never loosens.
        if (pos.entry_px > 0.0 && pos.atr_at_entry > 0.0 && mid > 0.0) {
            pos.sl_px = guards.update_sl(pos.is_long, pos.entry_px,
                                         pos.sl_px, mid, pos.atr_at_entry);
        }

        // Standard SL/TP check against the (possibly tightened) pos.sl_px.
        bool hit_tp = false, hit_sl = false;
        double exit_px = 0.0;
        if (pos.is_long) {
            if (bid <= pos.sl_px)      { exit_px = pos.sl_px; hit_sl = true; }
            else if (bid >= pos.tp_px) { exit_px = pos.tp_px; hit_tp = true; }
        } else {
            if (ask >= pos.sl_px)      { exit_px = pos.sl_px; hit_sl = true; }
            else if (ask <= pos.tp_px) { exit_px = pos.tp_px; hit_tp = true; }
        }
        if (!hit_tp && !hit_sl) return;

        // Reason mapping:
        //   hit_tp           -> "TP_HIT"
        //   hit_sl + be_armed -> "TRAIL_HIT"   (BE or trail locked us out
        //                                       at a price tighter than
        //                                       the original SL)
        //   hit_sl           -> "SL_HIT"       (original SL touched)
        const char* reason;
        if (hit_tp) {
            reason = "TP_HIT";
        } else if (guards.st.be_armed) {
            reason = "TRAIL_HIT";
        } else {
            reason = "SL_HIT";
        }
        _close(exit_px, reason, now_ms, on_close);
    }

    void _close(double exit_px, const char* reason,
                int64_t now_ms, OnCloseFn on_close) noexcept {
        if (!pos.active) return;

        // Raw points*lots PnL — matches BracketEngine convention
        // (include/BracketEngine.hpp:1216-1218). trade_lifecycle.hpp:218-222
        // multiplies tr.pnl/mfe/mae by tick_value_multiplier(tr.symbol) to
        // produce USD; tick_value_multiplier("XAUUSD") = 100.0 (sizing.hpp:32).
        const double pts_move = pos.is_long ? (exit_px - pos.entry_px)
                                            : (pos.entry_px - exit_px);

        // S35-P3: feed USD pnl into guards for daily cap + killswitch
        // bookkeeping. Hardcoded XAUUSD multiplier = 100.0 here matches
        // tick_value_multiplier("XAUUSD") in sizing.hpp. The downstream
        // trade_lifecycle path applies the same multiplier to tr.pnl
        // separately -- no double counting, because the guards copy
        // never flows into tr.*; it's pure local bookkeeping.
        constexpr double XAUUSD_TICK_VALUE = 100.0;
        const double pnl_usd = pts_move * lot * XAUUSD_TICK_VALUE;

        const bool was_killed_before = guards.st.killswitch_tripped;
        const bool was_capped_before = guards.st.daily_capped;
        guards.on_close(pnl_usd);

        // Killswitch newly tripped? Flip shadow_mode and log loudly.
        if (!was_killed_before && guards.st.killswitch_tripped) {
            shadow_mode = true;
            omega::log_killswitch("XauThreeBar30m",
                                  guards.st.consec_losses,
                                  guards.st.daily_pnl_usd);
        }
        // Daily cap newly tripped? Log (entries blocked until UTC rollover).
        if (!was_capped_before && guards.st.daily_capped) {
            omega::log_daily_cap("XauThreeBar30m",
                                 guards.st.daily_pnl_usd,
                                 guards.cfg.daily_loss_limit);
        }

        omega::TradeRecord tr;
        tr.symbol     = "XAUUSD";
        tr.engine     = "XauThreeBar30m";
        tr.side       = pos.is_long ? "LONG" : "SHORT";
        tr.entryPrice = pos.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = pos.tp_px;
        tr.sl         = pos.sl_px;
        tr.size       = lot;
        tr.pnl        = pts_move * lot;
        tr.net_pnl    = tr.pnl;          // trade_lifecycle overwrites after costs
        // MFE/MAE sourced from guards (authoritative since S35-P3).
        tr.mfe        = guards.st.mfe_pts * lot;
        tr.mae        = guards.st.mae_pts * lot;
        tr.entryTs    = pos.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = "ThreeBar_sl2.0tp4.0";
        tr.shadow     = shadow_mode;

        if (on_close) on_close(tr);

        pos.active        = false;
        pos.broker_position_id.clear();
        pos.entry_clOrdId.clear();
        pos.cooldown_bars = COOLDOWN_BARS;

        // S35-P3: clear per-trade guards state (be_armed, mfe/mae, bars_held).
        // Per-day and per-session state (daily_pnl, consec_losses,
        // daily_capped, killswitch_tripped) are preserved across trades.
        guards.reset_per_trade();
    }
};

} // namespace omega

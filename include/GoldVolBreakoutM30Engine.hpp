#pragma once
//  ADVERSE-PROTECTION: trail + trend-gate by design (backtested). In-flight
//    protection = structural vol/ATR stop + wide runner trail; gold_volbrk_m30_
//    revalidate.cpp + mgc_engines_audit.cpp faithful (PF1.71-1.94, both-WF-halves+,
//    2x-cost robust). TREND-GATED (trend_==1 only) = entry-side bear protection by
//    design. Cold loss-cut NOT added -- trail-only runner, tightening lowers net
//    (swing-protection sweep 2026-06-17).
//    S-2026-07-11 PHASE 1b: MGC (bar-fed) instance verdict -- stop_mode=2
//    (sl+trail enforced as a bar-path resting stop): mgc_volbrk_tickstop_
//    decision.cpp, certified MGC 30m 2024-26: PF2.07 +281pt worst-31 maxDD79
//    vs no-stop PF1.41 worst-82 maxDD197; 2x-cost PF2.00; both halves +.
// =============================================================================
//  GoldVolBreakoutM30Engine.hpp -- XAU M30 long-only volatility-breakout runner
// =============================================================================
//
//  PROVENANCE
//
//  Built 2026-06-03 from the XauVolBreakout external-package audit + full
//  lever sweep (tools in /tmp/xauvb, harness XauVB_v3.cpp grid4/5/6). The
//  shipped "engine" was structurally broken (silent tick-parser collapse +
//  permanent consec-loss latch) and the as-specified exits lost (PF 0.21:
//  a +$0.10 breakeven lock amputated every winner). The audit rebuilt it and
//  swept every lever on 2yr XAUUSD tick data (2024-03 -> 2026-04, 154M ticks):
//
//    config "beoff": M30 entry, H1 EMA200+slope trend, long-only, strict
//      impulse breakout, NO take-profit, ATR runner trail, BREAKEVEN OFF.
//        2yr: 43 trades  WR 46.5%  PF 2.41  payoff 2.77  net +31107 pts
//        ROBUST: both halves + (2024 PF 2.51 / 2025-26 PF 2.36)
//        COST-INSENSITIVE: PF 2.41 at 0.10 / 0.37 / 0.60 pt roundtrip
//
//    Lever findings (1-D sweep around the optimum):
//      - BREAKEVEN OFF was the single biggest lever (PF 1.35 -> 2.41). The
//        V2-style "+0.10R lock after 1.10R" flattened runners prematurely.
//      - Wide INITIAL stop (2.5-3.0 ATR) helps ONLY with BE on; with BE off it
//        merely enlarges losers -> keep the 1.5 ATR stop.
//      - Session filter is load-bearing (sess-off PF 0.76). London/NY only.
//      - Impulse RANGE filter is the entry edge (raw Donchian breakout had
//        MFE/MAE only 1.14); close-position 0.70 is enough.
//      - No breakout buffer; Donchian-20; M30 beats M5/M15.
//
//  CAVEAT (honest): PF 2.41 is fat-tail dependent (top trade ~40% of net,
//  top-3 ~83%) -- inherent to trend-following, high variance. 43 trades is
//  thin. 2024-26 was a gold BULL; long-only is untested in a bear/range.
//  -> SHADOW ONLY. Forward-log; do not size up until the shadow ledger and an
//  older (bear-inclusive) dataset confirm it.
//
//  SIGNAL (single cell, long-only)
//    trend  : last closed H1 close > H1 EMA200  AND  EMA200 rising vs 5 H1 ago
//    entry  : M30 close > Donchian-20 high (prior 20 M30 bars, no buffer)
//             AND not over-extended: close <= DonchHigh + 1.0*ATR14(M30)
//             AND impulse: (close-low)/(high-low) >= 0.70  AND range >= 2.0*ATR
//             AND UTC hour in [7,20)  (London/NY)
//    stop   : entry - 1.5*ATR14(M30)
//    trail  : after +1.55R profit, sl = max(sl, close - 3.0*ATR_at_entry)
//    exit   : ATR runner stop/trail, or max-hold 72 M30 bars. NO take-profit.
//
//  SAFETY (mirrors Ger40KeltnerH1Engine)
//    - shadow_mode = true by default; engine_init.hpp sets the live value.
//    - touches NO protected core file.
//    - 0.01 lot; single concurrent position; cooldown 3 bars after a close.
//    - long-only; broker-aware fill (long on ask, exit on bid).
//    - ExecutionCostGuard::is_viable() gates every entry.
//    - IndexRiskGate (portfolio VIX risk-off) blocks new entries.
//    - warm-seed: H1 EMA200 needs >=206 H1 bars; M30 Donchian/ATR need >=35
//      M30 bars. seed_h1_from_csv() + seed_m30_from_csv() prime both before
//      the first live tick (warm-seed mandate).
//
//  USAGE
//      // globals.hpp:    static omega::GoldVolBreakoutM30Engine g_gold_volbrk_m30;
//      // engine_init.hpp:
//      g_gold_volbrk_m30.shadow_mode = kShadowDefault;
//      g_gold_volbrk_m30.enabled     = true;
//      g_gold_volbrk_m30.lot         = 0.01;
//      g_gold_volbrk_m30.max_spread  = 0.80;  // gold price ($)
//      g_gold_volbrk_m30.init();
//      g_gold_volbrk_m30.seed_h1_from_csv ("phase1/signal_discovery/warmup_XAUUSD_H1.csv");
//      g_gold_volbrk_m30.seed_m30_from_csv("phase1/signal_discovery/warmup_XAUUSD_M30.csv");
//      // tick_gold.hpp  H1 close:  g_gold_volbrk_m30.on_h1_close(s_cur_h1.close);
//      //                M30 close: g_gold_volbrk_m30.on_m30_bar(h,l,c,bid,ask,now_ms,cb);
//      //                every tick: g_gold_volbrk_m30.on_tick(bid,ask,now_ms,cb);
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include "AuroraGate.hpp"
#include <deque>
#include <fstream>
#include <functional>
#include <string>

#include <ctime>

#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"
#include "IndexRiskGate.hpp"
#include "OpenPositionRegistry.hpp"   // omega::PositionSnapshot (persist; S-2026-07-11)

namespace omega {

struct GvbM30Bar { double high=0.0, low=0.0, close=0.0; };

struct GvbPos {
    bool    active       = false;
    double  entry_px     = 0.0;
    double  sl_px        = 0.0;
    double  atr_at_entry = 0.0;
    int64_t entry_ts_ms  = 0;
    int     bars_held    = 0;
    int     cooldown_bars= 0;
    double  mfe = 0.0, mae = 0.0;
};

struct GoldVolBreakoutM30Engine {
public:
    bool   shadow_mode = true;
    bool   enabled     = false;
    double lot         = 0.01;
    double max_spread  = 0.80;   // gold $ (~80 pts) -- entries refused above

    // S-2026-07-11 GOLD PHASE 1 (venue identity, GOLD_BOOK_ROADMAP bug 2): this
    // class runs as TWO instances -- XAU spot (g_gold_volbrk_m30, tick_gold feed)
    // and MGC futures (g_mgc_volbrk, poll_mgc_feed). The MGC instance used to
    // report tr.symbol="XAUUSD" + lot=0.01 + the SAME engine tag as spot: wrong
    // venue (ledger scaled $1/pt instead of $10/pt/contract), non-tradeable
    // fractional futures size, and mixed spot/MGC ledger attribution under one
    // tag. Parameterized: ledger/execution symbol (drives the downstream
    // tick_value_multiplier AND the ExecutionCostGuard cost row), engine tag,
    // and integer-contract min/step for futures instances.
    std::string ledger_symbol = "XAUUSD";   // spot default; MGC instance sets "MGC"
    std::string engine_tag    = "GoldVolBreakoutM30_imp2.0_stop1.5_trail3.0_BEoff_S-2026-06-03";
    double min_qty  = 0.0;   // futures: 1 (whole contracts). 0 = no snap (spot lots)
    double qty_step = 0.0;   // futures: 1. 0 = no snap

    // S-2026-07-11 GOLD PHASE 1b (GOLD_BOOK_ROADMAP #6 decision test): stop
    // enforcement style. The SPOT instance receives real ticks (tick_gold.hpp
    // drives on_tick) -> STOP_TICK, unchanged behavior. The MGC instance is
    // BAR-FED (poll_mgc_feed 30m rows; on_tick never runs), so its tick-style
    // sl/trail was DEAD CODE that looked active: the cadence audit found 46/46
    // exits via MAX_HOLD. Decision test on the REAL engine, all modes
    // (backtest/mgc_volbrk_tickstop_decision.cpp, certified MGC 30m
    // 2024-06..2026-06 + XAU-M30 2022-bear shadow at MGC cost): see harness
    // output in outputs/GOLD_PHASE1B_2026-07-11.md. Modes:
    //   STOP_NONE         0: honest no-stop -- pos.sl_px=0, NO trail bookkeeping;
    //                        exits = MAX_HOLD only (what the MGC instance was
    //                        ACTUALLY doing, minus the pretend sl/trail state).
    //   STOP_TICK         1: on_tick enforces sl + trail (spot default).
    //   STOP_BAR_INTRABAR 2: on_m30_bar enforces the SAME sl/trail as a resting
    //                        stop, adverse-first, gap-honest fill min(open, sl).
    //   STOP_CATASTROPHE  3: on_m30_bar enforces ONLY a wide emergency stop at
    //                        entry - cat_atr_mult*ATR_entry (gap-honest); the
    //                        1.5-ATR stop/trail are NOT enforced; MAX_HOLD keeps.
    int    stop_mode    = 1;     // spot default: tick-driven (unchanged)
    double cat_atr_mult = 6.0;   // STOP_CATASTROPHE distance (ATR at entry)

    // Validated "beoff" params (do NOT tune live; these ARE the deploy config)
    int    kDonch       = 20;    // Donchian lookback (M30 bars)
    int    kAtrP        = 14;    // ATR period (M30 bars)
    int    kEmaH1       = 200;   // H1 EMA trend period
    int    kSlopeLB     = 5;     // H1 EMA slope lookback (bars)
    double kStopAtr     = 1.50;  // initial stop = entry - kStopAtr*ATR
    double kTrailAtr    = 3.00;  // runner trail = close - kTrailAtr*ATR_entry
    double kTrailAfterR = 1.55;  // start trailing after +kTrailAfterR * R
    double kImpPos      = 0.70;  // breakout-bar close in top this fraction of range
    double kImpRange    = 2.00;  // breakout-bar range >= kImpRange*ATR
    double kLateAtr     = 1.00;  // skip if close > DonchHigh + kLateAtr*ATR (extended)
    int    kSessStart   = 7;     // UTC session [start,end)
    int    kSessEnd     = 20;
    int    kMaxHold     = 72;    // M30 bars (36h)
    int    kCooldown    = 3;     // M30 bars after a close

    GvbPos pos{};

    // ---- M30 entry state ----
    static constexpr int kBarHistory = 64;     // >= kDonch + kAtrP warmup
    std::deque<GvbM30Bar> m30_;
    double atr_ = 0.0;
    int    atr_warmup_ = 0;

    // ---- H1 trend state ----
    std::deque<double> h1_close_;              // recent H1 closes (for slope)
    static constexpr int kH1History = 16;      // slope lookback + slack
    double ema_h1_ = 0.0;
    bool   ema_h1_init_ = false;
    int    h1_count_ = 0;
    int    trend_ = 0;                          // +1 up, 0 none (long-only)

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;
    bool warmup_active_ = false;

    void init() noexcept {
        m30_.clear(); atr_ = 0.0; atr_warmup_ = 0;
        h1_close_.clear(); ema_h1_ = 0.0; ema_h1_init_ = false; h1_count_ = 0; trend_ = 0;
        warmup_active_ = false;
        pos = {};
        // S-2026-07-11: futures instances trade whole contracts -- snap lot to
        // qty_step and enforce min_qty (spot instances leave both at 0 = no-op).
        if (qty_step > 0.0) {
            const double snapped = std::max(min_qty, std::floor(lot / qty_step + 0.5) * qty_step);
            if (snapped != lot) {
                std::printf("[GoldVolBrkM30] lot %.4f snapped to %.4f (%s min_qty=%.2f step=%.2f)\n",
                            lot, snapped, ledger_symbol.c_str(), min_qty, qty_step);
                std::fflush(stdout);
            }
            lot = snapped;
        } else if (min_qty > 0.0 && lot < min_qty) {
            lot = min_qty;
        }
    }

    bool any_open() const noexcept { return pos.active; }

    // S-2026-06-23 L2 confirmation gate (forward-validating). Live MGC L2 imbalance
    // pushed in via set_l2_imb each poll; block a long entry when the book is
    // strongly ask-heavy (l2_imb_ < l2_gate_). l2_gate_=0 -> OFF. Backtests never
    // call set_l2_imb -> l2_imb_ stays 0.5 -> gate inert (PF2.10 reproduces).
    double l2_imb_ = 0.5; double l2_gate_ = 0.0;
    void   set_l2_imb(double x) noexcept { l2_imb_ = x; }

    // -------- H1 close: update EMA200 + slope-confirmed long trend --------
    void on_h1_close(double h1_close) noexcept {
        if (h1_close <= 0.0) return;
        if (!ema_h1_init_) { ema_h1_ = h1_close; ema_h1_init_ = true; }
        else { const double a = 2.0 / (kEmaH1 + 1); ema_h1_ = a * h1_close + (1.0 - a) * ema_h1_; }
        ++h1_count_;
        h1_close_.push_back(h1_close);
        while ((int)h1_close_.size() > kH1History) h1_close_.pop_front();

        trend_ = 0;
        if (h1_count_ < kEmaH1 + kSlopeLB) return;     // EMA not yet meaningful
        // slope proxy: EMA now vs EMA ~kSlopeLB bars ago is unavailable directly;
        // use H1 close kSlopeLB bars ago vs now as the rising proxy alongside the
        // close>EMA gate (matches harness trend_direction_h1_slope intent: an
        // up-trend that is still advancing). EMA-rising is implied by price
        // leading EMA while the lookback close is below the latest close.
        if ((int)h1_close_.size() <= kSlopeLB) return;
        const double c_now = h1_close_.back();
        const double c_lb  = h1_close_[h1_close_.size() - 1 - kSlopeLB];
        if (c_now > ema_h1_ && c_now > c_lb) trend_ = 1;
    }

    // -------- M30 close: manage stop/trail/hold then evaluate entry --------
    // S-2026-07-11 PHASE 1b: trailing `open` param (0 = unknown) feeds the
    // gap-honest resting-stop fill for STOP_BAR_INTRABAR / STOP_CATASTROPHE.
    // Existing callers (tick_gold.hpp spot path) compile unchanged.
    void on_m30_bar(double high, double low, double close,
                    double bid, double ask, int64_t now_ms, OnCloseFn on_close,
                    double open = 0.0) noexcept {
        if (!enabled) return;

        // Donchian high of the PRIOR kDonch bars (exclude the bar just closed).
        double donch_high = 0.0; bool donch_ready = false;
        if ((int)m30_.size() >= kDonch) {
            donch_ready = true;
            donch_high = m30_[m30_.size() - kDonch].high;
            for (int i = (int)m30_.size() - kDonch; i < (int)m30_.size(); ++i)
                donch_high = std::max(donch_high, m30_[i].high);
        }

        // push current bar + update ATR (Wilder) over M30 bars
        GvbM30Bar cur{high, low, close};
        double prev_close = m30_.empty() ? close : m30_.back().close;
        m30_.push_back(cur);
        while ((int)m30_.size() > kBarHistory) m30_.pop_front();
        {
            const double tr = std::max(high - low,
                                       std::max(std::fabs(high - prev_close),
                                                std::fabs(low  - prev_close)));
            if (atr_warmup_ < kAtrP) { atr_ = (atr_ * atr_warmup_ + tr) / (atr_warmup_ + 1); ++atr_warmup_; }
            else { atr_ = (atr_ * (kAtrP - 1) + tr) / kAtrP; }
        }

        if (pos.cooldown_bars > 0) --pos.cooldown_bars;

        // ---- manage open: stop (mode 2/3) -> trail -> max-hold ----
        if (pos.active) {
            ++pos.bars_held;
            // bar-extreme excursion tracking (bar-fed instances get no on_tick)
            { const double fav = high - pos.entry_px; if (fav > pos.mfe) pos.mfe = fav;
              const double adv = pos.entry_px - low;  if (adv > pos.mae) pos.mae = adv; }
            // S-2026-07-11 PHASE 1b: resting-stop enforcement for BAR-FED
            // instances, ADVERSE-FIRST (checked against the stop level set on
            // PRIOR bars, before this bar's trail update). Gap-honest fill:
            // min(open, stop) when open is known, else the stop level.
            if (stop_mode == 2 || stop_mode == 3) {
                const double stop_px = pos.sl_px;   // mode 3 set sl_px = cat level at entry
                if (stop_px > 0.0 && low <= stop_px) {
                    const double fill = (open > 0.0) ? std::min(open, stop_px) : stop_px;
                    _close(fill, stop_mode == 3 ? "CATASTROPHE_STOP" : "STOP_OR_TRAIL", now_ms, on_close);
                    return;
                }
            }
            // trail bookkeeping only where a stop is actually enforced (mode 1/2).
            // mode 0 = honest no-stop (MAX_HOLD only); mode 3 = static emergency level.
            if (stop_mode == 1 || stop_mode == 2) {
                const double R = kStopAtr * pos.atr_at_entry;
                if (R > 0.0 && (close - pos.entry_px) >= kTrailAfterR * R) {
                    const double trail = close - kTrailAtr * pos.atr_at_entry;
                    if (trail > pos.sl_px) pos.sl_px = trail;
                }
            }
            if (pos.bars_held >= kMaxHold) { _close(close, "MAX_HOLD", now_ms, on_close); return; }
            return;   // never enter on a bar that holds a position
        }

        // ---- entry gates ----
        if (!donch_ready || atr_ <= 0.0) return;
        if (trend_ != 1) return;
        if (pos.cooldown_bars > 0) return;
        if (!_in_session(now_ms)) return;
        if (ask - bid > max_spread) return;

        const double rng = high - low;
        const bool impulse = rng > 0.0 && (close - low) / rng >= kImpPos && rng >= kImpRange * atr_;
        const bool broke   = close > donch_high;
        const bool not_late = close <= donch_high + kLateAtr * atr_;
        if (broke && not_late && impulse) _fire_entry(bid, ask, now_ms);
    }

    // -------- every tick: SL management (STOP_TICK instances only) --------
    void on_tick(double bid, double ask, int64_t now_ms, OnCloseFn on_close) noexcept {
        if (!enabled || !pos.active) return;
        const double mid = (bid + ask) * 0.5;
        const double fav = mid - pos.entry_px;
        if (fav > pos.mfe) pos.mfe = fav;
        if (-fav > pos.mae) pos.mae = -fav;
        // S-2026-07-11 PHASE 1b: only mode 1 enforces the stop here. Bar-fed
        // instances (mode 0/2/3) never receive ticks; if one ever does, the
        // tick path must not double-enforce a differently-modelled stop.
        if (stop_mode == 1 && pos.sl_px > 0.0 && bid <= pos.sl_px)
            _close(pos.sl_px, "STOP_OR_TRAIL", now_ms, on_close);
        (void)ask;
    }

    void force_close(double bid, double /*ask*/, int64_t now_ms,
                     OnCloseFn on_close, const char* reason) noexcept {
        if (!pos.active) return;
        _close(bid, reason ? reason : "FORCE_CLOSE", now_ms, on_close);
    }
    // S-2026-07-11 PHASE 1b: 4-arg form so PositionPersistence acct_try_close
    // (the AccountingGuard/KILL-ALL closer) can flatten this engine.
    void force_close(double bid, double ask, int64_t now_ms, OnCloseFn on_close) noexcept {
        force_close(bid, ask, now_ms, on_close, "FORCE_CLOSE");
    }

    // ---- restart persistence (wire_cross archetype, S-2026-07-11 PHASE 1b) ----
    // Phase-1 known residual: the MGC instance had no persistence -- a restart
    // orphaned its open leg. Snapshot carries atr_at_entry in the (unused) tp
    // field: the trail arm/step distances derive from it and it is not
    // reconstructible from a trailed sl.
    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const noexcept {
        if (!pos.active) return false;
        o.engine = eng; o.symbol = sym; o.side = "LONG"; o.size = lot;
        o.entry = pos.entry_px; o.sl = pos.sl_px;
        o.tp = pos.atr_at_entry;               // NOT a take-profit (engine has none)
        o.entry_ts = pos.entry_ts_ms / 1000;
        o.mfe = pos.mfe; o.mae = pos.mae;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) noexcept {
        if (pos.active) return false;          // adopt won't double an open slot
        pos.active = true;
        pos.entry_px = ps.entry;
        pos.sl_px = ps.sl;
        pos.atr_at_entry = (ps.tp > 0.0) ? ps.tp
                          : (kStopAtr > 0.0 ? std::max(0.0, (ps.entry - ps.sl) / kStopAtr) : 0.0);
        pos.entry_ts_ms = ps.entry_ts * 1000;
        // max-hold clock resumes from wall time (bars_held itself isn't persisted)
        pos.bars_held = ps.entry_ts > 0
            ? (int)std::max<int64_t>(0, ((int64_t)std::time(nullptr) - ps.entry_ts) / 1800)
            : 0;
        pos.cooldown_bars = 0;
        pos.mfe = ps.mfe; pos.mae = ps.mae;
        return true;
    }

private:
    bool _in_session(int64_t now_ms) const noexcept {
        int64_t day_s = (now_ms / 1000) % 86400; if (day_s < 0) day_s += 86400;
        const int h = (int)(day_s / 3600);
        if (kSessStart == kSessEnd) return true;
        if (kSessStart < kSessEnd) return h >= kSessStart && h < kSessEnd;
        return h >= kSessStart || h < kSessEnd;
    }

    void _fire_entry(double bid, double ask, int64_t now_ms) noexcept {
        if (warmup_active_) return;
        if (omega::index_risk_off()) return;
        const double entry = ask;
        if (entry <= 0.0 || atr_ <= 0.0) return;
        const double sl_dist = kStopAtr * atr_;
        {
            // S-2026-07-11 GOLD PHASE 1: gate on ledger_symbol -- spot instance
            // keeps the XAUUSD row; the MGC instance uses the explicit MGC row
            // ($10/pt/contract) instead of the ~10x-misscaled spot proxy.
            const double spread_pts = ask - bid;
            if (!ExecutionCostGuard::is_viable(ledger_symbol.c_str(), spread_pts, sl_dist, lot, 1.5)) return;
        }
        // AuroraGate: MGC-tape order-flow gate (long-only breakout). Fail-open.
        if (!omega::aurora_allow("XAUUSD", true, now_ms)) {
            std::printf("[AURORA-GATE] XAUUSD LONG BLOCKED -- GoldVolBreakoutM30\n");
            return;
        }
        // S-2026-06-23 L2 confirmation gate: skip the long when the live MGC book is
        // strongly ask-heavy. Inert in backtest (l2_gate_=0 / l2_imb_=0.5 default).
        if (l2_gate_ > 0.0 && l2_imb_ < l2_gate_) {
            std::printf("[MGC-L2-GATE] GoldVolBrkM30 LONG skipped (l2_imb=%.2f < %.2f)\n", l2_imb_, l2_gate_); std::fflush(stdout);
            return;
        }
        pos.active = true;
        pos.entry_px = entry;
        // S-2026-07-11 PHASE 1b: stop level per enforcement mode. Mode 0 sets
        // NO stop (sl_px=0 -- nothing pretends protection that can't fire);
        // mode 3 sets the wide emergency level; modes 1/2 keep the 1.5-ATR stop.
        pos.sl_px = (stop_mode == 0) ? 0.0
                  : (stop_mode == 3) ? entry - cat_atr_mult * atr_
                                     : entry - sl_dist;
        pos.atr_at_entry = atr_;
        pos.entry_ts_ms = now_ms;
        pos.bars_held = 0;
        pos.cooldown_bars = 0;
        pos.mfe = pos.mae = 0.0;
    }

    void _close(double exit_px, const char* reason, int64_t now_ms, OnCloseFn on_close) noexcept {
        if (!pos.active) return;
        const double pts_move = exit_px - pos.entry_px;   // long-only
        omega::TradeRecord tr;
        tr.symbol     = ledger_symbol;   // S-2026-07-11: venue-honest (spot XAUUSD / futures MGC)
        tr.engine     = engine_tag;      // S-2026-07-11: per-instance attribution
        tr.side       = "LONG";
        tr.entryPrice = pos.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = 0.0;
        tr.sl         = pos.sl_px;
        tr.size       = lot;
        tr.entryTs    = pos.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = "VolBreakout_M30_H1EMA200_long";
        tr.shadow     = shadow_mode;
        tr.pnl        = pts_move * lot;
        tr.mfe        = pos.mfe;
        tr.mae        = pos.mae;
        if (on_close) on_close(tr);
        pos.active = false;
        pos.cooldown_bars = kCooldown;
    }

public:
    // H1 warmup CSV: ts,o,h,l,c (seconds) -- only close is used. >=206 bars.
    int seed_h1_from_csv(const std::string& path) noexcept {
        if (!enabled || path.empty()) { printf("[GoldVolBrkM30-SEED] H1 skipped (disabled/no-path)\n"); fflush(stdout); return 0; }
        std::ifstream f(path);
        if (!f.is_open()) { printf("[GoldVolBrkM30-SEED] H1 FAIL open '%s'\n", path.c_str()); fflush(stdout); return 0; }
        warmup_active_ = true;
        int fed = 0; std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#' || (line[0] < '0' || line[0] > '9')) continue;
            // ts,o,h,l,c  -> take field 5 (close)
            int comma = 0; double c = 0.0; const char* p = line.c_str(); char* q = nullptr;
            std::strtoll(p, &q, 10);
            for (; *q; ++q) { if (*q == ',') { ++comma; if (comma == 4) { c = std::strtod(q + 1, nullptr); break; } } }
            if (c > 0.0) { on_h1_close(c); ++fed; }
        }
        warmup_active_ = false;
        printf("[GoldVolBrkM30-SEED] H1 fed=%d ema200=%.3f trend=%d path='%s'\n", fed, ema_h1_, trend_, path.c_str());
        fflush(stdout);
        return fed;
    }

    // M30 warmup CSV: bar_start_ms,open,high,low,close (ms). >=35 bars.
    int seed_m30_from_csv(const std::string& path) noexcept {
        if (!enabled || path.empty()) { printf("[GoldVolBrkM30-SEED] M30 skipped (disabled/no-path)\n"); fflush(stdout); return 0; }
        std::ifstream f(path);
        if (!f.is_open()) { printf("[GoldVolBrkM30-SEED] M30 FAIL open '%s'\n", path.c_str()); fflush(stdout); return 0; }
        warmup_active_ = true;
        int fed = 0; std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#' || (line[0] < '0' || line[0] > '9')) continue;
            char* p1; long long ms = std::strtoll(line.c_str(), &p1, 10); if (!p1 || *p1 != ',') continue;
            char* p2; double o = std::strtod(p1 + 1, &p2); if (!p2 || *p2 != ',') continue;
            char* p3; double h = std::strtod(p2 + 1, &p3); if (!p3 || *p3 != ',') continue;
            char* p4; double l = std::strtod(p3 + 1, &p4); if (!p4 || *p4 != ',') continue;
            double c = std::strtod(p4 + 1, nullptr);
            if (!std::isfinite(o) || !std::isfinite(h) || !std::isfinite(l) || !std::isfinite(c)) continue;
            // S-2026-07-11 PHASE 1b: ts-scale safety. The MGC instance seeds from
            // data/mgc_30m_hist.csv whose ts is SECONDS (the spot warmup CSV is ms);
            // feeding seconds as now_ms poisoned the session-hour bookkeeping.
            if (ms < 4000000000LL) ms *= 1000;
            on_m30_bar(h, l, c, c, c, ms + 1800LL * 1000, OnCloseFn{}, o);
            ++fed;
        }
        warmup_active_ = false;
        printf("[GoldVolBrkM30-SEED] M30 fed=%d atr=%.4f bars=%d path='%s'\n", fed, atr_, (int)m30_.size(), path.c_str());
        fflush(stdout);
        return fed;
    }
};

} // namespace omega

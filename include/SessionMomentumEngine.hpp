#pragma once
// =============================================================================
//  SessionMomentumEngine.hpp -- clock-based session-window momentum long (S42)
// =============================================================================
//
//  PROVENANCE
//
//  Built 2026-05-31 (S42) from a NEW signal axis: time-of-day. Every Omega
//  trend engine to date is clock-blind (breakout/pullback/Keltner fire any
//  hour). This engine exploits gold's intraday session structure -- a long
//  held across a fixed UTC window, gated by a slow uptrend filter.
//
//  Found in backtest/xau_session_scan.cpp + xau_session_trendfilter.cpp and
//  validated LOOKAHEAD-FREE (filter on the PRIOR bar's close, decision at the
//  open of the entry hour). Numbers below are the corrected, no-lookahead,
//  cross-spread, bps figures over XAU H1 2yr (12,688 bars):
//
//    XAU NYpm  : enter 16:00 UTC, hold 4h, exit close 20:00, c>EMA200(h1)
//        1bp PF1.57 .. 5bp PF1.22, 6/6 blocks positive at EVERY cost level
//        (blocks @5bp: +369 +22 +32 +23 +173 +546 -- all +, distributed)
//        max single trade 12-20% of total g (not outlier-driven)
//    XAU o/n   : enter 23:00 UTC, hold 5h, exit close 04:00, c>EMA200(h1)
//        1bp PF1.56 .. 5bp PF1.26, 6/6 to 3bp, 5/6 at 4-5bp
//
//  De-trend check: the entry hour is SPECIFIC -- the NY-afternoon / overnight
//  windows beat the mean 5h-hold across all 19 entry hours by 2-4.5x, and
//  daytime hours are below mean. So this is a genuine session effect, NOT
//  gold-beta (else all hours would be equal). GER40 was tested and is marginal
//  post-lookahead-fix (PF2.10, 4/6 at 3bp) -- NOT instantiated here.
//
//  CAVEAT (do not over-trust): regime-sensitive (stronger in bull halves);
//  backtest cannot prove forward edge (see memory chimera-harness-optimism).
//  SHADOW-ONLY until live ledger confirms. The trend filter is what makes it
//  cost-robust to 5bp -- never run this engine without use_trend_filter=true
//  unless the live overnight spread is confirmed tight.
//
//  SIGNAL (single position, long-only)
//    decision  : on close of the bar at hour (entry_hour-1)
//    filter    : (no filter) OR close > EMA(ema_period) on H1 closes
//    entry     : long at the open of entry_hour (first tick of that hour)
//    exit      : at the close of the bar at hour exit_hour=(entry_hour+hold)%24
//                (pure TIME exit -- the validated edge has NO take-profit)
//    optional  : disaster stop at entry - sl_atr*ATR14 (default OFF, sl_atr=0)
//
//  SAFETY (mirrors Ger40KeltnerH1Engine)
//    - shadow_mode=true by default; engine_init.hpp sets the live value.
//    - touches NO protected core file.
//    - 0.01 lot; single concurrent position; long-only (shorts DEAD on gold).
//    - max_spread cap refuses bad fills; cooldown 1 bar after each close.
//    - ExecutionCostGuard::is_viable() gates every entry.
//    - warmup_from_csv primes EMA/ATR/close-ring (needs >= ema_period+2 bars).
//    - failsafe: force-exit if held > hold_hours+2 bars (missed exit bar).
//
//  USAGE  (WIRING IS DEFERRED -- this header is built + canary-checked but not
//          yet referenced by globals.hpp / engine_init.hpp / tick_indices.hpp.
//          Wire AFTER the current shadow batch deploys and live XAU overnight
//          spread is confirmed. Two instances:)
//
//      // globals.hpp:
//      static omega::SessionMomentumEngine g_xau_sess_nypm;
//      static omega::SessionMomentumEngine g_xau_sess_overnight;
//      // engine_init.hpp:
//      g_xau_sess_nypm.symbol     = "XAUUSD";
//      g_xau_sess_nypm.label      = "XauSessNYpm_h16L4_EMA200_S42";
//      g_xau_sess_nypm.entry_hour = 16; g_xau_sess_nypm.hold_hours = 4;
//      g_xau_sess_nypm.use_trend_filter = true; g_xau_sess_nypm.ema_period = 200;
//      g_xau_sess_nypm.shadow_mode = kShadowDefault; g_xau_sess_nypm.enabled = true;
//      g_xau_sess_nypm.lot = 0.01; g_xau_sess_nypm.max_spread = 2.0; // XAU $
//      g_xau_sess_nypm.warmup_csv_path = "phase1/signal_discovery/warmup_XAUUSD_H1.csv";
//      g_xau_sess_nypm.init();
//      g_xau_sess_nypm.warmup_from_csv(g_xau_sess_nypm.warmup_csv_path);
//      // (overnight: entry_hour=23, hold_hours=5, same symbol/warmup)
//      // tick_indices.hpp at every XAU tick:
//      //   g_xau_sess_nypm.feed_tick(bid, ask, now_ms, cb);
//      //   g_xau_sess_overnight.feed_tick(bid, ask, now_ms, cb);
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "AuroraGate.hpp"
#include <deque>
#include <fstream>
#include <functional>
#include <string>

#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"
#include "RegimeState.hpp"       // S-2026-06-24: shared price-brain bear long-block (item-2 coverage)
//  ADVERSE-PROTECTION: time-stop + fixed SL by design (backtested). In-flight
//    protection = fixed ATR SL + a PURE time-exit (hold N hours, no TP) -- the
//    bounded hold IS the in-flight protection. + price-bear long-block S-2026-06-24k.
//    Bull-beta (both bear halves NEG, AUDITED_CONFIGS), shadow-cap; no cold cut added
//    (short hold already bounds adverse excursion).

namespace omega {

struct SessBar {
    int64_t bar_start_ms = 0;
    double  open=0.0, high=0.0, low=0.0, close=0.0;
};

struct SessPos {
    bool    active       = false;
    double  entry_px     = 0.0;
    double  sl_px        = 0.0;     // 0 => no stop
    double  atr_at_entry = 0.0;
    int64_t entry_ts_ms  = 0;
    int     bars_held    = 0;
    int     cooldown_bars= 0;
    double  mfe=0.0, mae=0.0;
    std::string broker_position_id;
    std::string entry_clOrdId;
};

struct SessionMomentumEngine {
public:
    // ---- config (engine_init overrides; defaults are inert/safe) ----
    bool        shadow_mode = true;
    bool        enabled     = false;
    double      lot         = 0.01;
    double      max_spread  = 2.0;      // instrument price units
    std::string symbol      = "XAUUSD";
    std::string label       = "SessionMomentum_S42";

    int    entry_hour       = 16;       // UTC hour to be long from (open)
    int    hold_hours       = 4;        // exit at close of (entry_hour+hold)%24
    bool   use_trend_filter = true;     // require close > EMA(ema_period)
    int    ema_period       = 200;      // H1 EMA for the trend gate
    double sl_atr           = 0.0;      // 0 => pure time exit (validated default)
    int    skip_dow_mask    = 0;        // bit d set => skip entries on weekday d
                                        // (Sun=0..Sat=6). 0 => trade every day.

    SessPos pos{};

    std::deque<SessBar> bars_;
    int kBarHistory() const noexcept { return ema_period + 60; }

    static constexpr int kAtrPeriod = 14;
    double atr14_ = 0.0;
    int    atr_warmup_count_ = 0;

    double ema_ = 0.0;
    bool   ema_initialised_ = false;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;
    bool warmup_active_ = false;
    std::string warmup_csv_path;

    // self-contained H1 tick->bar aggregator (XAU arrives as raw ticks; no
    // shared g_bars_xau in this path). Mirrors Ger40KeltnerH1Engine.
    struct H1Accum {
        bool    active=false; int64_t bucket_ms=0;
        double  open=0.0, high=0.0, low=0.0, close=0.0;
    } h1_acc_;

    int decision_hour() const noexcept { return (entry_hour + 23) % 24; }
    int exit_hour()     const noexcept { return (entry_hour + hold_hours) % 24; }
    static int hourUTC(int64_t ms) noexcept { return (int)((ms / 3600000LL) % 24); }
    static int dowUTC (int64_t ms) noexcept { return (int)(((ms / 86400000LL) + 4) % 7); } // 1970-01-01 = Thu(4)

    void init() noexcept {
        bars_.clear();
        atr14_ = 0.0; atr_warmup_count_ = 0;
        ema_ = 0.0; ema_initialised_ = false;
        warmup_active_ = false;
        h1_acc_ = {};
        pos = {};
    }

    bool any_open() const noexcept { return pos.active; }

    // Called with the JUST-CLOSED H1 bar. All session entry/exit logic lives here.
    void on_h1_bar(const SessBar& bar, double bid, double ask,
                   double atr14_external, int64_t now_ms, OnCloseFn on_close) noexcept {
        if (!enabled) return;

        bars_.push_back(bar);
        while ((int)bars_.size() > kBarHistory()) bars_.pop_front();

        if (atr14_external > 0.0) atr14_ = atr14_external;
        else                       _update_local_atr();
        _update_ema();

        if (pos.cooldown_bars > 0) --pos.cooldown_bars;

        const int h = hourUTC(bar.bar_start_ms);

        // ---- manage an open position on this just-closed bar ----
        if (pos.active) {
            ++pos.bars_held;
            // time exit at the close of the exit-hour bar (the validated rule)
            if (h == exit_hour()) {
                _close(bid > 0.0 ? bid : bar.close, "SESSION_TIME_EXIT", now_ms, on_close);
            }
            // failsafe: a missed exit bar (weekend/gap) -- don't hold forever
            else if (pos.bars_held > hold_hours + 2) {
                _close(bid > 0.0 ? bid : bar.close, "SESSION_FAILSAFE_EXIT", now_ms, on_close);
            }
            return;   // never enter and exit on the same bar
        }

        // ---- entry: decision at the close of the (entry_hour-1) bar ----
        if ((int)bars_.size() < ema_period + 2) return;   // filter not armed
        if (atr14_ <= 0.0) return;
        if (use_trend_filter && !ema_initialised_) return;
        if (pos.cooldown_bars > 0) return;
        if (ask - bid > max_spread) return;
        if (h != decision_hour()) return;
        // skip configured weekdays (entry day == decision-bar day, same UTC date)
        if (skip_dow_mask & (1 << dowUTC(bar.bar_start_ms))) return;
        if (use_trend_filter && !(bar.close > ema_)) return;

        _fire_entry(bid, ask, now_ms);
        (void)on_close;
    }

    void on_tick(double bid, double ask, int64_t now_ms, OnCloseFn on_close) noexcept {
        if (!enabled || !pos.active) return;
        _manage_open(bid, ask, now_ms, on_close);
    }

    // LIVE entry point (tick_indices.hpp): aggregate ticks to H1 + manage open.
    void feed_tick(double bid, double ask, int64_t now_ms, OnCloseFn on_close) noexcept {
        if (!enabled) return;
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid = (bid + ask) * 0.5;

        if (pos.active) _manage_open(bid, ask, now_ms, on_close);

        const int64_t bucket = (now_ms / 3600000LL) * 3600000LL;   // UTC hour
        if (!h1_acc_.active) {
            h1_acc_.active=true; h1_acc_.bucket_ms=bucket;
            h1_acc_.open=h1_acc_.high=h1_acc_.low=h1_acc_.close=mid;
            return;
        }
        if (bucket != h1_acc_.bucket_ms) {
            SessBar bar;
            bar.bar_start_ms = h1_acc_.bucket_ms;
            bar.open=h1_acc_.open; bar.high=h1_acc_.high;
            bar.low =h1_acc_.low;  bar.close=h1_acc_.close;
            on_h1_bar(bar, bid, ask, 0.0, now_ms, on_close);
            h1_acc_.bucket_ms=bucket;
            h1_acc_.open=h1_acc_.high=h1_acc_.low=h1_acc_.close=mid;
        } else {
            if (mid > h1_acc_.high) h1_acc_.high = mid;
            if (mid < h1_acc_.low)  h1_acc_.low  = mid;
            h1_acc_.close = mid;
        }
    }

    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseFn on_close, const char* reason) noexcept {
        if (!pos.active) return;
        _close(bid > 0.0 ? bid : pos.entry_px, reason ? reason : "FORCE_CLOSE", now_ms, on_close);
        (void)ask;
    }

private:
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

    void _update_ema() noexcept {
        if (bars_.empty()) return;
        const double c = bars_.back().close;
        if (!ema_initialised_) { ema_ = c; ema_initialised_ = true; }
        else { const double a = 2.0 / (ema_period + 1); ema_ = a * c + (1.0 - a) * ema_; }
    }

    void _fire_entry(double bid, double ask, int64_t now_ms) noexcept {
        if (warmup_active_) return;
        const double entry = ask;   // long on ask
        if (entry <= 0.0 || atr14_ <= 0.0) return;

        // S-2026-06-24 universal price-bear long-block (item-2 bear coverage).
        // SessionMomentum is a long-only bull-beta gold engine (audit: both bear
        // halves NEG) and self-enters -- it bypasses the enter_directional
        // chokepoint, so it needs its OWN bear gate, the same shared price-brain
        // idiom the 4 sibling gold engines use (XauTf 1h/2h/D1 + ThreeBar). Blocks
        // NEW longs in a confirmed gold price-bear OR macro-hostile regime; manage/
        // exit paths (_manage_open) are untouched. Fail-open while the brain is
        // cold (long_blocked()=false until warm). gold_regime() because this is the
        // XAU session engine (g_xau_sess_nypm is the only instance).
        if (omega::gold_regime().long_blocked()) {
            std::printf("[BEAR-GATE] %s LONG BLOCKED -- SessionMomentum price-bear/macro-hostile\n",
                        symbol.c_str());
            return;
        }

        const double sl_dist = (sl_atr > 0.0) ? sl_atr * atr14_ : 0.0;
        const double sl_px   = (sl_atr > 0.0) ? entry - sl_dist : 0.0;

        {   // cost gate: no fixed TP -> use one ATR as the viability span proxy
            const double spread_pts = ask - bid;
            const double span = std::max(atr14_, spread_pts * 2.0);
            if (!ExecutionCostGuard::is_viable(symbol.c_str(), spread_pts, span, lot, 1.0)) return;
        }

        // AuroraGate: MGC-tape order-flow gate (session-momentum long). Fail-open.
        if (!omega::aurora_allow(symbol.c_str(), true, now_ms)) {
            std::printf("[AURORA-GATE] %s LONG BLOCKED -- SessionMomentum\n", symbol.c_str());
            return;
        }
        pos.active        = true;
        pos.entry_px      = entry;
        pos.sl_px         = sl_px;
        pos.atr_at_entry  = atr14_;
        pos.entry_ts_ms   = now_ms;
        pos.bars_held     = 0;
        pos.cooldown_bars = 0;
        pos.mfe = pos.mae = 0.0;
        pos.broker_position_id.clear();
        pos.entry_clOrdId.clear();
    }

    void _manage_open(double bid, double ask, int64_t now_ms, OnCloseFn on_close) noexcept {
        if (!pos.active) return;
        const double mid = (bid + ask) * 0.5;
        const double fav = mid - pos.entry_px;   // long-only
        if (fav > pos.mfe) pos.mfe = fav;
        if (-fav > pos.mae) pos.mae = -fav;
        if (pos.sl_px > 0.0 && bid <= pos.sl_px) { _close(pos.sl_px, "SL_HIT", now_ms, on_close); return; }
        (void)ask;
    }

    void _close(double exit_px, const char* reason, int64_t now_ms, OnCloseFn on_close) noexcept {
        if (!pos.active) return;
        const double pts_move = exit_px - pos.entry_px;   // long-only

        omega::TradeRecord tr;
        tr.symbol     = symbol;
        tr.engine     = label;
        tr.side       = "LONG";
        tr.entryPrice = pos.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = 0.0;
        tr.sl         = pos.sl_px;
        tr.size       = lot;
        tr.entryTs    = pos.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = "session_window_long";
        tr.shadow     = shadow_mode;
        tr.pnl        = pts_move * lot;
        tr.mfe        = pos.mfe;
        tr.mae        = pos.mae;

        if (on_close) on_close(tr);

        pos.active = false;
        pos.broker_position_id.clear();
        pos.entry_clOrdId.clear();
        pos.cooldown_bars = 1;
    }

public:
    // CSV: ts,o,h,l,c  (ts seconds or ms -- only closes are used to seed EMA/ATR;
    // hour logic is irrelevant during warmup since warmup_active_ blocks entries).
    int warmup_from_csv(const std::string& path) noexcept {
        if (!enabled)      { printf("[%s-WARMUP] skipped -- disabled\n", label.c_str()); fflush(stdout); return 0; }
        if (path.empty())  { printf("[%s-WARMUP] skipped -- no path (cold start)\n", label.c_str()); fflush(stdout); return 0; }
        std::ifstream f(path);
        if (!f.is_open())  { printf("[%s-WARMUP] FAIL -- cannot open '%s'\n", label.c_str(), path.c_str()); fflush(stdout); return 0; }
        warmup_active_ = true;

        int fed = 0;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#' || line[0] == 't') continue;   // header 'ts,...'
            char* p1; long long ts = std::strtoll(line.c_str(), &p1, 10);
            if (!p1 || *p1 != ',') continue;
            char* p2; double o = std::strtod(p1+1, &p2); if (!p2 || *p2 != ',') continue;
            char* p3; double h = std::strtod(p2+1, &p3); if (!p3 || *p3 != ',') continue;
            char* p4; double l = std::strtod(p3+1, &p4); if (!p4 || *p4 != ',') continue;
            char* p5; double c = std::strtod(p4+1, &p5);
            if (!std::isfinite(o) || !std::isfinite(h) || !std::isfinite(l) || !std::isfinite(c)) continue;

            // normalise ts to ms (accept seconds or ms input)
            int64_t ms = (ts < 100000000000LL) ? ts * 1000LL : ts;
            SessBar bar; bar.bar_start_ms = ms; bar.open=o; bar.high=h; bar.low=l; bar.close=c;
            on_h1_bar(bar, c, c, 0.0, ms + 3600LL*1000, OnCloseFn{});
            ++fed; (void)p5;
        }
        warmup_active_ = false;
        printf("[%s-WARMUP] fed=%d bars, atr=%.4f ema=%.4f bars=%d path='%s'\n",
               label.c_str(), fed, atr14_, ema_, (int)bars_.size(), path.c_str());
        fflush(stdout);
        return fed;
    }
};

} // namespace omega

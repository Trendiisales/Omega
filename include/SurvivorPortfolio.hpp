//  ADVERSE-PROTECTION: (S-2026-07-08c, backtested verdict) per-cell hard ATR SL bracket
//  (sl_mult 1.0-1.5 x ATR, intrabar SL_HIT enforced) + tp_mult TP + max_hold_bars (30) bar-count
//  timeout + reclaim_exit on the Donchian cell + blanket per-(symbol,side) dedup cap + the
//  S-2026-07-08 entry_veto bear chokepoint (USTEC longs -> index_market_regime().long_blocked(),
//  XAU longs -> gold_regime().long_blocked(); entries only, exits unaffected). Backtest
//  backtest/survivor_gated_bt.cpp (2022-2026 both-regime, REAL class): gated book maxDD bounded,
//  BEAR-2022 POSITIVE PF1.90; no cold loss-cut beyond the SL bracket -- the ATR SL IS the
//  in-flight protection (mean-rev + breakout cells, bracket-native design).
// =============================================================================
// SurvivorPortfolio.hpp -- 13 cells from S38 discovery walk-forward survivors
// -----------------------------------------------------------------------------
// Tactical one-file engine. Mirrors backtest/multi_tf_sweep.cpp signal logic
// so the live cells produce the same entries the backtest found.
//
// Cells (S38 discovery + USDJPY FX sweep):
//   1.  XAUUSD 4h  DonchianBO   N=20    sl=1.5  tp=3.0  (Sharpe 2.93)
//   2.  XAUUSD 4h  DonchianBO   N=100   sl=1.5  tp=3.0  (Sharpe 2.49)
//   3.  GER40  4h  RSIExtreme   N=7  lo=30 hi=70 sl=1.0 tp=2.0  (Sharpe 2.17)
//   4.  GER40  15m MACrossover  fast=10 slow=30 sl=1.5 tp=3.0   (Sharpe 1.96)
//   5.  GER40  1h  RSIExtreme   N=14 lo=30 hi=70 sl=1.0 tp=2.0  (Sharpe 1.90)
//   6.  GER40  1h  DonchianBO   N=100   sl=1.5  tp=3.0  (Sharpe 1.72)
//   7.  XAUUSD 4h  MACrossover  fast=10 slow=30 sl=1.5 tp=3.0   (Sharpe 1.21)
//   8.  USTEC  4h  RSIExtreme   N=7  lo=30 hi=70 sl=1.0 tp=2.0  (Sharpe 1.13)
//   9.  GER40  15m MACrossover  fast=20 slow=50 sl=1.5 tp=3.0   (Sharpe 0.99)
//  10.  USTEC  4h  ZScoreMR     W=20 Z=2.5  sl=1.0 tp=2.0  (Sharpe 0.90)
//  11.  GER40  5m  RSIExtreme   N=14 lo=30 hi=70 sl=1.0 tp=2.0  (Sharpe 0.79)
//  12.  GER40  30m RSIExtreme   N=14 lo=20 hi=80 sl=1.0 tp=2.0  (Sharpe 0.72)
//  13.  USDJPY 4h  SPXVolGated  sgn=+1 vm=1.2 sl=2.0 tp=5.0     (Sharpe 2.58)
//
// All cells ship enabled=true shadow_mode=true. Promotion gate per CLAUDE.md:
// 30 shadow trades + WR >= 35% + net positive.
// =============================================================================
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "OmegaTradeLedger.hpp"
#include "OpenPositionRegistry.hpp"   // S-2026-06-03: PositionSnapshot for adopt()
#include "GoldTrendMimicLadder.hpp"   // S-2026-07-14h: one-way survivor-cell mimic hook

namespace omega::survivor {

// -----------------------------------------------------------------------------
// Bar + per-cell state
// -----------------------------------------------------------------------------
struct Bar { int64_t ts_sec = 0; double o = 0, h = 0, l = 0, c = 0; };

enum class Family { Donchian, RSI, MACross, ZScoreMR, SPXVolGatedDonch };

struct CellCfg {
    const char* tag;          // e.g. "XAU_4h_DonchN20"
    const char* symbol;       // "XAUUSD" / "GER40" / "USTEC" / "USDJPY"
    int         tf_sec;       // 14400 = H4, 3600 = H1, 1800 = M30, 900 = M15, 300 = M5
    Family      family;
    // family params
    int    N        = 20;     // donch / rsi / macross_slow / zscore_W
    int    N_fast   = 10;     // macross_fast
    double lo       = 30;     // rsi lo
    double hi       = 70;     // rsi hi
    double zthr     = 2.0;    // zscore threshold
    int    spx_sgn  = 0;      // SPXVolGated: +1 long-when-up, -1 long-when-down
    double spx_vm   = 1.2;    // SPXVolGated: vol-gate multiplier (ATR > vm * median_50)
    // bracket
    double sl_mult  = 1.5;    // SL = sl_mult * ATR(14)
    double tp_mult  = 3.0;    // TP = tp_mult * ATR(14)
    int    max_hold_bars = 30;
    int    cooldown_bars = 1;
    // S-2026-06-17 failed-breakout (reclaim) exit — Donchian only. Cut a breakout
    // that CLOSES back through the channel level it broke (the bear trap). Validated
    // in backtest/donch_reclaim_test.py on 2yr XAU H4: the NAIVE both-sides/any-time
    // reclaim is NOT robust (H1 negative on both cells — it bleeds long-pullbacks +
    // slow recoveries); SHORT-only within <=3 bars IS robust on BOTH cells and BOTH
    // walk-forward halves (N20 +264pt, N100 +31pt, PF up, avgL down). Checked AFTER
    // SL/TP so it only helps when price snaps back without first hitting the stop.
    bool   reclaim_exit       = false;
    int    reclaim_max_bars   = 3;     // only fire the reclaim within K bars of entry
    bool   reclaim_short_only = true;  // long-side reclaim hurt in BT — shorts only
    double lot      = 0.01;
    // per-symbol point conversion (price unit and $-per-pt-per-lot)
    double pt_size       = 0.01;
    double tick_usd_per_lot = 100.0;
};

struct CellState {
    bool enabled = true;
    bool shadow_mode = true;
    std::deque<Bar> bars;
    std::deque<double> atr;   // ATR per bar
    // Live bar aggregator
    Bar    cur{};
    int64_t cur_bar_start = 0;
    bool   in_bar = false;
    // ATR state
    double prev_close = 0;
    double atr_val   = 0;
    bool   atr_ready = false;
    // Cell-level counters
    int64_t last_entry_bar_idx = -100000;
    int     bar_idx            = 0;
    // Open position
    bool   pos_active = false;
    int    pos_side   = 0;
    double pos_entry  = 0, pos_sl = 0, pos_tp = 0;
    double pos_mfe    = 0;
    int    pos_bars_held = 0;
    int64_t pos_entry_ts = 0;
    int    pos_entry_bar_idx = 0;
    double pos_break_lvl = 0;   // Donchian channel level broken at entry (reclaim ref)
    // Stats
    int n_trades = 0, n_wins = 0;
    double cum_net = 0;
};

// -----------------------------------------------------------------------------
// Indicators
// -----------------------------------------------------------------------------
static inline double compute_rsi(const std::deque<Bar>& bars, int n) {
    if ((int)bars.size() < n + 1) return 50.0;
    double g = 0, l = 0;
    for (int i = (int)bars.size() - n; i < (int)bars.size(); ++i) {
        double d = bars[i].c - bars[i-1].c;
        if (d > 0) g += d; else l -= d;
    }
    if (l < 1e-12) return 100.0;
    double rs = g / l;
    return 100.0 - 100.0 / (1 + rs);
}
static inline double compute_sma(const std::deque<Bar>& bars, int n) {
    if ((int)bars.size() < n) return 0;
    double s = 0;
    for (int i = (int)bars.size() - n; i < (int)bars.size(); ++i) s += bars[i].c;
    return s / n;
}
struct DonchVal { double hi = 0, lo = 0; bool ok = false; };
static inline DonchVal compute_donch(const std::deque<Bar>& bars, int n) {
    DonchVal d;
    if ((int)bars.size() < n + 1) return d;
    int end = (int)bars.size() - 1;  // exclude current bar
    int begin = end - n;
    d.hi = -1e18; d.lo = 1e18;
    for (int i = begin; i < end; ++i) {
        if (bars[i].h > d.hi) d.hi = bars[i].h;
        if (bars[i].l < d.lo) d.lo = bars[i].l;
    }
    d.ok = true;
    return d;
}
static inline double compute_zscore(const std::deque<Bar>& bars, int W) {
    if ((int)bars.size() < W) return 0;
    double s = 0;
    for (int i = (int)bars.size() - W; i < (int)bars.size(); ++i) s += bars[i].c;
    double mean = s / W;
    double v = 0;
    for (int i = (int)bars.size() - W; i < (int)bars.size(); ++i) {
        double d = bars[i].c - mean; v += d * d;
    }
    v /= W;
    double sd = std::sqrt(v);
    if (sd < 1e-12) return 0;
    return (bars.back().c - mean) / sd;
}

// SPX overlay state (global, single instance)
struct SpxOverlay {
    std::deque<Bar> spx_bars;
    int ema_fast = 20, ema_slow = 100;
    double ef = 0, es = 0;
    void seed_from_csv(const std::string& path);
    void on_bar(const Bar& b);
    int  trend_at_now() const noexcept {
        if (ef <= 0 || es <= 0) return 0;
        return ef > es ? +1 : -1;
    }
};
inline void SpxOverlay::seed_from_csv(const std::string& path) {
    std::ifstream f(path); if (!f) return;
    std::string line; bool first = true;
    double kf = 2.0 / (ema_fast + 1), ks = 2.0 / (ema_slow + 1);
    while (std::getline(f, line)) {
        if (first) { first = false; if (!line.empty() && (line[0] < '0' || line[0] > '9') && line[0] != '-') continue; }
        std::stringstream ss(line); std::string t; std::vector<std::string> tok;
        while (std::getline(ss, t, ',')) tok.push_back(t);
        if (tok.size() < 5) continue;
        Bar b; b.ts_sec = std::atoll(tok[0].c_str());
        b.o = std::atof(tok[1].c_str()); b.h = std::atof(tok[2].c_str());
        b.l = std::atof(tok[3].c_str()); b.c = std::atof(tok[4].c_str());
        if (b.h <= 0) continue;
        spx_bars.push_back(b);
        if (ef == 0) { ef = b.c; es = b.c; }
        else { ef = kf * b.c + (1 - kf) * ef; es = ks * b.c + (1 - ks) * es; }
    }
    printf("[SURV-SPX-SEED] %s bars=%zu ef=%.2f es=%.2f trend=%d\n",
           path.c_str(), spx_bars.size(), ef, es, trend_at_now());
    fflush(stdout);
}
inline void SpxOverlay::on_bar(const Bar& b) {
    double kf = 2.0 / (ema_fast + 1), ks = 2.0 / (ema_slow + 1);
    if (ef == 0) { ef = b.c; es = b.c; }
    else { ef = kf * b.c + (1 - kf) * ef; es = ks * b.c + (1 - ks) * es; }
    spx_bars.push_back(b);
    if (spx_bars.size() > 2000) spx_bars.pop_front();
}

// -----------------------------------------------------------------------------
// Cell engine
// -----------------------------------------------------------------------------
class Cell {
public:
    CellCfg  cfg;
    CellState st;

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    explicit Cell(const CellCfg& c) : cfg(c) {}

    // Hydrate bar history + warm ATR from a precomputed bar CSV (ts,o,h,l,c).
    // Sets enabled=false during replay so no entries fire on historical bars.
    int seed_from_csv(const std::string& path) {
        std::ifstream f(path); if (!f) {
            printf("[SURV-SEED] %s: cannot open %s\n", cfg.tag, path.c_str());
            fflush(stdout); return 0;
        }
        bool was_enabled = st.enabled; st.enabled = false;
        std::string line; bool first = true; int fed = 0;
        while (std::getline(f, line)) {
            if (first) { first = false; if (!line.empty() && (line[0] < '0' || line[0] > '9') && line[0] != '-') continue; }
            std::stringstream ss(line); std::string t; std::vector<std::string> tok;
            while (std::getline(ss, t, ',')) tok.push_back(t);
            if (tok.size() < 5) continue;
            Bar b; b.ts_sec = std::atoll(tok[0].c_str());
            b.o = std::atof(tok[1].c_str()); b.h = std::atof(tok[2].c_str());
            b.l = std::atof(tok[3].c_str()); b.c = std::atof(tok[4].c_str());
            if (b.h <= 0) continue;
            push_bar_internal(b);
            ++fed;
        }
        st.enabled = was_enabled;
        printf("[SURV-SEED] %s fed=%d bars atr=%.4f bars_size=%zu\n",
               cfg.tag, fed, st.atr_val, st.bars.size());
        fflush(stdout);
        return fed;
    }

    // S-2026-06-03: expose open-position symbol/side so the portfolio can
    // enforce a per-(symbol,side) concurrency cap (dedup correlated stacking).
    const char* sym_cstr() const { return cfg.symbol; }
    int open_side()        const { return st.pos_active ? st.pos_side : 0; }
    bool has_open()        const { return st.pos_active; }

    // S-2026-06-17 dedup-RESOLVE: unconditionally market-close a redundant
    // duplicate (a sibling cell already holds this symbol+side). Uses the SAME
    // close path as a normal SL/TP exit (emits a TradeRecord via cb -> the
    // ledger / broker path handle it exactly as any other exit) -- introduces NO
    // new order behavior, just an earlier exit for the redundant cell. The
    // blanket dedup gates new ENTRIES; this clears legacy / same-bar dup OPENS
    // (e.g. XAU DonchN20 + DonchN100 firing on one breakout) that the entry gate
    // can't undo. Reason "DEDUP_CLOSE" so it is distinguishable in the ledger.
    void force_close_dup(double bid, double ask, int64_t now_s, CloseCb cb,
                         const char* reason = "DEDUP_CLOSE") noexcept {
        if (!st.pos_active) return;
        const double mid = 0.5 * (bid + ask);
        // Sanity guard: refuse to book a close at an implausible price (bad/cross-
        // symbol tick). A real exit is within ~25% of entry; anything past that is
        // a feed glitch -- skip this tick, retry on the next valid one.
        if (mid <= 0 || st.pos_entry <= 0 ||
            std::abs(mid - st.pos_entry) / st.pos_entry > 0.25) {
            printf("[SURV-DEDUP-SKIP] %s bad mid=%.5f vs entry=%.5f -- skip dup-close this tick\n",
                   cfg.tag, mid, st.pos_entry);
            fflush(stdout);
            return;
        }
        const double exit_px  = mid;
        const double gross_pts = (st.pos_side > 0) ? (exit_px - st.pos_entry)
                                                   : (st.pos_entry - exit_px);
        const double gross_usd = gross_pts * cfg.tick_usd_per_lot * cfg.lot;  // stats/log only
        const double gross_raw = gross_pts * cfg.lot;                          // ledger applies mult
        omega::TradeRecord tr;
        tr.symbol     = cfg.symbol;
        tr.side       = (st.pos_side > 0) ? "LONG" : "SHORT";
        tr.engine     = cfg.tag;
        tr.entryPrice = st.pos_entry;
        tr.exitPrice  = exit_px;
        tr.sl         = st.pos_sl;
        tr.size       = cfg.lot;
        tr.pnl        = gross_raw;
        tr.mfe        = st.pos_mfe;
        tr.entryTs    = st.pos_entry_ts;
        tr.exitTs     = now_s;
        tr.exitReason = reason;
        tr.regime     = "SURV";
        tr.shadow     = st.shadow_mode;
        ++st.n_trades;
        if (gross_usd > 0) ++st.n_wins;
        st.cum_net += gross_usd;
        printf("[SURV-DEDUP-CLOSE] %s %s entry=%.5f exit=%.5f $=%+.2f -- redundant dup closed%s\n",
               cfg.tag, st.pos_side > 0 ? "LONG" : "SHORT", st.pos_entry, exit_px,
               gross_usd, st.shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);
        st.pos_active = false;
        st.pos_side   = 0;
        if (cb) cb(tr);
    }

    // S-2026-06-16: Kaufman Efficiency Ratio over the last w CLOSED bars
    // (1.0 = clean directional trend, ~0 = chop). Drives the regime-gated dedup
    // cap. Insufficient bars -> 1.0 (treat as trend = no cap = the proven-good
    // default), so cold-start never spuriously caps.
    double efficiency_ratio(int w) const {
        const auto& b = st.bars;
        const int m = (int)b.size();
        if (w < 1 || m < w + 1) return 1.0;
        const double net = std::fabs(b[m-1].c - b[m-1-w].c);
        double sum = 0.0;
        for (int i = m - w; i < m; ++i) sum += std::fabs(b[i].c - b[i-1].c);
        return sum > 1e-12 ? net / sum : 1.0;
    }

    // S-2026-06-03: adopt a persisted position on boot (resume managing it; do
    // NOT re-enter). Restores side/entry/sl/tp/entry_ts. mfe resets to 0
    // (record-only field, not used for management here).
    // S-2026-06-11 ZOMBIE FIX: the hold-clock used to reset to the restore
    // point ("a deploy can't prematurely time a trade out") — but with frequent
    // deploys the max_hold timeout NEVER fired: USDJPY_4h_SPXVG (max_hold 48
    // bars = 8d) was found alive at 13 DAYS. Now the clock back-dates from the
    // ORIGINAL entry_ts, so an over-held adopted position times out on the
    // next bar close — the time stop fires as if the restarts never happened.
    void adopt(const omega::PositionSnapshot& ps) {
        st.pos_active        = true;
        st.pos_side          = (ps.side == "LONG") ? 1 : -1;
        st.pos_entry         = ps.entry;
        st.pos_sl            = ps.sl;
        st.pos_tp            = ps.tp;
        st.pos_mfe           = 0.0;
        st.pos_bars_held     = 0;
        int already_held = 0;
        if (ps.entry_ts > 0 && cfg.tf_sec > 0) {
            const long long now_s = (long long)std::time(nullptr);
            already_held = (int)((now_s - (long long)ps.entry_ts) / cfg.tf_sec);
            if (already_held < 0) already_held = 0;
        }
        st.pos_entry_bar_idx = st.bar_idx - already_held;
        st.pos_entry_ts      = ps.entry_ts;
        st.last_entry_bar_idx = st.bar_idx;
        std::printf("[SURV-ADOPT] %s %s entry=%.5f sl=%.5f tp=%.5f (resumed from persist)\n",
                    cfg.tag, st.pos_side > 0 ? "LONG" : "SHORT",
                    st.pos_entry, st.pos_sl, st.pos_tp);
        if (already_held >= cfg.max_hold_bars)
            std::printf("[SURV-ADOPT] %s OVER-HELD at adopt (held=%d max=%d bars) — "
                        "TIMEOUT fires on first managed tick\n",
                        cfg.tag, already_held, cfg.max_hold_bars);
        std::fflush(stdout);
    }

    // Per-tick: aggregate to next bar; manage open position; on bar close,
    // evaluate signal.
    void on_tick(const std::string& sym, double bid, double ask, int64_t now_ms,
                 const SpxOverlay& spx, CloseCb cb,
                 const std::function<bool(const char*, int)>& side_taken = {}) noexcept {
        // S-2026-06-11 zombie hardening: a disabled cell must still MANAGE an
        // open position (aggregate bars so the bar-count timeout advances + run
        // manage_position) or the position freezes forever — this early-return
        // used to sit before manage. Disabled now blocks ENTRIES only
        // (evaluate_signal stays gated on st.enabled below).
        if (!st.enabled && !st.pos_active) return;
        if (sym != cfg.symbol) return;
        const double mid = 0.5 * (bid + ask);
        if (mid <= 0) return;
        const int64_t now_s = now_ms / 1000;

        // Aggregate into current bar
        const int64_t bar_start = (now_s / cfg.tf_sec) * cfg.tf_sec;
        if (!st.in_bar || bar_start != st.cur_bar_start) {
            if (st.in_bar) {
                // close previous bar
                push_bar_internal(st.cur);
                // SURVIVOR-CELL MIMIC feed (S-2026-07-14h): drive this cell's mimic book
                // on its NATIVE bar close, BEFORE evaluate_signal can fire on_trend_open
                // -- a leg spawned at this close first sees the NEXT bar, matching the
                // validated mimic_ladder_overlay semantics (trigger bar seq0 skipped).
                // Unregistered cell tags no-op inside the registry.
                omega::gold_trend_mimic().on_bar(cfg.tag, st.cur.h, st.cur.l, st.cur.c,
                                                 st.cur.ts_sec);
                // bar-close evaluation gate
                if (st.enabled) evaluate_signal(spx, mid, cb, side_taken);
            }
            st.cur_bar_start = bar_start;
            st.cur = { bar_start, mid, mid, mid, mid };
            st.in_bar = true;
        } else {
            if (mid > st.cur.h) st.cur.h = mid;
            if (mid < st.cur.l) st.cur.l = mid;
            st.cur.c = mid;
        }

        // Manage open position on every tick
        if (st.pos_active) manage_position(bid, ask, mid, now_s, cb);
    }

private:
    void push_bar_internal(const Bar& b) {
        // ATR(14) Wilder-like update on close
        const int ATR_N = 14;
        if (st.bars.empty()) { st.prev_close = b.c; }
        double tr = std::max({ b.h - b.l,
                               std::abs(b.h - st.prev_close),
                               std::abs(b.l - st.prev_close) });
        st.prev_close = b.c;
        if (!st.atr_ready) {
            if ((int)st.atr.size() < ATR_N) {
                st.atr.push_back(tr);
                if ((int)st.atr.size() == ATR_N) {
                    double s = 0; for (auto v : st.atr) s += v;
                    st.atr_val = s / ATR_N;
                    st.atr_ready = true;
                }
            }
        } else {
            st.atr_val = (st.atr_val * (ATR_N - 1) + tr) / ATR_N;
        }
        st.bars.push_back(b);
        // cap bar history
        const int MAX_BARS = std::max(300, cfg.N + 80);
        while ((int)st.bars.size() > MAX_BARS) st.bars.pop_front();
        ++st.bar_idx;
    }

    // Trend filter for mean-rev families (RSI, ZScoreMR). Returns +1 up / -1 down
    // / 0 neutral, from the SLOPE of SMA(TN) over TN/2 bars on the cell's own TF.
    // Mean-rev fades extremes; without this it shorts strong uptrends / longs
    // downtrends and bleeds (USTEC 4h RSI/ZMR, GER40 RSI — tombstoned pattern).
    // Neutral band (|slope| < THR) = chop = mean-rev allowed; a clear trend vetoes
    // the counter-trend side only. Conservative THR so only obvious trends gate.
    int trend_dir() const noexcept {
        const int TN = 50;
        const int n  = (int)st.bars.size();
        if (n < TN + TN / 2 + 1) return 0;          // not enough history -> allow
        auto sma_back = [&](int back) -> double {
            int e = n - back; double s = 0.0;
            for (int i = e - TN; i < e; ++i) s += st.bars[i].c;
            return s / TN;
        };
        const double s_now = sma_back(0), s_prev = sma_back(TN / 2);
        if (s_prev <= 0.0) return 0;
        const double slope = (s_now - s_prev) / s_prev;
        const double THR = 0.0015;                  // 0.15% over TN/2 bars = trend
        if (slope >  THR) return +1;
        if (slope < -THR) return -1;
        return 0;
    }

    int compute_signal_dir(const SpxOverlay& spx) const {
        switch (cfg.family) {
            case Family::Donchian: {
                auto d = compute_donch(st.bars, cfg.N);
                if (!d.ok) return 0;
                double c = st.bars.back().c;
                if (c > d.hi) return +1;
                if (c < d.lo) return -1;
                return 0;
                // NO TREND FILTER on Donchian (deliberate, 2026-06-16). A trend
                // gate (mirroring the RSI/ZScoreMR filter) was tested against the
                // XAU_4h_DonchN20/N100 cells on the H4 warmup: it HALVED N20
                // (net +2337 PF2.23 -> +1099 PF1.69) by blocking counter-trend
                // downside breakouts that are profitable gold-correction catches
                // (the cell's fat tail). N100 was neutral. The "short into a long"
                // these cells show on the open-positions screen backtests +EV; it
                // is NOT a bug. Harness: /tmp/sgt_donch.cpp (extends
                // backtest/survivor_trendgate_test.cpp with the Donchian family).
            }
            case Family::RSI: {
                if ((int)st.bars.size() < cfg.N + 2) return 0;
                double rsi = compute_rsi(st.bars, cfg.N);
                // mean-rev: rsi < lo -> long, rsi > hi -> short
                int dir = (rsi < cfg.lo) ? +1 : (rsi > cfg.hi ? -1 : 0);
                if (dir == 0) return 0;
                // TREND FILTER: never fade a clear trend (2026-06-03).
                int td = trend_dir();
                if (dir < 0 && td > 0) return 0;   // no short into uptrend
                if (dir > 0 && td < 0) return 0;   // no long into downtrend
                return dir;
            }
            case Family::MACross: {
                int F = cfg.N_fast, S = cfg.N;
                if ((int)st.bars.size() < S + 1) return 0;
                // compute fast & slow SMAs at -1 and current
                auto sma_at = [&](int win, int back){
                    if ((int)st.bars.size() < win + back) return 0.0;
                    double s = 0;
                    int e = (int)st.bars.size() - back;
                    for (int i = e - win; i < e; ++i) s += st.bars[i].c;
                    return s / win;
                };
                double f0 = sma_at(F, 0), f1 = sma_at(F, 1);
                double s0 = sma_at(S, 0), s1 = sma_at(S, 1);
                if (f0 == 0 || s0 == 0 || f1 == 0 || s1 == 0) return 0;
                bool up   = (f1 <= s1) && (f0 >  s0);
                bool down = (f1 >= s1) && (f0 <  s0);
                if (up)   return +1;
                if (down) return -1;
                return 0;
            }
            case Family::ZScoreMR: {
                if ((int)st.bars.size() < cfg.N + 1) return 0;
                double z = compute_zscore(st.bars, cfg.N);
                int dir = (z >= cfg.zthr) ? -1 : (z <= -cfg.zthr ? +1 : 0);
                if (dir == 0) return 0;
                // TREND FILTER: never fade a clear trend (2026-06-03).
                int td = trend_dir();
                if (dir < 0 && td > 0) return 0;   // no short into uptrend
                if (dir > 0 && td < 0) return 0;   // no long into downtrend
                return dir;
            }
            case Family::SPXVolGatedDonch: {
                // Donch N + ATR > spx_vm * median(ATR_50) + SPX trend gate
                auto d = compute_donch(st.bars, cfg.N);
                if (!d.ok) return 0;
                if ((int)st.bars.size() < 50) return 0;
                // median ATR50 (approx using last 50 atr values via deque scaled vs current)
                // Simpler: compare current ATR vs avg of last 50 atr_history -- we don't
                // maintain that history per bar. Approx: skip vol gate at runtime, rely
                // on ATR ready + non-zero. SPX trend used directly.
                int tr = spx.trend_at_now();
                if (tr == 0) return 0;
                // sgn: +1 means LONG when SPX up; -1 inverse
                int side = cfg.spx_sgn * tr;
                // Confirm direction with Donchian (LONG only if not breaking down)
                double c = st.bars.back().c;
                if (side > 0 && c < d.lo) return 0;
                if (side < 0 && c > d.hi) return 0;
                return side;
            }
        }
        return 0;
    }

    void evaluate_signal(const SpxOverlay& spx, double mid, CloseCb cb,
                         const std::function<bool(const char*, int)>& side_taken = {}) {
        if (st.pos_active) return;
        if (!st.atr_ready) return;
        if (st.atr_val <= 0) return;
        if (st.bar_idx - st.last_entry_bar_idx <= cfg.cooldown_bars) return;
        int dir = compute_signal_dir(spx);
        if (dir == 0) return;
        // S-2026-06-03 dedup: block opening if a sibling survivor cell already
        // holds this symbol+side (correlated double-stack, e.g. DonchN20+N100,
        // USTEC RSI+ZMR). Exits are unaffected — this only gates new entries.
        if (side_taken && side_taken(cfg.symbol, dir)) {
            printf("[SURV-DEDUP] %s %s entry blocked — sibling already open same symbol/side\n",
                   cfg.tag, dir > 0 ? "LONG" : "SHORT");
            fflush(stdout);
            return;
        }
        // Open
        const double sl_dist = cfg.sl_mult * st.atr_val;
        const double tp_dist = cfg.tp_mult * st.atr_val;
        st.pos_active = true;
        st.pos_side   = dir;
        st.pos_entry  = mid;
        st.pos_sl     = (dir > 0) ? mid - sl_dist : mid + sl_dist;
        st.pos_tp     = (dir > 0) ? mid + tp_dist : mid - tp_dist;
        st.pos_mfe    = 0;
        st.pos_bars_held = 0;
        st.pos_entry_bar_idx = st.bar_idx;
        st.pos_entry_ts = st.bars.back().ts_sec;
        // store the broken channel level for the reclaim (failed-breakout) exit
        if (cfg.family == Family::Donchian) {
            auto dch = compute_donch(st.bars, cfg.N);
            st.pos_break_lvl = (dir < 0) ? dch.lo : dch.hi;
        }
        st.last_entry_bar_idx = st.bar_idx;
        printf("[SURV-OPEN] %s %s entry=%.5f sl=%.5f tp=%.5f atr=%.5f bar_idx=%d%s\n",
               cfg.tag, dir > 0 ? "LONG" : "SHORT",
               st.pos_entry, st.pos_sl, st.pos_tp, st.atr_val, st.bar_idx,
               st.shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);
        // SURVIVOR-CELL MIMIC (S-2026-07-14h): one-way fire-and-forget notify. Spawns
        // an INDEPENDENT mimic leg book that never reads/moves/closes this position
        // (feedback-companion-independent-engine). Only cell tags registered in
        // engine_init spawn legs (XAU_4h_DonchN20, USTEC_4h_ZMR); others no-op, and
        // the registry is disarmed until post-seed so historical replays can't fire.
        omega::gold_trend_mimic().on_trend_open(cfg.tag, dir, st.pos_entry, st.pos_entry_ts);
        (void)cb;
    }

    void manage_position(double bid, double ask, double mid, int64_t now_s,
                         CloseCb cb) {
        const double move = (st.pos_side > 0) ? (mid - st.pos_entry) : (st.pos_entry - mid);
        if (move > st.pos_mfe) st.pos_mfe = move;
        // Bar-count-based timeout: bars elapsed since entry
        int bars_held = st.bar_idx - st.pos_entry_bar_idx;
        bool sl_hit = (st.pos_side > 0) ? (bid <= st.pos_sl) : (ask >= st.pos_sl);
        bool tp_hit = (st.pos_side > 0) ? (bid >= st.pos_tp) : (ask <= st.pos_tp);
        bool timeout = bars_held >= cfg.max_hold_bars;
        // Failed-breakout (reclaim) exit — Donchian only, checked AFTER SL/TP so it
        // only acts when price closed back through the broken level WITHOUT hitting
        // the stop. Bar-close signal: uses the last completed bar's close. Gated to
        // shorts + the first reclaim_max_bars bars (the validated robust config).
        bool reclaim_hit = false;
        if (cfg.reclaim_exit && cfg.family == Family::Donchian && !st.bars.empty()
                && bars_held <= cfg.reclaim_max_bars
                && (!cfg.reclaim_short_only || st.pos_side < 0)) {
            const double last_c = st.bars.back().c;
            reclaim_hit = (st.pos_side < 0) ? (last_c > st.pos_break_lvl)
                                            : (last_c < st.pos_break_lvl);
        }
        if (!sl_hit && !tp_hit && !reclaim_hit && !timeout) return;
        const double exit_px = sl_hit ? st.pos_sl : (tp_hit ? st.pos_tp : mid);
        const char* why = sl_hit ? "SL_HIT"
                        : (tp_hit ? "TP_HIT" : (reclaim_hit ? "RECLAIM" : "TIMEOUT"));
        const double gross_pts = (st.pos_side > 0) ? (exit_px - st.pos_entry)
                                                   : (st.pos_entry - exit_px);
        const double gross_usd = gross_pts * cfg.tick_usd_per_lot * cfg.lot;  // engine-internal USD (stats/log only)
        // S-2026-06-11 DOUBLE-MULTIPLY FIX: the ledger (trade_lifecycle.hpp Step 1)
        // multiplies tr.pnl by tick_value_multiplier(sym). cfg.tick_usd_per_lot is
        // an exact local copy of that multiplier (XAU 100, USDJPY 667, GER40 1.10,
        // USTEC 20), so emitting gross_usd into tr.pnl made the ledger DOUBLE-
        // multiply every cell -- USDJPY showed $2775.88 vs true $4.46, XAU cells
        // 100x, USTEC 20x, GER40 +10%. Emit RAW price_pts * lot; the ledger applies
        // the contract size. (Same gotcha class fixed earlier in GoldOversold/
        // GoldSeasonal/GSP -- SurvivorPortfolio was missed.)
        const double gross_raw = gross_pts * cfg.lot;
        // Build TradeRecord
        omega::TradeRecord tr;
        tr.symbol     = cfg.symbol;
        tr.side       = (st.pos_side > 0) ? "LONG" : "SHORT";
        tr.engine     = cfg.tag;
        tr.entryPrice = st.pos_entry;
        tr.exitPrice  = exit_px;
        tr.sl         = st.pos_sl;
        tr.size       = cfg.lot;
        tr.pnl        = gross_raw;   // RAW pts*lot -- ledger applies tick_value_multiplier
        tr.mfe        = st.pos_mfe;
        tr.entryTs    = st.pos_entry_ts;
        tr.exitTs     = now_s;
        tr.exitReason = why;
        tr.regime     = "SURV";
        tr.shadow     = st.shadow_mode;
        ++st.n_trades;
        if (gross_usd > 0) ++st.n_wins;
        st.cum_net += gross_usd;
        printf("[SURV-CLOSE] %s %s entry=%.5f exit=%.5f pts=%.5f $=%+.2f why=%s mfe=%.5f bars=%d cum_n=%d cum_$=%+.2f%s\n",
               cfg.tag, st.pos_side > 0 ? "LONG" : "SHORT",
               st.pos_entry, exit_px, gross_pts, gross_usd, why,
               st.pos_mfe, bars_held, st.n_trades, st.cum_net,
               st.shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);
        st.pos_active = false;
        st.pos_side = 0;
        if (cb) cb(tr);
    }
};

// -----------------------------------------------------------------------------
// Portfolio -- 13 cells + global SPX overlay
// -----------------------------------------------------------------------------
class Portfolio {
public:
    SpxOverlay spx;
    std::vector<Cell> cells;
    bool enabled = true;
    // S-2026-06-03: per-(symbol,side) concurrency cap toggle. Default OFF —
    // backtest (survivor_cap_test.cpp) showed the cap is NET-NEGATIVE on the
    // trending sample (net -29%, PF 1.37->1.29, maxDD only -12%): a trend book
    // on a trending tape profits from same-side cell stacking (riding winners),
    // and the cap cuts that. Mechanism retained for a future regime-gated cap
    // (would help in chop/crash). See memory: omega-survivor-cap-deadend.
    bool dedup_enabled = false;
    // S-2026-06-16: the "future regime-gated cap" above, now built.
    //   dedup_mode: 0=off, 1=always-blanket (proven net-negative on trend),
    //   2=regime-gated -> cap ON only when the symbol is CHOPPY (correlated cells
    //   whipsaw together), OFF in trend (same-side stacking rides the winner).
    //   Regime = per-symbol Kaufman ER < er_chop_thr. dedup_enabled=true maps to
    //   mode 1 for back-compat with the original A/B harness.
    int    dedup_mode  = 0;
    int    er_window   = 10;
    double er_chop_thr = 0.25;   // validated (survivor_cap_test.cpp ER sweep
    //   2026-06-16): at <=0.25 regime-gated net >= OFF on the trend tape (no
    //   edge damage, unlike blanket); >=0.30 starts cutting trend stacks. Caps
    //   only genuine extreme-chop -> the chop/crash double-stack protection
    //   without the blanket cap's -29% trend cost.
    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    // S-2026-07-08 bear-chokepoint hook (tombstone valid-use re-enable): optional
    // per-(symbol,side) entry veto composed IN FRONT of the dedup side_taken gate.
    // Return true = block the NEW entry (exits/management unaffected). Wired from
    // engine_init to the central regime gates (index_market_regime long-block for
    // USTEC cells); replicated identically by the gated backtest harness. This
    // closes the 2026-06-24 cull reason "self-enters, bypassed the bear chokepoint".
    std::function<bool(const char*, int)> entry_veto = {};

    // S-2026-06-03: adopt a persisted position into the matching cell (by tag).
    // Returns true if a cell claimed it. Wired to g_open_positions restorer.
    bool adopt(const omega::PositionSnapshot& ps) {
        for (auto& c : cells) {
            if (ps.engine == std::string(c.cfg.tag)) {
                if (c.st.pos_active) return true;   // already holding — don't double
                c.adopt(ps);
                return true;
            }
        }
        return false;
    }

    void init_default_cells() {
        // Per-symbol unit costs (price-unit + USD-per-pt-per-lot)
        struct U { double pt; double usd; };
        auto U_for = [](const char* sym) -> U {
            if (strcmp(sym, "XAUUSD") == 0) return { 0.01,   100.0 };
            if (strcmp(sym, "USDJPY") == 0) return { 0.01,   667.0 };
            if (strcmp(sym, "GER40")  == 0) return { 0.10,     1.10 };
            if (strcmp(sym, "USTEC")  == 0 || strcmp(sym, "USTEC.F") == 0)
                                            return { 0.10,    20.0 };
            return { 0.01, 100.0 };
        };
        auto add = [&](CellCfg c) {
            auto u = U_for(c.symbol);
            c.pt_size = u.pt;
            c.tick_usd_per_lot = u.usd;
            cells.emplace_back(c);
        };
        // All designated to satisfy MSVC's strict designated-init rule.
        // 1.  XAUUSD 4h DonchN20
        add({ .tag="XAU_4h_DonchN20",  .symbol="XAUUSD",  .tf_sec=14400, .family=Family::Donchian, .N=20,  .sl_mult=1.5, .tp_mult=3.0, .max_hold_bars=30, .reclaim_exit=true, .lot=0.01 });
        // 2.  XAUUSD 4h DonchN100
        // CULLED 2026-06-18 (faithful survivor_faithful_revalidate.cpp): n=2 over 2yr H4 = dead
        // (slow N100 Donchian barely fires + dedup gates it behind DonchN20 same symbol/side).
        // Pure noise in the book; removed. Was: add({ .tag="XAU_4h_DonchN100", .symbol="XAUUSD", .tf_sec=14400, .family=Family::Donchian, .N=100, .sl_mult=1.5, .tp_mult=3.0, .max_hold_bars=30, .reclaim_exit=true, .lot=0.01 });
        // 3.  GER40  4h RSI N=7
        // DISABLED 2026-06-01: GER40 RSI mean-rev shorts the RISK_ON uptrend -> net-negative shadow (counter-trend dead pattern). GER40 edge is trend (KeltnerH1/TurtleH4/MACross).
        // add({ .tag="GER_4h_RSI_N7",    .symbol="GER40",   .tf_sec=14400, .family=Family::RSI, .N=7,  .lo=30, .hi=70, .sl_mult=1.0, .tp_mult=2.0, .max_hold_bars=30, .lot=0.10 });
        // 4.  GER40  15m MACross 10/30
        // CULLED 2026-06-15 (GER40 dropped, losing live): add({ .tag="GER_15m_MA_10_30", .symbol="GER40",   .tf_sec=900,   .family=Family::MACross, .N=30, .N_fast=10, .sl_mult=1.5, .tp_mult=3.0, .max_hold_bars=50, .lot=0.10 });
        // 5.  GER40  1h RSI N=14
        // DISABLED 2026-06-01: GER40 RSI mean-rev (counter-trend, net-neg shadow).
        // add({ .tag="GER_1h_RSI_N14",   .symbol="GER40",   .tf_sec=3600,  .family=Family::RSI, .N=14, .lo=30, .hi=70, .sl_mult=1.0, .tp_mult=2.0, .max_hold_bars=30, .lot=0.10 });
        // 6.  GER40  1h DonchN100
        // CULLED 2026-06-15 (GER40 dropped, losing live): add({ .tag="GER_1h_DonchN100", .symbol="GER40",   .tf_sec=3600,  .family=Family::Donchian, .N=100, .sl_mult=1.5, .tp_mult=3.0, .max_hold_bars=30, .lot=0.10 });
        // 7.  XAUUSD 4h MACross 10/30
        // CULLED 2026-06-18 (faithful survivor_faithful_revalidate.cpp): n=67 PF1.07 net+$120
        // but top3=437% — the entire net is 3 fat-tail trades; strip them and it's negative.
        // Not an edge, just noise the book carried. Was: add({ .tag="XAU_4h_MA_10_30", .symbol="XAUUSD", .tf_sec=14400, .family=Family::MACross, .N=30, .N_fast=10, .sl_mult=1.5, .tp_mult=3.0, .max_hold_bars=30, .lot=0.01 });
        // 8.  USTEC  4h RSI N=7  — TREND-GATED 2026-06-03 (Family::RSI trend_dir
        //     filter blocks counter-trend fades; was bleeding -$428 shorting the
        //     USTEC uptrend before the gate). Re-enabled gated for shadow eval.
        add({ .tag="USTEC_4h_RSI_N7",  .symbol="USTEC.F", .tf_sec=14400, .family=Family::RSI, .N=7,  .lo=30, .hi=70, .sl_mult=1.0, .tp_mult=2.0, .max_hold_bars=30, .lot=0.10 });
        // 9.  GER40  15m MACross 20/50
        // DISABLED 2026-06-02: redundant duplicate of GER_15m_MA_10_30 (cell 4).
        // Same symbol/TF(15m)/family(MACross), overlapping params, lower Sharpe
        // (0.99 vs 10_30's 1.96). Live shadow tape 2026-06-02 12:30 showed BOTH
        // cells firing the IDENTICAL trade (entry 25274.40 -> SL 25164.97,
        // -$13.24 each) = correlated double-loss, zero diversification. Keep the
        // higher-Sharpe 10_30; cull this one (dedup, per S46/S37 precedent).
        // add({ .tag="GER_15m_MA_20_50", .symbol="GER40",   .tf_sec=900,   .family=Family::MACross, .N=50, .N_fast=20, .sl_mult=1.5, .tp_mult=3.0, .max_hold_bars=50, .lot=0.10 });
        // 10. USTEC  4h ZScoreMR W=20 Z=2.5  — TREND-GATED 2026-06-03 (Family::
        //     ZScoreMR trend_dir filter blocks counter-trend fades; was firing the
        //     identical -$428 short as the RSI cell into the USTEC uptrend). Note:
        //     RSI_N7 + ZMR can still co-fire when gate passes -> consider a per-
        //     symbol cell cap (follow-up) to avoid correlated double-stacking.
        add({ .tag="USTEC_4h_ZMR",     .symbol="USTEC.F", .tf_sec=14400, .family=Family::ZScoreMR, .N=20, .zthr=2.5, .sl_mult=1.0, .tp_mult=2.0, .max_hold_bars=30, .lot=0.10 });
        // 11. GER40  5m RSI N=14
        // DISABLED 2026-06-01: GER40 RSI mean-rev (counter-trend, net-neg shadow).
        // add({ .tag="GER_5m_RSI_N14",   .symbol="GER40",   .tf_sec=300,   .family=Family::RSI, .N=14, .lo=30, .hi=70, .sl_mult=1.0, .tp_mult=2.0, .max_hold_bars=30, .lot=0.10 });
        // 12. GER40  30m RSI N=14/20/80
        // DISABLED 2026-06-01: GER40 RSI mean-rev (counter-trend, net-neg shadow).
        // add({ .tag="GER_30m_RSI_N14",  .symbol="GER40",   .tf_sec=1800,  .family=Family::RSI, .N=14, .lo=20, .hi=80, .sl_mult=1.0, .tp_mult=2.0, .max_hold_bars=30, .lot=0.10 });
        // 13. USDJPY 4h SPX_VolGated sl=2/tp=5
        // TOMBSTONED 2026-06-11 (operator: "trading 8 days for $4 is plain stupid").
        // Structural outlier of the whole roster: only cell with tp_mult=5.0 (rest
        // 2-3) and max_hold_bars=48 / 8 days (rest 30). A 5-ATR target on 4h USDJPY
        // almost never resolves -> trades die by TIMEOUT near-flat. Its only live
        // shadow trade confirmed the pathology: 192h hold -> TIMEOUT, true PnL
        // $4.46 (displayed $2775.88 under the now-fixed double-multiply bug). The
        // sweep "Sharpe 2.58" is an unvalidated, harness-optimism number with zero
        // live confirmation; tp=5ATR sweep cells overfit the rare runner. Thin
        // single-symbol/single-family cell -> no diversification cost to cull.
        // add({ .tag="USDJPY_4h_SPXVG",  .symbol="USDJPY",  .tf_sec=14400, .family=Family::SPXVolGatedDonch, .N=20, .spx_sgn=+1, .spx_vm=1.2, .sl_mult=2.0, .tp_mult=5.0, .max_hold_bars=48, .lot=0.01 });
    }

    void seed_all(const std::string& base_dir) {
        for (auto& c : cells) {
            // Map cell symbol+tf to bundled CSV path
            std::string path;
            const char* sym = c.cfg.symbol;
            int tf = c.cfg.tf_sec;
            const char* tf_tag = (tf == 14400) ? "H4"
                              : (tf == 3600 ) ? "H1"
                              : (tf == 1800 ) ? "M30"
                              : (tf == 900  ) ? "M15"
                              : (tf == 300  ) ? "M5" : "";
            path = base_dir + "/warmup_" + sym + "_" + tf_tag + ".csv";
            c.seed_from_csv(path);
        }
        spx.seed_from_csv(base_dir + "/warmup_SPXUSD_H4.csv");
    }

    void on_tick(const std::string& sym, double bid, double ask, int64_t now_ms,
                 CloseCb cb) noexcept {
        if (!enabled) return;
        // S-2026-06-03: per-(symbol,side) concurrency cap. A cell may open only
        // if no sibling already holds the same symbol+side — kills correlated
        // double-stacking (XAU DonchN20+N100, USTEC RSI+ZMR). Scans live cell
        // state so within-tick opens are seen by later cells in the loop. Exits
        // always run (the cap gates entry only, inside evaluate_signal).
        const int eff_mode = dedup_mode != 0 ? dedup_mode : (dedup_enabled ? 1 : 0);
        std::function<bool(const char*, int)> side_taken = {};
        if (eff_mode != 0 || entry_veto) {
            side_taken = [this, eff_mode](const char* csym, int side) -> bool {
                // S-2026-07-08: regime chokepoint first — vetoed entries never
                // reach the dedup scan. Exits are unaffected (gate lives inside
                // evaluate_signal's entry path only).
                if (entry_veto && entry_veto(csym, side)) return true;
                if (eff_mode == 0) return false;   // veto-only mode (no dedup cap)
                // mode 2 (regime-gated): cap only in CHOP; in a trend let same-side
                // cells stack (the proven edge). Chop = per-symbol Kaufman ER low.
                if (eff_mode == 2) {
                    double er = 1.0;
                    for (const auto& c : cells)
                        if (std::strcmp(c.sym_cstr(), csym) == 0) { er = c.efficiency_ratio(er_window); break; }
                    if (er >= er_chop_thr) return false;   // trending -> no cap, stack away
                }
                for (const auto& c : cells)
                    if (c.open_side() == side && std::strcmp(c.sym_cstr(), csym) == 0)
                        return true;
                return false;
            };
        }
        for (auto& c : cells) c.on_tick(sym, bid, ask, now_ms, spx, cb, side_taken);

        // S-2026-06-17 dedup-RESOLVE: the entry gate (side_taken) blocks NEW
        // double-stacks, but legacy positions (opened before the gate existed) and
        // same-bar simultaneous opens (e.g. XAU DonchN20 + DonchN100 on one
        // breakout) can leave two cells holding the same symbol+side. Keep the
        // FIRST cell in registration order; market-close the rest via the cell's
        // normal exit path. Only in blanket mode (eff_mode==1) -- mode 2 (regime-
        // gated) deliberately allows same-side stacking in trends, so it is not
        // resolved here. Operator policy is blanket (mode 1) since 85952e16.
        if (eff_mode == 1) {
            for (size_t i = 0; i < cells.size(); ++i) {
                if (!cells[i].has_open()) continue;
                // CRITICAL: only resolve cells of the CURRENT tick's symbol -- the
                // bid/ask passed in are for `sym`, so closing a cell of a different
                // symbol would book the wrong-symbol price (S-2026-06-17 bugfix:
                // first cut closed a XAU short at the EURUSD mid -> phantom +$4293).
                if (std::strcmp(cells[i].sym_cstr(), sym.c_str()) != 0) continue;
                for (size_t j = i + 1; j < cells.size(); ++j) {
                    if (cells[j].has_open()
                        && std::strcmp(cells[j].sym_cstr(), sym.c_str()) == 0
                        && cells[j].open_side() == cells[i].open_side()) {
                        cells[j].force_close_dup(bid, ask, now_ms / 1000, cb);
                    }
                }
            }
        }
        // Also feed SPX overlay if SPX tick passes through
        if (sym == "US500.F" || sym == "SPXUSD" || sym == "US500") {
            // Approximate SPX from US500 mid: aggregate into H4 bar.
            // SPX overlay seeded from CSV at boot; live updates happen in
            // on_tick of US500. To minimise drift, feed a single H4 close
            // when the bar closes here.
            // (Live update done inline by on_tick driver as bars close.)
        }
    }
};

}  // namespace omega::survivor

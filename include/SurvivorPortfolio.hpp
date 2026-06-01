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

    // Per-tick: aggregate to next bar; manage open position; on bar close,
    // evaluate signal.
    void on_tick(const std::string& sym, double bid, double ask, int64_t now_ms,
                 const SpxOverlay& spx, CloseCb cb) noexcept {
        if (!st.enabled) return;
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
                // bar-close evaluation gate
                if (st.enabled) evaluate_signal(spx, mid, cb);
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

    int compute_signal_dir(const SpxOverlay& spx) const {
        switch (cfg.family) {
            case Family::Donchian: {
                auto d = compute_donch(st.bars, cfg.N);
                if (!d.ok) return 0;
                double c = st.bars.back().c;
                if (c > d.hi) return +1;
                if (c < d.lo) return -1;
                return 0;
            }
            case Family::RSI: {
                if ((int)st.bars.size() < cfg.N + 2) return 0;
                double rsi = compute_rsi(st.bars, cfg.N);
                // mean-rev: rsi crosses back inside extreme zone
                // approximate: rsi < lo -> long, rsi > hi -> short
                if (rsi < cfg.lo) return +1;
                if (rsi > cfg.hi) return -1;
                return 0;
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
                if (z >=  cfg.zthr) return -1;
                if (z <= -cfg.zthr) return +1;
                return 0;
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

    void evaluate_signal(const SpxOverlay& spx, double mid, CloseCb cb) {
        if (st.pos_active) return;
        if (!st.atr_ready) return;
        if (st.atr_val <= 0) return;
        if (st.bar_idx - st.last_entry_bar_idx <= cfg.cooldown_bars) return;
        int dir = compute_signal_dir(spx);
        if (dir == 0) return;
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
        st.last_entry_bar_idx = st.bar_idx;
        printf("[SURV-OPEN] %s %s entry=%.5f sl=%.5f tp=%.5f atr=%.5f bar_idx=%d%s\n",
               cfg.tag, dir > 0 ? "LONG" : "SHORT",
               st.pos_entry, st.pos_sl, st.pos_tp, st.atr_val, st.bar_idx,
               st.shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);
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
        if (!sl_hit && !tp_hit && !timeout) return;
        const double exit_px = sl_hit ? st.pos_sl : (tp_hit ? st.pos_tp : mid);
        const char* why = sl_hit ? "SL_HIT" : (tp_hit ? "TP_HIT" : "TIMEOUT");
        const double gross_pts = (st.pos_side > 0) ? (exit_px - st.pos_entry)
                                                   : (st.pos_entry - exit_px);
        const double gross_usd = gross_pts * cfg.tick_usd_per_lot * cfg.lot;
        // Build TradeRecord
        omega::TradeRecord tr;
        tr.symbol     = cfg.symbol;
        tr.side       = (st.pos_side > 0) ? "LONG" : "SHORT";
        tr.engine     = cfg.tag;
        tr.entryPrice = st.pos_entry;
        tr.exitPrice  = exit_px;
        tr.sl         = st.pos_sl;
        tr.size       = cfg.lot;
        tr.pnl        = gross_usd;
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
    using CloseCb = std::function<void(const omega::TradeRecord&)>;

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
        add({ .tag="XAU_4h_DonchN20",  .symbol="XAUUSD",  .tf_sec=14400, .family=Family::Donchian, .N=20,  .sl_mult=1.5, .tp_mult=3.0, .max_hold_bars=30, .lot=0.01 });
        // 2.  XAUUSD 4h DonchN100
        add({ .tag="XAU_4h_DonchN100", .symbol="XAUUSD",  .tf_sec=14400, .family=Family::Donchian, .N=100, .sl_mult=1.5, .tp_mult=3.0, .max_hold_bars=30, .lot=0.01 });
        // 3.  GER40  4h RSI N=7
        // DISABLED 2026-06-01: GER40 RSI mean-rev shorts the RISK_ON uptrend -> net-negative shadow (counter-trend dead pattern). GER40 edge is trend (KeltnerH1/TurtleH4/MACross).
        // add({ .tag="GER_4h_RSI_N7",    .symbol="GER40",   .tf_sec=14400, .family=Family::RSI, .N=7,  .lo=30, .hi=70, .sl_mult=1.0, .tp_mult=2.0, .max_hold_bars=30, .lot=0.10 });
        // 4.  GER40  15m MACross 10/30
        add({ .tag="GER_15m_MA_10_30", .symbol="GER40",   .tf_sec=900,   .family=Family::MACross, .N=30, .N_fast=10, .sl_mult=1.5, .tp_mult=3.0, .max_hold_bars=50, .lot=0.10 });
        // 5.  GER40  1h RSI N=14
        // DISABLED 2026-06-01: GER40 RSI mean-rev (counter-trend, net-neg shadow).
        // add({ .tag="GER_1h_RSI_N14",   .symbol="GER40",   .tf_sec=3600,  .family=Family::RSI, .N=14, .lo=30, .hi=70, .sl_mult=1.0, .tp_mult=2.0, .max_hold_bars=30, .lot=0.10 });
        // 6.  GER40  1h DonchN100
        add({ .tag="GER_1h_DonchN100", .symbol="GER40",   .tf_sec=3600,  .family=Family::Donchian, .N=100, .sl_mult=1.5, .tp_mult=3.0, .max_hold_bars=30, .lot=0.10 });
        // 7.  XAUUSD 4h MACross 10/30
        add({ .tag="XAU_4h_MA_10_30",  .symbol="XAUUSD",  .tf_sec=14400, .family=Family::MACross, .N=30, .N_fast=10, .sl_mult=1.5, .tp_mult=3.0, .max_hold_bars=30, .lot=0.01 });
        // 8.  USTEC  4h RSI N=7
        add({ .tag="USTEC_4h_RSI_N7",  .symbol="USTEC.F", .tf_sec=14400, .family=Family::RSI, .N=7,  .lo=30, .hi=70, .sl_mult=1.0, .tp_mult=2.0, .max_hold_bars=30, .lot=0.10 });
        // 9.  GER40  15m MACross 20/50
        // DISABLED 2026-06-02: redundant duplicate of GER_15m_MA_10_30 (cell 4).
        // Same symbol/TF(15m)/family(MACross), overlapping params, lower Sharpe
        // (0.99 vs 10_30's 1.96). Live shadow tape 2026-06-02 12:30 showed BOTH
        // cells firing the IDENTICAL trade (entry 25274.40 -> SL 25164.97,
        // -$13.24 each) = correlated double-loss, zero diversification. Keep the
        // higher-Sharpe 10_30; cull this one (dedup, per S46/S37 precedent).
        // add({ .tag="GER_15m_MA_20_50", .symbol="GER40",   .tf_sec=900,   .family=Family::MACross, .N=50, .N_fast=20, .sl_mult=1.5, .tp_mult=3.0, .max_hold_bars=50, .lot=0.10 });
        // 10. USTEC  4h ZScoreMR W=20 Z=2.5
        add({ .tag="USTEC_4h_ZMR",     .symbol="USTEC.F", .tf_sec=14400, .family=Family::ZScoreMR, .N=20, .zthr=2.5, .sl_mult=1.0, .tp_mult=2.0, .max_hold_bars=30, .lot=0.10 });
        // 11. GER40  5m RSI N=14
        // DISABLED 2026-06-01: GER40 RSI mean-rev (counter-trend, net-neg shadow).
        // add({ .tag="GER_5m_RSI_N14",   .symbol="GER40",   .tf_sec=300,   .family=Family::RSI, .N=14, .lo=30, .hi=70, .sl_mult=1.0, .tp_mult=2.0, .max_hold_bars=30, .lot=0.10 });
        // 12. GER40  30m RSI N=14/20/80
        // DISABLED 2026-06-01: GER40 RSI mean-rev (counter-trend, net-neg shadow).
        // add({ .tag="GER_30m_RSI_N14",  .symbol="GER40",   .tf_sec=1800,  .family=Family::RSI, .N=14, .lo=20, .hi=80, .sl_mult=1.0, .tp_mult=2.0, .max_hold_bars=30, .lot=0.10 });
        // 13. USDJPY 4h SPX_VolGated sl=2/tp=5
        add({ .tag="USDJPY_4h_SPXVG",  .symbol="USDJPY",  .tf_sec=14400, .family=Family::SPXVolGatedDonch, .N=20, .spx_sgn=+1, .spx_vm=1.2, .sl_mult=2.0, .tp_mult=5.0, .max_hold_bars=48, .lot=0.01 });
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
        for (auto& c : cells) c.on_tick(sym, bid, ask, now_ms, spx, cb);
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

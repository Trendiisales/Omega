#pragma once
// =============================================================================
//  GoldBullTrendGatedEngine.hpp -- XAU/MGC long-only, bull-regime-GATED trend
//  ensemble. Two independent cells on two timeframes:
//
//     [DONCH]  1-hour Donchian(20) breakout (Turtle). Long on a stop-order above
//              the prior 20-bar high. Exit: ATR(14)x2 hard stop, monotone
//              opposite-Donchian(10) trail, chandelier(3xATR) trail.
//     [EMA]    30-min EMA(20/50) regime cross. Long on the bull cross (market-on-
//              open next bar). Exit: opposite (bear) cross, ATR(14)x2 hard stop,
//              chandelier(3xATR) trail.
//
//  PROVENANCE (S-2026-07-20, this build).
//  Certified on HONEST gap-through fills (backtest/gold_ls_gated_bt.cpp, the
//  salvaged edge-neutral harness gold_ls_harness.hpp) over two XAU tick regimes:
//    * BULL  = /Users/jo/Tick/xau_6mo_corrected.csv  (2025-11 .. 2026-04)
//    * BEAR  = /Users/jo/Tick/xau_2022bear_tick.csv  (2022-06 .. 2022-09)
//  UNGATED (long): DONCH bull +2228bp PF2.12 / bear -534bp PF0.53;
//                  EMA   bull +1561bp PF2.13 / bear -362bp PF0.51.
//  => Ungated it is BULL BETA (long a rising asset), not an all-weather edge: it
//     bleeds in the 2022 bear. THAT IS WHY THIS ENGINE IS GATED, long-only, and
//     SHADOW. Do NOT present the ungated bull figure as the engine's edge.
//
//  GATES (both applied at entry; either vetoes a long):
//    (1) SHARED sustained-bear brain omega::gold_regime().long_blocked()
//        (RegimeState.hpp: close<EMA200_H1 AND EMA200 falling 100 bars AND
//        EMA50<EMA200). Warm-seeded + restart-persistent, fed globally in
//        tick_gold.hpp. On the cert tape it lifts DONCH bear -534->-176 /
//        EMA bear -362->-180 while keeping ~90%/55% of bull edge.
//    (2) SLOW-SMA bull-trend confirm on the cell timeframe (DONCH: SMA400 of 1h
//        closes ~16d; EMA: SMA800 of 30m closes ~16d). Only long when close>SMA.
//        With BOTH gates the cert tape flattens the bear:
//          DONCH bull +1126bp PF1.64 (h1+1033/h2+94, 2x +881 PF1.46) / bear -13bp PF0.92
//          EMA   bull  +784bp PF1.98 (h1 +651/h2+134, 2x +619 PF1.68) / bear -70bp PF0.40
//        = bull edge preserved, bear ~flat, 2x-cost-robust, both halves positive.
//
//  HONEST LIMITATIONS (operator is distrustful -- state these, never bury them):
//    * ONE bull window + ONE bear window only. The gate is PROVEN to flatten
//      THIS 2022 bear and preserve THIS bull; multi-window certification is
//      still OWED before any live promotion. SHADOW until then.
//    * MGC has NO minute/tick data anywhere in /Users/jo/Tick (only H1/H4/30m).
//      The MGC instance is an XAU PROXY (MGC~=XAU, corr~=1); it inherits the XAU
//      verdict and CANNOT be independently certified on the data on hand.
//    * Bear-backtest UNDER-counts the regime gate's protection (the gate cold-
//      warms ~12d into the bear file; live warm-seed makes it protect from t0).
//
//  ADVERSE-PROTECTION: (S-2026-07-20 -- backtested verdict, honest gap-through
//    fills.) VERDICT = ATR(14)x2 HARD STOP + chandelier(3xATR) monotone trail +
//    (DONCH) opposite-Donchian(10) trail; NO cold %-loss cut. The stop+trail is
//    the in-flight protection and it is what the cert net figures already
//    INCLUDE (bull +1126/+784, bear ~flat under the dual gate). A tighter cold
//    cut was NOT added: on trend-runner cells a cold cut lowers net (the standing
//    2026-06-17 swing-protection finding); the trail IS the protection here.
//
//  SAFETY / SEPARATION:
//    * shadow_mode=true by default; engine_init sets it. Long-only by design.
//    * Self-contained: does NOT read/modify any other engine's state, does NOT
//      touch a protected core file. Emits closed trades to the shared ledger via
//      the on_close callback (same convention as XauTrendFollow1hEngine).
//    * ExecutionCostGuard::is_viable() gates every entry.
//    * Fills are RAW (pre-cost) levels + RAW pnl (ledger applies realistic cost
//      downstream, exactly like XauTrendFollow1hEngine::_close) -- so the shadow
//      ledger never double-counts cost, and raw levels match the cert 1:1.
//    * MGC VENUE PORT: ledger_prefix/ledger_symbol default to the XAU spot
//      instance byte-for-byte; the MGC instance sets "MGC" so the cost gate +
//      ledger key the MGC cost row instead of the spot proxy.
//
//  USAGE (globals.hpp):  static omega::GoldBullTrendGatedEngine g_gold_bull_trend;
//  engine_init.hpp:      g_gold_bull_trend.shadow_mode = kShadowDefault;
//                        g_gold_bull_trend.enabled = true;
//                        g_gold_bull_trend.init();
//                        g_gold_bull_trend.warmup();          // seeds both cells
//  tick_gold.hpp (per-tick, ~L1731):
//                        g_gold_bull_trend.on_tick(bid, ask, now_ms_g, bracket_on_close);
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <limits>
#include <string>

#include "OmegaTradeLedger.hpp"   // omega::TradeRecord
#include "OmegaCostGuard.hpp"     // omega::ExecutionCostGuard::is_viable
#include "RegimeState.hpp"        // omega::gold_regime() sustained-bear brain
#include "SeedGuard.hpp"          // omega::resolve_seed_path (VPS-cwd robust seed)

namespace omega {

// ----------------------------------------------------------------------------
//  Self-contained, edge-neutral helpers (behaviour identical to the certified
//  gold_ls_harness.hpp). Kept private to this engine so it depends on no
//  backtest/ header.
// ----------------------------------------------------------------------------
namespace gbt {

constexpr double kBpDiv = 10000.0;

struct Bar { int64_t start = 0, end = 0; double open = 0, high = 0, low = 0, close = 0; };

// EMA with a readiness gate at `period` samples.
struct EMA {
    double alpha = 0, v = 0; int period = 0; bool ready_ = false; uint64_t n = 0;
    explicit EMA(int p) : alpha(2.0 / (p + 1.0)), period(p) {}
    double update(double x) { if (!std::isfinite(x)) return v; if (!ready_) { v = x; ready_ = true; } else v += alpha * (x - v); ++n; return v; }
    [[nodiscard]] bool ready() const { return ready_ && n >= (uint64_t)period; }
    [[nodiscard]] double value() const { return v; }
};

// Wilder ATR on (high,low,close).
struct ATR {
    int period = 0; double atr = 0, sum = 0, pc = 0; bool has = false; uint64_t n = 0;
    explicit ATR(int p) : period(p) {}
    double update(const Bar& b) {
        double tr = b.high - b.low;
        if (has) tr = std::max({tr, std::fabs(b.high - pc), std::fabs(b.low - pc)});
        pc = b.close; has = true;
        if (n < (uint64_t)period) { sum += tr; ++n; atr = sum / n; }
        else { atr = ((atr * (period - 1)) + tr) / period; ++n; }
        return atr;
    }
    [[nodiscard]] bool ready() const { return n >= (uint64_t)period; }
    [[nodiscard]] double value() const { return atr; }
};

// Rolling SMA of the last N values.
struct SMA {
    int n = 0; std::deque<double> q; double sum = 0;
    explicit SMA(int p) : n(p) {}
    double update(double x) { q.push_back(x); sum += x; while ((int)q.size() > n) { sum -= q.front(); q.pop_front(); } return value(); }
    [[nodiscard]] bool ready() const { return n > 0 && (int)q.size() >= n; }
    [[nodiscard]] double value() const { return q.empty() ? 0.0 : sum / (double)q.size(); }
};

// Tick -> N-minute bar aggregator (mid price). Returns a completed bar on rollover.
struct TickAgg {
    int64_t secs = 0; bool active = false; int64_t start = 0; double o = 0, h = 0, l = 0, c = 0;
    explicit TickAgg(int minutes) : secs((int64_t)minutes * 60) {}
    bool update(double px, int64_t ts_sec, Bar& out) {
        const int64_t bucket = (ts_sec / secs) * secs;
        if (!active) { begin(bucket, px); return false; }
        if (bucket == start) { h = std::max(h, px); l = std::min(l, px); c = px; return false; }
        if (bucket < start) return false;                       // out-of-order tick: drop
        out = Bar{start, start + secs, o, h, l, c}; begin(bucket, px); return true;
    }
    void begin(int64_t b, double px) { active = true; start = b; o = h = l = c = px; }
};

} // namespace gbt

enum class GbtMode { DONCH, EMA };

// ----------------------------------------------------------------------------
//  One long-only cell (a mode + timeframe). Mirrors the certified harness'
//  per-bar causal logic EXACTLY: signal from COMPLETED bar b -> execute NEXT bar;
//  an open position is managed on bar b using indicator state only through b-1;
//  the protective stop is checked BEFORE the favourable-excursion update.
// ----------------------------------------------------------------------------
struct GoldBullTrendGatedEngine; // fwd

struct GbtCell {
    // ---- config (set by the owning engine) ----
    GbtMode mode = GbtMode::DONCH;
    int     bar_min = 60;
    int     atr_p = 14;
    int     don_n = 20, don_exit_n = 10;      // DONCH
    int     ema_fast = 20, ema_slow = 50;     // EMA
    double  sl_atr = 2.0, trail_atr = 3.0;
    int     slow_sma = 0;                      // 0 = SMA gate off
    double  viab_reward_atr = 3.0;             // expected reward (ATR mult) for the cost gate
    const char* name = "";

    // ---- indicators / state ----
    gbt::TickAgg agg{60};
    gbt::ATR atr{14};
    gbt::EMA ema_f{20}, ema_s{50};
    gbt::SMA sma{1};
    std::deque<double> hi, lo;                 // completed-bar highs/lows (Donchian window)

    // prev snapshot (through b-1) for causal management/entry
    double prev_atr = 0, prev_ex_lo = 0;
    bool   have_ema_dir = false, ema_prev_up = false;

    // position
    bool   in_pos = false, just_entered = false;
    double entry_raw = 0, stop_level = 0, risk_px = 0, hh_since = 0;
    int    bars_held = 0; int64_t entry_ts = 0;
    double mfe = 0, mae = 0;

    // pending entry armed from the previous completed bar
    bool   pend = false, pend_stop = false; double pend_level = 0;

    void configure() {
        agg = gbt::TickAgg(bar_min);
        atr = gbt::ATR(atr_p);
        ema_f = gbt::EMA(ema_fast); ema_s = gbt::EMA(ema_slow);
        sma = gbt::SMA(std::max(1, slow_sma));
        hi.clear(); lo.clear();
        prev_atr = prev_ex_lo = 0; have_ema_dir = ema_prev_up = false;
        in_pos = just_entered = pend = pend_stop = false;
        entry_raw = stop_level = risk_px = hh_since = 0; bars_held = 0; entry_ts = 0; mfe = mae = 0;
    }
};

// ----------------------------------------------------------------------------
struct GoldBullTrendGatedEngine {
    // ---- public knobs (set by engine_init before init()) ----
    bool   enabled     = false;
    bool   shadow_mode = true;
    double lot         = 0.01;
    double max_spread  = 1.0;                       // USD; refuse entries above
    uint32_t cell_enable_mask = 0x03;              // bit0=DONCH(1h), bit1=EMA(30m)
    bool   use_regime_gate = true;                 // omega::gold_regime().long_blocked()
    bool   use_sma_gate    = true;                 // slow-SMA bull-trend confirm

    // PARITY-ONLY (default false = production). When true, the entry filter uses
    // the certification harness' own cost-viability gate (reward_bp >=
    // parity_viab_mult * parity_base_cost_bp) and skips the production spread +
    // ExecutionCostGuard path, so a parity backtest reproduces the certified cell
    // trades 1:1. NEVER set true in a live/shadow build -- production must use the
    // real ExecutionCostGuard (+ daily-loss halt). Asserted off in engine_init.
    bool   bypass_cost_gate  = false;
    double parity_base_cost_bp = 5.0;
    double parity_viab_mult    = 1.5;

    // MGC venue port (defaults keep the spot instance byte-identical).
    std::string ledger_prefix = "GoldBullTrend_";
    std::string ledger_symbol = "XAUUSD";
    // Warm-seed CSVs per cell (ts,o,h,l,c; ts sec or ms). Empty => cold-warm.
    std::string donch_seed_csv = "phase1/signal_discovery/warmup_XAUUSD_H1.csv";
    std::string ema_seed_csv   = "phase1/signal_discovery/warmup_XAUUSD_M30.csv";

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    static constexpr int kNumCells = 2;
    GbtCell cells_[kNumCells];
    double last_bid_ = 0, last_ask_ = 0;
    bool   warmup_active_ = false;

    // ---- setup ----
    void init() noexcept {
        // Cell 0: DONCH 1h Donchian-20; Cell 1: EMA 30m 20/50.
        cells_[0] = GbtCell{}; cells_[0].mode = GbtMode::DONCH; cells_[0].bar_min = 60;
        cells_[0].don_n = 20; cells_[0].don_exit_n = 10; cells_[0].sl_atr = 2.0; cells_[0].trail_atr = 3.0;
        cells_[0].slow_sma = 400; cells_[0].name = "DONCH_1h_N20_trail3_S20";
        cells_[0].configure();

        cells_[1] = GbtCell{}; cells_[1].mode = GbtMode::EMA; cells_[1].bar_min = 30;
        cells_[1].ema_fast = 20; cells_[1].ema_slow = 50; cells_[1].sl_atr = 2.0; cells_[1].trail_atr = 3.0;
        cells_[1].slow_sma = 800; cells_[1].name = "EMA_30m_20_50_trail3_S20";
        cells_[1].configure();
    }

    bool any_open() const noexcept { for (auto& c : cells_) if (c.in_pos) return true; return false; }
    int  open_count() const noexcept { int n = 0; for (auto& c : cells_) if (c.in_pos) ++n; return n; }

    // ---- per-tick entry point (self-aggregates to each cell's TF) ----
    void on_tick(double bid, double ask, int64_t now_ms, OnCloseFn on_close) noexcept {
        if (!enabled) return;
        last_bid_ = bid; last_ask_ = ask;
        const double mid = (bid + ask) * 0.5;
        if (!(mid > 0.0)) return;
        const int64_t ts_sec = now_ms / 1000;
        for (int ci = 0; ci < kNumCells; ++ci) {
            gbt::Bar b;
            if (cells_[ci].agg.update(mid, ts_sec, b)) _on_bar(ci, b, now_ms, on_close);
        }
    }

    // ---- warm-seed both cells from their bar CSVs (entries suppressed) ----
    void warmup() noexcept {
        warmup_active_ = true;
        _seed_cell(0, donch_seed_csv);
        _seed_cell(1, ema_seed_csv);
        warmup_active_ = false;
    }

private:
    // Fold one COMPLETED bar into a cell: replicate the certified harness exactly.
    void _on_bar(int ci, const gbt::Bar& b, int64_t now_ms, OnCloseFn on_close) noexcept {
        GbtCell& e = cells_[ci];
        if (!(b.close > 0.0)) return;
        const bool cell_on = (cell_enable_mask & (1u << ci)) != 0;

        // ---- ctx snapshot as of b-1 (before folding b) ----
        const double ctx_atr = e.prev_atr;
        const double ctx_ex_lo = e.prev_ex_lo;
        const bool ind_ready = e.atr.ready() && (int)e.hi.size() >= e.don_n;

        // opposite-Donchian(exitN) trailing level for DONCH longs (through b-1)
        double trail = std::numeric_limits<double>::quiet_NaN();
        if (e.mode == GbtMode::DONCH && e.don_exit_n > 0 && (int)e.lo.size() >= e.don_exit_n)
            trail = ctx_ex_lo;

        // ---------- MANAGE (open) or EXECUTE (pending) on bar b ----------
        e.just_entered = false;
        if (e.in_pos) {
            _manage(ci, b, ctx_atr, trail, now_ms, on_close);
        } else if (e.pend) {
            double reward_bp = 0.0;
            if (ctx_atr > 0 && b.open > 0) reward_bp = e.viab_reward_atr * ctx_atr / b.open * gbt::kBpDiv;
            _execute(ci, b, ctx_atr, reward_bp, now_ms);
        }

        // ---------- fold bar b into indicators ----------
        const double a = e.atr.update(b);
        const double ef = e.ema_f.update(b.close);
        const double es = e.ema_s.update(b.close);
        const double sm = e.sma.update(b.close);
        e.hi.push_back(b.high); e.lo.push_back(b.low);
        const int maxwin = std::max({e.don_n, e.don_exit_n, 1});
        while ((int)e.hi.size() > maxwin) e.hi.pop_front();
        while ((int)e.lo.size() > maxwin) e.lo.pop_front();

        // recompute donchian over the freshest window (through b)
        double dhi = 0, dlo = 1e18, elo = 1e18;
        {
            const int nH = (int)e.hi.size();
            for (int k = std::max(0, nH - e.don_n); k < nH; ++k) { dhi = std::max(dhi, e.hi[k]); dlo = std::min(dlo, e.lo[k]); }
            for (int k = std::max(0, nH - e.don_exit_n); k < nH; ++k) elo = std::min(elo, e.lo[k]);
        }
        (void)dlo;

        // ---------- EMA opposite-cross exit (bear cross closes an open long) ----------
        if (e.mode == GbtMode::EMA && e.have_ema_dir && e.in_pos) {
            const bool up_now = ef > es;
            if (!up_now && e.ema_prev_up) _close(ci, b.close, "EMA_BEAR_CROSS", now_ms, on_close);
        }

        // ---------- ARM pending for NEXT bar from COMPLETED bar b ----------
        if (cell_on && !warmup_active_) _arm(ci, b, ind_ready, ef, es, sm, dhi);
        else e.pend = false;

        // advance prev snapshots to include b
        e.prev_atr = a; e.prev_ex_lo = (elo >= 1e17 ? 0 : elo);
        if (e.ema_f.ready() && e.ema_s.ready()) { e.ema_prev_up = ef > es; e.have_ema_dir = true; }
    }

    // Arm a pending long from completed bar b (applies the bull gates).
    void _arm(int ci, const gbt::Bar& b, bool ind_ready, double ef, double es, double sm, double dhi) noexcept {
        GbtCell& e = cells_[ci];
        if (e.in_pos || e.just_entered) { e.pend = false; return; }
        e.pend = false;
        if (!ind_ready) return;
        // (1) shared sustained-bear brain: no new long in a bear.
        if (use_regime_gate && omega::gold_regime().long_blocked()) return;
        // (2) slow-SMA bull-trend confirm on this cell's timeframe.
        if (use_sma_gate && e.slow_sma > 0) { if (!e.sma.ready() || !(b.close > sm)) return; }

        if (e.mode == GbtMode::DONCH) {
            e.pend = true; e.pend_stop = true; e.pend_level = dhi;      // stop-order above prior 20-bar high
        } else { // EMA: long on a fresh bull cross, market-on-open next bar
            if (!e.have_ema_dir) return;
            const bool up_now = ef > es;
            if (up_now && !e.ema_prev_up) { e.pend = true; e.pend_stop = false; }
        }
    }

    // Execute a pending long on bar b (raw fill levels; cost gate via ExecutionCostGuard).
    void _execute(int ci, const gbt::Bar& b, double atr, double reward_bp, int64_t now_ms) noexcept {
        GbtCell& e = cells_[ci];
        if (!e.pend) return;
        if (warmup_active_) { e.pend = false; return; }
        if (!(atr > 0.0) || !(b.open > 0.0)) { e.pend = false; return; }

        if (bypass_cost_gate) {
            // certification-harness viab gate (parity): refuse trades that can't cover cost.
            if (reward_bp < parity_viab_mult * parity_base_cost_bp) { e.pend = false; return; }
        }

        // trigger check
        double raw;
        if (e.pend_stop) {
            if (!(b.high >= e.pend_level)) { e.pend = false; return; }      // breakout not triggered
            raw = std::max(b.open, e.pend_level);
        } else {
            raw = b.open;                                                   // market-on-open
        }

        if (!bypass_cost_gate) {
            // production spread + cost-viability gate (near-inert on these runners).
            const double spread = last_ask_ - last_bid_;
            if (spread > max_spread) { e.pend = false; return; }
            const double reward_dist = e.viab_reward_atr * atr;            // expected reward (pts)
            if (!ExecutionCostGuard::is_viable(ledger_symbol.c_str(), spread, reward_dist, lot, 1.5)) {
                e.pend = false; return;
            }
        }

        e.entry_raw = raw;
        e.risk_px   = e.sl_atr * atr;
        e.stop_level = raw - e.risk_px;
        e.hh_since  = b.high;
        e.bars_held = 0;
        e.entry_ts  = b.start;
        e.mfe = e.mae = 0.0;
        e.in_pos = true; e.just_entered = true; e.pend = false;
        (void)now_ms;
    }

    // Manage an open long on bar b using ctx (through b-1). Stop checked BEFORE
    // the favourable-excursion update; gap-through booked at the real price.
    void _manage(int ci, const gbt::Bar& b, double atr, double trail, int64_t now_ms, OnCloseFn on_close) noexcept {
        GbtCell& e = cells_[ci];
        ++e.bars_held;
        double sl = e.stop_level;
        if (std::isfinite(trail)) sl = std::max(sl, trail);                 // opp-Donchian trail (monotone)
        if (e.trail_atr > 0.0 && atr > 0.0) sl = std::max(sl, e.hh_since - e.trail_atr * atr);  // chandelier
        e.stop_level = sl;
        if (b.low <= sl) {
            const double raw = std::min(b.open, sl);                        // gap-through at real price
            _close(ci, raw, "SL_TRAIL", now_ms, on_close);
            return;
        }
        // favourable-excursion update (AFTER the stop check)
        e.hh_since = std::max(e.hh_since, b.high);
    }

    // Book a closed long. RAW levels + RAW pnl (ledger applies realistic cost).
    void _close(int ci, double exit_raw, const char* reason, int64_t now_ms, OnCloseFn on_close) noexcept {
        GbtCell& e = cells_[ci];
        if (!e.in_pos) return;
        const double pts_move = exit_raw - e.entry_raw;                     // long-only
        const double favourable = exit_raw - e.entry_raw;
        e.mfe = std::max(e.mfe, e.hh_since - e.entry_raw);
        e.mae = std::max(e.mae, e.entry_raw - std::min(e.entry_raw, exit_raw));
        (void)favourable;

        omega::TradeRecord tr;
        tr.symbol     = ledger_symbol;
        tr.engine     = ledger_prefix + cells_[ci].name;
        tr.side       = "LONG";
        tr.entryPrice = e.entry_raw;
        tr.exitPrice  = exit_raw;
        tr.sl         = e.entry_raw - e.risk_px;
        tr.tp         = 0.0;
        tr.size       = lot;
        tr.entryTs    = e.entry_ts;                                         // bar_start is already seconds
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = cells_[ci].name;
        tr.shadow     = shadow_mode;
        tr.pnl        = pts_move * lot;                                     // RAW (ledger applies cost)
        tr.mfe        = e.mfe;
        tr.mae        = e.mae;
        if (on_close) on_close(tr);

        e.in_pos = false;
    }

    // Warm-seed one cell: replay bar CSV closes through the fold path (entries
    // suppressed via warmup_active_) so all indicators + prev snapshots warm.
    void _seed_cell(int ci, const std::string& rel) noexcept {
        if (rel.empty()) return;
        const std::string path = omega::resolve_seed_path(rel);
        std::ifstream f(path);
        if (!f.is_open()) {
            std::printf("[GBT-SEED] cell=%d MISS %s (cold-warms ~16d)\n", ci, path.c_str());
            std::fflush(stdout); return;
        }
        std::string line; size_t n = 0;
        while (std::getline(f, line)) {
            if (line.empty() || !std::isdigit((unsigned char)line[0])) continue;
            double ts = 0, o = 0, h = 0, l = 0, c = 0;
            if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c) == 5 && c > 0) {
                const double a = std::fabs(ts);
                const int64_t ts_sec = (int64_t)(a > 1e11 ? ts / 1000.0 : ts);  // ms -> sec if needed (sec~1.7e9, ms~1.7e12)
                gbt::Bar b{ts_sec, ts_sec + (int64_t)cells_[ci].bar_min * 60, o, h, l, c};
                _on_bar(ci, b, ts_sec * 1000, nullptr);                    // warmup_active_ => no entries
                ++n;
            }
        }
        std::printf("[GBT-SEED] cell=%d %s %zu bars -> atr_ready=%d sma_ready=%d don=%d\n",
                    ci, cells_[ci].name, n, (int)cells_[ci].atr.ready(),
                    (int)cells_[ci].sma.ready(), (int)cells_[ci].hi.size());
        std::fflush(stdout);
    }
};

} // namespace omega

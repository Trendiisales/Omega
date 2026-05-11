// =============================================================================
// honest_backtest_xauusd_v2.cpp  -- Experiment A (gated) + Experiment B (wide)
//                                   + S26 Part 4 extensions (single-config,
//                                     latency sweep, commission, gate-knobs,
//                                     time-of-day, early-exit, cross-instrument).
// -----------------------------------------------------------------------------
// History:
//   v1 (honest_backtest_xauusd.cpp): basic z-MR sweep, honest-fill model.
//   v2 (this file, 2026-05-08): added --gated (production-style entry gates)
//       and --wide (session-scale geometry grid).  Produced Part 3 outputs.
//   v2-ext (this file, 2026-05-11): per HANDOFF_S26_PART3.md §4 + the
//       operator-approved §2.x sweep ordering, this file now also accepts:
//
//         --single TP,SL,z       run ONE config (no grid loop)
//         --latency N            override LATENCY_TICKS (default 1)
//         --commission C         per round-trip USD (default 0.0)
//         --window W             override z-score window (default 200)
//         --imb-long F           override L2 long imb threshold (default .502)
//         --imb-short F          override L2 short imb threshold(default .498)
//         --max-spread S         override MAX_SPREAD_PT (default 1.0)
//         --session H1-H2        only enter when UTC hour in [H1, H2)
//                                (H1<0 disables, default disabled)
//         --early-exit-z X       exit early if |z - z_entry| diverges by >=X
//                                in the wrong direction (X<=0 disables)
//         --early-exit-ticks K   max look-ahead for early-exit (default 200)
//         --tick-mult M          USD per point per std-lot (default 100.0,
//                                which is XAUUSD 0.01-lot scaled)
//         --size-lots L          contract size in std-lots (default 0.01)
//         --cooldown C           cooldown ticks after exit (default 100)
//         --csv-out PATH         append CSV summary rows per fill_model to PATH
//         --trade-log PATH       append one CSV row per trade to PATH
//         --label LBL            free-form label included in every CSV row
//
//   Default invocation (no new flags, --gated or --wide as before, no
//   --single) is BIT-IDENTICAL to v2-baseline.  Verified on the 21-day
//   --gated --wide sweep:  sum still = +$423.03, per-day rows unchanged.
//
//   S29 (2026-05-11): added --wide-extreme (72 wider/asymmetric geometry
//       configs) and --invert (z-momentum signal direction).
//   S30 (2026-05-11): added --wide-fine (256 cells:
//       TP{25,30,35,40} × SL{12,15,18,20} × z{1.5,2,2.5,3} × W{100,200,400,800})
//       — a deep dive around the S29 extreme_asia PRIMARY candidate.
//       Per-cell window is carried via new GridPoint.w_override and
//       ConfigStats.window fields so the CSV writer records the actual
//       window used during sim, not the global default.  Bare --wide on
//       this binary remains bit-identical to v2-baseline / _s29 / _s28
//       on the simulation data lines.
//
// Build:
//   g++ -std=c++17 -O2 -Wall -Wextra backtest/honest_backtest_xauusd_v2.cpp \
//       -o backtest/honest_backtest_xauusd_v2
// Usage:
//   honest_backtest_xauusd_v2 [flags] <ticks.csv>
//
// IMPORTANT (handoff rule 7): this is the ONLY file that should be modified
//   in this directory for backtest extensions. Core engine headers
//   (omega_main.hpp, order_exec.hpp, OmegaTradeLedger.hpp, IndexFlowEngine.hpp,
//   microscalper_crtp_sweep.cpp, RiskMonitor.hpp, trade_lifecycle.hpp) are
//   off-limits without explicit operator instruction.
// =============================================================================

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace omega::honest_v2 {

// ----- tick data ----------------------------------------------------------
struct Tick {
    int64_t ts_ms;
    double  bid;
    double  ask;
    double  l2_imb;        // 0..1; 0.5 = balanced
    int     regime;        // 0/1/2/4 in the capture; --gated allows {0,2}
    int     watchdog_dead; // 0 healthy / 1 stale (skip)
    double  mid() const { return 0.5 * (bid + ask); }
    double  spread() const { return ask - bid; }
};

// ----- per-trade record ---------------------------------------------------
enum class ExitReason { TP_HIT, SL_HIT, EOD_FORCE, EARLY_EXIT_Z };

inline const char* exit_reason_str(ExitReason r) {
    switch (r) {
        case ExitReason::TP_HIT:        return "TP_HIT";
        case ExitReason::SL_HIT:        return "SL_HIT";
        case ExitReason::EOD_FORCE:     return "EOD_FORCE";
        case ExitReason::EARLY_EXIT_Z:  return "EARLY_EXIT_Z";
    }
    return "?";
}

struct Trade {
    int        id;
    bool       is_long;
    double     entry_px;
    double     exit_px;
    int        entry_idx;
    int        exit_idx;
    int64_t    entry_ts_ms;
    int64_t    exit_ts_ms;
    ExitReason reason;
    double     net_usd;
    double     z_at_entry;
    double     l2_imb_at_entry;
    int        regime_at_entry;
};

// ----- aggregate stats ----------------------------------------------------
struct ConfigStats {
    double tp_pts;
    double sl_pts;
    double z_thresh;

    // S30: window actually used during this config's simulation.
    // Populated from SimParams.window inside run_sim.  Default 0 lets the
    // existing brace-init `ConfigStats S{tp, sl, z};` continue to compile;
    // for any S30 fine-grid run, run_sim assigns S.window = P.window so
    // the CSV writer can preserve the per-cell window after sort.
    int    window     = 0;

    int    n_trades   = 0;
    int    n_wins     = 0;
    int    n_losses   = 0;
    int    n_tp_hit   = 0;
    int    n_sl_hit   = 0;
    int    n_eod_force= 0;
    int    n_early_z  = 0;

    double total_net_usd       = 0.0;
    double sum_win_usd         = 0.0;
    double sum_loss_usd        = 0.0;
    double max_drawdown_usd    = 0.0;
    double worst_trade_usd     = 0.0;
    double best_trade_usd      = 0.0;
    double avg_hold_ticks      = 0.0;

    double win_rate() const {
        return n_trades > 0 ? double(n_wins) / n_trades : 0.0;
    }
    double avg_win() const {
        return n_wins   > 0 ? sum_win_usd  / n_wins   : 0.0;
    }
    double avg_loss() const {
        return n_losses > 0 ? sum_loss_usd / n_losses : 0.0;
    }
    double expectancy() const {
        return n_trades > 0 ? total_net_usd / n_trades : 0.0;
    }
};

// =============================================================================
// FillModels -- identical semantics to v1
// =============================================================================
struct ProductionFill {
    static double resolve_entry_fill(const Tick& t, bool is_long,
                                     double /*intended_px*/) {
        return is_long ? t.ask : t.bid;
    }
    static double resolve_exit_fill(const Tick& t, bool is_long,
                                    double trigger_px,
                                    ExitReason reason) {
        if (reason == ExitReason::TP_HIT) return trigger_px;
        return is_long ? t.bid : t.ask;
    }
    static const char* name() { return "production-fill (pos.tp exact)"; }
    static const char* short_name() { return "production"; }
};

struct HonestFill {
    static double resolve_entry_fill(const Tick& t, bool is_long,
                                     double /*intended_px*/) {
        return is_long ? t.ask : t.bid;
    }
    static double resolve_exit_fill(const Tick& t, bool is_long,
                                    double /*trigger_px*/,
                                    ExitReason /*reason*/) {
        return is_long ? t.bid : t.ask;
    }
    static const char* name() { return "honest-fill (next-tick worst-side)"; }
    static const char* short_name() { return "honest"; }
};

// =============================================================================
// Signal: z-score mean reversion -- identical to v1, with last_z exposed
// =============================================================================
struct ZScoreMR {
    int    window;
    double z_thresh;
    double sum  = 0.0;
    double sum2 = 0.0;
    std::vector<double> ring;
    int    head = 0;
    int    n    = 0;
    double last_z = 0.0;
    bool   warm   = false;

    ZScoreMR(int w, double zt) : window(w), z_thresh(zt), ring(w, 0.0) {}

    int update(double mid) {
        if (n == window) {
            const double old = ring[head];
            sum  -= old;
            sum2 -= old * old;
        }
        ring[head] = mid;
        sum  += mid;
        sum2 += mid * mid;
        head = (head + 1) % window;
        if (n < window) {
            ++n;
            last_z = 0.0;
            warm   = false;
            return 0;
        }
        warm = true;
        const double m   = sum / window;
        const double var = std::max(sum2 / window - m * m, 1e-12);
        const double sd  = std::sqrt(var);
        const double z   = (mid - m) / sd;
        last_z = z;
        if (z >  z_thresh) return -1;
        if (z < -z_thresh) return +1;
        return 0;
    }
};

// =============================================================================
// Runtime-configurable production-style gates
// =============================================================================
struct GateConfig {
    double max_spread_pt = 1.0;
    double imb_long      = 0.502;
    double imb_short     = 0.498;

    bool regime_ok(int r) const { return r == 0 || r == 2; }

    int filter(int raw, const Tick& t) const {
        if (raw == 0) return 0;
        if (t.watchdog_dead != 0) return 0;
        if (t.spread() > max_spread_pt) return 0;
        if (!regime_ok(t.regime)) return 0;
        if (raw == +1 && t.l2_imb < imb_long)  return 0;
        if (raw == -1 && t.l2_imb > imb_short) return 0;
        return raw;
    }
};

// =============================================================================
// Simulation parameters (everything previously hardcoded becomes a field)
// =============================================================================
struct SimParams {
    double tp_pts          = 5.0;
    double sl_pts          = 16.0;
    double z_thresh        = 2.0;
    int    window          = 200;
    int    latency_ticks   = 1;
    int    cooldown_ticks  = 100;
    double size_lots       = 0.01;
    double tick_mult       = 100.0;   // USD per point per std-lot * size_lots
    double commission_rt   = 0.0;     // per trade (round-trip), USD
    bool   gated           = false;
    GateConfig gate{};
    // session filter (half-open [start, end) on UTC hour 0..23; <0 disables)
    int    session_start_h = -1;
    int    session_end_h   = -1;
    // S29: signal direction inversion (false = z-MR fade extremes;
    //                                  true  = z-momentum chase extremes)
    bool   invert_signal   = false;
    // early-exit on z-divergence (early_exit_z <= 0 disables)
    double early_exit_z      = 0.0;
    int    early_exit_ticks  = 200;
    // optional trade log emitter
    std::FILE* trade_log_fp  = nullptr;
    std::string trade_log_tag;  // tag to identify this run's trades in the log
};

inline int utc_hour_of(int64_t ts_ms) {
    const time_t t = static_cast<time_t>(ts_ms / 1000);
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    return g.tm_hour;
}

inline bool session_allows(const SimParams& P, int64_t ts_ms) {
    if (P.session_start_h < 0 || P.session_end_h < 0) return true;
    const int h = utc_hour_of(ts_ms);
    if (P.session_start_h <= P.session_end_h)
        return h >= P.session_start_h && h < P.session_end_h;
    // wrap-around (e.g. 22-04 = 22..23 ∪ 0..3)
    return h >= P.session_start_h || h < P.session_end_h;
}

// =============================================================================
// CRTP simulator -- core trade loop
// =============================================================================
template <typename Fill>
ConfigStats run_sim(const std::vector<Tick>& ticks, const SimParams& P) {
    ZScoreMR sig(P.window, P.z_thresh);

    ConfigStats S{P.tp_pts, P.sl_pts, P.z_thresh};
    S.window = P.window;   // S30: preserve per-cell window for CSV write
    std::vector<Trade> trades;
    trades.reserve(2000);

    bool   pos_open     = false;
    bool   pos_is_long  = false;
    double pos_entry_px = 0.0;
    int    pos_entry_ix = 0;
    double pos_z_at_entry      = 0.0;
    double pos_l2_at_entry     = 0.5;
    int    pos_regime_at_entry = 0;
    int64_t pos_entry_ts_ms    = 0;

    int pending_entry_at  = -1;
    int pending_entry_dir = 0;
    int pending_exit_at   = -1;
    ExitReason pending_exit_reason = ExitReason::TP_HIT;
    double pending_exit_trigger_px = 0.0;

    int next_allowed_entry = P.window;

    const int N = static_cast<int>(ticks.size());

    auto book_trade = [&](double fill_px, int exit_idx, ExitReason reason) {
        const double pnl_units = pos_is_long
            ? (fill_px - pos_entry_px)
            : (pos_entry_px - fill_px);
        const double gross_usd = pnl_units * P.size_lots * P.tick_mult;
        const double net = gross_usd - P.commission_rt;
        const int64_t exit_ts = (exit_idx >= 0 && exit_idx < N)
                                ? ticks[exit_idx].ts_ms : 0;
        Trade tr{
            static_cast<int>(trades.size()) + 1,
            pos_is_long, pos_entry_px, fill_px,
            pos_entry_ix, exit_idx,
            pos_entry_ts_ms, exit_ts,
            reason, net,
            pos_z_at_entry, pos_l2_at_entry, pos_regime_at_entry
        };
        trades.push_back(tr);
        if (net > 0) { ++S.n_wins;   S.sum_win_usd  += net; }
        else         { ++S.n_losses; S.sum_loss_usd += net; }
        if (reason == ExitReason::TP_HIT)        ++S.n_tp_hit;
        if (reason == ExitReason::SL_HIT)        ++S.n_sl_hit;
        if (reason == ExitReason::EOD_FORCE)     ++S.n_eod_force;
        if (reason == ExitReason::EARLY_EXIT_Z)  ++S.n_early_z;
        S.total_net_usd += net;
        if (net < S.worst_trade_usd) S.worst_trade_usd = net;
        if (net > S.best_trade_usd)  S.best_trade_usd  = net;
        pos_open = false;
        next_allowed_entry = exit_idx + P.cooldown_ticks;

        if (P.trade_log_fp) {
            std::fprintf(P.trade_log_fp,
                "%s,%d,%d,%lld,%lld,%d,%d,%d,%.5f,%.5f,%s,%d,%.4f,%.4f,%.5f,%d\n",
                P.trade_log_tag.c_str(),
                tr.id,
                tr.is_long ? 1 : 0,
                static_cast<long long>(tr.entry_ts_ms),
                static_cast<long long>(tr.exit_ts_ms),
                tr.entry_idx,
                tr.exit_idx,
                tr.exit_idx - tr.entry_idx,
                tr.entry_px,
                tr.exit_px,
                exit_reason_str(tr.reason),
                static_cast<int>(tr.is_long ? 1 : -1),
                tr.net_usd,
                tr.z_at_entry,
                tr.l2_imb_at_entry,
                tr.regime_at_entry);
        }
    };

    for (int i = 0; i < N; ++i) {
        const Tick& t = ticks[i];

        if (pending_entry_at == i) {
            pos_is_long  = (pending_entry_dir == +1);
            pos_entry_px = Fill::resolve_entry_fill(t, pos_is_long, t.mid());
            pos_entry_ix = i;
            pos_open     = true;
            pos_entry_ts_ms     = t.ts_ms;
            pos_z_at_entry      = sig.last_z;
            pos_l2_at_entry     = t.l2_imb;
            pos_regime_at_entry = t.regime;
            pending_entry_at  = -1;
            pending_entry_dir = 0;
        }

        if (pos_open && pending_exit_at == i) {
            const double fpx = Fill::resolve_exit_fill(
                t, pos_is_long, pending_exit_trigger_px,
                pending_exit_reason);
            book_trade(fpx, i, pending_exit_reason);
            pending_exit_at = -1;
        }

        int fire = sig.update(t.mid());
        if (P.invert_signal) fire = -fire;            // S29: z-momentum mode
        if (P.gated) fire = P.gate.filter(fire, t);
        if (fire != 0 && !session_allows(P, t.ts_ms)) fire = 0;

        // TP/SL trigger checks
        if (pos_open && pending_exit_at == -1) {
            if (pos_is_long) {
                if (t.bid >= pos_entry_px + P.tp_pts) {
                    pending_exit_at = std::min(i + P.latency_ticks, N - 1);
                    pending_exit_reason = ExitReason::TP_HIT;
                    pending_exit_trigger_px = pos_entry_px + P.tp_pts;
                } else if (t.bid <= pos_entry_px - P.sl_pts) {
                    pending_exit_at = std::min(i + P.latency_ticks, N - 1);
                    pending_exit_reason = ExitReason::SL_HIT;
                    pending_exit_trigger_px = pos_entry_px - P.sl_pts;
                }
            } else {
                if (t.ask <= pos_entry_px - P.tp_pts) {
                    pending_exit_at = std::min(i + P.latency_ticks, N - 1);
                    pending_exit_reason = ExitReason::TP_HIT;
                    pending_exit_trigger_px = pos_entry_px - P.tp_pts;
                } else if (t.ask >= pos_entry_px + P.sl_pts) {
                    pending_exit_at = std::min(i + P.latency_ticks, N - 1);
                    pending_exit_reason = ExitReason::SL_HIT;
                    pending_exit_trigger_px = pos_entry_px + P.sl_pts;
                }
            }
        }

        // Early-exit on z-divergence (only if enabled and no other exit pending)
        if (pos_open && pending_exit_at == -1
            && P.early_exit_z > 0.0
            && sig.warm
            && (i - pos_entry_ix) <= P.early_exit_ticks)
        {
            const double zn = sig.last_z;
            bool diverged = false;
            if (pos_is_long) {
                // entered when z <= -z_thresh; want z to revert upward; if it
                // drops MORE negative by early_exit_z, exit.
                diverged = (pos_z_at_entry - zn) >= P.early_exit_z;
            } else {
                // entered when z >= +z_thresh; want reversion downward; if it
                // moves MORE positive by early_exit_z, exit.
                diverged = (zn - pos_z_at_entry) >= P.early_exit_z;
            }
            if (diverged) {
                pending_exit_at = std::min(i + P.latency_ticks, N - 1);
                pending_exit_reason = ExitReason::EARLY_EXIT_Z;
                // Trigger price irrelevant -- honest-fill uses worst-side anyway
                pending_exit_trigger_px = t.mid();
            }
        }

        if (!pos_open && pending_entry_at == -1
            && i >= next_allowed_entry && fire != 0)
        {
            pending_entry_at  = std::min(i + P.latency_ticks, N - 1);
            pending_entry_dir = fire;
        }
    }

    if (pos_open) {
        const Tick& last = ticks.back();
        const double fpx = Fill::resolve_exit_fill(
            last, pos_is_long, last.mid(), ExitReason::EOD_FORCE);
        book_trade(fpx, N - 1, ExitReason::EOD_FORCE);
    }

    double cum = 0.0, peak = 0.0, mdd = 0.0;
    long total_hold = 0;
    for (const auto& tr : trades) {
        cum += tr.net_usd;
        if (cum > peak) peak = cum;
        if (peak - cum > mdd) mdd = peak - cum;
        total_hold += (tr.exit_idx - tr.entry_idx);
    }
    S.n_trades = static_cast<int>(trades.size());
    S.max_drawdown_usd = mdd;
    S.avg_hold_ticks   = S.n_trades > 0
        ? double(total_hold) / S.n_trades : 0.0;

    return S;
}

// =============================================================================
// CSV loader -- extended to read l2_imb, regime, watchdog_dead.
// =============================================================================
bool load_ticks(const std::string& path, std::vector<Tick>& out) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[FATAL] cannot open %s\n", path.c_str());
        return false;
    }
    std::string line;
    if (!std::getline(f, line)) return false;

    int ix_ts = -1, ix_bid = -1, ix_ask = -1;
    int ix_l2 = -1, ix_reg = -1, ix_wd  = -1;
    {
        std::stringstream ss(line);
        std::string cell;
        int col = 0;
        while (std::getline(ss, cell, ',')) {
            while (!cell.empty() && (cell.back() == ' ' || cell.back() == '\r'
                                      || cell.back() == '\n'))
                cell.pop_back();
            if      (cell == "ts_ms")          ix_ts  = col;
            else if (cell == "bid")            ix_bid = col;
            else if (cell == "ask")            ix_ask = col;
            else if (cell == "l2_imb")         ix_l2  = col;
            else if (cell == "regime")         ix_reg = col;
            else if (cell == "watchdog_dead")  ix_wd  = col;
            ++col;
        }
    }
    if (ix_ts < 0 || ix_bid < 0 || ix_ask < 0) {
        std::fprintf(stderr,
            "[FATAL] header missing ts_ms/bid/ask: ts=%d bid=%d ask=%d\n",
            ix_ts, ix_bid, ix_ask);
        return false;
    }
    out.reserve(400000);
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string cell;
        int col = 0;
        Tick t{0, 0.0, 0.0, 0.5, 0, 0};
        while (std::getline(ss, cell, ',')) {
            try {
                if      (col == ix_ts ) t.ts_ms         = std::stoll(cell);
                else if (col == ix_bid) t.bid           = std::stod(cell);
                else if (col == ix_ask) t.ask           = std::stod(cell);
                else if (col == ix_l2 ) t.l2_imb        = std::stod(cell);
                else if (col == ix_reg) t.regime        = std::stoi(cell);
                else if (col == ix_wd ) t.watchdog_dead = std::stoi(cell);
            } catch (...) { /* skip malformed cell */ }
            ++col;
        }
        if (t.ts_ms > 0 && t.bid > 0.0 && t.ask > 0.0) out.push_back(t);
    }
    return true;
}

// =============================================================================
// Grids
// =============================================================================
struct GridPoint {
    double tp;
    double sl;
    double z;
    // S30: per-cell window override; 0 means "use SimParams.window default".
    // Used by --wide-fine to scan W in the same in-process loop.
    int    w_override = 0;
};

std::vector<GridPoint> build_grid(bool quick, bool wide, bool extreme,
                                  bool fine) {
    if (quick) {
        return {
            {0.79, 3.0, 2.0},
            {2.0,  4.0, 2.0},
            {3.0,  5.0, 2.5},
        };
    }
    if (fine) {
        // S30 §3.1 fine-geometry sweep around the S29 extreme_asia PRIMARY
        // candidate (TP=30, SL=15, z=2.0, W=200 in Asia session).
        //   TP {25, 30, 35, 40}   × 4
        //   SL {12, 15, 18, 20}   × 4
        //   z  {1.5, 2.0, 2.5, 3} × 4
        //   W  {100, 200, 400, 800} × 4
        // = 256 configs.  Per-cell window carried via GridPoint.w_override.
        // No SL>TP constraint here (the S29 PRIMARY uses TP>SL, momentum-
        // continuation geometry — symmetric to the v1/wide grids).
        std::vector<GridPoint> g;
        const std::vector<double> tps = {25.0, 30.0, 35.0, 40.0};
        const std::vector<double> sls = {12.0, 15.0, 18.0, 20.0};
        const std::vector<double> zs  = {1.5, 2.0, 2.5, 3.0};
        const std::vector<int>    ws  = {100, 200, 400, 800};
        for (double tp : tps)
            for (double sl : sls)
                for (double z : zs)
                    for (int w : ws)
                        g.push_back({tp, sl, z, w});
        return g;                                            // total = 256
    }
    if (extreme) {
        // S29 Phase 3 — wider/asymmetric geometries beyond the (3-12, 8-24) box.
        // Tier A: wider symmetric + momentum-continuation (TP can equal/exceed SL).
        // Tier B: scalp-and-ride (very tight TP, very wide SL).
        std::vector<GridPoint> g;
        const std::vector<double> zs = {1.5, 2.0, 2.5, 3.0};
        const std::vector<double> tps_a = {15.0, 20.0, 30.0};
        const std::vector<double> sls_a = {15.0, 20.0, 30.0, 50.0};
        for (double tp : tps_a)
            for (double sl : sls_a)
                for (double z : zs)
                    g.push_back({tp, sl, z});                // 3 * 4 * 4 = 48
        const std::vector<double> tps_b = {1.0, 2.0};
        const std::vector<double> sls_b = {30.0, 50.0, 100.0};
        for (double tp : tps_b)
            for (double sl : sls_b)
                if (sl > tp)
                    for (double z : zs)
                        g.push_back({tp, sl, z});            // 2 * 3 * 4 = 24
        return g;                                            // total = 72
    }
    if (wide) {
        std::vector<double> tps = {3.0, 5.0, 8.0, 12.0};
        std::vector<double> sls = {8.0, 12.0, 16.0, 24.0};
        std::vector<double> zs  = {1.5, 2.0, 2.5, 3.0};
        std::vector<GridPoint> g;
        for (double tp : tps)
            for (double sl : sls)
                if (sl > tp)
                    for (double z : zs)
                        g.push_back({tp, sl, z});
        return g;
    }
    // v1 grid (default)
    std::vector<double> tps = {0.79, 1.0, 1.5, 2.0, 3.0, 5.0};
    std::vector<double> sls = {3.0,  4.0, 5.0, 8.0};
    std::vector<double> zs  = {1.5,  2.0, 2.5};
    std::vector<GridPoint> g;
    for (double tp : tps)
        for (double sl : sls)
            if (sl > tp)
                for (double z : zs)
                    g.push_back({tp, sl, z});
    return g;
}

// =============================================================================
// Output -- leaderboard format unchanged from v1/v2-baseline
// =============================================================================
void print_leaderboard(const char* fill_name,
                       std::vector<ConfigStats>& rows) {
    std::sort(rows.begin(), rows.end(),
              [](const ConfigStats& a, const ConfigStats& b) {
                  return a.expectancy() > b.expectancy();
              });
    std::printf("\n");
    std::printf("================================================================="
                "===================================\n");
    std::printf(" FILL MODEL: %s\n", fill_name);
    std::printf("================================================================="
                "===================================\n");
    std::printf(" %4s %5s %5s %4s %6s %6s %8s %8s %9s %10s %8s %7s\n",
                "rank", "TP", "SL", "z", "N", "WR%",
                "avgW$", "avgL$", "exp/tr$", "total$",
                "MDD$", "tp/sl");
    std::printf("-----------------------------------------------------------------"
                "-----------------------------------\n");
    int rank = 0;
    for (const auto& r : rows) {
        ++rank;
        std::printf(" %4d %5.2f %5.1f %4.1f %6d %5.1f%% %+8.3f %+8.3f %+9.4f "
                    "%+10.3f %8.2f %4d/%-3d\n",
                    rank, r.tp_pts, r.sl_pts, r.z_thresh, r.n_trades,
                    r.win_rate() * 100.0, r.avg_win(), r.avg_loss(),
                    r.expectancy(), r.total_net_usd,
                    r.max_drawdown_usd, r.n_tp_hit, r.n_sl_hit);
    }
    std::fflush(stdout);
}

void print_verdict(const std::vector<ConfigStats>& honest_rows,
                   const std::vector<ConfigStats>& production_rows,
                   bool gated, bool wide)
{
    std::printf("\n");
    std::printf("================================================================="
                "===================================\n");
    std::printf(" VERDICT (mode: %sgated %s)\n",
                gated ? "" : "un",
                wide  ? "wide-grid" : "v1-grid");
    std::printf("================================================================="
                "===================================\n");

    int n_profitable_honest = 0;
    int n_profitable_prod   = 0;
    const ConfigStats* best_honest = nullptr;
    for (const auto& r : honest_rows) {
        if (r.expectancy() > 0.0 && r.n_trades >= 10) {
            ++n_profitable_honest;
            if (!best_honest || r.expectancy() > best_honest->expectancy())
                best_honest = &r;
        }
    }
    for (const auto& r : production_rows) {
        if (r.expectancy() > 0.0 && r.n_trades >= 10) ++n_profitable_prod;
    }

    std::printf(" production-fill: %d/%zu configs look profitable\n",
                n_profitable_prod, production_rows.size());
    std::printf(" honest-fill    : %d/%zu configs survive real costs\n",
                n_profitable_honest, honest_rows.size());
    std::printf("\n");

    if (best_honest) {
        std::printf(" BEST UNDER HONEST FILLS:\n");
        std::printf("   TP=%.2fpt SL=%.1fpt z=%.1f  N=%d  WR=%.1f%%\n",
                    best_honest->tp_pts, best_honest->sl_pts,
                    best_honest->z_thresh, best_honest->n_trades,
                    best_honest->win_rate() * 100.0);
        std::printf("   exp/trade=$%+.4f  total=$%+.2f  MDD=$%.2f\n",
                    best_honest->expectancy(),
                    best_honest->total_net_usd,
                    best_honest->max_drawdown_usd);
    } else {
        std::printf(" NO honest-fill config survives.\n");
    }

    double prod_total = 0.0, honest_total = 0.0;
    for (const auto& r : production_rows) prod_total   += r.total_net_usd;
    for (const auto& r : honest_rows)     honest_total += r.total_net_usd;
    std::printf("\n");
    std::printf(" FICTIONAL-PROFIT GAP (sum across all configs):\n");
    std::printf("   production-fill total $%+.2f vs honest-fill total $%+.2f\n",
                prod_total, honest_total);
    std::printf("   gap = $%+.2f\n", prod_total - honest_total);
    std::printf("================================================================="
                "===================================\n");
    std::fflush(stdout);
}

// =============================================================================
// CSV summary emitter (per fill_model, per config)
// =============================================================================
static const char* CSV_HEADER =
    "file,instrument,label,fill_model,tp,sl,z,window,latency,commission_rt,"
    "max_spread_pt,imb_long,imb_short,session_start_h,session_end_h,"
    "early_exit_z,early_exit_ticks,gated,n_trades,n_wins,n_losses,"
    "n_tp_hit,n_sl_hit,n_eod_force,n_early_z,wr_pct,sum_usd,mdd_usd,"
    "worst_usd,best_usd,exp_per_trade,avg_hold_ticks";

void ensure_csv_header(const std::string& path) {
    std::ifstream test(path);
    bool exists = test.good();
    test.close();
    if (!exists) {
        std::ofstream f(path);
        f << CSV_HEADER << "\n";
    }
}

void append_csv_row(const std::string& path,
                    const std::string& tick_file,
                    const std::string& instrument,
                    const std::string& label,
                    const char* fill_short_name,
                    const SimParams& P,
                    const ConfigStats& s)
{
    std::ofstream f(path, std::ios::app);
    f << tick_file << "," << instrument << "," << label << ","
      << fill_short_name << ","
      << P.tp_pts << "," << P.sl_pts << "," << P.z_thresh << ","
      << P.window << "," << P.latency_ticks << "," << P.commission_rt << ","
      << P.gate.max_spread_pt << "," << P.gate.imb_long << ","
      << P.gate.imb_short << ","
      << P.session_start_h << "," << P.session_end_h << ","
      << P.early_exit_z << "," << P.early_exit_ticks << ","
      << (P.gated ? 1 : 0) << ","
      << s.n_trades << "," << s.n_wins << "," << s.n_losses << ","
      << s.n_tp_hit << "," << s.n_sl_hit << "," << s.n_eod_force << ","
      << s.n_early_z << ","
      << s.win_rate() * 100.0 << ","
      << s.total_net_usd << "," << s.max_drawdown_usd << ","
      << s.worst_trade_usd << "," << s.best_trade_usd << ","
      << s.expectancy() << "," << s.avg_hold_ticks
      << "\n";
}

// =============================================================================
// Helpers
// =============================================================================
static std::string basename_of(const std::string& p) {
    auto pos = p.find_last_of("/\\");
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

static std::string infer_instrument(const std::string& path) {
    const std::string b = basename_of(path);
    if (b.find("XAUUSD") != std::string::npos) return "XAUUSD";
    if (b.find("US500")  != std::string::npos) return "US500";
    if (b.find("USTEC")  != std::string::npos) return "USTEC";
    if (b.find("NAS100") != std::string::npos) return "NAS100";
    if (b.find("XAGUSD") != std::string::npos) return "XAGUSD";
    // Unprefixed (pre-rename XAUUSD captures from 2026-04-09..04-21)
    return "XAUUSD";
}

}  // namespace omega::honest_v2

// =============================================================================
// main
// =============================================================================
int main(int argc, char** argv) {
    using namespace omega::honest_v2;

    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s [flags] <ticks.csv>\n"
            "  --gated          add production-style entry gates (Experiment A)\n"
            "  --wide           use wider geometry grid (Experiment B)\n"
            "  --wide-extreme   72 wider/asymmetric geometry configs (S29)\n"
            "  --wide-fine      256 cells TP*SL*z*W around the PRIMARY (S30)\n"
            "  --invert         z-momentum signal direction (S29)\n"
            "  --quick          3-point smoke grid\n"
            "  --single TP,SL,z run ONE config (skips grid; --gated optional)\n"
            "  --latency N\n"
            "  --commission C   per round-trip USD\n"
            "  --window W\n"
            "  --imb-long F   --imb-short F   --max-spread S\n"
            "  --session H1-H2  enter only when UTC hour in [H1, H2)\n"
            "  --early-exit-z X --early-exit-ticks K\n"
            "  --tick-mult M  --size-lots L  --cooldown C\n"
            "  --csv-out PATH   append summary CSV row(s) to PATH\n"
            "  --trade-log PATH append per-trade CSV row(s) to PATH\n"
            "  --label LBL      free-form label included in CSV rows\n",
            argv[0]);
        return 2;
    }

    bool   quick = false, gated = false, wide = false;
    bool   extreme = false;          // S29: --wide-extreme (72 wider/asym configs)
    bool   invert_signal = false;    // S29: --invert (z-momentum)
    bool   fine    = false;          // S30: --wide-fine (256 cells around PRIMARY)
    bool   single_mode = false;
    double s_tp = 5.0, s_sl = 16.0, s_z = 2.0;

    int    latency_ticks  = 1;
    double commission_rt  = 0.0;
    int    window         = 200;
    int    cooldown_ticks = 100;
    double size_lots      = 0.01;
    double tick_mult      = 100.0;
    double max_spread_pt  = 1.0;
    double imb_long       = 0.502;
    double imb_short      = 0.498;
    int    session_start_h = -1;
    int    session_end_h   = -1;
    double early_exit_z      = 0.0;
    int    early_exit_ticks  = 200;
    std::string csv_out_path;
    std::string trade_log_path;
    std::string label;

    std::string ticks_path;
    auto parse_double = [](const char* s) { return std::stod(s); };
    auto parse_int    = [](const char* s) { return std::stoi(s); };

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if      (std::strcmp(a, "--quick") == 0) quick = true;
        else if (std::strcmp(a, "--gated") == 0) gated = true;
        else if (std::strcmp(a, "--wide")  == 0) wide  = true;
        else if (std::strcmp(a, "--wide-extreme") == 0) extreme = true;  // S29
        else if (std::strcmp(a, "--invert") == 0)       invert_signal = true;  // S29
        else if (std::strcmp(a, "--wide-fine") == 0)    fine = true;            // S30
        else if (std::strcmp(a, "--single") == 0 && i + 1 < argc) {
            single_mode = true;
            const char* v = argv[++i];
            // parse "TP,SL,z"
            char buf[64]; std::strncpy(buf, v, sizeof(buf)-1); buf[63]=0;
            char* tok = std::strtok(buf, ",");
            if (tok) s_tp = parse_double(tok);
            tok = std::strtok(nullptr, ",");
            if (tok) s_sl = parse_double(tok);
            tok = std::strtok(nullptr, ",");
            if (tok) s_z  = parse_double(tok);
        }
        else if (std::strcmp(a, "--latency") == 0 && i + 1 < argc)
            latency_ticks = parse_int(argv[++i]);
        else if (std::strcmp(a, "--commission") == 0 && i + 1 < argc)
            commission_rt = parse_double(argv[++i]);
        else if (std::strcmp(a, "--window") == 0 && i + 1 < argc)
            window = parse_int(argv[++i]);
        else if (std::strcmp(a, "--cooldown") == 0 && i + 1 < argc)
            cooldown_ticks = parse_int(argv[++i]);
        else if (std::strcmp(a, "--size-lots") == 0 && i + 1 < argc)
            size_lots = parse_double(argv[++i]);
        else if (std::strcmp(a, "--tick-mult") == 0 && i + 1 < argc)
            tick_mult = parse_double(argv[++i]);
        else if (std::strcmp(a, "--max-spread") == 0 && i + 1 < argc)
            max_spread_pt = parse_double(argv[++i]);
        else if (std::strcmp(a, "--imb-long") == 0 && i + 1 < argc)
            imb_long = parse_double(argv[++i]);
        else if (std::strcmp(a, "--imb-short") == 0 && i + 1 < argc)
            imb_short = parse_double(argv[++i]);
        else if (std::strcmp(a, "--session") == 0 && i + 1 < argc) {
            const char* v = argv[++i];
            char buf[32]; std::strncpy(buf, v, sizeof(buf)-1); buf[31]=0;
            char* tok = std::strtok(buf, "-");
            if (tok) session_start_h = parse_int(tok);
            tok = std::strtok(nullptr, "-");
            if (tok) session_end_h = parse_int(tok);
        }
        else if (std::strcmp(a, "--early-exit-z") == 0 && i + 1 < argc)
            early_exit_z = parse_double(argv[++i]);
        else if (std::strcmp(a, "--early-exit-ticks") == 0 && i + 1 < argc)
            early_exit_ticks = parse_int(argv[++i]);
        else if (std::strcmp(a, "--csv-out") == 0 && i + 1 < argc)
            csv_out_path = argv[++i];
        else if (std::strcmp(a, "--trade-log") == 0 && i + 1 < argc)
            trade_log_path = argv[++i];
        else if (std::strcmp(a, "--label") == 0 && i + 1 < argc)
            label = argv[++i];
        else if (a[0] == '-' && a[1] == '-') {
            std::fprintf(stderr, "[WARN] unknown flag %s\n", a);
        }
        else {
            ticks_path = a;
        }
    }
    if (ticks_path.empty()) {
        std::fprintf(stderr, "[FATAL] missing ticks CSV path\n");
        return 2;
    }

    std::vector<Tick> ticks;
    auto t0 = std::chrono::steady_clock::now();
    if (!load_ticks(ticks_path, ticks)) return 2;
    auto t1 = std::chrono::steady_clock::now();
    const auto load_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(t1 - t0).count();
    std::printf("[INFO] loaded %zu ticks from %s in %lld ms "
                "(mode: %s%sgated %s)\n",
                ticks.size(), ticks_path.c_str(), (long long)load_ms,
                single_mode ? "single " : "",
                gated ? "" : "un",
                wide  ? "wide-grid" : (single_mode ? "single" : "v1-grid"));
    if (ticks.size() < 1000) {
        std::fprintf(stderr,
            "[FATAL] too few ticks (%zu) -- need >=1000 for warmup\n",
            ticks.size());
        return 2;
    }

    double sum_spread = 0.0; double med_spread = 0.0;
    {
        std::vector<double> spreads; spreads.reserve(ticks.size());
        for (const auto& tt : ticks) {
            const double s = tt.ask - tt.bid;
            spreads.push_back(s);
            sum_spread += s;
        }
        std::sort(spreads.begin(), spreads.end());
        med_spread = spreads[spreads.size() / 2];
    }
    std::printf("[INFO] spread stats: mean=%.4fpt median=%.4fpt\n",
                sum_spread / ticks.size(), med_spread);

    if (gated) {
        GateConfig gpre{ max_spread_pt, imb_long, imb_short };
        long n_gate_ok = 0;
        for (const auto& tt : ticks) {
            if (tt.watchdog_dead != 0) continue;
            if (tt.spread() > gpre.max_spread_pt) continue;
            if (!gpre.regime_ok(tt.regime)) continue;
            ++n_gate_ok;
        }
        std::printf("[INFO] gate pre-screen passes: %ld/%zu ticks (%.1f%%)\n",
                    n_gate_ok, ticks.size(),
                    100.0 * n_gate_ok / ticks.size());
    }

    // Open optional trade-log file
    std::FILE* trade_fp = nullptr;
    if (!trade_log_path.empty()) {
        bool needs_header = !std::ifstream(trade_log_path).good();
        trade_fp = std::fopen(trade_log_path.c_str(), "a");
        if (!trade_fp) {
            std::fprintf(stderr, "[WARN] cannot open trade-log %s\n",
                         trade_log_path.c_str());
        } else if (needs_header) {
            std::fprintf(trade_fp,
                "tag,trade_id,is_long,entry_ts_ms,exit_ts_ms,entry_idx,"
                "exit_idx,hold_ticks,entry_px,exit_px,reason,dir,net_usd,"
                "z_at_entry,l2_imb_at_entry,regime_at_entry\n");
        }
    }

    // Optional CSV header bootstrap
    if (!csv_out_path.empty()) ensure_csv_header(csv_out_path);

    const std::string instrument = infer_instrument(ticks_path);

    auto build_params = [&](double tp, double sl, double z) {
        SimParams P;
        P.tp_pts = tp; P.sl_pts = sl; P.z_thresh = z;
        P.window = window;
        P.latency_ticks  = latency_ticks;
        P.cooldown_ticks = cooldown_ticks;
        P.size_lots      = size_lots;
        P.tick_mult      = tick_mult;
        P.commission_rt  = commission_rt;
        P.gated          = gated;
        P.gate.max_spread_pt = max_spread_pt;
        P.gate.imb_long  = imb_long;
        P.gate.imb_short = imb_short;
        P.session_start_h = session_start_h;
        P.session_end_h   = session_end_h;
        P.invert_signal     = invert_signal;     // S29
        P.early_exit_z      = early_exit_z;
        P.early_exit_ticks  = early_exit_ticks;
        P.trade_log_fp = trade_fp;
        return P;
    };

    if (single_mode) {
        SimParams P = build_params(s_tp, s_sl, s_z);
        // Use a tag that identifies this run in the per-trade log
        char tagbuf[256];
        std::snprintf(tagbuf, sizeof(tagbuf),
                      "%s|%s|tp=%.2f|sl=%.1f|z=%.1f|lat=%d|comm=%.4f|w=%d|"
                      "imbL=%.3f|imbS=%.3f|sess=%d-%d|ez=%.2f|et=%d",
                      basename_of(ticks_path).c_str(),
                      label.empty() ? "-" : label.c_str(),
                      P.tp_pts, P.sl_pts, P.z_thresh,
                      P.latency_ticks, P.commission_rt, P.window,
                      P.gate.imb_long, P.gate.imb_short,
                      P.session_start_h, P.session_end_h,
                      P.early_exit_z, P.early_exit_ticks);
        P.trade_log_tag = tagbuf;

        std::printf("[INFO] SINGLE-CONFIG: TP=%.2f SL=%.1f z=%.1f "
                    "(window=%d latency=%d comm=%.4f gated=%d)\n",
                    P.tp_pts, P.sl_pts, P.z_thresh,
                    P.window, P.latency_ticks, P.commission_rt,
                    P.gated ? 1 : 0);

        ConfigStats s_prod = run_sim<ProductionFill>(ticks, P);
        ConfigStats s_hone = run_sim<HonestFill>(ticks, P);

        std::vector<ConfigStats> v_prod{s_prod};
        std::vector<ConfigStats> v_hone{s_hone};
        print_leaderboard(ProductionFill::name(), v_prod);
        print_leaderboard(HonestFill::name(),     v_hone);
        print_verdict(v_hone, v_prod, gated, /*wide=*/false);

        if (!csv_out_path.empty()) {
            append_csv_row(csv_out_path, basename_of(ticks_path),
                           instrument, label,
                           ProductionFill::short_name(), P, s_prod);
            append_csv_row(csv_out_path, basename_of(ticks_path),
                           instrument, label,
                           HonestFill::short_name(),     P, s_hone);
        }
        if (trade_fp) std::fclose(trade_fp);
        return s_hone.expectancy() > 0.0 && s_hone.n_trades >= 10 ? 0 : 1;
    }

    // Grid mode (default / --wide / --quick / --wide-extreme / --wide-fine).
    // Behavior bit-identical to v2-baseline when no v2-ext / S29 / S30
    // flags are passed.
    const auto grid = build_grid(quick, wide, extreme, fine);
    std::printf("[INFO] running %zu configs under BOTH fill models...\n",
                grid.size());

    std::vector<ConfigStats> honest_rows;
    std::vector<ConfigStats> production_rows;
    honest_rows.reserve(grid.size());
    production_rows.reserve(grid.size());

    auto t2 = std::chrono::steady_clock::now();
    for (const auto& g : grid) {
        SimParams P = build_params(g.tp, g.sl, g.z);
        // S30: per-cell window override (--wide-fine).  For all other
        // grids w_override is 0 and this is a no-op, preserving bit-identical
        // behavior with _s29 / _s28 / v2-baseline.
        if (g.w_override > 0) P.window = g.w_override;
        // Trade log per grid cell would be huge; only enable for single-mode.
        P.trade_log_fp = nullptr;
        production_rows.push_back(run_sim<ProductionFill>(ticks, P));
        honest_rows.push_back   (run_sim<HonestFill>    (ticks, P));
    }
    auto t3 = std::chrono::steady_clock::now();
    const auto sim_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(t3 - t2).count();
    std::printf("[INFO] simulated %zu configs in %lld ms\n",
                grid.size() * 2, (long long)sim_ms);

    print_leaderboard(ProductionFill::name(), production_rows);
    print_leaderboard(HonestFill::name(),     honest_rows);
    print_verdict(honest_rows, production_rows, gated, wide);

    if (!csv_out_path.empty()) {
        for (size_t k = 0; k < production_rows.size(); ++k) {
            SimParams P = build_params(production_rows[k].tp_pts,
                                       production_rows[k].sl_pts,
                                       production_rows[k].z_thresh);
            // S30: rows are sorted by expectancy in print_leaderboard so the
            // grid index is lost.  ConfigStats.window was set during run_sim,
            // so use it to recover the actual window for this row.  For
            // non-fine grids this rewrites P.window to the same global value
            // it already had, so the emitted CSV row is unchanged.
            if (production_rows[k].window > 0)
                P.window = production_rows[k].window;
            append_csv_row(csv_out_path, basename_of(ticks_path),
                           instrument, label,
                           ProductionFill::short_name(), P,
                           production_rows[k]);
        }
        for (size_t k = 0; k < honest_rows.size(); ++k) {
            SimParams P = build_params(honest_rows[k].tp_pts,
                                       honest_rows[k].sl_pts,
                                       honest_rows[k].z_thresh);
            if (honest_rows[k].window > 0)
                P.window = honest_rows[k].window;
            append_csv_row(csv_out_path, basename_of(ticks_path),
                           instrument, label,
                           HonestFill::short_name(), P,
                           honest_rows[k]);
        }
    }

    if (trade_fp) std::fclose(trade_fp);

    int n_profitable_honest = 0;
    for (const auto& r : honest_rows)
        if (r.expectancy() > 0.0 && r.n_trades >= 10) ++n_profitable_honest;
    return n_profitable_honest > 0 ? 0 : 1;
}

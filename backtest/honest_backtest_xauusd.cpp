// =============================================================================
// honest_backtest_xauusd.cpp -- CRTP backtest with broker-realistic fills
// =============================================================================
//
// 2026-05-12 S26 Part 2 (Claude / Jo): definitive test of how to make the
// microscalper edge survive real costs.
//
// MOTIVATION
//   microscalper_crtp_sweep.cpp closes TP_HIT trades at pos.tp EXACT and
//   subtracts ZERO exit cost on TP_HIT. That assumption inflated the
//   parameter sweep's apparent profitability by ~$0.90 per trade vs broker
//   reality. See HANDOFF_S26_PART1B_VERIFICATION_REBUILD.md §6.1 and the
//   smoking-gun lines microscalper_crtp_sweep.cpp:671 and :822.
//
//   This tool is a parallel CRTP sweep that uses the SAME tick CSV format
//   and SAME parameter idea, but plugs in a broker-realistic fill model:
//
//     Entry fill: arrives latency_ticks AFTER the engine fires.
//                 LONG  pays ask at the arrival tick.
//                 SHORT receives bid at the arrival tick.
//     Exit fill:  arrives latency_ticks AFTER the engine sees the
//                 TP/SL trigger. LONG closes at bid; SHORT closes at ask.
//                 This is the fundamental fix: TP exits do NOT fill at
//                 pos.tp -- they fill at the worse side of the prevailing
//                 spread at fill arrival, exactly as live BlackBull does.
//
// CRTP STRUCTURE
//   FillModel is a template parameter on the simulator. Two specialisations
//   live in this TU:
//
//     ProductionFill -- replicates the pos.tp-exact bug of the existing
//                       sweep (entry crosses spread, TP fills at pos.tp,
//                       SL fills at trigger-side). Included so the operator
//                       can SEE the gap between the two on identical data.
//     HonestFill     -- next-tick worst-side fills with latency_ticks lag,
//                       which is what the live broker does.
//
//   Signal is also a CRTP parameter (Signal). For now we ship one signal,
//   ZScoreMR (z-score mean reversion over a rolling window). The production
//   GoldMicroScalper signal can later be ported as a second Signal type to
//   answer "does THIS specific signal have edge after honest costs."
//
// WHAT THIS TOOL ANSWERS
//   For the (Signal=ZScoreMR) generic mean-reversion signal:
//     - Under the production fill model, which (TP,SL,Z) configs look
//       profitable? (We expect: many. The model lies in their favour.)
//     - Under the honest fill model, which configs look profitable?
//       (This is the real answer.)
//     - What's the per-trade gap between the two models?
//       (This is the size of the fictional profit the bug created.)
//
//   If at least one config is profitable under HonestFill, that geometry is
//   a candidate for shadow-live promotion. If none are, the strategy class
//   is broken on this instrument and we move to indices.
//
// CSV SCHEMA
//   ts_ms,bid,ask,... (first three columns mandatory, rest ignored)
//   Same format as data/l2_ticks_*.csv.
//
// BUILD
//   g++ -std=c++17 -O2 -Wall -Wextra \
//       backtest/honest_backtest_xauusd.cpp \
//       -o backtest/honest_backtest_xauusd
//
// RUN
//   backtest/honest_backtest_xauusd data/l2_ticks_2026-04-16.csv
//   backtest/honest_backtest_xauusd --quick data/l2_ticks_2026-04-12.csv
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace omega::honest {

// ----- tick data ----------------------------------------------------------
struct Tick {
    int64_t ts_ms;
    double  bid;
    double  ask;
    double  mid() const { return 0.5 * (bid + ask); }
};

// ----- per-trade record ---------------------------------------------------
enum class ExitReason { TP_HIT, SL_HIT, EOD_FORCE };

struct Trade {
    int        id;
    bool       is_long;
    double     entry_px;     // broker-actual fill
    double     exit_px;      // broker-actual fill
    int        entry_idx;    // tick index
    int        exit_idx;
    ExitReason reason;
    double     net_usd;      // size * tick_mult * (signed price diff) - commission
};

// ----- aggregate stats ----------------------------------------------------
struct ConfigStats {
    double tp_pts;
    double sl_pts;
    double z_thresh;

    int    n_trades   = 0;
    int    n_wins     = 0;
    int    n_losses   = 0;
    int    n_tp_hit   = 0;
    int    n_sl_hit   = 0;

    double total_net_usd       = 0.0;
    double sum_win_usd         = 0.0;
    double sum_loss_usd        = 0.0;
    double max_drawdown_usd    = 0.0;
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
// CRTP: FillModel concept
//
//   Two static methods on each FillModel:
//
//     resolve_entry_fill(tick_arrival, is_long, engine_intended_px)
//       -> fill_px           (what price the broker actually fills at)
//
//     resolve_exit_fill(tick_arrival, is_long, engine_trigger_px,
//                       trigger_reason)
//       -> fill_px
//
//   tick_arrival is the tick AT WHICH the fill arrives at the broker
//   (i.e. engine_decision_tick + latency_ticks).
// =============================================================================

struct ProductionFill {
    // Mirrors microscalper_crtp_sweep.cpp lines 534, 671, 677.
    static double resolve_entry_fill(const Tick& t, bool is_long,
                                     double /*intended_px*/) {
        // Entry crosses the spread (this part the production was correct on)
        return is_long ? t.ask : t.bid;
    }
    static double resolve_exit_fill(const Tick& t, bool is_long,
                                    double trigger_px,
                                    ExitReason reason) {
        // The bug: TP exits fill at the trigger price exactly, ignoring
        // spread + latency. SL exits fill at the worse-side, which is at
        // least correct in direction (but still no latency cost).
        if (reason == ExitReason::TP_HIT) return trigger_px;
        return is_long ? t.bid : t.ask;
    }
    static const char* name() { return "production-fill (pos.tp exact)"; }
};

struct HonestFill {
    // What the live broker actually does on a retail FIX bridge.
    static double resolve_entry_fill(const Tick& t, bool is_long,
                                     double /*intended_px*/) {
        return is_long ? t.ask : t.bid;
    }
    static double resolve_exit_fill(const Tick& t, bool is_long,
                                    double /*trigger_px*/,
                                    ExitReason /*reason*/) {
        // ALL exits cross the spread the wrong way at the arrival tick.
        // No special case for TP -- that was the production lie.
        return is_long ? t.bid : t.ask;
    }
    static const char* name() { return "honest-fill (next-tick worst-side)"; }
};

// =============================================================================
// Signal: z-score mean reversion
//
//   ZScoreMR maintains a running mean/stdev of mid over window N. Emits:
//     +1 (go LONG)  when z(mid) < -z_thresh
//     -1 (go SHORT) when z(mid) > +z_thresh
//      0 otherwise
//
//   Causal: only uses past N samples; window=200 ticks ≈ 20-40s at typical
//   tick density.
// =============================================================================

struct ZScoreMR {
    int    window;
    double z_thresh;
    // O(1) update via running sums; valid once we've seen >= window samples.
    double sum  = 0.0;
    double sum2 = 0.0;
    std::vector<double> ring;
    int    head = 0;
    int    n    = 0;

    ZScoreMR(int w, double zt) : window(w), z_thresh(zt), ring(w, 0.0) {}

    int update(double mid) {
        // Pop oldest if full
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
            return 0;
        }
        const double m   = sum / window;
        const double var = std::max(sum2 / window - m * m, 1e-12);
        const double sd  = std::sqrt(var);
        const double z   = (mid - m) / sd;
        if (z >  z_thresh) return -1;   // mean reversion: short the high
        if (z < -z_thresh) return +1;   // mean reversion: long the low
        return 0;
    }
};

// =============================================================================
// CRTP simulator
//
//   Templated on FillModel so a single TU can run BOTH production and
//   honest fills on the same data with no code duplication. Signal is
//   passed by value (cheap, just a window of doubles).
// =============================================================================

template <typename Fill>
ConfigStats run_sim(const std::vector<Tick>& ticks,
                    double tp_pts, double sl_pts,
                    double z_thresh,
                    int window, int latency_ticks, int cooldown_ticks,
                    double size_lots, double tick_mult, double commission)
{
    ZScoreMR sig(window, z_thresh);

    ConfigStats S{tp_pts, sl_pts, z_thresh};
    std::vector<Trade> trades;
    trades.reserve(2000);

    bool   pos_open     = false;
    bool   pos_is_long  = false;
    double pos_entry_px = 0.0;
    int    pos_entry_ix = 0;

    int pending_entry_at  = -1;
    int pending_entry_dir = 0;
    int pending_exit_at   = -1;
    ExitReason pending_exit_reason = ExitReason::TP_HIT;
    double pending_exit_trigger_px = 0.0;

    int next_allowed_entry = window;

    const int N = static_cast<int>(ticks.size());

    auto book_trade = [&](double fill_px, int exit_idx, ExitReason reason) {
        const double pnl_units = pos_is_long
            ? (fill_px - pos_entry_px)
            : (pos_entry_px - fill_px);
        const double net = pnl_units * size_lots * tick_mult - commission;
        trades.push_back({
            static_cast<int>(trades.size()) + 1,
            pos_is_long, pos_entry_px, fill_px,
            pos_entry_ix, exit_idx, reason, net
        });
        if (net > 0) { ++S.n_wins;   S.sum_win_usd  += net; }
        else         { ++S.n_losses; S.sum_loss_usd += net; }
        if (reason == ExitReason::TP_HIT) ++S.n_tp_hit;
        if (reason == ExitReason::SL_HIT) ++S.n_sl_hit;
        S.total_net_usd += net;
        pos_open = false;
        next_allowed_entry = exit_idx + cooldown_ticks;
    };

    for (int i = 0; i < N; ++i) {
        const Tick& t = ticks[i];

        // (1) Resolve pending entry that's scheduled to land at this tick.
        if (pending_entry_at == i) {
            pos_is_long  = (pending_entry_dir == +1);
            pos_entry_px = Fill::resolve_entry_fill(t, pos_is_long, t.mid());
            pos_entry_ix = i;
            pos_open     = true;
            pending_entry_at  = -1;
            pending_entry_dir = 0;
            // fall through; possible same-tick exit on a violent move
        }

        // (2) Resolve pending exit that's scheduled to land at this tick.
        if (pos_open && pending_exit_at == i) {
            const double fpx = Fill::resolve_exit_fill(
                t, pos_is_long, pending_exit_trigger_px,
                pending_exit_reason);
            book_trade(fpx, i, pending_exit_reason);
            pending_exit_at = -1;
        }

        // (3) Update signal on every tick (warmup builds the window).
        const int fire = sig.update(t.mid());

        // (4) If position open and no pending exit, watch TP/SL triggers
        //     from the engine's POV (engine uses bid/ask, not fill price).
        if (pos_open && pending_exit_at == -1) {
            if (pos_is_long) {
                if (t.bid >= pos_entry_px + tp_pts) {
                    pending_exit_at = std::min(i + latency_ticks, N - 1);
                    pending_exit_reason = ExitReason::TP_HIT;
                    pending_exit_trigger_px = pos_entry_px + tp_pts;
                } else if (t.bid <= pos_entry_px - sl_pts) {
                    pending_exit_at = std::min(i + latency_ticks, N - 1);
                    pending_exit_reason = ExitReason::SL_HIT;
                    pending_exit_trigger_px = pos_entry_px - sl_pts;
                }
            } else {
                if (t.ask <= pos_entry_px - tp_pts) {
                    pending_exit_at = std::min(i + latency_ticks, N - 1);
                    pending_exit_reason = ExitReason::TP_HIT;
                    pending_exit_trigger_px = pos_entry_px - tp_pts;
                } else if (t.ask >= pos_entry_px + sl_pts) {
                    pending_exit_at = std::min(i + latency_ticks, N - 1);
                    pending_exit_reason = ExitReason::SL_HIT;
                    pending_exit_trigger_px = pos_entry_px + sl_pts;
                }
            }
        }

        // (5) If flat and past cooldown, check signal.
        if (!pos_open && pending_entry_at == -1
            && i >= next_allowed_entry && fire != 0)
        {
            pending_entry_at  = std::min(i + latency_ticks, N - 1);
            pending_entry_dir = fire;
        }
    }

    // Force-close at EOD if still open.
    if (pos_open) {
        const Tick& last = ticks.back();
        const double fpx = Fill::resolve_exit_fill(
            last, pos_is_long, last.mid(), ExitReason::EOD_FORCE);
        book_trade(fpx, N - 1, ExitReason::EOD_FORCE);
    }

    // Drawdown pass over the trade equity curve.
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
// CSV loader (just ts_ms, bid, ask; rest ignored).
// =============================================================================

bool load_ticks(const std::string& path, std::vector<Tick>& out) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[FATAL] cannot open %s\n", path.c_str());
        return false;
    }
    std::string line;
    // Header
    if (!std::getline(f, line)) return false;
    // Determine column indices for ts_ms, bid, ask
    int ix_ts = -1, ix_bid = -1, ix_ask = -1;
    {
        std::stringstream ss(line);
        std::string cell;
        int col = 0;
        while (std::getline(ss, cell, ',')) {
            // strip whitespace
            while (!cell.empty() && (cell.back() == ' ' || cell.back() == '\r'
                                      || cell.back() == '\n'))
                cell.pop_back();
            if (cell == "ts_ms")       ix_ts  = col;
            else if (cell == "bid")    ix_bid = col;
            else if (cell == "ask")    ix_ask = col;
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
        Tick t{0, 0.0, 0.0};
        while (std::getline(ss, cell, ',')) {
            try {
                if      (col == ix_ts ) t.ts_ms = std::stoll(cell);
                else if (col == ix_bid) t.bid   = std::stod(cell);
                else if (col == ix_ask) t.ask   = std::stod(cell);
            } catch (...) { /* skip malformed row */ }
            ++col;
        }
        if (t.ts_ms > 0 && t.bid > 0.0 && t.ask > 0.0) out.push_back(t);
    }
    return true;
}

// =============================================================================
// Grid + printing
// =============================================================================

struct GridPoint { double tp; double sl; double z; };

std::vector<GridPoint> build_grid(bool quick) {
    if (quick) {
        return {
            {0.79, 3.0, 2.0},   // production-like, expected to bleed
            {2.0,  4.0, 2.0},   // wider-TP candidate
            {3.0,  5.0, 2.5},   // wider-still candidate
        };
    }
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
                   const std::vector<ConfigStats>& production_rows)
{
    std::printf("\n");
    std::printf("================================================================="
                "===================================\n");
    std::printf(" VERDICT\n");
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

    std::printf(" production-fill: %d/%zu configs look profitable "
                "(this is the LIE the production sweep tells)\n",
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
        std::printf(" --> CANDIDATE for shadow-live promotion. ONE DAY OF DATA "
                    "IS NOT ENOUGH; re-run on 14+ days before any live capital.\n");
    } else {
        std::printf(" NO honest-fill config survives. The microscalping geometry\n");
        std::printf(" (small TP, tight SL) does not extract edge from this\n");
        std::printf(" instrument after real costs. Next steps:\n");
        std::printf("   1. Re-run on a different day; this could be regime-specific.\n");
        std::printf("   2. Re-run against indices (l2_ticks_US500_*, _NAS100_*).\n");
        std::printf("   3. If indices also fail, the strategy class is the\n");
        std::printf("      problem -- move to a swing/session engine instead.\n");
    }

    // Diagnostic: how much fictional profit does the production model create?
    double prod_total = 0.0, honest_total = 0.0;
    for (const auto& r : production_rows) prod_total   += r.total_net_usd;
    for (const auto& r : honest_rows)     honest_total += r.total_net_usd;
    std::printf("\n");
    std::printf(" FICTIONAL-PROFIT GAP (sum across all configs):\n");
    std::printf("   production-fill total $%+.2f vs honest-fill total $%+.2f\n",
                prod_total, honest_total);
    std::printf("   gap = $%+.2f  <-- this is the size of the lie\n",
                prod_total - honest_total);
    std::printf("================================================================="
                "===================================\n");
    std::fflush(stdout);
}

}  // namespace omega::honest

// =============================================================================
// main
// =============================================================================

int main(int argc, char** argv) {
    using namespace omega::honest;

    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s [--quick] <ticks.csv>\n"
            "  --quick   3-point grid for fast smoke test\n", argv[0]);
        return 2;
    }

    bool quick = false;
    std::string ticks_path;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) quick = true;
        else ticks_path = argv[i];
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
    std::printf("[INFO] loaded %zu ticks from %s in %lld ms\n",
                ticks.size(), ticks_path.c_str(), (long long)load_ms);
    if (ticks.size() < 1000) {
        std::fprintf(stderr,
            "[FATAL] too few ticks (%zu) -- need >=1000 for warmup\n",
            ticks.size());
        return 2;
    }

    // Diagnostic on the data
    double sum_spread = 0.0; double med_spread = 0.0;
    {
        std::vector<double> spreads; spreads.reserve(ticks.size());
        for (const auto& t : ticks) {
            const double s = t.ask - t.bid;
            spreads.push_back(s);
            sum_spread += s;
        }
        std::sort(spreads.begin(), spreads.end());
        med_spread = spreads[spreads.size() / 2];
    }
    std::printf("[INFO] spread stats: mean=%.4fpt median=%.4fpt\n",
                sum_spread / ticks.size(), med_spread);

    const auto grid = build_grid(quick);
    std::printf("[INFO] running %zu configs under BOTH fill models...\n",
                grid.size());

    constexpr int    WINDOW         = 200;
    constexpr int    LATENCY_TICKS  = 1;
    constexpr int    COOLDOWN_TICKS = 100;
    constexpr double SIZE_LOTS      = 0.01;
    constexpr double TICK_MULT      = 100.0;   // XAUUSD 0.01 lot
    constexpr double COMMISSION     = 0.0;

    std::vector<ConfigStats> honest_rows;
    std::vector<ConfigStats> production_rows;
    honest_rows.reserve(grid.size());
    production_rows.reserve(grid.size());

    auto t2 = std::chrono::steady_clock::now();
    for (const auto& g : grid) {
        production_rows.push_back(
            run_sim<ProductionFill>(
                ticks, g.tp, g.sl, g.z,
                WINDOW, LATENCY_TICKS, COOLDOWN_TICKS,
                SIZE_LOTS, TICK_MULT, COMMISSION));
        honest_rows.push_back(
            run_sim<HonestFill>(
                ticks, g.tp, g.sl, g.z,
                WINDOW, LATENCY_TICKS, COOLDOWN_TICKS,
                SIZE_LOTS, TICK_MULT, COMMISSION));
    }
    auto t3 = std::chrono::steady_clock::now();
    const auto sim_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(t3 - t2).count();
    std::printf("[INFO] simulated %zu configs in %lld ms\n",
                grid.size() * 2, (long long)sim_ms);

    print_leaderboard(ProductionFill::name(), production_rows);
    print_leaderboard(HonestFill::name(),     honest_rows);
    print_verdict(honest_rows, production_rows);

    // Exit non-zero if honest-fill found no profitable config.
    int n_profitable_honest = 0;
    for (const auto& r : honest_rows)
        if (r.expectancy() > 0.0 && r.n_trades >= 10) ++n_profitable_honest;
    return n_profitable_honest > 0 ? 0 : 1;
}

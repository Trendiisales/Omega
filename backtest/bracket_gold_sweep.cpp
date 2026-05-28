// =============================================================================
// bracket_gold_sweep.cpp -- parameter sweep harness for GoldBracketEngine.
//
// Built 2026-05-28 (S37-Y) after 2yr audit showed -$311 / 28.4% WR with
// default params across all sessions. Operator directive: "keep searching
// until we find an edge that will work, especially with the last year's data".
//
// Strategy: load tape into RAM once, replay in parallel against many engine
// configurations. Each thread owns one GoldBracketEngine + tagged trade
// collector. Reports per-config aggregate + session breakdown so config
// quality is judged on the same fidelity bar as bracket_gold_2yr_audit.
//
// FIDELITY (per HARNESS_FIDELITY_CHECKLIST.md):
//   - Production omega::GoldBracketEngine class -- no inline reimpl.
//   - OmegaTimeShim per-thread g_sim_now_ms set before each tick.
//   - shadow_mode=true on every engine instance.
//
// GRID (subject to expansion):
//   TRAIL_ACTIVATION_PTS in {1.5, 3.0, 5.0, 8.0, 12.0}
//   TRAIL_DISTANCE_PTS   in {0.5, 1.5, 3.0, 5.0}
//   LOSS_CUT_PCT         in {0.05, 0.10, 0.20, 1.00}  (1.00 = effectively off)
//   STRUCTURE_LOOKBACK   in {20, 30, 60}
//   = 5 * 4 * 4 * 3 = 240 configs
//
// USAGE:
//   ./build/bracket_gold_sweep /Users/jo/Tick/2yr_XAUUSD_tick_fresh.csv [N_THREADS]
//   ./build/bracket_gold_sweep /tmp/xau_last12mo.csv 8
//
// OUTPUT (stdout):
//   Per-config one-line summary CSV (sortable by gross/PF/Sharpe).
//   Top-20 leaderboard at end with full session breakdown.
// =============================================================================

#include "OmegaTimeShim.hpp"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <chrono>

#include "OmegaTradeLedger.hpp"
#include "BracketEngine.hpp"

// ---------------------------------------------------------------------------
// Tick + parse
// ---------------------------------------------------------------------------
struct Tick { int64_t ts_ms; double bid; double ask; };

static bool parse_duka_row(const char* line, Tick& out) {
    int Y, M, D, h, m, s;
    double bid, ask;
    if (std::sscanf(line, "%4d%2d%2d,%d:%d:%d,%lf,%lf",
                    &Y, &M, &D, &h, &m, &s, &bid, &ask) != 8) return false;
    if (bid <= 0.0 || ask <= 0.0 || ask < bid) return false;
    std::tm tm{};
    tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D;
    tm.tm_hour = h;        tm.tm_min  = m;    tm.tm_sec  = s;
    const time_t epoch_s = timegm(&tm);
    if (epoch_s <= 0) return false;
    out.ts_ms = static_cast<int64_t>(epoch_s) * 1000LL;
    out.bid = bid; out.ask = ask;
    return true;
}

// ---------------------------------------------------------------------------
// Session tagger
// ---------------------------------------------------------------------------
static const char* utc_session_of_ts_ms(int64_t ts_ms) {
    const time_t t_s = static_cast<time_t>(ts_ms / 1000LL);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t_s);
#else
    gmtime_r(&t_s, &tm);
#endif
    const int hr = tm.tm_hour;
    if (hr >= 22 || hr < 7)  return "Asia";
    if (hr >= 7  && hr < 12) return "London";
    if (hr >= 12 && hr < 17) return "NY";
    return "Late_NY";
}

// ---------------------------------------------------------------------------
// Per-config result
// ---------------------------------------------------------------------------
struct Cfg {
    double trail_act;
    double trail_dist;
    double loss_cut;
    int    lookback;
};

struct Result {
    Cfg cfg;
    int    n_trades = 0, wins = 0;
    double gross = 0, gw = 0, gl = 0;
    double max_dd = 0;
    // Session breakdown
    int    n_asia = 0, n_london = 0, n_ny = 0, n_late = 0;
    double g_asia = 0, g_london = 0, g_ny = 0, g_late = 0;

    double wr() const { return n_trades ? 100.0 * wins / n_trades : 0.0; }
    double pf() const { return gl > 0 ? gw / gl : 0.0; }
};

// ---------------------------------------------------------------------------
// Run one config against the tape (in-memory ticks).
// ---------------------------------------------------------------------------
static Result run_one(const std::vector<Tick>& ticks, const Cfg& cfg) {
    Result r{};
    r.cfg = cfg;

    omega::GoldBracketEngine eng;
    eng.symbol      = "XAUUSD";
    eng.shadow_mode = true;
    eng.ENTRY_SIZE  = 0.01;
    eng.TRAIL_ACTIVATION_PTS = cfg.trail_act;
    eng.TRAIL_DISTANCE_PTS   = cfg.trail_dist;
    eng.LOSS_CUT_PCT         = cfg.loss_cut;
    eng.STRUCTURE_LOOKBACK   = cfg.lookback;

    double running_eq = 0, peak_eq = 0;

    // Cost model: subtract round-trip spread from gross PnL.
    //   apply_realistic_costs uses spreadAtEntry directly:
    //     slip_entry = (spread/2) * tick_mult * size
    //   In our TradeRecord.pnl unit (= price_diff * size), this collapses to:
    //     slip_entry_in_pnl_units = (spread/2) * size  (when tick_mult=100, size=0.01 -> 1.0)
    //   Wait -- tick_mult=100, size=0.01 -> tick_mult*size=1, so slip_in_$$$ = spread/2.
    //   TradeRecord.pnl from engine is price_diff * size (no tick_mult applied).
    //   For comparable units in $$$:
    //     gross_$$$  = pnl * tick_mult = pnl * 100
    //     slip_$$$   = (spread/2) * tick_mult * size = (spread/2) * 1 = spread/2
    //     net_$$$    = pnl*100 - 2*(spread/2)        (non-TP, both legs)
    //                = pnl*100 - spread
    //   We compute pnl_after_cost back in pnl-units by dividing by 100:
    //     pnl_after = pnl - spread/100               (non-TP)
    //     pnl_after = pnl - (spread/2)/100           (TP)
    //   Use spreadAtEntry as the spread estimate (BlackBull XAU live ~0.30).
    auto cb = [&](const omega::TradeRecord& t) {
        const double round_trip_spread = (t.exitReason == "TP_HIT")
            ? (t.spreadAtEntry * 0.5)   // TP: no exit slip
            :  t.spreadAtEntry;          // non-TP: both legs
        const double pnl = t.pnl - (round_trip_spread / 100.0);
        ++r.n_trades;
        r.gross += pnl;
        running_eq += pnl;
        if (running_eq > peak_eq) peak_eq = running_eq;
        const double dd = peak_eq - running_eq;
        if (dd > r.max_dd) r.max_dd = dd;
        if (pnl > 0) { ++r.wins; r.gw += pnl; }
        else         { r.gl += -pnl; }

        const char* sess = utc_session_of_ts_ms(omega::bt::g_sim_now_ms);
        if      (!std::strcmp(sess, "Asia"))    { ++r.n_asia;   r.g_asia   += pnl; }
        else if (!std::strcmp(sess, "London"))  { ++r.n_london; r.g_london += pnl; }
        else if (!std::strcmp(sess, "NY"))      { ++r.n_ny;     r.g_ny     += pnl; }
        else                                    { ++r.n_late;   r.g_late   += pnl; }
    };

    for (const Tick& t : ticks) {
        omega::bt::set_sim_time(t.ts_ms);
        eng.on_tick(t.bid, t.ask, t.ts_ms,
                    /*can_enter*/ true,
                    /*macro_regime*/ "NEUTRAL",
                    cb);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <xau_tick_csv> [n_threads]\n", argv[0]);
        return 1;
    }
    const std::string path = argv[1];
    const int n_threads = (argc >= 3) ? std::max(1, std::atoi(argv[2]))
                                      : static_cast<int>(std::thread::hardware_concurrency());

    // -----------------------------------------------------------------------
    // Load ticks into RAM (24 bytes/tick * 80M = ~1.9GB). Single fopen + fgets
    // loop -- fast enough relative to engine cost.
    // -----------------------------------------------------------------------
    std::vector<Tick> ticks;
    ticks.reserve(160'000'000);  // headroom for 2yr corpus
    {
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) { std::fprintf(stderr, "ERROR: cannot open %s\n", path.c_str()); return 2; }
        char line[1024];
        int64_t lines = 0, parsed = 0;
        std::fprintf(stderr, "[SWEEP] loading %s ...\n", path.c_str());
        const auto t0 = std::chrono::steady_clock::now();
        while (std::fgets(line, sizeof(line), f)) {
            ++lines;
            Tick t;
            if (!parse_duka_row(line, t)) continue;
            ++parsed;
            ticks.push_back(t);
            if (lines % 10'000'000 == 0) {
                std::fprintf(stderr, "\r[SWEEP] %lldM lines  %zu ticks loaded  ",
                             (long long)(lines / 1'000'000), ticks.size());
            }
        }
        std::fclose(f);
        const auto elapsed_s = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "\n[SWEEP] %lld lines, %lld parsed, %zu ticks in %.1fs\n",
                     (long long)lines, (long long)parsed, ticks.size(), elapsed_s);
    }

    // -----------------------------------------------------------------------
    // Build config grid
    // -----------------------------------------------------------------------
    std::vector<Cfg> grid;
    for (double ta : {1.5, 3.0, 5.0, 8.0, 12.0}) {
      for (double td : {0.5, 1.5, 3.0, 5.0}) {
        for (double lc : {0.05, 0.10, 0.20, 1.00}) {
          for (int lb : {20, 30, 60}) {
            // SKIP: trail distance >= activation makes trail trigger immediately
            // (locks negative or zero -- never lets winner run). Drop those.
            if (td >= ta) continue;
            grid.push_back(Cfg{ta, td, lc, lb});
          }
        }
      }
    }
    std::fprintf(stderr, "[SWEEP] %zu configs * %zu ticks, %d threads\n",
                 grid.size(), ticks.size(), n_threads);

    // -----------------------------------------------------------------------
    // Run grid with thread pool. Atomic config index dispatcher.
    // -----------------------------------------------------------------------
    std::vector<Result> results(grid.size());
    std::atomic<size_t> next_idx{0};
    std::atomic<int>    done_count{0};
    std::mutex log_mtx;

    auto worker = [&]() {
        for (;;) {
            const size_t i = next_idx.fetch_add(1);
            if (i >= grid.size()) return;
            results[i] = run_one(ticks, grid[i]);
            const int d = done_count.fetch_add(1) + 1;
            std::lock_guard<std::mutex> lk(log_mtx);
            const auto& r = results[i];
            std::fprintf(stderr,
                "[CFG %d/%zu] ta=%.1f td=%.1f lc=%.2f lb=%d  n=%-6d  WR=%.1f%%  PF=%.2f  gross=%+.2f  DD=%.2f\n",
                d, grid.size(),
                r.cfg.trail_act, r.cfg.trail_dist, r.cfg.loss_cut, r.cfg.lookback,
                r.n_trades, r.wr(), r.pf(), r.gross, r.max_dd);
        }
    };

    std::vector<std::thread> pool;
    for (int t = 0; t < n_threads; ++t) pool.emplace_back(worker);
    for (auto& th : pool) th.join();

    // -----------------------------------------------------------------------
    // Sort by gross PnL (descending) and print leaderboard
    // -----------------------------------------------------------------------
    std::sort(results.begin(), results.end(),
              [](const Result& a, const Result& b){ return a.gross > b.gross; });

    std::printf("\n");
    std::printf("==================================================================================================\n");
    std::printf("  bracket_gold_sweep  --  %s  --  %zu configs\n", path.c_str(), results.size());
    std::printf("==================================================================================================\n");
    std::printf("rank,trail_act,trail_dist,loss_cut,lookback,n,WR%%,PF,gross,MaxDD,asia_g,london_g,ny_g,late_g,n_london,n_ny\n");
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        std::printf("%zu,%.1f,%.1f,%.2f,%d,%d,%.2f,%.3f,%+.2f,%.2f,%+.2f,%+.2f,%+.2f,%+.2f,%d,%d\n",
                    i + 1,
                    r.cfg.trail_act, r.cfg.trail_dist, r.cfg.loss_cut, r.cfg.lookback,
                    r.n_trades, r.wr(), r.pf(), r.gross, r.max_dd,
                    r.g_asia, r.g_london, r.g_ny, r.g_late,
                    r.n_london, r.n_ny);
    }

    std::printf("\nTop 5 detail:\n");
    for (size_t i = 0; i < std::min<size_t>(5, results.size()); ++i) {
        const auto& r = results[i];
        std::printf("  #%zu ta=%.1f td=%.1f lc=%.2f lb=%d  n=%d WR=%.2f%% PF=%.3f gross=%+.2f DD=%.2f\n",
                    i + 1, r.cfg.trail_act, r.cfg.trail_dist, r.cfg.loss_cut, r.cfg.lookback,
                    r.n_trades, r.wr(), r.pf(), r.gross, r.max_dd);
        std::printf("     Asia:    n=%d gross=%+.2f\n", r.n_asia,   r.g_asia);
        std::printf("     London:  n=%d gross=%+.2f\n", r.n_london, r.g_london);
        std::printf("     NY:      n=%d gross=%+.2f\n", r.n_ny,     r.g_ny);
        std::printf("     Late_NY: n=%d gross=%+.2f\n", r.n_late,   r.g_late);
    }
    return 0;
}

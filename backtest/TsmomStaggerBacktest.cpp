// =============================================================================
//  TsmomStaggerBacktest.cpp -- S100 stagger verification harness.
//
//  Tick-level backtest that reads Dukascopy CSV (timestamp_ms,ask,bid),
//  builds H1 bars on-the-fly, calls on_h1_bar() at bar close, then
//  continues feeding ticks via on_tick() between bars -- which is where
//  the stagger mechanism executes pending entries.
//
//  Runs TWO copies of TsmomPortfolio in lockstep over the same tick
//  stream:
//    (A) baseline  -- all cells entry_delay_sec = 0 (immediate, pre-S100)
//    (B) stagger   -- H1=0, H2=300, H4=600, H6=900, D1=1200
//
//  Outputs: per-run equity, trade count, PnL, and a correlated-entry
//  metric (# of bar boundaries where >= 2 cells entered within 60s).
//
//  Build (sandbox):
//    cd ~/omega_repo
//    g++ -O2 -std=c++17 -Iinclude -o backtest/TsmomStaggerBacktest \
//        backtest/TsmomStaggerBacktest.cpp
//
//  Run:
//    ./backtest/TsmomStaggerBacktest --csv ~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv
//
//  Options:
//    --csv <path>       Dukascopy tick CSV (timestamp_ms,askPrice,bidPrice)
//    --max-ticks <n>    Stop after N ticks (0 = all, default)
//    --max-pos <n>      max_positions_per_cell (default: 10)
//    --quiet            Suppress per-trade printf
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <unistd.h>

#include "../include/OmegaTradeLedger.hpp"
#include "../include/TsmomEngine.hpp"

namespace {

// -----------------------------------------------------------------------------
//  Args
// -----------------------------------------------------------------------------
struct Args {
    std::string csv_path;
    int64_t     max_ticks = 0;      // 0 = unlimited
    int         max_pos   = 10;
    bool        quiet     = false;
};

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto take = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "ERROR: '%s' needs arg\n", argv[i]); std::exit(2); }
            return std::string(argv[++i]);
        };
        if      (s == "--csv")       a.csv_path  = take();
        else if (s == "--max-ticks") a.max_ticks  = std::atoll(take().c_str());
        else if (s == "--max-pos")   a.max_pos    = std::atoi(take().c_str());
        else if (s == "--quiet")     a.quiet      = true;
        else { std::fprintf(stderr, "ERROR: unknown arg '%s'\n", s.c_str()); std::exit(2); }
    }
    if (a.csv_path.empty()) {
        std::fprintf(stderr, "Usage: TsmomStaggerBacktest --csv <dukascopy_tick.csv> [--max-ticks N] [--max-pos N] [--quiet]\n");
        std::exit(1);
    }
    return a;
}

// -----------------------------------------------------------------------------
//  H1 bar builder from ticks
// -----------------------------------------------------------------------------
struct BarBuilder {
    static constexpr int64_t H1_MS = 3600LL * 1000LL;

    int64_t bar_start_ms = 0;
    double  open = 0, high = 0, low = 0, close = 0;
    bool    has_data = false;

    // Returns true if the tick closed a bar (bar is filled in `out`)
    bool on_tick(int64_t ts_ms, double mid, omega::TsmomBar& out) {
        int64_t bar_slot = (ts_ms / H1_MS) * H1_MS;

        if (!has_data) {
            bar_start_ms = bar_slot;
            open = high = low = close = mid;
            has_data = true;
            return false;
        }

        if (bar_slot != bar_start_ms) {
            // New bar started — emit the completed bar
            out.bar_start_ms = bar_start_ms;
            out.open  = open;
            out.high  = high;
            out.low   = low;
            out.close = close;

            // Start new bar
            bar_start_ms = bar_slot;
            open = high = low = close = mid;
            return true;
        }

        // Same bar — update OHLC
        if (mid > high) high = mid;
        if (mid < low)  low  = mid;
        close = mid;
        return false;
    }
};

// -----------------------------------------------------------------------------
//  Weekend gate (matches production: Fri 20:00 UTC through end of Sunday)
// -----------------------------------------------------------------------------
static bool is_weekend(int64_t ts_ms) {
    // Convert to seconds, then to day-of-week
    const int64_t ts_sec = ts_ms / 1000;
    // Unix epoch (1970-01-01) was a Thursday (day 4).
    // 0=Thu, 1=Fri, 2=Sat, 3=Sun, 4=Mon, 5=Tue, 6=Wed
    const int64_t day_since_epoch = ts_sec / 86400;
    const int dow = (int)((day_since_epoch + 4) % 7);  // 0=Sun, 1=Mon, ..., 6=Sat
    if (dow == 0 || dow == 6) return true;  // Sat or Sun
    // Friday after 20:00 UTC
    if (dow == 5) {
        const int hour = (int)((ts_sec % 86400) / 3600);
        if (hour >= 20) return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
//  Trade stats collector
// -----------------------------------------------------------------------------
struct Stats {
    int    trades      = 0;
    double total_pnl   = 0.0;
    int    winners     = 0;
    int    losers      = 0;
    // Correlated entry tracking: count how many times >= 2 cells enter
    // within 60 seconds of each other
    std::vector<int64_t> entry_timestamps;  // all entry timestamps
    int    correlated_clusters = 0;

    void on_trade(const omega::TradeRecord& tr) {
        ++trades;
        double pnl_usd = tr.pnl * 100.0;
        total_pnl += pnl_usd;
        if (pnl_usd > 0) ++winners;
        else              ++losers;
        entry_timestamps.push_back(tr.entryTs);
    }

    void compute_correlation() {
        if (entry_timestamps.size() < 2) return;
        std::sort(entry_timestamps.begin(), entry_timestamps.end());

        // Sliding window: count clusters where >= 2 entries within 60s
        size_t i = 0;
        while (i < entry_timestamps.size()) {
            size_t j = i + 1;
            while (j < entry_timestamps.size() &&
                   (entry_timestamps[j] - entry_timestamps[i]) <= 60) {
                ++j;
            }
            int cluster_size = (int)(j - i);
            if (cluster_size >= 2) {
                ++correlated_clusters;
                i = j;  // skip past this cluster
            } else {
                ++i;
            }
        }
    }
};

// Configure a TsmomPortfolio with given stagger mode
static void configure(omega::TsmomPortfolio& p, int max_pos, bool stagger_on) {
    p.shadow_mode             = true;
    p.enabled                 = true;
    p.max_concurrent          = max_pos * 5 + 5;
    p.max_positions_per_cell  = max_pos;
    p.risk_pct                = 0.005;
    p.start_equity            = 10000.0;
    p.margin_call             = 1000.0;
    p.max_lot_cap             = 0.02;
    p.block_on_risk_off       = false;
    p.warmup_csv_path         = "";
    p.init();

    if (!stagger_on) {
        // Override: force all cells to immediate entry
        p.h1_long_.entry_delay_sec = 0;
        p.h2_long_.entry_delay_sec = 0;
        p.h4_long_.entry_delay_sec = 0;
        p.h6_long_.entry_delay_sec = 0;
        p.d1_long_.entry_delay_sec = 0;
    }
    // else: init() already set H1=0, H2=300, H4=600, H6=900, D1=1200
}

}  // namespace

// =============================================================================
//  main
// =============================================================================
int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);

    std::ifstream fin(a.csv_path);
    if (!fin.is_open()) {
        std::fprintf(stderr, "ERROR: cannot open '%s'\n", a.csv_path.c_str());
        return 3;
    }

    // Skip header
    std::string line;
    std::getline(fin, line);  // "timestamp,askPrice,bidPrice" or similar

    // Set up two portfolios
    omega::TsmomPortfolio baseline;
    omega::TsmomPortfolio stagger;
    configure(baseline, a.max_pos, false);
    configure(stagger,  a.max_pos, true);

    Stats stats_base, stats_stag;

    auto base_cb = [&](const omega::TradeRecord& tr) { stats_base.on_trade(tr); };
    auto stag_cb = [&](const omega::TradeRecord& tr) { stats_stag.on_trade(tr); };

    BarBuilder bb_base, bb_stag;
    omega::TsmomBar bar_base, bar_stag;

    int64_t tick_count = 0;
    int     bars_count = 0;

    // Suppress V1 printf noise when --quiet
    if (a.quiet) {
        // Redirect stdout to /dev/null during the main loop
        // (we'll restore it for the summary)
    }

    std::printf("[STAGGER-BT] loading ticks from %s ...\n", a.csv_path.c_str());
    std::printf("[STAGGER-BT] max_pos=%d, max_ticks=%lld\n",
                a.max_pos, (long long)a.max_ticks);
    std::fflush(stdout);

    // Redirect stdout for quiet mode
    int saved_fd = -1;
    if (a.quiet) {
        std::fflush(stdout);
        saved_fd = dup(1);
        FILE* devnull = fopen("/dev/null", "w");
        if (devnull) dup2(fileno(devnull), 1);
    }

    while (std::getline(fin, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Parse: timestamp_ms,askPrice,bidPrice
        char* p1 = nullptr;
        int64_t ts_ms = std::strtoll(line.c_str(), &p1, 10);
        if (!p1 || *p1 != ',') continue;
        char* p2 = nullptr;
        double ask = std::strtod(p1 + 1, &p2);
        if (!p2 || *p2 != ',') continue;
        char* p3 = nullptr;
        double bid = std::strtod(p2 + 1, &p3);
        if (!std::isfinite(bid) || !std::isfinite(ask) || bid <= 0 || ask <= 0) continue;

        // Skip weekends
        if (is_weekend(ts_ms)) continue;

        double mid = (bid + ask) * 0.5;
        ++tick_count;

        // Feed bar builders
        bool bar_closed_base = bb_base.on_tick(ts_ms, mid, bar_base);
        bool bar_closed_stag = bb_stag.on_tick(ts_ms, mid, bar_stag);

        // If a bar closed, call on_h1_bar
        if (bar_closed_base) {
            int64_t bar_end_ms = bar_base.bar_start_ms + 3600LL * 1000LL;
            baseline.on_h1_bar(bar_base, bid, ask, 0.0, bar_end_ms, base_cb);
            ++bars_count;
        }
        if (bar_closed_stag) {
            int64_t bar_end_ms = bar_stag.bar_start_ms + 3600LL * 1000LL;
            stagger.on_h1_bar(bar_stag, bid, ask, 0.0, bar_end_ms, stag_cb);
        }

        // Feed on_tick to BOTH (manages open positions + executes stagger pending)
        baseline.on_tick(bid, ask, ts_ms, base_cb);
        stagger.on_tick(bid, ask, ts_ms, stag_cb);

        if (a.max_ticks > 0 && tick_count >= a.max_ticks) break;

        // Progress report every 10M ticks
        if (tick_count % 10000000 == 0) {
            std::fprintf(stderr, "[STAGGER-BT] %lldM ticks, %d bars ...\r",
                         (long long)(tick_count / 1000000), bars_count);
        }
    }

    // Force-close residuals
    {
        double last_bid = bb_base.close;
        double last_ask = bb_base.close;  // approximate
        int64_t end_ms = bb_base.bar_start_ms + 3600LL * 1000LL;
        baseline.force_close_all(last_bid, last_ask, end_ms, base_cb);
        stagger.force_close_all(last_bid, last_ask, end_ms, stag_cb);
    }

    // Restore stdout
    if (saved_fd >= 0) {
        std::fflush(stdout);
        dup2(saved_fd, 1);
        close(saved_fd);
    }

    // Compute correlated entry stats
    stats_base.compute_correlation();
    stats_stag.compute_correlation();

    auto base_s = baseline.status();
    auto stag_s = stagger.status();

    std::printf("\n[STAGGER-BT] ====== RESULTS (%lld ticks, %d H1 bars) ======\n\n",
                (long long)tick_count, bars_count);

    std::printf("  %-25s %15s %15s\n", "", "BASELINE", "STAGGER");
    std::printf("  %-25s %15s %15s\n", "", "(delay=0)", "(H1=0,H2=5m,H4=10m...)");
    std::printf("  %-25s %15s %15s\n", "-------------------------", "---------------", "---------------");
    std::printf("  %-25s %15.2f %15.2f\n", "Final equity ($)",  base_s.equity,  stag_s.equity);
    std::printf("  %-25s %15.2f %15.2f\n", "Peak equity ($)",   base_s.peak,    stag_s.peak);
    std::printf("  %-25s %15.2f%% %14.2f%%\n", "Max drawdown",  base_s.max_dd_pct * 100.0, stag_s.max_dd_pct * 100.0);
    std::printf("  %-25s %15d %15d\n",   "Total trades",        stats_base.trades, stats_stag.trades);
    std::printf("  %-25s %15d %15d\n",   "Winners",             stats_base.winners, stats_stag.winners);
    std::printf("  %-25s %15d %15d\n",   "Losers",              stats_base.losers, stats_stag.losers);
    double wr_base = stats_base.trades > 0 ? 100.0 * stats_base.winners / stats_base.trades : 0;
    double wr_stag = stats_stag.trades > 0 ? 100.0 * stats_stag.winners / stats_stag.trades : 0;
    std::printf("  %-25s %14.1f%% %14.1f%%\n", "Win rate",      wr_base, wr_stag);
    std::printf("  %-25s %15.2f %15.2f\n", "Total PnL ($)",     stats_base.total_pnl, stats_stag.total_pnl);
    std::printf("  %-25s %15d %15d\n",   "Correlated clusters", stats_base.correlated_clusters, stats_stag.correlated_clusters);
    std::printf("    (>=2 entries within 60s of each other)\n");

    double pnl_delta = stats_stag.total_pnl - stats_base.total_pnl;
    int cluster_delta = stats_stag.correlated_clusters - stats_base.correlated_clusters;
    std::printf("\n  %-25s %+.2f\n", "PnL delta (stagger-base)", pnl_delta);
    std::printf("  %-25s %+d\n",    "Cluster delta",             cluster_delta);

    std::printf("\n[STAGGER-BT] done.\n");
    return 0;
}

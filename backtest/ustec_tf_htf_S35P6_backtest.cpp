// =============================================================================
//  ustec_tf_htf_S35P6_backtest.cpp -- backtest harness for
//                                     UstecTrendFollowHtfEngine.
// =============================================================================
//
//  PURPOSE
//
//      Drive the new UstecTrendFollowHtfEngine through historic NSXUSD tick
//      data and produce per-period summaries that should roughly track
//      the §C 3-period intersection results that justified building the
//      engine in the first place. Because the engine wraps the raw signals
//      with the ProtectedEngineGuards bundle (BE arm, trail-after-BE,
//      ATR floor, etc) the engine's per-cell totals will differ from the
//      bare edge_hunt cells -- the BE+trail will save some losing trades
//      and clip some winning trades shorter, with net usually mildly
//      positive (matches what the XAU S35-P4 backtest showed for
//      XauThreeBar30m).
//
//  INPUT
//
//      One or more tick CSVs in edge_hunt format: ts_ms,bid,ask
//      (the same files produced by histdata_to_edgehunt for the §B sweep).
//
//  OUTPUT
//
//      <prefix>_trades.csv   -- per-trade ledger
//      <prefix>_summary.txt  -- per-period summary stats
//      <prefix>_summary.csv  -- machine-readable summary
//
//  USAGE
//
//      g++ -std=c++17 -O2 -Wall -Wextra -Iinclude \
//          backtest/ustec_tf_htf_S35P6_backtest.cpp \
//          -o backtest/ustec_tf_htf_S35P6_backtest
//
//      backtest/ustec_tf_htf_S35P6_backtest \
//          --csv  /path/to/nsx_2025H1.csv \
//          --csv  /path/to/nsx_2025H2.csv \
//          --csv  /path/to/nsx_2026.csv \
//          --out-prefix /tmp/ustec_htf
//
//  BAR SYNTHESIS
//
//      The harness aggregates incoming ticks into M15 bars (15-minute UTC
//      windows). Each closed M15 bar is dispatched to the engine via
//      on_15m_bar. The engine internally synthesizes H1/H2/H4 from these
//      M15 bars. Per-tick on_tick() is called for SL/TP/BE/trail mgmt.
//
//  CAVEAT
//
//      USTEC.F has a daily session gap (22:00-23:00 UTC roughly, depending
//      on DST). The aggregator handles gaps gracefully -- it never emits a
//      bar that spans a gap by closing the in-progress bar at the last
//      pre-gap tick and starting fresh after the gap.
// =============================================================================

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "UstecTrendFollowHtfEngine.hpp"
#include "OmegaTradeLedger.hpp"

// -----------------------------------------------------------------------------
//  Cmd-line parsing
// -----------------------------------------------------------------------------
struct Args {
    std::vector<std::string> csvs;
    std::string out_prefix = "/tmp/ustec_htf";
    bool        verbose    = false;
};

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto need = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing arg for %s\n", s.c_str()); std::exit(2); }
            return argv[++i];
        };
        if      (s == "--csv")        a.csvs.push_back(need());
        else if (s == "--out-prefix") a.out_prefix = need();
        else if (s == "--verbose")    a.verbose = true;
        else { std::fprintf(stderr, "unknown arg: %s\n", s.c_str()); std::exit(2); }
    }
    if (a.csvs.empty()) {
        std::fprintf(stderr, "usage: %s --csv X.csv [--csv Y.csv ...] [--out-prefix P]\n", argv[0]);
        std::exit(2);
    }
    return a;
}

// -----------------------------------------------------------------------------
//  Tick stream + M15 bar aggregation
// -----------------------------------------------------------------------------
struct Tick { int64_t ts_ms; double bid; double ask; };

// Determine which period a UTC ts_ms belongs to: "2025H1" / "2025H2" / "2026" / "OTHER".
static const char* period_for_ms(int64_t ms) {
    time_t t = (time_t)(ms / 1000);
    struct tm utc{}; gmtime_r(&t, &utc);
    int year = utc.tm_year + 1900;
    int mon  = utc.tm_mon + 1;
    if (year == 2025 && mon >= 1 && mon <= 6)  return "2025H1";
    if (year == 2025 && mon >= 7 && mon <= 12) return "2025H2";
    if (year == 2026)                          return "2026";
    return "OTHER";
}

// -----------------------------------------------------------------------------
//  Driver
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    // Open output files.
    std::string trades_path  = args.out_prefix + "_trades.csv";
    std::string summary_txt  = args.out_prefix + "_summary.txt";
    std::string summary_csv  = args.out_prefix + "_summary.csv";

    std::ofstream trades_out(trades_path);
    if (!trades_out) { std::fprintf(stderr, "cannot open %s\n", trades_path.c_str()); return 1; }
    trades_out << "period,entry_ts,exit_ts,engine,side,entry,exit,sl,tp,pts_pnl,"
                  "usd_pnl,mfe_pts,mae_pts,exit_reason,regime\n";

    // Per-period stats
    struct PeriodStats {
        int64_t n = 0, n_win = 0, n_loss = 0;
        double  gross_usd = 0.0, win_usd = 0.0, loss_usd = 0.0;
        std::map<std::string, int> exit_reason_counts;
        std::map<std::string, int> per_cell_counts;
        std::map<std::string, double> per_cell_pnl;
    };
    std::map<std::string, PeriodStats> stats;

    // Build engine. Use the same TUNED defaults the operator will eventually
    // set in engine_init.hpp (per the design call). BE+trail+ATR floor;
    // killswitch/daily-cap disabled.
    omega::UstecTrendFollowHtfEngine eng;
    eng.shadow_mode      = true;
    eng.enabled          = true;
    eng.lot              = 0.1;
    eng.max_spread       = 5.0;
    eng.be_trigger_atr   = 1.0;
    eng.be_cost_buffer_pts = 0.50;
    eng.trail_after_be   = true;
    eng.trail_atr_mult   = 0.75;
    eng.min_atr_floor    = 5.0;     // M15 ATR floor (raw points)
    eng.daily_loss_limit = 0.0;
    eng.max_consec_losses = 0;
    eng.init();

    // Trade-capture callback. We tag the trade with the period of its entry.
    // Since the engine doesn't know about periods, we capture the period at
    // dispatch time and stamp it into the next trade we see.
    std::string current_period = "UNKNOWN";
    auto on_close = [&](const omega::TradeRecord& tr) {
        const std::string period = current_period;
        const double pts_pnl = tr.pnl;     // raw pts*lot
        const double usd_pnl = pts_pnl * 20.0;   // USTEC.F multiplier
        auto& s = stats[period];
        ++s.n;
        s.gross_usd += usd_pnl;
        if (usd_pnl > 0)      { ++s.n_win;  s.win_usd  += usd_pnl; }
        else if (usd_pnl < 0) { ++s.n_loss; s.loss_usd += usd_pnl; }
        s.exit_reason_counts[tr.exitReason]++;
        s.per_cell_counts[tr.engine]++;
        s.per_cell_pnl[tr.engine] += usd_pnl;
        trades_out << period << ',' << tr.entryTs << ',' << tr.exitTs << ','
                   << tr.engine << ',' << tr.side << ','
                   << tr.entryPrice << ',' << tr.exitPrice << ','
                   << tr.sl << ',' << tr.tp << ','
                   << pts_pnl << ',' << usd_pnl << ','
                   << tr.mfe << ',' << tr.mae << ','
                   << tr.exitReason << ',' << tr.regime << '\n';
    };

    // M15 bar aggregator. 15-min UTC windows = ts_ms / 900000 * 900000.
    int64_t cur_bar_start = 0;
    omega::UstecTfHtfBar cur_bar{};
    bool   cur_open = false;
    int64_t bars_emitted = 0;

    auto close_and_dispatch_bar = [&](int64_t now_ms, double bid, double ask) {
        if (!cur_open) return;
        eng.on_15m_bar(cur_bar, bid, ask, /*atr15m_external=*/0.0,
                       now_ms, on_close);
        ++bars_emitted;
        cur_open = false;
    };

    // Main tick loop. Process all CSVs sequentially.
    int64_t total_ticks_read = 0;
    char line[256];
    for (const auto& csv : args.csvs) {
        std::fprintf(stderr, "[BT] reading %s ...\n", csv.c_str());
        FILE* f = std::fopen(csv.c_str(), "r");
        if (!f) { std::fprintf(stderr, "  ERROR cannot open %s\n", csv.c_str()); continue; }
        // Skip header
        if (!std::fgets(line, sizeof(line), f)) { std::fclose(f); continue; }
        int64_t ticks_in_file = 0;
        while (std::fgets(line, sizeof(line), f)) {
            int64_t ts_ms; double bid, ask;
            if (std::sscanf(line, "%" SCNd64 ",%lf,%lf", &ts_ms, &bid, &ask) != 3) continue;
            ++ticks_in_file; ++total_ticks_read;

            current_period = period_for_ms(ts_ms);
            const double mid = (bid + ask) * 0.5;
            const int64_t bar_start = (ts_ms / 900000LL) * 900000LL;

            if (!cur_open) {
                cur_bar = omega::UstecTfHtfBar{bar_start, mid, mid, mid, mid};
                cur_bar_start = bar_start;
                cur_open = true;
            } else if (bar_start != cur_bar_start) {
                // Bar change -- dispatch the closed bar.
                close_and_dispatch_bar(ts_ms, bid, ask);
                cur_bar = omega::UstecTfHtfBar{bar_start, mid, mid, mid, mid};
                cur_bar_start = bar_start;
                cur_open = true;
            } else {
                if (mid > cur_bar.high) cur_bar.high = mid;
                if (mid < cur_bar.low ) cur_bar.low  = mid;
                cur_bar.close = mid;
            }

            // Per-tick on_tick for SL/TP/BE/trail management.
            eng.on_tick(bid, ask, ts_ms, on_close);
        }
        std::fclose(f);
        std::fprintf(stderr, "[BT] %s: %" PRId64 " ticks\n", csv.c_str(), ticks_in_file);
    }
    // Flush the final bar.
    if (cur_open) close_and_dispatch_bar(0, 0, 0);

    std::fprintf(stderr, "[BT] total ticks: %" PRId64 "  M15 bars dispatched: %" PRId64 "\n",
        total_ticks_read, bars_emitted);

    // -----------------------------------------------------------------------
    // Per-period summary
    // -----------------------------------------------------------------------
    std::ofstream sum_txt(summary_txt);
    std::ofstream sum_csv(summary_csv);
    sum_csv << "period,n,n_win,n_loss,wr,gross_usd,win_usd,loss_usd,profit_factor\n";

    auto emit_period = [&](const std::string& period, const PeriodStats& s) {
        const double wr = s.n > 0 ? 100.0 * s.n_win / s.n : 0.0;
        const double pf = (s.loss_usd != 0.0)
            ? (-s.win_usd / s.loss_usd) : (s.win_usd > 0 ? 999.99 : 0.0);
        sum_txt << "================================================================\n";
        sum_txt << "  PERIOD " << period << "\n";
        sum_txt << "================================================================\n";
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "  n=%-5lld  win=%-4lld  loss=%-4lld  wr=%5.1f%%  net=%+10.2f  "
            "PF=%.2f\n", (long long)s.n, (long long)s.n_win,
            (long long)s.n_loss, wr, s.gross_usd, pf);
        sum_txt << buf;
        sum_txt << "  Per-cell:\n";
        for (const auto& [cell, n] : s.per_cell_counts) {
            std::snprintf(buf, sizeof(buf),
                "    %-40s  n=%-4d  net=%+10.2f\n",
                cell.c_str(), n, s.per_cell_pnl.at(cell));
            sum_txt << buf;
        }
        sum_txt << "  Exit reasons:\n";
        for (const auto& [r, n] : s.exit_reason_counts) {
            std::snprintf(buf, sizeof(buf), "    %-15s  %d\n", r.c_str(), n);
            sum_txt << buf;
        }
        sum_txt << "\n";

        sum_csv << period << ',' << s.n << ',' << s.n_win << ',' << s.n_loss
                << ',' << wr << ',' << s.gross_usd << ',' << s.win_usd
                << ',' << s.loss_usd << ',' << pf << '\n';

        std::printf("[%s] n=%lld win=%lld loss=%lld wr=%.1f%% net=%+.2f PF=%.2f\n",
            period.c_str(), (long long)s.n, (long long)s.n_win,
            (long long)s.n_loss, wr, s.gross_usd, pf);
    };

    PeriodStats total{};
    for (const std::string& p : {std::string("2025H1"), std::string("2025H2"),
                                  std::string("2026"),   std::string("OTHER"),
                                  std::string("UNKNOWN")}) {
        auto it = stats.find(p);
        if (it != stats.end() && it->second.n > 0) {
            emit_period(p, it->second);
            total.n += it->second.n;
            total.n_win += it->second.n_win;
            total.n_loss += it->second.n_loss;
            total.gross_usd += it->second.gross_usd;
            total.win_usd += it->second.win_usd;
            total.loss_usd += it->second.loss_usd;
            for (auto& [k, v] : it->second.per_cell_counts) total.per_cell_counts[k] += v;
            for (auto& [k, v] : it->second.per_cell_pnl)    total.per_cell_pnl[k] += v;
            for (auto& [k, v] : it->second.exit_reason_counts) total.exit_reason_counts[k] += v;
        }
    }
    emit_period("ALL_PERIODS", total);

    sum_txt.close(); sum_csv.close(); trades_out.close();
    std::fprintf(stderr, "[BT] outputs:\n  %s\n  %s\n  %s\n",
        trades_path.c_str(), summary_txt.c_str(), summary_csv.c_str());
    return 0;
}

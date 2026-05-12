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
//      <prefix>_summary.txt  -- per-period summary stats + guard histogram
//      <prefix>_summary.csv  -- machine-readable summary
//      <prefix>_guards.csv   -- machine-readable per-period guard hits  [S36-P2]
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
//      Cell A/B (S36-P2 follow-up): drop AtrMom1h from the ledger and
//      stats so the harness reports Stoch4h-only behaviour:
//
//          backtest/ustec_tf_htf_S35P6_backtest --disable-cell AtrMom1h ...
//
//      --disable-cell can be passed multiple times. The engine still
//      EVALUATES the disabled cells (no engine-side modification), but
//      any trade those cells close is silently dropped from the ledger,
//      per-cell counts, and PnL totals -- mathematically equivalent to
//      that cell not existing, because each cell holds an independent
//      pos[] slot with no capital-sharing or cross-cell interference in
//      this engine.
//
//      Cross-symbol portability (S36-P3): three knob overrides let the
//      same engine binary run on non-USTEC tick data without engine code
//      changes:
//
//          --usd-per-pt     <float>   default 20.0   (USTEC.F multiplier)
//          --min-atr-floor  <float>   default 5.0    (raw points, M15 ATR)
//          --max-spread     <float>   default 5.0    (raw points)
//
//      SPX example (cash CFD, ~$6000 level, $1/pt at retail):
//
//          backtest/ustec_tf_htf_S35P6_backtest \
//              --disable-cell AtrMom1h \
//              --usd-per-pt    1.0 \
//              --min-atr-floor 1.2 \
//              --max-spread    1.0 \
//              --csv /tmp/.../spx_2025H1.csv \
//              --csv /tmp/.../spx_2025H2.csv \
//              --csv /tmp/.../spx_2026.csv \
//              --out-prefix /tmp/s36_p3_spx_stoch_insample
//
//      The engine's static SYMBOL constant ("USTEC.F") and the per-cell
//      short_name strings (e.g. "UstecTrendFollowHtf_Stoch4h") still
//      appear in the trade ledger labels even when running on SPX --
//      that is cosmetic only and does not affect any math. Trades are
//      tagged by *cell*, not by symbol, so cell-level results remain
//      meaningful regardless of which symbol the harness is feeding.
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
//
//  S36-P2 / P3 INSTRUMENTATION (2026-05-12)
//
//      P2 (2024 OOS holdout on NSXUSD): see commit history.
//          Verdict: 2-cell engine fails OOS (-$2677); AtrMom1h cell
//          responsible for 90% of the loss; Stoch4h-only ships
//          (in-sample +$6666 PF 1.37; 2024 OOS -$270 PF 0.97 -- flat).
//
//      P3 (cross-symbol robustness): the three knob-override flags
//          above let us run Stoch4h-only on SPX in-sample + 2024 SPX
//          OOS to test whether the edge generalises beyond USTEC. If it
//          does, the strategy is more robust than a USTEC-specific
//          calibration would suggest. If it dies on SPX, the in-sample
//          NSXUSD edge is suspect.
//
//      After each closed M15 bar the harness still calls
//      eng.guards.check_entry_ok(...) and tallies bar-disposition by
//      reason; this remains useful for sanity-checking future runs.
// =============================================================================

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
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
    std::set<std::string> disabled_cells;        // [S36-P2] short_name to drop
    // [S36-P3] Cross-symbol knob overrides. Defaults preserve USTEC behaviour
    // (lot=0.1 stays a fixed engine knob; the three values below were
    // previously hard-coded in the engine setup block).
    double usd_per_pt    = 20.0;
    double min_atr_floor = 5.0;
    double max_spread    = 5.0;
};

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto need = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing arg for %s\n", s.c_str()); std::exit(2); }
            return argv[++i];
        };
        if      (s == "--csv")            a.csvs.push_back(need());
        else if (s == "--out-prefix")     a.out_prefix = need();
        else if (s == "--verbose")        a.verbose = true;
        else if (s == "--disable-cell")   a.disabled_cells.insert(need());   // [S36-P2]
        else if (s == "--usd-per-pt")     a.usd_per_pt    = std::atof(need().c_str());  // [S36-P3]
        else if (s == "--min-atr-floor")  a.min_atr_floor = std::atof(need().c_str());  // [S36-P3]
        else if (s == "--max-spread")     a.max_spread    = std::atof(need().c_str());  // [S36-P3]
        else { std::fprintf(stderr, "unknown arg: %s\n", s.c_str()); std::exit(2); }
    }
    if (a.csvs.empty()) {
        std::fprintf(stderr,
            "usage: %s --csv X.csv [--csv Y.csv ...] [--out-prefix P]\n"
            "       [--disable-cell SHORT_NAME ...]\n"
            "       [--usd-per-pt F] [--min-atr-floor F] [--max-spread F]\n",
            argv[0]);
        std::exit(2);
    }
    return a;
}

// -----------------------------------------------------------------------------
//  Tick stream + M15 bar aggregation
// -----------------------------------------------------------------------------
struct Tick { int64_t ts_ms; double bid; double ask; };

// Determine which period a UTC ts_ms belongs to.
// Periods: "2024H1" / "2024H2" / "2025H1" / "2025H2" / "2026" / "OTHER".
//   [S36-P2 2026-05-12]: added 2024H1 / 2024H2 for 2024 OOS holdout test.
static const char* period_for_ms(int64_t ms) {
    time_t t = (time_t)(ms / 1000);
    struct tm utc{}; gmtime_r(&t, &utc);
    int year = utc.tm_year + 1900;
    int mon  = utc.tm_mon + 1;
    if (year == 2024 && mon >= 1 && mon <= 6)  return "2024H1";
    if (year == 2024 && mon >= 7 && mon <= 12) return "2024H2";
    if (year == 2025 && mon >= 1 && mon <= 6)  return "2025H1";
    if (year == 2025 && mon >= 7 && mon <= 12) return "2025H2";
    if (year == 2026)                          return "2026";
    return "OTHER";
}

// [S36-P2 2026-05-12] Bar-disposition counters used by the in-harness guard
// histogram. A bar is "OK" when guards.check_entry_ok() returns nullptr
// (entry would be allowed), or tallied by reason string otherwise.
struct GuardCounters {
    int64_t bars_total = 0;
    int64_t bars_ok    = 0;
    std::map<std::string, int64_t> reason_hits;   // "ATR_BELOW_FLOOR" -> n, etc.

    void tally(const char* reason) {
        ++bars_total;
        if (reason == nullptr) ++bars_ok;
        else                   ++reason_hits[std::string(reason)];
    }

    void merge_into(GuardCounters& other) const {
        other.bars_total += bars_total;
        other.bars_ok    += bars_ok;
        for (const auto& [k, v] : reason_hits) other.reason_hits[k] += v;
    }
};

// -----------------------------------------------------------------------------
//  Driver
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    // [S36-P2] Echo the active disable list so it shows up in run logs.
    if (!args.disabled_cells.empty()) {
        std::fprintf(stderr, "[BT] disabled cells (trades dropped from ledger): ");
        bool first = true;
        for (const auto& c : args.disabled_cells) {
            std::fprintf(stderr, "%s%s", first ? "" : ",", c.c_str());
            first = false;
        }
        std::fprintf(stderr, "\n");
    }
    // [S36-P3] Echo the per-symbol knob overrides so it's obvious in logs
    // which calibration was used. Defaults match USTEC; SPX example would
    // print "usd_per_pt=1.0 min_atr_floor=1.2 max_spread=1.0".
    std::fprintf(stderr,
        "[BT] knobs: usd_per_pt=%.4f  min_atr_floor=%.4f  max_spread=%.4f\n",
        args.usd_per_pt, args.min_atr_floor, args.max_spread);

    // Open output files.
    std::string trades_path  = args.out_prefix + "_trades.csv";
    std::string summary_txt  = args.out_prefix + "_summary.txt";
    std::string summary_csv  = args.out_prefix + "_summary.csv";
    std::string guards_csv   = args.out_prefix + "_guards.csv";    // [S36-P2]

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
        GuardCounters guards;                                       // [S36-P2]
        int64_t n_dropped = 0;                                      // [S36-P2]
        double  dropped_usd = 0.0;                                  // [S36-P2]
    };
    std::map<std::string, PeriodStats> stats;

    // Build engine. Use the same TUNED defaults the operator will eventually
    // set in engine_init.hpp (per the design call). BE+trail+ATR floor;
    // killswitch/daily-cap disabled. [S36-P3] max_spread, min_atr_floor now
    // come from CLI args -- defaults preserve USTEC behaviour.
    omega::UstecTrendFollowHtfEngine eng;
    eng.shadow_mode      = true;
    eng.enabled          = true;
    eng.lot              = 0.1;
    eng.max_spread       = args.max_spread;        // [S36-P3] was 5.0 hardcoded
    eng.be_trigger_atr   = 1.0;
    eng.be_cost_buffer_pts = 0.50;
    eng.trail_after_be   = true;
    eng.trail_atr_mult   = 0.75;
    eng.min_atr_floor    = args.min_atr_floor;     // [S36-P3] was 5.0 hardcoded
    eng.daily_loss_limit = 0.0;
    eng.max_consec_losses = 0;
    eng.init();

    // Trade-capture callback. We tag the trade with the period of its entry.
    // Since the engine doesn't know about periods, we capture the period at
    // dispatch time and stamp it into the next trade we see.
    //
    // [S36-P2] If tr.engine matches a cell in args.disabled_cells, the trade
    // is dropped from the ledger / per-cell stats / PnL totals (still counted
    // separately in n_dropped + dropped_usd so we can confirm the engine
    // really did evaluate that cell). Per the cell short_name set in
    // kUstecTfHtfCells[], tr.engine is e.g. "UstecTrendFollowHtf_AtrMom1h"
    // -- we match on the suffix after the last underscore.
    auto trade_cell_short_name = [](const std::string& engine_tag) -> std::string {
        auto pos = engine_tag.find_last_of('_');
        if (pos == std::string::npos) return engine_tag;
        return engine_tag.substr(pos + 1);
    };

    std::string current_period = "UNKNOWN";
    auto on_close = [&](const omega::TradeRecord& tr) {
        const std::string period = current_period;
        const double pts_pnl = tr.pnl;     // raw pts*lot
        // [S36-P3] usd_per_pt was hard-coded 20.0 (USTEC). Now from CLI.
        const double usd_pnl = pts_pnl * args.usd_per_pt;
        auto& s = stats[period];

        // [S36-P2] Cell-disable filter. Track dropped trades separately so
        // we can verify the engine evaluated them (sanity check), but
        // exclude them from all real stats.
        const std::string cell = trade_cell_short_name(tr.engine);
        if (args.disabled_cells.count(cell)) {
            ++s.n_dropped;
            s.dropped_usd += usd_pnl;
            return;
        }

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

        // [S36-P2 2026-05-12] Post-bar guard probe. atr14_m15_ has now been
        // updated by the just-closed bar; check_entry_ok() answers "would a
        // fresh entry attempt at this instant pass the gates?". Tally into
        // the period of this bar (current_period was set by the tick that
        // triggered the bar close).
        const char* why = eng.guards.check_entry_ok(
            bid, ask, eng.atr14_m15_, now_ms / 1000);
        stats[current_period].guards.tally(why);

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
    std::ofstream grd_csv(guards_csv);                              // [S36-P2]
    sum_csv << "period,n,n_win,n_loss,wr,gross_usd,win_usd,loss_usd,profit_factor\n";
    grd_csv << "period,bars_total,bars_ok,ATR_BELOW_FLOOR,ATR_ABOVE_CEIL,"
               "SPREAD_CAP,DAILY_LOSS_CAP,KILLSWITCH,SESSION_BLOCK,OTHER\n";

    // [S36-P2 2026-05-12] Helper: emit one period's guard breakdown to
    // summary.txt (human-readable block) and to <prefix>_guards.csv (one
    // row per period). The OTHER bucket captures any reason string we
    // haven't pre-listed (defensive in case the guard set grows).
    auto emit_guards = [&](const std::string& period, const GuardCounters& g,
                           std::ostream& txt) {
        char buf[256];
        const int64_t ok = g.bars_ok;
        const double  ok_pct = g.bars_total > 0
            ? 100.0 * ok / g.bars_total : 0.0;
        std::snprintf(buf, sizeof(buf),
            "  Guards: bars=%" PRId64 "  ok=%" PRId64 " (%.1f%%)\n",
            g.bars_total, ok, ok_pct);
        txt << buf;
        const char* tracked[] = {
            "ATR_BELOW_FLOOR", "ATR_ABOVE_CEIL", "SPREAD_CAP",
            "DAILY_LOSS_CAP", "KILLSWITCH", "SESSION_BLOCK"
        };
        int64_t hits[6] = {0,0,0,0,0,0};
        int64_t other_hits = 0;
        for (const auto& [reason, n] : g.reason_hits) {
            bool matched = false;
            for (int i = 0; i < 6; ++i) {
                if (reason == tracked[i]) { hits[i] = n; matched = true; break; }
            }
            if (!matched) other_hits += n;
        }
        for (int i = 0; i < 6; ++i) {
            if (hits[i] > 0) {
                std::snprintf(buf, sizeof(buf),
                    "    %-20s %" PRId64 "\n", tracked[i], hits[i]);
                txt << buf;
            }
        }
        if (other_hits > 0) {
            std::snprintf(buf, sizeof(buf),
                "    %-20s %" PRId64 "\n", "OTHER", other_hits);
            txt << buf;
        }
        grd_csv << period << ',' << g.bars_total << ',' << g.bars_ok;
        for (int i = 0; i < 6; ++i) grd_csv << ',' << hits[i];
        grd_csv << ',' << other_hits << '\n';
    };

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
        // [S36-P2] Show dropped-trade tally so user can verify the engine
        // actually evaluated the disabled cell(s).
        if (s.n_dropped > 0) {
            std::snprintf(buf, sizeof(buf),
                "  (dropped from disabled cells: n=%lld  net_excluded=%+.2f)\n",
                (long long)s.n_dropped, s.dropped_usd);
            sum_txt << buf;
        }
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
        emit_guards(period, s.guards, sum_txt);                     // [S36-P2]
        sum_txt << "\n";

        sum_csv << period << ',' << s.n << ',' << s.n_win << ',' << s.n_loss
                << ',' << wr << ',' << s.gross_usd << ',' << s.win_usd
                << ',' << s.loss_usd << ',' << pf << '\n';

        std::printf("[%s] n=%lld win=%lld loss=%lld wr=%.1f%% net=%+.2f PF=%.2f "
                    "bars=%lld ok=%lld dropped=%lld\n",
            period.c_str(), (long long)s.n, (long long)s.n_win,
            (long long)s.n_loss, wr, s.gross_usd, pf,
            (long long)s.guards.bars_total, (long long)s.guards.bars_ok,
            (long long)s.n_dropped);
    };

    PeriodStats total{};
    // [S36-P2 2026-05-12] Added "2024H1" and "2024H2" to the iteration list.
    // Previously 2024 trades were summed into stats[] by on_close() but never
    // emitted to summary.txt and never rolled into ALL_PERIODS -- a latent
    // bug that turned the 2024 OOS run into a silent n=0 result regardless
    // of whether trades actually fired.
    for (const std::string& p : {std::string("2024H1"), std::string("2024H2"),
                                  std::string("2025H1"), std::string("2025H2"),
                                  std::string("2026"),   std::string("OTHER"),
                                  std::string("UNKNOWN")}) {
        auto it = stats.find(p);
        if (it != stats.end() && (it->second.n > 0 || it->second.guards.bars_total > 0)) {
            emit_period(p, it->second);
            total.n += it->second.n;
            total.n_win += it->second.n_win;
            total.n_loss += it->second.n_loss;
            total.gross_usd += it->second.gross_usd;
            total.win_usd += it->second.win_usd;
            total.loss_usd += it->second.loss_usd;
            total.n_dropped += it->second.n_dropped;
            total.dropped_usd += it->second.dropped_usd;
            for (auto& [k, v] : it->second.per_cell_counts) total.per_cell_counts[k] += v;
            for (auto& [k, v] : it->second.per_cell_pnl)    total.per_cell_pnl[k] += v;
            for (auto& [k, v] : it->second.exit_reason_counts) total.exit_reason_counts[k] += v;
            it->second.guards.merge_into(total.guards);             // [S36-P2]
        }
    }
    emit_period("ALL_PERIODS", total);

    sum_txt.close(); sum_csv.close(); grd_csv.close(); trades_out.close();
    std::fprintf(stderr, "[BT] outputs:\n  %s\n  %s\n  %s\n  %s\n",
        trades_path.c_str(), summary_txt.c_str(), summary_csv.c_str(),
        guards_csv.c_str());
    return 0;
}

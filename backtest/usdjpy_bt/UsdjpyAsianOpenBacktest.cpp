// =============================================================================
// UsdjpyAsianOpenBacktest.cpp
// -----------------------------------------------------------------------------
// Standalone tick-replay backtest for include/UsdjpyAsianOpenEngine.hpp.
//
// 2026-05-02 SESSION (Claude / Jo): Sister harness to the EURUSD S55/S56
//   sweep work. Reads HistData.com ASCII USDJPY tick CSVs, streams ticks
//   chronologically through a single UsdjpyAsianOpenEngine instance, and
//   writes per-trade and per-month CSV outputs the post-sweep analysis
//   scripts can consume. Single-config-per-binary; the parameter sweep
//   wrapper (scripts/usdjpy_asian_sweep.py) drives multi-config exploration
//   by sed-rewriting the engine header into a tmp dir and recompiling.
//
// BUILD (from omega_repo root):
//   g++ -std=c++17 -O3 -Wno-unused-parameter -DOMEGA_BACKTEST \
//       -I backtest/usdjpy_bt -I include \
//       backtest/usdjpy_bt/UsdjpyAsianOpenBacktest.cpp \
//       -o build/usdjpy_asian_bt -pthread
//
// RUN:
//   ./build/usdjpy_asian_bt --ticks ~/Tick/USDJPY \
//       --from 2025-03 --to 2026-04 \
//       --trades bt_usdjpy_trades.csv \
//       --report bt_usdjpy_report.csv
//
// Tick CSV format (HistData.com ASCII):
//   YYYYMMDD HHMMSSmmm,bid,ask,vol
// Tick value at 0.10 lot: $0.667 at JPY=150 (live conversion via
// 100000.0/g_usdjpy_mid in the production tick_value_multiplier; this
// harness uses the same convention with mid sampled per-tick).
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <map>
#include <unordered_map>
#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;

// -- Stubs needed by the production engine header (engine references symbols
//    from its includes that don't exist in a standalone build). --------------
#include "OmegaTradeLedger.hpp"
#include "SpreadRegimeGate.hpp"
#include "OmegaNewsBlackout.hpp"

// 'bracket_on_close' is referenced indirectly by the engine through the
// CloseCallback signature; the engine itself never calls it. Provided here
// as a no-op so the engine compiles with no missing symbols.
inline void bracket_on_close(const omega::TradeRecord&) {}

// -- Engine: a backtest-side copy of include/UsdjpyAsianOpenEngine.hpp.
//    Resolves to backtest/usdjpy_bt/UsdjpyAsianOpenEngine.hpp via the -I
//    backtest/usdjpy_bt path. Production engine in include/ stays untouched.
//    The sweep wrapper (scripts/usdjpy_asian_sweep.py) regenerates this
//    copy with sed-substituted parameter values per run.
#include "UsdjpyAsianOpenEngine.hpp"

// -- Engine instance + close-callback wiring --------------------------------
static omega::UsdjpyAsianOpenEngine g_eng;

// Global mutable state for cost accounting / live mid tracking
static std::atomic<double> g_usdjpy_live_mid{150.0};

// -- HistData ASCII parser --------------------------------------------------
struct Tick {
    int64_t ts_ms;   // unix milliseconds
    double  bid;
    double  ask;
};

// Convert "YYYYMMDD HHMMSSmmm" to unix ms (UTC).
// HistData publishes EST timestamps; we treat them as UTC for parity with
// the EURUSD S55/S56 work that ran on the same vendor data. The 06:00-09:00
// (EURUSD) and 00:00-04:00 (this engine, USDJPY) UTC session windows have
// equivalent EST-as-UTC interpretations.
static int64_t parse_histdata_ts(const char* p, size_t len) noexcept {
    if (len < 18) return 0;
    int Y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
    int M = (p[4]-'0')*10 + (p[5]-'0');
    int D = (p[6]-'0')*10 + (p[7]-'0');
    int h = (p[9]-'0')*10 + (p[10]-'0');
    int mn = (p[11]-'0')*10 + (p[12]-'0');
    int s  = (p[13]-'0')*10 + (p[14]-'0');
    int ms = (p[15]-'0')*100 + (p[16]-'0')*10 + (p[17]-'0');

    struct tm t{};
    t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D;
    t.tm_hour = h; t.tm_min = mn; t.tm_sec = s;
    time_t tt = timegm(&t);
    return (int64_t)tt * 1000LL + ms;
}

static bool parse_tick_line(const char* line, size_t len, Tick& out) noexcept {
    // Format: YYYYMMDD HHMMSSmmm,bid,ask,vol
    if (len < 25) return false;
    int64_t ts = parse_histdata_ts(line, 18);
    if (ts == 0) return false;
    const char* p = line + 18;
    if (*p != ',') return false;
    ++p;
    char* endp = nullptr;
    double bid = std::strtod(p, &endp);
    if (!endp || *endp != ',') return false;
    p = endp + 1;
    double ask = std::strtod(p, &endp);
    if (!endp) return false;
    if (bid <= 0.0 || ask <= 0.0 || ask < bid) return false;
    out.ts_ms = ts;
    out.bid = bid;
    out.ask = ask;
    return true;
}

// -- Runtime stats accumulator ----------------------------------------------
struct RunStats {
    int64_t  total_ticks   = 0;
    int      trades        = 0;
    int      wins          = 0;
    int      losses        = 0;
    int      breakevens    = 0;
    int      tp_hit        = 0;
    int      sl_hit        = 0;
    int      trail_hit     = 0;
    int      be_hit        = 0;
    double   gross_win_usd = 0.0;
    double   gross_loss_usd= 0.0;  // negative
    double   max_dd_usd    = 0.0;
    double   peak_equity   = 0.0;
    double   equity        = 0.0;
    std::map<std::string,double> pnl_by_month;   // YYYY-MM -> usd
    std::map<std::string,int>    trades_by_month;
    std::vector<std::pair<int64_t,double>> equity_curve;  // (ts, equity)
};

static RunStats g_stats;

// Convert raw price-distance * size pnl into USD using live mid.
// Mirrors production tick_value_multiplier("USDJPY") = 100000/g_usdjpy_mid.
static double convert_to_usd(double pnl_raw, double live_mid) noexcept {
    if (live_mid <= 0.0) live_mid = 150.0;
    const double tick_mult = 100000.0 / live_mid;
    return pnl_raw * tick_mult;
}

// Cost model (matches the EURUSD harness implicit assumption):
// Spread is already paid at entry/exit since the engine uses ask for long
// entries and bid for long exits (and vice versa). We add a fixed per-side
// slippage of 0.2 pips (0.002 in JPY price units) and a $0.20/lot per-side
// commission to match the BlackBull cost structure.
static double apply_costs_usd(double pnl_usd, double size, bool /*is_long*/) noexcept {
    const double slip_pips_per_side = 0.2;
    const double slip_jpy_price     = slip_pips_per_side * 0.01;
    const double live_mid = g_usdjpy_live_mid.load(std::memory_order_relaxed);
    const double tick_mult = (live_mid > 0.0) ? (100000.0 / live_mid) : 667.0;
    const double slip_usd = slip_jpy_price * size * tick_mult * 2.0; // both sides
    const double comm_usd = 0.20 * size * 2.0; // per-side per-lot
    return pnl_usd - slip_usd - comm_usd;
}

static std::ofstream g_trades_csv;

static void on_trade_close(const omega::TradeRecord& tr) {
    const double live_mid = g_usdjpy_live_mid.load(std::memory_order_relaxed);
    const double pnl_gross_usd = convert_to_usd(tr.pnl, live_mid);
    const double pnl_net_usd   = apply_costs_usd(pnl_gross_usd, tr.size, tr.side == "LONG");

    g_stats.trades++;
    g_stats.equity += pnl_net_usd;
    if (g_stats.equity > g_stats.peak_equity) g_stats.peak_equity = g_stats.equity;
    const double dd = g_stats.peak_equity - g_stats.equity;
    if (dd > g_stats.max_dd_usd) g_stats.max_dd_usd = dd;

    if      (tr.exitReason == "TP_HIT")    g_stats.tp_hit++;
    else if (tr.exitReason == "SL_HIT")    g_stats.sl_hit++;
    else if (tr.exitReason == "TRAIL_HIT") g_stats.trail_hit++;
    else if (tr.exitReason == "BE_HIT")    g_stats.be_hit++;

    if (pnl_net_usd > 0.5)       { g_stats.wins++;   g_stats.gross_win_usd  += pnl_net_usd; }
    else if (pnl_net_usd < -0.5) { g_stats.losses++; g_stats.gross_loss_usd += pnl_net_usd; }
    else                         { g_stats.breakevens++; }

    // Per-month bucket (use exitTs UTC year-month)
    time_t tt = (time_t)tr.exitTs;
    struct tm gm{};
    gmtime_r(&tt, &gm);
    char ym[16]; snprintf(ym, sizeof(ym), "%04d-%02d", gm.tm_year + 1900, gm.tm_mon + 1);
    g_stats.pnl_by_month[ym] += pnl_net_usd;
    g_stats.trades_by_month[ym]++;

    g_stats.equity_curve.emplace_back(tr.exitTs, g_stats.equity);

    if (g_trades_csv.is_open()) {
        g_trades_csv
            << tr.id << ',' << tr.symbol << ',' << tr.side << ',' << tr.engine << ','
            << tr.regime << ','
            << tr.entryTs << ',' << tr.exitTs << ','
            << tr.entryPrice << ',' << tr.exitPrice << ','
            << tr.tp << ',' << tr.sl << ',' << tr.size << ','
            << tr.pnl << ',' << pnl_gross_usd << ',' << pnl_net_usd << ','
            << tr.mfe << ',' << tr.mae << ',' << tr.exitReason << ','
            << tr.spreadAtEntry << ',' << tr.bracket_hi << ',' << tr.bracket_lo
            << '\n';
    }
}

// -- Drive a single CSV through the engine ----------------------------------
static int64_t run_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "[ERR] cannot open %s\n", path.c_str());
        return 0;
    }

    // 4 MiB read buffer
    static thread_local std::vector<char> buf(4 * 1024 * 1024);
    std::string line;
    line.reserve(64);
    int64_t ticks_in_file = 0;
    size_t n;
    bool eof = false;

    while (!eof) {
        n = std::fread(buf.data(), 1, buf.size(), f);
        if (n == 0) break;
        if (n < buf.size()) eof = true;

        size_t p = 0;
        while (p < n) {
            size_t lineStart = p;
            while (p < n && buf[p] != '\n') ++p;
            if (p < n) {
                size_t lineLen = p - lineStart;
                if (lineLen > 0 && buf[lineStart + lineLen - 1] == '\r') --lineLen;

                Tick t{};
                if (parse_tick_line(buf.data() + lineStart, lineLen, t)) {
                    g_usdjpy_live_mid.store((t.bid + t.ask) * 0.5, std::memory_order_relaxed);
                    g_eng.on_tick(t.bid, t.ask, t.ts_ms,
                                  /*can_enter*/ true,
                                  /*flow_live*/ false,
                                  /*flow_be_locked*/ false,
                                  /*flow_trail_stage*/ 0,
                                  on_trade_close,
                                  /*book_slope*/ 0.0,
                                  /*vacuum_ask*/ false,
                                  /*vacuum_bid*/ false,
                                  /*wall_above*/ false,
                                  /*wall_below*/ false,
                                  /*l2_real*/ false);
                    ++ticks_in_file;
                    ++g_stats.total_ticks;
                }
                ++p; // skip '\n'
            } else {
                // Carry incomplete line back via fseek
                if (!eof) std::fseek(f, -(long)(p - lineStart), SEEK_CUR);
                break;
            }
        }
    }

    std::fclose(f);
    return ticks_in_file;
}

// -- Locate monthly tick CSVs -----------------------------------------------
static std::vector<std::pair<std::string,std::string>>
collect_monthly_files(const std::string& root, const std::string& from, const std::string& to) {
    // root contains HISTDATA_COM_ASCII_USDJPY_T<YYYYMM>/ subdirs.
    // Returns vector of (yyyymm, path) sorted by yyyymm.
    std::vector<std::pair<std::string,std::string>> out;
    if (!fs::is_directory(root)) {
        std::fprintf(stderr, "[ERR] tick root %s is not a directory\n", root.c_str());
        return out;
    }
    auto from_yyyymm = (from.size() == 7)
        ? from.substr(0,4) + from.substr(5,2)
        : from;
    auto to_yyyymm = (to.size() == 7)
        ? to.substr(0,4) + to.substr(5,2)
        : to;

    for (auto& e : fs::directory_iterator(root)) {
        if (!e.is_directory()) continue;
        const std::string name = e.path().filename().string();
        // Expect name like HISTDATA_COM_ASCII_USDJPY_T202503 (or with trailing " (1)")
        const std::string prefix = "HISTDATA_COM_ASCII_USDJPY_T";
        if (name.find(prefix) != 0) continue;
        if (name.size() < prefix.size() + 6) continue;
        std::string yyyymm = name.substr(prefix.size(), 6);
        if (yyyymm < from_yyyymm || yyyymm > to_yyyymm) continue;
        // Skip ' (1)' duplicates
        if (name.find(" (1)") != std::string::npos) continue;

        // Find the ASCII tick CSV inside.
        for (auto& f : fs::directory_iterator(e.path())) {
            if (!f.is_regular_file()) continue;
            const std::string fname = f.path().filename().string();
            if (fname.find("DAT_ASCII_USDJPY_T_") == 0 &&
                fname.size() >= 4 &&
                fname.substr(fname.size() - 4) == ".csv") {
                out.emplace_back(yyyymm, f.path().string());
                break;
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// -- Main --------------------------------------------------------------------
int main(int argc, char** argv) {
    std::string ticks_root = "";
    std::string from_ym = "2025-03";
    std::string to_ym   = "2026-04";
    std::string trades_csv = "bt_usdjpy_trades.csv";
    std::string report_csv = "bt_usdjpy_report.csv";
    std::string label = "default";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--ticks"   && i+1 < argc) ticks_root = argv[++i];
        else if (a == "--from"    && i+1 < argc) from_ym    = argv[++i];
        else if (a == "--to"      && i+1 < argc) to_ym      = argv[++i];
        else if (a == "--trades"  && i+1 < argc) trades_csv = argv[++i];
        else if (a == "--report"  && i+1 < argc) report_csv = argv[++i];
        else if (a == "--label"   && i+1 < argc) label      = argv[++i];
        else {
            std::fprintf(stderr, "Unknown arg: %s\n", a.c_str());
            return 2;
        }
    }
    if (ticks_root.empty()) {
        std::fprintf(stderr, "Usage: %s --ticks <dir> [--from YYYY-MM] [--to YYYY-MM] "
                             "[--trades file] [--report file] [--label tag]\n", argv[0]);
        return 2;
    }

    auto files = collect_monthly_files(ticks_root, from_ym, to_ym);
    if (files.empty()) {
        std::fprintf(stderr, "[ERR] no monthly tick files found under %s for %s..%s\n",
                     ticks_root.c_str(), from_ym.c_str(), to_ym.c_str());
        return 2;
    }
    std::fprintf(stderr, "[BT] %zu monthly files to process (%s..%s)\n",
                 files.size(), files.front().first.c_str(), files.back().first.c_str());

    // Open trades CSV with header
    g_trades_csv.open(trades_csv, std::ios::out | std::ios::trunc);
    if (!g_trades_csv) {
        std::fprintf(stderr, "[ERR] cannot open %s for write\n", trades_csv.c_str());
        return 2;
    }
    g_trades_csv << "id,symbol,side,engine,regime,entryTs,exitTs,entryPrice,exitPrice,"
                    "tp,sl,size,pnl_raw,pnl_gross_usd,pnl_net_usd,mfe,mae,exitReason,"
                    "spreadAtEntry,bracket_hi,bracket_lo\n";

    auto wall_start = std::chrono::steady_clock::now();
    for (auto& f : files) {
        auto t0 = std::chrono::steady_clock::now();
        int64_t n = run_file(f.second);
        auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "[BT] %s : %lld ticks in %.1fs (%.0fk/s) | trades=%d equity=$%.2f\n",
                     f.first.c_str(), (long long)n, dt, n / 1e3 / std::max(dt, 1e-3),
                     g_stats.trades, g_stats.equity);
    }
    auto wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
    g_trades_csv.close();

    // Summary report
    const int decided = g_stats.wins + g_stats.losses;
    const double wr = decided > 0 ? (double)g_stats.wins / decided : 0.0;
    const double pf = (g_stats.gross_loss_usd != 0.0)
        ? (g_stats.gross_win_usd / std::fabs(g_stats.gross_loss_usd))
        : 0.0;
    const double avg_win = g_stats.wins > 0   ? (g_stats.gross_win_usd / g_stats.wins) : 0.0;
    const double avg_loss= g_stats.losses > 0 ? (g_stats.gross_loss_usd / g_stats.losses) : 0.0;
    int prof_months = 0;
    for (auto& kv : g_stats.pnl_by_month) if (kv.second > 0.0) ++prof_months;

    std::fprintf(stderr, "\n=========================  SUMMARY (%s)  =========================\n", label.c_str());
    std::fprintf(stderr, "Months processed    : %zu  (%s..%s)\n",
                 g_stats.pnl_by_month.size(),
                 g_stats.pnl_by_month.empty() ? "?" : g_stats.pnl_by_month.begin()->first.c_str(),
                 g_stats.pnl_by_month.empty() ? "?" : g_stats.pnl_by_month.rbegin()->first.c_str());
    std::fprintf(stderr, "Total ticks         : %lld\n", (long long)g_stats.total_ticks);
    std::fprintf(stderr, "Wall time           : %.1fs (%.0fk ticks/s)\n",
                 wall, g_stats.total_ticks / 1e3 / std::max(wall, 1e-3));
    std::fprintf(stderr, "Trades              : %d\n", g_stats.trades);
    std::fprintf(stderr, "  TP_HIT / TRAIL_HIT / SL_HIT / BE_HIT : %d / %d / %d / %d\n",
                 g_stats.tp_hit, g_stats.trail_hit, g_stats.sl_hit, g_stats.be_hit);
    std::fprintf(stderr, "Wins / Losses / BE  : %d / %d / %d  (decided WR = %.1f%%)\n",
                 g_stats.wins, g_stats.losses, g_stats.breakevens, wr * 100.0);
    std::fprintf(stderr, "Avg win / loss      : $%.2f / $%.2f\n", avg_win, avg_loss);
    std::fprintf(stderr, "Gross win / loss    : $%.2f / $%.2f\n",
                 g_stats.gross_win_usd, g_stats.gross_loss_usd);
    std::fprintf(stderr, "Net PnL             : $%.2f\n", g_stats.equity);
    std::fprintf(stderr, "Profit factor       : %.2f\n", pf);
    std::fprintf(stderr, "Max drawdown        : $%.2f\n", g_stats.max_dd_usd);
    std::fprintf(stderr, "Profitable months   : %d / %zu\n", prof_months, g_stats.pnl_by_month.size());
    std::fprintf(stderr, "==================================================================\n");

    // Per-month report CSV (append mode if exists -- caller decides)
    bool need_header = !fs::exists(report_csv);
    std::ofstream rep(report_csv, std::ios::app);
    if (need_header) {
        rep << "label,month,trades,wins,losses,breakevens,tp_hit,trail_hit,sl_hit,be_hit,"
               "gross_win_usd,gross_loss_usd,net_pnl_usd,profit_factor,wr,max_dd_usd,wall_s\n";
    }
    rep << label << ",ALL," << g_stats.trades << ',' << g_stats.wins << ',' << g_stats.losses << ','
        << g_stats.breakevens << ',' << g_stats.tp_hit << ',' << g_stats.trail_hit << ','
        << g_stats.sl_hit << ',' << g_stats.be_hit << ','
        << g_stats.gross_win_usd << ',' << g_stats.gross_loss_usd << ',' << g_stats.equity << ','
        << pf << ',' << wr << ',' << g_stats.max_dd_usd << ',' << wall << '\n';
    for (auto& kv : g_stats.pnl_by_month) {
        rep << label << ',' << kv.first << ',' << g_stats.trades_by_month[kv.first]
            << ",,,,,,,,,," << kv.second << ",,,,,\n";
    }
    rep.close();

    return 0;
}

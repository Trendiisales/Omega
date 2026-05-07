// =============================================================================
// GbpusdLondonOpenBacktest.cpp
// -----------------------------------------------------------------------------
// Standalone tick-replay backtest for include/GbpusdLondonOpenEngine.hpp.
//
// Built per audit-fixes-38 / Section 8.2 of the FX backtest audit. Reads
// HistData.com ASCII GBPUSD tick CSVs, streams ticks chronologically through
// a single GbpusdLondonOpenEngine instance, and writes per-trade and
// per-month CSV outputs. Single-config-per-binary; the parameter sweep
// wrapper (scripts/gbpusd_london_sweep.py) drives multi-config exploration
// by sed-rewriting the engine header into a tmp dir and recompiling.
//
// BUILD (from omega_repo root):
//   g++ -std=c++17 -O3 -Wno-unused-parameter -DOMEGA_BACKTEST \
//       -I backtest/gbpusd_bt -I include \
//       backtest/gbpusd_bt/GbpusdLondonOpenBacktest.cpp \
//       -o build/gbpusd_london_bt -pthread
//
// RUN:
//   ./build/gbpusd_london_bt --ticks ~/Tick/GBPUSD \
//       --from 2025-01 --to 2026-04 \
//       --trades bt_gbpusd_trades.csv \
//       --report bt_gbpusd_report.csv
//
// Tick CSV format (HistData.com ASCII):
//   YYYYMMDD HHMMSSmmm,bid,ask,vol
//
// Cost model (direct USD-quote pair, GBP-specific spread profile):
//   Slippage:    0.2 pips per side -> 0.4 pips round-trip.
//   Commission:  $0.20 per side per lot -> $0.40/lot round-trip.
//   Spread:      Implicit (engine uses ask for long entries, bid for long
//                exits and vice versa). Note: GBP typical spread on
//                BlackBull is 0.7-1.5 pips vs EUR's 0.5-1.4.
//
// USD conversion (direct USD-quote pair):
//   pnl_raw = (exit - entry) * size  (price units * lots)
//   pnl_usd = pnl_raw * 100000  (constant; same as EURUSD).
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

#include "OmegaTradeLedger.hpp"
#include "SpreadRegimeGate.hpp"
#include "OmegaNewsBlackout.hpp"

inline void bracket_on_close(const omega::TradeRecord&) {}

#include "GbpusdLondonOpenEngine.hpp"

static omega::GbpusdLondonOpenEngine g_eng;

struct Tick {
    int64_t ts_ms;
    double  bid;
    double  ask;
};

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
    double   gross_loss_usd= 0.0;
    double   max_dd_usd    = 0.0;
    double   peak_equity   = 0.0;
    double   equity        = 0.0;
    std::map<std::string,double> pnl_by_month;
    std::map<std::string,int>    trades_by_month;
    std::vector<std::pair<int64_t,double>> equity_curve;
};

static RunStats g_stats;

static double convert_to_usd(double pnl_raw) noexcept {
    return pnl_raw * 100000.0;
}

static double apply_costs_usd(double pnl_usd, double size, bool /*is_long*/) noexcept {
    const double slip_pips_per_side = 0.2;
    const double slip_usd = slip_pips_per_side * (size / 0.10) * 1.0 * 2.0;
    const double comm_usd = 0.20 * size * 2.0;
    return pnl_usd - slip_usd - comm_usd;
}

static std::ofstream g_trades_csv;

static void on_trade_close(const omega::TradeRecord& tr) {
    const double pnl_gross_usd = convert_to_usd(tr.pnl);
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

static int64_t run_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "[ERR] cannot open %s\n", path.c_str());
        return 0;
    }

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
                ++p;
            } else {
                if (!eof) std::fseek(f, -(long)(p - lineStart), SEEK_CUR);
                break;
            }
        }
    }

    std::fclose(f);
    return ticks_in_file;
}

static std::vector<std::pair<std::string,std::string>>
collect_monthly_files(const std::string& root, const std::string& from, const std::string& to) {
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
        const std::string prefix = "HISTDATA_COM_ASCII_GBPUSD_T";
        if (name.find(prefix) != 0) continue;
        if (name.size() < prefix.size() + 6) continue;
        std::string yyyymm = name.substr(prefix.size(), 6);
        if (yyyymm < from_yyyymm || yyyymm > to_yyyymm) continue;
        if (name.find(" (1)") != std::string::npos) continue;

        for (auto& f : fs::directory_iterator(e.path())) {
            if (!f.is_regular_file()) continue;
            const std::string fname = f.path().filename().string();
            if (fname.find("DAT_ASCII_GBPUSD_T_") == 0 &&
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

int main(int argc, char** argv) {
    std::string ticks_root = "";
    std::string from_ym = "2025-01";
    std::string to_ym   = "2026-04";
    std::string trades_csv = "bt_gbpusd_trades.csv";
    std::string report_csv = "bt_gbpusd_report.csv";
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

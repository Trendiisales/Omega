// ============================================================================
// AmrBacktest.cpp -- standalone tick-replay backtest for AtrMeanRevGridEngine.
//
// Streams a unix-ms tick CSV through one AtrMeanRevGridEngine<Traits> instance
// (default Traits = EURUSD). Aggregates H1 OHLC from ticks on the fly and
// pushes bar-close events into engine.on_h1_bar(); every tick also pushes
// into engine.on_tick() for SL / broker-style WAP TP monitoring.
//
// TradeRecords emitted via on_close_cb get summarised here (WR / PF / MaxDD
// per pair + per month) and written to a per-trade CSV the analysis scripts
// can consume.
//
// BUILD (from omega_repo root):
//   g++ -std=c++17 -O3 -DOMEGA_BACKTEST -I include \
//       backtest/amr_bt/AmrBacktest.cpp -o build/amr_bt -pthread
//
// RUN:
//   ./build/amr_bt --ticks backtest/eurusd-tick-2024-04-25-2026-04-25.csv \
//                  --symbol EURUSD \
//                  --warmup phase1/signal_discovery/warmup_EURUSD_H1.csv \
//                  --trades build/amr_eurusd_trades.csv \
//                  --report build/amr_eurusd_report.csv
//
// Tick CSV format (header: timestamp,askPrice,bidPrice ; ts in unix ms):
//   1714003200217,1.07002,1.07001
//
// Cost model (direct USD-quote pair, scaled to per-leg in TradeRecord):
//   See apply_costs_usd() below. 0.2 pip slip per side + $0.20/lot/side comm.
//
// USD conversion:
//   For direct USD-quote pairs (EUR/GBP/AUD/NZD): pnl_usd = pnl_price * 100000.
// ============================================================================
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "AtrMeanRevGridEngine.hpp"

// ---------- Engine instance (default Traits picked via #define -------------
// Resolved at compile time; sweep wrappers can -DAMR_TRAITS=AmrTraits_GBPUSD.
#ifndef AMR_TRAITS
#define AMR_TRAITS AmrTraits_EURUSD
#endif

static omega::AtrMeanRevGridEngine<omega::AMR_TRAITS> g_eng;

// ---------- Stats accumulator ----------------------------------------------
struct RunStats {
    int64_t  total_ticks   = 0;
    int      bars          = 0;
    int      trades        = 0;
    int      wins          = 0;
    int      losses        = 0;
    int      breakevens    = 0;
    double   gross_win_usd = 0.0;
    double   gross_loss_usd= 0.0;
    double   max_dd_usd    = 0.0;
    double   peak_equity   = 0.0;
    double   equity        = 0.0;
    std::map<std::string,int>    exit_reason_counts;
    std::map<std::string,double> pnl_by_month;
    std::map<std::string,int>    trades_by_month;
};
static RunStats g_stats;
static std::ofstream g_trades_csv;

// FX direct-USD-quote conversion (1 unit price move at 0.10 lot = $1 per pip):
//   pnl_raw = (exit - entry) * lots ; multiplier 100000 -> USD.
static double convert_to_usd(double pnl_raw) noexcept { return pnl_raw * 100000.0; }
static double apply_costs_usd(double pnl_usd, double lot) noexcept {
    const double slip_pips_per_side = 0.2;
    const double slip_usd = slip_pips_per_side * (lot / 0.10) * 1.0 * 2.0;
    const double comm_usd = 0.20 * lot * 2.0;
    return pnl_usd - slip_usd - comm_usd;
}

// ---------- on_close_cb: capture per-leg TradeRecords --------------------
static void on_trade_close(const omega::TradeRecord& tr) {
    const double gross = convert_to_usd(tr.pnl);
    // tr.size isn't populated by AtrMeanRevGridEngine -- it stamps tr.pnl
    // already as price-units * lots from close_all(). Reconstruct lot from
    // ratio if available; otherwise treat as nominal.
    double lot = 0.10;
    if (tr.entryPrice > 0.0 && tr.exitPrice > 0.0) {
        const double diff = std::fabs(tr.exitPrice - tr.entryPrice);
        if (diff > 0.0) lot = std::fabs(tr.pnl) / diff;
    }
    const double net = apply_costs_usd(gross, lot);

    g_stats.trades++;
    g_stats.equity += net;
    if (g_stats.equity > g_stats.peak_equity) g_stats.peak_equity = g_stats.equity;
    const double dd = g_stats.peak_equity - g_stats.equity;
    if (dd > g_stats.max_dd_usd) g_stats.max_dd_usd = dd;
    g_stats.exit_reason_counts[tr.exitReason.empty() ? "unknown" : tr.exitReason]++;

    if      (net > 0.5)   { g_stats.wins++;       g_stats.gross_win_usd  += net; }
    else if (net < -0.5)  { g_stats.losses++;     g_stats.gross_loss_usd += net; }
    else                  { g_stats.breakevens++; }

    time_t tt = (time_t)tr.exitTs;
    struct tm gm{}; gmtime_r(&tt, &gm);
    char ym[16]; snprintf(ym, sizeof(ym), "%04d-%02d", gm.tm_year + 1900, gm.tm_mon + 1);
    g_stats.pnl_by_month[ym] += net;
    g_stats.trades_by_month[ym]++;

    if (g_trades_csv.is_open()) {
        g_trades_csv
            << tr.symbol << ',' << tr.side << ',' << tr.engine << ','
            << tr.entryTs << ',' << tr.exitTs << ','
            << tr.entryPrice << ',' << tr.exitPrice << ','
            << lot << ','
            << tr.pnl << ',' << gross << ',' << net << ','
            << tr.exitReason << '\n';
    }
}

// ---------- H1 bar aggregator -- builds OHLC from streaming ticks ---------
struct H1Aggregator {
    std::int64_t bar_start_ms = 0;
    double o = 0, h = 0, l = 0, c = 0;
    bool   active = false;
    static constexpr std::int64_t H1_MS = 3600 * 1000;

    void on_tick(double bid, double ask, std::int64_t ts_ms,
                 void (*on_bar)(double, double, double, double, std::int64_t)) {
        const double mid = (bid + ask) * 0.5;
        const std::int64_t bar = (ts_ms / H1_MS) * H1_MS;
        if (!active) {
            bar_start_ms = bar;
            o = h = l = c = mid;
            active = true;
            return;
        }
        if (bar != bar_start_ms) {
            // Close prior bar.
            on_bar(o, h, l, c, bar_start_ms);
            // Open new.
            bar_start_ms = bar;
            o = h = l = c = mid;
        } else {
            if (mid > h) h = mid;
            if (mid < l) l = mid;
            c = mid;
        }
    }
};

static H1Aggregator g_h1;

static void deliver_bar(double o, double h, double l, double c, std::int64_t ts_ms) {
    g_eng.on_h1_bar(o, h, l, c, ts_ms);
    g_stats.bars++;
}

// ---------- Tick CSV parser (unix-ms format) ------------------------------
struct Tick { std::int64_t ts_ms; double bid; double ask; };

static bool parse_tick_line(const char* p, std::size_t n, Tick& out) noexcept {
    // header guard
    if (n == 0 || (p[0] == 't' || p[0] == '#')) return false;
    char* endp = nullptr;
    std::int64_t ts = std::strtoll(p, &endp, 10);
    if (!endp || *endp != ',') return false;
    double ask = std::strtod(endp + 1, &endp);
    if (!endp || *endp != ',') return false;
    double bid = std::strtod(endp + 1, &endp);
    if (ask <= 0.0 || bid <= 0.0) return false;
    if (ask < bid) std::swap(ask, bid); // tolerate misordered fields
    out.ts_ms = ts; out.bid = bid; out.ask = ask;
    return true;
}

static int64_t run_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "[ERR] cannot open %s\n", path.c_str()); return 0; }

    static thread_local std::vector<char> buf(4 * 1024 * 1024);
    std::int64_t ticks = 0;
    std::size_t carry = 0;
    bool first_line_skipped = false;

    while (true) {
        std::size_t want = buf.size() - carry;
        std::size_t got  = std::fread(buf.data() + carry, 1, want, f);
        std::size_t avail = carry + got;
        if (got == 0 && avail == 0) break;

        // Process complete lines
        std::size_t i = 0, line_start = 0;
        while (i < avail) {
            if (buf[i] == '\n') {
                std::size_t len = i - line_start;
                if (len > 0 && buf[line_start + len - 1] == '\r') --len;
                if (!first_line_skipped) {
                    first_line_skipped = true;
                } else {
                    Tick t;
                    if (parse_tick_line(buf.data() + line_start, len, t)) {
                        // Engine internally aggregates H1 bars from ticks now,
                        // so the harness only needs on_tick().
                        g_eng.on_tick(t.bid, t.ask, t.ts_ms);
                        ++ticks;
                    }
                }
                line_start = i + 1;
            }
            ++i;
        }
        // shift leftover
        carry = avail - line_start;
        if (carry > 0) std::memmove(buf.data(), buf.data() + line_start, carry);
        if (got == 0) break;
    }
    std::fclose(f);
    return ticks;
}

// ---------- Report writer ------------------------------------------------
static void write_report(const std::string& report_path) {
    std::ofstream r(report_path);
    if (!r) { std::fprintf(stderr, "[ERR] cannot open report %s\n", report_path.c_str()); return; }
    const double pf = (g_stats.gross_loss_usd < 0.0)
                      ? (g_stats.gross_win_usd / -g_stats.gross_loss_usd)
                      : (g_stats.gross_win_usd > 0.0 ? 99.0 : 0.0);
    const double wr = (g_stats.trades > 0) ? 100.0 * g_stats.wins / g_stats.trades : 0.0;
    const double avg_win  = g_stats.wins   > 0 ? g_stats.gross_win_usd  / g_stats.wins   : 0.0;
    const double avg_loss = g_stats.losses > 0 ? g_stats.gross_loss_usd / g_stats.losses : 0.0;

    r << "metric,value\n";
    r << "ticks,"          << g_stats.total_ticks  << "\n";
    r << "bars_h1,"        << g_stats.bars         << "\n";
    r << "trades,"         << g_stats.trades       << "\n";
    r << "wins,"           << g_stats.wins         << "\n";
    r << "losses,"         << g_stats.losses       << "\n";
    r << "breakevens,"     << g_stats.breakevens   << "\n";
    r << "win_rate_pct,"   << wr                   << "\n";
    r << "profit_factor,"  << pf                   << "\n";
    r << "net_pnl_usd,"    << g_stats.equity       << "\n";
    r << "max_drawdown,"   << g_stats.max_dd_usd   << "\n";
    r << "avg_win_usd,"    << avg_win              << "\n";
    r << "avg_loss_usd,"   << avg_loss             << "\n";

    r << "\nexit_reason,count\n";
    for (auto& kv : g_stats.exit_reason_counts) r << kv.first << ',' << kv.second << '\n';

    r << "\nmonth,trades,pnl_usd\n";
    for (auto& kv : g_stats.trades_by_month)
        r << kv.first << ',' << kv.second << ',' << g_stats.pnl_by_month[kv.first] << '\n';

    std::printf("\n[REPORT] %s\n", report_path.c_str());
    std::printf("  ticks=%lld bars=%d trades=%d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  MaxDD=$%.2f\n",
                (long long)g_stats.total_ticks, g_stats.bars, g_stats.trades, wr, pf,
                g_stats.equity, g_stats.max_dd_usd);
}

// ---------- main ----------------------------------------------------------
int main(int argc, char** argv) {
    std::string ticks_path, warmup_path, trades_path, report_path;
    std::string sym_label = "EURUSD";
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i+1];
        if      (k == "--ticks")  ticks_path  = v;
        else if (k == "--warmup") warmup_path = v;
        else if (k == "--trades") trades_path = v;
        else if (k == "--report") report_path = v;
        else if (k == "--symbol") sym_label   = v;
    }
    if (ticks_path.empty()) {
        std::fprintf(stderr, "usage: amr_bt --ticks <csv> [--warmup <h1csv>] [--trades <out>] [--report <out>] [--symbol EURUSD]\n");
        return 2;
    }

    g_eng.enabled     = true;
    g_eng.shadow_mode = true;
    g_eng.on_close_cb = on_trade_close;

    if (!warmup_path.empty()) {
        std::printf("[SEED] %s\n", warmup_path.c_str());
        g_eng.seed_from_h1_csv(warmup_path);
    }

    if (!trades_path.empty()) {
        g_trades_csv.open(trades_path);
        g_trades_csv << "symbol,side,engine,entry_ts,exit_ts,entry_px,exit_px,lot,pnl_raw,pnl_gross_usd,pnl_net_usd,exit_reason\n";
    }

    std::printf("[RUN] %s -> ticks=%s\n", sym_label.c_str(), ticks_path.c_str());
    int64_t n = run_file(ticks_path);
    g_stats.total_ticks = n;
    std::printf("[RUN] %lld ticks, %d H1 bars\n", (long long)n, g_stats.bars);

    if (!report_path.empty()) write_report(report_path);
    return 0;
}

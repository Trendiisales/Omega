// =============================================================================
// LatencyEdgeBacktest.cpp -- harness for LatencyEdgeEngines (Gold)
// =============================================================================
// Drives omega::latency::GoldSpreadDislocation and/or GoldEventCompression
// directly (bypassing the LatencyEdgeStack wrapper which is still no-op'd
// from the S13 RTT-68ms cull). Operates under OmegaTimeShim so the engines'
// internal session-gate and event-schedule logic resolve against the tape
// timestamps, not wall clock.
//
// CLI:
//   LatencyEdgeBacktest <ticks.csv> [--engine spread|event|both]
//                                   [--tp PTS] [--sl PTS]
//                                   [--cooldown SEC] [--max-hold SEC]
//                                   [--spike-ratio R]
//                                   [--min-med PTS] [--max-med PTS]
//                                   [--cost-rt USD]
//                                   [--trades PATH] [--report PATH]
//
// Tape format: A only (ts_ms,bid,ask). The XAUUSD 26mo tape at
// /Users/jo/Tick/2yr_XAUUSD_format_a.csv is the reference input.
//
// Cost model: --cost-rt subtracts a fixed dollar cost per round-trip from
// per-trade gross PnL to produce net PnL. Default 0.60 (BB gold @ 0.01 lot
// ~$0.22 spread + commission + slip per operator handoff).
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>

#include "OmegaTimeShim.hpp"
#include "OmegaTradeLedger.hpp"
#include "LatencyEdgeEngines.hpp"

// ----------------------------------------------------------------------------
// Tick parsing (format A only)
// ----------------------------------------------------------------------------
struct TickRow { int64_t ts_ms; double bid; double ask; };

static bool parse_row(const std::string& line, TickRow& out) {
    if (line.empty()) return false;
    // Skip header / non-numeric lead
    for (size_t i = 0; i < std::min<size_t>(12, line.size()); ++i) {
        if (std::isalpha(static_cast<unsigned char>(line[i]))) return false;
    }
    const char* p = line.c_str();
    char* e = nullptr;
    const int64_t ts = std::strtoll(p, &e, 10);
    if (*e != ',') return false; p = e + 1;
    const double bid = std::strtod(p, &e);
    if (*e != ',') return false; p = e + 1;
    const double ask = std::strtod(p, &e);
    if (bid <= 0.0 || ask <= 0.0 || bid > ask) return false;
    out.ts_ms = ts; out.bid = bid; out.ask = ask;
    return true;
}

// ----------------------------------------------------------------------------
// Stats accumulator (per-engine)
// ----------------------------------------------------------------------------
struct Stats {
    int64_t trades   = 0;
    int64_t wins     = 0;
    double  gross    = 0.0;
    double  best     = 0.0;
    double  worst    = 0.0;
    int64_t tp_hit   = 0;
    int64_t sl_hit   = 0;
    int64_t timeout  = 0;
    int64_t force    = 0;
    std::vector<double> losses;

    void record(const omega::TradeRecord& tr) {
        ++trades; gross += tr.pnl;
        if (tr.pnl > 0.0) ++wins;
        if (tr.pnl > best)  best  = tr.pnl;
        if (tr.pnl < worst) worst = tr.pnl;
        if (tr.pnl < 0.0) losses.push_back(tr.pnl);
        const std::string& r = tr.exitReason;
        if      (r == "TP_HIT")  ++tp_hit;
        else if (r == "SL_HIT")  ++sl_hit;
        else if (r == "TIMEOUT") ++timeout;
        else                     ++force;
    }
    double win_rate() const { return trades > 0 ? 100.0 * wins / trades : 0.0; }
    double p95_worst() {
        if (losses.empty()) return 0.0;
        std::vector<double> s = losses;
        std::sort(s.begin(), s.end());
        return s[static_cast<size_t>(0.05 * s.size())];
    }
};

static void print_stats(const char* tag, Stats& s, double cost_rt) {
    const double cost_total = cost_rt * static_cast<double>(s.trades);
    const double net = s.gross - cost_total;
    std::fprintf(stderr,
        "\n  %s\n"
        "    trades       = %lld   (wins=%lld  win_rate=%.1f%%)\n"
        "    gross_pnl    = %.4f   (avg=%.5f)\n"
        "    cost_rt      = %.4f  total_cost=%.2f\n"
        "    NET PnL      = %.4f   (avg=%.5f)\n"
        "    best         = %.4f\n"
        "    worst        = %.4f   (p95=%.4f)\n"
        "    exits: TP=%lld  SL=%lld  T/O=%lld  FORCE=%lld\n",
        tag,
        (long long)s.trades, (long long)s.wins, s.win_rate(),
        s.gross, s.trades > 0 ? s.gross / s.trades : 0.0,
        cost_rt, cost_total,
        net, s.trades > 0 ? net / s.trades : 0.0,
        s.best, s.worst, s.p95_worst(),
        (long long)s.tp_hit, (long long)s.sl_hit,
        (long long)s.timeout, (long long)s.force);
}

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: LatencyEdgeBacktest <ticks.csv> [options]\n"
            "  --engine    spread | event | both     (default both)\n"
            "  --tp        PTS                       (default engine default)\n"
            "  --sl        PTS                       (default engine default)\n"
            "  --cooldown  SEC\n"
            "  --max-hold  SEC\n"
            "  --spike-ratio R                       (spread only)\n"
            "  --min-med   PTS                       (spread only)\n"
            "  --max-med   PTS                       (spread only)\n"
            "  --cost-rt   USD                       (default 0.60)\n"
            "  --trades    PATH\n"
            "  --report    PATH\n");
        return 1;
    }

    const char* in_path     = argv[1];
    std::string engine_sel  = "both";
    double tp = -1.0, sl = -1.0;
    int    cooldown = -1, max_hold = -1;
    double spike_ratio = -1.0, min_med = -1.0, max_med = -1.0;
    double cost_rt = 0.60;
    std::string trades_path, report_path;

    for (int i = 2; i < argc; ++i) {
        if      (!std::strcmp(argv[i], "--engine")     && i+1 < argc) engine_sel  = argv[++i];
        else if (!std::strcmp(argv[i], "--tp")         && i+1 < argc) tp          = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--sl")         && i+1 < argc) sl          = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--cooldown")   && i+1 < argc) cooldown    = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--max-hold")   && i+1 < argc) max_hold    = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--spike-ratio")&& i+1 < argc) spike_ratio = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--min-med")    && i+1 < argc) min_med     = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--max-med")    && i+1 < argc) max_med     = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--cost-rt")    && i+1 < argc) cost_rt     = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--trades")     && i+1 < argc) trades_path = argv[++i];
        else if (!std::strcmp(argv[i], "--report")     && i+1 < argc) report_path = argv[++i];
    }

    const bool run_spread = (engine_sel == "spread" || engine_sel == "both");
    const bool run_event  = (engine_sel == "event"  || engine_sel == "both");

    omega::latency::GoldSpreadDislocation spread_eng;
    omega::latency::GoldEventCompression  event_eng;

    // Apply CLI overrides (use engine defaults otherwise)
    if (run_spread) {
        spread_eng.configure(
            spike_ratio > 0 ? spike_ratio : 2.5,
            min_med     > 0 ? min_med     : 0.30,
            max_med     > 0 ? max_med     : 1.20,
            tp          > 0 ? tp          : 0.30,
            sl          > 0 ? sl          : 0.15,
            cooldown    > 0 ? cooldown    : 60,
            max_hold    > 0 ? max_hold    : 30);
    }
    if (run_event) {
        event_eng.configure(
            0.40,                                  // EVENT_COMP_RANGE
            0.15,                                  // EVENT_TRIGGER
            tp       > 0 ? tp       : 3.00,
            sl       > 0 ? sl       : 0.80,
            max_hold > 0 ? max_hold : 300,
            600,                                   // COOLDOWN_SEC (event default)
            2.00);                                 // MAX_SPREAD
    }

    Stats spread_stats, event_stats;
    std::vector<omega::TradeRecord> all_trades;

    auto on_close_spread = [&](const omega::TradeRecord& tr) {
        spread_stats.record(tr);
        all_trades.push_back(tr);
    };
    auto on_close_event = [&](const omega::TradeRecord& tr) {
        event_stats.record(tr);
        all_trades.push_back(tr);
    };

    std::ifstream in(in_path);
    if (!in) {
        std::fprintf(stderr, "[ERROR] cannot open %s\n", in_path);
        return 2;
    }

    int64_t ticks_read = 0, parse_skips = 0;
    int64_t first_ts = 0, last_ts = 0;
    std::string line;
    while (std::getline(in, line)) {
        TickRow r;
        if (!parse_row(line, r)) { ++parse_skips; continue; }
        ++ticks_read;
        if (first_ts == 0) first_ts = r.ts_ms;
        last_ts = r.ts_ms;

        omega::bt::set_sim_time(r.ts_ms);

        const double latency_ms = 0.5;  // simulated colo RTT
        // can_enter=true: backtest does not coordinate cross-engine position lockout
        if (run_spread) spread_eng.on_tick(r.bid, r.ask, latency_ms, on_close_spread, true);
        if (run_event)  event_eng .on_tick(r.bid, r.ask, latency_ms, on_close_event,  true);

        if ((ticks_read % 10000000LL) == 0) {
            std::fprintf(stderr, "[progress] %lld ticks read\n", (long long)ticks_read);
        }
    }

    const double duration_days = (last_ts - first_ts) / 1000.0 / 86400.0;

    std::fprintf(stderr,
        "\n================================================================\n"
        "  LatencyEdgeBacktest  --  engine=%s  cost_rt=%.4f\n"
        "    input        = %s\n"
        "    ticks_read   = %lld   (parse_skips=%lld)\n"
        "    duration     = %.2f days\n",
        engine_sel.c_str(), cost_rt, in_path,
        (long long)ticks_read, (long long)parse_skips, duration_days);

    if (run_spread) print_stats("GoldSpreadDislocation", spread_stats, cost_rt);
    if (run_event)  print_stats("GoldEventCompression",  event_stats,  cost_rt);

    std::fprintf(stderr,
        "================================================================\n");

    // ---- Reports ----
    if (!report_path.empty()) {
        std::ofstream out(report_path);
        out << "metric,value\n";
        out << "engine," << engine_sel << "\n";
        out << "ticks_read," << ticks_read << "\n";
        out << "duration_days," << duration_days << "\n";
        out << "cost_rt," << cost_rt << "\n";
        auto dump = [&](const char* tag, Stats& s){
            const double cost_total = cost_rt * s.trades;
            const double net = s.gross - cost_total;
            out << tag << "_trades,"   << s.trades       << "\n";
            out << tag << "_wins,"     << s.wins         << "\n";
            out << tag << "_win_rate," << s.win_rate()   << "\n";
            out << tag << "_gross_pnl," << s.gross       << "\n";
            out << tag << "_cost_total," << cost_total   << "\n";
            out << tag << "_net_pnl,"  << net            << "\n";
            out << tag << "_best,"     << s.best         << "\n";
            out << tag << "_worst,"    << s.worst        << "\n";
            out << tag << "_p95_worst," << s.p95_worst() << "\n";
            out << tag << "_tp_hit,"   << s.tp_hit       << "\n";
            out << tag << "_sl_hit,"   << s.sl_hit       << "\n";
            out << tag << "_timeout,"  << s.timeout      << "\n";
            out << tag << "_force,"    << s.force        << "\n";
        };
        if (run_spread) dump("spread", spread_stats);
        if (run_event)  dump("event",  event_stats);
        out << "spread_tp," << spread_eng.has_open_position() << "\n"; // touch to silence unused
        std::fprintf(stderr, "[report] wrote %s\n", report_path.c_str());
    }

    if (!trades_path.empty()) {
        std::ofstream out(trades_path);
        out << "engine,entry_ts_ms,exit_ts_ms,side,entry,exit,tp,sl,size,pnl,mfe,mae,exit_reason\n";
        for (const auto& tr : all_trades) {
            out << tr.engine << ","
                << tr.entryTs << "," << tr.exitTs << ","
                << tr.side << ","
                << tr.entryPrice << "," << tr.exitPrice << ","
                << tr.tp << "," << tr.sl << "," << tr.size << ","
                << tr.pnl << "," << tr.mfe << "," << tr.mae << ","
                << tr.exitReason << "\n";
        }
        std::fprintf(stderr, "[trades] wrote %s (%zu rows)\n",
                     trades_path.c_str(), all_trades.size());
    }

    return 0;
}

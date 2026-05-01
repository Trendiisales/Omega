// =============================================================================
// hbg_duka_bt.cpp -- GoldHybridBracketEngine backtester for Dukascopy XAUUSD
// 2026-04-29 audit -- 26-month validation harness for HBG post-fixes-4
//
// PURPOSE
//   Validate that the post-audit GoldHybridBracketEngine (commit 675f063f,
//   audit-fixes-4) does not exhibit the Apr-7 "100x P&L race" failure mode
//   under any tick interleaving across 26 months. Tests:
//     - m_close_mtx mutex serialises _close() correctly
//     - Snapshot-pos.* into locals before tr build
//     - Sanity check on tr.pnl magnitude (|pnl| > size * 200pt) recompute path
//     - Atomic ostringstream log line
//
// DESIGN
//   Built from the same template as backtest/cfe_duka_bt.cpp and
//   backtest/mce_duka_bt.cpp. Tick parser is identical. HBG itself is a
//   self-contained tick-level state machine -- it computes its own range,
//   spread, expansion-history, and cost-feasibility internally. The harness
//   does NOT compute external indicators (ATR, drift, vol_ratio, RSI) because
//   HBG does not consume them.
//
// EXTERNAL FLAGS PASSED IN
//   can_enter        -- true (no external session/risk gate in BT)
//   flow_live        -- false (no parent flow engine; HBG operates standalone,
//                       which corresponds to RISK_DOLLARS = $30 sizing path)
//   flow_be_locked   -- false (only relevant when flow_live = true)
//   flow_trail_stage -- 0     (only relevant when flow_live = true)
//   DOM defaults     -- l2_real=false implicitly (we omit the optional args),
//                       so HBG's DOM filter is bypassed, which is the correct
//                       BT behaviour without an L2 feed.
//
// BRACKETTRENDSTATE
//   Same self-disabling behaviour as in mce_duka_bt: BT TUs do not populate
//   g_bracket_trend, so any bias accessor returns 0 / no-op. HBG does not
//   currently consult bracket_trend_bias() in its on_tick path, so this is
//   moot here; mentioned only for symmetry with the MCE harness.
//
// TRADE EMISSION
//   HBG has a single CloseCallback -- on_close(const TradeRecord&). It fires
//   once per parent trade at TP_HIT, SL_HIT, FORCE_CLOSE, or trail-related
//   exits. We aggregate exactly there, mirroring the cfe_duka_bt pattern.
//
//   The engine's _close path is mutex-serialised post-fixes-4. If the
//   sanity-check "tr.pnl recompute" path ever fires in a 26-month run, that
//   would be an interesting telemetry signal -- but the BT runs single-
//   threaded so we do not expect to observe it. Absence of crashes /
//   absurd pnl values is the test.
//
// COSTS
//   omega::apply_realistic_costs(tr, 3.0, 100.0) -- same model and parameters
//   as cfe_duka_bt and mce_duka_bt for apples-to-apples comparison.
//
// SHADOW MODE
//   hbg.shadow_mode = true (default). HBG's design comments (lines 23-29) say
//   shadow mode = log only, no live orders. on_close still fires in shadow
//   mode (same convention as MCE), so trade records reach the BT.
//
// BUILD (Mac, from repo root)
//   g++ -O2 -std=c++17 -I include -o hbg_duka_bt backtest/hbg_duka_bt.cpp
//
// RUN
//   Full 26-month dataset (default behaviour, ~2h 15m on Mac):
//     ./hbg_duka_bt /Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv
//
//   Windowed run (added 2026-05-01, ~30min for last-6-months window):
//     ./hbg_duka_bt <csv> --from 2025-11-01 --to 2026-04-30
//
//   Either flag can be omitted; missing --from = no lower bound, missing
//   --to = no upper bound. Dates are UTC midnight.
//
// OUTPUT
//   stdout: summary stats (trades, WR, net P&L, exit-reason breakdown, by month)
//   hbg_duka_bt_trades.csv: every closed trade for downstream analysis
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <cmath>
#include <algorithm>
#include <functional>
#include <chrono>
#include <ctime>
#include <mutex>

#include "OmegaTradeLedger.hpp"
#include "GoldHybridBracketEngine.hpp"

// ----------------------------------------------------------------------------
// Dukascopy parser (identical to cfe_duka_bt and mce_duka_bt)
// ----------------------------------------------------------------------------
struct DukaTick {
    int64_t ts_ms = 0;
    double  bid   = 0.0;
    double  ask   = 0.0;
};

static int64_t iso_to_epoch_ms(const std::string& ts) {
    if (ts.size() < 19) return 0;
    std::tm t{};
    t.tm_year = std::stoi(ts.substr(0, 4)) - 1900;
    t.tm_mon  = std::stoi(ts.substr(5, 2)) - 1;
    t.tm_mday = std::stoi(ts.substr(8, 2));
    t.tm_hour = std::stoi(ts.substr(11, 2));
    t.tm_min  = std::stoi(ts.substr(14, 2));
    t.tm_sec  = std::stoi(ts.substr(17, 2));
    int64_t ms = 0;
    if (ts.size() >= 23 && ts[19] == '.') {
        ms = std::stol(ts.substr(20, 3));
    }
    int64_t epoch_s = (int64_t)timegm(&t);
    return epoch_s * 1000 + ms;
}

static bool parse_duka(const std::string& line, DukaTick& t) {
    if (line.empty()) return false;
    std::vector<std::string> f;
    std::string cur;
    for (char c : line) {
        if (c == ',') { f.push_back(cur); cur.clear(); }
        else if (c != '\r') cur.push_back(c);
    }
    f.push_back(cur);
    if (f.size() < 3) return false;

    try {
        const std::string& ts = f[0];
        bool numeric = !ts.empty() && (ts[0] == '-' || std::isdigit((unsigned char)ts[0]));
        for (char c : ts) {
            if (!std::isdigit((unsigned char)c) && c != '.' && c != '-' && c != '+' && c != 'e' && c != 'E') {
                numeric = false;
                break;
            }
        }
        if (numeric) {
            const double v = std::stod(ts);
            if (v > 1e12)      t.ts_ms = (int64_t)v;
            else if (v > 1e9)  t.ts_ms = (int64_t)(v * 1000.0);
            else               t.ts_ms = (int64_t)v;
        } else {
            t.ts_ms = iso_to_epoch_ms(ts);
        }
        const double a = std::stod(f[1]);
        const double b = std::stod(f[2]);
        t.bid = std::min(a, b);
        t.ask = std::max(a, b);
    } catch (...) {
        return false;
    }
    return t.bid > 0.0 && t.ask > 0.0 && t.ask >= t.bid && t.ts_ms > 0;
}

// ----------------------------------------------------------------------------
// Run
// ----------------------------------------------------------------------------
struct MonthBucket {
    int    n    = 0;
    int    wins = 0;
    double pnl  = 0.0;
};

// ----------------------------------------------------------------------------
// Date-window helpers (added 2026-05-01)
// Converts "YYYY-MM-DD" to epoch_ms at UTC midnight. Returns -1 on parse fail.
// Used by the optional --from / --to flags to bound the tick stream so we
// can run fast iteration windows (e.g. last 6 months ~30min) instead of
// the full 26-month dataset (~2h 15m). Out-of-window ticks are skipped at
// the top of the main loop, BEFORE on_tick, so the engine sees a clean
// contiguous window with no fake gaps.
// ----------------------------------------------------------------------------
static int64_t ymd_to_epoch_ms(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return -1;
    try {
        std::tm t{};
        t.tm_year = std::stoi(s.substr(0,4)) - 1900;
        t.tm_mon  = std::stoi(s.substr(5,2)) - 1;
        t.tm_mday = std::stoi(s.substr(8,2));
        t.tm_hour = 0; t.tm_min = 0; t.tm_sec = 0;
        return (int64_t)timegm(&t) * 1000;
    } catch (...) { return -1; }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: hbg_duka_bt <duka_xauusd.csv> [--from YYYY-MM-DD] [--to YYYY-MM-DD]\n";
        std::cerr << "Outputs:\n";
        std::cerr << "  stdout : summary\n";
        std::cerr << "  hbg_duka_bt_trades.csv : per-trade records\n";
        return 1;
    }

    // Parse optional --from / --to date window flags.
    int64_t window_from_ms = -1;   // -1 = no lower bound
    int64_t window_to_ms   = -1;   // -1 = no upper bound
    std::string window_from_str, window_to_str;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--from" || arg == "--to") && i + 1 < argc) {
            std::string val = argv[++i];
            int64_t ms = ymd_to_epoch_ms(val);
            if (ms < 0) {
                std::cerr << "Bad date for " << arg << ": " << val
                          << " (expected YYYY-MM-DD)\n";
                return 1;
            }
            if (arg == "--from") { window_from_ms = ms;       window_from_str = val; }
            else                 { window_to_ms   = ms + 86400000LL; window_to_str = val; }
            // --to is end-of-day inclusive: bump by 24h so e.g. "--to 2026-04-30"
            // includes ticks all the way to 2026-05-01 00:00:00 UTC.
        } else {
            std::cerr << "Unknown arg: " << arg << "\n";
            return 1;
        }
    }

    omega::GoldHybridBracketEngine hbg;
    hbg.shadow_mode = true;   // safe -- BT does not place real orders

    int trades = 0, wins = 0, losses = 0;
    double pnl_total = 0.0;
    double cum_min = 0.0, cum_max = 0.0, max_dd = 0.0;
    std::map<std::string, int>    exit_n;
    std::map<std::string, double> exit_pnl;
    std::map<std::string, MonthBucket> by_month;

    std::ofstream out("hbg_duka_bt_trades.csv");
    out << "id,symbol,side,exit_reason,entry,exit,sl,tp,size,gross_pnl,net_pnl,mfe,mae,entry_ts,exit_ts\n";

    // HBG's on_close fires once per parent trade with the full TradeRecord.
    // tr.pnl arrives as raw (price_points * size) -- we apply tick mult and
    // realistic costs the same way the CFE and MCE harnesses do.
    omega::GoldHybridBracketEngine::CloseCallback on_close =
        [&](const omega::TradeRecord& tr_in) {
        omega::TradeRecord tr = tr_in;
        const double mult = 100.0;          // XAUUSD tick value
        tr.pnl *= mult;
        tr.mfe *= mult;
        tr.mae *= mult;
        omega::apply_realistic_costs(tr, 3.0, mult);

        trades++;
        if (tr.net_pnl > 0) wins++;
        else                losses++;
        pnl_total += tr.net_pnl;
        cum_min = std::min(cum_min, pnl_total);
        cum_max = std::max(cum_max, pnl_total);
        max_dd  = std::max(max_dd,  cum_max - pnl_total);
        exit_n[tr.exitReason]++;
        exit_pnl[tr.exitReason] += tr.net_pnl;

        std::time_t t = (std::time_t)tr.entryTs;
        std::tm* gm = std::gmtime(&t);
        char ym[16];
        std::snprintf(ym, sizeof(ym), "%04d-%02d", gm->tm_year + 1900, gm->tm_mon + 1);
        auto& mb = by_month[ym];
        mb.n++;
        if (tr.net_pnl > 0) mb.wins++;
        mb.pnl += tr.net_pnl;

        out << tr.id << "," << tr.symbol << "," << tr.side << ","
            << tr.exitReason << ","
            << std::fixed << std::setprecision(4)
            << tr.entryPrice << "," << tr.exitPrice << ","
            << tr.sl << "," << tr.tp << "," << tr.size << ","
            << tr.pnl << "," << tr.net_pnl << ","
            << tr.mfe << "," << tr.mae << ","
            << tr.entryTs << "," << tr.exitTs << "\n";
    };

    std::ifstream f(argv[1]);
    if (!f) {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 2;
    }

    // Silence HBG per-tick stdout chatter
    std::ofstream null_sink("/dev/null");
    std::streambuf* saved_cout = std::cout.rdbuf(null_sink.rdbuf());

    std::string line;
    long ticks = 0;
    long last_progress = 0;
    auto t_start = std::chrono::steady_clock::now();
    std::cerr << "reading " << argv[1] << "...\n";

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (ticks == 0 && (line.find("timestamp") != std::string::npos
                        || line.find("bid")       != std::string::npos
                        || line.find("Time")      != std::string::npos)) continue;
        DukaTick t;
        if (!parse_duka(line, t)) continue;

        // Window filter (added 2026-05-01). Skip out-of-window ticks BEFORE
        // counting them so the "ticks" total reflects only what HBG actually
        // saw. This means progress reports and the final tick count match
        // the windowed run, not the full file.
        if (window_from_ms >= 0 && t.ts_ms < window_from_ms) continue;
        if (window_to_ms   >= 0 && t.ts_ms >= window_to_ms) {
            // Stream is roughly time-ordered. Once we pass --to we can stop.
            break;
        }
        ++ticks;

        // HBG operates standalone in BT: no parent flow, no DOM. The engine
        // computes its own range, spread, and expansion-history internally
        // from the tick stream.
        hbg.on_tick(t.bid, t.ask, t.ts_ms,
                    /*can_enter*/        true,
                    /*flow_live*/        false,
                    /*flow_be_locked*/   false,
                    /*flow_trail_stage*/ 0,
                    on_close);

        if (ticks - last_progress >= 5000000) {
            last_progress = ticks;
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - t_start).count();
            std::cerr << "  " << ticks << " ticks, "
                      << trades << " trades, $"
                      << std::fixed << std::setprecision(2) << pnl_total
                      << " (" << elapsed << "s elapsed)\n";
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t_start).count();

    std::cout.rdbuf(saved_cout);

    std::cout << "\n=== HBG DUKASCOPY BACKTEST ===\n";
    std::cout << "File         : " << argv[1] << "\n";
    if (!window_from_str.empty() || !window_to_str.empty()) {
        std::cout << "Window       : "
                  << (window_from_str.empty() ? "(file start)" : window_from_str)
                  << " -> "
                  << (window_to_str.empty()   ? "(file end)"   : window_to_str)
                  << "\n";
    } else {
        std::cout << "Window       : (full file)\n";
    }
    std::cout << "Ticks        : " << ticks << "\n";
    std::cout << "Elapsed      : " << elapsed << " s\n";
    std::cout << "Trades       : " << trades << "\n";
    std::cout << "Wins         : " << wins << "\n";
    std::cout << "Losses       : " << losses << "\n";
    std::cout << "Win rate     : " << std::fixed << std::setprecision(1)
              << (trades > 0 ? 100.0 * wins / trades : 0.0) << " %\n";
    std::cout << "Net PnL      : $" << std::fixed << std::setprecision(2) << pnl_total << "\n";
    std::cout << "Avg / trade  : $" << std::fixed << std::setprecision(2)
              << (trades > 0 ? pnl_total / trades : 0.0) << "\n";
    std::cout << "Max DD       : $" << std::fixed << std::setprecision(2) << max_dd << "\n";

    std::cout << "\nExit reasons:\n";
    for (auto& kv : exit_n) {
        std::cout << "  " << std::setw(14) << std::left << kv.first
                  << " n=" << std::setw(6) << kv.second
                  << " pnl=$" << std::fixed << std::setprecision(2)
                  << exit_pnl[kv.first] << "\n";
    }

    std::cout << "\nBy month (n / WR / pnl):\n";
    for (auto& kv : by_month) {
        const auto& m = kv.second;
        std::cout << "  " << kv.first
                  << "  n=" << std::setw(5) << m.n
                  << "  WR=" << std::setw(5) << std::setprecision(1)
                  << (m.n > 0 ? 100.0 * m.wins / m.n : 0.0) << "%"
                  << "  pnl=$" << std::setprecision(2) << m.pnl << "\n";
    }

    std::cout << "\nWrote per-trade records to hbg_duka_bt_trades.csv\n";
    return 0;
}

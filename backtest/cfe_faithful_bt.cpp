// =============================================================================
// cfe_faithful_bt.cpp -- faithful CFE backtester
// 2026-04-29 audit
//
// Unlike backtest/candle_rsi_l2_bt_final.cpp (which only emits IMB_EXIT /
// SL_HIT / STAGNATION), this harness LINKS DIRECTLY to the live engine
// header CandleFlowEngine.hpp. Every exit path the live engine produces --
// PARTIAL_TP, TRAIL_SL, BE_HIT, MAX_HOLD, etc. -- is reproduced exactly,
// because the simulation IS the live code.
//
// Build:
//   g++ -O2 -std=c++17 -I<repo>/include -o cfe_faithful_bt cfe_faithful_bt.cpp
//
// Run:
//   ./cfe_faithful_bt l2_ticks_2026-04-09.csv l2_ticks_2026-04-10.csv ...
//
// Tick CSV columns:
//   ts_ms, bid, ask, l2_imb, l2_bid_vol, l2_ask_vol,
//   depth_bid_levels, depth_ask_levels, depth_events_total,
//   watchdog_dead, vol_ratio, regime, vpin, has_pos, micro_edge, ewm_drift
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
#include <mutex>

// Use the REAL OmegaTradeLedger.hpp and OmegaFIX.hpp from the repo so the
// engine code finds matching struct definitions and methods. We just need
// to predeclare so OmegaTradeLedger doesn't itself pull in the world.
#include "OmegaFIX.hpp"          // L2Level, L2Book + microstructure methods
#include "OmegaTradeLedger.hpp"  // omega::TradeRecord + apply_realistic_costs

// Bring in GoldHMM and CFE
#include "GoldHMM.hpp"
#include "CandleFlowEngine.hpp"

// ----------------------------------------------------------------------------
// Tick parser
// ----------------------------------------------------------------------------
struct Tick {
    int64_t ts_ms = 0;
    double  bid = 0, ask = 0;
    double  l2_imb = 0.5;
    double  l2_bid_vol = 0, l2_ask_vol = 0;
    int     depth_bid_levels = 0, depth_ask_levels = 0;
    bool    watchdog_dead = false;
    double  vol_ratio = 0;
    double  ewm_drift = 0;
};

static bool parse_tick(const std::string& line, Tick& t) {
    if (line.empty() || line[0] == 't') return false;  // header
    // Split by comma
    std::vector<std::string> f;
    std::string cur;
    for (char c : line) {
        if (c == ',') { f.push_back(cur); cur.clear(); }
        else if (c != '\r') cur.push_back(c);
    }
    f.push_back(cur);
    // Schema: 14-col (older, no micro_edge/ewm_drift) or 16-col (newer)
    if (f.size() < 14) return false;
    try {
        t.ts_ms             = (int64_t)std::stod(f[0]);
        t.bid               = std::stod(f[1]);
        t.ask               = std::stod(f[2]);
        t.l2_imb            = std::stod(f[3]);
        t.l2_bid_vol        = std::stod(f[4]);
        t.l2_ask_vol        = std::stod(f[5]);
        t.depth_bid_levels  = (int)std::stol(f[6]);
        t.depth_ask_levels  = (int)std::stol(f[7]);
        t.watchdog_dead     = (std::stoi(f[9]) != 0);
        t.vol_ratio         = std::stod(f[10]);
        t.ewm_drift         = (f.size() >= 16) ? std::stod(f[15]) : 0.0;
    } catch (...) { return false; }
    return t.bid > 0 && t.ask > 0 && t.ask >= t.bid;
}

// Build L2Book from a tick row (fields are aggregate, not per-level, so
// distribute uniformly across the reported number of levels).
static L2Book build_book(const Tick& t) {
    L2Book b;
    b.bid_count = std::min(5, t.depth_bid_levels);
    b.ask_count = std::min(5, t.depth_ask_levels);
    const double per_bid = (b.bid_count > 0) ? t.l2_bid_vol / b.bid_count : 0.0;
    const double per_ask = (b.ask_count > 0) ? t.l2_ask_vol / b.ask_count : 0.0;
    for (int i = 0; i < b.bid_count; ++i) {
        b.bids[i].price = t.bid - i * 0.01;
        b.bids[i].size  = per_bid;
    }
    for (int i = 0; i < b.ask_count; ++i) {
        b.asks[i].price = t.ask + i * 0.01;
        b.asks[i].size  = per_ask;
    }
    return b;
}

// ----------------------------------------------------------------------------
// M1 bar builder + ATR14
// ----------------------------------------------------------------------------
struct M1Bar {
    double open=0, high=0, low=1e9, close=0;
    int64_t ts_open_ms=0;
    bool valid=false;
};

class BarBuilder {
public:
    M1Bar prev_prev, prev, cur;
    int64_t cur_minute = -1;
    std::deque<double> tr_window;  // 14 true-ranges
    double atr14 = 0;
    bool has_prev_prev() const { return prev_prev.valid; }

    void on_tick(int64_t ts_ms, double mid) {
        const int64_t m = ts_ms / 60000;
        if (m != cur_minute) {
            // close current bar
            if (cur.valid) {
                // compute true range
                if (prev.valid) {
                    const double tr = std::max({cur.high - cur.low,
                                                std::fabs(cur.high - prev.close),
                                                std::fabs(cur.low  - prev.close)});
                    tr_window.push_back(tr);
                    if ((int)tr_window.size() > 14) tr_window.pop_front();
                    double s = 0;
                    for (double v : tr_window) s += v;
                    atr14 = (tr_window.size() > 0) ? s / tr_window.size() : 0;
                }
                prev_prev = prev;
                prev = cur;
            }
            cur = M1Bar{mid, mid, mid, mid, ts_ms, true};
            cur_minute = m;
        } else {
            if (mid > cur.high) cur.high = mid;
            if (mid < cur.low)  cur.low  = mid;
            cur.close = mid;
        }
    }
};

// ----------------------------------------------------------------------------
// Run engine over one file
// ----------------------------------------------------------------------------
struct Result {
    int trades = 0;
    int wins   = 0;
    int losses = 0;
    double pnl = 0;
    double max_dd = 0;
    double cum_min = 0, cum_max = 0;
    std::map<std::string,int>    exit_count;
    std::map<std::string,double> exit_pnl;
    std::vector<omega::TradeRecord> all;
};

static void run_file(const std::string& path,
                     omega::CandleFlowEngine& cfe,
                     Result& res) {
    std::ifstream f(path);
    if (!f) { std::cerr << "cannot open " << path << "\n"; return; }
    std::string line;
    std::getline(f, line);  // header

    BarBuilder bars;
    int64_t last_log_min = -1;
    long ticks = 0;

    while (std::getline(f, line)) {
        Tick t;
        if (!parse_tick(line, t)) continue;
        if (t.watchdog_dead) continue;
        ++ticks;

        const double mid = (t.bid + t.ask) * 0.5;
        bars.on_tick(t.ts_ms, mid);

        // Build DOMSnap
        omega::CandleFlowEngine::DOMSnap dom;
        dom.bid_count = t.depth_bid_levels;
        dom.ask_count = t.depth_ask_levels;
        dom.bid_vol   = t.l2_bid_vol;
        dom.ask_vol   = t.l2_ask_vol;
        // Use the recorded l2_imb directly (level-count imbalance, 0..1)
        dom.l2_imb    = t.l2_imb;
        // vacuum / wall flags: simple thresholds
        dom.vacuum_ask = (t.depth_ask_levels < 2);
        dom.vacuum_bid = (t.depth_bid_levels < 2);
        dom.wall_above = (t.depth_ask_levels >= 4 && t.l2_ask_vol > 3.0 * t.l2_bid_vol);
        dom.wall_below = (t.depth_bid_levels >= 4 && t.l2_bid_vol > 3.0 * t.l2_ask_vol);

        // Build BarSnap from completed bars
        omega::CandleFlowEngine::BarSnap bar;
        if (bars.has_prev_prev()) {
            bar.open      = bars.prev.open;
            bar.high      = bars.prev.high;
            bar.low       = bars.prev.low;
            bar.close     = bars.prev.close;
            bar.prev_high = bars.prev_prev.high;
            bar.prev_low  = bars.prev_prev.low;
            bar.valid     = true;
        }

        // Estimate tick rate (ticks per second over the last minute)
        double tick_rate = 0;  // OK to leave 0 -- HMM uses but degrades gracefully

        const double atr = std::max(2.0, bars.atr14);

        // Closed-trade callback: record stats
        auto on_close = [&](const omega::TradeRecord& tr_in) {
            // Apply lifecycle scaling: live engine hands pnl as ppts*size; lifecycle * 100
            omega::TradeRecord tr = tr_in;
            const double mult = 100.0;  // XAUUSD tick-value multiplier
            tr.pnl *= mult; tr.mfe *= mult; tr.mae *= mult;
            omega::apply_realistic_costs(tr, 3.0, mult);
            res.trades++;
            if (tr.net_pnl > 0) res.wins++; else res.losses++;
            res.pnl += tr.net_pnl;
            res.cum_min = std::min(res.cum_min, res.pnl);
            res.cum_max = std::max(res.cum_max, res.pnl);
            res.max_dd  = std::max(res.max_dd,  res.cum_max - res.pnl);
            res.exit_count[tr.exitReason]++;
            res.exit_pnl[tr.exitReason] += tr.net_pnl;
            res.all.push_back(tr);
        };

        cfe.on_tick(t.bid, t.ask, bar, dom, t.ts_ms, atr, on_close,
                    t.ewm_drift, tick_rate);
    }
    std::cerr << "  " << path << ": " << ticks << " ticks processed\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: cfe_faithful_bt <l2_ticks_*.csv> [more files]\n";
        return 1;
    }

    omega::CandleFlowEngine cfe;
    cfe.shadow_mode = true;

    Result total;
    std::cerr << "running CFE faithful backtest on " << (argc-1) << " files...\n";
    for (int i = 1; i < argc; ++i) {
        run_file(argv[i], cfe, total);
    }

    std::cout << "\n=== CFE FAITHFUL BACKTEST -- " << (argc-1) << " day(s) ===\n";
    std::cout << "Trades       : " << total.trades << "\n";
    std::cout << "Wins         : " << total.wins << "\n";
    std::cout << "Losses       : " << total.losses << "\n";
    std::cout << "Win rate     : " << std::fixed << std::setprecision(1)
              << (total.trades > 0 ? 100.0 * total.wins / total.trades : 0.0) << " %\n";
    std::cout << "Net PnL      : $" << std::fixed << std::setprecision(2) << total.pnl << "\n";
    std::cout << "Avg / trade  : $" << std::fixed << std::setprecision(2)
              << (total.trades > 0 ? total.pnl / total.trades : 0.0) << "\n";
    std::cout << "Max DD       : $" << std::fixed << std::setprecision(2) << total.max_dd << "\n";
    std::cout << "\nExit reasons:\n";
    for (auto& kv : total.exit_count) {
        std::cout << "  " << std::setw(14) << std::left << kv.first
                  << " n=" << std::setw(5) << kv.second
                  << " pnl=$" << std::fixed << std::setprecision(2)
                  << total.exit_pnl[kv.first] << "\n";
    }

    // Dump all trades to CSV for downstream analysis
    std::ofstream out("cfe_faithful_bt_trades.csv");
    out << "id,symbol,side,engine,exit_reason,entry,exit,sl,tp,size,gross_pnl,net_pnl,mfe,mae,entry_ts,exit_ts\n";
    for (auto& tr : total.all) {
        out << tr.id << "," << tr.symbol << "," << tr.side << ","
            << tr.engine << "," << tr.exitReason << ","
            << std::fixed << std::setprecision(4)
            << tr.entryPrice << "," << tr.exitPrice << ","
            << tr.sl << "," << tr.tp << "," << tr.size << ","
            << tr.pnl << "," << tr.net_pnl << ","
            << tr.mfe << "," << tr.mae << ","
            << tr.entryTs << "," << tr.exitTs << "\n";
    }
    return 0;
}

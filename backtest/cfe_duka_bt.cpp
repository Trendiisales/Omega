// =============================================================================
// cfe_duka_bt.cpp -- CandleFlowEngine backtester for Dukascopy XAUUSD CSVs
// 2026-04-29 audit -- 26-month validation harness
//
// Dukascopy gold tick CSV typically looks like:
//   2024-03-26 22:00:00.123,2079.450,2079.620,1.0,1.0
//      timestamp                     ,bid    ,ask    ,bidvol,askvol
//
//   ...or with milliseconds in epoch form:
//   1711490400123,2079.450,2079.620,1.0,1.0
//
// This harness auto-detects the timestamp column format. It synthesises a
// neutral L2 snapshot per tick (imbalance=0.5, level-count=5/5) so the
// IMB_EXIT and DOM-confirm code paths in CandleFlowEngine effectively
// degrade to no-op gates. The ENTRY signal (RSI + expansion candle), the
// SL/PARTIAL_TP/TRAIL_SL/MAX_HOLD exits, the TOD-deadzone gate and the
// HMM regime gate all still run normally. This is intentional: it tests
// whether the engine has edge from candle structure alone, without DOM
// signal, on the longest available history.
//
// Build (Mac):
//   g++ -O2 -std=c++17 -I<repo>/include -o cfe_duka_bt cfe_duka_bt.cpp
//
// Run:
//   ./cfe_duka_bt /path/to/XAUUSD_2024-03_2026-04_combined.csv
//
// Output:
//   stdout: summary stats (trades, WR, net P&L, exit-reason breakdown, by month)
//   cfe_duka_bt_trades.csv: every closed trade for downstream analysis
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

// Use the REAL repo headers
#include "OmegaFIX.hpp"          // L2Level, L2Book + microstructure methods
#include "OmegaTradeLedger.hpp"  // omega::TradeRecord + apply_realistic_costs
#include "GoldHMM.hpp"
#include "CandleFlowEngine.hpp"

// ----------------------------------------------------------------------------
// Dukascopy parser (handles both ISO timestamp and epoch-ms formats)
// ----------------------------------------------------------------------------
struct DukaTick {
    int64_t ts_ms = 0;
    double  bid   = 0.0;
    double  ask   = 0.0;
};

static int64_t iso_to_epoch_ms(const std::string& ts) {
    // Formats supported:
    //   2024-03-26 22:00:00
    //   2024-03-26 22:00:00.123
    //   2024-03-26T22:00:00Z
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
    // timegm = UTC mktime (Linux/Mac extension)
    int64_t epoch_s = (int64_t)timegm(&t);
    return epoch_s * 1000 + ms;
}

static bool parse_duka(const std::string& line, DukaTick& t) {
    if (line.empty()) return false;
    // Split by comma
    std::vector<std::string> f;
    std::string cur;
    for (char c : line) {
        if (c == ',') { f.push_back(cur); cur.clear(); }
        else if (c != '\r') cur.push_back(c);
    }
    f.push_back(cur);
    if (f.size() < 3) return false;

    // First field: timestamp (epoch-ms numeric OR ISO string)
    try {
        // Try numeric first (will throw if non-numeric)
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
            // Heuristic: if value > 1e12, it's epoch-ms; if 1e9..1e10 it's epoch-sec.
            if (v > 1e12)      t.ts_ms = (int64_t)v;
            else if (v > 1e9)  t.ts_ms = (int64_t)(v * 1000.0);
            else               t.ts_ms = (int64_t)v;  // already ms
        } else {
            t.ts_ms = iso_to_epoch_ms(ts);
        }
        t.bid = std::stod(f[1]);
        t.ask = std::stod(f[2]);
    } catch (...) {
        return false;
    }
    return t.bid > 0.0 && t.ask > 0.0 && t.ask >= t.bid && t.ts_ms > 0;
}

// Build neutral L2Book (CFE.build_dom is permissive when level counts are equal)
static omega::CandleFlowEngine::DOMSnap make_neutral_dom() {
    omega::CandleFlowEngine::DOMSnap d{};
    d.bid_count = 5;
    d.ask_count = 5;
    d.bid_vol   = 5.0;
    d.ask_vol   = 5.0;
    d.l2_imb    = 0.5;
    d.vacuum_ask = false;
    d.vacuum_bid = false;
    d.wall_above = false;
    d.wall_below = false;
    d.prev_bid_count = 5;
    d.prev_ask_count = 5;
    d.prev_bid_vol   = 5.0;
    d.prev_ask_vol   = 5.0;
    return d;
}

// ----------------------------------------------------------------------------
// M1 bar builder + ATR14 + ewm_drift estimator
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
    std::deque<double> tr_window;
    double atr14 = 0.0;

    bool has_prev_prev() const { return prev_prev.valid; }

    void on_tick(int64_t ts_ms, double mid) {
        const int64_t m = ts_ms / 60000;
        if (m != cur_minute) {
            if (cur.valid) {
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

// EWM drift: alpha=0.05 fast EWM of (mid - mid_prev) per second-aggregated tick
class DriftEst {
public:
    double drift = 0.0;
    double last_mid = 0.0;
    int64_t last_ms = 0;
    static constexpr double ALPHA = 0.05;
    void on_tick(int64_t ts_ms, double mid) {
        if (last_mid > 0.0) {
            // points per second
            const double dt = std::max(1.0, double(ts_ms - last_ms) / 1000.0);
            const double rate = (mid - last_mid) / dt;
            drift = ALPHA * rate + (1.0 - ALPHA) * drift;
        }
        last_mid = mid;
        last_ms  = ts_ms;
    }
};

// ----------------------------------------------------------------------------
// Run
// ----------------------------------------------------------------------------
struct MonthBucket {
    int    n      = 0;
    int    wins   = 0;
    double pnl    = 0.0;
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: cfe_duka_bt <duka_xauusd.csv>\n";
        std::cerr << "Outputs:\n";
        std::cerr << "  stdout : summary\n";
        std::cerr << "  cfe_duka_bt_trades.csv : per-trade records\n";
        return 1;
    }

    omega::CandleFlowEngine cfe;
    cfe.shadow_mode = true;

    BarBuilder bars;
    DriftEst   drift;

    int trades = 0, wins = 0, losses = 0;
    double pnl_total = 0.0;
    double cum_min = 0.0, cum_max = 0.0, max_dd = 0.0;
    std::map<std::string, int>    exit_n;
    std::map<std::string, double> exit_pnl;
    std::map<std::string, MonthBucket> by_month;

    std::ofstream out("cfe_duka_bt_trades.csv");
    out << "id,symbol,side,exit_reason,entry,exit,sl,tp,size,gross_pnl,net_pnl,mfe,mae,entry_ts,exit_ts\n";

    auto on_close = [&](const omega::TradeRecord& tr_in) {
        omega::TradeRecord tr = tr_in;
        const double mult = 100.0;  // XAUUSD tick-value
        tr.pnl *= mult; tr.mfe *= mult; tr.mae *= mult;
        omega::apply_realistic_costs(tr, 3.0, mult);
        trades++;
        if (tr.net_pnl > 0) wins++; else losses++;
        pnl_total += tr.net_pnl;
        cum_min = std::min(cum_min, pnl_total);
        cum_max = std::max(cum_max, pnl_total);
        max_dd  = std::max(max_dd,  cum_max - pnl_total);
        exit_n[tr.exitReason]++;
        exit_pnl[tr.exitReason] += tr.net_pnl;

        // Bucket by yyyy-mm
        std::time_t t = (std::time_t)tr.entryTs;
        std::tm* gm = gmtime(&t);
        char ym[16]; std::snprintf(ym, sizeof(ym), "%04d-%02d", gm->tm_year+1900, gm->tm_mon+1);
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
    if (!f) { std::cerr << "cannot open " << argv[1] << "\n"; return 2; }

    std::string line;
    long ticks = 0;
    long last_progress = 0;
    auto t_start = std::chrono::steady_clock::now();
    std::cerr << "reading " << argv[1] << "...\n";

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        // Skip header line if present
        if (ticks == 0 && (line.find("timestamp") != std::string::npos
                        || line.find("bid")       != std::string::npos
                        || line.find("Time")      != std::string::npos)) continue;
        DukaTick t;
        if (!parse_duka(line, t)) continue;
        ++ticks;

        const double mid = (t.bid + t.ask) * 0.5;
        bars.on_tick(t.ts_ms, mid);
        drift.on_tick(t.ts_ms, mid);

        omega::CandleFlowEngine::DOMSnap dom = make_neutral_dom();

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
        const double atr = std::max(2.0, bars.atr14);
        const double tick_rate = 0.0;
        cfe.on_tick(t.bid, t.ask, bar, dom, t.ts_ms, atr, on_close,
                    drift.drift, tick_rate);

        // Progress every 5M ticks
        if (ticks - last_progress >= 5000000) {
            last_progress = ticks;
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - t_start).count();
            std::cerr << "  " << ticks << " ticks, "
                      << trades << " trades, $" << std::fixed << std::setprecision(2)
                      << pnl_total << " (" << elapsed << "s elapsed)\n";
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t_start).count();
    std::cout << "\n=== CFE DUKASCOPY BACKTEST ===\n";
    std::cout << "File         : " << argv[1] << "\n";
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
        std::cout << "  " << kv.first << "  n=" << std::setw(5) << m.n
                  << "  WR=" << std::setw(5) << std::setprecision(1)
                  << (m.n > 0 ? 100.0 * m.wins / m.n : 0.0) << "%"
                  << "  pnl=$" << std::setprecision(2) << m.pnl << "\n";
    }
    std::cout << "\nWrote per-trade records to cfe_duka_bt_trades.csv\n";
    return 0;
}

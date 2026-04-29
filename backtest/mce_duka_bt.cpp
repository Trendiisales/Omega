// =============================================================================
// mce_duka_bt.cpp -- MacroCrashEngine backtester for Dukascopy XAUUSD CSVs
// 2026-04-29 audit -- 26-month validation harness for MCE post-fixes-4 (C-1, C-3)
//
// PURPOSE
//   Validate that the post-audit MacroCrashEngine (commit 675f063f, audit-fixes-4)
//   does not phantom-fire under sustained directional pressure on data the live
//   shadow has never seen. Tests the C-1 fix (m_consec_sl counts DOLLAR_STOP)
//   and C-3 fix (force_close updates pos.mae) at scale across 26 months.
//
// DESIGN
//   Built from the same template as backtest/cfe_duka_bt.cpp. Tick parser is
//   identical (auto-detects ISO timestamp vs epoch-ms, ask-first vs bid-first
//   column order). The MCE engine itself runs unmodified -- this harness only
//   computes the indicator inputs MCE needs and feeds them via on_tick.
//
// INDICATORS COMPUTED IN-HARNESS
//   atr           -- M1 EMA-20 of (high-low) per minute bar
//   ewm_drift     -- mid - EWM(mid, 30s halflife). Matches MCE design comment.
//   vol_ratio     -- EWM(|drift|, 5s halflife) / EWM(|drift|, 60s halflife)
//   rsi14         -- M1 RSI(14) over last 15 bar closes (Wilder)
//   session_slot  -- 0-6 from UTC hour. MCE only cares about slot=6 (Asia,
//                    22:00-05:00 UTC) for its lower thresholds.
//   expansion_regime -- vol_ratio > 2.0 && |drift| > 2.0 (mce_sweep heuristic;
//                    matches the live MacroRegimeDetector contract by intent
//                    even though we don't load the full HMM here)
//   DOM inputs    -- book_slope=0, vacuum=false, microprice_bias=0. The
//                    engine's DOM-relax path stays inactive, which is the
//                    conservative choice for a backtest with no L2 feed.
//
// BRACKETTRENDSTATE
//   MCE consults bracket_trend_bias("XAUUSD") at line 407 of MacroCrashEngine.hpp.
//   By design (see BracketTrendState.hpp:16-22 comment block) this returns 0
//   in any TU that doesn't populate g_bracket_trend, which is the correct
//   historical-simulation behaviour. We rely on that here -- no plumbing needed.
//
// TRADE EMISSION
//   MCE has two callbacks:
//     on_close          -- partial fills (BRACKET_TP, PARTIAL_1, PARTIAL_2,
//                          PYRAMID_CLOSE). We register a no-op for it.
//     on_trade_record   -- final close (SL_HIT / TRAIL_STOP / MAX_HOLD /
//                          DOLLAR_STOP / WEEKEND_CLOSE). This is the canonical
//                          per-trade record path. We aggregate and CSV-write
//                          here, mirroring cfe_duka_bt's on_close lambda.
//   The engine's banked_usd (from partials) is rolled into the final
//   TradeRecord by _close_all_locked(); we do not need to add it ourselves.
//
// COSTS
//   omega::apply_realistic_costs(tr, 3.0, 100.0) -- same model and parameters
//   as cfe_duka_bt for apples-to-apples comparison.
//
// SHADOW MODE
//   mce.shadow_mode = true (default). Engine still emits on_trade_record in
//   shadow mode (MCE source line 997: "fires in BOTH shadow and live mode").
//   pyramid_shadow = true keeps the safe-pyramid path silent at the order
//   layer; the BT only counts the BASE position trade record. This matches
//   how the live shadow has been running and is the correct comparison.
//
// BUILD (Mac, from repo root)
//   g++ -O2 -std=c++17 -I include -o mce_duka_bt backtest/mce_duka_bt.cpp
//
// RUN
//   ./mce_duka_bt /Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv
//
// OUTPUT
//   stdout: summary stats (trades, WR, net P&L, exit-reason breakdown, by month)
//   mce_duka_bt_trades.csv: every closed trade for downstream analysis
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
#include "MacroCrashEngine.hpp"

// ----------------------------------------------------------------------------
// Dukascopy parser (identical to cfe_duka_bt -- auto-detects timestamp format
// and ask-first vs bid-first column order)
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
// Indicators required by MacroCrashEngine.on_tick
//   atr, ewm_drift, vol_ratio, rsi14, session_slot, expansion_regime
// Mirrors mce_sweep.cpp's update() but adds RSI14 from M1 closes.
// ----------------------------------------------------------------------------

// Time-based EWM alpha for halflife in ms. dt_ms is time since last tick.
static inline double tba(double dt_ms, double hl_ms) {
    if (hl_ms <= 0.0) return 1.0;
    return 1.0 - std::exp(-dt_ms * 0.693147 / hl_ms);
}

struct MceIndicators {
    // Drift -- 30s halflife EWM of mid, drift = mid - ewm
    double  ewm_mid    = 0.0;
    double  ewm_drift  = 0.0;
    bool    init_drift = false;

    // Vol ratio -- EWM(|drift|,5s) / EWM(|drift|,60s)
    double  v_short   = 0.0;
    double  v_long    = 0.0;
    double  vol_ratio = 1.0;

    // M1 bar -- ATR(EMA-20) and RSI14 sources
    int64_t b_min     = -1;
    double  b_o = 0, b_h = 0, b_l = 0, b_c = 0;
    bool    bar_open  = false;
    double  atr       = 0.0;

    // RSI14 over last 15 bar closes (Wilder simple-average init)
    std::deque<double> rsi_closes;
    double  rsi       = 50.0;

    int64_t prev_ms   = 0;

    // Reset state on inter-tick gap > 1hr (weekend / data gap boundary)
    static constexpr int64_t GAP_MS = 3600000LL;

    void on_tick(double bid, double ask, int64_t ts_ms) {
        const double mid = (bid + ask) * 0.5;

        if (prev_ms > 0 && (ts_ms - prev_ms) > GAP_MS) {
            ewm_mid = mid; ewm_drift = 0.0;
            v_short = 0.0; v_long = 0.0; vol_ratio = 1.0;
        }
        const double dt = (prev_ms > 0) ? (double)(ts_ms - prev_ms) : 100.0;
        prev_ms = ts_ms;

        // Drift
        if (!init_drift) {
            ewm_mid    = mid;
            ewm_drift  = 0.0;
            init_drift = true;
        } else {
            const double a30 = tba(dt, 30000.0);
            ewm_mid   = a30 * mid + (1.0 - a30) * ewm_mid;
            ewm_drift = mid - ewm_mid;
        }

        // Vol ratio
        const double am  = std::fabs(ewm_drift);
        const double a5  = tba(dt, 5000.0);
        const double a60 = tba(dt, 60000.0);
        v_short = a5  * am + (1.0 - a5)  * v_short;
        v_long  = a60 * am + (1.0 - a60) * v_long;
        vol_ratio = (v_long > 1e-9) ? (v_short / v_long) : 1.0;

        // M1 bar boundary
        const int64_t bm = ts_ms / 60000LL;
        if (!bar_open) {
            b_o = b_h = b_l = b_c = mid;
            b_min    = bm;
            bar_open = true;
        } else if (bm != b_min) {
            // Close previous bar -- update ATR and RSI
            const double rng = b_h - b_l;
            if (atr == 0.0) atr = rng;
            else {
                const double a = 2.0 / 21.0;          // EMA-20
                atr = a * rng + (1.0 - a) * atr;
            }

            rsi_closes.push_back(b_c);
            while (rsi_closes.size() > 15) rsi_closes.pop_front();
            if (rsi_closes.size() == 15) {
                double up = 0.0, dn = 0.0;
                for (size_t i = 1; i < rsi_closes.size(); ++i) {
                    const double d = rsi_closes[i] - rsi_closes[i-1];
                    if (d > 0) up += d;
                    else       dn -= d;
                }
                up /= 14.0; dn /= 14.0;
                if (dn < 1e-9) {
                    rsi = 100.0;
                } else {
                    const double rs = up / dn;
                    rsi = 100.0 - (100.0 / (1.0 + rs));
                }
            }

            // Open the new bar
            b_o = b_h = b_l = b_c = mid;
            b_min = bm;
        } else {
            if (mid > b_h) b_h = mid;
            if (mid < b_l) b_l = mid;
            b_c = mid;
        }
    }

    bool expansion_regime() const {
        return std::fabs(ewm_drift) > 2.0 && vol_ratio > 2.0;
    }
};

// MCE differentiates Asia (slot=6, 22:00-05:00 UTC) from non-Asia. We
// intentionally collapse the rest into slot=1 -- MCE source only branches on
// (session_slot == 6), so the precise non-Asia bucket value does not matter.
static int session_slot_for(int64_t ts_ms) {
    std::time_t t = (std::time_t)(ts_ms / 1000);
    std::tm* gm = std::gmtime(&t);
    if (!gm) return 1;
    const int h = gm->tm_hour;
    if (h >= 22 || h < 5) return 6;
    return 1;
}

// ----------------------------------------------------------------------------
// Run
// ----------------------------------------------------------------------------
struct MonthBucket {
    int    n    = 0;
    int    wins = 0;
    double pnl  = 0.0;
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: mce_duka_bt <duka_xauusd.csv>\n";
        std::cerr << "Outputs:\n";
        std::cerr << "  stdout : summary\n";
        std::cerr << "  mce_duka_bt_trades.csv : per-trade records\n";
        return 1;
    }

    omega::MacroCrashEngine mce;
    mce.shadow_mode    = true;   // safe -- BT does not place real orders
    mce.pyramid_shadow = true;   // suppress pyramid live emission
    mce.enabled        = true;

    MceIndicators ind;

    int trades = 0, wins = 0, losses = 0;
    double pnl_total = 0.0;
    double cum_min = 0.0, cum_max = 0.0, max_dd = 0.0;
    std::map<std::string, int>    exit_n;
    std::map<std::string, double> exit_pnl;
    std::map<std::string, MonthBucket> by_month;

    std::ofstream out("mce_duka_bt_trades.csv");
    out << "id,symbol,side,exit_reason,entry,exit,sl,tp,size,gross_pnl,net_pnl,mfe,mae,entry_ts,exit_ts\n";

    // Final-close trade record callback. Mirrors cfe_duka_bt's on_close lambda
    // semantics: convert raw points*lots to USD, apply realistic costs, bucket
    // by month + exit-reason, write CSV row.
    mce.on_trade_record = [&](const omega::TradeRecord& tr_in) {
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

    // Partial-fill hook -- silent. The engine rolls partial banked P&L into the
    // final TradeRecord via pos.banked_usd, so aggregating partials separately
    // would double-count. Wired only to keep the engine quiet when it tries
    // to invoke on_close.
    mce.on_close = [&](double, bool, double, const std::string&) {};

    std::ifstream f(argv[1]);
    if (!f) {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 2;
    }

    // Silence MCE's per-tick stdout chatter (entry-trigger logs, gate-block
    // diagnostics, weekend-close logs). Restored before printing the summary.
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
        ++ticks;

        ind.on_tick(t.bid, t.ask, t.ts_ms);

        // Warmup: wait until ATR has accumulated. Matches mce_sweep.cpp:179.
        if (ind.atr < 0.5) continue;

        const int  slot    = session_slot_for(t.ts_ms);
        const bool exp_reg = ind.expansion_regime();

        // No L2 in BT -- pass DOM defaults. The engine's DOM-relax path stays
        // off (book_slope below threshold, vacuums false, microprice_bias=0).
        mce.on_tick(t.bid, t.ask,
                    ind.atr, ind.vol_ratio, ind.ewm_drift,
                    exp_reg, t.ts_ms,
                    /*book_slope*/      0.0,
                    /*vacuum_ask*/      false,
                    /*vacuum_bid*/      false,
                    /*microprice_bias*/ 0.0,
                    /*rsi14*/           ind.rsi,
                    /*session_slot*/    slot);

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

    // Restore cout for the summary
    std::cout.rdbuf(saved_cout);

    std::cout << "\n=== MCE DUKASCOPY BACKTEST ===\n";
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
        std::cout << "  " << kv.first
                  << "  n=" << std::setw(5) << m.n
                  << "  WR=" << std::setw(5) << std::setprecision(1)
                  << (m.n > 0 ? 100.0 * m.wins / m.n : 0.0) << "%"
                  << "  pnl=$" << std::setprecision(2) << m.pnl << "\n";
    }

    std::cout << "\nWrote per-trade records to mce_duka_bt_trades.csv\n";
    return 0;
}

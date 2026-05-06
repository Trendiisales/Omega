// =============================================================================
// tsmom_bt.cpp -- TsmomCell stage-trail validation harness.
// 2026-05-07 (S8): drives 5 long-only cells (H1, H2, H4, H6, D1) on the
// canonical phase1 H1 warmup CSV. Compares baseline (no trail) vs the new
// stage-trail (TSMOM_TRAIL_ENABLED=true) over the same bar tape.
//
// BUILD (Mac, from repo root):
//   baseline:    g++ -O2 -std=c++17 -DOMEGA_BACKTEST -DTSMOM_TRAIL_ENABLED=false \
//                    -I include -o /tmp/tsmom_bt_off backtest/tsmom_bt.cpp
//   trail-on:    g++ -O2 -std=c++17 -DOMEGA_BACKTEST -DTSMOM_TRAIL_ENABLED=true \
//                    -I include -o /tmp/tsmom_bt_on backtest/tsmom_bt.cpp
//
// RUN:
//   /tmp/tsmom_bt_off phase1/signal_discovery/tsmom_warmup_H1.csv
//   /tmp/tsmom_bt_on  phase1/signal_discovery/tsmom_warmup_H1.csv
//
// OUTPUT: per-cell trade count, WR, net PnL, exit-reason breakdown.
// =============================================================================
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <map>
#include <string>
#include <sstream>
#include <vector>

#include "OmegaTradeLedger.hpp"
#include "TsmomEngine.hpp"

struct CellResult {
    int n        = 0;
    int wins     = 0;
    double net   = 0.0;
    std::map<std::string, int>    by_reason_n;
    std::map<std::string, double> by_reason_pnl;
};

static const double USD_PER_PT = 1.0;   // XAUUSD at 0.01 lot ≈ $1/pt
static const double COST_PER_TRADE = 0.22;  // round-trip avg

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: tsmom_bt <h1_csv>\n";
        return 1;
    }

    std::ifstream f(argv[1]);
    if (!f) { std::cerr << "Cannot open " << argv[1] << "\n"; return 1; }

    std::string line;
    std::getline(f, line);  // header

    // 5 cells: H1, H2, H4, H6, D1 (all long)
    omega::TsmomCell c_h1, c_h2, c_h4, c_h6, c_d1;
    auto setup = [](omega::TsmomCell& c, const std::string& tf,
                    const std::string& cell_id, int dir) {
        c.shadow_mode             = true;
        c.enabled                 = true;
        c.lookback                = 20;
        c.hold_bars               = 12;
        c.max_positions_per_cell  = 1;
        c.cooldown_bars           = 0;
        c.hard_sl_atr             = 3.0;
        c.mae_exit_atr            = 2.0;
        c.direction               = dir;
        c.timeframe               = tf;
        c.symbol                  = "XAUUSD";
        c.cell_id                 = cell_id;
    };
    setup(c_h1, "H1", "Tsmom_H1_long", 1);
    setup(c_h2, "H2", "Tsmom_H2_long", 1);
    setup(c_h4, "H4", "Tsmom_H4_long", 1);
    setup(c_h6, "H6", "Tsmom_H6_long", 1);
    setup(c_d1, "D1", "Tsmom_D1_long", 1);

    // Per-TF synthesizers for H2/H4/H6/D1 (all driven by H1 closes).
    omega::TsmomBarSynth s_h2{}, s_h4{}, s_h6{}, s_d1{};
    s_h2.stride = 2; s_h4.stride = 4; s_h6.stride = 6; s_d1.stride = 24;

    omega::TsmomATR14 atr_h1, atr_h2, atr_h4, atr_h6, atr_d1;

    std::map<std::string, CellResult> results;

    auto on_close = [&](const omega::TradeRecord& tr) {
        const double pnl_pts = (tr.side == "LONG")
            ? (tr.exitPrice - tr.entryPrice)
            : (tr.entryPrice - tr.exitPrice);
        const double pnl_usd = pnl_pts * USD_PER_PT - COST_PER_TRADE;
        auto& r = results[tr.engine];
        ++r.n;
        if (pnl_usd > 0.0) ++r.wins;
        r.net += pnl_usd;
        ++r.by_reason_n[tr.exitReason];
        r.by_reason_pnl[tr.exitReason] += pnl_usd;
    };

    int nbars = 0;

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::vector<std::string> fld;
        std::string cur; for (char c : line) {
            if (c == ',') { fld.push_back(cur); cur.clear(); }
            else if (c != '\r') cur.push_back(c);
        }
        fld.push_back(cur);
        if (fld.size() < 5) continue;

        omega::TsmomBar h1{};
        try {
            h1.bar_start_ms = std::stoll(fld[0]);
            h1.open  = std::stod(fld[1]);
            h1.high  = std::stod(fld[2]);
            h1.low   = std::stod(fld[3]);
            h1.close = std::stod(fld[4]);
        } catch (...) { continue; }

        const double bid  = h1.close - 0.11;  // half-spread approx for XAUUSD
        const double ask  = h1.close + 0.11;
        const int64_t now_ms = h1.bar_start_ms + 60 * 60 * 1000;

        // Feed H1 cell directly.
        atr_h1.on_bar(h1);
        if (atr_h1.ready())
            c_h1.on_bar(h1, bid, ask, atr_h1.value(), now_ms, 0.01, on_close);

        // Synth H2/H4/H6/D1 from H1 close stream.
        s_h2.on_h1_bar(h1, [&](const omega::TsmomBar& b) {
            atr_h2.on_bar(b);
            if (atr_h2.ready())
                c_h2.on_bar(b, bid, ask, atr_h2.value(), now_ms, 0.01, on_close);
        });
        s_h4.on_h1_bar(h1, [&](const omega::TsmomBar& b) {
            atr_h4.on_bar(b);
            if (atr_h4.ready())
                c_h4.on_bar(b, bid, ask, atr_h4.value(), now_ms, 0.01, on_close);
        });
        s_h6.on_h1_bar(h1, [&](const omega::TsmomBar& b) {
            atr_h6.on_bar(b);
            if (atr_h6.ready())
                c_h6.on_bar(b, bid, ask, atr_h6.value(), now_ms, 0.01, on_close);
        });
        s_d1.on_h1_bar(h1, [&](const omega::TsmomBar& b) {
            atr_d1.on_bar(b);
            if (atr_d1.ready())
                c_d1.on_bar(b, bid, ask, atr_d1.value(), now_ms, 0.01, on_close);
        });

        ++nbars;
    }

    // ---- Report ----
    std::cout << "\n=== TSMOM BACKTEST ===\n";
    std::cout << "Input  : " << argv[1] << "\n";
    std::cout << "Bars   : " << nbars << "\n";
    std::cout << "Trail  : "
#ifdef TSMOM_TRAIL_ENABLED
              << (TSMOM_TRAIL_ENABLED ? "ENABLED" : "DISABLED")
#else
              << "(default)"
#endif
              << "\n\n";

    std::cout << std::left << std::setw(20) << "cell"
              << std::right << std::setw(8) << "n"
              << std::setw(7) << "wins"
              << std::setw(7) << "WR%"
              << std::setw(12) << "net_USD"
              << "  reasons\n";
    std::cout << std::string(80, '-') << "\n";

    int    agg_n = 0, agg_w = 0;
    double agg_net = 0.0;
    std::map<std::string, int>    agg_reasons_n;
    std::map<std::string, double> agg_reasons_pnl;

    for (const auto& kv : results) {
        const auto& r = kv.second;
        std::cout << std::left << std::setw(20) << kv.first
                  << std::right << std::setw(8) << r.n
                  << std::setw(7) << r.wins
                  << std::setw(7) << std::fixed << std::setprecision(1)
                  << (r.n > 0 ? 100.0 * r.wins / r.n : 0.0)
                  << std::setw(12) << std::fixed << std::setprecision(2)
                  << r.net << "  ";
        for (const auto& rk : r.by_reason_n) {
            std::cout << rk.first << "=" << rk.second
                      << "($" << std::fixed << std::setprecision(2)
                      << r.by_reason_pnl.at(rk.first) << ") ";
            agg_reasons_n[rk.first]   += rk.second;
            agg_reasons_pnl[rk.first] += r.by_reason_pnl.at(rk.first);
        }
        std::cout << "\n";
        agg_n   += r.n;
        agg_w   += r.wins;
        agg_net += r.net;
    }

    std::cout << std::string(80, '-') << "\n";
    std::cout << std::left << std::setw(20) << "AGGREGATE"
              << std::right << std::setw(8) << agg_n
              << std::setw(7) << agg_w
              << std::setw(7) << std::fixed << std::setprecision(1)
              << (agg_n > 0 ? 100.0 * agg_w / agg_n : 0.0)
              << std::setw(12) << std::fixed << std::setprecision(2)
              << agg_net << "  ";
    for (const auto& rk : agg_reasons_n) {
        std::cout << rk.first << "=" << rk.second
                  << "($" << std::fixed << std::setprecision(2)
                  << agg_reasons_pnl[rk.first] << ") ";
    }
    std::cout << "\n\n";

    return 0;
}

// gold_bull_trend_parity_bt.cpp — PARITY harness: drive the SHIPPED
// omega::GoldBullTrendGatedEngine over the certification tapes and confirm it
// reproduces the certified per-cell trades (S-2026-07-20). This is the
// "ensure the results are real" check applied to the LIVE engine code, not the
// research harness (the BTC-book parity pattern).
//
// Feed model: read 1-minute bars (goldls::MinuteCsvReader), then feed each bar's
// O,H,L,C as four ticks at the minute timestamp. Direct tick->N-min aggregation
// reproduces the cert's 1m->N-min bars exactly (open=first tick, high=max,
// low=min, close=last). gold_regime() is fed the 1m close once per minute (=the
// cert H1RegimeGate feed). Engine runs bypass_cost_gate=true so it applies the
// cert's own viab gate (not the production ExecutionCostGuard/daily-halt infra
// the cert never modelled). No warmup() — indicators warm in-file like the cert.
//
// Build:
//   clang++ -std=c++20 -O3 -DNDEBUG -Iinclude -I backtest \
//     backtest/gold_bull_trend_parity_bt.cpp -o /tmp/gbt_parity
// Run (per cell):
//   /tmp/gbt_parity --file <tape> --cell DONCH
//   /tmp/gbt_parity --file <tape> --cell EMA
#include "PortfolioGuard.hpp"            // omega::pg (link for the non-bypass branch)
#include "GoldBullTrendGatedEngine.hpp"
#include "gold_ls_harness.hpp"           // goldls::MinuteCsvReader / MinuteBar

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::string file, cell = "DONCH";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&](const char* d) { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(d); };
        if (a == "--file") file = val("");
        else if (a == "--cell") cell = val("DONCH");
    }
    if (file.empty()) { std::fprintf(stderr, "need --file\n"); return 2; }
    const bool want_donch = (cell == "DONCH");

    omega::GoldBullTrendGatedEngine eng;
    eng.enabled = true;
    eng.shadow_mode = true;
    eng.use_regime_gate = true;
    eng.use_sma_gate = true;
    eng.bypass_cost_gate = true;             // parity: cert viab gate, not prod cost infra
    eng.cell_enable_mask = want_donch ? 0x01u : 0x02u;
    eng.init();
    // NOTE: no eng.warmup() — warm in-file exactly like the cert.

    struct Fill { double entry = 0, exit = 0; };
    std::vector<Fill> fills;
    auto cb = [&](const omega::TradeRecord& tr) { fills.push_back({tr.entryPrice, tr.exitPrice}); };

    goldls::MinuteCsvReader reader(file);
    goldls::MinuteBar m;
    uint64_t nbars = 0;
    while (reader.next(m)) {
        if (!m.valid()) continue;
        ++nbars;
        const int64_t ts_ms = m.t * 1000;
        // regime brain: one close-tick per minute (matches the cert H1RegimeGate feed)
        omega::gold_regime().on_tick(m.close, m.close, ts_ms);
        // engine: O,H,L,C as four ticks at the minute ts -> correct N-min OHLC
        eng.on_tick(m.open,  m.open,  ts_ms, cb);
        eng.on_tick(m.high,  m.high,  ts_ms, cb);
        eng.on_tick(m.low,   m.low,   ts_ms, cb);
        eng.on_tick(m.close, m.close, ts_ms, cb);
    }

    // cost applied analytically, identical to the cert emit (5bp RT, half each side)
    const double cost_bp = 5.0;
    auto half = [&](double px) { return (cost_bp * 0.5 / 10000.0) * px; };
    int n = 0, win = 0; double net = 0, gw = 0, gl = 0;
    for (const auto& f : fills) {
        const double ef = f.entry + half(f.entry);   // long buys higher
        const double xf = f.exit  - half(f.exit);    // long sells lower
        const double ret_bp = (xf - ef) / ef * 10000.0;
        ++n; net += ret_bp; if (ret_bp > 0) { ++win; gw += ret_bp; } else gl += -ret_bp;
    }
    const double pf = gl > 1e-9 ? gw / gl : (gw > 0 ? 999.0 : 0.0);
    std::printf("PARITY|cell=%s|file=%s|bars=%llu|n=%d|win=%d|net_bp=%.1f|pf=%.2f\n",
                cell.c_str(), file.c_str(), (unsigned long long)nbars, n, win, net, pf);
    return 0;
}

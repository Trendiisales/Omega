// faithful_bear_recovery_bt.cpp -- drives the REAL CryptoBearRecoveryEngine
// class over the Coinbase hourly corpus (BACKTEST_TRUTH: the faithful arbiter
// drives the real engine, not a re-implementation). Verifies the C++ port
// reproduces the Python study (backtest/crypto_bear_bounce/bear_bounce_bt.py
// --phase final): expect n~20, PF in the 5-10 band, worst <= ~-5%, both
// symbols positive, 2019/2020/2023 carrying the net.
//
// Build:  g++ -O2 -std=c++17 -I../../include -o /tmp/cbr_bt faithful_bear_recovery_bt.cpp
// Run:    /tmp/cbr_bt <datadir-with-BTCUSD_1h.csv,ETHUSD_1h.csv> [risk_pct=0.01]
//
// Feed semantics: per hourly bar the engine sees o,h,l,c in sequence -- the
// same intrabar mark order as the Python harness (mfe from h before the stop
// check at l). Costs applied here on close: 6 bps/side + 10 bps extra slip on
// STOP/BE_FLOOR exits (engine emits gross at stop/floor price).
#include "CryptoBearRecoveryEngine.hpp"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

struct Row { long long ts; double o, h, l, c; };

static std::vector<Row> load_1h(const std::string& fp) {
    std::vector<Row> rows;
    FILE* f = std::fopen(fp.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "missing %s\n", fp.c_str()); return rows; }
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        Row r; double vol;
        if (std::sscanf(line, "%lld,%lf,%lf,%lf,%lf,%lf", &r.ts, &r.o, &r.h, &r.l, &r.c, &vol) >= 5)
            if (r.o > 0 && r.h > 0 && r.l > 0 && r.c > 0) rows.push_back(r);
    }
    std::fclose(f);
    return rows;
}

static int year_of_ms(long long ts_ms) {
    time_t t = (time_t)(ts_ms / 1000);
    struct tm g; gmtime_r(&t, &g);
    return g.tm_year + 1900;
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    const double risk = argc > 2 ? std::atof(argv[2]) : 0.01;
    const double cost = 0.0006, slip = 0.0010;     // 6 bps/side, 10 bps stop slip

    struct Trade { std::string sym; long long tin, tout; double ret, pnl; std::string reason; double mfe; };
    std::vector<Trade> trades;

    for (const char* sym : {"BTCUSD", "ETHUSD"}) {
        auto rows = load_1h(dir + "/" + sym + "_1h.csv");
        if (rows.empty()) return 1;
        omega::CryptoBearRecoveryEngine eng;
        eng.symbol = sym; eng.risk_pct = risk;
        eng.on_close = [&](const omega::CbrClose& t) {
            const bool stopish = std::strcmp(t.reason, "STOP") == 0 || std::strcmp(t.reason, "BE_FLOOR") == 0;
            const double exit_px  = t.exit * (1.0 - cost - (stopish ? slip : 0.0));
            const double entry_px = t.entry * (1.0 + cost);
            Trade tr; tr.sym = t.symbol; tr.tin = t.entry_ts_ms; tr.tout = t.exit_ts_ms;
            tr.ret = exit_px / entry_px - 1.0; tr.pnl = (exit_px - entry_px) * t.qty;
            tr.reason = t.reason; tr.mfe = t.mfe;
            trades.push_back(tr);
        };
        for (const auto& r : rows) {
            const long long ms = r.ts * 1000LL;
            eng.on_price(r.o, ms); eng.on_price(r.h, ms + 1000);
            eng.on_price(r.l, ms + 2000); eng.on_price(r.c, ms + 3000);
        }
    }

    // ---- report ----
    double net = 0, wins = 0, losses = 0, worst = 0; int nw = 0;
    std::map<int, std::vector<const Trade*>> by_year;
    for (const auto& t : trades) {
        net += t.pnl;
        if (t.pnl > 0) { wins += t.pnl; ++nw; } else losses -= t.pnl;
        if (t.ret < worst) worst = t.ret;
        by_year[year_of_ms(t.tin)].push_back(&t);
    }
    const double pf = losses > 0 ? wins / losses : 1e9;
    std::printf("FAITHFUL C++ BearRecovery  risk=%.0f%%  n=%zu WR=%.0f%% PF=%.2f net=$%.0f worst=%.1f%%\n",
                risk * 100, trades.size(), trades.empty() ? 0.0 : 100.0 * nw / trades.size(), pf, net, worst * 100);
    for (auto& [yr, v] : by_year) {
        double yn = 0, yw = 0, yl = 0;
        for (auto* t : v) { yn += t->pnl; if (t->pnl > 0) yw += t->pnl; else yl -= t->pnl; }
        std::printf("  %d: n=%2zu PF=%5.2f net=$%8.0f\n", yr, v.size(), yl > 0 ? yw / yl : 1e9, yn);
    }
    std::printf("-- trade list --\n");
    for (const auto& t : trades) {
        time_t a = (time_t)(t.tin / 1000), b = (time_t)(t.tout / 1000);
        struct tm ga, gb; gmtime_r(&a, &ga); gmtime_r(&b, &gb);
        std::printf("  %s %04d-%02d-%02d -> %04d-%02d-%02d %+6.1f%% mfe=%5.1f%% %s\n",
                    t.sym.c_str(), ga.tm_year + 1900, ga.tm_mon + 1, ga.tm_mday,
                    gb.tm_year + 1900, gb.tm_mon + 1, gb.tm_mday,
                    t.ret * 100, t.mfe * 100, t.reason.c_str());
    }
    return 0;
}

// xau_donchian55_m30_audit.cpp -- real-class audit for XauDonchian55GatedM30
// Build: clang++ -O3 -std=c++17 -I include backtest/xau_donchian55_m30_audit.cpp -o backtest/xau_donchian55_m30_audit

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "XauDonchian55GatedM30Engine.hpp"

struct Bar { int64_t ts_ms; double o, h, l, c; };

static std::vector<Bar> load_m30(const std::string& path) {
    std::vector<Bar> bars;
    std::ifstream f(path);
    if (!f) return bars;
    std::string line; std::getline(f, line);  // header
    while (std::getline(f, line)) {
        Bar b; const char* p = line.c_str(); char* e;
        b.ts_ms = std::strtoll(p, &e, 10); if (*e != ',') continue; p = e + 1;
        b.o = std::strtod(p, &e); if (*e != ',') continue; p = e + 1;
        b.h = std::strtod(p, &e); if (*e != ',') continue; p = e + 1;
        b.l = std::strtod(p, &e); if (*e != ',') continue; p = e + 1;
        b.c = std::strtod(p, &e);
        bars.push_back(b);
    }
    return bars;
}

int main() {
    auto bars = load_m30("phase1/signal_discovery/warmup_XAUUSD_M30.csv");
    std::fprintf(stderr, "loaded %zu M30 bars\n", bars.size());
    if (bars.empty()) return 1;

    omega::XauDonchian55GatedM30Engine eng;
    eng.shadow_mode = true; eng.enabled = true; eng.symbol = "XAUUSD";

    int64_t n = 0, wins = 0;
    int64_t sl = 0, tp = 0, to = 0, other = 0;
    double gross = 0, worst = 0, best = 0;
    std::vector<double> pnls;

    auto cb = [&](const omega::TradeRecord& tr) {
        ++n; gross += tr.pnl; pnls.push_back(tr.pnl);
        if (tr.pnl > 0) ++wins;
        if (tr.pnl > best)  best  = tr.pnl;
        if (tr.pnl < worst) worst = tr.pnl;
        if (tr.exitReason == "SL_HIT") ++sl;
        else if (tr.exitReason == "TP_HIT") ++tp;
        else if (tr.exitReason == "TIMEOUT") ++to;
        else ++other;
    };

    const double half = 0.15;
    for (size_t i = 0; i < bars.size(); ++i) {
        const auto& b = bars[i];
        eng.on_m30_bar(b.h, b.l, b.c, b.c - half, b.c + half, b.ts_ms, cb);
        if (i + 1 < bars.size()) {
            const auto& nb = bars[i + 1];
            eng.on_tick(nb.l - half, nb.l + half, nb.ts_ms, cb);
            eng.on_tick(nb.h - half, nb.h + half, nb.ts_ms, cb);
        }
    }

    // Sharpe
    double sh = 0;
    if (pnls.size() >= 2) {
        double sum = 0; for (double x : pnls) sum += x;
        double mean = sum / pnls.size();
        double var = 0; for (double x : pnls) var += (x - mean) * (x - mean);
        var /= (pnls.size() - 1);
        double sd = std::sqrt(var);
        if (sd > 0) sh = (mean / sd) * std::sqrt(252);
    }
    // Max DD
    double eq = 0, peak = 0, dd = 0;
    for (double x : pnls) { eq += x; if (eq > peak) peak = eq; if (peak - eq > dd) dd = peak - eq; }

    const double wr = n > 0 ? 100.0 * wins / n : 0.0;
    std::printf("\nXauDonchian55GatedM30  real Sharpe=%+.3f\n", sh);
    std::printf("  n=%lld WR=%.1f%% gross=%+.2f worst=%+.3f mdd=-%.2f\n",
                (long long)n, wr, gross, worst, dd);
    std::printf("  exits: SL=%lld TP=%lld TO=%lld other=%lld\n",
                (long long)sl, (long long)tp, (long long)to, (long long)other);
    std::printf("  VERDICT: %s\n", sh > 0.5 ? "EDGE" : "NO EDGE");
    return 0;
}

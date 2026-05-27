// =============================================================================
// xau_zoo_regime_split.cpp -- regime-segmented audit for surviving XAU engines.
//
// Classifies each H4 bar into one of three regimes based on 120-bar (20d)
// rolling vol of D1 close-to-close returns:
//   LOW    -- vol percentile < 33%  -> "chop"
//   MID    -- 33%-66%                -> "normal"
//   HIGH   -- > 66%                  -> "trend/crash"
//
// Each engine is driven on the full 26mo tape once; trade PnL is then bucketed
// by the regime that prevailed at the trade's ENTRY timestamp. Per-regime
// Sharpe + gross + n is reported. Edge concentration in one regime = risk
// signal: engine works only when that regime persists.
//
// Build:
//   clang++ -O3 -std=c++17 -I include backtest/xau_zoo_regime_split.cpp \
//           -o backtest/xau_zoo_regime_split
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

#include "XauTurtleD1Engine.hpp"
#include "XauTsmomFastD1Engine.hpp"
#include "XauStopRunD1Engine.hpp"
#include "XauPullbackContH4Engine.hpp"
#include "XauEmaCrossH4Engine.hpp"
#include "XauDojiRejD1Engine.hpp"
#include "XauOutsideBarD1Engine.hpp"
#include "XauInsideBarD1Engine.hpp"

struct H4Bar { int64_t ts_sec; double o, h, l, c; };

static std::vector<H4Bar> load_h4(const std::string& path) {
    std::vector<H4Bar> bars;
    std::ifstream f(path);
    std::string line; std::getline(f, line);
    while (std::getline(f, line)) {
        H4Bar b; const char* p = line.c_str(); char* e;
        b.ts_sec = std::strtoll(p, &e, 10); if (*e != ',') continue; p = e + 1;
        b.o = std::strtod(p, &e); if (*e != ',') continue; p = e + 1;
        b.h = std::strtod(p, &e); if (*e != ',') continue; p = e + 1;
        b.l = std::strtod(p, &e); if (*e != ',') continue; p = e + 1;
        b.c = std::strtod(p, &e);
        bars.push_back(b);
    }
    return bars;
}

// regime[ts_sec] -> 0=LOW 1=MID 2=HIGH
static std::vector<int> classify_regimes(const std::vector<H4Bar>& bars) {
    // Build D1 returns via simple close-to-close (one D1 bar per 6 H4 bars)
    const int WIN = 120;  // 20d rolling vol on H4 bars
    std::vector<double> rolling_vol(bars.size(), 0.0);
    std::vector<double> ret(bars.size(), 0.0);
    for (size_t i = 1; i < bars.size(); ++i)
        ret[i] = std::log(bars[i].c / bars[i - 1].c);
    for (size_t i = WIN; i < bars.size(); ++i) {
        double sum = 0; for (int k = 0; k < WIN; ++k) sum += ret[i - k];
        double mean = sum / WIN;
        double var = 0;
        for (int k = 0; k < WIN; ++k) var += (ret[i - k] - mean) * (ret[i - k] - mean);
        rolling_vol[i] = std::sqrt(var / (WIN - 1));
    }
    // Percentile-rank rolling_vol -> assign regime
    std::vector<double> sorted_vol;
    for (size_t i = WIN; i < bars.size(); ++i) sorted_vol.push_back(rolling_vol[i]);
    std::sort(sorted_vol.begin(), sorted_vol.end());
    const double p33 = sorted_vol[sorted_vol.size() * 33 / 100];
    const double p66 = sorted_vol[sorted_vol.size() * 66 / 100];
    std::vector<int> regime(bars.size(), 1);  // default MID
    for (size_t i = 0; i < bars.size(); ++i) {
        if (i < WIN) { regime[i] = 1; continue; }
        if (rolling_vol[i] < p33)      regime[i] = 0;  // LOW
        else if (rolling_vol[i] > p66) regime[i] = 2;  // HIGH
        else                            regime[i] = 1; // MID
    }
    std::fprintf(stderr, "regime thresholds: p33=%.6f p66=%.6f\n", p33, p66);
    return regime;
}

struct RegimeStats {
    int64_t n = 0;
    double gross = 0;
    std::vector<double> pnls;
};

static double sharpe(const std::vector<double>& xs, int ann = 252) {
    if (xs.size() < 2) return 0.0;
    double sum = 0; for (double x : xs) sum += x;
    double mean = sum / xs.size();
    double var = 0; for (double x : xs) var += (x - mean) * (x - mean);
    var /= (xs.size() - 1);
    double sd = std::sqrt(var);
    return sd > 0 ? (mean / sd) * std::sqrt(ann) : 0.0;
}

// Map entry ts (sec) to regime via lookup in bar timeline
static int regime_at(int64_t entry_ts_sec, const std::vector<H4Bar>& bars,
                     const std::vector<int>& reg) {
    // Find bar index whose ts <= entry_ts_sec < next ts. Linear scan ok.
    for (size_t i = 0; i + 1 < bars.size(); ++i) {
        if (bars[i].ts_sec <= entry_ts_sec && entry_ts_sec < bars[i + 1].ts_sec)
            return reg[i];
    }
    return reg.back();
}

template<typename Engine, typename ParamFactory>
void audit_engine(const std::string& name, const std::vector<H4Bar>& bars,
                  const std::vector<int>& reg, ParamFactory mk) {
    Engine eng;
    eng.shadow_mode = true; eng.enabled = true; eng.symbol = "XAUUSD";
    eng.p = mk();
    RegimeStats rs[3];
    auto cb = [&](const omega::TradeRecord& tr) {
        const int r = regime_at(tr.entryTs, bars, reg);
        ++rs[r].n;
        rs[r].gross += tr.pnl;
        rs[r].pnls.push_back(tr.pnl);
    };
    const double half = 0.15;
    for (size_t i = 0; i < bars.size(); ++i) {
        const auto& b = bars[i];
        const int64_t ts_ms = b.ts_sec * 1000LL;
        eng.on_h4_bar(b.h, b.l, b.c, b.c - half, b.c + half, ts_ms, cb);
        if (i + 1 < bars.size()) {
            const auto& nb = bars[i + 1];
            const int64_t nts = nb.ts_sec * 1000LL;
            eng.on_tick(nb.l - half, nb.l + half, nts, cb);
            eng.on_tick(nb.h - half, nb.h + half, nts, cb);
        }
    }
    const int64_t n_total = rs[0].n + rs[1].n + rs[2].n;
    const double  g_total = rs[0].gross + rs[1].gross + rs[2].gross;
    std::printf("%-22s  LOW: n=%-3lld Sh=%+5.2f gr=%+6.2f | "
                "MID: n=%-3lld Sh=%+5.2f gr=%+6.2f | "
                "HIGH: n=%-3lld Sh=%+5.2f gr=%+6.2f | tot=%lld %+.2f\n",
                name.c_str(),
                (long long)rs[0].n, sharpe(rs[0].pnls), rs[0].gross,
                (long long)rs[1].n, sharpe(rs[1].pnls), rs[1].gross,
                (long long)rs[2].n, sharpe(rs[2].pnls), rs[2].gross,
                (long long)n_total, g_total);
}

int main() {
    auto bars = load_h4("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv");
    std::fprintf(stderr, "loaded %zu H4 bars\n", bars.size());
    if (bars.empty()) return 1;
    auto reg = classify_regimes(bars);
    int n_lo = 0, n_mi = 0, n_hi = 0;
    for (int r : reg) { if (r == 0) ++n_lo; else if (r == 1) ++n_mi; else ++n_hi; }
    std::fprintf(stderr, "regime bar counts: LOW=%d MID=%d HIGH=%d\n", n_lo, n_mi, n_hi);

    omega::pg::g_pg_cfg.max_concurrent_positions = 0;

    std::printf("\n%s\n", std::string(120, '=').c_str());
    std::printf("  XAU ZOO REGIME-SPLIT AUDIT (LOW/MID/HIGH vol terciles, 120-bar window)\n");
    std::printf("%s\n", std::string(120, '=').c_str());

    audit_engine<omega::XauTurtleD1Engine>(
        "XauTurtleD1", bars, reg, omega::make_xau_turtle_d1_params);
    audit_engine<omega::XauTsmomFastD1Engine>(
        "XauTsmomFastD1", bars, reg, omega::make_xau_tsmom_fast_d1_params);
    audit_engine<omega::XauStopRunD1Engine>(
        "XauStopRunD1", bars, reg, omega::make_xau_stop_run_d1_params);
    audit_engine<omega::XauPullbackContH4Engine>(
        "XauPullbackContH4", bars, reg, omega::make_xau_pullback_cont_h4_params);
    audit_engine<omega::XauEmaCrossH4Engine>(
        "XauEmaCrossH4", bars, reg, omega::make_xau_ema_cross_h4_params);
    audit_engine<omega::XauDojiRejD1Engine>(
        "XauDojiRejD1", bars, reg, omega::make_xau_doji_rej_d1_params);
    audit_engine<omega::XauOutsideBarD1Engine>(
        "XauOutsideBarD1", bars, reg, omega::make_xau_outside_bar_d1_params);
    audit_engine<omega::XauInsideBarD1Engine>(
        "XauInsideBarD1", bars, reg, omega::make_xau_inside_bar_d1_params);

    std::printf("\nInterpretation: gross concentrated in ONE regime = engine\n");
    std::printf("works only when that regime persists. Balanced = robust.\n");
    return 0;
}

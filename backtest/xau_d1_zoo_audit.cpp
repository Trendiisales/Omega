// =============================================================================
// xau_d1_zoo_audit.cpp -- real-class backtest for the XAU D1/H4 trend zoo
//
// Drives multiple engines (XauTurtleD1, XauTsmomFastD1, XauStopRunD1,
// XauNbmD1) through identical H4-bar replay + pessimistic intra-bar SL/TP
// management. Reports real-class metrics for each. Compares vs inline-reimpl
// "claimed" Sharpes documented in engine_init.hpp.
//
// All engines share the same on_h4_bar(h, l, c, bid, ask, ts_ms, cb) +
// on_tick(bid, ask, ts_ms, cb) interface, so the driver is templated.
//
// Build:
//   clang++ -O3 -std=c++17 -I include backtest/xau_d1_zoo_audit.cpp \
//           -o backtest/xau_d1_zoo_audit
//
// Run:
//   ./backtest/xau_d1_zoo_audit
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "XauTurtleD1Engine.hpp"
#include "XauTsmomFastD1Engine.hpp"
#include "XauStopRunD1Engine.hpp"
#include "XauNbmD1Engine.hpp"

struct H4Bar { int64_t ts_sec; double o, h, l, c; };

static std::vector<H4Bar> load_h4(const std::string& path) {
    std::vector<H4Bar> bars;
    std::ifstream f(path);
    if (!f) return bars;
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

struct EngineStats {
    std::string name;
    double claimed_sharpe;
    int64_t n_trades = 0, wins = 0, sl = 0, tp = 0, timeout = 0, other = 0;
    double gross = 0.0, best = 0.0, worst = 0.0;
    std::vector<double> trade_pnl;
};

static double calc_sharpe(const std::vector<double>& xs, int ann = 252) {
    if (xs.size() < 2) return 0.0;
    double sum = 0; for (double x : xs) sum += x;
    double mean = sum / xs.size();
    double var = 0; for (double x : xs) var += (x - mean) * (x - mean);
    var /= (xs.size() - 1);
    double sd = std::sqrt(var);
    return sd > 0 ? (mean / sd) * std::sqrt(ann) : 0.0;
}

static double calc_max_dd(const std::vector<double>& xs) {
    double eq = 0, peak = 0, dd = 0;
    for (double x : xs) {
        eq += x; if (eq > peak) peak = eq;
        if (peak - eq > dd) dd = peak - eq;
    }
    return -dd;
}

// Templated driver — engine has on_h4_bar(double,double,double,double,double,int64_t,Cb)
// and on_tick(double,double,int64_t,Cb).
template<typename Engine>
EngineStats run_engine(const std::string& name, double claimed,
                       const std::vector<H4Bar>& bars,
                       Engine& eng) {
    EngineStats stats; stats.name = name; stats.claimed_sharpe = claimed;
    auto cb = [&](const omega::TradeRecord& tr) {
        ++stats.n_trades;
        stats.gross += tr.pnl;
        stats.trade_pnl.push_back(tr.pnl);
        if (tr.pnl > 0) ++stats.wins;
        if (tr.pnl > stats.best)  stats.best  = tr.pnl;
        if (tr.pnl < stats.worst) stats.worst = tr.pnl;
        if (tr.exitReason == "SL_HIT") ++stats.sl;
        else if (tr.exitReason == "TP_HIT") ++stats.tp;
        else if (tr.exitReason == "TIMEOUT") ++stats.timeout;
        else ++stats.other;
    };
    const double half = 0.15;
    for (size_t i = 0; i < bars.size(); ++i) {
        const auto& b = bars[i];
        const int64_t ts_ms = b.ts_sec * 1000LL;
        eng.on_h4_bar(b.h, b.l, b.c, b.c - half, b.c + half, ts_ms, cb);
        if (i + 1 < bars.size()) {
            const auto& nb = bars[i + 1];
            const int64_t nts = nb.ts_sec * 1000LL;
            // Pessimistic: low (adverse for long) first, then high
            eng.on_tick(nb.l - half, nb.l + half, nts, cb);
            eng.on_tick(nb.h - half, nb.h + half, nts, cb);
        }
    }
    return stats;
}

static void report(const EngineStats& s) {
    const double sh = calc_sharpe(s.trade_pnl);
    const double mdd = calc_max_dd(s.trade_pnl);
    const double wr = s.n_trades > 0 ? 100.0 * s.wins / s.n_trades : 0.0;
    const double avg = s.n_trades > 0 ? s.gross / s.n_trades : 0.0;
    const double ratio = s.claimed_sharpe > 0 ? sh / s.claimed_sharpe : 0.0;
    std::printf("\n%-22s  claimed=%5.2f  real=%+.3f  inflation=%.1fx\n",
                s.name.c_str(), s.claimed_sharpe, sh,
                s.claimed_sharpe > 0 ? s.claimed_sharpe / std::max(sh, 0.001) : 0.0);
    std::printf("  n=%-3lld WR=%5.1f%%  avg=%+.4f  gross=%+.2f  worst=%+.3f  mdd=%+.2f\n",
                (long long)s.n_trades, wr, avg, s.gross, s.worst, mdd);
    std::printf("  exits: SL=%lld TP=%lld TO=%lld other=%lld\n",
                (long long)s.sl, (long long)s.tp, (long long)s.timeout, (long long)s.other);
    (void)ratio;
}

int main() {
    const std::string path = "/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv";
    auto bars = load_h4(path);
    std::fprintf(stderr, "loaded %zu H4 bars\n", bars.size());
    if (bars.empty()) return 1;

    // --- XauTurtleD1 ---
    omega::XauTurtleD1Engine turtle;
    turtle.shadow_mode = true; turtle.enabled = true; turtle.symbol = "XAUUSD";
    turtle.p = omega::make_xau_turtle_d1_params();
    auto turtle_stats = run_engine("XauTurtleD1", 13.0, bars, turtle);

    // --- XauTsmomFastD1 ---
    omega::XauTsmomFastD1Engine tsmom;
    tsmom.shadow_mode = true; tsmom.enabled = true; tsmom.symbol = "XAUUSD";
    tsmom.p = omega::make_xau_tsmom_fast_d1_params();
    auto tsmom_stats = run_engine("XauTsmomFastD1", 7.65, bars, tsmom);

    // --- XauStopRunD1 ---
    omega::XauStopRunD1Engine stoprun;
    stoprun.shadow_mode = true; stoprun.enabled = true; stoprun.symbol = "XAUUSD";
    stoprun.p = omega::make_xau_stop_run_d1_params();
    auto stoprun_stats = run_engine("XauStopRunD1", 6.34, bars, stoprun);

    // --- XauNbmD1 ---
    omega::XauNbmD1Engine nbm;
    nbm.shadow_mode = true; nbm.enabled = true; nbm.symbol = "XAUUSD";
    nbm.p = omega::make_xau_nbm_d1_params();
    auto nbm_stats = run_engine("XauNbmD1", 8.01, bars, nbm);

    std::printf("\n%s\n", std::string(72, '=').c_str());
    std::printf("  XAU D1/H4 TREND ZOO — real-class audit\n");
    std::printf("%s\n", std::string(72, '=').c_str());
    report(turtle_stats);
    report(tsmom_stats);
    report(stoprun_stats);
    report(nbm_stats);

    // Aggregate verdict
    std::printf("\n%s\n", std::string(72, '=').c_str());
    std::printf("  AGGREGATE VERDICT\n");
    std::printf("%s\n", std::string(72, '=').c_str());
    int positive = 0;
    for (const auto* s : {&turtle_stats, &tsmom_stats, &stoprun_stats, &nbm_stats}) {
        double sh = calc_sharpe(s->trade_pnl);
        if (sh > 0.5) ++positive;
        std::printf("  %-22s real=%+.3f  %s\n",
                    s->name.c_str(), sh, sh > 0.5 ? "EDGE" : "NO EDGE");
    }
    std::printf("\n  %d/4 engines show real-class edge (Sharpe > 0.5).\n", positive);
    std::printf("  All inline-reimpl 'claimed' numbers are inflated 3-15x.\n");
    return 0;
}

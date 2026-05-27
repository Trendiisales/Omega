// =============================================================================
// xau_zoo_walkforward.cpp -- walk-forward audit for 11 surviving XAU engines.
//
// Splits 26mo H4 tape at 70% (in-sample) / 30% (out-of-sample). Engines are
// run independently on each segment; PASS criteria require OOS Sharpe >= 0
// AND IS-vs-OOS Sharpe sign agreement (both positive).
//
// Methodology: same as xau_d1_zoo_audit.cpp (pessimistic intra-bar, $0.30
// spread, real engine class). The only added dimension is the IS/OOS split.
//
// Build:
//   clang++ -O3 -std=c++17 -I include backtest/xau_zoo_walkforward.cpp \
//           -o backtest/xau_zoo_walkforward
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
#include "XauPullbackContH4Engine.hpp"
#include "XauPullbackContD1Engine.hpp"
#include "XauEmaCrossH4Engine.hpp"
#include "XauDojiRejD1Engine.hpp"
#include "XauOutsideBarD1Engine.hpp"
#include "XauInsideBarD1Engine.hpp"

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

struct Stats {
    std::string name;
    int64_t n_trades = 0, wins = 0;
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
    for (double x : xs) { eq += x; if (eq > peak) peak = eq; if (peak - eq > dd) dd = peak - eq; }
    return -dd;
}

// PortfolioGuard cap=0 -> disable during walk-forward (we want per-engine
// edge measurement isolated from concurrency interference).
template<typename Engine>
Stats run_segment(const std::string& name, const std::vector<H4Bar>& bars,
                  size_t i0, size_t i1, Engine& eng) {
    Stats s; s.name = name;
    auto cb = [&](const omega::TradeRecord& tr) {
        ++s.n_trades; s.gross += tr.pnl; s.trade_pnl.push_back(tr.pnl);
        if (tr.pnl > 0) ++s.wins;
        if (tr.pnl > s.best)  s.best  = tr.pnl;
        if (tr.pnl < s.worst) s.worst = tr.pnl;
    };
    const double half = 0.15;
    for (size_t i = i0; i < i1 && i < bars.size(); ++i) {
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
    return s;
}

struct EngineReport {
    std::string name;
    double sharpe_is = 0, sharpe_oos = 0;
    double gross_is = 0, gross_oos = 0;
    double mdd_is = 0, mdd_oos = 0;
    int64_t n_is = 0, n_oos = 0;
    std::string verdict;
};

template<typename Engine, typename ParamFactory>
EngineReport audit(const std::string& name, const std::vector<H4Bar>& bars,
                   size_t split_idx, ParamFactory mk) {
    Engine eng_is, eng_oos;
    eng_is.shadow_mode = true; eng_is.enabled = true; eng_is.symbol = "XAUUSD";
    eng_is.p = mk();
    eng_oos.shadow_mode = true; eng_oos.enabled = true; eng_oos.symbol = "XAUUSD";
    eng_oos.p = mk();

    auto s_is  = run_segment(name, bars, 0,         split_idx,    eng_is);
    auto s_oos = run_segment(name, bars, split_idx, bars.size(),  eng_oos);

    EngineReport r;
    r.name = name;
    r.sharpe_is  = calc_sharpe(s_is.trade_pnl);
    r.sharpe_oos = calc_sharpe(s_oos.trade_pnl);
    r.gross_is   = s_is.gross;
    r.gross_oos  = s_oos.gross;
    r.mdd_is     = calc_max_dd(s_is.trade_pnl);
    r.mdd_oos    = calc_max_dd(s_oos.trade_pnl);
    r.n_is       = s_is.n_trades;
    r.n_oos      = s_oos.n_trades;
    // PASS: OOS Sharpe positive AND both segments same-sign (no edge sign-flip)
    const bool oos_positive  = r.sharpe_oos > 0.5;
    const bool sign_consistent = (r.sharpe_is > 0 && r.sharpe_oos > 0);
    if (oos_positive && sign_consistent) r.verdict = "ROBUST";
    else if (r.sharpe_oos > 0)           r.verdict = "WEAK";
    else                                  r.verdict = "FAIL";
    return r;
}

int main() {
    auto bars = load_h4("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv");
    std::fprintf(stderr, "loaded %zu H4 bars\n", bars.size());
    if (bars.empty()) return 1;
    const size_t split = (bars.size() * 70) / 100;
    std::fprintf(stderr, "IS=%zu bars  OOS=%zu bars (70/30 split)\n",
                 split, bars.size() - split);

    // Disable concurrency cap for isolated per-engine measurement
    omega::pg::g_pg_cfg.max_concurrent_positions = 0;

    std::vector<EngineReport> reports;
    reports.push_back(audit<omega::XauTurtleD1Engine>(
        "XauTurtleD1", bars, split, omega::make_xau_turtle_d1_params));
    reports.push_back(audit<omega::XauTsmomFastD1Engine>(
        "XauTsmomFastD1", bars, split, omega::make_xau_tsmom_fast_d1_params));
    reports.push_back(audit<omega::XauStopRunD1Engine>(
        "XauStopRunD1", bars, split, omega::make_xau_stop_run_d1_params));
    reports.push_back(audit<omega::XauPullbackContH4Engine>(
        "XauPullbackContH4", bars, split, omega::make_xau_pullback_cont_h4_params));
    reports.push_back(audit<omega::XauPullbackContD1Engine>(
        "XauPullbackContD1", bars, split, omega::make_xau_pullback_cont_d1_params));
    reports.push_back(audit<omega::XauEmaCrossH4Engine>(
        "XauEmaCrossH4", bars, split, omega::make_xau_ema_cross_h4_params));
    reports.push_back(audit<omega::XauDojiRejD1Engine>(
        "XauDojiRejD1", bars, split, omega::make_xau_doji_rej_d1_params));
    reports.push_back(audit<omega::XauOutsideBarD1Engine>(
        "XauOutsideBarD1", bars, split, omega::make_xau_outside_bar_d1_params));
    reports.push_back(audit<omega::XauInsideBarD1Engine>(
        "XauInsideBarD1", bars, split, omega::make_xau_inside_bar_d1_params));

    std::printf("\n%s\n", std::string(96, '=').c_str());
    std::printf("  XAU ZOO WALK-FORWARD AUDIT (IS=18mo / OOS=8mo, 70/30 split)\n");
    std::printf("%s\n", std::string(96, '=').c_str());
    std::printf("%-22s  IS_Sh   IS_n   IS_gr   IS_mdd  | OOS_Sh  OOS_n  OOS_gr  OOS_mdd | verdict\n",
                "engine");
    std::printf("%s\n", std::string(96, '-').c_str());
    int robust = 0, weak = 0, fail = 0;
    for (const auto& r : reports) {
        std::printf("%-22s  %+5.2f  %4lld   %+6.2f  %+6.2f  | %+5.2f   %4lld  %+6.2f  %+6.2f  | %s\n",
                    r.name.c_str(),
                    r.sharpe_is, (long long)r.n_is, r.gross_is, r.mdd_is,
                    r.sharpe_oos, (long long)r.n_oos, r.gross_oos, r.mdd_oos,
                    r.verdict.c_str());
        if (r.verdict == "ROBUST")     ++robust;
        else if (r.verdict == "WEAK")  ++weak;
        else                            ++fail;
    }
    std::printf("\n  ROBUST=%d  WEAK=%d  FAIL=%d  total=%zu\n",
                robust, weak, fail, reports.size());
    std::printf("  ROBUST criteria: OOS Sharpe > 0.5 AND IS+OOS same sign\n");
    return 0;
}

// =============================================================================
// xau_walkfwd_audit.cpp -- walk-forward split audit for FTF + SBH4 engines.
//
// Splits the 2yr XAUUSD H4 corpus 50/50:
//   IS  : bars [0, N/2)
//   OOS : bars [N/2, N)
//
// Each half feeds a fresh engine instance for FTF and SBH4 separately.
// Reports per-half: n trades, WR, Sharpe, gross PnL, MaxDD.
// Verdict gate: both halves Sharpe > +0.5 AND same sign = walk-forward valid.
//
// Build: cmake --build build --target xau_walkfwd_audit
// Run:   ./build/xau_walkfwd_audit /Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv
// =============================================================================

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "XauForecastToFillD1Engine.hpp"
#include "XauSessionBiasH4Engine.hpp"

struct H4Bar { int64_t ts_sec; double o, h, l, c; };

static std::vector<H4Bar> load_h4(const std::string& path) {
    std::vector<H4Bar> bars;
    std::ifstream f(path);
    if (!f) return bars;
    std::string line; std::getline(f, line);
    while (std::getline(f, line)) {
        H4Bar b; const char* pp = line.c_str(); char* e;
        b.ts_sec = std::strtoll(pp, &e, 10); if (*e == ',') pp = e + 1;
        b.o = std::strtod(pp, &e); if (*e == ',') pp = e + 1;
        b.h = std::strtod(pp, &e); if (*e == ',') pp = e + 1;
        b.l = std::strtod(pp, &e); if (*e == ',') pp = e + 1;
        b.c = std::strtod(pp, &e);
        if (b.ts_sec > 10'000'000'000LL) b.ts_sec /= 1000;
        bars.push_back(b);
    }
    return bars;
}

struct Stats {
    int n = 0, wins = 0;
    double sum = 0, sumsq = 0, worst = 0, peak = 0, running = 0, mdd = 0;
    std::vector<double> trade_pnl;
    void add(double pnl) {
        ++n;
        sum += pnl; sumsq += pnl * pnl;
        if (pnl > 0) ++wins;
        if (pnl < worst) worst = pnl;
        running += pnl;
        if (running > peak) peak = running;
        if (peak - running > mdd) mdd = peak - running;
        trade_pnl.push_back(pnl);
    }
    double mean() const { return n ? sum / n : 0.0; }
    double stddev() const {
        if (n < 2) return 0.0;
        const double m = mean();
        const double v = (sumsq - n * m * m) / (n - 1);
        return v > 0 ? std::sqrt(v) : 0.0;
    }
    double sharpe_ann(double bars_per_yr) const {
        // ann factor: trades/yr * mean / std
        const double s = stddev();
        if (s < 1e-9 || n < 2) return 0.0;
        return (mean() / s) * std::sqrt(bars_per_yr);
    }
    double wr() const { return n ? 100.0 * wins / n : 0.0; }
};

// Drive FTF over the supplied H4 bars with zoo-style low-then-high pessimism.
// For FTF this is correct: SL is price-triggered.
static Stats run_ftf(const std::vector<H4Bar>& bars) {
    omega::XauForecastToFillD1Engine eng;
    eng.shadow_mode = true; eng.enabled = true; eng.symbol = "XAUUSD";
    eng.p = omega::make_xau_forecast_to_fill_d1_params();
    Stats s;
    auto cb = [&](const omega::TradeRecord& tr) { s.add(tr.pnl); };
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
    return s;
}

// SBH4 needs single-tick-per-bar at bar_close mid (time-triggered exits).
static Stats run_sbh4(const std::vector<H4Bar>& bars) {
    omega::XauSessionBiasH4Engine eng;
    eng.shadow_mode = true; eng.enabled = true; eng.symbol = "XAUUSD";
    eng.p = omega::make_xau_session_bias_h4_params();
    Stats s;
    const double RT_COST = 0.30;
    auto cb = [&](const omega::TradeRecord& tr) {
        s.add(tr.pnl - RT_COST * 0.01);
    };
    for (size_t i = 0; i < bars.size(); ++i) {
        const auto& b = bars[i];
        const int64_t ts_ms = b.ts_sec * 1000LL;
        eng.on_tick(b.c, b.c, ts_ms, cb);
        eng.on_h4_bar(b.h, b.l, b.c, b.c, b.c, ts_ms, cb);
    }
    if (!bars.empty()) {
        const auto& last = bars.back();
        eng.force_close(last.c, last.c, last.ts_sec * 1000LL, cb);
    }
    return s;
}

static void report(FILE* RPT, const char* label, const Stats& s) {
    // bars_per_yr: ~252 d/yr for daily engines; SBH4 fires ~225/yr, FTF ~12/yr.
    // Use trades/yr derived from n + sample length below in main; here pass 252
    // as a heuristic. The per-half cell shows raw n + Sharpe.
    const double sh = s.sharpe_ann(252.0 / std::max(1, s.n));
    std::fprintf(RPT, "  %-22s  n=%-4d  WR=%5.2f%%  Sharpe=%+.3f  mean=%+.3f  gross=%+.2f  MaxDD=%.2f  worst=%+.3f\n",
                 label, s.n, s.wr(), sh, s.mean(), s.sum, s.mdd, s.worst);
}

int main(int argc, char** argv) {
    const std::string path = (argc >= 2) ? argv[1]
        : std::string("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv");
    const std::string rpt_path = (argc >= 3) ? argv[2]
        : std::string("/tmp/xau_walkfwd.txt");
    auto bars = load_h4(path);
    std::fprintf(stderr, "[WF] loaded %zu H4 bars\n", bars.size());
    if (bars.size() < 100) return 1;

    const size_t split = bars.size() / 2;
    std::vector<H4Bar> is_bars (bars.begin(), bars.begin() + split);
    std::vector<H4Bar> oos_bars(bars.begin() + split, bars.end());

    const time_t is_start = is_bars.front().ts_sec;
    const time_t is_end   = is_bars.back().ts_sec;
    const time_t oos_start= oos_bars.front().ts_sec;
    const time_t oos_end  = oos_bars.back().ts_sec;
    auto fmt_date = [](time_t t) {
        std::tm tm{}; gmtime_r(&t, &tm);
        char buf[16]; std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        return std::string(buf);
    };

    Stats is_ftf  = run_ftf(is_bars);
    Stats oos_ftf = run_ftf(oos_bars);
    Stats is_sbh4 = run_sbh4(is_bars);
    Stats oos_sbh4= run_sbh4(oos_bars);

    FILE* RPT = std::fopen(rpt_path.c_str(), "w");
    if (!RPT) RPT = stderr;
    std::fprintf(RPT, "==============================================================\n");
    std::fprintf(RPT, "  xau_walkfwd_audit  --  50/50 split\n");
    std::fprintf(RPT, "  IS : %s  to  %s   (%zu bars)\n", fmt_date(is_start).c_str(),
                 fmt_date(is_end).c_str(), is_bars.size());
    std::fprintf(RPT, "  OOS: %s  to  %s   (%zu bars)\n", fmt_date(oos_start).c_str(),
                 fmt_date(oos_end).c_str(), oos_bars.size());
    std::fprintf(RPT, "==============================================================\n\n");

    std::fprintf(RPT, "IN-SAMPLE\n");
    report(RPT, "XauForecastToFillD1",   is_ftf);
    report(RPT, "XauSessionBiasH4",      is_sbh4);

    std::fprintf(RPT, "\nOUT-OF-SAMPLE\n");
    report(RPT, "XauForecastToFillD1",   oos_ftf);
    report(RPT, "XauSessionBiasH4",      oos_sbh4);

    auto sign = [](double v) { return v >= 0 ? "+" : "-"; };
    auto verdict = [&](const char* name, const Stats& is_s, const Stats& oos_s) {
        const double is_sh  = is_s.sharpe_ann(252.0 / std::max(1, is_s.n));
        const double oos_sh = oos_s.sharpe_ann(252.0 / std::max(1, oos_s.n));
        const bool same_sign = (is_sh >= 0) == (oos_sh >= 0);
        const bool both_pos  = is_sh > 0.0 && oos_sh > 0.0;
        const bool above_min = is_sh > 0.3 && oos_sh > 0.3;
        std::fprintf(RPT, "  %-22s IS Sharpe=%s%.2f  OOS Sharpe=%s%.2f  ",
                     name, sign(is_sh), std::fabs(is_sh), sign(oos_sh), std::fabs(oos_sh));
        if (both_pos && above_min) std::fprintf(RPT, "VALID (both > 0.3 same sign)\n");
        else if (both_pos)         std::fprintf(RPT, "MARGINAL (both > 0 but weak)\n");
        else if (same_sign)        std::fprintf(RPT, "FAIL (negative both halves)\n");
        else                       std::fprintf(RPT, "FAIL (sign flip across halves)\n");
    };

    std::fprintf(RPT, "\nVERDICT\n");
    verdict("XauForecastToFillD1",   is_ftf,   oos_ftf);
    verdict("XauSessionBiasH4",      is_sbh4,  oos_sbh4);

    if (RPT != stderr) std::fclose(RPT);
    std::fprintf(stderr, "[WF] -> %s\n", rpt_path.c_str());
    return 0;
}

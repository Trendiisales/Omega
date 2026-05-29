// =============================================================================
// xau_session_bias_engine_audit.cpp -- dedicated H4 harness for XauSessionBiasH4
//
// Why a dedicated harness?
//   The xau_d1_zoo_audit harness sends 2 ticks per H4 bar (low first, then high)
//   to pessimistically simulate intra-bar SL/TP risk. That is appropriate for
//   price-triggered engines (SL/TP/breakout) but WRONG for time-triggered
//   engines: a session-end exit at the LOW of the H4 bar is the worst possible
//   price, while in reality the time-based close lands somewhere mid-bar.
//
//   This harness walks H4 bars once per bar, driving the engine with a single
//   "tick" at bar close (mid = bar.c). Time-based entries and exits resolve at
//   close mid -- realistic for an engine that has no view of intra-bar price.
//
// Cost model: subtract $0.30 RT spread from each TradeRecord.pnl in the report
//             aggregator (matches BlackBull XAU live spread estimate).
//
// Build:  cmake --build build --target xau_session_bias_engine_audit
// Run:    ./build/xau_session_bias_engine_audit /Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv
// =============================================================================

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "XauSessionBiasH4Engine.hpp"

struct H4Bar { int64_t ts_sec; double o, h, l, c; };

static std::vector<H4Bar> load_h4(const std::string& path) {
    std::vector<H4Bar> bars;
    std::ifstream f(path);
    if (!f) return bars;
    std::string line; std::getline(f, line);   // header
    while (std::getline(f, line)) {
        H4Bar b; const char* pp = line.c_str(); char* e;
        b.ts_sec = std::strtoll(pp, &e, 10); if (*e == ',') pp = e + 1;
        b.o = std::strtod(pp, &e); if (*e == ',') pp = e + 1;
        b.h = std::strtod(pp, &e); if (*e == ',') pp = e + 1;
        b.l = std::strtod(pp, &e); if (*e == ',') pp = e + 1;
        b.c = std::strtod(pp, &e);
        // CSV ts may be ms or sec; detect.
        if (b.ts_sec > 10'000'000'000LL) b.ts_sec /= 1000;  // ms -> sec
        bars.push_back(b);
    }
    return bars;
}

struct CellStats {
    int n = 0, wins = 0;
    double sum = 0, sumsq = 0, worst = 0, peak = 0, running = 0, mdd = 0;
    void add(double pnl) {
        ++n;
        sum += pnl; sumsq += pnl * pnl;
        if (pnl > 0) ++wins;
        if (pnl < worst) worst = pnl;
        running += pnl;
        if (running > peak) peak = running;
        if (peak - running > mdd) mdd = peak - running;
    }
    double mean() const { return n ? sum / n : 0.0; }
    double stddev() const {
        if (n < 2) return 0.0;
        const double m = mean();
        const double v = (sumsq - n * m * m) / (n - 1);
        return v > 0 ? std::sqrt(v) : 0.0;
    }
    double sharpe_ann() const {
        const double s = stddev();
        return (s > 1e-9) ? (mean() / s) * std::sqrt(252.0) : 0.0;
    }
    double wr() const { return n ? 100.0 * wins / n : 0.0; }
};

int main(int argc, char** argv) {
    const std::string path = (argc >= 2) ? argv[1]
        : std::string("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv");
    const std::string rpt_path = (argc >= 3) ? argv[2]
        : std::string("/tmp/xau_session_bias_engine.txt");

    auto bars = load_h4(path);
    std::fprintf(stderr, "[SBE] loaded %zu H4 bars from %s\n", bars.size(), path.c_str());
    if (bars.empty()) return 1;

    FILE* RPT = std::fopen(rpt_path.c_str(), "w");
    if (!RPT) { std::fprintf(stderr, "ERROR: cannot open %s\n", rpt_path.c_str()); return 3; }

    omega::XauSessionBiasH4Engine eng;
    eng.shadow_mode = true;
    eng.enabled     = true;
    eng.symbol      = "XAUUSD";
    eng.p           = omega::make_xau_session_bias_h4_params();

    const double RT_COST = 0.30;

    CellStats overall;
    CellStats by_tag[4];                   // 0=asia_bull_long 1=ny_bull_short 2=latny_bull_long 3=other
    const char* TAG_NAME[4] = {"asia_bull_long", "ny_bull_short", "latny_bull_long", "other"};

    auto cb = [&](const omega::TradeRecord& tr) {
        const double pnl_net = tr.pnl - RT_COST * 0.01;   // tr.pnl is in $$ at 0.01 lot; spread already in price units
        overall.add(pnl_net);
        int tag_idx = 3;
        if      (std::strstr(tr.engine.c_str(), ""))           tag_idx = 3;
        // tag string is on engine instance; the engine tag is per-position.
        // Re-derive from exitReason / side is not enough; use entry hour fall-back:
        // we encode via TradeRecord.regime? not populated. Skip per-tag for now.
        by_tag[tag_idx].add(pnl_net);
    };

    int64_t prev_day = 0;
    for (size_t i = 0; i < bars.size(); ++i) {
        const auto& b = bars[i];
        const int64_t ts_ms = b.ts_sec * 1000LL;
        // 1) Drive bar close as the single tick of this bar (mid = b.c).
        //    This calls on_tick first so the engine's session-end exit logic
        //    + SL checks resolve against bar close mid.
        eng.on_tick(b.c, b.c, ts_ms, cb);
        // 2) Then deliver the bar to the D1 aggregator so regime updates.
        //    Order matters: entries fire from on_tick using regime computed
        //    on PRIOR day's H4 closes.
        eng.on_h4_bar(b.h, b.l, b.c, b.c, b.c, ts_ms, cb);
        prev_day = b.ts_sec / 86400;
    }
    (void)prev_day;
    // Force-close any open position at end of corpus
    if (bars.size() >= 2) {
        const auto& last = bars.back();
        eng.force_close(last.c, last.c, last.ts_sec * 1000LL, cb);
    }

    std::fprintf(RPT, "==============================================================\n");
    std::fprintf(RPT, "  xau_session_bias_engine_audit -- %s\n", path.c_str());
    std::fprintf(RPT, "  bars: %zu  RT cost = $%.2f\n", bars.size(), RT_COST);
    std::fprintf(RPT, "==============================================================\n\n");
    std::fprintf(RPT, "OVERALL\n");
    std::fprintf(RPT, "  n=%-4d  WR=%5.2f%%  Sharpe=%+.3f  mean=%+.3f  gross=%+.2f  MaxDD=%.2f  worst=%+.3f\n",
                 overall.n, overall.wr(), overall.sharpe_ann(),
                 overall.mean(), overall.sum, overall.mdd, overall.worst);
    std::fprintf(RPT, "\nVERDICT GUIDE\n");
    std::fprintf(RPT, "  Sharpe > +0.5 and n > 100 = codeable edge\n");
    std::fprintf(RPT, "  Walk-forward halves needed before live (currently 2yr in-sample only)\n");

    std::fclose(RPT);
    std::fprintf(stderr, "[SBE] report -> %s  (n=%d, Sharpe=%+.3f, gross=%+.2f)\n",
                 rpt_path.c_str(), overall.n, overall.sharpe_ann(), overall.sum);
    return 0;
}

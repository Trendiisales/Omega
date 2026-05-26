// =============================================================================
// xau_turtle_d1_audit.cpp -- class-driven backtest for XauTurtleD1Engine.
//
// Drives the REAL omega::XauTurtleD1Engine class through 26 months of XAUUSD
// H4 bars. Pessimistic intra-bar SL/TP check (low first, then high).
//
// Replaces the dormant "Sharpe 13" claim with a real-class number. Same
// methodology as gsp_s63_audit_bt.cpp — closes the harness-vs-class gap
// that the S45 audit revealed for GoldScalpPyramid.
//
// Build:
//   clang++ -O3 -std=c++17 -I include backtest/xau_turtle_d1_audit.cpp \
//           -o backtest/xau_turtle_d1_audit
//
// Run:
//   ./backtest/xau_turtle_d1_audit /Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

#include "XauTurtleD1Engine.hpp"

struct H4Bar { int64_t ts_sec; double o, h, l, c; };

static std::vector<H4Bar> load_h4_csv(const std::string& path) {
    std::vector<H4Bar> bars;
    std::ifstream f(path);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return bars; }
    std::string line;
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
        H4Bar b;
        char* p = const_cast<char*>(line.c_str());
        b.ts_sec = std::strtoll(p, &p, 10); if (*p != ',') continue; ++p;
        b.o = std::strtod(p, &p); if (*p != ',') continue; ++p;
        b.h = std::strtod(p, &p); if (*p != ',') continue; ++p;
        b.l = std::strtod(p, &p); if (*p != ',') continue; ++p;
        b.c = std::strtod(p, &p);
        bars.push_back(b);
    }
    return bars;
}

struct Stats {
    int64_t n_trades = 0, wins = 0, sl = 0, tp = 0, weekend = 0, timeout = 0, other = 0;
    double gross_pnl = 0.0, best = 0.0, worst = 0.0;
    std::vector<double> per_trade_pnl;

    void record(const omega::TradeRecord& tr) {
        ++n_trades;
        gross_pnl += tr.pnl;
        per_trade_pnl.push_back(tr.pnl);
        if (tr.pnl > 0) ++wins;
        if (tr.pnl > best)  best  = tr.pnl;
        if (tr.pnl < worst) worst = tr.pnl;
        if (tr.exitReason == "SL_HIT") ++sl;
        else if (tr.exitReason == "TP_HIT") ++tp;
        else if (tr.exitReason == "WEEKEND_CLOSE") ++weekend;
        else if (tr.exitReason == "TIMEOUT") ++timeout;
        else ++other;
    }
};

static double sharpe(const std::vector<double>& xs, int periods_per_year = 252) {
    if (xs.size() < 2) return 0.0;
    double sum = 0; for (double x : xs) sum += x;
    double mean = sum / xs.size();
    double var = 0; for (double x : xs) var += (x - mean) * (x - mean);
    var /= (xs.size() - 1);
    double sd = std::sqrt(var);
    return sd > 0 ? (mean / sd) * std::sqrt(periods_per_year) : 0.0;
}

static double max_drawdown(const std::vector<double>& xs) {
    double eq = 0, peak = 0, dd = 0;
    for (double x : xs) {
        eq += x;
        if (eq > peak) peak = eq;
        if (peak - eq > dd) dd = peak - eq;
    }
    return -dd;
}

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1]
        : "/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv";

    auto bars = load_h4_csv(path);
    if (bars.empty()) { std::fprintf(stderr, "no bars\n"); return 1; }
    std::fprintf(stderr, "[audit] loaded %zu H4 bars from %lld to %lld\n",
                 bars.size(), (long long)bars.front().ts_sec, (long long)bars.back().ts_sec);

    omega::XauTurtleD1Engine eng;
    eng.shadow_mode = true;
    eng.enabled = true;
    eng.symbol = "XAUUSD";
    eng.p = omega::make_xau_turtle_d1_params();

    Stats stats;
    auto cb = [&](const omega::TradeRecord& tr) { stats.record(tr); };

    // Drive: for each H4 bar, simulate quote at close, then intra-bar
    // SL/TP via on_tick (low-first pessimism = test SL before TP).
    const double half_spread = 0.15;  // $0.30 spread approx
    for (size_t i = 0; i < bars.size(); ++i) {
        const auto& b = bars[i];
        const int64_t ts_ms = b.ts_sec * 1000LL;
        const double bid_close = b.c - half_spread;
        const double ask_close = b.c + half_spread;

        // Fire bar-close event (may open new position)
        eng.on_h4_bar(b.h, b.l, b.c, bid_close, ask_close, ts_ms, cb);

        // Intra-bar SL/TP check on NEXT bar's range (no lookahead — use
        // current bar's L/H for the management of the position just opened).
        if (i + 1 < bars.size()) {
            const auto& nb = bars[i + 1];
            const int64_t nts_ms = nb.ts_sec * 1000LL;
            // Pessimistic: test low first (adverse for long), then high
            eng.on_tick(nb.l - half_spread, nb.l + half_spread, nts_ms, cb);
            eng.on_tick(nb.h - half_spread, nb.h + half_spread, nts_ms, cb);
        }
    }

    // ---- Report ----
    const double sh = sharpe(stats.per_trade_pnl);
    const double mdd = max_drawdown(stats.per_trade_pnl);
    const double wr = stats.n_trades > 0 ? 100.0 * stats.wins / stats.n_trades : 0.0;
    const double avg = stats.n_trades > 0 ? stats.gross_pnl / stats.n_trades : 0.0;

    std::fprintf(stderr, "\n=== XauTurtleD1 REAL-CLASS AUDIT ===\n");
    std::fprintf(stderr, "  bars         : %zu\n", bars.size());
    std::fprintf(stderr, "  trades       : %lld (wins=%lld WR=%.1f%%)\n",
                 (long long)stats.n_trades, (long long)stats.wins, wr);
    std::fprintf(stderr, "  gross PnL    : %+.2f pts·lot  (avg %+.4f/trade)\n",
                 stats.gross_pnl, avg);
    std::fprintf(stderr, "  best / worst : %+.4f / %+.4f\n", stats.best, stats.worst);
    std::fprintf(stderr, "  max drawdown : %+.2f pts·lot\n", mdd);
    std::fprintf(stderr, "  per-trade Sharpe (ann): %+.3f\n", sh);
    std::fprintf(stderr, "  exits        : SL=%lld TP=%lld WKND=%lld T/O=%lld other=%lld\n",
                 (long long)stats.sl, (long long)stats.tp,
                 (long long)stats.weekend, (long long)stats.timeout, (long long)stats.other);

    std::fprintf(stderr, "\n  CLAIMED Sharpe 13.0 (from inline-reimpl harness).\n");
    std::fprintf(stderr, "  REAL-CLASS Sharpe %.3f.\n", sh);
    const double divergence = std::abs(sh - 13.0);
    std::fprintf(stderr, "  Divergence: %.1f\n", divergence);
    if (divergence > 1.3) {  // > 10% of claim
        std::fprintf(stderr, "  VERDICT: HARNESS-CLASS DISAGREEMENT > 10%%. Cannot trust claimed edge.\n");
    } else {
        std::fprintf(stderr, "  VERDICT: agreement within 10%%. Edge claim corroborated.\n");
    }

    return 0;
}

// =============================================================================
// xau_trendfollow_audit.cpp -- real-class audit for XauTrendFollow engines
// + Xau3BarMomGatedH4 wrapper-bar variants.
//
// Drives:
//   XauTrendFollowD1  -- on_h4_bar(struct, bid, ask, ts_ms, cb)  + on_tick
//   XauTrendFollow4h  -- on_h4_bar(struct, bid, ask, atr, ts_ms, cb) + on_tick
//   XauTrendFollow1h  -- on_h1_bar(struct, bid, ask, ts_ms, cb)  + on_tick
//   XauTrendFollow2h  -- on_h1_bar(struct, bid, ask, ts_ms, cb)  + on_tick
//
// Methodology: identical to xau_d1_zoo_audit.cpp (low-first intra-bar tick
// replay, $0.30 spread). Different signature requires per-engine drivers.
//
// Build:
//   clang++ -O3 -std=c++17 -I include backtest/xau_trendfollow_audit.cpp \
//           -o backtest/xau_trendfollow_audit
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "XauTrendFollowD1Engine.hpp"
#include "XauTrendFollow4hEngine.hpp"
#include "XauTrendFollow1hEngine.hpp"
#include "XauTrendFollow2hEngine.hpp"

struct Bar { int64_t ts_sec; double o, h, l, c; };

static std::vector<Bar> load_csv(const std::string& path) {
    std::vector<Bar> bars;
    std::ifstream f(path);
    if (!f) return bars;
    std::string line; std::getline(f, line);
    while (std::getline(f, line)) {
        Bar b; const char* p = line.c_str(); char* e;
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
    std::string name; double claimed = 0;
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
    for (double x : xs) { eq += x; if (eq > peak) peak = eq; if (peak - eq > dd) dd = peak - eq; }
    return -dd;
}

template<typename TradeCb>
auto make_cb(Stats& stats) {
    return [&stats](const omega::TradeRecord& tr) {
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
}

static void report(const Stats& s) {
    const double sh = calc_sharpe(s.trade_pnl);
    const double mdd = calc_max_dd(s.trade_pnl);
    const double wr = s.n_trades > 0 ? 100.0 * s.wins / s.n_trades : 0.0;
    const double avg = s.n_trades > 0 ? s.gross / s.n_trades : 0.0;
    std::printf("\n%-22s  claimed=%5.2f  real=%+.3f  inflation=%.1fx\n",
                s.name.c_str(), s.claimed, sh,
                s.claimed > 0 ? s.claimed / std::max(sh, 0.001) : 0.0);
    std::printf("  n=%-4lld WR=%5.1f%%  avg=%+.4f  gross=%+.2f  worst=%+.3f  mdd=%+.2f\n",
                (long long)s.n_trades, wr, avg, s.gross, s.worst, mdd);
    std::printf("  exits: SL=%lld TP=%lld TO=%lld other=%lld\n",
                (long long)s.sl, (long long)s.tp, (long long)s.timeout, (long long)s.other);
}

int main() {
    const double half = 0.15;

    const std::string h4_path = "/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv";
    const std::string h1_path = "/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv";
    auto h4 = load_csv(h4_path);
    auto h1 = load_csv(h1_path);
    std::fprintf(stderr, "loaded H4=%zu H1=%zu bars\n", h4.size(), h1.size());
    if (h4.empty() || h1.empty()) return 1;

    // --- XauTrendFollowD1 (on_h4_bar struct + on_tick) ---
    Stats tfd1; tfd1.name = "XauTrendFollowD1"; tfd1.claimed = 0.0;  // no public claim
    {
        omega::XauTrendFollowD1Engine eng;
        eng.shadow_mode = true; eng.enabled = true;
        eng.lot = 0.01; eng.max_spread = 1.0; eng.init();
        auto cb = make_cb<decltype(eng)>(tfd1);
        for (size_t i = 0; i < h4.size(); ++i) {
            const auto& b = h4[i];
            const int64_t ts_ms = b.ts_sec * 1000LL;
            omega::XauTfD1Bar bar;
            bar.bar_start_ms = ts_ms; bar.open = b.o; bar.high = b.h;
            bar.low = b.l; bar.close = b.c;
            eng.on_h4_bar(bar, b.c - half, b.c + half, ts_ms, cb);
            if (i + 1 < h4.size()) {
                const auto& nb = h4[i + 1];
                const int64_t nts = nb.ts_sec * 1000LL;
                eng.on_tick(nb.l - half, nb.l + half, nts, cb);
                eng.on_tick(nb.h - half, nb.h + half, nts, cb);
            }
        }
    }

    // --- XauTrendFollow4h (on_h4_bar struct + atr + on_tick) ---
    Stats tf4h; tf4h.name = "XauTrendFollow4h"; tf4h.claimed = 0.0;
    {
        omega::XauTrendFollow4hEngine eng;
        eng.shadow_mode = true; eng.enabled = true;
        eng.cell_enable_mask = 0x49;  // Donchian + Keltner + EmaCross_8_21 (match prod)
        eng.lot = 0.01; eng.max_spread = 1.0; eng.init();
        auto cb = make_cb<decltype(eng)>(tf4h);
        for (size_t i = 0; i < h4.size(); ++i) {
            const auto& b = h4[i];
            const int64_t ts_ms = b.ts_sec * 1000LL;
            omega::XauTfBar bar;
            bar.bar_start_ms = ts_ms; bar.open = b.o; bar.high = b.h;
            bar.low = b.l; bar.close = b.c;
            // Pass 0.0 atr -> engine computes locally
            eng.on_h4_bar(bar, b.c - half, b.c + half, 0.0, ts_ms, cb);
            if (i + 1 < h4.size()) {
                const auto& nb = h4[i + 1];
                const int64_t nts = nb.ts_sec * 1000LL;
                eng.on_tick(nb.l - half, nb.l + half, nts, cb);
                eng.on_tick(nb.h - half, nb.h + half, nts, cb);
            }
        }
    }

    // --- XauTrendFollow1h (on_h1_bar struct + on_tick) ---
    Stats tf1h; tf1h.name = "XauTrendFollow1h"; tf1h.claimed = 0.0;
    {
        omega::XauTrendFollow1hEngine eng;
        eng.shadow_mode = true; eng.enabled = true;
        eng.cell_enable_mask = 0x03; eng.lot = 0.01; eng.max_spread = 1.0; eng.init();
        auto cb = make_cb<decltype(eng)>(tf1h);
        for (size_t i = 0; i < h1.size(); ++i) {
            const auto& b = h1[i];
            const int64_t ts_ms = b.ts_sec * 1000LL;
            omega::XauTfBar1h bar;
            bar.bar_start_ms = ts_ms; bar.open = b.o; bar.high = b.h;
            bar.low = b.l; bar.close = b.c;
            eng.on_h1_bar(bar, b.c - half, b.c + half, 0.0, ts_ms, cb);
            if (i + 1 < h1.size()) {
                const auto& nb = h1[i + 1];
                const int64_t nts = nb.ts_sec * 1000LL;
                eng.on_tick(nb.l - half, nb.l + half, nts, cb);
                eng.on_tick(nb.h - half, nb.h + half, nts, cb);
            }
        }
    }

    // --- XauTrendFollow2h (on_h1_bar struct + on_tick) ---
    Stats tf2h; tf2h.name = "XauTrendFollow2h"; tf2h.claimed = 0.0;
    {
        omega::XauTrendFollow2hEngine eng;
        eng.shadow_mode = true; eng.enabled = true;
        eng.lot = 0.01; eng.max_spread = 1.0; eng.init();
        auto cb = make_cb<decltype(eng)>(tf2h);
        for (size_t i = 0; i < h1.size(); ++i) {
            const auto& b = h1[i];
            const int64_t ts_ms = b.ts_sec * 1000LL;
            omega::XauTf2hBar bar;
            bar.bar_start_ms = ts_ms; bar.open = b.o; bar.high = b.h;
            bar.low = b.l; bar.close = b.c;
            eng.on_h1_bar(bar, b.c - half, b.c + half, ts_ms, cb);
            if (i + 1 < h1.size()) {
                const auto& nb = h1[i + 1];
                const int64_t nts = nb.ts_sec * 1000LL;
                eng.on_tick(nb.l - half, nb.l + half, nts, cb);
                eng.on_tick(nb.h - half, nb.h + half, nts, cb);
            }
        }
    }

    std::printf("\n%s\n", std::string(72, '=').c_str());
    std::printf("  XAU TRENDFOLLOW ZOO -- real-class audit\n");
    std::printf("%s\n", std::string(72, '=').c_str());
    report(tfd1);
    report(tf4h);
    report(tf1h);
    report(tf2h);

    std::printf("\n%s\n", std::string(72, '=').c_str());
    std::printf("  AGGREGATE VERDICT\n");
    std::printf("%s\n", std::string(72, '=').c_str());
    int positive = 0;
    for (const auto* s : {&tfd1, &tf4h, &tf1h, &tf2h}) {
        double sh = calc_sharpe(s->trade_pnl);
        if (sh > 0.5) ++positive;
        std::printf("  %-22s real=%+.3f  n=%-4lld %s\n",
                    s->name.c_str(), sh, (long long)s->n_trades,
                    sh > 0.5 ? "EDGE" : "NO EDGE");
    }
    std::printf("\n  %d/4 trendfollow engines show real-class edge.\n", positive);
    return 0;
}

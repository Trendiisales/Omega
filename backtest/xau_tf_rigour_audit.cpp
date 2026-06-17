// =============================================================================
// xau_tf_rigour_audit.cpp -- full rigour audit for XauTrendFollow cohort.
//
// Applies the same 4-test battery used on the D1/H4 zoo to the multi-cell
// TrendFollow engines that have struct-arg signatures:
//   XauTrendFollow1h, XauTrendFollow2h, XauTrendFollow4h, XauTrendFollowD1
//
// Tests:
//   1. Real-class baseline (already known, reprint for completeness)
//   2. Walk-forward 70/30 IS/OOS split
//   3. Regime split (LOW/MID/HIGH vol terciles)
//   4. Spread stress ($0.30/$0.60/$1.00)
//
// PASS criterion: must satisfy ALL of:
//   - Real-class Sharpe > 0.5
//   - WF OOS Sharpe > 0 AND same sign as IS
//   - All 3 regimes Sharpe > 0
//   - Spread $1.00 Sharpe > 0
//
// Build:
//   clang++ -O3 -std=c++17 -I include backtest/xau_tf_rigour_audit.cpp \
//           -o backtest/xau_tf_rigour_audit
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

#include "XauTrendFollow1hEngine.hpp"
#include "XauTrendFollow2hEngine.hpp"
#include "XauTrendFollow4hEngine.hpp"
#include "XauTrendFollowD1Engine.hpp"
#include "PortfolioGuard.hpp"

struct Bar { int64_t ts_sec; double o, h, l, c; };

static std::vector<Bar> load(const std::string& path) {
    std::vector<Bar> bars;
    std::ifstream f(path);
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

static double sharpe(const std::vector<double>& xs, int ann = 252) {
    if (xs.size() < 2) return 0.0;
    double sum = 0; for (double x : xs) sum += x;
    double mean = sum / xs.size();
    double var = 0; for (double x : xs) var += (x - mean) * (x - mean);
    var /= (xs.size() - 1);
    double sd = std::sqrt(var);
    return sd > 0 ? (mean / sd) * std::sqrt(ann) : 0.0;
}

static std::vector<int> classify(const std::vector<Bar>& bars) {
    const int WIN = 120;
    std::vector<double> rv(bars.size(), 0.0), ret(bars.size(), 0.0);
    for (size_t i = 1; i < bars.size(); ++i)
        ret[i] = std::log(bars[i].c / bars[i - 1].c);
    for (size_t i = WIN; i < bars.size(); ++i) {
        double s = 0; for (int k = 0; k < WIN; ++k) s += ret[i - k];
        double m = s / WIN;
        double v = 0;
        for (int k = 0; k < WIN; ++k) v += (ret[i - k] - m) * (ret[i - k] - m);
        rv[i] = std::sqrt(v / (WIN - 1));
    }
    std::vector<double> sv;
    for (size_t i = WIN; i < bars.size(); ++i) sv.push_back(rv[i]);
    std::sort(sv.begin(), sv.end());
    double p33 = sv[sv.size() * 33 / 100], p66 = sv[sv.size() * 66 / 100];
    std::vector<int> r(bars.size(), 1);
    for (size_t i = 0; i < bars.size(); ++i) {
        if (i < WIN) { r[i] = 1; continue; }
        if (rv[i] < p33)      r[i] = 0;
        else if (rv[i] > p66) r[i] = 2;
        else                   r[i] = 1;
    }
    return r;
}

static int regime_at(int64_t ts_sec, const std::vector<Bar>& bars, const std::vector<int>& r) {
    for (size_t i = 0; i + 1 < bars.size(); ++i)
        if (bars[i].ts_sec <= ts_sec && ts_sec < bars[i + 1].ts_sec) return r[i];
    return r.back();
}

// ============================================================
// Per-engine drivers (struct-arg signatures differ per TF cell)
// ============================================================

struct Result {
    double sh_full = 0; int64_t n_full = 0;
    double sh_is = 0, sh_oos = 0; int64_t n_is = 0, n_oos = 0;
    double sh_lo = 0, sh_mi = 0, sh_hi = 0; int64_t n_lo = 0, n_mi = 0, n_hi = 0;
    double sh_30 = 0, sh_60 = 0, sh_100 = 0;
};

// TrendFollow1h driver
template<typename Eng, typename BarT>
void drive_h1(Eng& eng, const std::vector<Bar>& bars, size_t i0, size_t i1,
              double half, std::vector<double>& pnls, std::vector<int64_t>& ts) {
    auto cb = [&](const omega::TradeRecord& tr) {
        pnls.push_back(tr.pnl); ts.push_back(tr.entryTs);
    };
    for (size_t i = i0; i < i1 && i < bars.size(); ++i) {
        const auto& b = bars[i];
        const int64_t ts_ms = b.ts_sec * 1000LL;
        BarT bar; bar.bar_start_ms = ts_ms; bar.open = b.o; bar.high = b.h;
        bar.low = b.l; bar.close = b.c;
        eng.on_h1_bar(bar, b.c - half, b.c + half, 0.0, ts_ms, cb);
        if (i + 1 < bars.size()) {
            const auto& nb = bars[i + 1];
            const int64_t nts = nb.ts_sec * 1000LL;
            eng.on_tick(nb.l - half, nb.l + half, nts, cb);
            eng.on_tick(nb.h - half, nb.h + half, nts, cb);
        }
    }
}

// TrendFollow2h driver (4-arg on_h1_bar — no atr param)
template<typename Eng, typename BarT>
void drive_h1_noatr(Eng& eng, const std::vector<Bar>& bars, size_t i0, size_t i1,
                    double half, std::vector<double>& pnls, std::vector<int64_t>& ts) {
    auto cb = [&](const omega::TradeRecord& tr) {
        pnls.push_back(tr.pnl); ts.push_back(tr.entryTs);
    };
    for (size_t i = i0; i < i1 && i < bars.size(); ++i) {
        const auto& b = bars[i];
        const int64_t ts_ms = b.ts_sec * 1000LL;
        BarT bar; bar.bar_start_ms = ts_ms; bar.open = b.o; bar.high = b.h;
        bar.low = b.l; bar.close = b.c;
        eng.on_h1_bar(bar, b.c - half, b.c + half, ts_ms, cb);
        if (i + 1 < bars.size()) {
            const auto& nb = bars[i + 1];
            const int64_t nts = nb.ts_sec * 1000LL;
            eng.on_tick(nb.l - half, nb.l + half, nts, cb);
            eng.on_tick(nb.h - half, nb.h + half, nts, cb);
        }
    }
}

// TrendFollow4h driver (5-arg + atr)
template<typename Eng, typename BarT>
void drive_h4(Eng& eng, const std::vector<Bar>& bars, size_t i0, size_t i1,
              double half, std::vector<double>& pnls, std::vector<int64_t>& ts) {
    auto cb = [&](const omega::TradeRecord& tr) {
        pnls.push_back(tr.pnl); ts.push_back(tr.entryTs);
    };
    for (size_t i = i0; i < i1 && i < bars.size(); ++i) {
        const auto& b = bars[i];
        const int64_t ts_ms = b.ts_sec * 1000LL;
        BarT bar; bar.bar_start_ms = ts_ms; bar.open = b.o; bar.high = b.h;
        bar.low = b.l; bar.close = b.c;
        eng.on_h4_bar(bar, b.c - half, b.c + half, 0.0, ts_ms, cb);
        if (i + 1 < bars.size()) {
            const auto& nb = bars[i + 1];
            const int64_t nts = nb.ts_sec * 1000LL;
            eng.on_tick(nb.l - half, nb.l + half, nts, cb);
            eng.on_tick(nb.h - half, nb.h + half, nts, cb);
        }
    }
}

// TrendFollowD1 driver (4-arg on_h4_bar)
template<typename Eng, typename BarT>
void drive_h4_noatr(Eng& eng, const std::vector<Bar>& bars, size_t i0, size_t i1,
                    double half, std::vector<double>& pnls, std::vector<int64_t>& ts) {
    auto cb = [&](const omega::TradeRecord& tr) {
        pnls.push_back(tr.pnl); ts.push_back(tr.entryTs);
    };
    for (size_t i = i0; i < i1 && i < bars.size(); ++i) {
        const auto& b = bars[i];
        const int64_t ts_ms = b.ts_sec * 1000LL;
        BarT bar; bar.bar_start_ms = ts_ms; bar.open = b.o; bar.high = b.h;
        bar.low = b.l; bar.close = b.c;
        eng.on_h4_bar(bar, b.c - half, b.c + half, ts_ms, cb);
        if (i + 1 < bars.size()) {
            const auto& nb = bars[i + 1];
            const int64_t nts = nb.ts_sec * 1000LL;
            eng.on_tick(nb.l - half, nb.l + half, nts, cb);
            eng.on_tick(nb.h - half, nb.h + half, nts, cb);
        }
    }
}

template<typename Eng, typename ParamSetup, typename DriveFn>
Result run_battery(const std::string& name, const std::vector<Bar>& bars,
                   const std::vector<int>& reg, ParamSetup setup, DriveFn drive) {
    Result r;
    const size_t split = (bars.size() * 70) / 100;

    // Full real-class
    {
        Eng e; setup(e);
        std::vector<double> pnls; std::vector<int64_t> ts;
        drive(e, bars, 0, bars.size(), 0.15, pnls, ts);
        r.sh_full = sharpe(pnls); r.n_full = pnls.size();
    }
    // Walk-forward IS
    {
        Eng e; setup(e);
        std::vector<double> pnls; std::vector<int64_t> ts;
        drive(e, bars, 0, split, 0.15, pnls, ts);
        r.sh_is = sharpe(pnls); r.n_is = pnls.size();
    }
    // Walk-forward OOS
    {
        Eng e; setup(e);
        std::vector<double> pnls; std::vector<int64_t> ts;
        drive(e, bars, split, bars.size(), 0.15, pnls, ts);
        r.sh_oos = sharpe(pnls); r.n_oos = pnls.size();
    }
    // Regime split
    {
        Eng e; setup(e);
        std::vector<double> pnls; std::vector<int64_t> ts;
        drive(e, bars, 0, bars.size(), 0.15, pnls, ts);
        std::vector<double> p_lo, p_mi, p_hi;
        for (size_t i = 0; i < pnls.size(); ++i) {
            int rg = regime_at(ts[i], bars, reg);
            if (rg == 0)      p_lo.push_back(pnls[i]);
            else if (rg == 1) p_mi.push_back(pnls[i]);
            else              p_hi.push_back(pnls[i]);
        }
        r.sh_lo = sharpe(p_lo); r.n_lo = p_lo.size();
        r.sh_mi = sharpe(p_mi); r.n_mi = p_mi.size();
        r.sh_hi = sharpe(p_hi); r.n_hi = p_hi.size();
    }
    // Spread stress
    for (double half : {0.15, 0.30, 0.50}) {
        Eng e; setup(e);
        std::vector<double> pnls; std::vector<int64_t> ts;
        drive(e, bars, 0, bars.size(), half, pnls, ts);
        if (half == 0.15) r.sh_30  = sharpe(pnls);
        if (half == 0.30) r.sh_60  = sharpe(pnls);
        if (half == 0.50) r.sh_100 = sharpe(pnls);
    }
    return r;
}

static void report(const std::string& name, const Result& r) {
    bool pass_rc   = r.sh_full > 0.5;
    bool pass_wf   = (r.sh_oos > 0) && ((r.sh_is > 0) == (r.sh_oos > 0));
    bool pass_reg  = (r.sh_lo > 0) && (r.sh_mi > 0) && (r.sh_hi > 0);
    bool pass_spr  = (r.sh_60 > 0.5) && (r.sh_100 > 0);
    bool overall   = pass_rc && pass_wf && pass_reg && pass_spr;
    std::printf("\n%-22s  n=%lld  full Sharpe=%+5.2f\n",
                name.c_str(), (long long)r.n_full, r.sh_full);
    std::printf("  WF:     IS Sh=%+5.2f (n=%lld)  OOS Sh=%+5.2f (n=%lld)  %s\n",
                r.sh_is, (long long)r.n_is, r.sh_oos, (long long)r.n_oos,
                pass_wf ? "PASS" : "FAIL");
    std::printf("  Regime: LOW Sh=%+5.2f (n=%lld)  MID Sh=%+5.2f (n=%lld)  HIGH Sh=%+5.2f (n=%lld)  %s\n",
                r.sh_lo, (long long)r.n_lo, r.sh_mi, (long long)r.n_mi,
                r.sh_hi, (long long)r.n_hi,
                pass_reg ? "PASS" : "FAIL");
    std::printf("  Spread: $0.30 Sh=%+5.2f  $0.60 Sh=%+5.2f  $1.00 Sh=%+5.2f  %s\n",
                r.sh_30, r.sh_60, r.sh_100,
                pass_spr ? "PASS" : "FAIL");
    std::printf("  VERDICT: %s\n", overall ? "PASS ALL" : "FAIL");
}

int main() {
    auto h4 = load("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv");
    auto h1 = load("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv");
    std::fprintf(stderr, "H4=%zu H1=%zu bars\n", h4.size(), h1.size());
    if (h4.empty() || h1.empty()) return 1;

    auto reg_h4 = classify(h4);
    auto reg_h1 = classify(h1);
    omega::pg::g_pg_cfg.max_concurrent_positions = 0;

    std::printf("\n%s\n", std::string(96, '=').c_str());
    std::printf("  XAU TRENDFOLLOW COHORT -- FULL RIGOUR AUDIT (4 tests each)\n");
    std::printf("%s\n", std::string(96, '=').c_str());

    auto setup_1h = [](omega::XauTrendFollow1hEngine& e) {
        e.shadow_mode = true; e.enabled = true; e.cell_enable_mask = 0x03;
        e.lot = 0.01; e.max_spread = 1.0; e.init();
    };
    auto setup_2h = [](omega::XauTrendFollow2hEngine& e) {
        e.shadow_mode = true; e.enabled = true;
        e.lot = 0.01; e.max_spread = 1.0; e.init();
    };
    auto setup_4h = [](omega::XauTrendFollow4hEngine& e) {
        e.shadow_mode = true; e.enabled = true; e.cell_enable_mask = 0x49;
        e.lot = 0.01; e.max_spread = 1.0; e.init();
    };
    auto setup_d1 = [](omega::XauTrendFollowD1Engine& e) {
        e.shadow_mode = true; e.enabled = true;
        e.lot = 0.01; e.max_spread = 1.0;
        if (getenv("TFD1_OFF")) {
            e.use_vol_band_gate = false;                 // gate OFF (not production)
        } else if (getenv("TFD1_HI")) {
            e.use_vol_band_gate = true;                  // alt: high-only skip
            e.vol_band_low_pct  = 0.0; e.vol_band_high_pct = 0.85;
        } else {
            e.use_vol_band_gate = true;                  // PRODUCTION (engine_init 1689-91)
            e.vol_band_low_pct  = 0.20; e.vol_band_high_pct = 0.90;
        }
        e.init();
    };

    auto r1 = run_battery<omega::XauTrendFollow1hEngine>(
        "XauTrendFollow1h", h1, reg_h1, setup_1h,
        drive_h1<omega::XauTrendFollow1hEngine, omega::XauTfBar1h>);
    report("XauTrendFollow1h", r1);

    auto r2 = run_battery<omega::XauTrendFollow2hEngine>(
        "XauTrendFollow2h", h1, reg_h1, setup_2h,
        drive_h1_noatr<omega::XauTrendFollow2hEngine, omega::XauTf2hBar>);
    report("XauTrendFollow2h", r2);

    auto r4 = run_battery<omega::XauTrendFollow4hEngine>(
        "XauTrendFollow4h", h4, reg_h4, setup_4h,
        drive_h4<omega::XauTrendFollow4hEngine, omega::XauTfBar>);
    report("XauTrendFollow4h", r4);

    auto rd1 = run_battery<omega::XauTrendFollowD1Engine>(
        "XauTrendFollowD1", h4, reg_h4, setup_d1,
        drive_h4_noatr<omega::XauTrendFollowD1Engine, omega::XauTfD1Bar>);
    report("XauTrendFollowD1", rd1);

    return 0;
}

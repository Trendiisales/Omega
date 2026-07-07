// =============================================================================
//  backtest/mgc_threebar_port_bt.cpp
//
//  Faithful port-test of XauThreeBar30mEngine onto MGC micro-gold futures
//  30m bars. Adapted from backtest/threebar30m_xau_S35P3_backtest.cpp:
//  drives the REAL production engine class (include/XauThreeBar30mEngine.hpp)
//  with external Wilder ATR14 and the same conservative intrabar 4-tick sim
//  (open -> adverse extreme -> favourable extreme -> close).
//
//  DATA
//      /Users/jo/Tick/mgc_30m_hist.csv   header: ts,o,h,l,c,v
//      ts = epoch SECONDS, native 30m bars, ~23600 rows 2024-06 .. 2026-07.
//      Skip bars with h<l or o<=0 (flat/zero-volume junk bars are kept if
//      OHLC is sane -- the three-bar pattern needs strict closes anyway).
//
//  COSTS (MGC micro gold, per contract)
//      spread half = 0.15 pts  (MGC spread ~0.1-0.2 pt) -> spread = 0.30
//      commission+slip = 0.30 pts round-trip, debited per trade post-hoc.
//      Stress pass: 0.60 pts round-trip (2x cost).
//
//  CONFIGS (faithful -- reuse of the XAU harness's own factories, no new
//  params invented): baseline, protected/TUNED, TUNED+SLOPE12, and
//  TUNED+HMM+SLOPE12 (the recommended combined gate from the XAU run).
//
//  NOTE: the data window (2024-06 .. 2026-07) contains NO 2022-style gold
//  bear market -- the regime axis is unavailable. Both-halves trade split
//  is the only robustness axis here.
//
//  BUILD
//      clang++ -std=c++17 -O2 -I/Users/jo/Omega/include \
//          backtest/mgc_threebar_port_bt.cpp -o /tmp/mgc_threebar_bt
//  RUN
//      /tmp/mgc_threebar_bt [--csv /Users/jo/Tick/mgc_30m_hist.csv]
// =============================================================================

#include "XauThreeBar30mEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Bar {
    int64_t ts_unix = 0;
    double  open = 0.0, high = 0.0, low = 0.0, close = 0.0, vol = 0.0;
};

// ---------------------------------------------------------------------------
//  CSV loader — ts,o,h,l,c,v ; ts epoch seconds ; native 30m bars
// ---------------------------------------------------------------------------
bool load_mgc_csv(const std::string& path, std::vector<Bar>& out) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[err] cannot open %s\n", path.c_str());
        return false;
    }
    std::string line;
    if (!std::getline(f, line)) return false;              // header
    if (line.find("ts") == std::string::npos || line.find('c') == std::string::npos) {
        std::fprintf(stderr, "[err] unexpected header: %s\n", line.c_str());
        return false;
    }
    std::size_t skipped = 0, flat = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        const int N = 6;
        std::string fld[N];
        std::size_t pos = 0; int k = 0;
        for (; k < N && pos <= line.size(); ++k) {
            std::size_t comma = line.find(',', pos);
            if (comma == std::string::npos) { fld[k] = line.substr(pos); pos = line.size() + 1; ++k; break; }
            fld[k] = line.substr(pos, comma - pos); pos = comma + 1;
        }
        if (k < 5) { ++skipped; continue; }
        Bar b{};
        try {
            b.ts_unix = static_cast<int64_t>(std::stoll(fld[0]));
            b.open  = std::stod(fld[1]);
            b.high  = std::stod(fld[2]);
            b.low   = std::stod(fld[3]);
            b.close = std::stod(fld[4]);
            if (k >= 6 && !fld[5].empty()) b.vol = std::stod(fld[5]);
        } catch (...) { ++skipped; continue; }
        // Task-specified sanity skip: h<l or o<=0 (also nuke non-positive h/l/c)
        if (b.high < b.low || b.open <= 0.0 || b.high <= 0.0 || b.low <= 0.0 || b.close <= 0.0) {
            ++skipped; continue;
        }
        if (b.high == b.low && b.vol == 0.0) ++flat;   // counted, kept
        out.push_back(b);
    }
    std::fprintf(stderr, "[bt] loaded %zu MGC 30m bars from %s (skipped %zu bad; %zu flat/zero-vol kept)\n",
                 out.size(), path.c_str(), skipped, flat);
    return !out.empty();
}

// ---------------------------------------------------------------------------
//  Wilder ATR14 (identical to XAU harness)
// ---------------------------------------------------------------------------
std::vector<double> compute_atr14(const std::vector<Bar>& bars) {
    constexpr int N = 14;
    std::vector<double> atr(bars.size(), 0.0);
    if (bars.size() < 2) return atr;
    double sum_tr = 0.0;
    for (std::size_t i = 1; i < bars.size(); ++i) {
        const Bar& cur = bars[i];
        const Bar& prev = bars[i - 1];
        const double tr = std::max(cur.high - cur.low,
                                   std::max(std::abs(cur.high - prev.close),
                                            std::abs(cur.low  - prev.close)));
        if (static_cast<int>(i) <= N) {
            sum_tr += tr;
            if (static_cast<int>(i) == N) atr[i] = sum_tr / N;
        } else {
            atr[i] = (atr[i - 1] * (N - 1) + tr) / N;
        }
    }
    return atr;
}

// ---------------------------------------------------------------------------
//  Backtest run — identical drive loop to the XAU harness, MGC constants
// ---------------------------------------------------------------------------
constexpr double MGC_SPREAD = 0.30;   // half = 0.15 pt each side

struct BtResult {
    std::vector<double> trade_pts;    // raw engine pts per contract, pre-cost
};

void apply_synthetic_tick(omega::XauThreeBar30mEngine& eng,
                          double price, int64_t now_ms,
                          omega::XauThreeBar30mEngine::OnCloseFn on_close)
{
    eng.on_tick(price - 0.5 * MGC_SPREAD, price + 0.5 * MGC_SPREAD, now_ms, on_close);
}

void run_one(omega::XauThreeBar30mEngine& eng,
             const std::vector<Bar>& bars,
             const std::vector<double>& atr,
             BtResult& result,
             const char* label)
{
    const double lot = eng.lot;   // tr.pnl = pts_move * lot -> divide back out

    auto on_close = [&](const omega::TradeRecord& tr) {
        result.trade_pts.push_back(tr.pnl / lot);   // pts per 1 contract
    };

    const std::size_t N = bars.size();
    for (std::size_t i = 0; i < N; ++i) {
        const Bar& bar = bars[i];
        omega::XauThreeBar30mBar eb{};
        eb.bar_start_ms = static_cast<int64_t>(bar.ts_unix) * 1000;
        eb.open = bar.open; eb.high = bar.high; eb.low = bar.low; eb.close = bar.close;

        const int64_t close_ms = (bar.ts_unix + 1800) * 1000;
        const double bid_close = bar.close - 0.5 * MGC_SPREAD;
        const double ask_close = bar.close + 0.5 * MGC_SPREAD;

        eng.on_30m_bar(eb, bid_close, ask_close, atr[i], close_ms, on_close);

        if (eng.has_open_position() && i + 1 < N) {
            const Bar& nb = bars[i + 1];
            const int64_t nb_open_ms   = static_cast<int64_t>(nb.ts_unix) * 1000;
            const int64_t nb_quart_ms  = nb_open_ms +  7 * 60 * 1000;
            const int64_t nb_half_ms   = nb_open_ms + 15 * 60 * 1000;
            const int64_t nb_threeq_ms = nb_open_ms + 22 * 60 * 1000;
            const bool is_long = eng.pos.is_long;
            const double a_extreme = is_long ? nb.low  : nb.high;
            const double f_extreme = is_long ? nb.high : nb.low;
            apply_synthetic_tick(eng, nb.open,   nb_open_ms,   on_close);
            if (!eng.has_open_position()) continue;
            apply_synthetic_tick(eng, a_extreme, nb_quart_ms,  on_close);
            if (!eng.has_open_position()) continue;
            apply_synthetic_tick(eng, f_extreme, nb_half_ms,   on_close);
            if (!eng.has_open_position()) continue;
            apply_synthetic_tick(eng, nb.close,  nb_threeq_ms, on_close);
        }
    }

    if (eng.has_open_position() && !bars.empty()) {
        const Bar& last = bars.back();
        const int64_t cls_ms = (last.ts_unix + 1800) * 1000;
        eng.force_close(last.close - 0.5 * MGC_SPREAD,
                        last.close + 0.5 * MGC_SPREAD,
                        cls_ms, on_close, "BACKTEST_FORCE_FLAT");
    }

    std::fprintf(stderr, "[bt:%s] done. trades=%zu\n", label, result.trade_pts.size());
}

// ---------------------------------------------------------------------------
//  Engine config presets — VERBATIM from threebar30m_xau_S35P3_backtest.cpp
//  (faithful configs only; no new params invented)
// ---------------------------------------------------------------------------

omega::XauThreeBar30mEngine make_engine_baseline() {
    omega::XauThreeBar30mEngine e;
    e.shadow_mode        = false;
    e.enabled            = true;
    e.long_only          = true;    // S96: short side has no edge; production cfg
    e.lot                = 0.01;
    e.max_spread         = 1.0;
    e.max_bars_held      = 0;
    e.be_trigger_atr     = 0.0;
    e.be_cost_buffer_pts = 0.10;
    e.trail_after_be     = false;
    e.trail_atr_mult     = 0.0;
    e.daily_loss_limit   = 0.0;
    e.max_consec_losses  = 0;
    e.min_atr_floor      = 0.0;
    e.max_atr_ceil       = 0.0;
    e.block_hour_start   = -1;
    e.block_hour_end     = -1;
    // Harness-fidelity: disable S63 PCT cuts (adverse-first synthetic tick
    // path would fire them on essentially every trade). Same as XAU harness.
    e.LOSS_CUT_PCT = 0.0;
    e.BE_ARM_PCT   = 0.0;
    e.BE_BUFFER_PCT= 0.0;
    e.init();
    return e;
}

omega::XauThreeBar30mEngine make_engine_protected() {
    // S35-P3 TUNED config (BE+trail+ATR floor+spread cap; no killswitch/
    // daily-cap/session-block) — verbatim from the XAU harness.
    omega::XauThreeBar30mEngine e;
    e.shadow_mode        = false;
    e.enabled            = true;
    e.long_only          = true;
    e.lot                = 0.01;
    e.max_spread         = 1.0;
    e.max_bars_held      = 0;
    e.be_trigger_atr     = 1.0;
    e.be_cost_buffer_pts = 0.10;
    e.trail_after_be     = true;
    e.trail_atr_mult     = 0.75;
    e.daily_loss_limit   = 0.0;
    e.max_consec_losses  = 0;
    e.min_atr_floor      = 0.30;
    e.max_atr_ceil       = 0.0;
    e.block_hour_start   = -1;
    e.block_hour_end     = -1;
    e.LOSS_CUT_PCT = 0.0;
    e.BE_ARM_PCT   = 0.0;
    e.BE_BUFFER_PCT= 0.0;
    e.init();
    return e;
}

// File-static gate instances (must outlive the engine objects).
static omega::XauM30HmmGate s_hmm_gate_combo{};

omega::XauThreeBar30mEngine make_engine_tuned_slope(int N) {
    // TUNED + N-bar close-slope sign-alignment gate (S88-followup).
    omega::XauThreeBar30mEngine e = make_engine_protected();
    e.slope_lookback_bars = N;
    e.use_slope_gate      = true;
    e.LOSS_CUT_PCT = 0.0;
    e.BE_ARM_PCT   = 0.0;
    e.BE_BUFFER_PCT= 0.0;
    e.init();
    return e;
}

omega::XauThreeBar30mEngine make_engine_tuned_hmm_slope(int N) {
    // TUNED + HMM not_NOISE gate + slope_N (recommended combined gate).
    omega::XauThreeBar30mEngine e = make_engine_protected();
    e.slope_lookback_bars = N;
    e.use_slope_gate      = true;
    e.use_hmm_gate        = true;
    e.hmm_gate            = &s_hmm_gate_combo;
    e.init();
    return e;
}

// ---------------------------------------------------------------------------
//  Stats
// ---------------------------------------------------------------------------
struct Stats {
    int n = 0, w = 0, l = 0;
    double wr = 0, pf = 0, net = 0, max_dd = 0;
    double half1_net = 0, half2_net = 0;
};

Stats compute_stats(const std::vector<double>& raw_pts, double cost_rt) {
    Stats s{};
    s.n = (int)raw_pts.size();
    double gw = 0, gl = 0, eq = 0, peak = 0;
    const std::size_t half = raw_pts.size() / 2;
    for (std::size_t i = 0; i < raw_pts.size(); ++i) {
        const double p = raw_pts[i] - cost_rt;
        if (p > 0) { s.w++; gw += p; } else if (p < 0) { s.l++; gl += -p; }
        s.net += p;
        if (i < half) s.half1_net += p; else s.half2_net += p;
        eq += p;
        if (eq > peak) peak = eq;
        if (peak - eq > s.max_dd) s.max_dd = peak - eq;
    }
    s.wr = s.n > 0 ? 100.0 * s.w / s.n : 0.0;
    s.pf = gl > 0 ? gw / gl : 0.0;
    return s;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    std::string csv_path = "/Users/jo/Tick/mgc_30m_hist.csv";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--csv" && i + 1 < argc) csv_path = argv[++i];
    }

    std::vector<Bar> bars;
    if (!load_mgc_csv(csv_path, bars)) return 1;
    if (bars.size() < 50) { std::fprintf(stderr, "[err] too few bars\n"); return 1; }
    std::fprintf(stderr, "[bt] window: ts %lld .. %lld\n",
                 (long long)bars.front().ts_unix, (long long)bars.back().ts_unix);

    std::vector<double> atr = compute_atr14(bars);

    // Silence engine [TRADE-COST-*]/[GUARD-*] stdout chatter.
    std::fflush(stdout);
    (void)!std::freopen("/dev/null", "w", stdout);

    struct Run { const char* name; BtResult res; };
    std::vector<Run> runs;
    {
        Run r{"baseline", {}};
        auto eng = make_engine_baseline();
        run_one(eng, bars, atr, r.res, r.name);
        runs.push_back(std::move(r));
    }
    {
        Run r{"TUNED (protected)", {}};
        auto eng = make_engine_protected();
        run_one(eng, bars, atr, r.res, r.name);
        runs.push_back(std::move(r));
    }
    {
        Run r{"TUNED+SLOPE12", {}};
        auto eng = make_engine_tuned_slope(12);
        run_one(eng, bars, atr, r.res, r.name);
        runs.push_back(std::move(r));
    }
    {
        Run r{"TUNED+HMM+SLOPE12", {}};
        auto eng = make_engine_tuned_hmm_slope(12);
        run_one(eng, bars, atr, r.res, r.name);
        runs.push_back(std::move(r));
    }

    const double COST_1X = 0.30, COST_2X = 0.60;

    std::fprintf(stderr, "\n## MGC ThreeBar30m port — faithful engine, 30m bars, "
                 "spread 0.30 pt, cost %.2f pt RT (stress %.2f)\n\n", COST_1X, COST_2X);
    std::fprintf(stderr, "| config | n | WR%% | PF | net pts | maxDD pts | "
                 "H1 net | H2 net | 2x-cost net |\n");
    std::fprintf(stderr, "|---|---|---|---|---|---|---|---|---|\n");
    for (auto& r : runs) {
        Stats s1 = compute_stats(r.res.trade_pts, COST_1X);
        Stats s2 = compute_stats(r.res.trade_pts, COST_2X);
        std::fprintf(stderr,
            "| %s | %d | %.1f | %.2f | %+.1f | %.1f | %+.1f | %+.1f | %+.1f |\n",
            r.name, s1.n, s1.wr, s1.pf, s1.net, s1.max_dd,
            s1.half1_net, s1.half2_net, s2.net);
    }
    std::fprintf(stderr, "\nNOTE: data window 2024-06..2026-07 has NO 2022 bear — "
                 "regime axis unavailable; both-halves split is the only robustness axis.\n");
    return 0;
}

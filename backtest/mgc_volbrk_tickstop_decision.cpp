// ─────────────────────────────────────────────────────────────────────────────
// mgc_volbrk_tickstop_decision.cpp  (S-2026-07-11 GOLD PHASE 1b, roadmap #6)
//
//   THE 3-VARIANT DECISION TEST for the MGC GoldVolBreakoutM30 instance's stop.
//   The MGC instance is bar-fed (poll_mgc_feed) so its on_tick() sl/trail was
//   DEAD CODE: cadence audit found 46/46 exits via MAX_HOLD. This harness drives
//   the REAL engine (include/GoldVolBreakoutM30Engine.hpp) in each stop_mode:
//
//     ORIGINAL     stop_mode=0 : honest no-stop -- exits MAX_HOLD only
//                                (== what the live MGC instance actually did)
//     PROTECTED    stop_mode=2 : the engine's own 1.5-ATR stop + 3-ATR trail
//                                enforced as a resting stop on the bar path
//                                (adverse-first, gap-honest fill min(open,sl))
//     CATASTROPHE  stop_mode=3 : ONLY a wide emergency stop at entry-K*ATR,
//                                K in {5,6,8}; MAX_HOLD still applies
//
//   Feed fidelity = poll_mgc_feed with the S-2026-07-11 ms fix: H1 closes
//   aggregated from 30m buckets (EMA200 trend gate), then
//   on_m30_bar(h,l,c, bid=c, ask=c, ts*1000, cb, open).
//
//   PnL: engine emits pnl=(exit-entry)*lot RAW pts; MGC $10/pt/contract, lot 1
//   -> USD = pts*10. Cost applied in post: 0.31 pt RT (spread 0.10 + comm
//   $2.08/RT/contract = 0.208pt + slip) and 0.62 (2x stress).
//
//   Axes: certified MGC 30m 2024-06..2026-06 (/Users/jo/Tick/mgc_30m_hist.csv)
//   + 2022 bear shadow on XAU M30 proxy at MGC cost (/Users/jo/Tick/XAU2022_m30.csv).
//
//   BUILD: c++ -std=c++17 -O2 -I include backtest/mgc_volbrk_tickstop_decision.cpp \
//            -o /tmp/mgc_volbrk_tickstop_decision
//   RUN:   /tmp/mgc_volbrk_tickstop_decision [csv=/Users/jo/Tick/mgc_30m_hist.csv]
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>

#include "GoldVolBreakoutM30Engine.hpp"

struct Bar { int64_t ts; double o,h,l,c; };

static std::vector<Bar> load(const char* path) {
    std::vector<Bar> out; std::ifstream f(path);
    if (!f.is_open()) { std::fprintf(stderr, "cannot open %s\n", path); return out; }
    std::string ln;
    while (std::getline(f, ln)) {
        if (ln.empty() || ln[0] < '0' || ln[0] > '9') continue;
        Bar b; char* e = nullptr; const char* s = ln.c_str();
        b.ts = std::strtoll(s, &e, 10); if (*e != ',') continue; ++e;
        b.o = std::strtod(e, &e); if (*e != ',') continue; ++e;
        b.h = std::strtod(e, &e); if (*e != ',') continue; ++e;
        b.l = std::strtod(e, &e); if (*e != ',') continue; ++e;
        b.c = std::strtod(e, &e);
        if (b.ts > 4000000000LL) b.ts /= 1000;           // ms -> s safety
        if (b.h >= b.l && b.o > 0 && b.c > 0) out.push_back(b);
    }
    return out;
}

struct Trade { int64_t ets, xts; double entry, exit; std::string reason; };

struct Stats {
    int n = 0, wins = 0, mh = 0, stp = 0;
    double net = 0, gw = 0, gl = 0, worst = 0, eq = 0, peak = 0, mdd = 0;
    double h1 = 0, h2 = 0;
};

static Stats summarize(const std::vector<Trade>& tr, double cost, int64_t mid_ts) {
    Stats s;
    for (auto& t : tr) {
        const double p = (t.exit - t.entry) - cost;
        ++s.n; s.net += p;
        if (p >= 0) { ++s.wins; s.gw += p; } else s.gl += -p;
        if (p < s.worst) s.worst = p;
        s.eq += p; if (s.eq > s.peak) s.peak = s.eq;
        if (s.peak - s.eq > s.mdd) s.mdd = s.peak - s.eq;
        if (t.ets < mid_ts) s.h1 += p; else s.h2 += p;
        if (t.reason.find("MAX_HOLD") != std::string::npos) ++s.mh; else ++s.stp;
    }
    return s;
}

// Drive the REAL engine in the given stop mode over the bar file, exactly as
// the (fixed) poll_mgc_feed does: H1 bucket close -> on_m30_bar(ms, open).
static std::vector<Trade> run_mode(const std::vector<Bar>& bars, int mode, double cat_k) {
    omega::GoldVolBreakoutM30Engine eng;
    eng.enabled = true; eng.lot = 1.0; eng.ledger_symbol = "MGC";
    eng.min_qty = 1.0; eng.qty_step = 1.0; eng.max_spread = 1.50;
    eng.stop_mode = mode; eng.cat_atr_mult = cat_k;
    eng.init(); eng.enabled = true;
    std::vector<Trade> out;
    auto cb = [&](const omega::TradeRecord& t) {
        out.push_back({t.entryTs, t.exitTs, t.entryPrice, t.exitPrice, t.exitReason});
    };
    int64_t h1_bucket = 0; double h1_close = 0.0;
    for (auto& b : bars) {
        const int64_t hb = (b.ts / 3600) * 3600;
        if (h1_bucket != 0 && hb != h1_bucket) eng.on_h1_close(h1_close);
        h1_bucket = hb; h1_close = b.c;
        eng.on_m30_bar(b.h, b.l, b.c, b.c, b.c, b.ts * 1000LL, cb, b.o);
    }
    return out;
}

static void report(const char* name, const std::vector<Trade>& tr,
                   double cost, int64_t mid_ts) {
    for (double c : {cost, 2.0 * cost}) {
        Stats s = summarize(tr, c, mid_ts);
        const double pf = s.gl > 0 ? s.gw / s.gl : (s.gw > 0 ? 99.9 : 0.0);
        std::printf("  %-22s cost=%.2f : n=%-3d WR=%4.1f%% net=%+8.1fpt ($%+9.0f) PF=%4.2f "
                    "H1=%+7.1f H2=%+7.1f worst=%+7.1f maxDD=%7.1f | exits MAX_HOLD=%d STOP=%d\n",
                    name, c, s.n, s.n ? 100.0 * s.wins / s.n : 0.0, s.net, s.net * 10.0, pf,
                    s.h1, s.h2, s.worst, s.mdd, s.mh, s.stp);
    }
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "/Users/jo/Tick/mgc_30m_hist.csv";
    auto bars = load(path);
    if (bars.size() < 500) { std::fprintf(stderr, "too few bars (%zu)\n", bars.size()); return 1; }
    const int64_t mid_ts = bars[bars.size() / 2].ts;
    const double cost = 0.31;   // MGC RT pts (spread 0.10 + comm 0.208 + slip)
    std::printf("# mgc_volbrk_tickstop_decision: %zu bars %s  span %lld..%lld  cost=%.2f/2x\n",
                bars.size(), path, (long long)bars.front().ts, (long long)bars.back().ts, cost);

    report("ORIGINAL (no stop)",   run_mode(bars, 0, 0.0), cost, mid_ts);
    report("PROTECTED (1.5ATR+tr)", run_mode(bars, 2, 0.0), cost, mid_ts);
    for (double k : {5.0, 6.0, 8.0})
        report(("CATASTROPHE " + std::to_string((int)k) + "ATR").c_str(),
               run_mode(bars, 3, k), cost, mid_ts);
    return 0;
}

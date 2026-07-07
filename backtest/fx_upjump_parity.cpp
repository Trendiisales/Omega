// fx_upjump_parity.cpp — registry §6 faithful-port PARITY TEST for
// FxUpJumpLadderCompanion vs backtest/omega_upjump_ladder_bt.py.
//
// Drives the PRODUCTION header standalone over the same Tick H1 CSVs with
// no-op exec fns and deploy_ts=0 (books everything), then compares
// net% / clips vs the python harness survivor cells:
//   EURUSD W48 thr0.5 : py +39.7%  n507  PF1.47
//   GBPUSD W48 thr1.0 : py +37.4%  n240  PF2.20
//   NZDUSD W24 thr1.5 : py +41.2%  n100  PF4.35
// Expected deviations: engine has no end-of-data flush (open legs reported as
// MTM), + the live gap guard blocks windows across multi-day data gaps.
//
// Build: g++ -std=c++17 -O2 -Iinclude -o /tmp/fx_upjump_parity backtest/fx_upjump_parity.cpp
// Run from a scratch dir (engine writes fxladder_companion_* persistence files).
#include "../include/FxUpJumpLadderCompanion.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>

struct Row { const char* pair; const char* csv; int W; double thr; double rt; double py_net; int py_n; };

int main() {
    const Row ROWS[] = {
        {"EURUSD", "/Users/jo/Tick/EURUSD_merged.h1.csv",   48, 0.5, 2.0, 39.7, 507},
        {"GBPUSD", "/Users/jo/Tick/GBPUSD_befloor_h1.csv",  48, 1.0, 2.0, 37.4, 240},
        {"NZDUSD", "/Users/jo/Tick/NZDUSD_befloor_h1.csv",  24, 1.5, 2.5, 41.2, 100},
    };
    for (const Row& r : ROWS) {
        omega::FxLadderPair::Config cfg;
        cfg.pair = r.pair; cfg.live_sym = r.pair;
        cfg.W = r.W; cfg.thr = r.thr; cfg.rt_cost_bp = r.rt;
        omega::FxLadderPair p(cfg);
        p.set_exec(
            [](const std::string&, bool, double, double) -> std::string { return ""; },
            [](const std::string&, bool, double, double, const std::string&) {},
            [](const std::string&, double, double) -> bool { return true; },
            [](const std::string&, const std::string&, bool, double, double, double, int64_t, int64_t, const char*) {});
        // deploy_ts stays 0 (no persisted anchor in a fresh scratch dir) -> everything books.
        std::ifstream f(r.csv);
        if (!f.is_open()) { std::printf("%s: MISSING %s\n", r.pair, r.csv); continue; }
        std::string line; size_t bars = 0;
        while (std::getline(f, line)) {
            if (line.empty() || !isdigit((unsigned char)line[0])) continue;
            double ts = 0, o = 0, h = 0, l = 0, c = 0;
            if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c) >= 5 && c > 0) {
                p.on_h1_bar((int64_t)ts, h, l, c); ++bars;
            }
        }
        const double booked = p.book_pct();
        const double open_mtm = p.open_mtm_pct();
        std::printf("%s W%d thr%.1f: bars=%zu  clips=%d  booked=%+.1f%%  open_mtm=%+.1f%%  total=%+.1f%%   (py %+.1f%% n%d)\n",
                    r.pair, r.W, r.thr, bars, p.total_clips(), booked, open_mtm, booked + open_mtm,
                    r.py_net, r.py_n);
    }
    return 0;
}

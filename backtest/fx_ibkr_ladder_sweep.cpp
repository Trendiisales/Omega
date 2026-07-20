// fx_ibkr_ladder_sweep.cpp — faithful FX up-jump ladder sweep on IBKR H1 data.
//
// Drives the REAL production engine class omega::FxLadderPair (the live in-binary
// engine) over one IBKR H1 CSV for ONE (W,thr,wide_arm,be_entry,wide_gb,short)
// config, capturing every BOOKED clip via the ledger callback, then computes the
// all-6 verdict basis: n, net%, PF, WF H1/H2 (by clip order, exactly the research
// stats() split), bull%/bear% (by trend regime at the clip's entry bar), + the
// open-leg MTM at last close (end-of-data flush the engine defers to window-end).
//
// deploy_ts=0 (fresh scratch cwd -> no persisted anchor) so every clip books, and
// the ledger_fn captures each booked clip (entry_px/exit_px/ts/is_long). pct is
// recomputed EXACTLY as book_clip_(): d*(exit/entry-1)*100 - rt_cost_bp/100.
//
// Regime label (bull/bear): the IBKR pull is a single ~3Y sample (no separate
// 2022-bear file), so regime is the pair's own trend at the clip entry bar:
// bull if entry close >= SMA(REGIME_N) of H1 closes, else bear. REGIME_N=480 H1
// bars (~1 trading month). Reported so the operator can judge the split.
//
// Build: g++ -std=c++17 -O2 -Iinclude -o /tmp/fx_ibkr_sweep backtest/fx_ibkr_ladder_sweep.cpp
// Run in a FRESH mktemp -d cwd (engine writes fxladder_companion_* persistence).
// argv: <csv> <W> <thr> <wide_arm> <be_entry> <wide_gb> <short 0|1> <rt_cost_bp>
#include "../include/FxMimicLadderCompanion.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>

static const int REGIME_N = 480;   // H1 bars for the bull/bear trend SMA (~1 month)

int main(int argc, char** argv) {
    if (argc < 9) { std::fprintf(stderr, "usage: csv W thr wide_arm be_entry wide_gb short rt\n"); return 2; }
    const std::string csv = argv[1];
    const int    W        = std::atoi(argv[2]);
    const double thr      = std::atof(argv[3]);
    const double wide_arm = std::atof(argv[4]);
    const double be_entry = std::atof(argv[5]);
    const double wide_gb  = std::atof(argv[6]);
    const bool   is_short = std::atoi(argv[7]) != 0;
    const double rt       = std::atof(argv[8]);

    // ---- load bars, build ts->regime(bull/bear) via SMA(REGIME_N) of closes ----
    std::vector<int64_t> ts; std::vector<double> cl;
    { std::ifstream f(csv);
      if (!f.is_open()) { std::fprintf(stderr, "MISSING %s\n", csv.c_str()); return 3; }
      std::string line;
      while (std::getline(f, line)) {
          if (line.empty() || !(std::isdigit((unsigned char)line[0]))) continue;
          double t=0,o=0,h=0,l=0,c=0;
          if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &t,&o,&h,&l,&c) >= 5 && c>0) {
              ts.push_back((int64_t)t); cl.push_back(c);
          }
      }
    }
    // ts -> bull(true)/bear(false)
    auto regime_at = [&](int64_t q)->int {
        // binary search q in ts
        size_t lo=0, hi=ts.size();
        while (lo<hi){ size_t m=(lo+hi)/2; if (ts[m]<q) lo=m+1; else hi=m; }
        if (lo>=ts.size()) lo=ts.size()-1;
        if (lo<(size_t)REGIME_N) return -1;   // not enough history -> unknown, exclude
        double s=0; for (size_t k=lo-REGIME_N; k<lo; ++k) s+=cl[k];
        double sma=s/REGIME_N;
        return cl[lo]>=sma ? 1 : 0;
    };

    // ---- capture booked clips ----
    struct Clip { double pct; int64_t ets; };
    std::vector<Clip> clips;
    const double d = is_short ? -1.0 : 1.0;

    omega::FxLadderPair::Config cfg;
    cfg.pair = "PAIR"; cfg.live_sym = "PAIR";
    cfg.W = W; cfg.thr = thr; cfg.rt_cost_bp = rt;
    cfg.wide_arm_pct = wide_arm; cfg.be_entry_pct = be_entry; cfg.wide_gb_frac = wide_gb;
    cfg.short_downjump = is_short;
    omega::FxLadderPair p(cfg);
    p.set_exec(
        [](const std::string&, bool, double, double) -> std::string { return ""; },
        [](const std::string&, bool, double, double, const std::string&) {},
        [](const std::string&, double, double) -> bool { return true; },
        [&](const std::string&, const std::string&, bool is_long, double entry_px, double exit_px,
            double, int64_t entry_ts, int64_t, const char*) {
            const double dd = is_long ? 1.0 : -1.0;
            const double pct = dd*(exit_px/entry_px - 1.0)*100.0 - rt/100.0;
            clips.push_back({pct, entry_ts});
        });

    { std::ifstream f(csv); std::string line;
      while (std::getline(f, line)) {
          if (line.empty() || !std::isdigit((unsigned char)line[0])) continue;
          double t=0,o=0,h=0,l=0,c=0;
          if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &t,&o,&h,&l,&c) >= 5 && c>0)
              p.on_h1_bar((int64_t)t, h, l, c);
      }
    }

    // ---- stats (research stats(): net, PF, WF halves by CLIP ORDER) ----
    const int n = (int)clips.size();
    double net=0, gw=0, gl=0, bull=0, bear=0;
    for (auto& c : clips) {
        net += c.pct;
        if (c.pct>0) gw += c.pct; else gl += -c.pct;
        int r = regime_at(c.ets);
        if (r==1) bull += c.pct; else if (r==0) bear += c.pct;
    }
    double pf = gl>1e-9 ? gw/gl : 99.9;
    int mid = n/2; double h1=0,h2=0;
    for (int i=0;i<n;++i){ if(i<mid) h1+=clips[i].pct; else h2+=clips[i].pct; }
    double open_mtm = p.open_mtm_pct();
    (void)d;

    // machine-readable single line
    std::printf("RESULT n=%d net=%.2f pf=%.2f h1=%.2f h2=%.2f bull=%.2f bear=%.2f open_mtm=%.2f total=%.2f\n",
                n, net, pf, h1, h2, bull, bear, open_mtm, net+open_mtm);
    return 0;
}

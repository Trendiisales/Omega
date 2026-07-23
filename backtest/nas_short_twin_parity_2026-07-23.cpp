// nas_short_twin_parity_2026-07-23.cpp — C++ PARITY CERT for the NAS100 SHORT twin
// (scratchpad/edge/nas_short_twin_sweep.py certified cell: bearHyst W24/thr2.5
// confirm=0.5*thr(1.25%) g70 tight-ON cap0 on NSXUSD_2022_2026.h1.csv rt3bp ->
// python n=345 +69.0% PF1.46 DD29.3 2022 +46.4 bull-era +22.6 WF+ 2x PF1.38).
// Drives the REAL production engine omega::FxLadderPair (FxMimicLadderCompanion.hpp)
// in its SHORT down-jump direction (cfg.short_downjump=true, d_=-1) — the
// backtest/ladder_reconfig_parity_2026-07-23.cpp pattern extended short-side.
//
// SHORT-SIDE ENGINE VERIFICATION (read before trusting):
//   * detector: d_-parameterized — close <= -thr% under the max HIGH of the W bars
//     before this one (live_step_ step 3). Sign-mirror of python precompute_triggers.
//   * pre_arm_floor_stop (S-23): flr = epx*(1 + d_*rt_bp/1e4) -> SHORT floor
//     epx*(1-RT), fires on a rally (d_*(px-flr)<=0 == px>=flr), gap-open fill when
//     bar OPEN is through it. NOT long-only — matches python C.floor=epx*(1-rt/100).
//   * BE-ENTRY: enters at trig*(1+d_*be/100) = trig*(1-confirm) short. Python books
//     the gap OPEN when o<=confirm (worse fill); engine books the confirm level
//     (optimistic there) — known delta 3 of the committed harness.
//   * reclip/Layer2/Layer3 all d_-parameterized.
//
// GIVEBACK MAPPING: python nas_short g_pct: gfrac=1-g/100, stop=entry-gfrac*(entry-
// trough) -> realized-at-stop = (1-g/100)*MFE. Engine: stop=entry+(1-gbf)*(peak-entry)
// -> engine wide_gb_frac = g/100. python "g70" = gb 0.70 (keep 30% of MFE — runner).
// Same convention as nas_ladder_sweep2 (see committed harness header).
//
// KNOWN engine-vs-python deltas (C++ = shippable truth), inherited + one new:
//   1. TIGHT always spawns -> the tight-OFF alt cell (n=276 +54.1 PF1.44) is NOT
//      representable; the tight-ON primary is the apples-to-apples cell.
//   2. Python clamps the ARMED stop to the floor (stop=min(stop,floor) short);
//      engine has no armed-floor clamp (S-20 honest ledger removed the BE clamp) —
//      bites only the TIGHT abs trail right after arming (~floor-vs-stop bp).
//   3. BE-ENTRY confirm-level fill vs python gap-open fill (engine optimistic);
//      trail/LC gap-throughs book worse-of (engine MORE pessimistic than python's
//      resting-level fill: python books the stop LEVEL on an h>=stop pierce, the
//      engine books worse-of(level, adverse extreme in seq h->l->c)).
//   4. cap counts BATCHES: engine cap=1 == python cap0 (no reclips).
//   5. pend_bars=W == python pend-till-window-flush (cancel coincides with flush).
//
// Gate bearhyst — python build_gates() hysteresis port: 200SMA of the daily CSV's
// own closes (rows only once >=200 closes), state ON (bear, shorts allowed) when
// close < MA*0.99, OFF when close > MA*1.01; per-H1-bar = latest daily row STRICTLY
// before the bar's UTC day start (prior-day, no look-ahead). Daily series: scratchpad
// ndx_canonext.csv == /Users/jo/Tick/NDX_daily_2016_2026.csv closes (verified 0
// mismatches over all 2653 rows), so either file yields the identical gate.
//
// Build: g++ -std=c++17 -O2 -Iinclude -o /tmp/nas_short_parity backtest/nas_short_twin_parity_2026-07-23.cpp
// Run:   /tmp/nas_short_parity <h1_csv> <ndx_daily_csv> <W> <thr> <rt_bp> <gb_frac>
//                              <be_entry> <pend> <cap> <wknd 0|1> <carry> <workdir> <tag>
// (books 1x AND 2x cost internally; one RESULT line per run.)
#include "../include/FxMimicLadderCompanion.hpp"
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

struct Bar { int64_t ts; double o, h, l, c; };

static std::vector<Bar> load_bars(const std::string& path) {
    std::vector<Bar> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || !std::isdigit((unsigned char)line[0])) continue;
        double t=0,o=0,h=0,l=0,c=0;
        if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &t,&o,&h,&l,&c) == 5 && c > 0) {
            int64_t tt = (int64_t)t; if (tt >= 100000000000LL) tt /= 1000;
            out.push_back({tt,o,h,l,c});
        }
    }
    return out;
}

// bear-hysteresis gate — python nas_short_twin_sweep.build_gates()["bearHyst"] exact port.
static std::vector<char> gate_bearhyst(const std::vector<Bar>& b, const std::string& daily_csv) {
    std::vector<std::pair<int64_t,char>> rows;   // (ts, bear-state) once >=200 closes
    { std::ifstream f(daily_csv); std::string line;
      std::vector<double> closes; double s = 0.0; bool state = false;
      while (std::getline(f, line)) {
          if (line.empty() || !std::isdigit((unsigned char)line[0])) continue;
          double t=0,o=0,h=0,l=0,c=0;
          if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &t,&o,&h,&l,&c) != 5 || c <= 0) continue;
          closes.push_back(c); s += c;
          if ((int)closes.size() > 200) s -= closes[closes.size()-201];
          if ((int)closes.size() >= 200) {
              const double m = s / 200.0;
              if      (c < m * 0.99) state = true;    // bear ON  (shorts allowed)
              else if (c > m * 1.01) state = false;   // bear OFF
              rows.push_back({(int64_t)t, (char)state});
          }
      } }
    std::vector<char> g(b.size(), 0);
    for (size_t i = 0; i < b.size(); ++i) {
        const int64_t ds = (b[i].ts / 86400) * 86400;
        size_t lo = 0, hi = rows.size();   // latest row STRICTLY before day start (no look-ahead)
        while (lo < hi) { size_t m = (lo+hi)/2; if (rows[m].first < ds) lo = m+1; else hi = m; }
        g[i] = (lo > 0) ? rows[lo-1].second : 0;
    }
    return g;
}

struct Rec { double pct; int64_t ets, xts; };
struct RunOut { std::vector<Rec> recs; double open_mtm = 0.0; };

static RunOut run_engine(const std::vector<Bar>& bars, int W, double thr, double rt,
                         double gb, double be, int pend, int cap,
                         const std::vector<char>* allow, bool wknd, double carry,
                         const std::string& workdir, const std::string& prefix) {
    namespace fs = std::filesystem;
    fs::create_directories(workdir);
    omega::FxLadderPair::Config cfg;
    cfg.pair = "NASSHORT"; cfg.live_sym = "NASSHORT";
    cfg.short_downjump = true;                // d_=-1: the DOWN-JUMP short mirror
    cfg.W = W; cfg.thr = thr; cfg.rt_cost_bp = rt;
    cfg.wide_gb_frac = gb; cfg.wide_arm_pct = -1.0;   // legacy 2.7*thr engage (python a_w)
    cfg.be_entry_pct = be; cfg.pend_bars = pend; cfg.cap = cap;
    cfg.pre_arm_floor_stop = true;            // python floored arch: pre-arm stop at epx*(1-RT) short
    cfg.be_floor_on_open = true;              // family marker only (booking-inert since S-20 honest ledger)
    cfg.block_weekend_arms = wknd; cfg.weekend_carry_frac = carry;
    cfg.catchup_max_age_bars = 0;
    cfg.deploy_path = workdir + "/" + prefix + "deploy.txt";
    cfg.bars_path   = workdir + "/" + prefix + "h1.csv";
    cfg.book_path   = workdir + "/" + prefix + "book.txt";
    cfg.live_path   = workdir + "/" + prefix + "live.txt";
    cfg.closed_path = workdir + "/" + prefix + "closed.csv";
    for (const auto& p : {cfg.deploy_path, cfg.bars_path, cfg.book_path, cfg.live_path, cfg.closed_path})
        std::remove(p.c_str());
    static size_t cur_i; cur_i = 0;
    if (allow) cfg.block_new_windows_fn = [allow]() { return !(*allow)[cur_i]; };
    omega::FxLadderPair pair(cfg);
    pair.set_exec(
        [](const std::string&, bool, double, double) -> std::string { return "x"; },
        [](const std::string&, bool, double, double, const std::string&) {},
        [](const std::string&, double, double) -> bool { return true; },
        [](const std::string&, const std::string&, bool, double, double, double, int64_t, int64_t, const char*) {});
    for (size_t i = 0; i < bars.size(); ++i) {
        cur_i = i;
        pair.on_h1_bar(bars[i].ts, bars[i].h, bars[i].l, bars[i].c, bars[i].o);
    }
    RunOut out;
    out.open_mtm = pair.open_mtm_pct();
    pair.kill_all(bars.back().ts);            // end-of-data flush at last close = python EOF flush
    { std::ifstream f(cfg.closed_path); std::string line;
      while (std::getline(f, line)) {
          int ti=0; double e=0,x=0,pct=0,usd=0; long long ets=0,xts=0;
          if (std::sscanf(line.c_str(), "%d,%lf,%lf,%lf,%lf,%lld,%lld", &ti,&e,&x,&pct,&usd,&ets,&xts) == 7)
              out.recs.push_back({pct, (int64_t)ets, (int64_t)xts});
      } }
    return out;
}

struct Stats { int n=0, nNeg=0, n22=0; double net=0, pf=0, dd=0, y22=0, y2326=0, h1=0, h2=0, hd1=0, hd2=0, worst=0; };

static Stats stats_of(const std::vector<Rec>& r, int64_t half_ts) {
    Stats s; if (r.empty()) return s;
    s.n = (int)r.size();
    double gw=0, gl=0, cur=0, peak=0;
    const int mid = s.n / 2;
    for (int i = 0; i < s.n; ++i) {
        const double p = r[i].pct;
        s.net += p; if (p > 0) gw += p; else { gl += -p; if (p < -1e-9) ++s.nNeg; }
        cur += p; if (cur > peak) peak = cur; if (peak - cur > s.dd) s.dd = peak - cur;
        if (p < s.worst) s.worst = p;
        if (i < mid) s.h1 += p; else s.h2 += p;                       // python n//2 booking-order split
        if (r[i].xts < half_ts) s.hd1 += p; else s.hd2 += p;          // date-range half split
        if (r[i].xts >= 1640995200LL && r[i].xts < 1672531200LL) { s.y22 += p; ++s.n22; }
        if (r[i].xts >= 1672531200LL) s.y2326 += p;                   // bull-era slice (2023-26)
    }
    s.pf = gl > 1e-9 ? gw/gl : 99.9;
    return s;
}

int main(int argc, char** argv) {
    if (argc < 14) {
        std::fprintf(stderr, "usage: h1_csv ndx_daily_csv W thr rt_bp gb_frac be_entry pend cap wknd carry workdir tag\n");
        return 2;
    }
    const std::string csv  = argv[1];
    const std::string dcsv = argv[2];
    const int    W     = std::atoi(argv[3]);
    const double thr   = std::atof(argv[4]);
    const double rt    = std::atof(argv[5]);
    const double gb    = std::atof(argv[6]);
    const double be    = std::atof(argv[7]);
    const int    pend  = std::atoi(argv[8]);
    const int    cap   = std::atoi(argv[9]);
    const bool   wknd  = std::atoi(argv[10]) != 0;
    const double carry = std::atof(argv[11]);
    const std::string workdir = argv[12];
    const std::string tag     = argv[13];

    const std::vector<Bar> bars = load_bars(csv);
    if ((int)bars.size() < W + 2) { std::fprintf(stderr, "TOO_FEW_BARS %zu\n", bars.size()); return 4; }
    const std::vector<char> allow = gate_bearhyst(bars, dcsv);
    { size_t nopen = 0; for (char v : allow) nopen += (v != 0);
      std::fprintf(stderr, "gate bearHyst: %zu/%zu H1 bars open\n", nopen, allow.size()); }

    const int64_t half_ts = (bars.front().ts + bars.back().ts) / 2;
    RunOut r1 = run_engine(bars, W, thr, rt,     gb, be, pend, cap, &allow, wknd, carry, workdir, tag + "_1x_");
    RunOut r2 = run_engine(bars, W, thr, rt*2.0, gb, be, pend, cap, &allow, wknd, carry, workdir, tag + "_2x_");
    const Stats s  = stats_of(r1.recs, half_ts);
    const Stats s2 = stats_of(r2.recs, half_ts);

    std::printf("RESULT tag=%s W=%d thr=%.2f rt=%.1f gb=%.2f be=%.2f pend=%d cap=%d wknd=%d carry=%.1f | "
                "n=%d net=%.1f pf=%.3f dd=%.1f y22=%.1f(n%d) y2326=%.1f wf_clip=%.1f/%.1f wf_date=%.1f/%.1f "
                "worst=%.2f nNeg=%d openMTM=%.2f | 2x: n=%d net=%.1f pf=%.3f\n",
                tag.c_str(), W, thr, rt, gb, be, pend, cap, wknd ? 1 : 0, carry,
                s.n, s.net, s.pf, s.dd, s.y22, s.n22, s.y2326, s.h1, s.h2, s.hd1, s.hd2,
                s.worst, s.nNeg, r1.open_mtm, s2.n, s2.net, s2.pf);
    return 0;
}

// ladder_reconfig_parity_2026-07-23.cpp — FAITHFUL C++ PARITY CERTS for the 4 ladder
// reconfigs of backtest/BULLGATE_PROTECTION_SWEEPS_2026-07-23.md (build queue items 1-4),
// driving the REAL production engine class omega::FxLadderPair (FxMimicLadderCompanion.hpp)
// over certified H1 CSVs — the backtest/index_upjump_ladder_cadence_sweep.cpp pattern.
//
// Python search semantics being reproduced (scratchpad sweep_ladder_h1.py +
// nas_ladder_sweep2.py). GIVEBACK MAPPING (resolved, per engine header L112):
//   * sweep_ladder_h1.py:  stop = entry + g_py*(peak-entry)      -> g_py = KEEP fraction.
//   * engine wide_gb_frac: stop = entry + (1-frac)*(peak-entry)  -> frac = GIVEBACK fraction.
//   => engine wide_gb_frac = 1 - g_py.  python "g70" (keep 70%, give back 30%) = frac 0.30;
//      python "g50" = frac 0.50. (nas_ladder_sweep2.py's g is already a giveback %, = frac.)
//
// KNOWN engine-vs-python mechanism deltas (the C++ number is the shippable truth):
//   1. TIGHT leg always spawns (engine has no tight-off switch) — the NAS python cert rows
//      are tight=OFF; the apples-to-apples python reference is the tight=ON row.
//   2. No pre-arm BE-floor stop: engine pre-arm protection is LOSS_CUT 5*thr below entry
//      (python "floored" arch stops pre-arm legs at epx*(1+RT)).
//   3. BE-ENTRY fills AT the confirm level (python floored books the NEXT-bar OPEN on a gap
//      through confirm — engine is optimistic there); trail/LC gap-throughs book worse-of
//      (stop level vs bar LOW — engine is MORE pessimistic than python's open fill).
//   4. No armed-stop BE clamp (S-2026-07-20 honest ledger removed it). Inert for the g-trail
//      (entry + (1-frac)*(peak-entry) >= entry by construction); only the TIGHT abs trail
//      could have used the python befloor clamp.
//   5. Engine cap counts BATCHES (base=1): engine cap=1 == python nas_sweep2 cap0 (no
//      reclips); engine cap=5 == sweep_ladder_h1 cap5 (base + 4 reclips).
// End-of-data: kill_all() flushes remaining ENTERED legs at the last close = python's
// end-of-file flush; pending legs drop unbooked (both sides).
//
// Gates (python-exact, applied at window TRIGGER only via block_new_windows_fn):
//   none      — ungated.
//   dma200h1  — sweep_ladder_h1.gates_for(): current H1 close > SMA200 of COMPLETED daily
//               closes derived from the H1 file itself.
//   volcalm   — sweep_ladder_h1: pstdev(last 24 H1 rets) <= pstdev(last 240), incl. current bar.
//   daily200  — nas_ladder_sweep2: external daily CSV; gate = latest daily row STRICTLY before
//               the bar's UTC day start has close > its own 200DMA (prior-day, no look-ahead).
//
// Build: g++ -std=c++17 -O2 -Iinclude -o /tmp/ladder_parity backtest/ladder_reconfig_parity_2026-07-23.cpp
// Run:   /tmp/ladder_parity <csv> <W> <thr> <rt_bp> <gb_frac> <wide_arm> <be_entry> <pend_bars>
//                           <cap> <gate> <daily_csv|-> <wknd 0|1> <carry> <workdir> <tag>
// (books at 1x AND 2x cost internally; one RESULT line per run). Persistence goes to <workdir>
// (fresh files each invocation). See backtest/ladder_reconfig_parity_2026-07-23.sh for the
// exact per-cert invocations.
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

// gate arrays — python-exact ports (see header)
static std::vector<char> gate_dma200h1(const std::vector<Bar>& b) {
    std::vector<char> g(b.size(), 0);
    std::vector<double> day_closes; double run_sum = 0.0; int64_t cur_day = -1;
    for (size_t i = 0; i < b.size(); ++i) {
        const int64_t d = b[i].ts / 86400;
        if (cur_day < 0) cur_day = d;
        if (d != cur_day) {
            day_closes.push_back(b[i-1].c); run_sum += b[i-1].c;
            if ((int)day_closes.size() > 200) run_sum -= day_closes[day_closes.size()-201];
            cur_day = d;
        }
        if ((int)day_closes.size() >= 200) g[i] = b[i].c > run_sum / 200.0;
    }
    return g;
}
static std::vector<char> gate_volcalm(const std::vector<Bar>& b) {
    std::vector<char> g(b.size(), 0);
    std::vector<double> rets; double prev_c = 0.0;
    for (size_t i = 0; i < b.size(); ++i) {
        if (prev_c > 0) rets.push_back(b[i].c / prev_c - 1.0);
        prev_c = b[i].c;
        if ((int)rets.size() >= 240) {
            const size_t n = rets.size();
            double ms=0, mL=0;
            for (size_t k=n-24;  k<n; ++k) ms += rets[k]; ms /= 24.0;
            for (size_t k=n-240; k<n; ++k) mL += rets[k]; mL /= 240.0;
            double vs=0, vL=0;
            for (size_t k=n-24;  k<n; ++k) vs += (rets[k]-ms)*(rets[k]-ms);
            for (size_t k=n-240; k<n; ++k) vL += (rets[k]-mL)*(rets[k]-mL);
            g[i] = std::sqrt(vs/24.0) <= std::sqrt(vL/240.0);
        }
    }
    return g;
}
static std::vector<char> gate_daily200(const std::vector<Bar>& b, const std::string& daily_csv) {
    std::vector<std::pair<int64_t,char>> rows;   // (ts, close > own 200DMA), once >=200 closes
    { std::ifstream f(daily_csv); std::string line;
      std::vector<double> closes; double s = 0.0;
      while (std::getline(f, line)) {
          if (line.empty() || !std::isdigit((unsigned char)line[0])) continue;
          double t=0,o=0,h=0,l=0,c=0;
          if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &t,&o,&h,&l,&c) != 5 || c <= 0) continue;
          closes.push_back(c); s += c;
          if ((int)closes.size() > 200) s -= closes[closes.size()-201];
          if ((int)closes.size() >= 200) rows.push_back({(int64_t)t, c > s/200.0});
      } }
    std::vector<char> g(b.size(), 0);
    for (size_t i = 0; i < b.size(); ++i) {
        const int64_t ds = (b[i].ts / 86400) * 86400;
        // latest row STRICTLY before the bar's day start (prior-day close, no look-ahead)
        size_t lo = 0, hi = rows.size();
        while (lo < hi) { size_t m = (lo+hi)/2; if (rows[m].first < ds) lo = m+1; else hi = m; }
        g[i] = (lo > 0) ? rows[lo-1].second : 0;
    }
    return g;
}

struct Rec { double pct; int64_t ets, xts; };

struct RunOut { std::vector<Rec> recs; double open_mtm = 0.0; };

static RunOut run_engine(const std::vector<Bar>& bars, int W, double thr, double rt,
                         double gb, double warm, double be, int pend, int cap,
                         const std::vector<char>* allow, bool wknd, double carry,
                         const std::string& workdir, const std::string& prefix) {
    namespace fs = std::filesystem;
    fs::create_directories(workdir);
    omega::FxLadderPair::Config cfg;
    cfg.pair = "CERT"; cfg.live_sym = "CERT";
    cfg.W = W; cfg.thr = thr; cfg.rt_cost_bp = rt;
    cfg.wide_gb_frac = gb; cfg.wide_arm_pct = warm;
    cfg.be_entry_pct = be; cfg.pend_bars = pend; cfg.cap = cap;
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
    static size_t cur_i; cur_i = 0;           // gate index for the bar currently being processed
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
    out.open_mtm = pair.open_mtm_pct();       // pre-flush MTM (info only)
    pair.kill_all(bars.back().ts);            // end-of-data flush at last close = python EOF flush
    // read back the engine's own booked record (includes cost + Layer-3 weekend adj)
    { std::ifstream f(cfg.closed_path); std::string line;
      while (std::getline(f, line)) {
          int ti=0; double e=0,x=0,pct=0,usd=0; long long ets=0,xts=0;
          if (std::sscanf(line.c_str(), "%d,%lf,%lf,%lf,%lf,%lld,%lld", &ti,&e,&x,&pct,&usd,&ets,&xts) == 7)
              out.recs.push_back({pct, (int64_t)ets, (int64_t)xts});
      } }
    return out;
}

struct Stats { int n=0, nNeg=0, n22=0; double net=0, pf=0, dd=0, y22=0, h1=0, h2=0, hd1=0, hd2=0, worst=0; };

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
    }
    s.pf = gl > 1e-9 ? gw/gl : 99.9;
    return s;
}

int main(int argc, char** argv) {
    if (argc < 16) {
        std::fprintf(stderr, "usage: csv W thr rt_bp gb_frac wide_arm be_entry pend_bars cap "
                             "gate{none|dma200h1|volcalm|daily200} daily_csv|- wknd carry workdir tag\n");
        return 2;
    }
    const std::string csv = argv[1];
    const int    W     = std::atoi(argv[2]);
    const double thr   = std::atof(argv[3]);
    const double rt    = std::atof(argv[4]);
    const double gb    = std::atof(argv[5]);
    const double warm  = std::atof(argv[6]);
    const double be    = std::atof(argv[7]);
    const int    pend  = std::atoi(argv[8]);
    const int    cap   = std::atoi(argv[9]);
    const std::string gate  = argv[10];
    const std::string dcsv  = argv[11];
    const bool   wknd  = std::atoi(argv[12]) != 0;
    const double carry = std::atof(argv[13]);
    const std::string workdir = argv[14];
    const std::string tag     = argv[15];

    const std::vector<Bar> bars = load_bars(csv);
    if ((int)bars.size() < W + 2) { std::fprintf(stderr, "TOO_FEW_BARS %zu\n", bars.size()); return 4; }

    std::vector<char> allow;
    if      (gate == "dma200h1") allow = gate_dma200h1(bars);
    else if (gate == "volcalm")  allow = gate_volcalm(bars);
    else if (gate == "daily200") {
        if (dcsv == "-") { std::fprintf(stderr, "daily200 needs daily_csv\n"); return 5; }
        allow = gate_daily200(bars, dcsv);
    } else if (gate != "none") { std::fprintf(stderr, "unknown gate %s\n", gate.c_str()); return 6; }
    const std::vector<char>* ap = allow.empty() ? nullptr : &allow;

    const int64_t half_ts = (bars.front().ts + bars.back().ts) / 2;
    RunOut r1 = run_engine(bars, W, thr, rt,     gb, warm, be, pend, cap, ap, wknd, carry, workdir, tag + "_1x_");
    RunOut r2 = run_engine(bars, W, thr, rt*2.0, gb, warm, be, pend, cap, ap, wknd, carry, workdir, tag + "_2x_");
    const Stats s  = stats_of(r1.recs, half_ts);
    const Stats s2 = stats_of(r2.recs, half_ts);

    std::printf("RESULT tag=%s W=%d thr=%.2f rt=%.1f gb=%.2f arm=%.2f be=%.2f pend=%d cap=%d gate=%s wknd=%d carry=%.1f | "
                "n=%d net=%.1f pf=%.3f dd=%.1f y22=%.1f(n%d) wf_clip=%.1f/%.1f wf_date=%.1f/%.1f "
                "worst=%.2f nNeg=%d openMTM=%.2f | 2x: n=%d net=%.1f pf=%.3f\n",
                tag.c_str(), W, thr, rt, gb, warm, be, pend, cap, gate.c_str(), wknd ? 1 : 0, carry,
                s.n, s.net, s.pf, s.dd, s.y22, s.n22, s.h1, s.h2, s.hd1, s.hd2,
                s.worst, s.nNeg, r1.open_mtm, s2.n, s2.net, s2.pf);
    return 0;
}

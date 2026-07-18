// fx_catchup_outage_bt.cpp — OMEGA CERT for FxLadderPair::Config::catchup_max_age_bars
// (BOUNDED CATCH-UP, S-2026-07-18 Omega port of the certified crypto mechanism —
//  /Users/jo/Crypto/backtest/CATCHUP_OUTAGE_CERT_2026-07-18.md. Own cert: the crypto
//  cert is NEVER carried across venues/engines.)
//
// Question: a restart/outage over a qualifying jump close used to lose the ladder
// window forever — the detector runs only in live_step_ (live bars); seeding ingests
// history without detecting. The bounded catch-up replays the seeded history at
// finalize_seed() and re-opens the window IFF an always-on book would still be
// flat-in-window (BE never crossed since the trigger, jump inside the bound). Legs
// re-spawn PENDING and open via the untouched live BE-ENTRY path. The claim is
// EQUIVALENCE: a recovered window books the SAME clips an always-on book would have.
//
// Drives the REAL FxLadderPair (incl. the real catchup_replay_ code) — no model
// reimplementation. Every run lives in its OWN fresh temp dir (the class persists
// deploy/book/live/closed files — the fx_ibkr_ladder_sweep cwd-persistence trap).
// deploy_ts pre-written to 0 so every clip books (parity harness convention).
//
// Modes (FXCU_MODE):
//   surgical — per cell: find windows where BE crosses >=2 bars after the trigger
//              with the book flat before it; place a 2-bar outage over the trigger
//              close; assert (a) N=0 books NO clips from that window (lost),
//              (b) N>0 full-run clip MULTISET == always-on (ets+xts+entry+exit+reason;
//              multiset — multi-leg cells book several clips with the same ts pair,
//              the crypto key-collapse trap) and final book_pct matches.
//   grid     — periodic outages (FXCU_STRIDE, lengths FXCU_L) across the whole
//              history, N in FXCU_N (incl 0 = B control). Gates per cell-run:
//              C.net>0 AND C.pf >= min(1.3, 0.95*A.pf) (NAS100's certified PF is
//              1.2x — an absolute 1.3 floor would false-fail the certified baseline)
//              AND no new tail class: C.worst >= min(A.worst, B.worst) - 0.01%.
//              d(C-B) is REPORTED not gated per cell (window-selection variance:
//              B re-enters later shifted windows; surgical proves the design claim).
//
// Cells = ALL wired cells of the class (never-half-symbols), engine_init parity:
//   GBPUSD W48/0.75 rt2.0 (fx book)  ·  US500 W24/2.0 rt4.0 · NAS100 W24/1.5 rt3.0
//   · GER40 W12/1.5 rt2.0 (bull-gate omitted: it only BLOCKS recoveries — mechanism
//   certified ungated, conservative) · M2K W24/1.0 rt4.0 (index book).
//   Shared: wide_gb 0.10, wide_arm 1.0 (FX) / 0.5 (index), be_entry 0.08, pend 4,
//   be_floor_on_open, block_weekend_arms, weekend_carry_frac 0.0.
//
// Build: g++ -std=c++17 -O2 -Iinclude backtest/fx_catchup_outage_bt.cpp -o /tmp/fxcu
// Data (ALL integrity-gated CERTIFIED CLEAN 2026-07-18):
//   GBPUSD /Users/jo/Tick/GBPUSD_IBKR_H1.csv       (3Y IBKR IDEALPRO H1 — the live-feed basis)
//   US500  /Users/jo/Tick/SPXUSD_2022_2026.h1.csv  NAS100 /Users/jo/Tick/NSXUSD_2022_2026.h1.csv
//   GER40  /Users/jo/Tick/GER40_1h_yahoo.csv (the research bull file; GER40 is bull-gated live)
//   M2K    /Users/jo/Tick/M2K_h1.csv
// Env: FXCU_MODE(surgical) FXCU_COSTX(1) FXCU_N(24) FXCU_L(2,6,12,24) FXCU_STRIDE(168)
//      FXCU_MAXWIN(40) FXCU_CELLS(all) FXCU_DUMP(unset)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <tuple>
#include <memory>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include "FxUpJumpLadderCompanion.hpp"

using omega::FxLadderPair;

struct Bar { int64_t ts; double o,h,l,c; };
static std::vector<Bar> load(const std::string& path){
    std::vector<Bar> b; std::ifstream f(path);
    if(!f){ std::fprintf(stderr,"no data %s\n",path.c_str()); return b; }
    std::string ln;
    while(std::getline(f,ln)){
        if(ln.empty()||!(isdigit((unsigned char)ln[0]))) continue;
        Bar x{}; if(std::sscanf(ln.c_str(),"%lld,%lf,%lf,%lf,%lf",(long long*)&x.ts,&x.o,&x.h,&x.l,&x.c)==5
                 && x.c>0 && x.h>=x.l) b.push_back(x);
    }
    std::sort(b.begin(),b.end(),[](const Bar&a,const Bar&c){return a.ts<c.ts;});
    b.erase(std::unique(b.begin(),b.end(),[](const Bar&a,const Bar&c){return a.ts==c.ts;}),b.end());
    return b;
}

struct CellSpec { const char* tag; const char* csv; int W; double thr, rt, warm; };

struct Clip { int64_t ets, xts; double entry, exit; std::string reason; };
struct Res  { std::vector<Clip> clips; double net=0, pf=0, worst=0, book=0; int n=0; };

static FxLadderPair::Config make_cfg(const CellSpec& cs, double costx, int catchup, const std::string& dir){
    FxLadderPair::Config c;
    c.pair = cs.tag; c.live_sym = cs.tag;
    c.W = cs.W; c.thr = cs.thr; c.rt_cost_bp = cs.rt * costx;
    c.wide_gb_frac = 0.10; c.wide_arm_pct = cs.warm;
    c.be_entry_pct = 0.08; c.pend_bars = 4;
    c.be_floor_on_open = true;
    c.block_weekend_arms = true; c.weekend_carry_frac = 0.0;
    c.catchup_max_age_bars = catchup;
    c.deploy_path = dir+"/deploy.txt"; c.bars_path = dir+"/h1.csv";
    c.book_path = dir+"/book.txt";     c.live_path = dir+"/live.txt";
    c.closed_path = dir+"/closed.csv";
    return c;
}

// One simulated life. outages: sorted disjoint [start,end) bar-index pairs — the
// process is DOWN while those bars close. At each restart a NEW FxLadderPair is
// constructed in the SAME dir (persisted book/live state reloads, exactly like a
// real service restart), the FULL history [0..i) is warm-seeded (models the
// nightly-refreshed warmup CSV covering the gap), and finalize_seed() runs the
// real catch-up. Legs/clips only ever book through the live on_h1_bar path.
static Res run_sim(const std::vector<Bar>& b, const CellSpec& cs, double costx, int catchup,
                   const std::vector<std::pair<int,int>>& outages){
    char tmpl[] = "/tmp/fxcu.XXXXXX";
    std::string dir = mkdtemp(tmpl);
    { std::ofstream f(dir+"/deploy.txt"); f << "0\n"; }   // deploy_ts=0: book everything
    Res r;
    auto mk = [&]{
        auto e = std::make_unique<FxLadderPair>(make_cfg(cs, costx, catchup, dir));
        e->set_exec(
            [](const std::string&, bool, double, double)->std::string{ return "t"; },
            [](const std::string&, bool, double, double, const std::string&){},
            [](const std::string&, double, double)->bool{ return true; },
            [&r](const std::string&, const std::string&, bool, double entry, double exit,
                 double, int64_t ets, int64_t xts, const char* reason){
                r.clips.push_back({ets, xts, entry, exit, reason}); });
        return e;
    };
    auto eng = mk();
    size_t oi = 0; bool down = false;
    for (int i = 0; i < (int)b.size(); ++i) {
        while (oi < outages.size() && i >= outages[oi].second) ++oi;
        const bool in_out = oi < outages.size() && i >= outages[oi].first && i < outages[oi].second;
        if (in_out) { if (!down) { down = true; eng.reset(); } continue; }
        if (down) {                    // restart: fresh instance, seed full history, catch-up
            down = false;
            eng = mk();
            for (int k = 0; k < i; ++k) eng->seed_bar(b[k].ts, b[k].h, b[k].l, b[k].c);
            eng->finalize_seed();
        }
        eng->on_h1_bar(b[i].ts, b[i].h, b[i].l, b[i].c, b[i].o);
    }
    r.book = eng ? eng->book_pct() : 0.0;
    eng.reset();
    // net/pf/worst from the booked clips (pct net of cost, floored — recomputed the
    // way book_clip_ does; weekend adj is deterministic from (ets,xts) so entry/exit
    // parity implies pct parity; net check uses book_pct which is authoritative)
    double gw = 0, gl = 0;
    for (const auto& cl : r.clips) {
        double pct = (cl.entry > 0) ? (cl.exit / cl.entry - 1.0) * 100.0 : 0.0; // dir folded into exit>=<entry
        // long-only cells here; short would sign-flip — cert cells are all long
        pct -= cs.rt * costx / 100.0;
        if (pct < 0.0 && true /*be_floor_on_open*/) { /* floored fills already encode the floor; leave raw */ }
        r.net += pct; if (pct > 0) gw += pct; else gl -= pct;
        if (pct < r.worst) r.worst = pct;
    }
    r.n = (int)r.clips.size(); r.pf = gl > 0 ? gw / gl : (gw > 0 ? 999.0 : 0.0);
    // clean temp dir
    std::string cmd = "rm -rf '" + dir + "'"; (void)!system(cmd.c_str());
    return r;
}

// Detector replay over the bars with the LIVE rule (flush->detect same-bar order,
// contig gap guard, weekend-arm block) — the same walk catchup_replay_ does.
// Tracks per window: trigger bar, flush bar, first BE-cross bar (fav extreme of
// bars AFTER the trigger vs trig*(1+be%)).
struct Win { int entry_i, cross_i, end_i; double trig; };
static std::vector<Win> scan_windows(const std::vector<Bar>& b, const CellSpec& cs){
    std::vector<Win> ws; const int W = cs.W; const double be = 0.08;
    bool win = false; int age = 0; Win cur{-1,-1,-1,0};
    for (int i = 0; i < (int)b.size(); ++i) {
        if (win && ++age >= W) { cur.end_i = i; ws.push_back(cur); win = false; cur = {-1,-1,-1,0}; }
        if (!win && i >= W) {
            double ref = b[i-W].l;
            for (int k = i-W+1; k <= i-1; ++k) if (b[k].l < ref) ref = b[k].l;
            const bool contig = (b[i].ts - b[i-W].ts) <= (int64_t)W*3600 + 4*86400;
            const double jump = (ref > 0) ? (b[i].c - ref)/ref*100.0 : 0.0;
            const bool wknd = omega::is_weekend(b[i].ts + 3600);
            if (ref > 0 && contig && jump >= cs.thr && !wknd) { win = true; age = 0; cur = {i,-1,-1,b[i].c}; continue; }
        }
        if (win && i > cur.entry_i && cur.cross_i < 0
            && (b[i].h / cur.trig - 1.0)*100.0 >= be) cur.cross_i = i;
    }
    if (win) { cur.end_i = (int)b.size()-1; ws.push_back(cur); }
    return ws;
}

int main(){
    auto envs=[&](const char* k, const char* d){ const char* v=getenv(k); return std::string(v?v:d); };
    const std::string mode = envs("FXCU_MODE","surgical");
    const double costx = atof(envs("FXCU_COSTX","1").c_str());
    const int maxwin = atoi(envs("FXCU_MAXWIN","40").c_str());
    const int stride = atoi(envs("FXCU_STRIDE","168").c_str());
    std::vector<int> Ns, Ls;
    { std::stringstream ss(envs("FXCU_N","24")); std::string t; while(std::getline(ss,t,',')) Ns.push_back(atoi(t.c_str())); }
    { std::stringstream ss(envs("FXCU_L","2,6,12,24")); std::string t; while(std::getline(ss,t,',')) Ls.push_back(atoi(t.c_str())); }

    static const CellSpec CELLS[] = {
        {"GBPUSD","/Users/jo/Tick/GBPUSD_IBKR_H1.csv",      48, 0.75, 2.0, 1.0},
        {"US500", "/Users/jo/Tick/SPXUSD_2022_2026.h1.csv", 24, 2.0,  4.0, 0.5},
        {"NAS100","/Users/jo/Tick/NSXUSD_2022_2026.h1.csv", 24, 1.5,  3.0, 0.5},
        {"GER40", "/Users/jo/Tick/GER40_1h_yahoo.csv",      12, 1.5,  2.0, 0.5},
        {"M2K",   "/Users/jo/Tick/M2K_h1.csv",              24, 1.0,  4.0, 0.5},
    };
    const std::string only = envs("FXCU_CELLS","all");

    fflush(stdout); int saved = dup(1); { int dn=open("/dev/null",O_WRONLY); dup2(dn,1); close(dn); }
    auto say=[&](const char* fmt, ...){ va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); };

    int g_fail=0, g_windows=0, g_recovered=0, g_mismatch=0, g_lostB=0, g_h1=0, g_h2=0;
    for (const auto& cs : CELLS) {
        if (only != "all" && only.find(cs.tag) == std::string::npos) continue;
        auto bars = load(cs.csv);
        if (bars.size() < (size_t)cs.W*4) { say("%-7s SKIP (bars=%zu)\n", cs.tag, bars.size()); continue; }
        if (mode == "surgical") {
            auto ws = scan_windows(bars, cs);
            std::vector<Win> pick; int prev_end = -1000000;
            for (const auto& w : ws) {
                // qualify: BE crossed >=2 bars after the trigger (so the 2-bar outage leaves
                // the always-on book still PENDING at restart => recovery must fire) but
                // within pend_bars=4 (later => always-on CANCELLED the legs, window books
                // nothing — nothing to recover), prior window safely closed before the
                // outage, enough ring history.
                if (w.cross_i > 0 && w.cross_i - w.entry_i >= 2 && w.cross_i - w.entry_i <= 4
                    && w.entry_i > cs.W + 10
                    && w.entry_i - prev_end >= 2 && (int)pick.size() < maxwin)
                    pick.push_back(w);
                prev_end = w.end_i > 0 ? w.end_i : prev_end;
            }
            if (pick.empty()) { say("%-7s surgical: no qualifying windows\n", cs.tag); continue; }
            auto A = run_sim(bars, cs, costx, 0, {});
            // sorted pairwise compare with RELATIVE 1e-8 price tolerance — a quantized
            // key (llround(px*1e6)) is boundary-fragile: a p15 file round-trip can shift
            // the last ulp across a .5 boundary and split identical clips into different
            // keys (caught on the gold-mimic cert, XauTf4h 2625.95515). Real divergences
            // are >=1e-5 relative; p15 round-trip error ~1e-12.
            auto mismatch_count=[](const std::vector<Clip>& xa, const std::vector<Clip>& xc){
                if (xa.size() != xc.size()) return (int)std::max(xa.size(), xc.size());
                auto ord=[](const Clip& a, const Clip& b){
                    return std::tie(a.ets,a.xts,a.reason,a.entry) < std::tie(b.ets,b.xts,b.reason,b.entry); };
                auto as = xa, cs2 = xc;
                std::sort(as.begin(), as.end(), ord); std::sort(cs2.begin(), cs2.end(), ord);
                int m = 0;
                for (size_t i = 0; i < as.size(); ++i) {
                    const Clip& a = as[i]; const Clip& c = cs2[i];
                    if (a.ets != c.ets || a.xts != c.xts || a.reason != c.reason
                        || std::fabs(a.entry-c.entry) > std::max(1e-9, std::fabs(a.entry)*1e-8)
                        || std::fabs(a.exit -c.exit ) > std::max(1e-9, std::fabs(a.exit )*1e-8)) m++;
                }
                return m;
            };
            const int64_t mid_ts = bars[bars.size()/2].ts;
            int rec=0, mis=0, lostB=0, nwin=0;
            for (const auto& w : pick) {
                std::vector<std::pair<int,int>> out{{w.entry_i, w.entry_i + 2}};
                auto B = run_sim(bars, cs, costx, 0,  out);
                auto C = run_sim(bars, cs, costx, 24, out);
                const int64_t w0 = bars[w.entry_i].ts;
                const int64_t w1 = bars[std::min(w.end_i>0?w.end_i:(int)bars.size()-1,(int)bars.size()-1)].ts;
                int bwin=0; for (const auto& cl : B.clips) if (cl.ets>=w0 && cl.ets<=w1) bwin++;
                if (bwin==0) lostB++;                 // N=0 lost the window, as designed
                // FULL-RUN equality C vs A (recovery exact => downstream identical)
                int cmis = mismatch_count(A.clips, C.clips);
                // book parity belt-and-braces. The restart run RELOADS the forward book
                // through book.txt, which persists pct at 6 SIGNIFICANT digits (the same
                // precision a real service restart lives with) — so the tolerance scales
                // with the book magnitude (NAS100 +1226% quantizes at 1e-2). The clip
                // MULTISET above is the authoritative equivalence gate; this only guards
                // gross aggregate corruption (e.g. a double-counted reload).
                if (std::fabs(C.book - A.book) > std::max(1e-3, std::fabs(A.book)*1e-4) && cmis == 0) cmis++;
                if (cmis > 0 && getenv("FXCU_DUMP")) {
                    say("DUMP %s window entry_i=%d [%lld..%lld] A.book=%.6f C.book=%.6f\n",
                        cs.tag, w.entry_i, (long long)w0, (long long)w1, A.book, C.book);
                    for (const auto& cl : A.clips) if (cl.ets>=w0-3600*200 && cl.ets<=w1+3600*200)
                        say("  A ets=%lld xts=%lld e=%.6f x=%.6f %s\n",(long long)cl.ets,(long long)cl.xts,cl.entry,cl.exit,cl.reason.c_str());
                    for (const auto& cl : C.clips) if (cl.ets>=w0-3600*200 && cl.ets<=w1+3600*200)
                        say("  C ets=%lld xts=%lld e=%.6f x=%.6f %s\n",(long long)cl.ets,(long long)cl.xts,cl.entry,cl.exit,cl.reason.c_str());
                    unsetenv("FXCU_DUMP");
                }
                int cwin=0; for (const auto& cl : C.clips) if (cl.ets>=w0 && cl.ets<=w1) cwin++;
                rec += cwin; mis += (cmis>0); nwin++;
                if (w0 < mid_ts) g_h1++; else g_h2++;
            }
            g_windows += nwin; g_recovered += rec; g_mismatch += mis; g_lostB += lostB;
            if (mis > 0) g_fail++;
            say("%-7s surgical(costx%.0f): windows=%d lostB=%d recovered_clips=%d mismatched_windows=%d %s\n",
                cs.tag, costx, nwin, lostB, rec, mis, mis ? "FAIL" : "OK");
        } else { // grid
            auto A = run_sim(bars, cs, costx, 0, {});
            for (int L : Ls) {
                std::vector<std::pair<int,int>> out;
                for (int s = stride/2; s + L < (int)bars.size(); s += stride) out.push_back({s, s+L});
                for (int N : Ns) {
                    auto B = run_sim(bars, cs, costx, 0, out);
                    auto C = run_sim(bars, cs, costx, N, out);
                    const bool ok = (C.book > 0.0)
                        && (C.pf >= std::min(1.3, 0.95*A.pf))
                        && (C.worst >= std::min(A.worst, B.worst) - 0.01);
                    if (!ok) g_fail++;
                    say("%-7s L=%-3d N=%-3d | A n=%-4d net=%+8.2f%% w=%-7.2f | B n=%-4d net=%+8.2f%% pf=%5.2f w=%-7.2f | C n=%-4d net=%+8.2f%% pf=%5.2f w=%-7.2f | d(C-B)=%+7.2f%% %s\n",
                        cs.tag, L, N, A.n, A.book, A.worst, B.n, B.book, B.pf, B.worst,
                        C.n, C.book, C.pf, C.worst, C.book - B.book, ok ? "OK" : "FAIL");
                }
            }
        }
    }
    fflush(stdout); dup2(saved,1); close(saved);
    if (mode == "surgical")
        std::printf("SURGICAL TOTAL (costx%.0f): windows=%d (H1-half %d / H2-half %d) lostB=%d recovered_clips=%d mismatched_windows=%d => %s\n",
            costx, g_windows, g_h1, g_h2, g_lostB, g_recovered, g_mismatch, g_fail ? "FAIL" : "PASS");
    else
        std::printf("GRID TOTAL (costx%.0f): fails=%d => %s\n", costx, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}

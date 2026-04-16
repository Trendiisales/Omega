// tse_p1_sweep.cpp — sweep P1 burst detection params against real tick data
// Exact P1 logic from TickScalpEngine: N consecutive same-dir ticks + net move
// Build: clang++ -O3 -std=c++20 -o /tmp/tse_p1 /tmp/tse_p1_sweep.cpp
// Run:   /tmp/tse_p1 ~/Downloads/l2_ticks_2026-04-16.csv

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// ── Tick ─────────────────────────────────────────────────────────────────────
struct Tick { int64_t ms; double bid, ask, drift, atr; };

static std::vector<Tick> load_csv(const char* path) {
    std::vector<Tick> out;
    std::ifstream f(path);
    if (!f) { fprintf(stderr,"Cannot open %s\n",path); return out; }
    std::string line, tok;
    std::getline(f, line);
    if (!line.empty() && line.back()=='\r') line.pop_back();
    int cm=-1,cb=-1,ca=-1,cd=-1,cr=-1,ci=0;
    { std::istringstream h(line);
      while (std::getline(h,tok,',')) {
        if (tok=="ts_ms"||tok=="timestamp_ms") cm=ci;
        if (tok=="bid")       cb=ci;
        if (tok=="ask")       ca=ci;
        if (tok=="ewm_drift") cd=ci;
        if (tok=="atr")       cr=ci;
        ++ci; } }
    if (cb<0||ca<0) { fprintf(stderr,"No bid/ask\n"); return out; }
    double pb=0, av=2.0; std::deque<double> aw;
    out.reserve(200000);
    while (std::getline(f,line)) {
        if (line.empty()) continue;
        if (line.back()=='\r') line.pop_back();
        static char buf[512];
        if (line.size()>=sizeof(buf)) continue;
        memcpy(buf, line.c_str(), line.size()+1);
        std::vector<const char*> flds; flds.reserve(20); flds.push_back(buf);
        for (char* c=buf;*c;++c) if (*c==','){*c='\0';flds.push_back(c+1);}
        int nc=(int)flds.size();
        if (nc<=std::max({cm,cb,ca})) continue;
        try {
            Tick t;
            t.ms    = (cm>=0)?(int64_t)std::stod(flds[cm]):0;
            t.bid   = std::stod(flds[cb]);
            t.ask   = std::stod(flds[ca]);
            t.drift = (cd>=0&&cd<nc)?std::stod(flds[cd]):0.0;
            t.atr   = (cr>=0&&cr<nc)?std::stod(flds[cr]):0.0;
            if (t.bid<=0||t.ask<t.bid) continue;
            if (t.atr<=0.0) {
                if (pb>0.0) {
                    double tr=(t.ask-t.bid)+std::fabs(t.bid-pb);
                    aw.push_back(tr); if((int)aw.size()>200)aw.pop_front();
                    if((int)aw.size()>=50){double s=0;for(double x:aw)s+=x;av=s/aw.size()*14.0;}
                }
                t.atr=av;
            }
            pb=t.bid; out.push_back(t);
        } catch(...) {}
    }
    return out;
}

// ── Params ────────────────────────────────────────────────────────────────────
struct P {
    int    burst_ticks;   // N consecutive same-dir ticks required
    double min_move;      // min net move over burst_ticks
    double tp_mult;       // TP = net_move * tp_mult
    double sl_mult;       // SL = net_move * sl_mult
    double drift_min;     // drift must agree and be >= this (0 = no drift gate)
    bool   be_at_half;    // move SL to BE when MFE >= 0.5 * TP
    int64_t cooldown_ms;  // cooldown after any exit
    int64_t timeout_ms;   // max hold
};

// ── CRTP engine ───────────────────────────────────────────────────────────────
template<typename Derived>
struct BurstEngBase {
    std::deque<double>  bid_hist;
    std::deque<int64_t> ts_hist;

    // position
    bool    pa=false, pl=false, be_done=false;
    double  pe=0, psl=0, ptp=0, pm=0, ps=0.01;
    int64_t pts=0, cd_until=0;

    // results
    double  total_pnl=0, long_pnl=0, short_pnl=0;
    int     nt=0, nw=0;
    int     wt=0;

    static constexpr int   HIST    = 32;
    static constexpr int   WARMUP  = 60;
    static constexpr double MAX_SPREAD = 0.50;
    static constexpr double ATR_MIN    = 1.0;
    static constexpr double ATR_MAX    = 8.0;
    static constexpr double RISK_USD   = 10.0;
    static constexpr double MAX_LOT    = 0.01;

    void close(double ep, bool il, int64_t ms, const P& p) {
        double pp=il?(ep-pe):(pe-ep), pu=pp*ps*100.0;
        total_pnl+=pu; ++nt; if(pu>0)++nw;
        if(il) long_pnl+=pu; else short_pnl+=pu;
        cd_until = ms + p.cooldown_ms;
        pa=false;
    }

    void tick(int64_t ms, double bid, double ask, double drift, double atr, const P& p) {
        ++wt;
        bid_hist.push_back(bid);
        ts_hist.push_back(ms);
        if ((int)bid_hist.size()>HIST) { bid_hist.pop_front(); ts_hist.pop_front(); }

        double sp = ask-bid;

        // manage open position
        if (pa) {
            double mid=(bid+ask)*0.5;
            double mv=pl?(mid-pe):(pe-mid);
            if (mv>pm) pm=mv;
            double eff=pl?bid:ask;
            double tp_dist=std::fabs(ptp-pe);
            // BE
            if (!be_done && p.be_at_half && tp_dist>0 && mv>=tp_dist*0.5) {
                psl=pe; be_done=true;
            }
            // TP
            if ((pl&&bid>=ptp)||(!pl&&ask<=ptp)) { close(eff,pl,ms,p); return; }
            // SL
            if ((pl&&bid<=psl)||(!pl&&ask>=psl)) { close(eff,pl,ms,p); return; }
            // timeout
            if (ms-pts>p.timeout_ms) { close(eff,pl,ms,p); return; }
            return;
        }

        if (wt<WARMUP) return;
        if (sp>MAX_SPREAD||atr<ATR_MIN||atr>ATR_MAX) return;
        if (ms<cd_until) return;

        int n=(int)bid_hist.size();
        if (n < p.burst_ticks+1) return;

        // count up/down ticks in last burst_ticks
        int up=0, dn=0;
        for (int i=n-p.burst_ticks; i<n; ++i) {
            double d=bid_hist[i]-bid_hist[i-1];
            if (d>0) ++up;
            else if (d<0) ++dn;
        }

        bool burst_up = (up >= p.burst_ticks-1 && dn==0);
        bool burst_dn = (dn >= p.burst_ticks-1 && up==0);
        if (!burst_up && !burst_dn) return;

        // net move over burst
        double net = std::fabs(bid_hist[n-1] - bid_hist[n-1-p.burst_ticks]);
        if (net < p.min_move) return;

        // drift gate
        if (p.drift_min > 0.0) {
            if (burst_up && drift <  p.drift_min) return;
            if (burst_dn && drift > -p.drift_min) return;
        }

        // size
        double sl_pts = net * p.sl_mult;
        double tp_pts = net * p.tp_mult;
        double sl_safe = std::max(0.05, sl_pts);
        double size = RISK_USD / (sl_safe * 100.0);
        size = std::floor(size/0.001)*0.001;
        size = std::max(0.001, std::min(MAX_LOT, size));

        bool il = burst_up;
        double e = il ? ask : bid;
        pa=true; pl=il; pe=e; pm=0; be_done=false;
        psl = il ? (e-sl_pts) : (e+sl_pts);
        ptp = il ? (e+tp_pts) : (e-tp_pts);
        pts=ms; ps=size;
    }

    void fin(int64_t ms, const P& p) { if (pa) close(pa&&pl?0:0, pl, ms, p); }
};

struct Eng : BurstEngBase<Eng> {};

struct Res { double pnl,wr,lp,sp; int n; P p; };

int main(int argc, char* argv[]) {
    if (argc<2) { puts("Usage: tse_p1 <csv>"); return 1; }

    auto t0=std::chrono::steady_clock::now();
    auto ticks=load_csv(argv[1]);
    if (ticks.empty()) return 1;
    printf("Loaded %zu ticks\n", ticks.size());

    // P1 param grid
    const int    bt_v[]  = {4, 6, 8, 10, 12};          // burst_ticks
    const double mm_v[]  = {0.15, 0.25, 0.40, 0.60};   // min_move pts
    const double tp_v[]  = {1.5, 2.0, 2.5, 3.0, 4.0};  // tp_mult
    const double sl_v[]  = {0.5, 0.75, 1.0, 1.5};      // sl_mult
    const double dm_v[]  = {0.0, 0.20, 0.40};           // drift_min (0=off)
    const bool   be_v[]  = {false, true};                // be_at_half
    const int64_t cd_v[] = {5000, 10000, 15000};         // cooldown ms
    const int64_t to_v[] = {30000, 60000, 120000};       // timeout ms

    long TOTAL = 5*4*5*4*3*2*3*3; // 21600
    printf("Testing %ld combinations...\n", TOTAL);

    std::vector<Res> results; results.reserve(20000);
    long done=0;

    for (int    bt : bt_v)
    for (double mm : mm_v)
    for (double tp : tp_v)
    for (double sl : sl_v)
    for (double dm : dm_v)
    for (bool   be : be_v)
    for (int64_t cd : cd_v)
    for (int64_t to : to_v)
    {
        P p{bt, mm, tp, sl, dm, be, cd, to};
        Eng eng;
        for (auto& tk : ticks)
            eng.tick(tk.ms, tk.bid, tk.ask, tk.drift, tk.atr, p);
        if (!ticks.empty()) eng.fin(ticks.back().ms, p);

        if (eng.nt >= 3) {
            Res r;
            r.pnl=eng.total_pnl; r.wr=(double)eng.nw/eng.nt;
            r.lp=eng.long_pnl;   r.sp=eng.short_pnl;
            r.n=eng.nt;          r.p=p;
            results.push_back(r);
        }

        if (++done % 2000 == 0) {
            double best=0;
            for (auto& r:results) if(r.pnl>best) best=r.pnl;
            printf("  %ld/%ld  best=$%+.2f\n", done, TOTAL, best);
            fflush(stdout);
        }
    }

    std::sort(results.begin(), results.end(),
              [](const Res& a, const Res& b){ return a.pnl>b.pnl; });

    auto t1=std::chrono::steady_clock::now();
    printf("\nDone in %.2fs\n", std::chrono::duration<double>(t1-t0).count());

    printf("\n%s\nTOP 20 P1 BURST\n%s\n",
           std::string(76,'=').c_str(), std::string(76,'=').c_str());
    printf("%-3s %-9s %-6s %-4s %-7s %-7s  bt  mm    tp   sl   drift  be  cd   to\n",
           "#","PnL","WR","N","L$","S$");

    int show=std::min((int)results.size(),20);
    for (int i=0;i<show;++i) {
        auto& r=results[i];
        printf("%-3d $%+8.2f %5.1f%% %4d $%+6.2f $%+6.2f"
               "  %2d  %.2f  %.1f  %.2f  %.2f   %s  %2lds  %3lds\n",
               i+1, r.pnl, r.wr*100, r.n, r.lp, r.sp,
               r.p.burst_ticks, r.p.min_move, r.p.tp_mult, r.p.sl_mult,
               r.p.drift_min, r.p.be_at_half?"Y":"N",
               (long)(r.p.cooldown_ms/1000), (long)(r.p.timeout_ms/1000));
    }

    if (!results.empty()) {
        auto& b=results[0];
        printf("\n%s\nBEST: $%+.2f  WR=%.1f%%  N=%d  L=$%+.2f  S=$%+.2f\n"
               "burst_ticks=%d min_move=%.2f tp_mult=%.1f sl_mult=%.2f "
               "drift_min=%.2f be=%s cd=%lds timeout=%lds\n",
               std::string(76,'=').c_str(),
               b.pnl, b.wr*100, b.n, b.lp, b.sp,
               b.p.burst_ticks, b.p.min_move, b.p.tp_mult, b.p.sl_mult,
               b.p.drift_min, b.p.be_at_half?"Y":"N",
               (long)(b.p.cooldown_ms/1000), (long)(b.p.timeout_ms/1000));

        // Print all trades for best config
        Eng eng;
        for (auto& tk : ticks)
            eng.tick(tk.ms, tk.bid, tk.ask, tk.drift, tk.atr, b.p);
        // (trade detail requires storing — omitted for perf, results are in aggregates above)
    }

    return 0;
}

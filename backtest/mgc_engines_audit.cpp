// ─────────────────────────────────────────────────────────────────────────────
// mgc_engines_audit.cpp — faithful audit of the two MGC futures engines, driving
// the REAL classes on real MGC 30m bars (data/mgc_30m_hist.csv). Both are LONG-ONLY
// 30m-bar breakout engines -> bar-replay IS faithful (close-driven exits, no intrabar
// stop ambiguity). MGC micro-gold cost applied per round-trip (spread+slip+commission).
//
//   g_mgc_fastdon  = omega::MgcFastDonchian30mEngine   (Donchian breakout + HVN skip)
//   g_mgc_volbrk   = omega::GoldVolBreakoutM30Engine   (vol-breakout; same class audited
//                                                       PF2.41 on gold spot, MGC instance new)
//
//   verdict grid: n, WR, PF, net(pts), maxDD(pts), both-halves PF.  argv: cost_pts (default 0.30)
//   NOTE: MGC futures history starts ~2024-06 -> NO 2022 bear in this data. Both-halves is the
//   robustness axis available; bear-regime is a DATA LIMITATION, flagged in the verdict.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include "MgcFastDonchian30mEngine.hpp"
#include "GoldVolBreakoutM30Engine.hpp"

struct Bar { int64_t ts; double o,h,l,c,v; };

static std::vector<Bar> load(const char* path) {
    std::vector<Bar> out; std::ifstream f(path);
    if (!f.is_open()) { std::fprintf(stderr, "cannot open %s\n", path); return out; }
    std::string ln; std::getline(f, ln);  // header
    while (std::getline(f, ln)) {
        Bar b; char* e=nullptr; const char* s=ln.c_str();
        b.ts=std::strtoll(s,&e,10); if(*e!=',')continue; ++e;
        b.o=std::strtod(e,&e); if(*e!=',')continue; ++e;
        b.h=std::strtod(e,&e); if(*e!=',')continue; ++e;
        b.l=std::strtod(e,&e); if(*e!=',')continue; ++e;
        b.c=std::strtod(e,&e); b.v=(*e==',')?std::strtod(e+1,&e):0.0;
        if(b.h>=b.l && b.o>0) out.push_back(b);
    }
    return out;
}

struct Trade { double entry, exit; };

static void report(const char* eng, std::vector<Trade>& tr, double cost_pts) {
    int n=(int)tr.size(); double gp=0,gl=0,net=0; int wins=0;
    double eq=0,peak=0,maxdd=0;
    auto pnl=[&](const Trade& t){ return (t.exit - t.entry) - cost_pts; };  // LONG, pts, round-trip cost
    for (auto& t: tr){ double p=pnl(t); net+=p; if(p>0){gp+=p;++wins;}else gl+=-p;
        eq+=p; if(eq>peak)peak=eq; if(peak-eq>maxdd)maxdd=peak-eq; }
    double pf=gl>1e-9?gp/gl:(gp>0?999:0), wr=n?100.0*wins/n:0;
    auto hpf=[&](int a,int b){double g=0,l=0;for(int i=a;i<b;++i){double p=pnl(tr[i]);if(p>0)g+=p;else l+=-p;}return l>1e-9?g/l:(g>0?999:0);};
    double pf1=n>=2?hpf(0,n/2):0, pf2=n>=2?hpf(n/2,n):0;
    std::printf("%-14s cost=%.2fpt | n=%-4d WR=%4.1f%% PF=%5.2f net=%8.2fpt maxDD=%7.2fpt | h1=%5.2f h2=%5.2f %s\n",
                eng, cost_pts, n, wr, pf, net, maxdd, pf1, pf2,
                (pf>=1.3 && pf1>=1.0 && pf2>=1.0)?"BOTH-HALVES+":"");
}

int main(int argc, char** argv) {
    const double cost = argc>1 ? std::atof(argv[1]) : 0.30;   // MGC round-trip pts (~$3 on 1 micro)
    const char* path = argc>2 ? argv[2] : "data/mgc_30m_hist.csv";
    auto bars = load(path);
    std::printf("# MGC audit: %zu 30m bars from %s\n", bars.size(), path);
    if (bars.size() < 100) { std::fprintf(stderr,"too few bars\n"); return 1; }

    // ---- g_mgc_fastdon : MgcFastDonchian30mEngine ----
    {
        omega::MgcFastDonchian30mEngine eng; eng.enabled=true; eng.lot=0.01;
        std::vector<Trade> trades; double cur_entry=0; bool open=false;
        auto cb=[&](const omega::TradeRecord& t){ trades.push_back({t.entryPrice, t.exitPrice}); open=false; };
        for (auto& b: bars) eng.on_30m_bar(b.o,b.h,b.l,b.c,b.v,b.ts,cb);
        (void)cur_entry;(void)open;
        report("MgcFastDon", trades, cost);
        report("MgcFastDon", trades, cost*2.0);   // 2x cost stress
    }
    // ---- g_mgc_volbrk : GoldVolBreakoutM30Engine (MGC instance) ----
    // needs h1 trend (on_h1_close sets trend_; m30 entry gated by trend_==1). Feed h1+m30
    // merged chronologically (the live wiring: seed_h1 primes trend, m30 bars drive entries).
    {
        // aggregate H1 closes from the 30m bars (hour bucket = last close in the hour)
        std::vector<std::pair<int64_t,double>> h1;  // (hour_bucket_ts, close)
        { int64_t cur=-1; double cc=0;
          for (auto& b: bars){ int64_t hb=(b.ts/3600)*3600; if(hb!=cur){ if(cur>=0)h1.push_back({cur,cc}); cur=hb; } cc=b.c; }
          if(cur>=0)h1.push_back({cur,cc}); }
        omega::GoldVolBreakoutM30Engine eng; eng.enabled=true;
        std::vector<Trade> trades;
        auto cb=[&](const omega::TradeRecord& t){ trades.push_back({t.entryPrice, t.exitPrice}); };
        const double half=0.15;
        size_t ih=0;
        for (auto& b: bars) {
            while (ih < h1.size() && h1[ih].first <= b.ts) { eng.on_h1_close(h1[ih].second); ++ih; }  // h1 trend up to now
            eng.on_m30_bar(b.h,b.l,b.c, b.c-half, b.c+half, b.ts*1000, cb);
        }
        std::printf("# volbrk h1 closes fed: %zu\n", ih);
        report("MgcVolBrk", trades, cost);
        report("MgcVolBrk", trades, cost*2.0);
    }
    return 0;
}

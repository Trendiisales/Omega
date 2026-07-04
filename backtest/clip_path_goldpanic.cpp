// clip_path_goldpanic.cpp -- emit per-BAR PATH csv for GoldPanicBounceEngine (wide-chandelier
// V-bounce dip-buyer) by driving the REAL engine class, then reconstructing each trade's H1 bar
// path (entry->natural exit). Output feeds dollar_trail_companion.py (STANDALONE $-gauge companion).
// S-2026-07-04: built for the gold-companion bull-gate re-check (operator: don't dismiss bull-good/
// bear-bad -> bull-gate it). Engine is tick-driven with internal H1 aggregation + on_close_cb member.
//
// PATH row: trade_id,seq,exit_ms,dir,entry_px,px,atr_pct,bull,cost_rt   (same schema as clip_path_xau_tf)
// argv[1] = H1 csv (ts,o,h,l,c); default = 2yr bull. Pass the 2022-23 bear h1 for the bear leg.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include "GoldPanicBounceEngine.hpp"

struct Bar { int64_t ts_sec; double o,h,l,c; };
static std::vector<Bar> load_csv(const std::string& path){
    std::vector<Bar> bars; std::ifstream f(path); if(!f) return bars;
    std::string line; std::getline(f,line);
    while(std::getline(f,line)){ Bar b; const char* p=line.c_str(); char* e;
        b.ts_sec=std::strtoll(p,&e,10); if(*e!=',')continue; p=e+1;
        b.o=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
        b.h=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
        b.l=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
        b.c=std::strtod(p,&e); bars.push_back(b);
    } return bars;
}
struct Trade{ int64_t entryTs,exitTs; int dir; double entry_px; };

static std::vector<double> atr_pct(const std::vector<Bar>&B){
    int N=B.size(); std::vector<double> ap(N,0.0); double a=0;
    for(int i=1;i<N;i++){ double tr=std::max({B[i].h-B[i].l,std::fabs(B[i].h-B[i-1].c),std::fabs(B[i].l-B[i-1].c)});
        a = i<15 ? (a*(i-1)+tr)/i : (a*13+tr)/14;
        ap[i]= B[i].c>0 ? a/B[i].c : 0.0; }
    return ap;
}
static std::vector<double> sma200(const std::vector<Bar>&B){
    int N=B.size(); std::vector<double> s(N,0.0); double sum=0;
    for(int i=0;i<N;i++){ sum+=B[i].c; if(i>=200) sum-=B[i-200].c;
        s[i]= i>=199 ? sum/200.0 : B[i].c; }
    return s;
}
static int idx_at(const std::vector<Bar>&B,int64_t ts){
    int lo=0,hi=B.size()-1,r=0;
    while(lo<=hi){int m=(lo+hi)/2; if(B[m].ts_sec<=ts){r=m;lo=m+1;}else hi=m-1;} return r;
}

int main(int argc,char**argv){
    const double half=0.15; // XAU H1 measured median spread ~0.30 -> half 0.15
    const char* h1path = argc>1?argv[1]:"/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv";
    auto h1=load_csv(h1path);
    fprintf(stderr,"loaded H1=%zu\n",h1.size());
    if(h1.empty()){ fprintf(stderr,"no h1\n"); return 1; }

    // Warm the shared price-based regime brain so gold_regime().long_blocked() reflects real
    // historical regime instead of blocking every long (engine_init:1536 pattern). Best-effort.
    omega::gold_regime().seed_from_h1_csv(h1path);

    std::vector<Trade> trades;
    omega::GoldPanicBounceEngine eng;
    eng.shadow_mode=true; eng.enabled=true;
    eng.TREND_GATE=true; eng.TREND_SLOPE_LB=200; eng.TREND_SLOPE_MIN=0.0; // live cfg (engine_init:4280-82)
    if(argc>2) eng.DROP_K=std::atof(argv[2]); // diagnostic: lower to prove drive fires on shallower dips
    eng.on_close_cb=[&trades](const omega::TradeRecord& tr){
        Trade t; t.entryTs=tr.entryTs; t.exitTs=tr.exitTs;
        t.dir=(tr.side=="LONG")?+1:-1; t.entry_px=tr.entryPrice;
        trades.push_back(t);
    };

    // Drive tick-by-tick: feed o,h,l,c ticks per H1 bar at that bar's ts (same bar index ->
    // accumulate one H1 bar); the NEXT bar's first tick advances the bar index -> engine closes
    // the bar + decides (matches the live on_tick path; H1-granular stop management).
    for(size_t i=0;i<h1.size();++i){ const auto&b=h1[i]; int64_t ms=b.ts_sec*1000LL;
        eng.on_tick(b.o-half,b.o+half,ms);
        eng.on_tick(b.h-half,b.h+half,ms);
        eng.on_tick(b.l-half,b.l+half,ms);
        eng.on_tick(b.c-half,b.c+half,ms);
    }
    fprintf(stderr,"goldpanic: %zu trades\n",trades.size());

    auto ap=atr_pct(h1); auto sm=sma200(h1);
    printf("trade_id,seq,exit_ms,dir,entry_px,px,atr_pct,bull,cost_rt\n");
    int tid=0;
    for(const auto& t:trades){
        int ei=idx_at(h1,t.entryTs); int xi=idx_at(h1,t.exitTs);
        if(xi<ei) xi=ei;
        double atrp=ap[ei];
        int bull=(h1[ei].c>sm[ei])?1:0;
        double cost_rt=2.0*0.00015 + (2.0*half)/t.entry_px;
        int seq=0;
        for(int i=ei;i<=xi && i<(int)h1.size(); ++i){
            printf("%d,%d,%lld,%d,%.5f,%.5f,%.6f,%d,%.6f\n",
                tid,seq,(long long)h1[i].ts_sec*1000LL,t.dir,t.entry_px,h1[i].c,atrp,bull,cost_rt);
            seq++;
        }
        tid++;
    }
    return 0;
}

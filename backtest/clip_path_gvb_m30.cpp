// clip_path_gvb_m30.cpp -- emit per-BAR PATH csv for GoldVolBreakoutM30 by
// driving the REAL engine class over merged H1-close + M30-bar streams, then
// reconstructing each trade's M30 bar path (entry->natural exit). Feeds
// standalone_clip_overlay.py. Companion clip judged STANDALONE (never vs WIDE).
//
// PATH row: trade_id,seq,exit_ms,dir,entry_px,px,atr_pct,bull,cost_rt
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include "GoldVolBreakoutM30Engine.hpp"

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
    (void)argc;(void)argv;
    const double half=0.15; // XAU M30 measured median spread ~0.30 -> half 0.15
    auto m30=load_csv("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.m30.csv");
    auto h1 =load_csv("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv");
    fprintf(stderr,"loaded M30=%zu H1=%zu\n",m30.size(),h1.size());
    if(m30.empty()){ fprintf(stderr,"no m30\n"); return 1; }

    std::vector<Trade> trades;
    auto cb=[&trades](const omega::TradeRecord& tr){
        Trade t; t.entryTs=tr.entryTs; t.exitTs=tr.exitTs;
        t.dir=(tr.side=="LONG")?+1:-1; t.entry_px=tr.entryPrice;
        trades.push_back(t);
    };

    omega::GoldVolBreakoutM30Engine eng;
    eng.shadow_mode=true; eng.enabled=true; eng.lot=0.01; eng.max_spread=1.0;
    eng.init();

    // Merge H1 closes + M30 bars by timestamp, drive in event order (as tick_gold.hpp does).
    size_t ih=0, im=0;
    while(im<m30.size()){
        int64_t mts=m30[im].ts_sec;
        // feed all H1 closes at or before this M30 bar's start
        while(ih<h1.size() && h1[ih].ts_sec<=mts){ eng.on_h1_close(h1[ih].c); ++ih; }
        const auto&b=m30[im]; int64_t ms=b.ts_sec*1000LL;
        eng.on_m30_bar(b.h,b.l,b.c,b.c-half,b.c+half,ms,cb);
        // intrabar SL ticks from NEXT bar's low then high (matches clip_path_xau_tf)
        if(im+1<m30.size()){ const auto&nb=m30[im+1]; int64_t nt=nb.ts_sec*1000LL;
            eng.on_tick(nb.l-half,nb.l+half,nt,cb); eng.on_tick(nb.h-half,nb.h+half,nt,cb); }
        ++im;
    }
    fprintf(stderr,"gvb_m30: %zu trades\n",trades.size());

    auto ap=atr_pct(m30); auto sm=sma200(m30);
    printf("trade_id,seq,exit_ms,dir,entry_px,px,atr_pct,bull,cost_rt\n");
    int tid=0;
    for(const auto& t:trades){
        int ei=idx_at(m30,t.entryTs); int xi=idx_at(m30,t.exitTs);
        if(xi<ei) xi=ei;
        double atrp=ap[ei];
        int bull=(m30[ei].c>sm[ei])?1:0;
        double cost_rt=2.0*0.00015 + (2.0*half)/t.entry_px;
        int seq=0;
        for(int i=ei;i<=xi && i<(int)m30.size(); ++i){
            printf("%d,%d,%lld,%d,%.5f,%.5f,%.6f,%d,%.6f\n",
                tid,seq,(long long)m30[i].ts_sec*1000LL,t.dir,t.entry_px,m30[i].c,atrp,bull,cost_rt);
            seq++;
        }
        tid++;
    }
    return 0;
}

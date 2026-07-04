// clip_path_mgc_fastdon.cpp -- per-BAR PATH csv for MgcFastDonchian30m (Donchian-20
// breakout, Donchian-10 CHANNEL exit -- structural flip-style, not a tight trail).
// Driven on XAU M30 (MGC~=gold) to test whether the channel-exit gap beats M30
// turnover for a companion clip. HVN skip OFF (no volume-node data) -> core shape.
// Companion clip judged STANDALONE (never vs WIDE). Feeds standalone_clip_overlay.py.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include "MgcFastDonchian30mEngine.hpp"

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
    const double half=0.15;
    const char* m30path = argc>1?argv[1]:"/Users/jo/Tick/2yr_XAUUSD_tick_fresh.m30.csv";
    auto m30=load_csv(m30path);
    fprintf(stderr,"loaded M30=%zu\n",m30.size());
    if(m30.empty()){ fprintf(stderr,"no m30\n"); return 1; }

    std::vector<Trade> trades;
    auto cb=[&trades](const omega::TradeRecord& tr){
        Trade t; t.entryTs=tr.entryTs; t.exitTs=tr.exitTs;
        t.dir=(tr.side=="LONG")?+1:-1; t.entry_px=tr.entryPrice;
        trades.push_back(t);
    };

    omega::MgcFastDonchian30mEngine eng;
    eng.shadow_mode=true; eng.enabled=true; eng.lot=0.01;
    eng.use_hvn_skip=false;         // no volume-node data on XAU M30 -> core Donchian shape
    eng.use_trend_filter=true;      // keep the EMA100 bear gate (deployed config)

    for(size_t i=0;i<m30.size();++i){ const auto&b=m30[i];
        eng.on_30m_bar(b.o,b.h,b.l,b.c,0.0,b.ts_sec,cb);
    }
    fprintf(stderr,"mgc_fastdon(XAU M30): %zu trades\n",trades.size());

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

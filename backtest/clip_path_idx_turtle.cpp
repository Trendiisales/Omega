// clip_path_idx_turtle.cpp -- per-BAR PATH csv for the index NasTurtleD1 chassis
// (DJ30 + SPX, both enabled=true in engine_init.hpp). Drives the REAL
// omega::NasTurtleD1Engine, captures each trade's entry/exit/dir/entry_px, then
// reconstructs the daily-close path from the same daily CSV. Output ->
// standalone_clip_overlay.py.  usage: clip_path_idx_turtle <daily.csv> <LABEL> [half_spread]
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include "NasTurtleD1Engine.hpp"

struct DBar{ int64_t ts_sec; double o,h,l,c; };
static std::vector<DBar> load_daily(const std::string& path){
    std::vector<DBar> bars; std::ifstream f(path); if(!f) return bars;
    std::string line; bool first=true;
    while(std::getline(f,line)){
        if(line.empty()) continue;
        // auto-detect header
        if(first){ first=false; if(!(line[0]>='0'&&line[0]<='9')) continue; }
        DBar b; const char* p=line.c_str(); char* e;
        b.ts_sec=std::strtoll(p,&e,10); if(*e!=',')continue; p=e+1;
        b.o=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
        b.h=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
        b.l=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
        b.c=std::strtod(p,&e); bars.push_back(b);
    } return bars;
}
struct Trade{ int64_t entryTs,exitTs; int dir; double entry_px; };

static std::vector<double> atr_pct(const std::vector<DBar>&B){
    int N=B.size(); std::vector<double> ap(N,0.0); double a=0;
    for(int i=1;i<N;i++){ double tr=std::max({B[i].h-B[i].l,std::fabs(B[i].h-B[i-1].c),std::fabs(B[i].l-B[i-1].c)});
        a=i<15?(a*(i-1)+tr)/i:(a*13+tr)/14; ap[i]= B[i].c>0? a/B[i].c:0.0; }
    return ap;
}
static std::vector<double> sma200(const std::vector<DBar>&B){
    int N=B.size(); std::vector<double> s(N,0.0); double sum=0;
    for(int i=0;i<N;i++){ sum+=B[i].c; if(i>=200)sum-=B[i-200].c; s[i]= i>=199?sum/200.0:B[i].c; }
    return s;
}
static int idx_at(const std::vector<DBar>&B,int64_t ts){
    int lo=0,hi=B.size()-1,r=0;
    while(lo<=hi){int m=(lo+hi)/2; if(B[m].ts_sec<=ts){r=m;lo=m+1;}else hi=m-1;} return r;
}

int main(int argc,char**argv){
    if(argc<3){ fprintf(stderr,"usage: %s <daily.csv> <LABEL> [half_spread]\n",argv[0]); return 2; }
    std::string path=argv[1], label=argv[2];
    double hs = argc>3?std::atof(argv[3]):0.5;
    auto bars=load_daily(path);
    fprintf(stderr,"[%s] loaded %zu daily bars\n",label.c_str(),bars.size());
    if(bars.size()<50){ fprintf(stderr,"too few bars\n"); return 1; }

    omega::NasTurtleD1Engine eng; eng.shadow_mode=true; eng.enabled=true;
    eng.symbol=label; eng.p=omega::make_nas_turtle_d1_params(); eng.p.dollars_per_pt=1.0;

    std::vector<Trade> trades;
    auto cb=[&](const omega::TradeRecord& tr){
        Trade t; t.entryTs=tr.entryTs; t.exitTs=tr.exitTs;
        t.dir=(tr.side=="LONG")?+1:-1; t.entry_px=tr.entryPrice; trades.push_back(t);
    };
    for(size_t i=0;i<bars.size();++i){ const auto&b=bars[i]; int64_t ms=b.ts_sec*1000LL;
        eng.on_tick(b.o-hs,b.o+hs,ms,cb); eng.on_tick(b.l-hs,b.l+hs,ms,cb);
        eng.on_tick(b.h-hs,b.h+hs,ms,cb); eng.on_tick(b.c-hs,b.c+hs,ms,cb); }
    fprintf(stderr,"[%s] %zu trades\n",label.c_str(),trades.size());

    // write to file (arg 4) so engine's stdout printf logs don't pollute the CSV
    const char* outp = argc>4?argv[4]:"/dev/stdout";
    FILE* out=fopen(outp,"w"); if(!out){fprintf(stderr,"cannot open %s\n",outp);return 1;}
    auto ap=atr_pct(bars); auto sm=sma200(bars);
    fprintf(out,"trade_id,seq,exit_ms,dir,entry_px,px,atr_pct,bull,cost_rt\n");
    int tid=0;
    for(const auto& t:trades){
        int ei=idx_at(bars,t.entryTs); int xi=idx_at(bars,t.exitTs); if(xi<ei)xi=ei;
        double atrp=ap[ei]; int bull=(bars[ei].c>sm[ei])?1:0;
        // index cost: full spread + ~0.5bp commission each side (IBKR CFD)
        double cost_rt = (2.0*hs)/t.entry_px + 2.0*0.00005;
        int seq=0;
        for(int i=ei;i<=xi && i<(int)bars.size();++i){
            fprintf(out,"%d,%d,%lld,%d,%.5f,%.5f,%.6f,%d,%.6f\n",
                tid,seq,(long long)bars[i].ts_sec*1000LL,t.dir,t.entry_px,bars[i].c,atrp,bull,cost_rt);
            seq++;
        }
        tid++;
    }
    fclose(out);
    return 0;
}

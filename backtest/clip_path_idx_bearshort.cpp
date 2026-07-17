// clip_path_idx_bearshort.cpp -- per-BAR PATH csv for IndexBearShortEngine (SHORT-only
// bear-breakdown; NAS100 DON24, US500 DON48 -- both enabled=true, shadow, engine_init).
// Drives the REAL omega::IndexBearShortEngine over an H1 (ts,o,h,l,c) index file by
// feeding each H1 bar as 4 intrabar ticks (o,l,h,c) so the engine's own H1 aggregator +
// SL/TP/manage path fire exactly as live; captures entry/exit/dir/entry_px; reconstructs
// the H1-close path entry->natural exit. Output -> mimic_ladder_overlay (STANDALONE).
//   usage: clip_path_idx_bearshort <h1.csv> <SYMBOL NAS100|US500> <DON> <out.csv> [half_spread]
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include "IndexBearShortEngine.hpp"

struct Bar{ int64_t ts_sec; double o,h,l,c; };
static std::vector<Bar> load_h1(const std::string& p){
    std::vector<Bar> b; std::ifstream f(p); if(!f) return b;
    std::string line;
    while(std::getline(f,line)){
        if(line.empty()||!(line[0]>='0'&&line[0]<='9')) continue;
        Bar x; const char* s=line.c_str(); char* e;
        x.ts_sec=std::strtoll(s,&e,10); if(*e!=',')continue; s=e+1;
        x.o=std::strtod(s,&e); if(*e!=',')continue; s=e+1;
        x.h=std::strtod(s,&e); if(*e!=',')continue; s=e+1;
        x.l=std::strtod(s,&e); if(*e!=',')continue; s=e+1;
        x.c=std::strtod(s,&e); b.push_back(x);
    } return b;
}
struct Trade{ int64_t entryTs,exitTs; int dir; double entry_px; };

static std::vector<double> atr_pct(const std::vector<Bar>&B){
    int N=B.size(); std::vector<double> ap(N,0.0); double a=0;
    for(int i=1;i<N;i++){ double tr=std::max({B[i].h-B[i].l,std::fabs(B[i].h-B[i-1].c),std::fabs(B[i].l-B[i-1].c)});
        a=i<15?(a*(i-1)+tr)/i:(a*13+tr)/14; ap[i]=B[i].c>0?a/B[i].c:0.0; } return ap;
}
static std::vector<double> sma200(const std::vector<Bar>&B){
    int N=B.size(); std::vector<double> s(N,0.0); double sum=0;
    for(int i=0;i<N;i++){ sum+=B[i].c; if(i>=200)sum-=B[i-200].c; s[i]=i>=199?sum/200.0:B[i].c; } return s;
}
static int idx_at(const std::vector<Bar>&B,int64_t ts){
    int lo=0,hi=B.size()-1,r=0;
    while(lo<=hi){int m=(lo+hi)/2; if(B[m].ts_sec<=ts){r=m;lo=m+1;}else hi=m-1;} return r;
}

int main(int argc,char**argv){
    if(argc<5){ fprintf(stderr,"usage: %s <h1.csv> <SYMBOL> <DON> <out.csv> [half]\n",argv[0]); return 2; }
    std::string path=argv[1], sym=argv[2]; int DON=std::atoi(argv[3]);
    std::string outp=argv[4]; double hs=argc>5?std::atof(argv[5]):(sym=="NAS100"?1.0:0.3);
    auto B=load_h1(path);
    fprintf(stderr,"[%s] loaded %zu H1 bars\n",sym.c_str(),B.size());
    if(B.size()<400){ fprintf(stderr,"too few bars\n"); return 1; }

    omega::IndexBearShortEngine eng; eng.shadow_mode=true; eng.enabled=true;
    eng.symbol=sym; eng.DON=DON; eng.USE_RISKOFF_GATE=false;
    eng.COST_PTS = (sym=="NAS100")?2.0:0.6; eng.lot=1.0;
    std::vector<Trade> trades;
    eng.on_close_cb=[&](const omega::TradeRecord& tr){
        Trade t; t.entryTs=tr.entryTs; t.exitTs=tr.exitTs; // seconds
        t.dir=(tr.side=="LONG")?+1:-1; t.entry_px=tr.entryPrice; trades.push_back(t);
    };
    // feed each H1 bar as 4 intrabar ticks (o,l,h,c) inside the same 3600s bucket
    for(size_t i=0;i<B.size();++i){ const auto&b=B[i]; int64_t base=b.ts_sec*1000LL;
        eng.on_tick(b.o-hs,b.o+hs,base+0);
        eng.on_tick(b.l-hs,b.l+hs,base+900000);
        eng.on_tick(b.h-hs,b.h+hs,base+1800000);
        eng.on_tick(b.c-hs,b.c+hs,base+2700000);
    }
    fprintf(stderr,"[%s] %zu trades\n",sym.c_str(),trades.size());

    FILE* out=fopen(outp.c_str(),"w"); if(!out){fprintf(stderr,"cannot open %s\n",outp.c_str());return 1;}
    auto ap=atr_pct(B); auto sm=sma200(B);
    fprintf(out,"trade_id,seq,exit_ms,dir,entry_px,px,atr_pct,bull,cost_rt\n");
    int tid=0;
    for(const auto& t:trades){
        int ei=idx_at(B,t.entryTs); int xi=idx_at(B,t.exitTs); if(xi<ei)xi=ei;
        double atrp=ap[ei]; int bull=(B[ei].c>sm[ei])?1:0;
        double cost_rt = eng.COST_PTS/t.entry_px + (2.0*hs)/t.entry_px; // pts cost + spread, as fraction
        int seq=0;
        for(int i=ei;i<=xi && i<(int)B.size();++i){
            fprintf(out,"%d,%d,%lld,%d,%.5f,%.5f,%.6f,%d,%.6f\n",
                tid,seq,(long long)B[i].ts_sec*1000LL,t.dir,t.entry_px,B[i].c,atrp,bull,cost_rt);
            seq++;
        }
        tid++;
    }
    fclose(out); return 0;
}

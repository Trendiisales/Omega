// =============================================================================
// fx_xrev_engine_fidelity.cpp -- prove production FxCrossRevEngine reproduces the
// validated cross-RV D1 edge (S43, harness-fidelity mandate).
// Feeds real Dukascopy D1 EURGBP + AUDNZD through include/FxCrossRevEngine.hpp at
// the cluster-center config; tallies PF / WR / total / trade count per pair.
// PASS = EURGBP PF~1.5-2.0, AUDNZD PF>=1.3, both positive, ~50-160 trades, WR>55%
// -- same character as backtest/fx_cross_reversion.cpp REAL D1 cluster.
//
// BUILD: c++ -std=c++17 -O2 -Iinclude backtest/fx_xrev_engine_fidelity.cpp -o backtest/fx_xrev_engine_fidelity
// =============================================================================
#include "FxCrossRevEngine.hpp"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <utility>
using namespace omega;

struct DBar { int64_t day_ms; double h,l,c; };
static std::vector<DBar> load_d1(const char* path){
    std::vector<DBar> v; FILE* f=fopen(path,"r"); if(!f){perror(path);return v;}
    char ln[256]; bool first=true;
    while(fgets(ln,sizeof ln,f)){
        if(first){first=false; if(ln[0]<'0'||ln[0]>'9') continue;}
        double ts,o,h,l,c; if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5) continue;
        if(c<=0) continue;
        {time_t _t=(time_t)(ts/1000.0);struct tm _g;gmtime_r(&_t,&_g);if(_g.tm_wday==6)continue;}
        int64_t day_ms=(((int64_t)(ts/1000.0))/86400)*86400*1000LL;
        v.push_back({day_ms,h,l,c});
    }
    fclose(f); return v;
}

static void run(const char* sym,const char* path,double hs_bps,
                int w,double zin,double zout,int hold,bool hook){
    auto bars=load_d1(path);
    if(bars.size()<260){ printf("%-8s no data\n",sym); return; }
    FxCrossRevEngine eng(sym);
    eng.enabled=true; eng.shadow_mode=true; eng.lot=0.01;
    eng.p.z_window=w; eng.p.z_in=zin; eng.p.z_out=zout; eng.p.hold_timeout=hold; eng.p.require_hook=hook;
    double gw=0,gl=0; int n=0,win=0;
    std::vector<std::pair<long long,double>> dump;
    auto cb=[&](const omega::TradeRecord& t){ ++n; if(t.net_pnl>0){++win;gw+=t.net_pnl;} else gl+=-t.net_pnl; dump.push_back({(long long)t.exitTs,t.net_pnl}); };
    const double sp=hs_bps/10000.0;
    for(auto&b:bars){ double half=b.c*sp; eng.on_d1_bar(b.h,b.l,b.c,b.c-half,b.c+half,b.day_ms,cb); }
    eng.force_close(bars.back().day_ms,cb);
    if(const char* pd=getenv("PORT_DUMP")){ FILE* pf=fopen(pd,"w"); if(pf){ for(auto&x:dump) fprintf(pf,"%lld,%.6f\n",x.first,x.second); fclose(pf);} }
    double pf = gl>0? gw/gl : (gw>0?99:0);
    printf("%-8s w%-3d zin%.1f zout%.1f h%-2d %-4s  n=%-4d WR=%4.1f%%  PF=%.2f  net=%+8.2f\n",
           sym,w,zin,zout,hold,hook?"HOOK":"raw",n,n?100.0*win/n:0,pf,gw-gl);
}

int main(){
    printf("FxCrossRevEngine FIDELITY -- production engine on REAL Dukascopy D1 (per-pair cfg)\n\n");
    #define DK(p) "/Users/jo/Omega/download/" p "-d1-bid-2019-01-01-2026-05-31.csv"
    // EURGBP cluster center (backtest w60/zin2.0/zout0.3/h20 raw = PF2.01)
    run("EURGBP",DK("eurgbp"),0.8, 60,2.0,0.3,20,false);
    if(!getenv("PORT_DUMP")){
    run("EURGBP",DK("eurgbp"),0.8, 60,2.0,0.5,40,false);
    // AUDNZD own cluster (backtest w40/zin2.5/zout0.3/h20 HOOK = PF3.09; raw w40/zin2.5/zout0.5/h40 PF1.49)
    run("AUDNZD",DK("audnzd"),1.5, 40,2.5,0.3,20,true);
    run("AUDNZD",DK("audnzd"),1.5, 40,2.5,0.5,40,false);
    }
    printf("\nPASS if EURGBP PF~2.0 + AUDNZD PF>=1.4, both net+ (matches REAL-D1 backtest cluster).\n");
    return 0;
}

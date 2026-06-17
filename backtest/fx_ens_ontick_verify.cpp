// fx_ens_ontick_verify.cpp -- PRODUCTION-FAITHFUL driver: on_tick() ONLY.
// Production tick_fx.hpp drives FxEnsembleEngine purely via on_tick(bid,ask,now_ms,cb);
// the internal aggregator builds M15 and calls on_15m_bar itself. The existing
// revalidate harness ALSO calls on_15m_bar explicitly -> double bar stream.
// This harness feeds ticks ONLY, reconstructing each M15 bar as o->l->h->c ticks
// at distinct sub-bar timestamps, so the engine's own aggregator does the work
// exactly as in production.
//
// BUILD: c++ -std=c++17 -O2 -Iinclude backtest/fx_ens_ontick_verify.cpp -o /tmp/fxens_ontick
#include "FxEnsembleEngine.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <cctype>
using namespace omega;

struct M15 { int64_t ms; double o,h,l,c,hs; };

static std::vector<M15> load(const char* path){
    std::vector<M15> v; FILE* f=fopen(path,"r"); if(!f){perror(path);return v;}
    char ln[256];
    while(fgets(ln,sizeof ln,f)){
        if(!isdigit((unsigned char)ln[0])) continue;
        M15 b{}; double ms;
        if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf,%lf",&ms,&b.o,&b.h,&b.l,&b.c,&b.hs)<5) continue;
        b.ms=(int64_t)ms; if(b.c<=0) continue; v.push_back(b);
    }
    fclose(f); return v;
}
struct TR { long long exitTs; double pnl; std::string cell; };

static void configure(FxEnsembleEngine& e,const std::string& pair,double ms,double af){
    e.shadow_mode=true; e.enabled=true; e.lot=1.0;
    e.max_spread_price=ms; e.min_atr_floor=af;
    if(pair=="eurusd"){e.enable_cell(FxCellId::DONCHIAN_55_H1_LONG,3.0,1.0,24);e.enable_cell(FxCellId::KELTNER_H1_LONG,3.0,1.67,24);e.enable_cell(FxCellId::ASIAN_BREAK_H4_LONG,1.0,1.0,24);}
    else if(pair=="gbpusd"){e.enable_cell(FxCellId::BB_REV_20_H2_LONG,3.0,1.67,96);e.enable_cell(FxCellId::LONDON_MOMO_H4_LONG,1.0,1.5,48);}
    else if(pair=="audusd"){e.enable_cell(FxCellId::BB_REV_20_H4_LONG,3.0,0.67,24);}
    else if(pair=="usdcad"){e.enable_cell(FxCellId::THREE_BAR_MOM_H4_SHORT,1.5,3.33,24);e.enable_cell(FxCellId::LONDON_MOMO_H4_LONG,1.0,2.0,96);e.enable_cell(FxCellId::KELTNER_H2_SHORT,2.0,0.75,96);e.enable_cell(FxCellId::KUMO_BREAK_H2_SHORT,3.0,0.5,24);}
    else if(pair=="usdjpy"){e.enable_cell(FxCellId::DONCHIAN_20_H2_LONG,1.5,3.33,96);e.enable_cell(FxCellId::ENGULFING_D1_LONG,1.0,1.0,48);}
    else if(pair=="nzdusd"){e.enable_cell(FxCellId::LONDON_MOMO_H2_SHORT,1.5,1.0,24);}
    e.init();
}

static void run(const std::string& pair,const char* path,double ms_g,double af,
                double base_hs,double stress){
    auto bars=load(path);
    if(bars.size()<300){printf("%-7s NO DATA\n",pair.c_str());return;}
    FxEnsembleEngine eng(pair.c_str()); configure(eng,pair,ms_g,af);
    std::vector<TR> trades;
    auto cb=[&](const omega::TradeRecord& t){ trades.push_back({(long long)t.exitTs,t.net_pnl,t.regime}); };
    int64_t midpoint=bars[bars.size()/2].ms;
    // Feed 4 ticks per bar at distinct timestamps WITHIN the 15-min window so the
    // internal aggregator keeps them in the same M15; the NEXT bar's ticks flush it.
    // Order o -> l -> h -> c (conservative: adverse before favourable for longs).
    for(auto& b: bars){
        double hs=(b.hs>0? b.hs*stress : base_hs*stress);
        int64_t t0=b.ms, t1=b.ms+200000, t2=b.ms+400000, t3=b.ms+600000;
        auto tick=[&](double px,int64_t ts){ eng.on_tick(px-hs,px+hs,ts,cb); };
        tick(b.o,t0); tick(b.l,t1); tick(b.h,t2); tick(b.c,t3);
    }
    eng.force_close(bars.back().c-base_hs*stress,bars.back().c+base_hs*stress,bars.back().ms,cb,"END");
    if(trades.empty()){printf("%-7s 0 trades\n",pair.c_str());return;}
    std::map<std::string,std::vector<double>> bycell;
    for(auto&t:trades) bycell[t.cell].push_back(t.pnl);
    double gw=0,gl=0,net=0;int n=0,win=0;double h1=0,h2=0;std::vector<double> ws;
    for(auto&t:trades){++n;net+=t.pnl;if(t.pnl>0){++win;gw+=t.pnl;ws.push_back(t.pnl);}else gl+=-t.pnl;
        if(t.exitTs*1000LL<midpoint) h1+=t.pnl; else h2+=t.pnl;}
    std::sort(ws.rbegin(),ws.rend());double top3=0;for(int i=0;i<3&&i<(int)ws.size();++i)top3+=ws[i];
    double pf=gl>0?gw/gl:(gw>0?99:0);
    printf("== %-7s stress=%.1fx ==\n",pair.c_str(),stress);
    for(auto&kv:bycell){double cw=0,cl=0,cn=0,cwin=0;for(double p:kv.second){++cn;if(p>0){++cwin;cw+=p;}else cl+=-p;}
        printf("   cell %-12s n=%-4.0f WR=%4.1f%% PF=%.2f net=%+10.5f\n",kv.first.c_str(),cn,cn?100*cwin/cn:0,cl>0?cw/cl:(cw>0?99:0),cw-cl);}
    printf("   PAIR TOTAL n=%d WR=%.1f%% PF=%.2f net=%+.5f | H1=%+.5f H2=%+.5f | top3=%.0f%%\n\n",
           n,100.0*win/n,pf,net,h1,h2,gw>0?top3/gw*100:0);
}
int main(int argc,char**argv){
    double stress=(argc>1)?atof(argv[1]):1.0;
    printf("FxEnsemble PRODUCTION-FAITHFUL (on_tick only) stress=%.1fx\n\n",stress);
    #define D(p) "/tmp/fxens/" p "_m15.csv"
    run("eurusd",D("eurusd"),0.00030,0.00010,0.000075,stress);
    run("gbpusd",D("gbpusd"),0.00035,0.00012,0.00009,stress);
    run("audusd",D("audusd"),0.00035,0.00012,0.00009,stress);
    run("usdcad",D("usdcad"),0.00040,0.00015,0.000065,stress);
    run("usdjpy",D("usdjpy"),0.050,0.01,0.009,stress);
    run("nzdusd",D("nzdusd"),0.00040,0.00015,0.000055,stress);
    return 0;
}

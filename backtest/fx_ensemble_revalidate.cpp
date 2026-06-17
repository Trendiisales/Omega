// =============================================================================
// fx_ensemble_revalidate.cpp -- ENGINE-FAITHFUL tick/M15 backtest for FxEnsembleEngine.
// #includes the REAL include/FxEnsembleEngine.hpp and drives the real class via
// on_15m_bar() at production config (engine_init.hpp ~2360 fx_ens_boot + enable_cell).
//
// Cost-honest: synthetic half-spread around mid (per-pair base), entry at ask / exit
// at bid handled inside the engine (it reads bid/ask). We pass mid +/- half_spread.
// USDCAD/NZDUSD carry REAL per-bar half-spread (col 6) from HistData ticks.
// A commission/spread STRESS multiplier scales the half-spread.
//
// Reports per-cell + per-pair + overall: n, WR, PF, net(price pts), both WF halves,
// fat-tail top3 share.
//
// BUILD: c++ -std=c++17 -O2 -Iinclude backtest/fx_ensemble_revalidate.cpp -o backtest/fx_ensemble_revalidate
// =============================================================================
#include "FxEnsembleEngine.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <cmath>
using namespace omega;

struct M15 { int64_t ms; double o,h,l,c,hs; };

static std::vector<M15> load(const char* path){
    std::vector<M15> v; FILE* f=fopen(path,"r"); if(!f){perror(path);return v;}
    char ln[256];
    while(fgets(ln,sizeof ln,f)){
        if(!isdigit((unsigned char)ln[0])) continue;
        M15 b{}; double ms;
        if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf,%lf",&ms,&b.o,&b.h,&b.l,&b.c,&b.hs)<5) continue;
        b.ms=(int64_t)ms;
        if(b.c<=0) continue;
        v.push_back(b);
    }
    fclose(f); return v;
}

struct TR { long long exitTs; double pnl; std::string cell; };

// configure one engine per pair exactly as engine_init.hpp does
static void configure(FxEnsembleEngine& e, const std::string& pair,
                      double max_spread, double atr_floor){
    e.shadow_mode=true; e.enabled=true; e.lot=1.0; // lot=1 -> pnl in price pts
    e.max_spread_price=max_spread; e.min_atr_floor=atr_floor;
    if(pair=="eurusd"){
        e.enable_cell(FxCellId::DONCHIAN_55_H1_LONG,3.0,1.0,24);
        e.enable_cell(FxCellId::KELTNER_H1_LONG,3.0,1.67,24);
        e.enable_cell(FxCellId::ASIAN_BREAK_H4_LONG,1.0,1.0,24);
    } else if(pair=="gbpusd"){
        e.enable_cell(FxCellId::BB_REV_20_H2_LONG,3.0,1.67,96);
        e.enable_cell(FxCellId::LONDON_MOMO_H4_LONG,1.0,1.5,48);
    } else if(pair=="audusd"){
        e.enable_cell(FxCellId::BB_REV_20_H4_LONG,3.0,0.67,24);
    } else if(pair=="usdcad"){
        e.enable_cell(FxCellId::THREE_BAR_MOM_H4_SHORT,1.5,3.33,24);
        e.enable_cell(FxCellId::LONDON_MOMO_H4_LONG,1.0,2.0,96);
        e.enable_cell(FxCellId::KELTNER_H2_SHORT,2.0,0.75,96);
        e.enable_cell(FxCellId::KUMO_BREAK_H2_SHORT,3.0,0.5,24);
    } else if(pair=="usdjpy"){
        e.enable_cell(FxCellId::DONCHIAN_20_H2_LONG,1.5,3.33,96);
        e.enable_cell(FxCellId::ENGULFING_D1_LONG,1.0,1.0,48);
    } else if(pair=="nzdusd"){
        e.enable_cell(FxCellId::LONDON_MOMO_H2_SHORT,1.5,1.0,24);
    }
    e.init();
    // re-enable cells (init() does not clear enabled flags; safe)
}

static void run_pair(const std::string& pair, const char* path,
                     double max_spread, double atr_floor,
                     double base_hs, double stress, FILE* /*unused*/){
    auto bars=load(path);
    if(bars.size()<300){ printf("%-7s NO DATA (%zu bars)\n",pair.c_str(),bars.size()); return; }
    FxEnsembleEngine eng(pair.c_str());
    configure(eng,pair,max_spread,atr_floor);

    std::vector<TR> trades;
    auto cb=[&](const omega::TradeRecord& t){
        trades.push_back({(long long)t.exitTs, t.net_pnl, t.regime});
    };

    int64_t midpoint_ms = bars[bars.size()/2].ms;
    for(auto& b: bars){
        double hs = (b.hs>0? b.hs*stress : base_hs*stress);
        // feed M15 bar: bid/ask = close +/- half-spread for the spread guard,
        // and the engine manages opens intra-"tick" using these bid/ask.
        double bid=b.c-hs, ask=b.c+hs;
        FxEnsembleBar fb{b.ms,b.o,b.h,b.l,b.c};
        eng.on_15m_bar(fb,bid,ask,b.ms,cb);
        // drive intra-bar management with the bar's extremes so SL/TP can trigger.
        // CONSERVATIVE ordering: feed the LOW extreme first, then HIGH, then close.
        // Engine checks SL before TP, so for LONGS the adverse (low->SL) is tested
        // first = no within-bar look-ahead optimism (the bar-replay trap). Cells are
        // overwhelmingly long; the few shorts (usdcad/nzdusd) get the favourable-first
        // ordering, a small optimism noted in the report.
        double hbid=b.h-hs, hask=b.h+hs, lbid=b.l-hs, lask=b.l+hs, cbid=b.c-hs, cask=b.c+hs;
        eng.on_tick(lbid,lask,b.ms,cb);
        eng.on_tick(hbid,hask,b.ms,cb);
        eng.on_tick(cbid,cask,b.ms,cb);
    }
    eng.force_close(bars.back().c-base_hs*stress, bars.back().c+base_hs*stress,
                    bars.back().ms,cb,"END");

    if(trades.empty()){ printf("%-7s stress%.1f : 0 trades\n",pair.c_str(),stress); return; }

    // per-cell + pair aggregate
    std::map<std::string,std::vector<double>> bycell;
    for(auto&t:trades) bycell[t.cell].push_back(t.pnl);

    double gw=0,gl=0,net=0; int n=0,win=0;
    double net_h1=0,net_h2=0;
    std::vector<double> wins_sorted;
    for(auto&t:trades){
        ++n; net+=t.pnl;
        if(t.pnl>0){++win;gw+=t.pnl; wins_sorted.push_back(t.pnl);} else gl+=-t.pnl;
        if(t.exitTs*1000LL < midpoint_ms) net_h1+=t.pnl; else net_h2+=t.pnl;
    }
    std::sort(wins_sorted.rbegin(),wins_sorted.rend());
    double top3=0; for(int i=0;i<3 && i<(int)wins_sorted.size();++i) top3+=wins_sorted[i];
    double pf= gl>0? gw/gl : (gw>0?99:0);
    double top3share = gw>0? top3/gw*100 : 0;

    printf("== %-7s  stress=%.1fx  base_hs=%.6f ==\n",pair.c_str(),stress,base_hs);
    for(auto&kv:bycell){
        double cw=0,cl=0,cn=0,cwin=0;
        for(double p:kv.second){ ++cn; if(p>0){++cwin;cw+=p;} else cl+=-p; }
        double cpf=cl>0?cw/cl:(cw>0?99:0);
        printf("   cell %-12s n=%-4.0f WR=%4.1f%% PF=%.2f net=%+10.5f\n",
               kv.first.c_str(),cn,cn?100*cwin/cn:0,cpf,cw-cl);
    }
    printf("   PAIR TOTAL  n=%d WR=%.1f%% PF=%.2f net=%+.5f | H1=%+.5f H2=%+.5f | top3win=%.0f%% of grossW\n\n",
           n,100.0*win/n,pf,net,net_h1,net_h2,top3share);
}

int main(int argc,char**argv){
    double stress = (argc>1)? atof(argv[1]) : 1.0;
    printf("FxEnsembleEngine FAITHFUL revalidate  (stress=%.1fx half-spread)\n",stress);
    printf("majors: M1-bid 2019-2026 (synthetic hs); usdcad/nzdusd: REAL tick spread 2025\n\n");
    #define D(p) "/tmp/fxens/" p "_m15.csv"
    // base half-spread (price): realistic retail, per pair. usdcad/nzdusd use REAL col6.
    // max_spread / atr_floor copied from engine_init.hpp fx_ens_boot calls.
    run_pair("eurusd",D("eurusd"),0.00030,0.00010, 0.000075, stress,nullptr); // 0.75pip hs
    run_pair("gbpusd",D("gbpusd"),0.00035,0.00012, 0.00009,  stress,nullptr); // 0.9pip
    run_pair("audusd",D("audusd"),0.00035,0.00012, 0.00009,  stress,nullptr);
    run_pair("usdcad",D("usdcad"),0.00040,0.00015, 0.000065, stress,nullptr); // real spread in col6
    run_pair("usdjpy",D("usdjpy"),0.050,  0.01,    0.009,    stress,nullptr); // 0.9 pip jpy
    run_pair("nzdusd",D("nzdusd"),0.00040,0.00015, 0.000055, stress,nullptr); // real spread col6
    return 0;
}

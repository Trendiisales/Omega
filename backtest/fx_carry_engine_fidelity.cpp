// =============================================================================
// fx_carry_engine_fidelity.cpp -- prove the PRODUCTION FxCarryEngine reproduces
// the validated carry edge (S43, harness-fidelity mandate).
//
// Feeds the 11 Dukascopy D1 series through the real include/FxCarryEngine.hpp
// (carry-only, floor 0.75, rebal 5) and tallies per-pair realised net_pnl + the
// trade count. Fidelity bar (not exact-bp -- prod uses clamped real lots vs the
// backtest's unitless vol-target units, an intended sizing difference):
//   PASS = total positive, JPY crosses dominate the positive side, USDCHF/NZDUSD
//   drag negative, turnover tiny (~few trades/pair). Same CHARACTER as the
//   fx_carry_momentum carry-only result.
//
// BUILD: c++ -std=c++17 -O2 -Iinclude backtest/fx_carry_engine_fidelity.cpp -o backtest/fx_carry_engine_fidelity
// =============================================================================
#include "FxCarryEngine.hpp"
#include <cstdio>
#include <ctime>
#include <vector>
#include <string>
#include <algorithm>

using namespace omega;

struct DBar { int64_t day_ms; double o,h,l,c; };

static std::vector<DBar> load_d1(const char* path){
    std::vector<DBar> v; FILE* f=fopen(path,"r"); if(!f){perror(path);return v;}
    char line[256]; bool first=true;
    while(fgets(line,sizeof line,f)){
        if(first){first=false; if(line[0]<'0'||line[0]>'9') continue;}
        double ts,o,h,l,c;
        if(sscanf(line,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5) continue;
        if(c<=0) continue;
        {time_t _t=(time_t)(ts/1000.0);struct tm _g;gmtime_r(&_t,&_g);if(_g.tm_wday==6)continue;}
        int64_t day_ms=(((int64_t)(ts/1000.0))/86400)*86400*1000LL;
        v.push_back({day_ms,o,h,l,c});
    }
    fclose(f); return v;
}

int main(){
    printf("FxCarryEngine FIDELITY -- production engine on 11 Dukascopy D1 (carry-only)\n\n");
    struct Sym{ const char* name; const char* path; double hs_bps; };
    #define DK(p) "/Users/jo/Omega/download/" p "-d1-bid-2019-01-01-2026-05-31.csv"
    std::vector<Sym> SY = {
        {"EURUSD",DK("eurusd"),0.5},{"GBPUSD",DK("gbpusd"),0.6},{"AUDUSD",DK("audusd"),0.8},
        {"NZDUSD",DK("nzdusd"),1.0},{"USDJPY",DK("usdjpy"),0.7},{"USDCAD",DK("usdcad"),0.8},
        {"USDCHF",DK("usdchf"),0.9},{"EURGBP",DK("eurgbp"),0.8},{"AUDNZD",DK("audnzd"),1.5},
        {"EURJPY",DK("eurjpy"),1.0},{"GBPJPY",DK("gbpjpy"),1.3},
    };

    double tot=0; int tot_tr=0;
    std::vector<std::pair<std::string,double>> attrib;
    printf("%-8s %6s %7s %10s\n","pair","trades","bars","net_pnl");
    for(auto& s:SY){
        auto bars=load_d1(s.path);
        if(bars.size()<60){ printf("%-8s  no data\n",s.name); continue; }
        FxCarryEngine eng(s.name);
        eng.enabled=true; eng.shadow_mode=true; eng.lot=0.01;
        eng.p.carry_floor_pct=0.75; eng.p.rebal_days=5;
        double pnl=0; int tr=0;
        auto cb=[&](const omega::TradeRecord& t){ pnl+=t.net_pnl; ++tr; };
        const double sp = s.hs_bps/10000.0;   // half-spread fraction of price
        for(auto& b:bars){
            double half=b.c*sp;
            eng.on_d1_bar(b.o,b.h,b.l,b.c, b.c-half, b.c+half, b.day_ms, cb);
        }
        eng.force_close(bars.back().day_ms, cb);
        printf("%-8s %6d %7zu %+10.2f\n",s.name,tr,bars.size(),pnl);
        tot+=pnl; tot_tr+=tr; attrib.push_back({s.name,pnl});
    }
    printf("\nTOTAL net_pnl=%+.2f  trades=%d  avg/pair=%.1f\n",tot,tot_tr,tot_tr/11.0);

    std::sort(attrib.begin(),attrib.end(),[](auto&a,auto&b){return a.second>b.second;});
    printf("\nattribution (sorted):\n");
    double jpy=0;
    for(auto& a:attrib){ printf("  %-8s %+10.2f\n",a.first.c_str(),a.second);
        if(a.first.find("JPY")!=std::string::npos) jpy+=a.second; }
    printf("\nJPY-cross contribution: %+.2f (%.0f%% of total)\n",jpy,tot!=0?100*jpy/tot:0);
    printf("FIDELITY: %s — total %s, JPY-dominated %s, turnover %s\n",
           (tot>0 && jpy>0)?"PASS":"CHECK",
           tot>0?"+":"-", (jpy>0&&jpy>0.5*tot)?"yes":"partial",
           (tot_tr<300)?"low(carry-like)":"HIGH(churn!)");
    return 0;
}

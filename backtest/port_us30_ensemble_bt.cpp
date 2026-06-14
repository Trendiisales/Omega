// =============================================================================
// port_us30_ensemble_bt.cpp -- PORTFOLIO per-trade dump for Us30EnsembleEngine.
// Drives the ACTUAL engine header. Validated cfg = HEADER DEFAULTS (lot 0.01,
// BE/trail OFF -- the proven bare fixed-SL/TP config, +$1411/1711tr per docstring;
// 2 cells InsBrkH1 + RsiExtH1). Builds M15 bars from DJ30 (USA30) tick CSV,
// feeds on_15m_bar (entries) + intrabar on_tick (exits).
//
// Build: c++ -std=c++17 -O2 -Iinclude backtest/port_us30_ensemble_bt.cpp -o /tmp/port/us30_bt
// Run:   PORT_DUMP=/tmp/port/Us30Ensemble_trades.txt /tmp/port/us30_bt <tick.csv>
// Tick CSV cols: timestamp_ms,askPrice,bidPrice
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include "Us30EnsembleEngine.hpp"

static constexpr double USD_PER_PT = omega::Us30EnsembleEngine::DJ30_USD_PER_PT;

int main(int argc,char**argv){
    const char* path = argc>1?argv[1]:"/Users/jo/Tick/USA30/usa30idxusd-tick-2024-03-01-2026-04-30.csv";
    FILE* f=fopen(path,"r"); if(!f){fprintf(stderr,"no %s\n",path);return 1;}

    omega::Us30EnsembleEngine eng;     // header defaults = validated config
    eng.enabled=true; eng.shadow_mode=true;
    eng.init();

    std::vector<std::pair<int64_t,double>> trades;
    auto on_close=[&](const omega::TradeRecord& tr){ trades.push_back({tr.exitTs, tr.pnl*USD_PER_PT}); };

    // M15 bar builder from ticks.
    char ln[256]; bool first=true; long nt=0;
    int64_t cur_bar=-1, bar_start_ms=0; double bo=0,bh=0,bl=0,bc=0; double lastbid=0,lastask=0;
    auto flush_bar=[&](int64_t now_ms){
        if(cur_bar<0) return;
        omega::Us30EnsembleBar b{}; b.bar_start_ms=bar_start_ms; b.open=bo; b.high=bh; b.low=bl; b.close=bc;
        // entries on the completed M15 bar
        eng.on_15m_bar(b, lastbid, lastask, 0.0, now_ms, on_close);
        // intrabar exit management: replay O,H,L,C as ticks within the bar
        const double sp=lastask-lastbid>0?lastask-lastbid:bc*0.0001;
        double seq[4]={bo,bh,bl,bc};
        for(double px:seq){ eng.on_tick(px-sp*0.5, px+sp*0.5, now_ms, on_close); }
    };

    while(fgets(ln,sizeof ln,f)){
        if(first){first=false; if(ln[0]<'0'||ln[0]>'9')continue;}
        long long ts; double ask,bid;
        if(sscanf(ln,"%lld,%lf,%lf",&ts,&ask,&bid)!=3)continue;
        if(ask<=0||bid<=0)continue;
        ++nt; lastbid=bid; lastask=ask;   // dense duka_ticks feed: real ~2pt spread
        const double mid=(ask+bid)*0.5;
        int64_t bar = (ts/1000)/900;   // M15 = 900s
        if(bar!=cur_bar){
            if(cur_bar>=0) flush_bar(ts);     // close prior bar at this tick's time
            cur_bar=bar; bar_start_ms=bar*900*1000; bo=bh=bl=bc=mid;
        } else { if(mid>bh)bh=mid; if(mid<bl)bl=mid; bc=mid; }
    }
    // final bar
    if(cur_bar>=0) flush_bar((cur_bar*900+900)*1000);
    fclose(f);

    double net=0,gw=0,gl=0; int w=0;
    for(auto&t:trades){net+=t.second; if(t.second>0){gw+=t.second;++w;}else gl+=-t.second;}
    printf("[Us30Ensemble] ticks=%ld trades=%zu net$=%.2f WR=%.1f%% PF=%.2f\n",
        nt,trades.size(),net,trades.empty()?0:100.0*w/trades.size(),gl>1e-9?gw/gl:0);
    if(getenv("PORT_DUMP")){FILE*pd=fopen(getenv("PORT_DUMP"),"w");for(auto&t:trades)fprintf(pd,"%lld,%.4f\n",(long long)t.first,t.second);fclose(pd);}
    return 0;
}

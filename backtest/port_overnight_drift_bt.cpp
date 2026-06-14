// =============================================================================
// port_overnight_drift_bt.cpp -- PORTFOLIO per-trade dump for OvernightDriftEngine.
// Drives the ACTUAL engine header via M15 tick replay (validated cfg: SMA20,
// trend-gated, flat at cash open). One trade = one overnight hold. Cost-gate via
// ExecutionCostGuard (production path). shadow_mode irrelevant to pnl.
//
// Build: c++ -std=c++17 -O2 -Iinclude backtest/port_overnight_drift_bt.cpp -o /tmp/port/ovd_bt
// Run:   PORT_DUMP=/tmp/port/OvernightDrift_trades.txt /tmp/port/ovd_bt <m15.csv> [SMA]
// CSV cols: ts(sec),o,h,l,c
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include "OvernightDriftEngine.hpp"

int main(int argc,char**argv){
    const char* path = argc>1?argv[1]:"/tmp/amrbt/SPXUSD_M15.csv";
    int sma = argc>2?atoi(argv[2]):20;
    FILE* f=fopen(path,"r"); if(!f){fprintf(stderr,"no %s\n",path);return 1;}
    omega::OvernightDriftEngine eng;
    eng.symbol="SPX500"; eng.engine_name="OvernightDrift";
    eng.SMA_LEN=sma; eng.lot=1.0; eng.stop_pct=0.0; eng.enabled=true; eng.shadow_mode=true; eng.verbose=false;
    eng.init();
    std::vector<std::pair<int64_t,double>> trades;
    eng.on_trade_record=[&](const omega::TradeRecord& tr){ trades.push_back({tr.exitTs,tr.pnl}); };

    char ln[256]; bool first=true; long n=0;
    while(fgets(ln,sizeof ln,f)){
        if(first){first=false; if(ln[0]<'0'||ln[0]>'9')continue;}
        double ts,o,h,l,c; if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5)continue; if(c<=0)continue;
        int64_t ms=(int64_t)(ts*1000.0);
        const double sp=c*0.00005;      // ~0.5bp spread (SPX CFD tight)
        eng.on_tick(c-sp, c+sp, ms);    // mid=c; one tick per M15 bar close
        ++n;
    }
    fclose(f);
    double net=0,gw=0,gl=0; int w=0;
    for(auto&t:trades){net+=t.second; if(t.second>0){gw+=t.second;++w;}else gl+=-t.second;}
    printf("[OvernightDrift] bars=%ld trades=%zu net=%.2f WR=%.1f%% PF=%.2f\n",
        n,trades.size(),net,trades.empty()?0:100.0*w/trades.size(),gl>1e-9?gw/gl:0);
    if(getenv("PORT_DUMP")){FILE*pd=fopen(getenv("PORT_DUMP"),"w");for(auto&t:trades)fprintf(pd,"%lld,%.4f\n",(long long)t.first,t.second);fclose(pd);}
    return 0;
}

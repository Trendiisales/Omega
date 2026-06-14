// =============================================================================
// port_index_session_bt.cpp -- PORTFOLIO per-trade dump for IndexSessionEngine.
// Drives the ACTUAL engine header via M15 tick replay. Validated SPX cfg:
// RTH 14-22 UTC, STOP_ATR 2.0, SKIP_FRIDAY, ENTER_ON_WEAK_ONLY (dip-buy, the
// edge -- ON in production engine_init). ATR seeded from D1 dukascopy CSV.
//
// Build: c++ -std=c++17 -O2 -Iinclude backtest/port_index_session_bt.cpp -o /tmp/port/sess_bt
// Run:   PORT_DUMP=/tmp/port/IndexSession_trades.txt /tmp/port/sess_bt <m15.csv> <d1_seed.csv>
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include "IndexSessionEngine.hpp"

int main(int argc,char**argv){
    const char* path = argc>1?argv[1]:"/tmp/amrbt/SPXUSD_M15.csv";
    const char* d1   = argc>2?argv[2]:"/Users/jo/Omega/download/usa500idxusd-d1-bid-2019-01-01-2026-05-31.csv";
    omega::IndexSessionEngine eng;
    eng.symbol="SPX500"; eng.engine_name="IndexSession";
    eng.RTH_OPEN_H=14; eng.RTH_CLOSE_H=22; eng.STOP_ATR=2.0; eng.SKIP_FRIDAY=true;
    eng.ENTER_ON_WEAK_ONLY=true; eng.WEAK_PREV_RET=0.0;   // dip-buy filter (validated edge, prod default ON)
    eng.lot=1.0; eng.enabled=true; eng.shadow_mode=true; eng.verbose=false;
    eng.init();
    eng.seed_from_d1_csv(d1);   // warm ATR

    std::vector<std::pair<int64_t,double>> trades;
    eng.on_trade_record=[&](const omega::TradeRecord& tr){ trades.push_back({tr.exitTs,tr.pnl}); };

    FILE* f=fopen(path,"r"); if(!f){fprintf(stderr,"no %s\n",path);return 1;}
    char ln[256]; bool first=true; long n=0;
    while(fgets(ln,sizeof ln,f)){
        if(first){first=false; if(ln[0]<'0'||ln[0]>'9')continue;}
        double ts,o,h,l,c; if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5)continue; if(c<=0)continue;
        int64_t ms=(int64_t)(ts*1000.0);
        const double sp=c*0.00005;
        eng.on_tick(c-sp, c+sp, ms);
        ++n;
    }
    fclose(f);
    double net=0,gw=0,gl=0; int w=0;
    for(auto&t:trades){net+=t.second; if(t.second>0){gw+=t.second;++w;}else gl+=-t.second;}
    printf("[IndexSession] bars=%ld trades=%zu net=%.2f WR=%.1f%% PF=%.2f\n",
        n,trades.size(),net,trades.empty()?0:100.0*w/trades.size(),gl>1e-9?gw/gl:0);
    if(getenv("PORT_DUMP")){FILE*pd=fopen(getenv("PORT_DUMP"),"w");for(auto&t:trades)fprintf(pd,"%lld,%.4f\n",(long long)t.first,t.second);fclose(pd);}
    return 0;
}

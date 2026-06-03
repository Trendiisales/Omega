// GoldSeasonalBacktest.cpp -- engine-driven verification of GoldSeasonalEngine.
// Feeds the REAL M5 gold corpus (which contains the ~21:00 UTC daily break gaps)
// as ticks through the actual engine, confirming the Mon+Tue long edge survives
// intraday execution + break handling.
//   build: g++ -O2 -std=c++17 -Iinclude backtest/GoldSeasonalBacktest.cpp -o /tmp/gsbt
//   run  : /tmp/gsbt /Users/jo/Tick/2yr_XAUUSD_tick_fresh.m5.csv
//   result (2026-06-03): trades=224 /yr=+24.5% PF=1.68 Sharpe=1.88 win=59% maxDD=10.3%
//                        per-year 2024:+4.7% 2025:+25.1% 2026:+22.8% (matches daily sim)
// GoldSeasonal engine-driven backtest: feed real M5 ticks (incl 21:00 break) through
// the actual GoldSeasonalEngine, verify the Mon+Tue edge survives intraday execution.
#include <cstdio>
#include <vector>
#include <string>
#include <cmath>
#include <fstream>
#include <map>
#include "GoldSeasonalEngine.hpp"
int main(int argc,char**argv){
    std::ifstream f(argv[1]); std::string ln; std::getline(f,ln);
    std::vector<std::pair<long,double>> bars;
    while(std::getline(f,ln)){double ts,o,h,l,c; if(sscanf(ln.c_str(),"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5)continue; if(c>0)bars.push_back({(long)ts,c});}
    if(bars.size()<1000){printf("few\n");return 1;}
    omega::GoldSeasonalEngine g; g.shadow_mode=false; g.enabled=true; g.lot=0.01;
    std::vector<double> rets; double cum=0,peak=0,mdd=0,gw=0,gl=0; int nw=0,nl=0;
    std::map<int,double> yr;
    auto cb=[&](const omega::TradeRecord&tr){
        double pct=(tr.exitPrice-tr.entryPrice)/tr.entryPrice*100.0;
        double cost=0.37/tr.entryPrice*100.0;  // ~0.37pt round-trip
        pct-=cost; rets.push_back(pct); cum+=pct; if(cum>peak)peak=cum; if(peak-cum>mdd)mdd=peak-cum;
        if(pct>0){nw++;gw+=pct;}else{nl++;gl+=-pct;}
        int year=1970+(int)(tr.exitTs/31557600); yr[year]+=pct;
    };
    const double SP=0.37;
    for(auto&b:bars) g.on_tick(b.second-SP/2, b.second+SP/2, (int64_t)b.first*1000, cb);
    g.force_close((int64_t)bars.back().first*1000, cb);
    int n=rets.size(); double mean=0;for(double r:rets)mean+=r; mean/=std::max(n,1);
    double sd=0;for(double r:rets)sd+=(r-mean)*(r-mean); sd=std::sqrt(sd/std::max(n-1,1));
    double yrs=(bars.back().first-bars.front().first)/31557600.0;
    double sharpe=sd>0?mean/sd*std::sqrt(n/yrs):0;
    double pf=gl>0?gw/gl:0;
    printf("GoldSeasonal engine-driven (M5, real break)  trades=%d  net=%.1f%%  /yr=%.1f%%  PF=%.2f  Sharpe=%.2f  win=%.0f%%  maxDD=%.1f%%\n",
        n,cum,cum/yrs,pf,sharpe,100.0*nw/std::max(n,1),mdd);
    printf("per-year:"); for(auto&kv:yr)printf(" %d:%+.1f%%",kv.first,kv.second); printf("\n");
    return 0;
}

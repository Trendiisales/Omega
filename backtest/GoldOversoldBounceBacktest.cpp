// GoldOversoldBounceBacktest.cpp -- engine-driven verification.
// Feeds the broker M5 gold corpus (real ticks incl 21:00 break) through the
// actual GoldOversoldBounceEngine. 2yr sample is bull-only (regime-robustness
// was established on 18yr GC=F in python); this confirms the engine reproduces
// the RSI<30 bounce signal on the broker feed.
//   build: g++ -O2 -std=c++17 -Iinclude backtest/GoldOversoldBounceBacktest.cpp -o /tmp/gob
//   run  : /tmp/gob /Users/jo/Tick/2yr_XAUUSD_tick_fresh.m5.csv
#include <cstdio>
#include <vector>
#include <string>
#include <cmath>
#include <fstream>
#include <map>
#include "GoldOversoldBounceEngine.hpp"
int main(int argc,char**argv){
    std::ifstream f(argv[1]); std::string ln; std::getline(f,ln);
    std::vector<std::pair<long,double>> bars;
    while(std::getline(f,ln)){double ts,o,h,l,c; if(sscanf(ln.c_str(),"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5)continue; if(c>0)bars.push_back({(long)ts,c});}
    if(bars.size()<1000){printf("few\n");return 1;}
    omega::GoldOversoldBounceEngine g; g.shadow_mode=false; g.enabled=true; g.lot=0.01;
    std::vector<double> rets; double cum=0,peak=0,mdd=0,gw=0,gl=0; int nw=0,nl=0;
    std::map<int,double> yr; std::map<std::string,int> why;
    auto cb=[&](const omega::TradeRecord&tr){
        double pct=(tr.exitPrice-tr.entryPrice)/tr.entryPrice*100.0;
        double cost=0.37/tr.entryPrice*100.0; pct-=cost;
        rets.push_back(pct); cum+=pct; if(cum>peak)peak=cum; if(peak-cum>mdd)mdd=peak-cum;
        if(pct>0){nw++;gw+=pct;}else{nl++;gl+=-pct;}
        yr[1970+(int)(tr.exitTs/31557600)]+=pct; why[tr.exitReason]++;
    };
    const double SP=0.37;
    for(auto&b:bars) g.on_tick(b.second-SP/2,b.second+SP/2,(int64_t)b.first*1000,cb);
    g.force_close((int64_t)bars.back().first*1000,cb);
    int n=rets.size(); double mean=0; for(double r:rets)mean+=r; mean/=(n?n:1);
    double sd=0; for(double r:rets)sd+=(r-mean)*(r-mean); sd=std::sqrt(sd/(n>1?n-1:1));
    double yrs=(bars.back().first-bars.front().first)/31557600.0;
    double pf=gl>0?gw/gl:0;
    printf("GoldOversoldBounce engine-driven (broker M5)  trades=%d  net=%.1f%%  PF=%.2f  win=%.0f%%  meanRet=%.2f%%  maxDD=%.1f%%\n",
        n,cum,pf,100.0*nw/(n?n:1),mean,mdd);
    printf("per-year:"); for(auto&kv:yr)printf(" %d:%+.1f%%",kv.first,kv.second); printf("\n");
    printf("exits:"); for(auto&kv:why)printf(" %s=%d",kv.first.c_str(),kv.second); printf("\n");
    return 0;
}

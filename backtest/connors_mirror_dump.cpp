// connors_mirror_dump.cpp — drive the REAL ConnorsRSI2Engine (deployed config
// REGIME_GATE=1) on NDX daily and DUMP each closed parent trade as CSV for the
// ConnorsMirror x2-companion study (backtest/connors_mirror_bt.py).
// Output: entry_ts,exit_ts,side,entry_px,exit_px,size,pnl_gross
//
// build: clang++ -std=c++17 -O2 -I/Users/jo/Omega/include connors_mirror_dump.cpp -o /tmp/connors_dump
#include "ConnorsRSI2Engine.hpp"
#include <cstdio>
#include <vector>
#include <string>
#include <fstream>
#include <ctime>

struct Bar { int64_t ts; double o,h,l,c; };

static std::vector<Bar> load(const std::string& p){
    std::vector<Bar> v; std::ifstream f(p); std::string ln;
    long long ts; double o,h,l,c;
    while(std::getline(f,ln)){
        if(std::sscanf(ln.c_str(),"%lld,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)==5 && c>0)
            v.push_back({ts,o,h,l,c});
    }
    return v;
}

int main(int argc,char**argv){
    std::string path = argc>1? argv[1] : "/Users/jo/Tick/NDX_daily_2016_2026.csv";
    std::string out  = argc>2? argv[2] : "/tmp/connors_parent_trades.csv";
    auto bars=load(path);
    fprintf(stderr,"Loaded %zu NDX daily bars\n",bars.size());

    omega::ConnorsRSI2Engine e;
    e.symbol="NAS100"; e.TREND_SMA=200; e.RSI_IN=10.0; e.SHORT_SMA=5; e.MAXHOLD=10;
    e.SCALEIN=true; e.MAX_UNITS=2; e.lot=1.0; e.enabled=true; e.shadow_mode=true;
    e.REGIME_GATE=1; e.BEAR_VETO_K=20;   // deployed config
    e.init();

    FILE* fo=fopen(out.c_str(),"w");
    fprintf(fo,"entry_ts,exit_ts,side,entry_px,exit_px,size,pnl_gross\n");
    e.on_trade_record=[&](const omega::TradeRecord& tr){
        fprintf(fo,"%lld,%lld,%s,%.2f,%.2f,%.2f,%.2f\n",
            (long long)tr.entryTs,(long long)tr.exitTs,tr.side.c_str(),
            tr.entryPrice,tr.exitPrice,tr.size,tr.pnl);
    };
    for(const auto& b: bars){
        std::time_t t=(std::time_t)b.ts; std::tm g{}; gmtime_r(&t,&g);
        g.tm_hour=14; g.tm_min=0; g.tm_sec=0;           // in-RTH tick
        int64_t in_utc=(int64_t)timegm(&g);
        std::tm g2=g; g2.tm_hour=22;                     // out-of-RTH -> daily close logic
        int64_t out_utc=(int64_t)timegm(&g2);
        e.on_tick(b.c,b.c,in_utc*1000);
        e.on_tick(b.c,b.c,out_utc*1000);
    }
    e.force_close(bars.back().c,bars.back().c,bars.back().ts*1000);
    fclose(fo);
    fprintf(stderr,"Wrote %s\n",out.c_str());
    return 0;
}

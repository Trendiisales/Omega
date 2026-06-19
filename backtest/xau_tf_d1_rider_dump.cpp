// xau_tf_d1_rider_dump.cpp -- faithful XauTrendFollowD1 driver that ALSO dumps
// each real trade window (side, entry/exit px+ts, atr_at_entry, mfe, pnl) to a
// CSV so the rider overlay (Python) can replay over the real engine trades.
// Engine logic is 100% unchanged from XauTrendFollowD1Backtest.cpp.
//
// BUILD: c++ -std=c++17 -O2 -Iinclude backtest/xau_tf_d1_rider_dump.cpp -o /tmp/xtf_d1_dump
// RUN:   /tmp/xtf_d1_dump <h4_csv> <out_trades_csv>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include "XauTrendFollowD1Engine.hpp"

static const double SPREAD = 0.20;
struct BarCSV { int64_t ts; double o,h,l,c; };
static std::vector<BarCSV> load_csv(const char* path){
    std::vector<BarCSV> v; std::ifstream f(path);
    if(!f.is_open()){ std::fprintf(stderr,"cannot open %s\n",path); return v; }
    std::string line; bool first=true;
    while(std::getline(f,line)){
        if(line.empty()) continue;
        if(first){ first=false; if(line[0]<'0'||line[0]>'9') continue; }
        BarCSV b{}; double ts;
        if(std::sscanf(line.c_str(),"%lf,%lf,%lf,%lf,%lf",&ts,&b.o,&b.h,&b.l,&b.c)!=5) continue;
        b.ts=(int64_t)ts; v.push_back(b);
    }
    return v;
}

int main(int argc,char**argv){
    const char* path = argc>1?argv[1]:"/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv";
    const char* out  = argc>2?argv[2]:"/tmp/xtf_d1_trades.csv";
    auto bars = load_csv(path);
    if((int)bars.size()<200){ std::fprintf(stderr,"not enough bars (%zu)\n",bars.size()); return 1; }

    omega::XauTrendFollowD1Engine eng;
    eng.shadow_mode=true; eng.enabled=true; eng.lot=0.01; eng.max_spread=1.0;
    eng.use_vol_band_gate=false;
    eng.init();

    FILE* tf=std::fopen(out,"w");
    std::fprintf(tf,"engine,side,entry_ts,exit_ts,entry_px,exit_px,atr_at_entry,mfe,pnl_usd,exit_reason\n");
    int ntr=0; double net=0;
    auto cb=[&](const omega::TradeRecord& tr){
        const double usd=tr.pnl*100.0; net+=usd; ntr++;
        std::fprintf(tf,"%s,%s,%lld,%lld,%.3f,%.3f,%.4f,%.4f,%.2f,%s\n",
            tr.engine.c_str(), tr.side.c_str(),
            (long long)tr.entryTs, (long long)tr.exitTs,
            tr.entryPrice, tr.exitPrice, tr.atr_at_entry, tr.mfe, usd, tr.exitReason.c_str());
    };

    const int N=(int)bars.size();
    for(int i=0;i<N;++i){
        const auto& b=bars[i]; const int64_t ts=b.ts*1000;
        if(i>0){
            eng.on_tick(b.l, b.l+SPREAD, ts, cb);
            eng.on_tick(b.h, b.h+SPREAD, ts, cb);
            eng.on_tick(b.c, b.c+SPREAD, ts, cb);
        }
        omega::XauTfD1Bar bar{}; bar.bar_start_ms=ts; bar.open=b.o; bar.high=b.h; bar.low=b.l; bar.close=b.c;
        eng.on_h4_bar(bar, b.c, b.c+SPREAD, ts, cb);
    }
    eng.force_close(bars.back().c, bars.back().c+SPREAD, bars.back().ts*1000, cb, "EOD_FLAT");
    std::fclose(tf);
    std::printf("[dump] %d trades  net=$%.1f  -> %s\n", ntr, net, out);
    return 0;
}

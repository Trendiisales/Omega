// connors_legs_dump.cpp — generic per-leg parent-trade dumper for the ConnorsMR
// breadth book (S-2026-07-07t mirror dump-check). Drives the REAL ConnorsRSI2Engine
// with one live leg's exact config (engine_init cfg_mr transcription) over the
// symbol's daily CSV and dumps closed parent trades for the mirror sim
// (backtest/connors_mirror_bt.py sim() via connors_legs_mirror_bt.py).
//
// Per-leg standalone (BOOK_CAP untouched = 0/uncapped), same as the validated
// ConnorsRSI2 NAS100 dump (connors_mirror_dump.cpp) — the live BOOK_CAP=3 only
// removes trades, so a mirror judged on the uncapped set sees MORE parents.
//
// usage: connors_legs_dump <daily_csv> <out_csv> <entry_mode> <regime_gate>
// output: entry_ts,exit_ts,side,entry_px,exit_px,size,pnl_gross
//
// build: clang++ -std=c++17 -O2 -I/Users/jo/Omega/include connors_legs_dump.cpp -o /tmp/connors_legs_dump
#include "ConnorsRSI2Engine.hpp"
#include <cstdio>
#include <cstdlib>
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
    if(argc<5){ fprintf(stderr,"usage: %s <daily_csv> <out_csv> <entry_mode> <regime_gate>\n",argv[0]); return 1; }
    std::string path=argv[1], out=argv[2];
    const int mode=std::atoi(argv[3]), gate=std::atoi(argv[4]);
    auto bars=load(path);
    fprintf(stderr,"Loaded %zu daily bars from %s\n",bars.size(),path.c_str());

    omega::ConnorsRSI2Engine e;
    // exact cfg_mr transcription (engine_init.hpp Connors MR breadth book)
    e.symbol="LEG"; e.engine_name="LEG"; e.ENTRY_MODE=mode;
    e.TREND_SMA=200; e.SHORT_SMA=5; e.MAXHOLD=10;
    e.IBS_IN=0.10; e.STREAK_N=3; e.DBL_IBS=0.20; e.DBL_RSI=15.0;
    e.SESS_OPEN_HM=930; e.SESS_CLOSE_HM=1600; e.TZ_STD_OFF_MIN=-300; e.TZ_EU_DST=false;
    e.REGIME_GATE=gate; e.BEAR_VETO_K=20;
    e.lot=1.0; e.enabled=true; e.shadow_mode=true;
    e.init();

    FILE* fo=fopen(out.c_str(),"w");
    fprintf(fo,"entry_ts,exit_ts,side,entry_px,exit_px,size,pnl_gross\n");
    e.on_trade_record=[&](const omega::TradeRecord& tr){
        fprintf(fo,"%lld,%lld,%s,%.2f,%.2f,%.2f,%.2f\n",
            (long long)tr.entryTs,(long long)tr.exitTs,tr.side.c_str(),
            tr.entryPrice,tr.exitPrice,tr.size,tr.pnl);
    };
    // IBS/DOUBLE modes need the daily bar's TRUE high/low (engine aggregates
    // m_day_high/low from in-RTH ticks) — feed o/h/l/c as four in-RTH ticks.
    // 15..19 UTC = 10..14 EST / 11..15 EDT, inside 09:30-16:00 both regimes;
    // 22:00 UTC is out-of-RTH both -> daily close transition.
    for(const auto& b: bars){
        std::time_t t=(std::time_t)b.ts; std::tm g{}; gmtime_r(&t,&g);
        g.tm_min=0; g.tm_sec=0;
        auto at=[&](int hh){ std::tm x=g; x.tm_hour=hh; return (int64_t)timegm(&x)*1000; };
        e.on_tick(b.o,b.o,at(15));
        e.on_tick(b.h,b.h,at(16));
        e.on_tick(b.l,b.l,at(17));
        e.on_tick(b.c,b.c,at(19));
        e.on_tick(b.c,b.c,at(22));
    }
    e.force_close(bars.back().c,bars.back().c,bars.back().ts*1000);
    fclose(fo);
    fprintf(stderr,"Wrote %s\n",out.c_str());
    return 0;
}

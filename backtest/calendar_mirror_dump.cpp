// calendar_mirror_dump.cpp — parent-trade dumper for the calendar engines
// (CalendarTom x6 + MondayRiskOn_NAS100) for the S-2026-07-07t mirror
// dump-check (backtest/mirror_parents_bt.py).
//
// Drives the REAL engine classes with the live engine_init configs over the
// symbol's daily CSV:
//   tom    — CalendarTomEngine::on_d1_bar per consecutive daily-bar pair
//            (identical to its seed path but enabled=true; next-open fills).
//   monday — MondayRiskOnEngine::on_tick two ticks/day (00:30 open, 21:00
//            close), SMA50 gate warm from the drive itself.
//
// usage: calendar_mirror_dump <tom|monday> <daily_csv> <SYMBOL> <usd_per_pt> <out_csv>
// out: ShadowBook columns (entryTs,symbol,side,engine,entryPrice,exitPrice,pnl,mfe,mae,hold_sec,exitReason)
//
// build: clang++ -std=c++17 -O2 -I/Users/jo/Omega/include backtest/calendar_mirror_dump.cpp -o /tmp/calendar_mirror_dump
#include "CalendarTomEngine.hpp"
#include "MondayRiskOnEngine.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

struct Bar { int64_t ts; double o,h,l,c; };

static std::vector<Bar> load(const std::string& p){
    std::vector<Bar> v; std::ifstream f(p); std::string ln;
    while(std::getline(f,ln)){
        long long ts; double o,h,l,c;
        if(std::sscanf(ln.c_str(),"%lld,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)==5 && c>0){
            if(ts>100000000000LL) ts/=1000;          // ms -> s
            v.push_back({ts,o,h,l,c});
        }
    }
    return v;
}

int main(int argc,char**argv){
    if(argc<6){ std::fprintf(stderr,"usage: %s <tom|monday> <daily_csv> <SYMBOL> <usd_per_pt> <out_csv>\n",argv[0]); return 1; }
    const std::string mode=argv[1], path=argv[2], sym=argv[3];
    const double upp=std::atof(argv[4]);
    const std::string out=argv[5];
    auto bars=load(path);
    std::fprintf(stderr,"[%s %s] %zu daily bars\n",mode.c_str(),sym.c_str(),bars.size());
    if(bars.size()<300) return 1;

    FILE* fo=fopen(out.c_str(),"a");
    const bool fresh = ftell(fo)==0;
    if(fresh) std::fprintf(fo,"entryTs,symbol,side,engine,entryPrice,exitPrice,pnl,mfe,mae,hold_sec,exitReason\n");
    auto dump=[&](const omega::TradeRecord& tr,const char* eng){
        std::fprintf(fo,"%lld,%s,%s,%s,%.5f,%.5f,%.4f,0,0,%lld,%s\n",
            (long long)tr.entryTs,sym.c_str(),tr.side.c_str(),eng,
            tr.entryPrice,tr.exitPrice,tr.pnl,(long long)(tr.exitTs-tr.entryTs),tr.exitReason.c_str());
    };

    if(mode=="tom"){
        omega::CalendarTomEngine e(sym.c_str());
        e.shadow_mode=true; e.enabled=true; e.lot=0.01;
        e.p.target_vol_bps=60.0; e.p.usd_per_pt=upp; e.p.last_n=3; e.p.first_n=3;
        std::string eng="CalendarTom_"+sym;
        auto cb=[&](const omega::TradeRecord& tr){ dump(tr,eng.c_str()); };
        for(size_t i=0;i+1<bars.size();++i){
            const Bar& b=bars[i]; const Bar& nx=bars[i+1];
            // weekday guard as in seed path
            const int64_t dz=b.ts/86400; const int wd=(int)((dz+4)%7);
            if(wd==0||wd==6) continue;
            const int64_t day_ms=(b.ts/86400)*86400000LL;
            const int64_t new_day_ms=(nx.ts/86400)*86400000LL;
            const double sp=nx.o*0.00010;
            e.on_d1_bar(b.h,b.l,b.c,nx.o-sp,nx.o+sp,day_ms,new_day_ms,cb);
        }
        e.force_close((bars.back().ts/86400)*86400000LL,cb);
    } else {
        omega::MondayRiskOnEngine e;
        e.symbol=sym; e.engine_name="MondayRiskOn_"+sym; e.tag="MONRISK";
        e.sma_len=50; e.shadow_mode=true; e.enabled=true; e.verbose=false; e.lot=1.0;
        std::string eng=e.engine_name;
        e.on_trade_record=[&](const omega::TradeRecord& tr){ dump(tr,eng.c_str()); };
        for(const auto& b: bars){
            // bar ts can be stamped intraday (NDX daily = 13:30 UTC) -> normalize to
            // the UTC day so both ticks land on the SAME day (open 14:00, close 21:00).
            const int64_t day0=(b.ts/86400)*86400;
            const double sp=b.c*0.00008;
            e.on_tick(b.o-sp,b.o+sp,day0*1000+50400000);   // 14:00 UTC open tick
            e.on_tick(b.c-sp,b.c+sp,day0*1000+75600000);   // 21:00 UTC close tick
        }
    }
    fclose(fo);
    std::fprintf(stderr,"wrote %s\n",out.c_str());
    return 0;
}

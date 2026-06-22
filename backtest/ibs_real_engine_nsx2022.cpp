// Faithful test of the REAL IndexBearShortEngine on real NSXUSD (NASDAQ) 2022 ticks.
// Drives the ACTUAL engine class (on_tick -> internal H1 -> on_close_cb), NOT a port.
// NAS leg config mirrors engine_init.hpp: symbol NAS100, COST_PTS 2.0, lot 1.0,
// USE_RISKOFF_GATE false. HISTDATA NSXUSD is EST -> +5h to UTC. 2022 = the bear.
//
// BUILD: c++ -std=c++17 -O2 -Iinclude backtest/ibs_real_engine_nsx2022.cpp -o /tmp/ibs_real
// RUN:   /tmp/ibs_real /tmp/nsx22
#include "IndexBearShortEngine.hpp"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <dirent.h>
#include <algorithm>
#include <ctime>

static int64_t parse_ms(const char* d, const char* t){
    // d="YYYYMMDD", t="HHMMSSmmm"
    int Y=(d[0]-'0')*1000+(d[1]-'0')*100+(d[2]-'0')*10+(d[3]-'0');
    int M=(d[4]-'0')*10+(d[5]-'0'); int D=(d[6]-'0')*10+(d[7]-'0');
    int hh=(t[0]-'0')*10+(t[1]-'0'); int mm=(t[2]-'0')*10+(t[3]-'0');
    int ss=(t[4]-'0')*10+(t[5]-'0'); int ms=(t[6]-'0')*100+(t[7]-'0')*10+(t[8]-'0');
    struct tm tmv{}; tmv.tm_year=Y-1900; tmv.tm_mon=M-1; tmv.tm_mday=D;
    tmv.tm_hour=hh; tmv.tm_min=mm; tmv.tm_sec=ss;
    time_t e=timegm(&tmv);
    return (int64_t)e*1000 + ms + 5LL*3600LL*1000LL;   // EST -> UTC +5h
}

struct Cls { int64_t ts; double pnl; bool win; };

int main(int argc,char**argv){
    const char* dir = argc>1?argv[1]:"/tmp/nsx22";
    std::vector<std::string> files;
    DIR* dp=opendir(dir); dirent* de;
    while(dp && (de=readdir(dp))){ std::string n=de->d_name; if(n.size()>4 && n.substr(n.size()-4)==".csv") files.push_back(std::string(dir)+"/"+n); }
    if(dp) closedir(dp);
    std::sort(files.begin(),files.end());

    omega::IndexBearShortEngine eng;
    eng.symbol="NAS100"; eng.engine_name="IndexBearShort"; eng.shadow_mode=true;
    eng.enabled=true; eng.COST_PTS=2.0; eng.lot=1.0; eng.USE_RISKOFF_GATE=false;
    std::vector<Cls> closes;
    int64_t cur_ts=0;
    eng.on_close_cb=[&](const omega::TradeRecord& tr){ closes.push_back({cur_ts, tr.pnl, tr.pnl>0}); };

    char line[128]; size_t nticks=0;
    for(const auto& f: files){
        FILE* cf=fopen(f.c_str(),"r"); if(!cf) continue;
        while(fgets(line,sizeof line,cf)){
            // "YYYYMMDD HHMMSSmmm,bid,ask,vol"
            if(line[8]!=' ') continue;
            char dbuf[9]; memcpy(dbuf,line,8); dbuf[8]=0;
            const char* tcol=line+9;
            const char* comma=strchr(line,',');
            if(!comma) continue;
            double bid=atof(comma+1);
            const char* c2=strchr(comma+1,','); if(!c2) continue;
            double ask=atof(c2+1);
            if(bid<=0||ask<=0) continue;
            int64_t ts=parse_ms(dbuf,tcol);
            cur_ts=ts;
            eng.on_tick(bid,ask,ts);
            ++nticks;
        }
        fclose(cf);
    }

    auto pf=[](const std::vector<Cls>&v){
        double gp=0,gl=0; int w=0;
        for(auto&c:v){ if(c.pnl>0){gp+=c.pnl;w++;} else gl+=-c.pnl; }
        double net=gp-gl;
        printf("n=%zu WR=%.0f%% PF=%.2f net=%+.0fpt\n", v.size(),
               v.empty()?0:100.0*w/v.size(), gl>0?gp/gl:9.9, net);
    };
    printf("# REAL IndexBearShortEngine on NSXUSD 2022: %zu ticks -> %zu closes\n", nticks, closes.size());
    printf("2022 ALL : "); pf(closes);
    size_t h=closes.size()/2;
    std::vector<Cls> h1(closes.begin(),closes.begin()+h), h2(closes.begin()+h,closes.end());
    printf("     H1  : "); pf(h1);
    printf("     H2  : "); pf(h2);
    // monthly
    int last_mo=-1; std::vector<Cls> bucket;
    auto flush=[&](int mo){ if(!bucket.empty()){ printf("   2022-%02d : ",mo); pf(bucket); bucket.clear(); } };
    for(auto&c:closes){ time_t e=c.ts/1000; struct tm tmv{}; gmtime_r(&e,&tmv); int mo=tmv.tm_mon+1; if(mo!=last_mo && last_mo>=0) flush(last_mo); last_mo=mo; bucket.push_back(c);} flush(last_mo);
    return 0;
}

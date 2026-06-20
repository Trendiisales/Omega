// Validate CalendarTomEngine reproduces backtest/tom_backtest.py (TOM last3+first3).
// Drives the REAL engine per index daily CSV: feeds OPEN then CLOSE tick per day so
// on_d1_bar fills at the next day's open (open-to-open, faithful). Zero spread ->
// engine net = gross price return; compare to python (2bp): multi-day hold so cost
// is tiny -> engine PF ~ python PF (a touch higher). clang++ -std=c++17, runs on Mac.
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include "../include/CalendarTomEngine.hpp"

struct Bar { long ts; double o,h,l,c; };
static std::vector<Bar> load(const std::string& p){
    std::vector<Bar> v; std::ifstream f(p); std::string ln;
    while(std::getline(f,ln)){ double ts,o,h,l,c;
        if(std::sscanf(ln.c_str(),"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5) continue;
        if(c<=0) continue; v.push_back({(long)ts,o,h,l,c}); }
    std::sort(v.begin(),v.end(),[](const Bar&a,const Bar&b){return a.ts<b.ts;});
    return v;
}
int main(int argc,char**argv){
    const char* TICK="/Users/jo/Tick";
    const char* syms[]={"SPX","NDX","DJ30","GER40","UK100"};
    const char* files[]={"SPX_daily_2016_2026.csv","NDX_daily_2016_2026.csv",
        "DJ30_daily_2016_2026.csv","GER40_daily_2016_2026.csv","UK100_daily_2016_2026.csv"};
    std::vector<std::pair<long,double>> book;
    printf("# CalendarTomEngine validate (last3+first3, real engine, zero-spread, open-to-open)\n");
    for(int s=0;s<5;++s){
        auto bars=load(std::string(TICK)+"/"+files[s]);
        omega::CalendarTomEngine eng(syms[s]);
        eng.enabled=true; eng.shadow_mode=true; eng.lot=1.0; eng.p.target_vol_bps=0; eng.p.last_n=3; eng.p.first_n=3;
        std::vector<std::pair<long,double>> tr;
        auto cb=[&](const omega::TradeRecord& t){ tr.push_back({(long)t.entryTs,t.net_pnl}); };
        for(auto&b:bars){
            long day=((long)b.ts/86400L)*86400L;            // floor to UTC midnight (bar ts is 14:30 UTC)
            long ms=day*1000L;
            eng.on_tick(b.o,b.o, ms + 3600L*1000, cb);      // open tick -> triggers on_d1_bar at next day
            eng.on_tick(b.c,b.c, ms + 79200L*1000, cb);     // close tick (same UTC day: 22:00)
        }
        // stats
        auto st=[&](std::vector<std::pair<long,double>>& v){
            double gp=0,gl=0; int w=0; for(auto&x:v){ if(x.second>0){gp+=x.second;++w;} else gl+=-x.second; }
            double pf=gl>0?gp/gl:9.9; return std::make_pair(pf,(v.empty()?0.0:100.0*w/v.size())); };
        std::vector<std::pair<long,double>> bull,bear;
        for(auto&x:tr){ int y=1970; { long z=x.first/86400; int yy,mm,dd; // crude year
                z+=719468; long era=(z>=0?z:z-146096)/146097; long doe=z-era*146097;
                long yoe=(doe-doe/1460+doe/36524-doe/146096)/365; yy=(int)(yoe+era*400);
                long doy=doe-(365*yoe+yoe/4-yoe/100); long mp=(5*doy+2)/153;
                mm=(int)(mp<10?mp+3:mp-9); y=yy+(mm<=2); }
            if(y==2022) bear.push_back(x); else bull.push_back(x); book.push_back(x); }
        size_t h=tr.size()/2; std::vector<std::pair<long,double>> h1(tr.begin(),tr.begin()+h), h2(tr.begin()+h,tr.end());
        auto a=st(tr); auto sb=st(bull); auto sr=st(bear); auto s1=st(h1); auto s2=st(h2);
        bool pass = s1.first>1&&s2.first>1&&sb.first>1&&sr.first>1;
        printf("  %-6s n=%3zu WR%3.0f%% PF%.2f  H1pf%.2f H2pf%.2f BULLpf%.2f BEARpf%.2f %s\n",
               syms[s],tr.size(),a.second,a.first,s1.first,s2.first,sb.first,sr.first,pass?"PASS":"");
    }
    double gp=0,gl=0; int w=0; for(auto&x:book){ if(x.second>0){gp+=x.second;++w;} else gl+=-x.second; }
    printf("  BOOK   n=%zu WR%.0f%% PF%.2f  [python 2bp: book PF1.52]\n",
           book.size(), book.empty()?0:100.0*w/book.size(), gl>0?gp/gl:9.9);
    return 0;
}

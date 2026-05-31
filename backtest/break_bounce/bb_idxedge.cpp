// bb_idxedge.cpp -- where does the index return actually live? (edge hunt)
//
// Decomposes each symbol's return into OVERNIGHT (RTH close -> next RTH open)
// vs INTRADAY (RTH open -> RTH close), plus day-of-week. The classic equity
// result: indices make their gains overnight and are flat/negative intraday --
// if true here, "hold the index overnight, flat intraday" is a real edge that
// trend-following (= beta) misses. Samples mid at the RTH session boundaries.
//
// Build: g++ -O3 -std=c++17 bb_idxedge.cpp -o bbidx
// Run:   ./bbidx <ticks.csv> <rth_open_h> <rth_close_h>   (UTC hours)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

static bool parse(const char* s,int64_t& ts,double& bid,double& ask){
    if(s[0]>='0'&&s[0]<='9'){ char* e=nullptr; double f0=std::strtod(s,&e);
        if(e&&*e==','){ if(f0>=1e11){ char* e2=nullptr; bid=std::strtod(e+1,&e2);
            if(!e2||*e2!=',')return false; ask=std::strtod(e2+1,nullptr); ts=(int64_t)f0;
            if(ask<bid)std::swap(bid,ask); return bid>0&&ask>0; } } }
    return false;
}

struct Day { int64_t day; double open=0, close=0; int wday=0; bool ho=false, hc=false; };

int main(int argc,char** argv){
    if(argc<4){ std::printf("usage: bbidx <ticks.csv> <rth_open_h> <rth_close_h>\n"); return 1; }
    const int OH=std::atoi(argv[2]), CH=std::atoi(argv[3]);
    std::vector<Day> days; days.reserve(1000);
    int64_t cur_day=-1; Day* d=nullptr;
    std::ifstream in(argv[1]); if(!in.is_open()){std::printf("open fail\n");return 1;}
    std::string line; std::getline(in,line); // header
    long rows=0;
    while(std::getline(in,line)){ int64_t ts; double bid,ask;
        if(!parse(line.c_str(),ts,bid,ask)) continue; rows++;
        const double mid=(bid+ask)*0.5; const int64_t sec=ts/1000;
        const int64_t day=sec/86400; const int hh=(int)((sec%86400)/3600);
        if(day!=cur_day){ days.push_back(Day{day}); d=&days.back(); cur_day=day;
            std::time_t t=(std::time_t)sec; std::tm* tm=std::gmtime(&t); d->wday=tm?tm->tm_wday:0; }
        if(hh>=OH && !d->ho){ d->open=mid; d->ho=true; }
        if(hh>=CH){ d->close=mid; d->hc=true; }   // last tick at/after close
    }
    std::printf("rows=%ld days=%zu  (RTH %02d:00-%02d:00 UTC)\n", rows, days.size(), OH, CH);

    // overnight: close[D-1] -> open[D]; intraday: open[D] -> close[D]. percent.
    double on_sum=0,on_sq=0,in_sum=0,in_sq=0,cc_sum=0; int n=0;
    double wd_sum[7]={0}; int wd_n[7]={0};
    for(size_t i=1;i<days.size();i++){
        Day& a=days[i-1]; Day& b=days[i];
        if(!a.hc||!b.ho||!b.hc||a.close<=0||b.open<=0) continue;
        double onr=(b.open/a.close-1.0)*100.0;     // overnight
        double inr=(b.close/b.open-1.0)*100.0;      // intraday
        double ccr=(b.close/a.close-1.0)*100.0;     // total (close-to-close)
        on_sum+=onr; on_sq+=onr*onr; in_sum+=inr; in_sq+=inr*inr; cc_sum+=ccr; n++;
        wd_sum[b.wday]+=ccr; wd_n[b.wday]++;
    }
    if(n<10){ std::printf("too few days\n"); return 0; }
    auto sh=[&](double sum,double sq){ double m=sum/n,v=(sq-sum*sum/n)/(n-1); return v>0?(m/std::sqrt(v))*std::sqrt(252.0):0; };
    std::printf("\n  days=%d\n", n);
    std::printf("  OVERNIGHT (close->open):  cum=%+7.1f%%  avg=%+.4f%%  annSharpe=%+.2f\n", on_sum, on_sum/n, sh(on_sum,on_sq));
    std::printf("  INTRADAY  (open->close):  cum=%+7.1f%%  avg=%+.4f%%  annSharpe=%+.2f\n", in_sum, in_sum/n, sh(in_sum,in_sq));
    std::printf("  TOTAL     (close->close): cum=%+7.1f%%\n", cc_sum);
    const char* W[7]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    std::printf("  day-of-week avg close-close %%:");
    for(int w=1;w<=5;w++) std::printf("  %s=%+.3f", W[w], wd_n[w]?wd_sum[w]/wd_n[w]:0);
    std::printf("\n");
    return 0;
}

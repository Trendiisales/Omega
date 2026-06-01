// bb_idxscan.cpp -- hunt for MORE index edges + test vol-target sizing.
//
// On the daily cash-session return (open->close, the IndexSession edge), tests:
//   1. TURN-OF-MONTH  : last trading day + first 3 of month vs the rest.
//   2. POST-DOWN      : next session after a down session (capitulation bounce).
//   3. GAP            : does the overnight gap continue or fade intraday?
//   4. VOL-TARGET     : session-return / sessionATR%  (inverse-vol weighted) --
//      does equalizing risk per trade lift the Sharpe vs fixed size?
//
// Build: g++ -O3 -std=c++17 bb_idxscan.cpp -o bbscan
// Run:   ./bbscan <ticks.csv> <OH> <CH>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <vector>

static bool parse(const char* s,int64_t& ts,double& bid,double& ask){
    if(s[0]>='0'&&s[0]<='9'){ char* e=nullptr; double f0=std::strtod(s,&e);
        if(e&&*e==','){ if(f0>=1e11){ char* e2=nullptr; bid=std::strtod(e+1,&e2);
            if(!e2||*e2!=',')return false; ask=std::strtod(e2+1,nullptr); ts=(int64_t)f0;
            if(ask<bid)std::swap(bid,ask); return bid>0&&ask>0; } } }
    return false;
}
struct D { double open=0,close=0,pclose=0,hi=-1e18,lo=1e18; int mday=0,wday=0; bool ho=false,hc=false; };
static double sh(double sum,double sq,int n){ if(n<2)return 0; double m=sum/n,v=(sq-sum*sum/n)/(n-1); return v>0?(m/std::sqrt(v))*std::sqrt(252.0):0; }

int main(int argc,char** argv){
    if(argc<4){ std::printf("usage: bbscan <ticks.csv> <OH> <CH>\n"); return 1; }
    const int OH=std::atoi(argv[2]), CH=std::atoi(argv[3]);
    std::vector<D> v; v.reserve(2000); int64_t cd=-1; D* d=nullptr;
    std::ifstream in(argv[1]); if(!in.is_open()){std::printf("open fail\n");return 1;}
    std::string line; std::getline(in,line);
    while(std::getline(in,line)){ int64_t ts; double bid,ask; if(!parse(line.c_str(),ts,bid,ask))continue;
        const double mid=(bid+ask)*0.5; const int64_t sec=ts/1000; const int64_t day=sec/86400; const int hh=(int)((sec%86400)/3600);
        if(day!=cd){ v.push_back(D{}); d=&v.back(); cd=day; std::time_t t=(std::time_t)sec; std::tm* tm=std::gmtime(&t); if(tm){d->mday=tm->tm_mday;d->wday=tm->tm_wday;} }
        if(hh>=OH){ if(!d->ho){d->open=mid;d->ho=true;} if(hh<CH){d->hi=std::max(d->hi,mid);d->lo=std::min(d->lo,mid);} }
        if(hh>=CH){ d->close=mid; d->hc=true; }
    }
    // build session-return series + session ATR%
    struct R{ double sret,gap,atrp; int mday,wday; double pret; };
    std::vector<R> rs; double atr=0; int ac=0; double prev_sret=0;
    for(size_t i=1;i<v.size();i++){ D&a=v[i-1]; D&b=v[i];
        if(!a.hc||!b.ho||!b.hc||a.close<=0||b.open<=0||b.hi<-1e17) continue;
        double sret=(b.close/b.open-1)*100, gap=(b.open/a.close-1)*100;
        double rng=(b.hi-b.lo)/b.open*100; if(ac<14){atr+=rng;ac++; if(ac==14)atr/=14;} else atr=(atr*13+rng)/14;
        rs.push_back({sret,gap,(ac>=14?atr:0),b.mday,b.wday,prev_sret}); prev_sret=sret;
    }
    int N=rs.size(); std::printf("sessions=%d  (RTH %02d-%02d)\n", N, OH, CH);
    if(N<60) return 0;

    auto stat=[&](const char* lbl, auto pred){ double s=0,sq=0; int n=0,w=0;
        for(auto&r:rs) if(pred(r)){ s+=r.sret; sq+=r.sret*r.sret; n++; if(r.sret>0)w++; }
        if(n) std::printf("  %-28s n=%-4d avg=%+.4f%% WR=%.0f%% Sharpe=%+.2f\n", lbl,n,s/n,100.0*w/n,sh(s,sq,n)); };

    std::printf("[1] TURN-OF-MONTH (mday>=28 or <=3 vs rest):\n");
    stat("TOM days", [](const R&r){return r.mday>=28||r.mday<=3;});
    stat("non-TOM days", [](const R&r){return !(r.mday>=28||r.mday<=3);});

    std::printf("[2] POST-DOWN session (prev session < -0.5%%):\n");
    stat("after down day", [](const R&r){return r.pret<-0.5;});
    stat("after up day",   [](const R&r){return r.pret> 0.5;});

    std::printf("[3] GAP continuation vs fade (session return | gap sign):\n");
    stat("after gap UP   (>+0.2%)", [](const R&r){return r.gap>0.2;});
    stat("after gap DOWN (<-0.2%)", [](const R&r){return r.gap<-0.2;});

    std::printf("[4] VOL-TARGET sizing (session-long):\n");
    { double s=0,sq=0,sv=0,sqv=0; int n=0;
      for(auto&r:rs){ if(r.atrp<=0)continue; double raw=r.sret; double vt=r.sret/r.atrp; // inverse-vol unit
        s+=raw; sq+=raw*raw; sv+=vt; sqv+=vt*vt; n++; }
      std::printf("  fixed-size   Sharpe=%+.2f\n", sh(s,sq,n));
      std::printf("  vol-targeted Sharpe=%+.2f  (size ~ 1/sessionATR%%)\n", sh(sv,sqv,n));
    }
    return 0;
}

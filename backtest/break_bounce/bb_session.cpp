// bb_session.cpp -- validate the IndexSession edge: long up-regime sessions,
// short down-regime sessions, flat overnight, with an intraday ATR stop.
//
// Per UTC day: capture RTH open/close (real bid/ask) + session hi/lo. Direction
// from the HTF trend (close vs SMA(N)). Enter at RTH open, exit at RTH close OR
// intraday ATR stop. Reports overall + UP-regime vs DOWN-regime + IS/OOS, so we
// see profit in both directions and the protection's effect.
//
// Build: g++ -O3 -std=c++17 bb_session.cpp -o bbsession
// Run:   ./bbsession <ticks.csv> <rth_open_h> <rth_close_h> [mode] [stopATR] [skipFri]
//        mode: 0=long-only  1=two-sided(trend)   default 1

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
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
struct Day { int64_t day; double oB=0,oA=0,cB=0,cA=0,hi=-1e18,lo=1e18; int wday=0; bool ho=false; };
struct Met{ int n=0,w=0; double net=0,gw=0,gl=0,sum=0,sq=0;
    void add(double p){ n++; net+=p; sum+=p; sq+=p*p; if(p>0){w++;gw+=p;}else gl+=-p; }
    double pf()const{return gl>0?gw/gl:(gw>0?99:0);} double wr()const{return n?100.0*w/n:0;}
    double sh()const{ if(n<2)return 0; double m=sum/n,v=(sq-sum*sum/n)/(n-1); return v>0?(m/std::sqrt(v))*std::sqrt(252.0):0; } };

int main(int argc,char** argv){
    if(argc<4){ std::printf("usage: bbsession <ticks.csv> <OH> <CH> [mode][stopATR][skipFri]\n"); return 1; }
    const int OH=std::atoi(argv[2]), CH=std::atoi(argv[3]);
    const int MODE=argc>4?std::atoi(argv[4]):1;
    const double STOP=argc>5?std::atof(argv[5]):2.0;
    const int SKIPFRI=argc>6?std::atoi(argv[6]):1;
    const int SMA=50;

    std::vector<Day> D; D.reserve(2000); int64_t cd=-1; Day* d=nullptr;
    std::ifstream in(argv[1]); if(!in.is_open()){std::printf("open fail\n");return 1;}
    std::string line; std::getline(in,line); long rows=0;
    while(std::getline(in,line)){ int64_t ts; double bid,ask; if(!parse(line.c_str(),ts,bid,ask))continue; rows++;
        const double mid=(bid+ask)*0.5; const int64_t sec=ts/1000; const int64_t day=sec/86400; const int hh=(int)((sec%86400)/3600);
        if(day!=cd){ D.push_back(Day{day}); d=&D.back(); cd=day; std::time_t t=(std::time_t)sec; std::tm* tm=std::gmtime(&t); d->wday=tm?tm->tm_wday:0; }
        if(hh>=OH){ if(!d->ho){d->oB=bid;d->oA=ask;d->ho=true;} if(hh<CH){ d->hi=std::max(d->hi,mid); d->lo=std::min(d->lo,mid);} }
        if(hh>=CH){ d->cB=bid; d->cA=ask; }
    }
    // close-mid series + SMA + ATR(14 of session range)
    int N=D.size(); std::vector<double> cm(N,0),sma(N,0),atr(N,0);
    for(int i=0;i<N;i++) cm[i]=(D[i].cB+D[i].cA)*0.5;
    for(int i=0;i<N;i++){ double s=0;int c=0; for(int j=std::max(0,i-SMA+1);j<=i;j++){s+=cm[j];c++;} sma[i]=s/c; }
    { double a=0;int c=0; for(int i=0;i<N;i++){ double r=(D[i].hi>D[i].lo)?(D[i].hi-D[i].lo):0; if(c<14){a+=r;c++; if(c==14)a/=14;} else a=(a*13+r)/14; atr[i]=a; } }

    Met all,up,dn,is,oos; double eq=0,peak=0,mdd=0;
    int64_t firstday=D.empty()?0:D[0].day, lastday=D.empty()?0:D.back().day;
    int64_t split=firstday+(int64_t)((lastday-firstday)*0.6);
    for(int i=20;i<N;i++){ Day& x=D[i];
        if(!x.ho||x.cB<=0||x.oA<=0||x.hi<-1e17) continue;
        if(SKIPFRI && x.wday==5) continue;
        const bool uptrend = cm[i-1] > sma[i-1];
        int dir = (MODE==0)? +1 : (uptrend? +1 : -1);
        double entry = dir>0? x.oA : x.oB;          // pay spread in
        double stopd = STOP*atr[i-1];
        double exit;
        if(dir>0){ double stop=entry-stopd; if(x.lo<=stop) exit=stop; else exit=x.cB; }
        else     { double stop=entry+stopd; if(x.hi>=stop) exit=stop; else exit=x.cA; }
        double pnl = (dir>0?(exit-entry):(entry-exit)) / entry * 100.0;   // % return
        all.add(pnl); (uptrend?up:dn).add(pnl); (x.day<split?is:oos).add(pnl);
        eq+=pnl; if(eq>peak)peak=eq; if(peak-eq>mdd)mdd=peak-eq;
    }
    std::printf("rows=%ld days=%d  mode=%s stopATR=%.1f skipFri=%d\n", rows,N, MODE?"two-sided":"long-only",STOP,SKIPFRI);
    auto pr=[&](const char* l,Met m){ std::printf("  %-16s n=%-4d WR=%.1f%% PF=%.2f Sharpe=%+.2f cumRet=%+.1f%%\n",l,m.n,m.wr(),m.pf(),m.sh(),m.net); };
    pr("ALL",all); pr("UP-regime",up); pr("DOWN-regime",dn); pr("IS(60%)",is); pr("OOS(40%)",oos);
    std::printf("  maxDD(equity)=%.1f%%  cum=%.1f%%\n", mdd, all.net);
    return 0;
}

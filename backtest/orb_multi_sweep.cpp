// =============================================================================
// orb_multi_sweep.cpp -- Opening Range Breakout, multi-symbol, m5-native, 2yr.
// Each UTC day: build the opening range over [OR0,OR1) (minutes of day), place an
// OCO Buy-Stop=ORhigh+buf / Sell-Stop=ORlow-buf. One fills -> cancel the other
// (one shot/day). Hard SL at the opposite OR edge (range) or ATR; fixed TP=TPr*risk.
// FLAT by TEND (no overnight). Output in PRICE units (per-symbol cost passed in).
//
//   g++ -std=c++17 -O3 -o backtest/orb_multi_sweep backtest/orb_multi_sweep.cpp
//   ./backtest/orb_multi_sweep <m5.csv ts,o,h,l,c> <cost_price_units> [oos_frac]
// env: OR0 OR1 TEND (minutes-of-day UTC), BUF (xATR), TP (R; 0=exit at TEND),
//      SLMODE (range|atr), STOPATR, SIDE (both|long|short)
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <array>
#include <deque>
#include <fstream>
#include <algorithm>

int main(int argc,char**argv){
    if(argc<3){std::fprintf(stderr,"usage: %s <m5.csv> <cost> [oos]\n",argv[0]);return 1;}
    const char* path=argv[1]; double COST=atof(argv[2]); double oos=(argc>3)?atof(argv[3]):0.0;
    int OR0=getenv("OR0")?atoi(getenv("OR0")):360;
    int OR1=getenv("OR1")?atoi(getenv("OR1")):420;
    int TEND=getenv("TEND")?atoi(getenv("TEND")):960;
    double BUF=getenv("BUF")?atof(getenv("BUF")):0.05;
    double TPr=getenv("TP")?atof(getenv("TP")):1.0;
    std::string SLMODE=getenv("SLMODE")?getenv("SLMODE"):"range";
    double STOPATR=getenv("STOPATR")?atof(getenv("STOPATR")):3.0;
    std::string SIDE=getenv("SIDE")?getenv("SIDE"):"both";

    std::vector<std::array<double,5>> m5;
    { std::ifstream f(path); if(!f){std::fprintf(stderr,"open fail %s\n",path);return 1;}
      std::string ln; std::getline(f,ln);
      while(std::getline(f,ln)){ if(ln.empty())continue; const char* s=ln.c_str(); char* e=nullptr;
        double ts=strtod(s,&e); if(*e!=',')continue; double o=strtod(e+1,&e);if(*e!=',')continue;
        double h=strtod(e+1,&e);if(*e!=',')continue; double l=strtod(e+1,&e);if(*e!=',')continue; double c=strtod(e+1,&e);
        if(ts>1e11) ts/=1000.0;   // accept ms
        if(o>0&&h>0&&l>0&&c>0) m5.push_back({ts,o,h,l,c}); } }
    if((int)m5.size()<2000){std::fprintf(stderr,"few m5 (%zu)\n",m5.size());return 1;}
    int evalStart=(oos>0&&oos<1)?(int)(m5.size()*(1.0-oos)):0;

    const int ATRP=14; double atr=0; std::deque<double> trq; double pc=0;
    int64_t curday=-1; double orh=0,orl=1e18; bool orDone=false,traded=false;
    bool pos=false; int dir=0; double entry=0,sl=0,tp=0;
    double cum=0,peak=0,mdd=0; int nw=0,nl=0; double gw=0,gl=0; int ntr=0;
    int longN=0,shortN=0; double longNet=0,shortNet=0; std::vector<double> tp_;

    auto close=[&](double px){ double pnl=dir*(px-entry)-COST; cum+=pnl; if(cum>peak)peak=cum;
        double dd=peak-cum; if(dd>mdd)mdd=dd; if(pnl>0){nw++;gw+=pnl;}else if(pnl<0){nl++;gl+=-pnl;}
        if(dir>0){longN++;longNet+=pnl;}else{shortN++;shortNet+=pnl;} tp_.push_back(pnl); ntr++; pos=false; };

    for(size_t i=0;i<m5.size();++i){
        double ts=m5[i][0],h=m5[i][2],l=m5[i][3],c=m5[i][4];
        if(pc>0){ double tr=std::max({h-l,std::fabs(h-pc),std::fabs(l-pc)}); trq.push_back(tr); if((int)trq.size()>ATRP)trq.pop_front(); double s=0;for(double v:trq)s+=v; atr=s/trq.size(); }
        pc=c;
        int64_t day=(int64_t)(ts/86400.0); int mod=(int)(((int64_t)ts%86400)/60);
        if(day!=curday){ if(pos)close(c); curday=day; orh=0; orl=1e18; orDone=false; traded=false; }
        if(mod>=OR0 && mod<OR1){ if(h>orh)orh=h; if(l<orl)orl=l; }
        if(mod>=OR1 && !orDone && orh>0 && orl<1e18) orDone=true;
        if(pos){
            if(dir>0){ if(l<=sl)close(sl); else if(tp>0&&h>=tp)close(tp); }
            else     { if(h>=sl)close(sl); else if(tp>0&&l<=tp)close(tp); }
            if(pos && mod>=TEND) close(c);
        }
        if((int)i<evalStart) continue;
        if(!pos && orDone && !traded && mod>=OR1 && mod<TEND && atr>0){
            double bs=orh+BUF*atr, ss=orl-BUF*atr;
            bool armL=(SIDE!="short"), armS=(SIDE!="long");
            int side=0; double fill=0;
            if(armL && h>=bs){side=1;fill=bs;} else if(armS && l<=ss){side=-1;fill=ss;}
            if(side!=0){
                double risk=(SLMODE=="atr")?STOPATR*atr:(side>0?fill-orl:orh-fill);
                if(risk<=0) risk=STOPATR*atr;
                entry=fill; dir=side; sl=fill-side*risk; tp=TPr>0?fill+side*TPr*risk:0; pos=true; traded=true;
            }
        }
    }
    if(pos) close(m5.back()[4]);

    double pf=(gl>0)?gw/gl:(gw>0?999:0), hit=(nw+nl>0)?100.0*nw/(nw+nl):0;
    double years; {int n=(int)m5.size();int s=std::max(evalStart,1); years=(m5[n-1][0]-m5[s][0])/86400.0/365.25;}
    double sh=0; if(ntr>=2&&years>0){double m=0;for(double v:tp_)m+=v;m/=ntr;double s=0;for(double v:tp_)s+=(v-m)*(v-m);double sd=std::sqrt(s/(ntr-1));if(sd>0)sh=(m/sd)*std::sqrt((double)ntr/years);}
    std::printf("OR%d-%d end%d BUF%.2f TP%.1f %-5s | tr=%-4d net=%-11.4f PF=%.2f Sh=%+.2f win=%.0f%% mdd=%-9.4f | L:%d/%.3f S:%d/%.3f\n",
        OR0,OR1,TEND,BUF,TPr,SLMODE.c_str(),ntr,cum,pf,sh,hit,mdd,longN,longNet,shortN,shortNet);
    return 0;
}

// heikin_scalp.cpp -- Peachy's scalp on HEIKIN-ASHI (her explicit "game-changer").
// HA smooths noise; she reads band(4/9 EMA on HA) color + reaction. Continuation entry:
//   trend up = ema4(HA) > ema9(HA). Pullback = HA touches the 9EMA band. Reaction =
//   the next HA candle flips back to trend color (green in uptrend) = continuation confirmed.
//   Enter -> stop below the pullback real low -> RUNNER trail (prior TRWIN real-bar low).
// Two-sided. 10:00-16:00 ET. Optional MAXTR/day, RSI exhaustion gate, 2x-loss lockout.
// run: ./heikin_scalp <barfile ts,o,h,l,c> [HA=1] [TREND=cross|price] [MAXTR=..] [COST=..]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
using namespace std;
struct Bar{int64_t ts;double o,h,l,c;};
static int et_off(int64_t ts){time_t t=ts;struct tm g;gmtime_r(&t,&g);int y=g.tm_year+1900,mo=g.tm_mon+1,d=g.tm_mday;
    auto dow=[](int Y,int M,int D){struct tm v{};v.tm_year=Y-1900;v.tm_mon=M-1;v.tm_mday=D;v.tm_hour=12;time_t tt=timegm(&v);struct tm o;gmtime_r(&tt,&o);return o.tm_wday;};
    int mar=14;for(int dd=8;dd<=14;dd++)if(dow(y,3,dd)==0){mar=dd;break;}int nov=7;for(int dd=1;dd<=7;dd++)if(dow(y,11,dd)==0){nov=dd;break;}
    bool dst;if(mo<3||mo>11)dst=false;else if(mo>3&&mo<11)dst=true;else if(mo==3)dst=(d>=mar);else dst=(d<nov);return dst?-4:-5;}
static int et_hm(int64_t ts){int64_t lt=ts+et_off(ts)*3600;int s=((lt%86400)+86400)%86400;return (s/3600)*100+(s%3600)/60;}
static int64_t et_day(int64_t ts){int64_t lt=ts+et_off(ts)*3600;return (lt-((lt%86400)+86400)%86400)/86400;}
static int yearOf(int64_t ts){time_t t=ts;struct tm g;gmtime_r(&t,&g);return g.tm_year+1900;}

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage:%s <barfile>\n",argv[0]);return 1;}
    int HA=getenv("HA")?atoi(getenv("HA")):1;
    string TR=getenv("TREND")?getenv("TREND"):"cross";
    int MAXTR=getenv("MAXTR")?atoi(getenv("MAXTR")):99;
    double COST=getenv("COST")?atof(getenv("COST")):0.37;
    int TRWIN=getenv("TRWIN")?atoi(getenv("TRWIN")):3;
    int RSIG=getenv("RSIG")?atoi(getenv("RSIG")):0;
    double bufA=0.10;int maxHold=120;int MAXLOSS=getenv("MAXLOSS")?atoi(getenv("MAXLOSS")):99; double TRENDDAY=getenv("TRENDDAY")?atof(getenv("TRENDDAY")):0.0;

    vector<Bar> B;{ifstream f(argv[1]);string ln;bool fst=true;
        while(getline(f,ln)){if(ln.empty())continue;if(fst){fst=false;if(ln[0]<'0'||ln[0]>'9')continue;}
            const char*s=ln.c_str();char*e=nullptr;long long ts=strtoll(s,&e,10);if(*e!=',')continue;
            if(ts>100000000000LL)ts/=1000;Bar b;b.ts=ts;b.o=strtod(e+1,&e);if(*e!=',')continue;b.h=strtod(e+1,&e);if(*e!=',')continue;
            b.l=strtod(e+1,&e);if(*e!=',')continue;b.c=strtod(e+1,&e);if(b.h>0)B.push_back(b);}}
    int n=B.size();if(n<500){fprintf(stderr,"few\n");return 1;}
    sort(B.begin(),B.end(),[](const Bar&a,const Bar&b){return a.ts<b.ts;});
    // Heikin-Ashi series
    vector<double> ho(n),hh(n),hl(n),hc(n);
    for(int i=0;i<n;i++){hc[i]=(B[i].o+B[i].h+B[i].l+B[i].c)/4.0;
        ho[i]= i==0? (B[i].o+B[i].c)/2.0 : (ho[i-1]+hc[i-1])/2.0;
        hh[i]=max(B[i].h,max(ho[i],hc[i])); hl[i]=min(B[i].l,min(ho[i],hc[i]));}
    // if HA=0, use raw candles for signal
    auto SO=[&](int i){return HA?ho[i]:B[i].o;}; auto SH=[&](int i){return HA?hh[i]:B[i].h;};
    auto SL=[&](int i){return HA?hl[i]:B[i].l;}; auto SC=[&](int i){return HA?hc[i]:B[i].c;};
    // EMA4/9 on signal-close + ATR + RSI
    vector<double> e4(n),e9(n),atr(n,0),rsi(n,50);
    {double k4=2.0/5,k9=2.0/10;e4[0]=SC(0);e9[0]=SC(0);for(int i=1;i<n;i++){e4[i]=e4[i-1]+k4*(SC(i)-e4[i-1]);e9[i]=e9[i-1]+k9*(SC(i)-e9[i-1]);}
     double a=0;int W=14;for(int i=1;i<n;i++){double tr=max(B[i].h-B[i].l,max(fabs(B[i].h-B[i-1].c),fabs(B[i].l-B[i-1].c)));a=(i<=W)?(a*(i-1)+tr)/i:a+(tr-a)/W;atr[i]=a;}
     double ag=0,al=0;int RW=14;for(int i=1;i<n;i++){double ch=B[i].c-B[i-1].c,g=ch>0?ch:0,l=ch<0?-ch:0;if(i<=RW){ag+=g;al+=l;if(i==RW){ag/=RW;al/=RW;rsi[i]=al<1e-9?100:100-100/(1+ag/al);}}else{ag=(ag*(RW-1)+g)/RW;al=(al*(RW-1)+l)/RW;rsi[i]=al<1e-9?100:100-100/(1+ag/al);}}}

    struct Tr{int64_t ts;double R;int yr;};vector<Tr> trades;long entries=0;
    int64_t curday=-1;int trToday=0,lossToday=0; double dayOpen=0; deque<double> lows,highs;
    bool inpos=false;int side=0;double ep=0,sl=0,trail=0,risk=0;int eidx=0;
    for(int i=2;i<n;i++){
        int hm=et_hm(B[i].ts);int64_t day=et_day(B[i].ts);
        lows.push_back(B[i].l);highs.push_back(B[i].h);while((int)lows.size()>TRWIN)lows.pop_front();while((int)highs.size()>TRWIN)highs.pop_front();
        if(day!=curday){curday=day;trToday=0;lossToday=0;dayOpen=B[i].o;}
        if(inpos){
            if(side>0){double s=1e18;for(double v:lows)s=min(s,v);s-=bufA*atr[i];if(s>trail)trail=s;if(B[i].l<=trail){double R=(trail-ep)/risk-COST/risk;if(R<0)lossToday++;trades.push_back({B[i].ts,R,yearOf(B[i].ts)});inpos=false;}}
            else{double s=-1e18;for(double v:highs)s=max(s,v);s+=bufA*atr[i];if(s<trail)trail=s;if(B[i].h>=trail){double R=(ep-trail)/risk-COST/risk;if(R<0)lossToday++;trades.push_back({B[i].ts,R,yearOf(B[i].ts)});inpos=false;}}
            if(inpos&&(hm>=1600||i-eidx>maxHold)){double xp=B[i].c;double R=(side>0?(xp-ep):(ep-xp))/risk-COST/risk;if(R<0)lossToday++;trades.push_back({B[i].ts,R,yearOf(B[i].ts)});inpos=false;}
            continue;
        }
        if(trToday>=MAXTR||lossToday>=MAXLOSS)continue;
        if(hm<1000||hm>=1545)continue; if(atr[i]<=0)continue;
        // trend
        int bias = (TR=="price")? (SC(i)>e9[i]?1:(SC(i)<e9[i]?-1:0)) : (e4[i]>e9[i]?1:(e4[i]<e9[i]?-1:0));
        if(!bias)continue;
        if(RSIG){if(bias>0&&rsi[i]>75)continue;if(bias<0&&rsi[i]<25)continue;}
        if(TRENDDAY>0&&dayOpen>0){double ext=bias>0?(B[i].c-dayOpen):(dayOpen-B[i].c); if(ext<TRENDDAY*atr[i])continue;}
        // pullback to band on PRIOR bar (HA touched 9EMA against trend), reaction THIS bar (HA flips back to trend color)
        bool pulled = bias>0 ? (SL(i-1)<=e9[i-1]) : (SH(i-1)>=e9[i-1]);
        bool reactGreen = bias>0 ? (SC(i)>SO(i)) : (SC(i)<SO(i));     // HA candle back in trend color
        bool prevCounter= bias>0 ? (SC(i-1)<SO(i-1)) : (SC(i-1)>SO(i-1)); // the pullback candle was counter-color
        if(!(pulled && reactGreen && prevCounter))continue;
        double ent=B[i].c; double stp= bias>0?(min(B[i].l,B[i-1].l)-bufA*atr[i]):(max(B[i].h,B[i-1].h)+bufA*atr[i]);
        double rk= bias>0?(ent-stp):(stp-ent); if(rk<=0.05*atr[i])continue;
        inpos=true;side=bias;ep=ent;sl=stp;trail=stp;risk=rk;eidx=i;entries++;trToday++;
    }
    int N=trades.size();if(!N){printf("NO TRADES\n");return 0;}
    auto stat=[&](vector<Tr>&t,const char*tag){int m=t.size();if(!m){printf("  %-6s n=0\n",tag);return;}int w=0;double gw=0,gl=0,sum=0;for(auto&x:t){if(x.R>0){w++;gw+=x.R;}else gl+=-x.R;sum+=x.R;}printf("  %-6s n=%d WR=%.1f%% PF=%.2f avgR=%+.3f totR=%+.1f\n",tag,m,100.0*w/m,gl>0?gw/gl:99,sum/m,sum);};
    printf("=== HEIKIN SCALP HA=%d TREND=%s MAXTR=%d COST=%.3f ===\n",HA,TR.c_str(),MAXTR,COST);
    printf("entries=%ld\n",entries);stat(trades,"ALL");
    vector<Tr> h1(trades.begin(),trades.begin()+N/2),h2(trades.begin()+N/2,trades.end());stat(h1,"H1");stat(h2,"H2");
    vector<int> ys;for(auto&t:trades)if(find(ys.begin(),ys.end(),t.yr)==ys.end())ys.push_back(t.yr);sort(ys.begin(),ys.end());
    for(int y:ys){vector<Tr> yt;for(auto&t:trades)if(t.yr==y)yt.push_back(t);char tg[8];snprintf(tg,8,"%d",y);stat(yt,tg);}
    return 0;
}

// level_reaction_scalp.cpp -- Peachy's ACTUAL discretionary method (from her gold/oil
// live-trade walkthrough): mark KEY LEVELS (KPLs), WAIT for a REACTION (rejection candle)
// at the level, enter WITH the reaction, then aggressively TRAIL (runner). Two-sided.
// Levels (mechanizable KPLs): prior-day high/low, overnight session high/low, round numbers.
// Reaction = a bar that pierces the level then CLOSES back (rejection wick) within tol*ATR.
//   support reject -> LONG (stop below wick); resistance reject -> SHORT (stop above wick).
// Exit = structural RUNNER trail (prior TRWIN-bar low/high). 2x-loss/day lockout. EOD flat.
// run: ./level_reaction_scalp <m5> [TOL=0.5] [ROUND=25] [TRWIN=3] [COST=..]
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
struct Bar{ int64_t ts; double o,h,l,c; };
static int et_off(int64_t ts){ time_t t=ts; struct tm g; gmtime_r(&t,&g); int y=g.tm_year+1900,mo=g.tm_mon+1,d=g.tm_mday;
    auto dow=[](int Y,int M,int D){ struct tm v{}; v.tm_year=Y-1900;v.tm_mon=M-1;v.tm_mday=D;v.tm_hour=12; time_t tt=timegm(&v); struct tm o; gmtime_r(&tt,&o); return o.tm_wday;};
    int mar=14;for(int dd=8;dd<=14;dd++)if(dow(y,3,dd)==0){mar=dd;break;} int nov=7;for(int dd=1;dd<=7;dd++)if(dow(y,11,dd)==0){nov=dd;break;}
    bool dst;if(mo<3||mo>11)dst=false;else if(mo>3&&mo<11)dst=true;else if(mo==3)dst=(d>=mar);else dst=(d<nov); return dst?-4:-5;}
static int et_hm(int64_t ts){int64_t lt=ts+et_off(ts)*3600;int s=((lt%86400)+86400)%86400;return (s/3600)*100+(s%3600)/60;}
static int64_t et_day(int64_t ts){int64_t lt=ts+et_off(ts)*3600;return (lt-((lt%86400)+86400)%86400)/86400;}
static int yearOf(int64_t ts){time_t t=ts;struct tm g;gmtime_r(&t,&g);return g.tm_year+1900;}

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: %s <m5>\n",argv[0]);return 1;}
    double TOL=getenv("TOL")?atof(getenv("TOL")):0.5;      // level proximity = TOL*ATR
    double ROUND=getenv("ROUND")?atof(getenv("ROUND")):25; // round-number grid (0=off)
    int    TRWIN=getenv("TRWIN")?atoi(getenv("TRWIN")):3;
    double COST=getenv("COST")?atof(getenv("COST")):0.37;
    double bufA=0.10; int maxHold=78; int MAXLOSS=2; double WICK=getenv("WICK")?atof(getenv("WICK")):0.25; int MAXTR=getenv("MAXTR")?atoi(getenv("MAXTR")):99;        // her 2x-loss/day lockout
    int    TREND=getenv("TREND")?atoi(getenv("TREND")):0;   // 1=only with EMA50 trend dir

    vector<Bar> B;{ifstream f(argv[1]);string ln;bool fst=true;
        while(getline(f,ln)){if(ln.empty())continue;if(fst){fst=false;if(ln[0]<'0'||ln[0]>'9')continue;}
            const char*s=ln.c_str();char*e=nullptr;long long ts=strtoll(s,&e,10);if(*e!=',')continue;
            Bar b;b.ts=ts;b.o=strtod(e+1,&e);if(*e!=',')continue;b.h=strtod(e+1,&e);if(*e!=',')continue;
            b.l=strtod(e+1,&e);if(*e!=',')continue;b.c=strtod(e+1,&e);if(b.h>0)B.push_back(b);}}
    int n=B.size();if(n<500){fprintf(stderr,"few bars\n");return 1;}
    sort(B.begin(),B.end(),[](const Bar&a,const Bar&b){return a.ts<b.ts;});
    vector<double> atr(n,0),ema(n,0);{double a=0;int W=14;double k=2.0/51;ema[0]=B[0].c;
        for(int i=1;i<n;i++){double tr=max(B[i].h-B[i].l,max(fabs(B[i].h-B[i-1].c),fabs(B[i].l-B[i-1].c)));a=(i<=W)?(a*(i-1)+tr)/i:a+(tr-a)/W;atr[i]=a;ema[i]=ema[i-1]+k*(B[i].c-ema[i-1]);}}

    struct Tr{int64_t ts;double R;int yr;};vector<Tr> trades;long entries=0;
    // per-day levels
    int64_t curday=-1,prevday=-1; double pdh=0,pdl=0,onh=0,onl=0, dh=-1e18,dl=1e18, prevdh=-1e18,prevdl=1e18;
    int lossesToday=0, trToday=0; deque<double> lows,highs;
    bool inpos=false;int side=0;double ep=0,sl=0,trail=0,risk=0;int entryIdx=0;

    for(int i=1;i<n;i++){
        int hm=et_hm(B[i].ts); int64_t day=et_day(B[i].ts);
        lows.push_back(B[i].l);highs.push_back(B[i].h);while((int)lows.size()>TRWIN)lows.pop_front();while((int)highs.size()>TRWIN)highs.pop_front();
        if(day!=curday){ // roll day: prior-day H/L = last day's; overnight = pre-0930 range so far reset
            prevdh=dh;prevdl=dl; pdh=prevdh;pdl=prevdl; dh=-1e18;dl=1e18; onh=-1e18;onl=1e18; curday=day; lossesToday=0; trToday=0; }
        dh=max(dh,B[i].h);dl=min(dl,B[i].l);
        if(hm<930){ onh=max(onh,B[i].h); onl=min(onl,B[i].l); }   // overnight range before NY open

        // ---- manage open (runner) ----
        if(inpos){
            if(side>0){double s=1e18;for(double v:lows)s=min(s,v);s-=bufA*atr[i];if(s>trail)trail=s;
                if(B[i].l<=trail){double R=(trail-ep)/risk;R-=COST/risk;if(R<0)lossesToday++;trades.push_back({B[i].ts,R,yearOf(B[i].ts)});inpos=false;}}
            else{double s=-1e18;for(double v:highs)s=max(s,v);s+=bufA*atr[i];if(s<trail)trail=s;
                if(B[i].h>=trail){double R=(ep-trail)/risk;R-=COST/risk;if(R<0)lossesToday++;trades.push_back({B[i].ts,R,yearOf(B[i].ts)});inpos=false;}}
            if(inpos && (hm>=1600 || i-entryIdx>maxHold)){double xp=B[i].c;double R=(side>0?(xp-ep):(ep-xp))/risk;R-=COST/risk;if(R<0)lossesToday++;trades.push_back({B[i].ts,R,yearOf(B[i].ts)});inpos=false;}
            continue;
        }
        if(lossesToday>=MAXLOSS||trToday>=MAXTR) continue;
        if(hm<1000||hm>=1545) continue;
        if(atr[i]<=0) continue;
        // build candidate level set
        double levels[8]; int nl=0;
        if(pdh>0)levels[nl++]=pdh; if(pdl<1e17)levels[nl++]=pdl;
        if(onh>0)levels[nl++]=onh; if(onl<1e17)levels[nl++]=onl;
        if(ROUND>0){ double rl=floor(B[i].c/ROUND)*ROUND; levels[nl++]=rl; levels[nl++]=rl+ROUND; }
        const double tol=TOL*atr[i];
        // resistance rejection -> SHORT: high pierces a level, close back below it, upper wick
        // support rejection -> LONG: low pierces a level, close back above it, lower wick
        for(int li=0; li<nl; li++){ double L=levels[li];
            bool resist = (B[i].h>=L-tol && B[i].h<=L+tol*2 && B[i].c<L && (B[i].h-max(B[i].o,B[i].c))>WICK*(B[i].h-B[i].l));
            bool support= (B[i].l<=L+tol && B[i].l>=L-tol*2 && B[i].c>L && (min(B[i].o,B[i].c)-B[i].l)>WICK*(B[i].h-B[i].l));
            int s2=0; double stp=0;
            if(support){ s2=+1; stp=B[i].l-bufA*atr[i]; }
            else if(resist){ s2=-1; stp=B[i].h+bufA*atr[i]; }
            if(s2==0) continue;
            if(TREND){ if(s2>0 && !(B[i].c>ema[i])) continue; if(s2<0 && !(B[i].c<ema[i])) continue; }
            double ent=B[i].c; double rk= s2>0?(ent-stp):(stp-ent); if(rk<=0.05*atr[i]) continue;
            inpos=true;side=s2;ep=ent;sl=stp;trail=stp;risk=rk;entryIdx=i;entries++;trToday++; break;
        }
    }
    int N=trades.size(); if(!N){printf("NO TRADES\n");return 0;}
    auto stat=[&](vector<Tr>&t,const char*tag){int m=t.size();if(!m){printf("  %-6s n=0\n",tag);return;}
        int w=0;double gw=0,gl=0,sum=0;for(auto&x:t){if(x.R>0){w++;gw+=x.R;}else gl+=-x.R;sum+=x.R;}
        printf("  %-6s n=%d WR=%.1f%% PF=%.2f avgR=%+.3f totR=%+.1f\n",tag,m,100.0*w/m,gl>0?gw/gl:99,sum/m,sum);};
    printf("=== LEVEL-REACTION SCALP  TOL=%.1f ROUND=%.0f TRWIN=%d TREND=%d COST=%.3f ===\n",TOL,ROUND,TRWIN,TREND,COST);
    printf("entries=%ld\n",entries); stat(trades,"ALL");
    vector<Tr> h1(trades.begin(),trades.begin()+N/2),h2(trades.begin()+N/2,trades.end());stat(h1,"H1");stat(h2,"H2");
    vector<int> ys;for(auto&t:trades)if(find(ys.begin(),ys.end(),t.yr)==ys.end())ys.push_back(t.yr);sort(ys.begin(),ys.end());
    for(int y:ys){vector<Tr> yt;for(auto&t:trades)if(t.yr==y)yt.push_back(t);char tg[8];snprintf(tg,8,"%d",y);stat(yt,tg);}
    return 0;
}

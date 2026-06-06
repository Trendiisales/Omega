// ema_vwap_trend_gold.cpp — Peachy "follow the trend" 9EMA / VWAP / 4-9 cross, gold 5m.
// Spec: only after 10:00 ET ("true trend"). Bias rules (env MODE):
//   MODE=ema9 : price>9EMA = bull, <9EMA = bear. Enter on pullback-to-9EMA in bias dir.
//   MODE=cross: 4EMA vs 9EMA cross sets bias. Enter on pullback to 9EMA.
//   VWAP regime gate (env VWAP=1): long only if 9EMA>sessionVWAP (price regime bull); sym short.
//   Entry: pullback touch of 9EMA in bias direction (her "retrace to the nine").
//   Stop: below/above the pullback reaction bar - buf. Target: Rmult*risk (env R) OR next swing.
//   Session VWAP resets each ET day. EOD flat 16:00 ET. One-side-per-bias, MAXTR/day.
// Cost-incl, WF both-halves, per-year. env MODE/VWAP/R/COST/MAXTRADES.
// build: g++ -std=c++17 -O2 ema_vwap_trend_gold.cpp -o ema_vwap_trend
// run:   ./ema_vwap_trend <m5file>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
using namespace std;
struct Bar{ int64_t ts; double o,h,l,c; };

static int et_off(int64_t ts){
    time_t t=ts; struct tm g; gmtime_r(&t,&g); int y=g.tm_year+1900,mo=g.tm_mon+1,d=g.tm_mday;
    auto dow=[](int Y,int M,int D){ struct tm tmv{}; tmv.tm_year=Y-1900; tmv.tm_mon=M-1; tmv.tm_mday=D; tmv.tm_hour=12; time_t tt=timegm(&tmv); struct tm o; gmtime_r(&tt,&o); return o.tm_wday; };
    int marSun=14; for(int dd=8;dd<=14;dd++) if(dow(y,3,dd)==0){marSun=dd;break;}
    int novSun=7;  for(int dd=1;dd<=7;dd++)  if(dow(y,11,dd)==0){novSun=dd;break;}
    bool dst; if(mo<3||mo>11) dst=false; else if(mo>3&&mo<11) dst=true; else if(mo==3) dst=(d>=marSun); else dst=(d<novSun);
    return dst?-4:-5;
}
static int et_hm(int64_t ts){ int off=et_off(ts); int64_t lt=ts+off*3600; int s=(int)(((lt%86400)+86400)%86400); return (s/3600)*100+(s%3600)/60; }
static int64_t et_day(int64_t ts){ int off=et_off(ts); int64_t lt=ts+off*3600; return (lt-((lt%86400)+86400)%86400)/86400; }
static int yearOf(int64_t ts){ time_t t=ts; struct tm g; gmtime_r(&t,&g); return g.tm_year+1900; }

int main(int argc,char**argv){
    if(argc<2){ fprintf(stderr,"usage: %s <m5file>\n",argv[0]); return 1; }
    string MODE = getenv("MODE")? getenv("MODE"):"ema9";
    int    useVWAP = getenv("VWAP")? atoi(getenv("VWAP")):1;
    double Rmult = getenv("R")? atof(getenv("R")):2.0;
    double COST = getenv("COST")? atof(getenv("COST")):0.5;
    int    MAXTR = getenv("MAXTRADES")? atoi(getenv("MAXTRADES")):3;
    int    e1=4, e2=9;                       // 4 & 9 EMA (her values, in bars on 5m)
    double bufA=0.10; int maxHold=78;
    int    RSIGATE = getenv("RSIGATE")? atoi(getenv("RSIGATE")):0;   // skip continuation into exhaustion
    double TRSTR   = getenv("TRSTR")? atof(getenv("TRSTR")):0.0;     // require |ema9-ema50|/atr >= TRSTR (strong-trend day)

    vector<Bar> B; { ifstream f(argv[1]); string ln; bool first=true;
        while(getline(f,ln)){ if(ln.empty())continue; if(first){first=false; if(ln[0]<'0'||ln[0]>'9')continue;}
            const char* s=ln.c_str(); char* e=nullptr; long long ts=strtoll(s,&e,10); if(*e!=',')continue;
            Bar b; b.ts=ts; b.o=strtod(e+1,&e); if(*e!=',')continue; b.h=strtod(e+1,&e); if(*e!=',')continue;
            b.l=strtod(e+1,&e); if(*e!=',')continue; b.c=strtod(e+1,&e); if(b.h>0) B.push_back(b);} }
    int n=B.size(); if(n<500){ fprintf(stderr,"too few bars %d\n",n); return 1; }
    sort(B.begin(),B.end(),[](const Bar&a,const Bar&b){return a.ts<b.ts;});

    vector<double> ema4(n,0),ema9(n,0),ema50(n,0),atr(n,0),vwap(n,0),rsi(n,50);
    { double k1=2.0/(e1+1),k2=2.0/(e2+1),k50=2.0/51; ema4[0]=B[0].c; ema9[0]=B[0].c; ema50[0]=B[0].c;
      for(int i=1;i<n;i++){ ema4[i]=ema4[i-1]+k1*(B[i].c-ema4[i-1]); ema9[i]=ema9[i-1]+k2*(B[i].c-ema9[i-1]); ema50[i]=ema50[i-1]+k50*(B[i].c-ema50[i-1]); }
      // RSI(14) Wilder
      double ag=0,al=0; int RW=14; for(int i=1;i<n;i++){ double ch=B[i].c-B[i-1].c; double g=ch>0?ch:0, l=ch<0?-ch:0;
        if(i<=RW){ ag+=g; al+=l; if(i==RW){ag/=RW;al/=RW; rsi[i]=al<1e-9?100:100-100/(1+ag/al);} }
        else { ag=(ag*(RW-1)+g)/RW; al=(al*(RW-1)+l)/RW; rsi[i]=al<1e-9?100:100-100/(1+ag/al); } }
      double a=0; int W=14; for(int i=1;i<n;i++){ double tr=max(B[i].h-B[i].l,max(fabs(B[i].h-B[i-1].c),fabs(B[i].l-B[i-1].c))); a=(i<=W)?(a*(i-1)+tr)/i:a+(tr-a)/W; atr[i]=a; }
      // session VWAP (resets per ET day)
      int64_t cday=-1; double pv=0,vv=0; for(int i=0;i<n;i++){ int64_t d=et_day(B[i].ts); if(d!=cday){cday=d;pv=0;vv=0;} double tp=(B[i].h+B[i].l+B[i].c)/3.0; pv+=tp; vv+=1; vwap[i]=pv/vv; } }

    struct Tr{ int64_t ts; double R; int yr; };
    vector<Tr> trades; long entries=0;
    int64_t lastDay=-1; int trToday=0;
    for(int i=2;i<n-1;i++){
        int hm=et_hm(B[i].ts); int64_t day=et_day(B[i].ts);
        if(day!=lastDay){ lastDay=day; trToday=0; }
        if(hm<1000||hm>=1600) continue;             // only 10:00-16:00 ET
        if(trToday>=MAXTR) continue;
        // bias
        int bias=0;
        if(MODE=="cross") bias = (ema4[i]>ema9[i])?+1 : (ema4[i]<ema9[i])?-1:0;
        else              bias = (B[i].c>ema9[i])?+1 : (B[i].c<ema9[i])?-1:0;
        if(bias==0) continue;
        if(useVWAP){ if(bias>0 && !(ema9[i]>vwap[i])) continue; if(bias<0 && !(ema9[i]<vwap[i])) continue; }
        // trend-strength day gate (her "only on strong trending days")
        if(TRSTR>0 && atr[i]>0){ if(fabs(ema9[i]-ema50[i])/atr[i] < TRSTR) continue; }
        // RSI exhaustion gate (don't chase continuation into exhaustion)
        if(RSIGATE){ if(bias>0 && rsi[i]>70) continue; if(bias<0 && rsi[i]<30) continue; }
        // pullback-to-9EMA touch in bias dir on THIS bar (prev bar was away from ema)
        bool touch = bias>0 ? (B[i].l<=ema9[i] && B[i-1].l>ema9[i-1]) : (B[i].h>=ema9[i] && B[i-1].h<ema9[i-1]);
        if(!touch) continue;
        double ep=B[i+1].o;                          // next-bar entry
        double stop = bias>0 ? (B[i].l-bufA*atr[i]) : (B[i].h+bufA*atr[i]);
        double risk = bias>0?(ep-stop):(stop-ep); if(risk<=1e-9) continue;
        // EXITMODE: "r"=fixed Rmult TP ; "trail"=ride until a candle CLOSES back through ema9
        // against the position (her "bands stop holding = trend break"), stop still below reaction.
        static const char* EM = getenv("EXITMODE")? getenv("EXITMODE"):"r";
        double tgt = bias>0 ? ep+Rmult*risk : ep-Rmult*risk;
        double R=0; bool done=false;
        for(int q=i+1;q<min(n,i+1+maxHold);q++){ int hh=et_hm(B[q].ts);
            if(bias>0){ if(B[q].l<=stop){R=-1;done=true;break;} }
            else      { if(B[q].h>=stop){R=-1;done=true;break;} }
            if(!strcmp(EM,"trail")){
                bool brk = bias>0 ? (B[q].c<ema9[q]) : (B[q].c>ema9[q]);   // trend break = band lost
                if(brk){ double xp=B[q].c; R=(bias>0?(xp-ep):(ep-xp))/risk; done=true; break; }
            } else {
                if(bias>0){ if(B[q].h>=tgt){R=Rmult;done=true;break;} } else { if(B[q].l<=tgt){R=Rmult;done=true;break;} }
            }
            if(hh>=1600){ double xp=B[q].c; R=(bias>0?(xp-ep):(ep-xp))/risk; done=true; break; }
        }
        if(!done){ int q=min(n-1,i+maxHold); double xp=B[q].c; R=(bias>0?(xp-ep):(ep-xp))/risk; }
        R-=COST/risk; trades.push_back({B[i].ts,R,yearOf(B[i].ts)}); entries++; trToday++;
    }

    int N=trades.size();
    printf("=== GOLD EMA/VWAP TREND  MODE=%s VWAP=%d R=%.1f MAXTR=%d COST=%.2f ===\n",MODE.c_str(),useVWAP,Rmult,MAXTR,COST);
    printf("entries=%d\n",N); if(!N){printf("NO TRADES\n");return 0;}
    auto stat=[&](vector<Tr>&t,const char*tag){ int m=t.size(); if(!m){printf("  %-6s n=0\n",tag);return;}
        int w=0; double gw=0,gl=0,sum=0,sum2=0; for(auto&x:t){ if(x.R>0){w++;gw+=x.R;}else gl+=-x.R; sum+=x.R; sum2+=x.R*x.R; }
        double pf=gl>0?gw/gl:99,mean=sum/m,sd=sqrt(max(1e-9,(sum2-m*mean*mean)/max(1,m-1)));
        printf("  %-6s n=%d WR=%.1f%% PF=%.2f avgR=%+.3f totR=%+.1f Sharpe/tr=%+.2f\n",tag,m,100.0*w/m,pf,mean,sum,mean/sd); };
    stat(trades,"ALL");
    vector<Tr> h1(trades.begin(),trades.begin()+N/2),h2(trades.begin()+N/2,trades.end()); stat(h1,"H1"); stat(h2,"H2");
    vector<int> ys; for(auto&t:trades) if(find(ys.begin(),ys.end(),t.yr)==ys.end()) ys.push_back(t.yr); sort(ys.begin(),ys.end());
    for(int y:ys){ vector<Tr> yt; for(auto&t:trades) if(t.yr==y) yt.push_back(t); char tg[8]; snprintf(tg,8,"%d",y); stat(yt,tg); }
    return 0;
}

// intraday_breakout_pullback.cpp -- generalize the ORB edge: detect ANY intraday
// consolidation (W bars within C*ATR), on breakout-close arm a retrace-to-RETR entry
// in the breakout direction (trend-filtered), tight stop, structural RUNNER trail.
// = the ORB mechanic applied to every intraday range, not just the opening one.
// run: ./ibp <m5> [W=12] [CRANGE=1.5] [RETR=0.382] [TRWIN=3] [TREND=1] [MAXTR=99] [COST=..]
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
 if(argc<2){fprintf(stderr,"usage:%s <m5>\n",argv[0]);return 1;}
 int W=getenv("W")?atoi(getenv("W")):12; double CR=getenv("CRANGE")?atof(getenv("CRANGE")):1.5;
 double RETR=getenv("RETR")?atof(getenv("RETR")):0.382; int TRWIN=getenv("TRWIN")?atoi(getenv("TRWIN")):3;
 int TREND=getenv("TREND")?atoi(getenv("TREND")):1; int MAXTR=getenv("MAXTR")?atoi(getenv("MAXTR")):99;
 double COST=getenv("COST")?atof(getenv("COST")):0.37; double bufA=0.10; int maxHold=120;
 vector<Bar> B;{ifstream f(argv[1]);string ln;bool fst=true;
  while(getline(f,ln)){if(ln.empty())continue;if(fst){fst=false;if(ln[0]<'0'||ln[0]>'9')continue;}
   const char*s=ln.c_str();char*e=nullptr;long long ts=strtoll(s,&e,10);if(*e!=',')continue;if(ts>100000000000LL)ts/=1000;
   Bar b;b.ts=ts;b.o=strtod(e+1,&e);if(*e!=',')continue;b.h=strtod(e+1,&e);if(*e!=',')continue;b.l=strtod(e+1,&e);if(*e!=',')continue;b.c=strtod(e+1,&e);if(b.h>0)B.push_back(b);}}
 int n=B.size();if(n<500)return 1; sort(B.begin(),B.end(),[](const Bar&a,const Bar&b){return a.ts<b.ts;});
 vector<double> atr(n,0),ema(n,0);{double a=0;int WW=14;double k=2.0/51;ema[0]=B[0].c;for(int i=1;i<n;i++){double tr=max(B[i].h-B[i].l,max(fabs(B[i].h-B[i-1].c),fabs(B[i].l-B[i-1].c)));a=(i<=WW)?(a*(i-1)+tr)/i:a+(tr-a)/WW;atr[i]=a;ema[i]=ema[i-1]+k*(B[i].c-ema[i-1]);}}
 struct Tr{int64_t ts;double R;int yr;};vector<Tr> trades;long entries=0,cons=0,brk=0;
 int64_t curday=-1;int trToday=0; deque<double> lows,highs;
 bool inpos=false;int side=0;double ep=0,sl=0,trail=0,risk=0;int eidx=0;
 bool armed=false;int abias=0;double alvl=0,arangeHi=0,arangeLo=0;int armBar=0;
 for(int i=W;i<n;i++){
  int hm=et_hm(B[i].ts);int64_t day=et_day(B[i].ts);
  lows.push_back(B[i].l);highs.push_back(B[i].h);while((int)lows.size()>TRWIN)lows.pop_front();while((int)highs.size()>TRWIN)highs.pop_front();
  if(day!=curday){curday=day;trToday=0;armed=false;}
  if(inpos){
   if(side>0){double s=1e18;for(double v:lows)s=min(s,v);s-=bufA*atr[i];if(s>trail)trail=s;if(B[i].l<=trail){double R=(trail-ep)/risk-COST/risk;trades.push_back({B[i].ts,R,yearOf(B[i].ts)});inpos=false;}}
   else{double s=-1e18;for(double v:highs)s=max(s,v);s+=bufA*atr[i];if(s<trail)trail=s;if(B[i].h>=trail){double R=(ep-trail)/risk-COST/risk;trades.push_back({B[i].ts,R,yearOf(B[i].ts)});inpos=false;}}
   if(inpos&&(hm>=1600||i-eidx>maxHold)){double xp=B[i].c;double R=(side>0?(xp-ep):(ep-xp))/risk-COST/risk;trades.push_back({B[i].ts,R,yearOf(B[i].ts)});inpos=false;}
   continue;
  }
  if(hm<1000||hm>=1545){armed=false;continue;} if(atr[i]<=0)continue; if(trToday>=MAXTR)continue;
  // fill armed retrace
  if(armed){
   bool touch = abias>0?(B[i].l<=alvl):(B[i].h>=alvl);
   if(touch){double stp=abias>0?(B[i].l-bufA*atr[i]):(B[i].h+bufA*atr[i]);double rk=abias>0?(alvl-stp):(stp-alvl);
    if(rk>0.05*atr[i]){inpos=true;side=abias;ep=alvl;sl=stp;trail=stp;risk=rk;eidx=i;entries++;trToday++;} armed=false;}
   if(i-armBar>W){armed=false;} // expire stale arm
   continue;
  }
  // detect consolidation over [i-W, i-1] then breakout on bar i
  double hi=-1e18,lo=1e18;for(int k=i-W;k<i;k++){hi=max(hi,B[k].h);lo=min(lo,B[k].l);}
  double rng=hi-lo; if(rng<=0||rng>CR*atr[i])continue;  // must be a tight consolidation
  cons++;
  int dir=0; if(B[i].c>hi)dir=1; else if(B[i].c<lo)dir=-1; if(!dir)continue; brk++;
  if(TREND){if(dir>0&&!(B[i].c>ema[i]))continue;if(dir<0&&!(B[i].c<ema[i]))continue;}
  arangeHi=hi;arangeLo=lo;abias=dir;alvl=dir>0?(hi-RETR*rng):(lo+RETR*rng);armed=true;armBar=i;
 }
 int N=trades.size();
 printf("=== INTRADAY BREAKOUT-PULLBACK W=%d CRANGE=%.1f RETR=%.3f TREND=%d MAXTR=%d COST=%.3f ===\n",W,CR,RETR,TREND,MAXTR,COST);
 printf("consolidations=%ld breakouts=%ld entries=%ld\n",cons,brk,entries); if(!N){printf("NO TRADES\n");return 0;}
 auto stat=[&](vector<Tr>&t,const char*tag){int m=t.size();if(!m){printf("  %-6s n=0\n",tag);return;}int w=0;double gw=0,gl=0,sum=0;for(auto&x:t){if(x.R>0){w++;gw+=x.R;}else gl+=-x.R;sum+=x.R;}printf("  %-6s n=%d WR=%.1f%% PF=%.2f avgR=%+.3f totR=%+.1f\n",tag,m,100.0*w/m,gl>0?gw/gl:99,sum/m,sum);};
 stat(trades,"ALL");vector<Tr> h1(trades.begin(),trades.begin()+N/2),h2(trades.begin()+N/2,trades.end());stat(h1,"H1");stat(h2,"H2");
 vector<int> ys;for(auto&t:trades)if(find(ys.begin(),ys.end(),t.yr)==ys.end())ys.push_back(t.yr);sort(ys.begin(),ys.end());
 for(int y:ys){vector<Tr> yt;for(auto&t:trades)if(t.yr==y)yt.push_back(t);char tg[8];snprintf(tg,8,"%d",y);stat(yt,tg);}
 return 0;
}

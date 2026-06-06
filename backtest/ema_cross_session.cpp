// ema_cross_session.cpp -- Peachy futures 4/9 EMA CROSS strategy, SESSION-gated.
// Her claim: take 4/9 crosses in LOW-VOLUME overnight sessions (London 0345-0900 ET +
// Asian 1800-2400 ET) where price trends cleanly with small candles -> good RR; crosses
// FAIL in NY (big candles). Entry = the cross (golden=long 4>9, death=short). Stop =
// prior PIVOT (recent swing). Exit = opposite cross (her primary "stay in while crossed")
// or structural trail. run: ./ema_cross_session <m5> [SESS=london|asian|ny|all] [EXIT=cross|trail] [COST=..]
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
static int yearOf(int64_t ts){time_t t=ts;struct tm g;gmtime_r(&t,&g);return g.tm_year+1900;}
int main(int argc,char**argv){
 if(argc<2){fprintf(stderr,"usage:%s <m5>\n",argv[0]);return 1;}
 string SESS=getenv("SESS")?getenv("SESS"):"london";
 string EXIT=getenv("EXIT")?getenv("EXIT"):"cross";
 double COST=getenv("COST")?atof(getenv("COST")):0.37;
 int TRWIN=getenv("TRWIN")?atoi(getenv("TRWIN")):3; int PIV=getenv("PIV")?atoi(getenv("PIV")):5;
 double bufA=0.10; double KPLTOL=getenv("KPLTOL")?atof(getenv("KPLTOL")):0.0;
 auto inSess=[&](int hm)->bool{
   if(SESS=="london") return hm>=345&&hm<900;
   if(SESS=="asian")  return hm>=1800||hm<100;     // 18:00-01:00 ET
   if(SESS=="ny")     return hm>=930&&hm<1500;
   if(SESS=="overnight") return (hm>=1800||hm<900); // both quiet sessions
   return true; };
 vector<Bar> B;{ifstream f(argv[1]);string ln;bool fst=true;
  while(getline(f,ln)){if(ln.empty())continue;if(fst){fst=false;if(ln[0]<'0'||ln[0]>'9')continue;}
   const char*s=ln.c_str();char*e=nullptr;long long ts=strtoll(s,&e,10);if(*e!=',')continue;if(ts>100000000000LL)ts/=1000;
   Bar b;b.ts=ts;b.o=strtod(e+1,&e);if(*e!=',')continue;b.h=strtod(e+1,&e);if(*e!=',')continue;b.l=strtod(e+1,&e);if(*e!=',')continue;b.c=strtod(e+1,&e);if(b.h>0)B.push_back(b);}}
 int n=B.size();if(n<500)return 1; sort(B.begin(),B.end(),[](const Bar&a,const Bar&b){return a.ts<b.ts;});
 vector<double> e4(n),e9(n),atr(n,0);{double k4=2.0/5,k9=2.0/10;e4[0]=B[0].c;e9[0]=B[0].c;double a=0;int W=14;
  for(int i=1;i<n;i++){e4[i]=e4[i-1]+k4*(B[i].c-e4[i-1]);e9[i]=e9[i-1]+k9*(B[i].c-e9[i-1]);double tr=max(B[i].h-B[i].l,max(fabs(B[i].h-B[i-1].c),fabs(B[i].l-B[i-1].c)));a=(i<=W)?(a*(i-1)+tr)/i:a+(tr-a)/W;atr[i]=a;}}
 struct Tr{int64_t ts;double R;int yr;};vector<Tr> trades;long entries=0;
 deque<double> lows,highs; int64_t cday=-1; double pdh=0,pdl=0,onh=0,onl=0,dh=-1e18,dl=1e18; bool inpos=false;int side=0;double ep=0,sl=0,trail=0,risk=0;
 for(int i=PIV+1;i<n;i++){
  int hm=et_hm(B[i].ts);
  { int64_t dd=(B[i].ts+et_off(B[i].ts)*3600)/86400; if(dd!=cday){cday=dd;pdh=dh;pdl=dl;dh=-1e18;dl=1e18;onh=-1e18;onl=1e18;} dh=max(dh,B[i].h);dl=min(dl,B[i].l); if(hm>=1800||hm<300){onh=max(onh,B[i].h);onl=min(onl,B[i].l);} }
  lows.push_back(B[i].l);highs.push_back(B[i].h);while((int)lows.size()>TRWIN)lows.pop_front();while((int)highs.size()>TRWIN)highs.pop_front();
  // cross detection
  bool golden = e4[i-1]<=e9[i-1] && e4[i]>e9[i];
  bool death  = e4[i-1]>=e9[i-1] && e4[i]<e9[i];
  if(inpos){
   bool opp = side>0? death : golden;
   if(EXIT=="cross"){ if(opp){double xp=B[i].c;double R=(side>0?(xp-ep):(ep-xp))/risk-COST/risk;trades.push_back({B[i].ts,R,yearOf(B[i].ts)});inpos=false;}
     else if(side>0&&B[i].l<=sl){double R=-1-COST/risk;trades.push_back({B[i].ts,R,yearOf(B[i].ts)});inpos=false;}
     else if(side<0&&B[i].h>=sl){double R=-1-COST/risk;trades.push_back({B[i].ts,R,yearOf(B[i].ts)});inpos=false;} }
   else { // trail
     if(side>0){double s=1e18;for(double v:lows)s=min(s,v);s-=bufA*atr[i];if(s>trail)trail=s;if(B[i].l<=trail){double R=(trail-ep)/risk-COST/risk;trades.push_back({B[i].ts,R,yearOf(B[i].ts)});inpos=false;}}
     else{double s=-1e18;for(double v:highs)s=max(s,v);s+=bufA*atr[i];if(s<trail)trail=s;if(B[i].h>=trail){double R=(ep-trail)/risk-COST/risk;trades.push_back({B[i].ts,R,yearOf(B[i].ts)});inpos=false;}} }
   continue;
  }
  if(!inSess(hm))continue; if(atr[i]<=0)continue;
  int dir = golden?1:(death?-1:0); if(!dir)continue;
  if(KPLTOL>0){double pc=B[i].c;double tol=KPLTOL*atr[i];bool near=false; double L[4]={pdh,pdl,onh,onl}; for(int z=0;z<4;z++){if(L[z]>0&&L[z]<1e17&&fabs(pc-L[z])<=tol){near=true;break;}} if(!near)continue;}
  // stop at prior pivot (recent swing low/high over PIV bars)
  double stp; if(dir>0){double lo=1e18;for(int k=i-PIV;k<=i;k++)lo=min(lo,B[k].l);stp=lo-bufA*atr[i];}
  else {double hi=-1e18;for(int k=i-PIV;k<=i;k++)hi=max(hi,B[k].h);stp=hi+bufA*atr[i];}
  double ent=B[i].c; double rk=dir>0?(ent-stp):(stp-ent); if(rk<=0.05*atr[i])continue;
  inpos=true;side=dir;ep=ent;sl=stp;trail=stp;risk=rk;entries++;
 }
 int N=trades.size();
 printf("=== EMA 4/9 CROSS  SESS=%s EXIT=%s COST=%.3f ===\n",SESS.c_str(),EXIT.c_str(),COST);
 printf("entries=%ld\n",entries);if(!N){printf("NO TRADES\n");return 0;}
 auto stat=[&](vector<Tr>&t,const char*tag){int m=t.size();if(!m){printf("  %-6s n=0\n",tag);return;}int w=0;double gw=0,gl=0,sum=0;for(auto&x:t){if(x.R>0){w++;gw+=x.R;}else gl+=-x.R;sum+=x.R;}printf("  %-6s n=%d WR=%.1f%% PF=%.2f avgR=%+.3f totR=%+.1f\n",tag,m,100.0*w/m,gl>0?gw/gl:99,sum/m,sum);};
 stat(trades,"ALL");vector<Tr> h1(trades.begin(),trades.begin()+N/2),h2(trades.begin()+N/2,trades.end());stat(h1,"H1");stat(h2,"H2");
 vector<int> ys;for(auto&t:trades)if(find(ys.begin(),ys.end(),t.yr)==ys.end())ys.push_back(t.yr);sort(ys.begin(),ys.end());
 for(int y:ys){vector<Tr> yt;for(auto&t:trades)if(t.yr==y)yt.push_back(t);char tg[8];snprintf(tg,8,"%d",y);stat(yt,tg);}
 return 0;
}

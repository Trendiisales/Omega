// index_session_cap_test.cpp — does a hard intraday MAX_LOSS cap help the
// IndexSession long-at-open / exit-at-close engine? Models entry at the session
// open, exit at min(ATR-stop, MAX_LOSS cap, session close). Compares cap off vs
// 1.0% vs 1.5% on the real intraday index tapes. cost-incl, walk-forward.
// build: g++ -std=c++17 -O2 backtest/index_session_cap_test.cpp -o backtest/idxsess_cap
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
struct Bar{int64_t ts;double o,h,l,c;int day,hr;};
static std::vector<Bar> load(const std::string&p,bool ms){std::vector<Bar>v;std::ifstream f(p);if(!f)return v;
 std::string ln;bool fst=true;while(std::getline(f,ln)){if(fst){fst=false;if(!ln.empty()&&(ln[0]<'0'||ln[0]>'9')&&ln[0]!='-')continue;}
  std::stringstream s(ln);std::string t;std::vector<std::string>k;while(std::getline(s,t,','))k.push_back(t);if(k.size()<5)continue;
  Bar b;int64_t ts=std::atoll(k[0].c_str());int64_t sec=ms?ts/1000:ts;b.ts=sec;b.o=atof(k[1].c_str());b.h=atof(k[2].c_str());b.l=atof(k[3].c_str());b.c=atof(k[4].c_str());
  b.day=(int)(sec/86400);b.hr=(int)((sec%86400)/3600);if(b.h>0)v.push_back(b);}return v;}
int main(int argc,char**argv){
 if(argc<5){std::printf("usage: %s csv ms(0/1) h0 h1 [costpts]\n",argv[0]);return 1;}
 std::string path=argv[1];bool ms=atoi(argv[2]);int h0=atoi(argv[3]),h1=atoi(argv[4]);double COST=argc>5?atof(argv[5]):1.0;
 auto b=load(path,ms);int N=(int)b.size();std::printf("[tape] %s bars=%d (sess %02d-%02d UTC)\n",path.c_str(),N,h0,h1);if(N<200)return 1;
 // group by day
 std::map<int,std::vector<int>> byday; for(int i=0;i<N;++i)byday[b[i].day].push_back(i);
 // session-ATR = EWM of daily range
 double atr=0;int seen=0; std::map<int,double> dayATR; int prevday=-1;
 for(auto&kv:byday){double hi=-1e18,lo=1e18;for(int i:kv.second){hi=std::max(hi,b[i].h);lo=std::min(lo,b[i].l);}double r=hi-lo;
   if(seen<14){atr+= r;if(++seen==14)atr/=14;}else atr=(atr*13+r)/14; dayATR[kv.first]=atr;(void)prevday;}
 auto run=[&](double maxloss_pct,double stop_atr,int lo,int hi,double&n1,double&n2,int mid,int&nt,double&net,double&dd,int&w){
   double cum=0,peak=0;nt=0;net=0;dd=0;w=0;n1=0;n2=0;
   for(auto&kv:byday){int d=kv.first;auto&idx=kv.second; double A=dayATR.count(d-1)?dayATR[d-1]:dayATR[d]; if(A<=0)continue;
     // entry = first bar with hr>=h0; exit at hr>=h1 or stop/cap
     int ei=-1;for(int i:idx)if(b[i].hr>=h0){ei=i;break;} if(ei<0)continue; int gi=&kv==nullptr?0:0;(void)gi;
     if(ei<lo||ei>=hi)continue;
     double entry=b[ei].o>0?b[ei].o:b[ei].c; double stop=entry-stop_atr*A; double cap=maxloss_pct>0?entry*(1-maxloss_pct/100):-1e18;
     double exitpx=entry; bool done=false;
     for(int i:idx){ if(i<ei)continue; if(b[i].l<=stop){exitpx=stop;done=true;break;} if(maxloss_pct>0&&b[i].l<=cap){exitpx=cap;done=true;break;} if(b[i].hr>=h1){exitpx=b[i].c;done=true;break;} exitpx=b[i].c; }
     (void)done; double p=(exitpx-entry)-COST; nt++; if(p>=0)w++; net+=p; cum+=p; if(cum>peak)peak=cum; if(peak-cum>dd)dd=peak-cum; if(ei<mid)n1+=p;else n2+=p;
   }
 };
 int mid=(b.front().ts+b.back().ts)/2; // not bar-index; approximate split by ts via ei compare? use day-index mid
 // simpler: split by calendar — use mid = middle day index*86400
 int dlo=b.front().day, dhi=b.back().day; int dmid=(dlo+dhi)/2; mid=dmid*86400 + 0; // ei is bar index though
 // re-map: run() compares ei(bar index) to mid — make mid a bar index
 mid=N/2;
 std::printf("%-16s | n=%4d net=%9.1f maxDD=%8.1f rDD=%5.2f wr=%4.1f%% | H1=%8.1f H2=%8.1f %s\n","",0,0.0,0.0,0.0,0.0,0.0,0.0,"");
 auto rep=[&](const char*tag,double cap,double sa){int n;double net,dd;int w;double n1,n2;run(cap,sa,0,N,n1,n2,mid,n,net,dd,w);
   std::printf("%-16s | n=%4d net=%9.1f maxDD=%8.1f rDD=%5.2f wr=%4.1f%% | H1=%8.1f H2=%8.1f %s\n",
     tag,n,net,dd,dd>0?net/dd:0,n?100.0*w/n:0,n1,n2,(n1>0&&n2>0)?"BOTH+":"");};
 rep("no-cap",0,2.0); rep("cap 1.5%",1.5,2.0); rep("cap 1.0%",1.0,2.0); rep("cap 0.7%",0.7,2.0);
 return 0;
}

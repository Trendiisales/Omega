// sweep_reversal.cpp — liquidity-sweep reversal ("turtle soup"), the structural
// complement to FVG continuation. Price wicks BEYOND a draw-on-liquidity
// (prior-day H/L or N-bar swing), then CLOSES back inside = a failed breakout /
// stop-run -> fade it. Long after a swept low reclaims; short after a swept high.
// Stop beyond the sweep wick; target = opposite side / fixed R. cost-incl, WF.
// build: g++ -std=c++17 -O2 sweep_reversal.cpp -o sweeprev
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
using namespace std;
struct Bar{int64_t ts;double o,h,l,c;};
static int64_t iso2unix(const string&s){int Y=atoi(s.substr(0,4).c_str()),M=atoi(s.substr(5,2).c_str()),D=atoi(s.substr(8,2).c_str());
 int h=atoi(s.substr(11,2).c_str()),mi=atoi(s.substr(14,2).c_str()),se=atoi(s.substr(17,2).c_str());
 int y=Y-(M<=2);int era=(y>=0?y:y-399)/400;unsigned yoe=(unsigned)(y-era*400);unsigned doy=(153*(M+(M>2?-3:9))+2)/5+D-1;
 unsigned doe=yoe*365+yoe/4-yoe/100+doy;int64_t days=(int64_t)era*146097+(int)doe-719468;return days*86400+h*3600+mi*60+se;}
static vector<Bar> load(const string&p){vector<Bar>v;ifstream f(p);if(!f){fprintf(stderr,"no %s\n",p.c_str());return v;}
 string ln;bool fst=true;while(getline(f,ln)){if(fst){fst=false;if(!ln.empty()&&(ln[0]<'0'||ln[0]>'9'))continue;}
  stringstream s(ln);string t;vector<string>k;while(getline(s,t,','))k.push_back(t);if(k.size()<5)continue;Bar b;
  b.ts=(k[0].find('T')!=string::npos)?iso2unix(k[0]):(int64_t)atoll(k[0].c_str());
  b.o=atof(k[1].c_str());b.h=atof(k[2].c_str());b.l=atof(k[3].c_str());b.c=atof(k[4].c_str());if(b.h>0)v.push_back(b);}return v;}
static vector<Bar> agg(const vector<Bar>&m,int N){vector<Bar>o;int64_t W=N*60,cur=-1;Bar b{};for(auto&x:m){int64_t g=(x.ts/W)*W;
 if(g!=cur){if(cur>=0)o.push_back(b);cur=g;b=x;b.ts=g;}else{b.h=max(b.h,x.h);b.l=min(b.l,x.l);b.c=x.c;}}if(cur>=0)o.push_back(b);return o;}
static int hm(int64_t s){int x=(int)(((s%86400)+86400)%86400);return (x/3600)*100+(x%3600)/60;}
static int64_t day(int64_t s){return s/86400;}
static double atrAt(const vector<Bar>&b,int i,int n){if(i<1)return 0;int lo=max(1,i-n+1);double s=0;int c=0;
 for(int k=lo;k<=i;++k){double tr=max(b[k].h-b[k].l,max(fabs(b[k].h-b[k-1].c),fabs(b[k].l-b[k-1].c)));s+=tr;c++;}return c?s/c:0;}

int main(int argc,char**argv){
 if(argc<2){printf("usage: %s csv [HTF=15] [sess=ny|all] [cost=2] [stopBuf=0.25] [tpR=2] [swingLB=20] [name]\n",argv[0]);return 1;}
 string path=argv[1];int HTF=argc>2?atoi(argv[2]):15;string SESS=argc>3?argv[3]:"ny";double COST=argc>4?atof(argv[4]):2.0;
 double SBUF=argc>5?atof(argv[5]):0.25;double TPR=argc>6?atof(argv[6]):2.0;int LB=argc>7?atoi(argv[7]):20;string NAME=argc>8?argv[8]:path;
 auto m1=load(path);if((int)m1.size()<500){printf("[%s] few\n",NAME.c_str());return 1;}
 auto b=agg(m1,HTF);int N=(int)b.size();
 // prior-day H/L
 vector<int64_t>days;vector<double>dh,dl;{int64_t cd=-1;double hi=0,lo=0;for(auto&x:b){int64_t d=day(x.ts);
  if(d!=cd){if(cd>=0){days.push_back(cd);dh.push_back(hi);dl.push_back(lo);}cd=d;hi=x.h;lo=x.l;}else{hi=max(hi,x.h);lo=min(lo,x.l);}}
  if(cd>=0){days.push_back(cd);dh.push_back(hi);dl.push_back(lo);}}
 auto pdh=[&](int64_t t){int64_t d=day(t);for(int i=(int)days.size()-1;i>=0;--i)if(days[i]<d)return dh[i];return -1.0;};
 auto pdl=[&](int64_t t){int64_t d=day(t);for(int i=(int)days.size()-1;i>=0;--i)if(days[i]<d)return dl[i];return -1.0;};
 auto inS=[&](int64_t t){int x=hm(t);if(SESS=="all")return true;if(SESS=="ny")return x>=1330&&x<=1600;return true;};
 struct T{double e,sl,tp;int dir;double net,r;bool win;};vector<T>tr;
 bool in=false;T cur{};
 for(int i=LB+1;i<N;++i){
   // manage
   if(in){bool sl=cur.dir>0?(b[i].l<=cur.sl):(b[i].h>=cur.sl);bool tp=cur.dir>0?(b[i].h>=cur.tp):(b[i].l<=cur.tp);
     if(sl||tp){double ex=sl?cur.sl:cur.tp;double p=(cur.dir>0?(ex-cur.e):(cur.e-ex))-COST;cur.net=p;cur.r=cur.sl!=cur.e?fabs(p)/fabs(cur.e-cur.sl):0;cur.win=p>0;
       tr.push_back(cur);in=false;}continue;}
   if(!inS(b[i].ts))continue;double a=atrAt(b,i,14);if(a<=0)continue;
   // swing high/low over last LB bars (excl current)
   double shi=-1e18,slo=1e18;for(int k=i-LB;k<i;++k){shi=max(shi,b[k].h);slo=min(slo,b[k].l);}
   double ph=pdh(b[i].ts),pl=pdl(b[i].ts);
   double hiLvl=max(shi,ph>0?ph:-1e18), loLvl=min(slo,pl>0?pl:1e18);
   // SWEEP HIGH -> short: wick above level, close back below
   if(b[i].h>hiLvl && b[i].c<hiLvl && (b[i].h-hiLvl)<1.5*a){
     double e=b[i].c,sl=b[i].h+SBUF*a,r=sl-e;if(r>0.1*a){double tp=e-TPR*r;cur=T{e,sl,tp,-1,0,0,false};in=true;continue;}}
   // SWEEP LOW -> long
   if(b[i].l<loLvl && b[i].c>loLvl && (loLvl-b[i].l)<1.5*a){
     double e=b[i].c,sl=b[i].l-SBUF*a,r=e-sl;if(r>0.1*a){double tp=e+TPR*r;cur=T{e,sl,tp,+1,0,0,false};in=true;}}
 }
 auto rep=[&](const char*tag,int lo,int hi){int n=0,w=0;double net=0,gw=0,gl=0,pk=0,cm=0,dd=0,sR=0;vector<double>rs;
   for(int i=lo;i<hi;++i){auto&t=tr[i];n++;net+=t.net;double R=t.net/(fabs(t.e-t.sl)>0?fabs(t.e-t.sl):1);sR+=R;rs.push_back(R);
     if(t.win){w++;gw+=t.net;}else gl+=-t.net;cm+=t.net;if(cm>pk)pk=cm;if(pk-cm>dd)dd=pk-cm;}
   double pf=gl>0?gw/gl:(gw>0?99:0),mR=n?sR/n:0,var=0;for(double r:rs)var+=(r-mR)*(r-mR);double sd=rs.size()>1?sqrt(var/(rs.size()-1)):0;
   double tpy=n/2.0,sh=sd>0?mR/sd*sqrt(tpy>0?tpy:1):0;
   printf("  %-9s n=%3d WR=%4.1f%% PF=%4.2f net=%8.1f avgR=%+5.2f Sharpe/yr=%4.2f maxDD=%7.1f ret/DD=%4.2f\n",
     tag,n,n?100.0*w/n:0,pf,net,mR,sh,dd,dd>0?net/dd:0);};
 printf("[%s] SWEEP-REV HTF=%dm sess=%s cost=%.2f sbuf=%.2f tpR=%.1f LB=%d | tr=%d\n",NAME.c_str(),HTF,SESS.c_str(),COST,SBUF,TPR,LB,(int)tr.size());
 if(tr.empty()){printf("  (none)\n");return 0;}
 rep("ALL",0,(int)tr.size());int m=(int)tr.size()/2;rep("H1",0,m);rep("H2",m,(int)tr.size());
 return 0;}

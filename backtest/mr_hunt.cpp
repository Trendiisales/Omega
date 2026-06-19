// Daily index mean-reversion signal hunt. Same chassis: close>SMA200 uptrend filter +
// enhanced exit (close>SMA5 || maxhold10). Swaps the ENTRY oversold signal. Long-only.
// RUN: mr_hunt <daily.csv ts,o,h,l,c> <label> [cost=4]
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <cmath>
#include <ctime>
struct Bar{int64_t ts;double o,h,l,c;};
static std::vector<Bar> B;
static std::vector<Bar> load(const char*p){std::vector<Bar>v;std::ifstream f(p);std::string l;
 while(std::getline(f,l)){if(l.empty()||(l[0]<'0'||l[0]>'9'))continue;Bar b{};double t;
  if(std::sscanf(l.c_str(),"%lf,%lf,%lf,%lf,%lf",&t,&b.o,&b.h,&b.l,&b.c)!=5)continue;b.ts=(int64_t)t;v.push_back(b);}return v;}
static int yr(int64_t s){std::time_t t=s;std::tm g{};gmtime_r(&t,&g);return g.tm_year+1900;}
static double sma(int i,int n){double s=0;for(int k=i-n+1;k<=i;k++)s+=B[k].c;return s/n;}
static double ibs(int i){double r=B[i].h-B[i].l;return r<=0?.5:(B[i].c-B[i].l)/r;}
static double rsi(int i,int n){double g=0,l=0;for(int k=i-n+1;k<=i;k++){double ch=B[k].c-B[k-1].c;if(ch>0)g+=ch;else l+=-ch;}return l==0?100:100-100/(1+g/l);}
static int downstreak(int i){int n=0;for(int k=i;k>0&&B[k].c<B[k-1].c;k--)n++;return n;}
static double pctb(int i,int n){double m=sma(i,n),sd=0;for(int k=i-n+1;k<=i;k++)sd+=(B[k].c-m)*(B[k].c-m);sd=std::sqrt(sd/n);double lo=m-2*sd,hi=m+2*sd;return hi<=lo?.5:(B[i].c-lo)/(hi-lo);}
typedef bool(*Sig)(int);
static bool s_ibs(int i){return ibs(i)<0.10;}
static bool s_rsi2(int i){return rsi(i,2)<10;}
static bool s_rsi3(int i){return rsi(i,3)<15;}
static bool s_streak3(int i){return downstreak(i)>=3;}
static bool s_streak4(int i){return downstreak(i)>=4;}
static bool s_pctb(int i){return pctb(i,20)<0.0;}
static bool s_dbl(int i){return ibs(i)<0.20 && rsi(i,2)<15;}    // double-confirm
static bool s_ret2(int i){return (B[i].c/B[i-2].c-1.0)<-0.02;}  // 2-day drop >2%
struct S{const char*name;Sig f;};
static void run(const char*lbl,const char*nm,Sig f,double cost){
 const int TREND=200,SHORT=5,MAXHOLD=10;
 struct T{int64_t ts;double pnl;};std::vector<T>trs;bool act=false;double entry=0;int64_t ets=0;int held=0;
 for(int i=TREND;i<(int)B.size();++i){
   if(act){held++;bool ex=(B[i].c>sma(i,SHORT))||(held>=MAXHOLD);if(ex){trs.push_back({ets,(B[i].c-entry)-cost});act=false;}}
   if(!act){ if(B[i].c>sma(i,TREND) && f(i)){act=true;entry=B[i].c;ets=B[i].ts;held=0;} }
 }
 double net=0,gw=0,gl=0;int w=0,y22=0;for(auto&t:trs){net+=t.pnl;if(t.pnl>0){gw+=t.pnl;++w;}else gl+=-t.pnl;if(yr(t.ts)==2022)y22+=t.pnl;}
 double pf=gl>0?gw/gl:(gw>0?99:0);double h1=0,h2=0;for(size_t i=0;i<trs.size();++i)(i<trs.size()/2?h1:h2)+=trs[i].pnl;
 printf("  %-12s n=%-3zu WR=%2.0f%% PF=%5.2f net=%+7.0f H1=%+6.0f H2=%+6.0f 22=%+5d %s\n",
   nm,trs.size(),trs.empty()?0:100.0*w/trs.size(),pf,net,h1,h2,y22,(h1>0&&h2>0&&!trs.empty())?"BOTH+":"");
}
int main(int argc,char**argv){
 const char*path=argv[1];const char*lbl=argv[2];double cost=argc>3?atof(argv[3]):4.0;
 B=load(path); if((int)B.size()<260){fprintf(stderr,"few\n");return 1;}
 S sigs[]={{"IBS<.10",s_ibs},{"RSI2<10",s_rsi2},{"RSI3<15",s_rsi3},{"streak>=3",s_streak3},
   {"streak>=4",s_streak4},{"%b<0",s_pctb},{"IBS.2&RSI15",s_dbl},{"2dret<-2%",s_ret2}};
 printf("== %s (cost=%.0f) ==\n",lbl,cost);
 for(auto&s:sigs) run(lbl,s.name,s.f,cost);
 return 0;}

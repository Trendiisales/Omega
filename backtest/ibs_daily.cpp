// IBS (Internal Bar Strength) daily mean-reversion: IBS=(c-l)/(h-l). Buy when IBS<IN
// AND close>SMA(TREND) (uptrend). Exit when IBS>OUT OR close>SMA(SHORT) OR maxhold.
// Long-only. RUN: ibs <daily.csv ts,o,h,l,c> <label> [cost=4] [IN=0.2] [OUT=0.7] [trend=200]
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <ctime>
struct Bar{int64_t ts;double o,h,l,c;};
static std::vector<Bar> load(const char*p){std::vector<Bar>v;std::ifstream f(p);std::string l;
 while(std::getline(f,l)){if(l.empty()||(l[0]<'0'||l[0]>'9'))continue;Bar b{};double t;
  if(std::sscanf(l.c_str(),"%lf,%lf,%lf,%lf,%lf",&t,&b.o,&b.h,&b.l,&b.c)!=5)continue;b.ts=(int64_t)t;v.push_back(b);}return v;}
static int yr(int64_t s){std::time_t t=s;std::tm g{};gmtime_r(&t,&g);return g.tm_year+1900;}
static double sma(const std::vector<Bar>&B,int i,int n){double s=0;for(int k=i-n+1;k<=i;k++)s+=B[k].c;return s/n;}
static double ibs(const Bar&b){double r=b.h-b.l; return r<=0?0.5:(b.c-b.l)/r;}
int main(int argc,char**argv){
 const char*path=argv[1]; std::string lbl=argv[2];
 double COST=argc>3?atof(argv[3]):4.0, IN=argc>4?atof(argv[4]):0.2, OUT=argc>5?atof(argv[5]):0.7; int TREND=argc>6?atoi(argv[6]):200;
 int SHORT=5,MAXHOLD=10;
 auto B=load(path); if((int)B.size()<TREND+20){fprintf(stderr,"few\n");return 1;}
 struct T{int64_t ts;double pnl;}; std::vector<T> trs;
 bool act=false; double entry=0; int64_t ets=0; int held=0;
 for(int i=TREND;i<(int)B.size();++i){
   if(act){ held++;
     bool ex = (ibs(B[i])>OUT) || (B[i].c>sma(B,i,SHORT)) || (held>=MAXHOLD);
     if(ex){ trs.push_back({ets,(B[i].c-entry)-COST}); act=false; } }
   if(!act){ if(B[i].c>sma(B,i,TREND) && ibs(B[i])<IN){ act=true; entry=B[i].c; ets=B[i].ts; held=0; } }
 }
 double net=0,gw=0,gl=0;int w=0,y22=0; for(auto&t:trs){net+=t.pnl;if(t.pnl>0){gw+=t.pnl;++w;}else gl+=-t.pnl; if(yr(t.ts)==2022)y22+=t.pnl;}
 double pf=gl>0?gw/gl:(gw>0?99:0); double h1=0,h2=0; for(size_t i=0;i<trs.size();++i)(i<trs.size()/2?h1:h2)+=trs[i].pnl;
 printf("%-7s IBS<%.2f>%.2f | n=%-3zu WR=%2.0f%% PF=%.2f net=%+7.0f H1=%+6.0f H2=%+6.0f 2022=%+5d %s\n",
   lbl.c_str(),IN,OUT,trs.size(),trs.empty()?0:100.0*w/trs.size(),pf,net,h1,h2,y22,(h1>0&&h2>0&&!trs.empty())?"BOTH+":"");
 return 0;}

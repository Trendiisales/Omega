// Faithful daily replica of ConnorsRSI2Engine: close>SMA(TREND) AND RSI(2)<RSI_IN
// -> buy@close, exit@close HOLD days later. Long-only. RSI = Cutler's (matches
// engine _rsi: g/l over last n daily changes). Cost = round-trip pts on entry.
// RUN: connors <daily.csv ts,o,h,l,c> <LABEL> [cost_pts=2] [RSI_IN=10] [TREND=200]
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
struct Bar{int64_t ts;double o,h,l,c;};
static std::vector<Bar> load(const char*p){std::vector<Bar>v;std::ifstream f(p);std::string l;
 while(std::getline(f,l)){if(l.empty()||(l[0]<'0'||l[0]>'9'))continue;Bar b{};double t;
  if(std::sscanf(l.c_str(),"%lf,%lf,%lf,%lf,%lf",&t,&b.o,&b.h,&b.l,&b.c)!=5)continue;b.ts=(int64_t)t;v.push_back(b);}return v;}
static int yr(int64_t s){std::time_t t=s;std::tm g{};gmtime_r(&t,&g);return g.tm_year+1900;}
int main(int argc,char**argv){
 if(argc<3){fprintf(stderr,"usage: %s <csv> <label> [cost] [rsi_in] [trend]\n",argv[0]);return 2;}
 const char*path=argv[1]; std::string lbl=argv[2];
 double COST=argc>3?atof(argv[3]):2.0; double RSI_IN=argc>4?atof(argv[4]):10.0; int TREND=argc>5?atoi(argv[5]):200; int HOLD=1;
 auto B=load(path); if((int)B.size()<TREND+50){fprintf(stderr,"few bars\n");return 1;}
 auto rsi=[&](int i,int n){ if(i<n)return 50.0; double g=0,l=0; for(int k=i-n+1;k<=i;k++){double ch=B[k].c-B[k-1].c; if(ch>0)g+=ch; else l+=-ch;} return l==0?100.0:100.0-100.0/(1.0+g/l);};
 struct T{int64_t ts;double pnl;}; std::vector<T> trs;
 bool active=false; double entry=0; int64_t ets=0; int held=0;
 for(int i=TREND;i<(int)B.size();++i){
   // exit at close HOLD days after entry
   if(active){ held++; if(held>=HOLD){ double pnl=(B[i].c-entry)-COST; trs.push_back({ets,pnl}); active=false; } }
   if(!active){
     double sma=0; for(int k=i-TREND+1;k<=i;k++)sma+=B[k].c; sma/=TREND;
     double r=rsi(i,2);
     if(B[i].c>sma && r<RSI_IN){ active=true; entry=B[i].c; ets=B[i].ts; held=0; }
   }
 }
 // stats
 double net=0,gw=0,gl=0; int w=0; for(auto&t:trs){net+=t.pnl; if(t.pnl>0){gw+=t.pnl;++w;}else gl+=-t.pnl;}
 double pf=gl>0?gw/gl:(gw>0?99:0); double wr=trs.empty()?0:100.0*w/trs.size();
 // WF halves
 double h1=0,h2=0; for(size_t i=0;i<trs.size();++i)(i<trs.size()/2?h1:h2)+=trs[i].pnl;
 // per-year
 printf("=== ConnorsRSI2 %s | cost=%.1fpt RSI<%.0f SMA%d | n=%zu WR=%.0f%% PF=%.2f net=%+.0fpt  WF H1=%+.0f H2=%+.0f %s ===\n",
   lbl.c_str(),COST,RSI_IN,TREND,trs.size(),wr,pf,net,h1,h2,(h1>0&&h2>0&&!trs.empty())?"BOTH+":"NOT both+");
 int y0=trs.empty()?0:yr(trs.front().ts), y1=trs.empty()?0:yr(trs.back().ts);
 for(int y=y0;y<=y1;++y){ double yn=0;int nn=0; for(auto&t:trs)if(yr(t.ts)==y){yn+=t.pnl;++nn;} if(nn)printf("   %d n=%-3d net=%+.0f\n",y,nn,yn);}
 return 0;}

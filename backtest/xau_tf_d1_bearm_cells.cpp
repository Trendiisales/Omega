// Per-cell attribution: baseline (0,0) vs arm3/buf0 on XauTrendFollowD1.
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <map>
#include "XauTrendFollowD1Engine.hpp"
static double SPREAD=0.60;
struct BarCSV{int64_t ts;double o,h,l,c;};
static std::vector<BarCSV> load(const char*p){std::vector<BarCSV>v;std::ifstream f(p);std::string l;bool fst=true;
 while(std::getline(f,l)){if(l.empty())continue;if(fst){fst=false;if(l[0]<'0'||l[0]>'9')continue;}BarCSV b{};double t;
 if(std::sscanf(l.c_str(),"%lf,%lf,%lf,%lf,%lf",&t,&b.o,&b.h,&b.l,&b.c)!=5)continue;b.ts=(int64_t)t;v.push_back(b);}return v;}
struct S{int n=0,w=0;double pnl=0,gw=0,gl=0,pk=0,eq=0,mdd=0;
 void rec(double u){++n;pnl+=u;if(u>0){++w;gw+=u;}else gl+=std::fabs(u);eq+=u;if(eq>pk)pk=eq;if(pk-eq>mdd)mdd=pk-eq;}
 double pf()const{return gl>0?gw/gl:(gw>0?99:0);} double wr()const{return n?100.0*w/n:0;}};
static void run(const std::vector<BarCSV>&bars,double arm,double buf,std::map<std::string,S>&cells,S&all){
 omega::XauTrendFollowD1Engine e;e.shadow_mode=true;e.enabled=true;e.lot=0.01;e.max_spread=1.0;
 e.LOSS_CUT_PCT=1.0;                       // PROD: live cold-loss cut
 e.use_vol_band_gate=true;e.vol_band_low_pct=0.20;e.vol_band_high_pct=0.90; // PROD vol gate
 e.BE_ARM_PCT=arm;e.BE_BUFFER_PCT=buf;e.init();
 auto cb=[&](const omega::TradeRecord&tr){double u=tr.pnl*100.0;all.rec(u);cells[tr.engine].rec(u);};
 int N=(int)bars.size();
 for(int i=0;i<N;++i){const auto&b=bars[i];int64_t ts=b.ts*1000;
  if(i>0){e.on_tick(b.l,b.l+SPREAD,ts,cb);e.on_tick(b.h,b.h+SPREAD,ts,cb);e.on_tick(b.c,b.c+SPREAD,ts,cb);}
  omega::XauTfD1Bar bar{};bar.bar_start_ms=ts;bar.open=b.o;bar.high=b.h;bar.low=b.l;bar.close=b.c;
  e.on_h4_bar(bar,b.c,b.c+SPREAD,ts,cb);}
 e.force_close(bars.back().c,bars.back().c+SPREAD,bars.back().ts*1000,cb,"EOD_FLAT");}
int main(int argc,char**argv){const char*p=argc>1?argv[1]:"/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv";
 auto bars=load(p);std::printf("[CELLS %s] spread=%.2f\n",p,SPREAD);
 std::map<std::string,S> cb0,cb3;S a0,a3;
 run(bars,0.0,0.0,cb0,a0); run(bars,3.0,0.0,cb3,a3);
 std::printf("%-42s %18s   %18s\n","cell","BASELINE n/PF/$/DD","arm3buf0 n/PF/$/DD");
 std::map<std::string,bool> keys; for(auto&k:cb0)keys[k.first]=1; for(auto&k:cb3)keys[k.first]=1;
 for(auto&kv:keys){const S&x=cb0[kv.first];const S&y=cb3[kv.first];
  std::printf("%-42s  %3d %4.2f %+7.0f %5.0f   %3d %4.2f %+7.0f %5.0f\n",
   kv.first.c_str(),x.n,x.pf(),x.pnl,x.mdd,y.n,y.pf(),y.pnl,y.mdd);}
 std::printf("%-42s  %3d %4.2f %+7.0f %5.0f   %3d %4.2f %+7.0f %5.0f\n",
  "ALL",a0.n,a0.pf(),a0.pnl,a0.mdd,a3.n,a3.pf(),a3.pnl,a3.mdd);
 return 0;}

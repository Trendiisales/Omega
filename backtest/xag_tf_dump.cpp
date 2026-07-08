// xag_tf_dump.cpp -- SILVER RESEARCH (2026-07-08): drive the validated gold trend
// engines (XauTrendFollow4h mask 0xC9 prod / XauTrendFollow2h defaults) over an
// arbitrary H4/H1 CSV with a configurable half-spread, dump per-trade windows.
// Mirrors xau_tf4h_rider_dump.cpp / xau_tf2h_rider_dump.cpp exactly except:
//   argv: <mode 4h|2h> <bars.csv> <out.csv> <half_spread>
// PnL is re-scored in python with silver costs (commission 2*0.00025*px RT);
// the spread cost IS embedded here via bid/ask = px -/+ half.
// build: g++ -std=c++17 -O2 -Iinclude backtest/xag_tf_dump.cpp -o backtest/xag_tf_dump
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "XauTrendFollow4hEngine.hpp"
#include "XauTrendFollow2hEngine.hpp"
struct Bar{int64_t ts;double o,h,l,c;};
static std::vector<Bar> load(const std::string&p){std::vector<Bar>v;std::ifstream f(p);if(!f)return v;std::string ln;std::getline(f,ln);
 while(std::getline(f,ln)){Bar b;const char*s=ln.c_str();char*e;b.ts=std::strtoll(s,&e,10);if(*e!=',')continue;s=e+1;
 b.o=std::strtod(s,&e);if(*e!=',')continue;s=e+1;b.h=std::strtod(s,&e);if(*e!=',')continue;s=e+1;b.l=std::strtod(s,&e);if(*e!=',')continue;s=e+1;b.c=std::strtod(s,&e);v.push_back(b);}return v;}
int main(int argc,char**argv){
 if(argc<5){std::fprintf(stderr,"usage: %s <4h|2h> <bars.csv> <out.csv> <half_spread>\n",argv[0]);return 1;}
 const bool m4h = std::strcmp(argv[1],"4h")==0;
 const char*path=argv[2]; const char*out=argv[3]; const double half=std::atof(argv[4]);
 auto bars=load(path); if(bars.empty()){std::fprintf(stderr,"no bars\n");return 1;}
 FILE*tf=std::fopen(out,"w");
 std::fprintf(tf,"engine,side,entry_ts,exit_ts,entry_px,exit_px,atr_at_entry,mfe,pnl_pts,exit_reason\n");
 int ntr=0; double net=0;
 auto cb=[&](const omega::TradeRecord&tr){double pts=tr.pnl*100.0;net+=pts;ntr++;   // lot=0.01 -> pnl*100 = raw pts (x size_mult)
  std::fprintf(tf,"%s,%s,%lld,%lld,%.4f,%.4f,%.4f,%.4f,%.4f,%s\n",tr.engine.c_str(),tr.side.c_str(),
   (long long)tr.entryTs,(long long)tr.exitTs,tr.entryPrice,tr.exitPrice,tr.atr_at_entry,tr.mfe,pts,tr.exitReason.c_str());};
 if(m4h){
  omega::XauTrendFollow4hEngine eng; eng.shadow_mode=true; eng.enabled=true;
  eng.cell_enable_mask=0xC9; eng.use_regime_long_gate=false;
  eng.lot=0.01; eng.max_spread=1.0; eng.init();
  for(size_t i=0;i<bars.size();++i){const auto&b=bars[i];const int64_t ts=b.ts*1000LL;
   omega::XauTfBar bar; bar.bar_start_ms=ts;bar.open=b.o;bar.high=b.h;bar.low=b.l;bar.close=b.c;
   eng.on_h4_bar(bar,b.c-half,b.c+half,0.0,ts,cb);
   if(i+1<bars.size()){const auto&nb=bars[i+1];const int64_t nts=nb.ts*1000LL;
    eng.on_tick(nb.l-half,nb.l+half,nts,cb); eng.on_tick(nb.h-half,nb.h+half,nts,cb);}}
 }else{
  omega::XauTrendFollow2hEngine eng; eng.shadow_mode=true; eng.enabled=true;
  eng.lot=0.01; eng.max_spread=1.0; eng.init();
  for(size_t i=0;i<bars.size();++i){const auto&b=bars[i];const int64_t ts=b.ts*1000LL;
   omega::XauTf2hBar bar; bar.bar_start_ms=ts;bar.open=b.o;bar.high=b.h;bar.low=b.l;bar.close=b.c;
   eng.on_h1_bar(bar,b.c-half,b.c+half,ts,cb);
   if(i+1<bars.size()){const auto&nb=bars[i+1];const int64_t nts=nb.ts*1000LL;
    eng.on_tick(nb.l-half,nb.l+half,nts,cb); eng.on_tick(nb.h-half,nb.h+half,nts,cb);}}
 }
 std::fclose(tf); std::printf("[%s dump] %d trades net=%.1fpts -> %s\n",m4h?"4h":"2h",ntr,net,out); return 0;}

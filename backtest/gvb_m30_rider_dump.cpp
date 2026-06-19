// gvb_m30_rider_dump.cpp -- faithful GoldVolBreakoutM30 driver -> dumps real
// trade windows (side, entry/exit, atr_at_entry, mfe, pnl) for the rider overlay.
// Drives the REAL engine with production config (defaults; shadow). Feeds M30
// bars + intrabar ticks (low->high->close) + H1 closes (aggregated from M30, the
// trend filter needs them or NO entries fire). Long-only breakout runner.
//
// BUILD: c++ -std=c++17 -O2 -I/Users/jo/Omega/include gvb_m30_rider_dump.cpp -o gvb_dump
// RUN:   gvb_dump <m30_csv> <out_trades_csv> [spread]
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <map>
#include "GoldVolBreakoutM30Engine.hpp"
static double SPREAD=0.20;
struct BarCSV{int64_t ts;double o,h,l,c;};
static std::vector<BarCSV> load(const char*p){std::vector<BarCSV>v;std::ifstream f(p);
 if(!f.is_open()){std::fprintf(stderr,"cannot open %s\n",p);return v;}
 std::string l;bool fst=true;
 while(std::getline(f,l)){if(l.empty())continue;if(fst){fst=false;if(l[0]<'0'||l[0]>'9')continue;}
  BarCSV b{};double t;if(std::sscanf(l.c_str(),"%lf,%lf,%lf,%lf,%lf",&t,&b.o,&b.h,&b.l,&b.c)!=5)continue;
  b.ts=(int64_t)t;v.push_back(b);}return v;}
int main(int argc,char**argv){
 const char*path=argc>1?argv[1]:"/Users/jo/Tick/2yr_XAUUSD_tick_fresh.m30.csv";
 const char*out =argc>2?argv[2]:"/tmp/cullwork/gvb_trades.csv";
 if(argc>3)SPREAD=std::atof(argv[3]);
 auto bars=load(path);
 if((int)bars.size()<200){std::fprintf(stderr,"few bars (%zu)\n",bars.size());return 1;}
 omega::GoldVolBreakoutM30Engine e;
 e.shadow_mode=true;e.enabled=true;e.lot=0.01;e.max_spread=0.80;e.init();
 FILE*tf=std::fopen(out,"w");
 std::fprintf(tf,"engine,side,entry_ts,exit_ts,entry_px,exit_px,atr_at_entry,mfe,pnl_usd,exit_reason\n");
 int ntr=0;double net=0;
 std::map<long long,double> atr_by_entry;   // entry_ts -> atr_at_entry (engine _close drops it)
 auto cb=[&](const omega::TradeRecord&tr){const double usd=tr.pnl*100.0;net+=usd;ntr++;
  double a=tr.atr_at_entry; auto it=atr_by_entry.find((long long)tr.entryTs); if(a<=0.0&&it!=atr_by_entry.end())a=it->second;
  std::fprintf(tf,"GoldVolBreakoutM30,%s,%lld,%lld,%.3f,%.3f,%.4f,%.4f,%.2f,%s\n",
   tr.side.c_str(),(long long)tr.entryTs,(long long)tr.exitTs,tr.entryPrice,tr.exitPrice,
   a,tr.mfe,usd,tr.exitReason.c_str());};
 int64_t cur_hour=-1; bool was_active=false;
 const int N=(int)bars.size();
 for(int i=0;i<N;++i){const auto&b=bars[i];const int64_t ts=b.ts*1000;
  const double bid=b.c, ask=b.c+SPREAD;
  if(i>0){e.on_tick(b.l,b.l+SPREAD,ts,cb);e.on_tick(b.h,b.h+SPREAD,ts,cb);e.on_tick(b.c,b.c+SPREAD,ts,cb);}
  e.on_m30_bar(b.h,b.l,b.c,bid,ask,ts,cb);
  if(e.pos.active && !was_active) atr_by_entry[(long long)(e.pos.entry_ts_ms/1000)]=e.pos.atr_at_entry;
  was_active=e.pos.active;
  const int64_t hr=b.ts/3600;
  if(hr!=cur_hour){cur_hour=hr;e.on_h1_close(b.c);}  // 1 H1 close per hour (aggregated)
 }
 e.force_close(bars.back().c,bars.back().c+SPREAD,bars.back().ts*1000,cb,"EOD_FLAT");
 std::fclose(tf);
 std::fprintf(stderr,"[GVB DUMP] %s  trades=%d  net=$%.0f  -> %s\n",path,ntr,net,out);
 return 0;}

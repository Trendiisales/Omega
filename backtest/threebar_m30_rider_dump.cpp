// threebar_m30_rider_dump.cpp -- faithful XauThreeBar30m driver -> dumps real
// trade windows for the rider overlay. Production config (long_only, BE-ratchet,
// 0.75 ATR trail, slope+vol-band gates, EMA200). atr14_external=0 -> self-compute.
// vol-band gate is NOT standalone-reproducible (live percentile state) -> relative
// rider effect is the signal, not the absolute baseline. Single position.
//
// BUILD: c++ -std=c++17 -O2 -I/Users/jo/Omega/include threebar_m30_rider_dump.cpp -o tb_dump
// RUN:   tb_dump <m30_csv> <out_trades_csv> [spread]
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include "XauThreeBar30mEngine.hpp"
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
 const char*out =argc>2?argv[2]:"/tmp/cullwork/tb_trades.csv";
 if(argc>3)SPREAD=std::atof(argv[3]);
 auto bars=load(path);
 if((int)bars.size()<300){std::fprintf(stderr,"few bars (%zu)\n",bars.size());return 1;}
 omega::XauThreeBar30mEngine e;
 e.shadow_mode=true; e.enabled=true; e.long_only=true; e.lot=0.01; e.max_spread=1.0;
 e.LOSS_CUT_PCT=0.05; e.BE_ARM_PCT=0.03; e.BE_BUFFER_PCT=0.012;
 e.be_trigger_atr=1.0; e.be_cost_buffer_pts=0.10; e.trail_after_be=true; e.trail_atr_mult=0.75;
 e.min_atr_floor=0.30; e.use_slope_gate=true; e.slope_lookback_bars=12;
 e.use_vol_band_gate=true; e.vol_band_low_pct=0.30; e.vol_band_high_pct=0.90;
 e.init();
 FILE*tf=std::fopen(out,"w");
 std::fprintf(tf,"engine,side,entry_ts,exit_ts,entry_px,exit_px,atr_at_entry,mfe,pnl_usd,exit_reason\n");
 int ntr=0;double net=0; double cur_atr=0.0; bool was_active=false;
 auto cb=[&](const omega::TradeRecord&tr){const double usd=tr.pnl*100.0;net+=usd;ntr++;
  double a=tr.atr_at_entry; if(a<=0.0)a=cur_atr;
  std::fprintf(tf,"XauThreeBar30m,%s,%lld,%lld,%.3f,%.3f,%.4f,%.4f,%.2f,%s\n",
   tr.side.c_str(),(long long)tr.entryTs,(long long)tr.exitTs,tr.entryPrice,tr.exitPrice,
   a,tr.mfe,usd,tr.exitReason.c_str());};
 const int N=(int)bars.size();
 for(int i=0;i<N;++i){const auto&b=bars[i];const int64_t ts=b.ts*1000;
  const double bid=b.c, ask=b.c+SPREAD;
  if(i>0){e.on_tick(b.l,b.l+SPREAD,ts,cb);e.on_tick(b.h,b.h+SPREAD,ts,cb);e.on_tick(b.c,b.c+SPREAD,ts,cb);}
  omega::XauThreeBar30mBar bar{}; bar.bar_start_ms=ts; bar.open=b.o; bar.high=b.h; bar.low=b.l; bar.close=b.c;
  e.on_30m_bar(bar,bid,ask,0.0,ts,cb);
  if(e.pos.active && !was_active) cur_atr=e.pos.atr_at_entry;
  was_active=e.pos.active;
 }
 e.force_close(bars.back().c,bars.back().c+SPREAD,bars.back().ts*1000,cb,"EOD_FLAT");
 std::fclose(tf);
 std::fprintf(stderr,"[TB DUMP] %s  trades=%d  net=$%.0f  -> %s\n",path,ntr,net,out);
 return 0;}

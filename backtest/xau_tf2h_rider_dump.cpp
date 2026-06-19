#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include "XauTrendFollow2hEngine.hpp"
struct Bar{int64_t ts;double o,h,l,c;};
static std::vector<Bar> load(const std::string&p){std::vector<Bar>v;std::ifstream f(p);if(!f)return v;std::string ln;std::getline(f,ln);
 while(std::getline(f,ln)){Bar b;const char*s=ln.c_str();char*e;b.ts=std::strtoll(s,&e,10);if(*e!=',')continue;s=e+1;
 b.o=std::strtod(s,&e);if(*e!=',')continue;s=e+1;b.h=std::strtod(s,&e);if(*e!=',')continue;s=e+1;b.l=std::strtod(s,&e);if(*e!=',')continue;s=e+1;b.c=std::strtod(s,&e);v.push_back(b);}return v;}
int main(int argc,char**argv){
 const char*path=argc>1?argv[1]:"/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv";
 const char*out =argc>2?argv[2]:"/tmp/xtf_2h_trades.csv";
 double half=0.15; auto h1=load(path); if(h1.empty()){std::fprintf(stderr,"no bars\n");return 1;}
 omega::XauTrendFollow2hEngine eng; eng.shadow_mode=true; eng.enabled=true;
 eng.lot=0.01; eng.max_spread=1.0; eng.init();
 FILE*tf=std::fopen(out,"w"); std::fprintf(tf,"engine,side,entry_ts,exit_ts,entry_px,exit_px,atr_at_entry,mfe,pnl_usd,exit_reason\n");
 int ntr=0; double net=0;
 auto cb=[&](const omega::TradeRecord&tr){double usd=tr.pnl*100.0;net+=usd;ntr++;
  std::fprintf(tf,"%s,%s,%lld,%lld,%.3f,%.3f,%.4f,%.4f,%.2f,%s\n",tr.engine.c_str(),tr.side.c_str(),
   (long long)tr.entryTs,(long long)tr.exitTs,tr.entryPrice,tr.exitPrice,tr.atr_at_entry,tr.mfe,usd,tr.exitReason.c_str());};
 for(size_t i=0;i<h1.size();++i){const auto&b=h1[i];const int64_t ts=b.ts*1000LL;
  omega::XauTf2hBar bar; bar.bar_start_ms=ts;bar.open=b.o;bar.high=b.h;bar.low=b.l;bar.close=b.c;
  eng.on_h1_bar(bar,b.c-half,b.c+half,ts,cb);
  if(i+1<h1.size()){const auto&nb=h1[i+1];const int64_t nts=nb.ts*1000LL;
   eng.on_tick(nb.l-half,nb.l+half,nts,cb); eng.on_tick(nb.h-half,nb.h+half,nts,cb);}}
 std::fclose(tf); std::printf("[2h dump] %d trades net=$%.1f -> %s\n",ntr,net,out); return 0;}

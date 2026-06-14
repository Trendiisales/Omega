// =============================================================================
// xau_session_portdump.cpp -- per-trade PnL dump for the SessionMomentumEngine
// validated config (S42): XAU NYpm (enter 16:00 UTC, hold 4h, c>EMA200h1) +
// overnight (enter 23:00 UTC, hold 5h, c>EMA200h1), long-only, cross-spread bps.
// Logic lifted verbatim from xau_session_trendfilter.cpp (the validated harness).
// Cost = 1bp (live XAU spread confirmed ~1.1bp tight, S42b handoff).
//
// PnL units: basis points of notional per trade (matches the validating harness).
// PORT_DUMP=/path writes "exit_ts,pnl_bps" per trade (both windows merged, sorted).
//
// BUILD: c++ -std=c++17 -O2 backtest/xau_session_portdump.cpp -o /tmp/port/sess
// RUN  : PORT_DUMP=/tmp/port/XauSessionMomentum_trades.txt /tmp/port/sess
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <algorithm>
struct Bar{int64_t ts;double o,h,l,c;};
static std::vector<Bar> load(const char*p){std::vector<Bar>v;FILE*f=fopen(p,"r");if(!f)return v;char l[256];bool fst=true;while(fgets(l,256,f)){if(fst){fst=false;if(l[0]<'0'||l[0]>'9')continue;}Bar b{};double t;if(sscanf(l,"%lf,%lf,%lf,%lf,%lf",&t,&b.o,&b.h,&b.l,&b.c)!=5)continue;b.ts=(int64_t)t;v.push_back(b);}fclose(f);return v;}
static inline int hr(int64_t ts){return (int)((ts/3600)%24);}
int main(){
 auto B=load("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv");int N=B.size();
 std::vector<double>e(N,0);double k=2.0/201;if(N)e[0]=B[0].c;for(int i=1;i<N;i++)e[i]=k*B[i].c+(1-k)*e[i-1]; // EMA200(h1)
 const double COST=1.0; // bp
 std::vector<std::pair<int64_t,double>> trades;
 double g=0,gw=0,gl=0;int n=0,win=0;
 // NYpm: enter 16:00, hold 4h ; overnight: enter 23:00, hold 5h ; both c>EMA200h1
 struct W{int hour,hold;}; W ws[]={{16,4},{23,5}};
 for(auto&w:ws){
  for(int i=24;i+w.hold<N;i++){
   if(hr(B[i].ts)!=w.hour)continue;
   if(B[i+w.hold].ts-B[i].ts!=(int64_t)w.hold*3600)continue; // contiguous, no gap
   if(!(B[i].c>e[i]))continue; // trend filter
   double pnl=(B[i+w.hold].c-B[i].o)/B[i].o*10000.0-COST;
   trades.push_back({B[i+w.hold].ts,pnl});
   g+=pnl;n++;if(pnl>0){win++;gw+=pnl;}else gl+=-pnl;
  }
 }
 std::sort(trades.begin(),trades.end());
 printf("XauSessionMomentum NYpm(16h L4)+overnight(23h L5) c>EMA200h1 cost=1bp  n=%d WR=%.1f PF=%.2f net=%+.1fbp\n",
   n,n?100.0*win/n:0,gl>0?gw/gl:99,g);
 if(getenv("PORT_DUMP")){FILE*pd=fopen(getenv("PORT_DUMP"),"w");for(auto&t:trades)fprintf(pd,"%lld,%.4f\n",(long long)t.first,t.second);fclose(pd);}
 return 0;}

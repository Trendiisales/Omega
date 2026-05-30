// =============================================================================
// xau_session_trendfilter.cpp -- de-risk the XAU overnight session edge (S42).
//
// The base overnight edge (xau_session_scan.cpp: long 23:00->04:00 UTC) dies at
// 4bp cost -- and Asian-session retail gold spread can be 2-4bp RT. This harness
// adds a TREND FILTER (only hold overnight when XAU is in an uptrend) to lift the
// cost-margin. Result: c>EMA200h1 filter survives to 5bp at 5/6 robust, 4bp at
// 6/6 (unfiltered = dead 3/6 at 4bp). H1 stays + at every cost level.
//
// Filters tested: none / c>close[-24h] / c>EMA200(h1, ~8 days) / both.
// Winner: c>EMA200h1 (highest PF, cleanest H1, n=367). Same fidelity as
// xau_session_scan (cross-spread cost in bps, WF50% + 6-block, long-only).
//
// BUILD: c++ -std=c++17 -O2 backtest/xau_session_trendfilter.cpp -o backtest/xau_session_trendfilter
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <vector>
#include <algorithm>
struct Bar{int64_t ts;double o,h,l,c;};
static std::vector<Bar> load(const char*p){std::vector<Bar>v;FILE*f=fopen(p,"r");if(!f)return v;char l[256];bool fst=true;while(fgets(l,256,f)){if(fst){fst=false;if(l[0]<'0'||l[0]>'9')continue;}Bar b{};double t;if(sscanf(l,"%lf,%lf,%lf,%lf,%lf",&t,&b.o,&b.h,&b.l,&b.c)!=5)continue;b.ts=(int64_t)t;v.push_back(b);}fclose(f);return v;}
static inline int hr(int64_t ts){return (int)((ts/3600)%24);}
int main(){
 auto B=load("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv");int N=B.size();
 int64_t TS0=B.front().ts,TS1=B.back().ts,SP=B[N/2].ts;
 auto blk=[&](int64_t ts){int b=(int)(6.0*(ts-TS0)/(double)(TS1-TS0+1));return b<0?0:(b>5?5:b);};
 std::vector<double>e(N,0);double k=2.0/201;e[0]=B[0].c;for(int i=1;i<N;i++)e[i]=k*B[i].c+(1-k)*e[i-1]; // EMA200(h1)~8 days
 struct F{const char*nm;int mode;};
 // mode 0 = no filter; 1 = c>close[-24h]; 2 = c>EMA200h1; 3 = both
 F fs[]={{"none",0},{"c>c[-24h]",1},{"c>EMA200h1",2},{"both",3}};
 for(auto&f:fs){printf("== A=23 L=5  filter=%s ==\n",f.nm);
  for(double c:{1.0,2.0,3.0,4.0,5.0}){double g=0,gw=0,gl=0,h1=0,h2=0;int n=0,win=0;double bg[6]={0};
   for(int i=24;i+5<N;i++){if(hr(B[i].ts)!=23)continue;if(B[i+5].ts-B[i].ts!=5*3600)continue;
    bool bull=true;
    if(f.mode==1)bull=B[i].c>B[i-24].c;
    else if(f.mode==2)bull=B[i].c>e[i];
    else if(f.mode==3)bull=(B[i].c>B[i-24].c)&&(B[i].c>e[i]);
    if(!bull)continue;
    double pnl=(B[i+5].c-B[i].o)/B[i].o*10000.0-c;g+=pnl;n++;if(pnl>0){win++;gw+=pnl;}else gl+=-pnl;(B[i].ts<SP?h1:h2)+=pnl;bg[blk(B[i].ts)]+=pnl;}
   int bp=0;for(int kk=0;kk<6;kk++)if(bg[kk]>0)bp++;
   printf("  %.0fbp: n=%-4d WR=%4.1f PF=%.2f g=%+7.1f H1=%+6.1f H2=%+6.1f blk+=%d/6 %s\n",
     c,n,n?100.0*win/n:0,gl>0?gw/gl:99,g,h1,h2,bp,(h1>0&&h2>0&&bp>=5)?"ROB":"");}
  printf("\n");}
 return 0;}

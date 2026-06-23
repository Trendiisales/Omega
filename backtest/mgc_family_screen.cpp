// mgc_family_screen.cpp -- SCREEN (not a real-engine BT) of intraday strategy
// FAMILIES on MGC 30m gold futures, to map which families revive on futures cost
// (they are dead on spot gold CFD per [[omega-intraday-spot-cfd-cost-wall]]).
// Winners must be re-confirmed on the REAL engine before any deploy (drive-real-
// engine rule). Cost in price points (MGC tick=0.10pt=$1; r/t ~0.2-0.4pt real).
// build: g++ -std=c++17 -O2 backtest/mgc_family_screen.cpp -o /tmp/mgcfam
// run:   /tmp/mgcfam data/mgc_30m_hist.csv <cost_pt>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <fstream>
struct B{double o,h,l,c,v;long long ts;};
static std::vector<B> load(const char*p){std::vector<B>v;std::ifstream f(p);std::string ln;bool first=true;
  while(std::getline(f,ln)){if(first){first=false;if(!isdigit((unsigned char)ln[0]))continue;}B b;long long ts;
    if(std::sscanf(ln.c_str(),"%lld,%lf,%lf,%lf,%lf,%lf",&ts,&b.o,&b.h,&b.l,&b.c,&b.v)>=5){b.ts=ts;v.push_back(b);}}return v;}
struct R{int n=0,w=0;double net=0,gw=0,gl=0,peak=0,cum=0,dd=0,n1=0,n2=0;
  void add(double p,bool h1){n++;net+=p;if(p>=0){w++;gw+=p;}else gl+=-p;cum+=p;if(cum>peak)peak=cum;if(peak-cum>dd)dd=peak-cum;(h1?n1:n2)+=p;}
  double pf()const{return gl>0?gw/gl:(gw>0?9.9:0);} };
static void rep(const char*name,const R&r){
  printf("  %-24s n=%-4d WR=%4.1f%% PF=%5.2f net=%+8.1f rDD=%5.2f | H1=%+7.1f H2=%+7.1f %s\n",
    name,r.n,r.n?100.0*r.w/r.n:0,r.pf(),r.net,r.dd>0?r.net/r.dd:0,r.n1,r.n2,
    (r.net>0&&r.n1>0&&r.n2>0)?"both-halves+ ✓":"FAIL");}
// runner exit: enter at bar i+1 open-ish (use close i), arm ATR-trail; for LONG
// exit when price closes below (peak - TRAIL*ATR) or hard SL (entry - SL*ATR).
static double runner(const std::vector<B>&b,int N,int i,int dir,double*atr,double SL,double TR,double C){
  double entry=b[i].c, peak=entry, ea=atr[i]; if(ea<=0)return 0;
  for(int j=i+1;j<N && j<i+200;j++){
    if(dir>0){ if(b[j].h>peak)peak=b[j].h;
      double trail=peak-TR*ea, hardsl=entry-SL*ea, stop=std::max(trail,hardsl);
      if(b[j].l<=stop){ double ex=std::min(b[j].o,stop); return (ex-entry)-C; } }
    else{ if(b[j].l<peak)peak=b[j].l;
      double trail=peak+TR*ea, hardsl=entry+SL*ea, stop=std::min(trail,hardsl);
      if(b[j].h>=stop){ double ex=std::max(b[j].o,stop); return (entry-ex)-C; } }
  }
  double last=b[N-1].c; return (dir>0?(last-entry):(entry-last))-C;
}
int main(int argc,char**argv){
  const char*path=argc>1?argv[1]:"data/mgc_30m_hist.csv"; double C=argc>2?atof(argv[2]):0.4;
  auto b=load(path); int N=b.size(); int mid=N/2; if(N<300){printf("few bars\n");return 1;}
  printf("[MGC-FAMILY-SCREEN] %d 30m bars  cost=%.2fpt  exit=ATR-trail RUNNER(SL2 TR2.5)  (SCREEN -- confirm winners on real engine)\n\n",N,C);
  std::vector<double> atr(N,0),ema(N,0); double a=0,e=b[0].c;
  for(int i=1;i<N;i++){double tr=std::max(b[i].h-b[i].l,std::max(fabs(b[i].h-b[i-1].c),fabs(b[i].l-b[i-1].c)));
    a=(a*13+tr)/14; atr[i]=a; e=e+(2.0/21)*(b[i].c-e); ema[i]=e;}
  const double SL=2.0,TR=2.5;
  R donch20l,donch40l,orbL,keltL,momC,emaCross;
  int look=20,cd=0;
  for(int i=41;i<N;i++){bool h1=i<mid; if(cd>0){cd--;}
    double hh=b[i-1].h,ll=b[i-1].l; for(int k=2;k<=look;k++){if(b[i-k].h>hh)hh=b[i-k].h; if(b[i-k].l<ll)ll=b[i-k].l;}
    double hh40=b[i-1].h; for(int k=2;k<=40;k++) if(b[i-k].h>hh40)hh40=b[i-k].h;
    if(b[i].c>hh && b[i-1].c<=hh){ donch20l.add(runner(b,N,i,1,atr.data(),SL,TR,C),h1); }
    if(b[i].c>hh40 && b[i-1].c<=hh40){ donch40l.add(runner(b,N,i,1,atr.data(),SL,TR,C),h1); }
    long long day=b[i].ts/86400, pday=b[i-1].ts/86400;
    if(day==pday){ int s=i; while(s>0 && b[s-1].ts/86400==day) s--; double orh=b[s].h;
      if(b[i].c>orh && b[i-1].c<=orh){ orbL.add(runner(b,N,i,1,atr.data(),SL,TR,C),h1); } }
    if(b[i].c>ema[i]+1.0*atr[i] && b[i-1].c<=ema[i-1]+1.0*atr[i-1]){ keltL.add(runner(b,N,i,1,atr.data(),SL,TR,C),h1); }
    double m3=b[i].c-b[i-3].c; if(m3>1.5*atr[i] && b[i-1].c-b[i-4].c<=1.5*atr[i-1]){ momC.add(runner(b,N,i,1,atr.data(),SL,TR,C),h1); }
    // EMA20 cross up (close crosses above ema)
    if(b[i].c>ema[i] && b[i-1].c<=ema[i-1]){ emaCross.add(runner(b,N,i,1,atr.data(),SL,TR,C),h1); }
  }
  printf("LONG breakout/trend families with ATR-trail runner exit:\n");
  rep("Donchian20-break",donch20l);
  rep("Donchian40-break",donch40l);
  rep("ORB-break",orbL);
  rep("Keltner-vol-break",keltL);
  rep("Momentum3-cont",momC);
  rep("EMA20-cross",emaCross);
  return 0;
}

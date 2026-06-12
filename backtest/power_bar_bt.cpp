// =============================================================================
// power_bar_bt.cpp -- Stephen "power bar" setup backtest (NAS m5, both ways)
// -----------------------------------------------------------------------------
// Setup: a bar LARGER than surrounding bars, starting AT/NEAR the 20-SMA anchor,
// moving in the anchor's direction, that CLEARS recent support/resistance.
//   long  : SMA20 rising, bullish power bar touches SMA, breaks prior swing high.
//   short : SMA20 falling, bearish power bar touches SMA, breaks prior swing low.
//   entry = power-bar close; stop = opposite extreme of the power bar.
//   skip after >=6 consecutive same-direction pushes (climax).
// Exit variants compared:
//   FIX2R : take-profit = 2x risk (the literal rule)
//   TRAIL : no TP, chandelier ATR-trail off the extreme (runner)
//   BE1R  : move stop to breakeven once +1R, else 2R TP (the "can't lose" rule)
// cost-incl, long/short split, walk-forward halves, 2022-bear OOS.
// Build: clang++ -O3 -std=c++17 -o backtest/power_bar_bt backtest/power_bar_bt.cpp
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

struct Bar{int64_t ts;double o,h,l,c;};
static std::vector<Bar> load(const char*p){std::vector<Bar> v;std::ifstream f(p);if(!f.is_open()){std::fprintf(stderr,"  no %s\n",p);return v;}std::string ln;
  while(std::getline(f,ln)){if(ln.empty()||!std::isdigit((unsigned char)ln[0]))continue;Bar b;char*q=(char*)ln.c_str();char*e;
    b.ts=std::strtoll(q,&e,10);if(*e!=',')continue;q=e+1;b.o=std::strtod(q,&e);if(*e!=',')continue;q=e+1;b.h=std::strtod(q,&e);if(*e!=',')continue;q=e+1;b.l=std::strtod(q,&e);if(*e!=',')continue;q=e+1;b.c=std::strtod(q,&e);
    if(b.o>0&&b.h>0&&b.l>0&&b.c>0)v.push_back(b);} return v;}

enum Exit{FIX2R,TRAIL,BE1R};
struct P{
  int sma_n=20, avg_n=10, slope_lb=5, sr_lb=12;
  double body_k=1.6;       // power bar range >= body_k * avg range
  double body_frac=0.55;   // close in this fraction of range (directional close)
  double anchor_tol=0.6;   // bar must touch within anchor_tol*avgrange of SMA20
  int    push_cap=6;       // skip after this many consecutive same-dir power bars
  double tp_r=2.0;
  double trail_atr=2.0;
  int    max_hold=24;      // m5 bars (~2h)
  double cost=2.0;         // NAS RT pts
  Exit   ex=FIX2R;
  bool   longs=true, shorts=true;
  double max_risk_atr=0.0; // 0=off; else skip if bar range > this*ATR (Peachy maxStop selectivity)
};
struct St{int n=0,w=0,l=0;double net=0,gw=0,gl=0,dd=0;
  double pf()const{return gl>1e-9?gw/gl:(gw>0?999:0);} double wr()const{return n?100.0*w/n:0;}};

static void run(const std::vector<Bar>&B,const P&pp,St&L,St&S){
  int n=(int)B.size(); if(n<200)return;
  std::vector<double> sma(n,0),avgr(n,0),atr(n,0);
  double ss=0; for(int i=0;i<n;i++){ss+=B[i].c; if(i>=pp.sma_n)ss-=B[i-pp.sma_n].c; sma[i]= i>=pp.sma_n-1? ss/pp.sma_n:0;}
  double rs=0; for(int i=0;i<n;i++){double r=B[i].h-B[i].l; rs+=r; if(i>=pp.avg_n)rs-=(B[i-pp.avg_n].h-B[i-pp.avg_n].l); avgr[i]= i>=pp.avg_n-1? rs/pp.avg_n:0;}
  double ts2=0; for(int i=1;i<n;i++){double tr=std::max(B[i].h-B[i].l,std::max(std::fabs(B[i].h-B[i-1].c),std::fabs(B[i].l-B[i-1].c)));ts2+=tr;if(i>14)ts2-=0;atr[i]= ts2/std::min(i,14);} // rough
  // proper ATR14
  { double a=0; std::vector<double> tr(n,0); for(int i=1;i<n;i++)tr[i]=std::max(B[i].h-B[i].l,std::max(std::fabs(B[i].h-B[i-1].c),std::fabs(B[i].l-B[i-1].c)));
    double sum=0; for(int i=1;i<n;i++){sum+=tr[i]; if(i>14)sum-=tr[i-14]; atr[i]= sum/std::min(i,14);} }

  int push=0,push_dir=0;
  bool inpos=false;int dir=0,entry_i=0;double entry=0,stop=0,tp=0,hh=0,ll=0;bool be=false;
  St* side=nullptr; double cumL=0,pkL=0,cumS=0,pkS=0;
  int start=std::max({pp.sma_n,pp.avg_n,pp.sr_lb,16})+2;
  for(int i=start;i<n;i++){
    bool eod = (B[i].ts - B[i-1].ts) > 7200; // session gap -> flatten any pos at prior close
    if(inpos && eod){ double px=B[i-1].c; double pnl=(dir>0?(px-entry):(entry-px))-pp.cost; side->n++; side->net+=pnl; if(pnl>0){side->w++;side->gw+=pnl;}else{side->l++;side->gl+=-pnl;} inpos=false; }
    if(inpos){
      double A=atr[i]>0?atr[i]:(stop>0?std::fabs(entry-stop):1);
      if(dir>0){ if(B[i].h>hh)hh=B[i].h; }else{ if(B[i].l<ll)ll=B[i].l; }
      double exitpx=0;bool out=false;
      // stop / be
      double eff=stop;
      if(pp.ex==TRAIL){ double t= dir>0? hh-pp.trail_atr*A : ll+pp.trail_atr*A; eff = dir>0? std::max(stop,t):std::min(stop,t); }
      if(pp.ex==BE1R && be){ eff = entry + (dir>0? pp.cost: -pp.cost); }
      if(dir>0){
        if(B[i].l<=eff){exitpx=eff;out=true;}
        else if(pp.ex!=TRAIL && B[i].h>=tp){exitpx=tp;out=true;}
      }else{
        if(B[i].h>=eff){exitpx=eff;out=true;}
        else if(pp.ex!=TRAIL && B[i].l<=tp){exitpx=tp;out=true;}
      }
      if(!out && pp.ex==BE1R && !be){ double r=std::fabs(entry-stop); if(dir>0? B[i].h>=entry+r : B[i].l<=entry-r) be=true; }
      if(!out && i-entry_i>=pp.max_hold){exitpx=B[i].c;out=true;}
      if(out){ double pnl=(dir>0?(exitpx-entry):(entry-exitpx))-pp.cost; side->n++; side->net+=pnl;
        if(pnl>0){side->w++;side->gw+=pnl;}else{side->l++;side->gl+=-pnl;}
        double&cum=dir>0?cumL:cumS;double&pk=dir>0?pkL:pkS;cum+=pnl;if(cum>pk)pk=cum;if(pk-cum>side->dd)side->dd=pk-cum;
        inpos=false; }
      if(inpos)continue;
    }
    // detect power bar
    double A=avgr[i]; if(A<=0)continue;
    double rng=B[i].h-B[i].l; if(rng < pp.body_k*A) {  } // not power-sized -> may reset push below
    bool big = rng >= pp.body_k*A;
    double slope = sma[i]-sma[i-pp.slope_lb];
    bool bull = B[i].c>B[i].o && (B[i].c-B[i].l)>=pp.body_frac*rng;
    bool bear = B[i].c<B[i].o && (B[i].h-B[i].c)>=pp.body_frac*rng;
    // anchor touch
    double dmin = std::min(std::fabs(B[i].o-sma[i]), std::min(std::fabs(B[i].l-sma[i]),std::fabs(B[i].h-sma[i])));
    bool touch = dmin <= pp.anchor_tol*A;
    // S/R clear
    double prevHi=0,prevLo=1e18; for(int k=i-pp.sr_lb;k<i;k++){if(B[k].h>prevHi)prevHi=B[k].h; if(B[k].l<prevLo)prevLo=B[k].l;}
    bool risk_ok = pp.max_risk_atr<=0 || (atr[i]>0 && rng <= pp.max_risk_atr*atr[i]);
    bool longsig = big&&bull&&touch&&slope>0&&B[i].c>prevHi&&risk_ok;
    bool shortsig= big&&bear&&touch&&slope<0&&B[i].c<prevLo&&risk_ok;
    // push tracking (consecutive big directional bars same dir)
    int thisdir = big&&bull?1:(big&&bear?-1:0);
    if(thisdir!=0 && thisdir==push_dir) push++; else if(thisdir!=0){push=1;push_dir=thisdir;} else {push=0;push_dir=0;}
    if(push>=pp.push_cap){ continue; } // climax -> skip
    if(!inpos){
      if(longsig&&pp.longs){ dir=1;entry=B[i].c;stop=B[i].l;double r=entry-stop;if(r<=0)continue;tp=entry+pp.tp_r*r;hh=B[i].h;be=false;entry_i=i;inpos=true;side=&L; }
      else if(shortsig&&pp.shorts){ dir=-1;entry=B[i].c;stop=B[i].h;double r=stop-entry;if(r<=0)continue;tp=entry-pp.tp_r*r;ll=B[i].l;be=false;entry_i=i;inpos=true;side=&S; }
    }
  }
}

static St merge(const St&a,const St&b){St r;r.n=a.n+b.n;r.w=a.w+b.w;r.l=a.l+b.l;r.net=a.net+b.net;r.gw=a.gw+b.gw;r.gl=a.gl+b.gl;r.dd=std::max(a.dd,b.dd);return r;}
static void row(const char*nm,const St&s){std::printf("%-22s %5d %6.1f %+9.1f %6.2f %8.1f\n",nm,s.n,s.wr(),s.net,s.pf(),s.dd);}

static void runfile(const char*nm,const char*path,P pp,int half){
  auto B=load(path); if(B.empty())return; int n=(int)B.size(),lo=0,hi=n;
  if(half==1){hi=n/2;}else if(half==2){lo=n/2;}
  std::vector<Bar> sub(B.begin()+lo,B.begin()+hi);
  St L,S; run(sub,pp,L,S);
  char b[40]; std::snprintf(b,sizeof(b),"%s.L",nm); row(b,L);
  std::snprintf(b,sizeof(b),"%s.S",nm); row(b,S);
  St all=merge(L,S); std::snprintf(b,sizeof(b),"%s.ALL",nm); row(b,all);
}

int main(int argc,char**argv){
  const char* main_csv="/Users/jo/Tick/NSXUSD_merged.m5.csv";
  const char* bear_csv="/Users/jo/Tick/NAS2022_bear_m5.csv";
  std::printf("=== POWER-BAR backtest (NAS m5, cost 2pt RT) ===\n");
  std::printf("power bar: range>=1.6xavg, touches SMA20, anchor-dir, clears S/R; skip>=6 pushes\n\n");

  const char* exn[]={"FIX2R","TRAIL","BE1R"};
  for(int e=0;e<3;e++){
    P pp; pp.ex=(Exit)e;
    std::printf("---------- EXIT=%s ----------\n",exn[e]);
    std::printf("%-22s %5s %6s %9s %6s %8s\n","series","N","WR%","net","PF","maxDD");
    runfile("NAS24-26",main_csv,pp,0);
    runfile(" h1",main_csv,pp,1);
    runfile(" h2",main_csv,pp,2);
    runfile("NAS2022bear",bear_csv,pp,0);
    std::printf("\n");
  }

  // ---- SELECTIVITY pass (Peachy lever): FIX2R, vary body_k + max_risk_atr ----
  std::printf("========== SELECTIVITY SWEEP (FIX2R, both-halves+bear) ==========\n");
  std::printf("%-30s %5s %6s %9s %6s | h1PF h2PF bearPF  ROBUST\n","cfg(bodyK/maxRiskATR/side)","N","WR%","net","PF");
  double BK[]={1.6,2.2,3.0}; double MR[]={0.0,1.5,2.5}; int SIDE[]={0,1}; // 0=long-only,1=both
  for(double bk:BK)for(double mr:MR)for(int sd:SIDE){
    P pp; pp.ex=FIX2R; pp.body_k=bk; pp.max_risk_atr=mr; pp.shorts=(sd==1);
    auto agg=[&](const char*path,int half)->St{ auto B=load(path); int n=(int)B.size(),lo=0,hi=n; if(half==1)hi=n/2; else if(half==2)lo=n/2; std::vector<Bar> s(B.begin()+lo,B.begin()+hi); St L,S; run(s,pp,L,S); return merge(L,S);};
    St full=agg(main_csv,0),h1=agg(main_csv,1),h2=agg(main_csv,2),bear=agg(bear_csv,0);
    bool robust = h1.net>0 && h2.net>0 && bear.net>0 && full.n>=40;
    char lab[48]; std::snprintf(lab,sizeof(lab),"bk%.1f/mr%.1f/%s",bk,mr,sd?"both":"long");
    std::printf("%-30s %5d %6.1f %+9.1f %6.2f | %4.2f %4.2f %5.2f  %s\n",lab,full.n,full.wr(),full.net,full.pf(),h1.pf(),h2.pf(),bear.pf(),robust?"YES":"-");
  }
  return 0;
}

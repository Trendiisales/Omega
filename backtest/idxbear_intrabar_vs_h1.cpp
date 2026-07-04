// =============================================================================
// idxbear_intrabar_vs_h1.cpp -- does an INTRA-BAR exit beat the current
//   H1-BAR-CLOSE-only exit for g_idx_bear_short (IndexBearShortEngine)?
// -----------------------------------------------------------------------------
// FAITHFUL to include/IndexBearShortEngine.hpp _on_bar_close (entry) + _manage
// (exit). Entries + params are IDENTICAL between the two modes; only the exit
// CHECK CADENCE varies:
//
//   H1-CLOSE  (current live): SL/TP tested ONLY at H1 close, against the
//     just-completed H1 bar's high/low, with the engine's check order
//     (SL_HIT if curH>=stop  ->  else TP_HIT if curL<=tp  ->  else TIME_STOP).
//     This is exactly _manage()  (IndexBearShortEngine.hpp:207-231).
//
//   INTRA-BAR (proposed): SAME SL/TP levels tested against the finer m5 series
//     within the exit hour, chronologically. SHORT: stop touched if m5 HIGH>=sl
//     -> exit sl (or m5 OPEN on a gap-up-through, a WORSE fill); TP touched if
//     m5 LOW<=tp -> exit tp (or m5 OPEN on a gap-down-through, a BETTER fill).
//
// Because an H1 bar's high/low is the max/min of its m5 sub-bars, BOTH modes
// necessarily exit on the SAME H1 hour (first hour any level enters H1 range),
// so the entry stream is provably identical. We run ONE canonical lifecycle
// (H1-close, drives the state machine) and dual-account the exit hour.
//
// Entry/param faithfulness vs the engine (file:line in IndexBearShortEngine.hpp):
//   bear_regime  L179 : c<emaS && emaS<emaS[-PERSIST] && ema<emaS
//   down-momo    L183 : c<ema && ema<ema[-5]
//   Donchian     L186-187 : c < min(low over prior DON bars)
//   swing stop   L190-192 : max(high over last 4 bars); stop = sh>entry? sh : entry+SL_ATR*ATR
//   tp           L194 : entry - TP_R*risk        (TP_R=2.0 fixed)
//   manage order L214-216 : SL first, then TP, then TIME_STOP
//   cost         L219 : pnl = (entry-exit) - COST_PTS   (RAW pts; *lot; ledger *tick_value)
//   ATR_N=24 EMA_FAST=50 EMA_SLOW=200 PERSIST=100 SL_ATR=2.0 TP_R=2.0 MAX_HOLD=240 COOLDOWN=12
//   NAS100 instance: DON=24 COST_PTS=2.0 mult(NAS100)=1
//   US500  instance: DON=48 COST_PTS=0.6 mult(US500)=50 (economic; see report caveat)
//
// Build: clang++ -O3 -std=c++17 -o backtest/idxbear_intrabar_vs_h1 backtest/idxbear_intrabar_vs_h1.cpp
// Run:   ./backtest/idxbear_intrabar_vs_h1 <h1.csv> <m5.csv> <cost> <don> <mult> <label>
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
#include <unordered_map>

struct Bar{int64_t ts;double o,h,l,c;};
static std::vector<Bar> load(const char*p){std::vector<Bar> v;std::ifstream f(p);if(!f.is_open()){std::fprintf(stderr,"  no %s\n",p);return v;}std::string ln;
  while(std::getline(f,ln)){if(ln.empty()||!std::isdigit((unsigned char)ln[0]))continue;Bar b;char*q=(char*)ln.c_str();char*e;
    b.ts=std::strtoll(q,&e,10);if(*e!=',')continue;q=e+1;b.o=std::strtod(q,&e);if(*e!=',')continue;q=e+1;b.h=std::strtod(q,&e);if(*e!=',')continue;q=e+1;b.l=std::strtod(q,&e);if(*e!=',')continue;q=e+1;b.c=std::strtod(q,&e);
    if(b.o>0&&b.h>0&&b.l>0&&b.c>0)v.push_back(b);} return v;}

// engine params (baked to the validated cfg)
struct P{ int atr_n=24, don=48, ema_n=50, ema_slow=200, persist=100, max_hold=240, cooldown=12;
          double sl_atr=2.0, tp_r=2.0, cost=1.5; };

struct Trade{ int64_t ts; double pnl; char why; };  // why: 'S'top 'T'p 'I'time
struct Acct{ std::vector<Trade> tr;
  double net()const{double s=0;for(auto&t:tr)s+=t.pnl;return s;}
  int n()const{return (int)tr.size();}
  int wins()const{int w=0;for(auto&t:tr)if(t.pnl>0)w++;return w;}
  double worst()const{double m=0;bool f=true;for(auto&t:tr){if(f||t.pnl<m){m=t.pnl;f=false;}}return m;}
  double gw()const{double s=0;for(auto&t:tr)if(t.pnl>0)s+=t.pnl;return s;}
  double gl()const{double s=0;for(auto&t:tr)if(t.pnl<0)s-=t.pnl;return s;}
  double pf()const{double g=gl();return g>1e-9?gw()/g:(gw()>0?999:0);}
  double maxdd()const{double cum=0,pk=0,dd=0;for(auto&t:tr){cum+=t.pnl;if(cum>pk)pk=cum;if(pk-cum>dd)dd=pk-cum;}return dd;}
};

int main(int argc,char**argv){
  if(argc<7){std::fprintf(stderr,"usage: %s h1 m5 cost don mult label\n",argv[0]);return 1;}
  P pp; pp.cost=atof(argv[3]); pp.don=atoi(argv[4]);
  double mult=atof(argv[5]); const char*label=argv[6];
  auto B=load(argv[1]); auto M=load(argv[2]);
  if(B.size()<300){std::fprintf(stderr,"too few H1 bars (%zu)\n",B.size());return 1;}

  // index m5 bars by H1 bucket start
  std::unordered_map<int64_t,std::vector<int>> h1map; h1map.reserve(M.size());
  for(int j=0;j<(int)M.size();++j){ int64_t hb=(M[j].ts/3600)*3600; h1map[hb].push_back(j); }
  // (M is already ts-sorted, so each bucket's index list is chronological)

  int n=(int)B.size();
  std::vector<double> atr(n,0),ema(n,0),emaS(n,0),tr(n,0);
  for(int i=1;i<n;i++)tr[i]=std::max(B[i].h-B[i].l,std::max(std::fabs(B[i].h-B[i-1].c),std::fabs(B[i].l-B[i-1].c)));
  double s=0; for(int i=1;i<n;i++){s+=tr[i];if(i>pp.atr_n)s-=tr[i-pp.atr_n];atr[i]=s/std::min(i,pp.atr_n);}
  double k=2.0/(pp.ema_n+1); ema[0]=B[0].c; for(int i=1;i<n;i++)ema[i]=ema[i-1]+k*(B[i].c-ema[i-1]);
  double ks=2.0/(pp.ema_slow+1); emaS[0]=B[0].c; for(int i=1;i<n;i++)emaS[i]=emaS[i-1]+ks*(B[i].c-emaS[i-1]);

  Acct h1acc, ibacc;
  int whipsaw=0, gap_worse=0, nomatch=0;
  int exit_both_in_range=0, hold_both_in_range=0, tp_gap_better=0;
  bool inpos=false; double entry=0,stop=0,tp=0,ll=0; int ei=0; int last_exit=-100000;
  int start=std::max({pp.atr_n,pp.don,pp.ema_n,pp.persist})+2;

  for(int i=start;i<n;i++){
    double A=atr[i]; if(A<=0)continue;
    if(inpos){
      if(B[i].l<ll)ll=B[i].l;
      // DIAGNOSTIC: geometric whipsaw-precondition -- does THIS H1 bar's range
      // contain BOTH the stop and the tp? (only then can intra-bar resolution
      // flip the outcome). Counted over every in-position hour.
      if(B[i].h>=stop && B[i].l<=tp) hold_both_in_range++;
      // ---- H1-CLOSE exit (engine _manage order): SL, then TP, then TIME ----
      double ex_h1=0; char why_h1=0;
      if(B[i].h>=stop){ ex_h1=stop; why_h1='S'; }
      else if(B[i].l<=tp){ ex_h1=tp; why_h1='T'; }
      else if(i-ei>=pp.max_hold){ ex_h1=B[i].c; why_h1='I'; }
      if(!why_h1){ continue; }   // not an exit hour -> keep holding (both modes agree)

      // ---- INTRA-BAR exit over this hour's m5 sub-bars, chronological ----
      double ex_ib=ex_h1; char why_ib=why_h1;   // default = H1 result (covers TIME + no-m5)
      if(why_h1!='I'){
        auto it=h1map.find(B[i].ts);
        bool resolved=false;
        if(it!=h1map.end()){
          for(int idx:it->second){ const Bar&m=M[idx];
            // gap-through at bar open (SHORT): open already past a level
            if(m.o>=stop){ ex_ib=m.o; why_ib='S'; resolved=true; break; }   // worse than stop
            if(m.o<=tp){   ex_ib=m.o; why_ib='T'; resolved=true; break; }   // better than tp
            bool hs=m.h>=stop, ht=m.l<=tp;
            if(hs&&ht){ ex_ib=stop; why_ib='S'; resolved=true; break; }     // both in one m5: pessimistic
            if(hs){ ex_ib=stop; why_ib='S'; resolved=true; break; }
            if(ht){ ex_ib=tp;   why_ib='T'; resolved=true; break; }
          }
        }
        if(!resolved){ nomatch++; ex_ib=ex_h1; why_ib=why_h1; } // no m5 coverage -> fall back
      }

      double pnl_h1=(entry-ex_h1)-pp.cost;
      double pnl_ib=(entry-ex_ib)-pp.cost;
      h1acc.tr.push_back({B[i].ts,pnl_h1,why_h1});
      ibacc.tr.push_back({B[i].ts,pnl_ib,why_ib});
      if(why_h1=='S' && B[i].l<=tp) exit_both_in_range++;  // exit hour had both levels in range
      if(why_ib!=why_h1) whipsaw++;                       // reason flipped (SL<->TP)
      if(why_ib==why_h1 && ex_ib>ex_h1+1e-6 && why_ib=='S') gap_worse++; // worse gap fill same reason
      if(why_ib=='T' && ex_ib<ex_h1-1e-6) tp_gap_better++; // TP gap-through -> better fill
      inpos=false; last_exit=i;
      continue;
    }
    // ---------- ENTRY (faithful to _on_bar_close) ----------
    if(i-last_exit<pp.cooldown)continue;
    bool bear_regime = B[i].c<emaS[i] && emaS[i]<emaS[i-pp.persist] && ema[i]<emaS[i];
    if(!bear_regime)continue;
    bool down = B[i].c<ema[i] && ema[i]<ema[i-5];
    if(!down)continue;
    double lo=1e18; for(int kk=i-pp.don;kk<i;kk++) if(B[kk].l<lo)lo=B[kk].l;
    if(B[i].c<lo){
      entry=B[i].c; double sh=B[i].h; for(int kk=i-1;kk>=i-3&&kk>=0;kk--) if(B[kk].h>sh)sh=B[kk].h;
      stop = sh>entry? sh : entry+pp.sl_atr*A; double risk=stop-entry; if(risk<=0)continue;
      tp = entry-pp.tp_r*risk; ll=B[i].l; ei=i; inpos=true;
    }
  }

  auto row=[&](const char*nm,const Acct&a){
    std::printf("%-10s n=%-4d WR=%5.1f%%  net=%+9.1fpt (%+12.1f USD)  PF=%4.2f  worst=%+8.1fpt (%+11.1f USD)  maxDD=%8.1fpt (%11.1f USD)\n",
      nm,a.n(), a.n()?100.0*a.wins()/a.n():0.0, a.net(), a.net()*mult, a.pf(), a.worst(), a.worst()*mult, a.maxdd(), a.maxdd()*mult);
  };
  std::printf("\n=== %s  (mult=%.0f, cost=%.2fpt/RT, DON=%d, TP_R=%.1f) ===\n",label,mult,pp.cost,pp.don,pp.tp_r);
  std::printf("H1 bars=%d  m5 bars=%zu  m5-coverage-miss=%d\n",n,M.size(),nomatch);
  row("H1-CLOSE ",h1acc);
  row("INTRA-BAR",ibacc);
  double dpt=ibacc.net()-h1acc.net(); double dusd=dpt*mult;
  double base=std::fabs(h1acc.net())>1e-9?h1acc.net():1.0;
  std::printf("DELTA (intrabar - h1close): %+.1fpt  %+.1f USD  (%+.1f%% of H1-close net)\n",dpt,dusd,100.0*dpt/base);
  std::printf("whipsaw (exit reason flipped SL<->TP): %d   worse-gap-fills(stop): %d   better-gap-fills(tp): %d\n",whipsaw,gap_worse,tp_gap_better);
  std::printf("geometric precondition: in-position hours whose H1 range spans BOTH stop&tp: hold=%d exit-hours=%d  (>=3R bar needed; if 0, intra-bar CANNOT diverge)\n",hold_both_in_range,exit_both_in_range);
  // per-half (chronological split of the SAME trade list)
  auto half=[&](const Acct&a,int h){Acct o;int m=a.n();int lo=h==1?0:m/2,hi=h==1?m/2:m;for(int i=lo;i<hi;i++)o.tr.push_back(a.tr[i]);return o;};
  std::printf("  H1-CLOSE  H1/H2 net: %+.1f / %+.1f pt\n",half(h1acc,1).net(),half(h1acc,2).net());
  std::printf("  INTRA-BAR H1/H2 net: %+.1f / %+.1f pt\n",half(ibacc,1).net(),half(ibacc,2).net());
  return 0;
}

// connors_intraday_stop_probe.cpp — DAY-CLOSE (faithful deployed config) vs an
// INTRA-DAY protective-stop variant, on NDX daily OHLC.
//
// Purpose: confirm/refute that ConnorsRSI2Engine's day-close-only exit is the
// correct/faithful design, vs. adding an intra-day protective stop (checked
// against the daily LOW while a position is open). Connors MR systems classically
// get HURT by stops — test empirically.
//
// The DAY-CLOSE path is a faithful replica of ConnorsRSI2Engine's deployed config
// (validated to match the real-engine harness connors_regime_gate_audit.cpp):
//   TREND_SMA=200 RSI_IN=10 RSI_LEN=2 SHORT_SMA=5 MAXHOLD=10 SCALEIN MAX_UNITS=2
//   REGIME_GATE=1 (asym sustained-bear veto) BEAR_VETO_K=20  ENTRY_MODE=0 (RSI2)
// Replicates engine ordering: push OHLC -> update bear_below -> exit/scale -> entry.
//
// build: clang++ -std=c++17 -O2 connors_intraday_stop_probe.cpp -o /tmp/connors_stop
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <ctime>

struct Bar{int64_t ts;double o,h,l,c;};
static std::vector<Bar> load(const char*p){std::vector<Bar>v;std::ifstream f(p);std::string l;long long ts;double o,h,l2,c;
 while(std::getline(f,l)){ if(std::sscanf(l.c_str(),"%lld,%lf,%lf,%lf,%lf",&ts,&o,&h,&l2,&c)==5 && c>0) v.push_back({ts,o,h,l2,c}); } return v;}
static int yr(int64_t s){std::time_t t=s;std::tm g{};gmtime_r(&t,&g);return g.tm_year+1900;}

static double smaC(const std::vector<Bar>&B,int i,int n){double s=0;for(int k=i-n+1;k<=i;k++)s+=B[k].c;return s/n;}
static double rsi(const std::vector<Bar>&B,int i,int n){ if(i<n)return 50.0; double g=0,l=0;
 for(int k=i-n+1;k<=i;k++){double ch=B[k].c-B[k-1].c; if(ch>0)g+=ch; else l+=-ch;} return l==0?100.0:100.0-100.0/(1.0+g/l);}
// Wilder-ish simple ATR over last n true ranges ending at bar i.
static double atr(const std::vector<Bar>&B,int i,int n){ if(i<n)return 0; double s=0;
 for(int k=i-n+1;k<=i;k++){ double tr=B[k].h-B[k].l; double a=std::fabs(B[k].h-B[k-1].c),b=std::fabs(B[k].l-B[k-1].c);
   if(a>tr)tr=a; if(b>tr)tr=b; s+=tr;} return s/n;}

struct Trade{int64_t ets;double pnl;double size;};

// stop_k<=0 => pure DAY-CLOSE (faithful). stop_k>0 => intra-day stop at entry-avg - k*ATR14(entry).
static std::vector<Trade> run(const std::vector<Bar>&B,double cost_pt,double stop_k){
 const int TREND=200,RSI_LEN=2,SHORT_SMA=5,MAXHOLD=10,BEAR_VETO_K=20,MAX_UNITS=2; const double RSI_IN=10.0;
 const bool SCALEIN=true;
 std::vector<Trade> out;
 bool act=false; double entry=0,units=0; int64_t ets=0; int held=0; int bear_below=0; double atr_entry=0;
 for(int i=0;i<(int)B.size();++i){
   const double close=B[i].c;
   // update bear_below (needs 205 closes)
   if(i>=TREND+5-1){ double s0=smaC(B,i,TREND); double s5=smaC(B,i-5,TREND);
     if(close<s0 && s0<s5) bear_below++; else bear_below=0; }
   else bear_below=0;

   // ---- INTRA-DAY STOP (variant only): check the day's LOW while holding ----
   // Applies from the day AFTER entry (held>=1). On entry day, entered at close, no intraday left.
   if(act && stop_k>0 && held>=1){
     double stop_px=entry - stop_k*atr_entry;
     if(B[i].l<=stop_px){ // stop breached intraday
       double fill = (B[i].o<=stop_px)? B[i].o : stop_px;    // gap-through => fill at open
       double pnl=(fill-entry)*units - cost_pt*units;
       out.push_back({ets,pnl,units}); act=false;
     }
   }

   // ---- DAY-CLOSE manage/exit (faithful) ----
   if(act){
     held++;
     double s5=smaC(B,i,SHORT_SMA);
     bool exit = (close>s5) || (held>=MAXHOLD);
     if(exit){ double pnl=(close-entry)*units - cost_pt*units; out.push_back({ets,pnl,units}); act=false; }
     else if(SCALEIN && units<MAX_UNITS && i>=TREND){
       double s0=smaC(B,i,TREND); bool regime_ok=(bear_below<BEAR_VETO_K);
       if(regime_ok && rsi(B,i,RSI_LEN)<RSI_IN){ entry=(entry*units+close)/(units+1); units+=1; }
     }
   }
   // ---- entry (faithful: same-day re-entry allowed after an exit, matching engine) ----
   if(!act && i>=TREND){
     double s0=smaC(B,i,TREND); bool regime_ok=(bear_below<BEAR_VETO_K);
     if(regime_ok && rsi(B,i,RSI_LEN)<RSI_IN){ act=true; entry=close; ets=B[i].ts; held=0; units=1; atr_entry=atr(B,i,14); }
   }
 }
 return out;
}

struct Stat{int n=0,w=0;double net=0,gp=0,gl=0,worst=0,peak=0,eq=0,dd=0,y22=0;};
static void rec(Stat&s,double pnl,int64_t ets){ s.n++; if(pnl>0){s.w++;s.gp+=pnl;}else s.gl+=-pnl; s.net+=pnl;
 if(pnl<s.worst)s.worst=pnl; s.eq+=pnl; if(s.eq>s.peak)s.peak=s.eq; double d=s.peak-s.eq; if(d>s.dd)s.dd=d; if(yr(ets)==2022)s.y22+=pnl;}

static void report(const char*tag,const std::vector<Trade>&tr,double lot,double mult){
 Stat all,h1,h2; int total=(int)tr.size();
 for(int i=0;i<total;i++){ rec(all,tr[i].pnl,tr[i].ets); if(i<total/2)rec(h1,tr[i].pnl,tr[i].ets); else rec(h2,tr[i].pnl,tr[i].ets);}
 double k=lot*mult; // pt -> USD (size in trades is at lot=1; deployed lot & index mult applied here)
 double pf=all.gl>0?all.gp/all.gl:(all.gp>0?999:0), wr=all.n?100.0*all.w/all.n:0;
 printf("%-30s n=%-4d WR=%4.1f%% PF=%5.2f  net=$%+9.0f  worst=$%+8.0f  maxDD=$%8.0f  2022=$%+8.0f  H1/H2=%s\n",
   tag, all.n, wr, pf, all.net*k, all.worst*k, all.dd*k, all.y22*k, (h1.net>0&&h2.net>0)?"BOTH+":"NOT+");
}

int main(int argc,char**argv){
 std::string path=argc>1?argv[1]:"/Users/jo/Tick/NDX_daily_2016_2026.csv";
 double cost=argc>2?atof(argv[2]):8.0;
 double lot=argc>3?atof(argv[3]):3.0;   // deployed lot=3.0
 double mult=1.0;                        // NAS100 tick_value_multiplier = 1 (registry)
 auto B=load(path.c_str());
 printf("Loaded %zu NDX daily bars | cost=%.1fpt r/t | lot=%.1f mult=%.1f (USD figures)\n\n",B.size(),cost,lot,mult);
 // Validation line: lot=1, so pt==USD, must match connors_regime_gate_audit REGIME_GATE=1.
 report("VALIDATE(lot1)=DAY-CLOSE", run(B,cost,0.0), 1.0, 1.0);
 printf("\n-- DEPLOYED lot=%.1f, USD --\n",lot);
 report("DAY-CLOSE (faithful/live)", run(B,cost,0.0), lot, mult);
 report("+ intraday stop 1.0xATR",   run(B,cost,1.0), lot, mult);
 report("+ intraday stop 1.5xATR",   run(B,cost,1.5), lot, mult);
 report("+ intraday stop 2.0xATR",   run(B,cost,2.0), lot, mult);
 report("+ intraday stop 3.0xATR",   run(B,cost,3.0), lot, mult);
 return 0;}

// connors_lever_sweep.cpp — ⚠️⚠️ UNFAITHFUL — DO NOT TRUST ITS LEVER NUMBERS ⚠️⚠️
// ============================================================================
// STATUS (S-2026-07-17h): this replica DOES NOT match the real engine. Its own
// `--validate` proves it: replica @ NAS gate1 flat-8pt fires n=235 (net=29983,
// PF2.87), but the FAITHFUL real-engine harness connors_regime_gate_audit.cpp
// fires n=142 (net=25456, PF4.43) on the same config. It OVER-TRADES by 93 —
// so every lever/stop result this file produces is computed on a wrong trade
// population and is GARBAGE. The "VALIDATED / n=142" claim in the old banner
// below was FALSE; this header supersedes it.
//
// RULE: if you EVER want to sweep a stop/leverage on ConnorsRSI2, do it by
// porting the L1..L4 lever logic INTO connors_regime_gate_audit.cpp (that one
// drives the real engine). Do NOT resurrect this file's numbers. Kept only as
// a reference for the lever definitions below, NOT for its results.
// ============================================================================
//
// (ORIGINAL — now-disproven — banner: claimed a FAITHFUL daily-close replica of
// the ConnorsRSI2 family sweeping 4 adverse-protection LEVERS in ONE pass, with
// close-to-close fills and same-day re-entry, "VALIDATED against the real-engine
// harness connors_regime_gate_audit.cpp (NAS gate1 @ flat 8pt: n=142
// net=25456pt) via --validate." The --validate delta above shows it is not.)
//
// LEVERS (default = current/no-lever baseline):
//   L1 catastrophe price-stop: intraday vs daily LOW. -N*ATR14(entry) or -X% of entry.
//   L2 in-trade regime-abort  : while holding, exit when bear_below >= ABORT_K (regime
//                               developed into sustained-bear DURING the hold). NOVEL.
//   L3 falling-knife entry veto: skip entry when close < SMA200*(1-d) (dip too deep).
//   L4 maxhold / MAE early-exit: MAXHOLD override, or exit if held>=K AND still adverse.
//
// Cost: price-proportional per the IBKR RULE (never freeze commission across eras):
//   RT_cost_pt(px) = 2*COMM_BPS/1e4*px + SPREAD_pt   (COMM_BPS=1.0=0.01%/side).
//   Measured spreads: NAS100 1.15 US500 0.53 DJ30 2.09 GER40 ~2.0. --costx N scales it.
//
// build: clang++ -std=c++17 -O2 backtest/connors_lever_sweep.cpp -o /tmp/clsweep
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <ctime>

struct Bar{int64_t ts;double o,h,l,c;};
static std::vector<Bar> load(const char*p){std::vector<Bar>v;std::ifstream f(p);std::string ln;long long ts;double o,h,l,c;
 while(std::getline(f,ln)){ if(std::sscanf(ln.c_str(),"%lld,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)==5 && c>0) v.push_back({(int64_t)ts,o,h,l,c}); } return v;}
static int yr(int64_t s){std::time_t t=s;std::tm g{};gmtime_r(&t,&g);return g.tm_year+1900;}

static double smaC(const std::vector<Bar>&B,int i,int n){double s=0;for(int k=i-n+1;k<=i;k++)s+=B[k].c;return s/n;}
static double rsiN(const std::vector<Bar>&B,int i,int n){ if(i<n)return 50.0; double g=0,l=0;
 for(int k=i-n+1;k<=i;k++){double ch=B[k].c-B[k-1].c; if(ch>0)g+=ch; else l+=-ch;} return l==0?100.0:100.0-100.0/(1.0+g/l);}
static double atr14(const std::vector<Bar>&B,int i){ int n=14; if(i<n)return 0; double s=0;
 for(int k=i-n+1;k<=i;k++){ double tr=B[k].h-B[k].l; double a=std::fabs(B[k].h-B[k-1].c),b=std::fabs(B[k].l-B[k-1].c);
   if(a>tr)tr=a; if(b>tr)tr=b; s+=tr;} return s/n;}
static double ibs(const Bar&b){ return (b.h<=b.l)?0.5:(b.c-b.l)/(b.h-b.l); }
static int downstreak(const std::vector<Bar>&B,int i){ int s=0; for(int k=i;k>0 && B[k].c<B[k-1].c;--k)++s; return s;}

// ── instance config ────────────────────────────────────────────────────────
struct Cfg{
  int entry_mode=0;    // 0 RSI2 | 1 IBS | 2 STREAK | 4 RSI3 | 5 DOUBLE
  int regime_gate=0;   // 0 close>SMA200 | 1 asym sustained-bear veto
  bool scalein=false;
  int TREND=200,SHORT=5,MAXHOLD=10,BEAR_VETO_K=20,STREAK_N=3,RSI3=3,MAX_UNITS=2;
  double RSI_IN=10.0,IBS_IN=0.10,RSI3_IN=15.0,DBL_IBS=0.20,DBL_RSI=15.0;
  double comm_bps=1.0, spread=1.0, costx=1.0;
  // LEVERS (defaults = OFF / baseline)
  double l1_atr=0.0;     // >0 => stop at entry - l1_atr*ATR14(entry)
  double l1_pct=0.0;     // >0 => stop at entry*(1-l1_pct)
  int    l2_abort_k=0;   // >0 => exit when bear_below>=k during hold
  double l3_knife=0.0;   // >0 => veto entry when close<SMA200*(1-l3_knife)
  int    l4_maxhold=0;   // >0 => override MAXHOLD
  int    l4_mae_k=0;     // >0 => exit if held>=k AND close<entry_avg
};

static bool signalfn(const std::vector<Bar>&B,int i,const Cfg&c){
  switch(c.entry_mode){
    case 1: return ibs(B[i])<c.IBS_IN;
    case 2: return downstreak(B,i)>=c.STREAK_N;
    case 4: return rsiN(B,i,c.RSI3)<c.RSI3_IN;
    case 5: return ibs(B[i])<c.DBL_IBS && rsiN(B,i,2)<c.DBL_RSI;
    default:return rsiN(B,i,2)<c.RSI_IN;
  }
}
static double costpt(const Cfg&c,double px){ return c.costx*(2.0*c.comm_bps/1e4*px + c.spread); }

struct Trade{int64_t ets;double pnl;double units;};

static std::vector<Trade> run(const std::vector<Bar>&B,const Cfg&c){
  std::vector<Trade> out;
  bool act=false; double entry=0,units=0,atr_e=0; int64_t ets=0; int held=0; int bear_below=0;
  const int MAXH = c.l4_maxhold>0? c.l4_maxhold : c.MAXHOLD;
  for(int i=0;i<(int)B.size();++i){
    const double close=B[i].c;
    if(i>=c.TREND+5-1){ double s0=smaC(B,i,c.TREND), s5=smaC(B,i-5,c.TREND); if(close<s0&&s0<s5)bear_below++; else bear_below=0; }
    else bear_below=0;

    // ── L1 catastrophe stop: intraday vs daily LOW (from held>=1) ──
    if(act && held>=1 && (c.l1_atr>0||c.l1_pct>0)){
      double stop_px = c.l1_atr>0? (entry - c.l1_atr*atr_e) : (entry*(1.0-c.l1_pct));
      if(B[i].l<=stop_px){
        double fill=(B[i].o<=stop_px)?B[i].o:stop_px;   // gap-through => open
        double pnl=(fill-entry)*units - costpt(c,fill)*units;
        out.push_back({ets,pnl,units}); act=false;
      }
    }

    // ── manage / exit (faithful day-close) ──
    if(act){
      held++;
      double s5=smaC(B,i,c.SHORT);
      bool exit = (close>s5) || (held>=MAXH);
      if(!exit && c.l2_abort_k>0 && bear_below>=c.l2_abort_k) exit=true;         // L2
      if(!exit && c.l4_mae_k>0 && held>=c.l4_mae_k && close<entry) exit=true;    // L4 MAE
      if(exit){ double pnl=(close-entry)*units - costpt(c,close)*units; out.push_back({ets,pnl,units}); act=false; }
      else if(c.scalein && units<c.MAX_UNITS && i>=c.TREND){
        double s0=smaC(B,i,c.TREND); bool rok=(c.regime_gate==1)?(bear_below<c.BEAR_VETO_K):(close>s0);
        if(rok && signalfn(B,i,c)){ entry=(entry*units+close)/(units+1); units+=1; }
      }
    }
    // ── entry (same-day re-entry allowed) ──
    if(!act && i>=c.TREND){
      double s0=smaC(B,i,c.TREND);
      bool rok=(c.regime_gate==1)?(bear_below<c.BEAR_VETO_K):(close>s0);
      bool knife_ok = (c.l3_knife<=0.0) || (close >= s0*(1.0-c.l3_knife));
      if(rok && knife_ok && signalfn(B,i,c)){ act=true; entry=close; ets=B[i].ts; held=0; units=1; atr_e=atr14(B,i); }
    }
  }
  return out;
}

struct Stat{int n=0,w=0;double net=0,gp=0,gl=0,worst=0,peak=0,eq=0,dd=0,y22=0,h1=0,h2=0;};
static Stat eval(const std::vector<Trade>&tr){
  Stat s; int total=(int)tr.size();
  for(int i=0;i<total;i++){ double p=tr[i].pnl; s.n++; if(p>0){s.w++;s.gp+=p;}else s.gl+=-p; s.net+=p;
    if(p<s.worst)s.worst=p; s.eq+=p; if(s.eq>s.peak)s.peak=s.eq; double d=s.peak-s.eq; if(d>s.dd)s.dd=d;
    if(yr(tr[i].ets)==2022)s.y22+=p; if(i<total/2)s.h1+=p; else s.h2+=p; }
  return s;
}
static double pf(const Stat&s){return s.gl>0?s.gp/s.gl:(s.gp>0?999:0);}

static const char* NDX="/Users/jo/Tick/NDX_daily_2016_2026.csv";
static const char* SPX="/Users/jo/Tick/SPX_daily_2016_2026.csv";
static const char* DJ ="/Users/jo/Tick/DJ30_daily_2016_2026.csv";
static const char* GER="/Users/jo/Tick/GER40_daily_2016_2026.csv";

static Cfg base(int mode,int gate,bool scale,double spread){ Cfg c; c.entry_mode=mode;c.regime_gate=gate;c.scalein=scale;c.spread=spread; return c; }

struct Inst{const char*name;const char*file;Cfg cfg;};

int main(int argc,char**argv){
  double costx=1.0; bool validate=false; std::string only;
  for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"--validate"))validate=true;
    else if(!strcmp(argv[i],"--costx")&&i+1<argc)costx=atof(argv[++i]);
    else if(!strcmp(argv[i],"--only")&&i+1<argc)only=argv[++i]; }

  std::vector<Inst> insts = {
    {"NAS100_RSI2", NDX, base(0,1,true ,1.15)},
    {"GER40_RSI2",  GER, base(0,0,false,2.00)},
    {"US500_RSI2",  SPX, base(0,0,false,0.53)},
    {"DJ30_RSI2",   DJ , base(0,0,false,2.09)},
    {"NAS100_IBS",  NDX, base(1,1,false,1.15)},
    {"NAS100_STREAK",NDX,base(2,0,false,1.15)},
    {"NAS100_DOUBLE",NDX,base(5,0,false,1.15)},
    {"NAS100_RSI3", NDX, base(4,1,false,1.15)},
    {"US500_STREAK",SPX, base(2,0,false,0.53)},
    {"US500_DOUBLE",SPX, base(5,0,false,0.53)},
    {"US500_IBS",   SPX, base(1,0,false,0.53)},
    {"DJ30_IBS",    DJ , base(1,0,false,2.09)},
    {"DJ30_DOUBLE", DJ , base(5,0,false,2.09)},
  };
  for(auto&in:insts) in.cfg.costx=costx;

  if(validate){
    auto B=load(NDX); Cfg c=insts[0].cfg; c.comm_bps=0; c.spread=8.0; c.costx=1.0;
    auto s=eval(run(B,c));
    printf("[VALIDATE] NAS gate1 SCALEIN @flat8pt: n=%d net=%.1fpt (real-engine target n=142 net=25456)\n",s.n,s.net);
    return 0;
  }

  printf("# ConnorsRSI2 family — adverse-protection LEVER sweep (costx=%.1f, price-prop IBKR cost)\n\n",costx);

  #define VAR(TAG, BODY) { Cfg c=in.cfg; BODY; variants.push_back({TAG,c}); }
  for(auto&in:insts){
    if(!only.empty() && only!=in.name) continue;
    auto B=load(in.file);
    Stat b=eval(run(B,in.cfg));
    printf("== %-14s  BASE: n=%d net=%.0f worst=%.0f H1=%+.0f H2=%+.0f 2022=%+.0f PF=%.2f DD=%.0f ==\n",
      in.name,b.n,b.net,b.worst,b.h1,b.h2,b.y22,pf(b),b.dd);
    std::vector<std::pair<std::string,Cfg>> variants;
    for(double n:{3.0,4.0,5.0,6.0}) VAR("L1_atr"+std::to_string((int)n), c.l1_atr=n);
    for(double x:{0.05,0.08,0.12}) VAR("L1_pct"+std::to_string((int)(x*100)), c.l1_pct=x);
    for(int k:{10,15,20,25}) VAR("L2_abort"+std::to_string(k), c.l2_abort_k=k);
    for(double d:{0.03,0.05,0.08,0.12}) VAR("L3_knife"+std::to_string((int)(d*100)), c.l3_knife=d);
    for(int m:{4,6,8}) VAR("L4_maxhold"+std::to_string(m), c.l4_maxhold=m);
    for(int k:{3,4,6}) VAR("L4_mae"+std::to_string(k), c.l4_mae_k=k);
    for(auto&v:variants){
      Stat s=eval(run(B,v.second));
      double dpct = b.net!=0? 100.0*(s.net-b.net)/std::fabs(b.net):0;
      printf("  %-12s n=%-4d net=%-9.0f d%%=%+6.1f worst=%-8.0f(%+.0f) H1%s H2%s 2022=%+.0f(%+.0f) PF=%.2f DD=%.0f\n",
        v.first.c_str(), s.n, s.net, dpct, s.worst, s.worst-b.worst,
        s.h1>0?"+":"-", s.h2>0?"+":"-", s.y22, s.y22-b.y22, pf(s), s.dd);
    }
    printf("\n");
  }
  return 0;
}

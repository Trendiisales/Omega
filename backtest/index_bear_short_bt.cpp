// =============================================================================
// index_bear_short_bt.cpp -- can an index engine make money in a BEAR? (SHORT side)
// -----------------------------------------------------------------------------
// Tests the OPPOSITE polarity to the failed long-only panic-bounce: a SHORT
// breakdown/trend-follow engine. Thesis: a bear-index engine must SHORT the
// breakdown (ride the down-trend), not buy the bounce. Gate it to down-regime
// (price<EMA & EMA falling) so it stays flat/loses little in the bull and pays
// in the bear. One engine class, per-symbol cost; pooled + per-instrument + the
// 2022 NAS bear as the real out-of-sample bear tape.
// Build: clang++ -O3 -std=c++17 -o backtest/index_bear_short_bt backtest/index_bear_short_bt.cpp
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

struct P{ int atr_n=24, don=48, ema_n=50, ema_slow=200, dd_lb=250; double sl_atr=2.0, trail_atr=3.0, dd_min=0.06; int persist=100;
  int max_hold=240, cooldown=12; double cost=1.5; double tp_r=0.0; /*0=trail,>0=fixed R TP*/ };
struct St{int n=0,w=0,l=0,trail=0,stop=0,tim=0;double net=0,gw=0,gl=0,dd=0;
  double pf()const{return gl>1e-9?gw/gl:(gw>0?999:0);} double wr()const{return n?100.0*w/n:0;}};

static void run(const std::vector<Bar>&B,const P&pp,St&st){
  int n=(int)B.size(); if(n<300)return;
  std::vector<double> atr(n,0),ema(n,0),emaS(n,0);
  std::vector<double> tr(n,0); for(int i=1;i<n;i++)tr[i]=std::max(B[i].h-B[i].l,std::max(std::fabs(B[i].h-B[i-1].c),std::fabs(B[i].l-B[i-1].c)));
  double s=0; for(int i=1;i<n;i++){s+=tr[i];if(i>pp.atr_n)s-=tr[i-pp.atr_n];atr[i]=s/std::min(i,pp.atr_n);}
  double k=2.0/(pp.ema_n+1); ema[0]=B[0].c; for(int i=1;i<n;i++)ema[i]=ema[i-1]+k*(B[i].c-ema[i-1]);
  double ks=2.0/(pp.ema_slow+1); emaS[0]=B[0].c; for(int i=1;i<n;i++)emaS[i]=emaS[i-1]+ks*(B[i].c-emaS[i-1]);
  bool inpos=false;double entry=0,stop=0,ll=0;int ei=0;double cum=0,pk=0;int last_exit=-100000;
  int start=std::max({pp.atr_n,pp.don,pp.ema_n})+2;
  for(int i=start;i<n;i++){
    double A=atr[i]; if(A<=0)continue;
    if(inpos){
      if(B[i].l<ll)ll=B[i].l;
      double ex=0;const char*why=nullptr;
      if(pp.tp_r>0){                                   // fixed-TP short (grab profit fast)
        double r=std::fabs(stop-entry); double tp=entry-pp.tp_r*r;
        if(B[i].h>=stop){ex=stop;why="STOP";}
        else if(B[i].l<=tp){ex=tp;why="TRAIL";}        // reuse TRAIL counter = "TP hit"
        else if(i-ei>=pp.max_hold){ex=B[i].c;why="TIME";}
      } else {
        double trail=ll+pp.trail_atr*A; double eff=std::min(stop,trail);
        if(B[i].h>=eff){ex=eff;why=(eff<stop?"TRAIL":"STOP");}
        else if(i-ei>=pp.max_hold){ex=B[i].c;why="TIME";}
      }
      if(why){ double pnl=(entry-ex)-pp.cost; st.n++;st.net+=pnl; if(pnl>0){st.w++;st.gw+=pnl;}else{st.l++;st.gl+=-pnl;}
        if(why[0]=='T'&&why[1]=='R')st.trail++;else if(why[0]=='S')st.stop++;else st.tim++;
        cum+=pnl;if(cum>pk)pk=cum;if(pk-cum>st.dd)st.dd=pk-cum; inpos=false;last_exit=i; }
      continue;
    }
    if(i-last_exit<pp.cooldown)continue;
    // HARD bear-regime gate (risk-off proxy): PERSISTENT slow-EMA decline.
    // A bull correction dips below EMA200 briefly; a real bear keeps EMA200
    // falling for a long stretch. Gate on the slow EMA being below where it was
    // PERSIST bars ago -> near-silent in bull, active through 2022-style bears.
    bool bear_regime = B[i].c<emaS[i] && emaS[i]<emaS[i-pp.persist] && ema[i]<emaS[i];
    if(!bear_regime)continue;
    // local down momentum
    bool down = B[i].c<ema[i] && ema[i]<ema[i-5];
    if(!down)continue;
    // Donchian breakdown: close below min low of prior `don` bars
    double lo=1e18; for(int kk=i-pp.don;kk<i;kk++) if(B[kk].l<lo)lo=B[kk].l;
    if(B[i].c<lo){
      entry=B[i].c; double sh=B[i].h; for(int kk=i-1;kk>=i-3;kk--) if(B[kk].h>sh)sh=B[kk].h;
      stop = sh>entry? sh : entry+pp.sl_atr*A; ll=B[i].l; ei=i; inpos=true;
    }
  }
}
struct Series{const char*name;const char*path;double cost;};
static St runseg(const Series&s,const P&base,int half){auto B=load(s.path);St st;if(B.size()<300)return st;P pp=base;pp.cost=s.cost;int n=(int)B.size(),lo=0,hi=n;if(half==1)hi=n/2;else if(half==2)lo=n/2;std::vector<Bar> sub(B.begin()+lo,B.begin()+hi);run(sub,pp,st);return st;}
static void row(const char*nm,const St&s){std::printf("%-16s %5d %6.1f %+10.1f %6.2f %9.1f  %d/%d/%d\n",nm,s.n,s.wr(),s.net,s.pf(),s.dd,s.trail,s.stop,s.tim);}

int main(int argc,char**argv){
  P pp; pp.tp_r = 2.0;   // fixed 2R TP -- grab profit before the bear counter-rally
  // ad-hoc single-file: ./index_bear_short_bt <csv> <cost> [tp_r] [don]
  if(argc>=3){
    Series s{"FILE",argv[1],atof(argv[2])};
    if(argc>3)pp.tp_r=atof(argv[3]); if(argc>4)pp.don=atoi(argv[4]);
    std::printf("=== BEAR-SHORT single-file %s cost=%.2f tp=%.1fR don=%d ===\n",argv[1],s.cost,pp.tp_r,pp.don);
    std::printf("%-16s %5s %6s %10s %6s %9s  %s\n","series","N","WR%","net(pt)","PF","maxDD","TP/ST/TI");
    St a=runseg(s,pp,0); row("FULL",a);
    St h1=runseg(s,pp,1),h2=runseg(s,pp,2); row("  H1",h1); row("  H2",h2);
    return 0;
  }
  std::vector<Series> bull={{"SPX24-26","/Users/jo/Tick/SPXUSD_merged.h1.csv",0.6},{"NAS24-26","/Users/jo/Tick/NSXUSD_merged.h1.csv",2.0},{"GER40-25","/Users/jo/Tick/GER40_merged.h1.csv",1.5}};
  Series bear={"NAS2022bear","/Users/jo/Tick/NAS2022_bear_h1.csv",2.0};
  std::printf("=== INDEX BEAR-SHORT engine (Donchian-breakdown, down-regime gate, FIXED 2R TP) ===\n");
  std::printf("don=%d ema=%d sl=%.1f tp=%.1fR cost-incl  (TR col = TP-hits)\n\n",pp.don,pp.ema_n,pp.sl_atr,pp.tp_r);
  std::printf("%-16s %5s %6s %10s %6s %9s  %s\n","series","N","WR%","net(pt)","PF","maxDD","TR/ST/TI");
  St agg;
  for(auto&s:bull){ St a=runseg(s,pp,0); row(s.name,a);
    St h1=runseg(s,pp,1),h2=runseg(s,pp,2); char b[24]; std::snprintf(b,24,"  %s.H1",s.name);row(b,h1);std::snprintf(b,24,"  %s.H2",s.name);row(b,h2);
    agg.n+=a.n;agg.w+=a.w;agg.l+=a.l;agg.net+=a.net;agg.gw+=a.gw;agg.gl+=a.gl;agg.trail+=a.trail;agg.stop+=a.stop;agg.tim+=a.tim;if(a.dd>agg.dd)agg.dd=a.dd;}
  std::printf("------ POOLED bull-corpus (shorts SHOULD be ~flat/small -- regime keeps them rare) ------\n"); row("POOLED",agg);
  std::printf("------ 2022 BEAR (the real test) ------\n"); { St b=runseg(bear,pp,0); row(bear.name,b);
    St h1=runseg(bear,pp,1),h2=runseg(bear,pp,2); row("  bear.H1",h1); row("  bear.H2",h2); }
  return 0;
}

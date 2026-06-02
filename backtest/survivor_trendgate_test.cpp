// survivor_trendgate_test.cpp
// Validate the SurvivorPortfolio RSI/ZScoreMR trend gate (2026-06-03) before deploy.
// Replicates the cell's EXACT signal + sim (ATR Wilder, level-based RSI, zscore,
// SL=sl_mult*ATR, TP=tp_mult*ATR, timeout=max_hold_bars, cost=tick_usd*lot) and
// compares baseline (no gate) vs gated. Reads the H4 warmup CSVs (ts,o,h,l,c).
//
// Build:  g++ -O2 -std=c++17 backtest/survivor_trendgate_test.cpp -o /tmp/sgt
// Run:    /tmp/sgt
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>

struct Bar { double o,h,l,c; };

static std::vector<Bar> load(const std::string& path){
    std::vector<Bar> v; std::ifstream f(path); std::string ln;
    while(std::getline(f,ln)){
        if(!ln.empty()&&ln.back()=='\r') ln.pop_back();
        if(ln.empty()||ln[0]=='t') continue;
        std::stringstream ss(ln); std::string t; std::vector<std::string> k;
        while(std::getline(ss,t,',')) k.push_back(t);
        if(k.size()<5) continue;
        try{ Bar b; b.o=std::stod(k[1]); b.h=std::stod(k[2]); b.l=std::stod(k[3]); b.c=std::stod(k[4]); v.push_back(b);}catch(...){continue;}
    }
    return v;
}

// exact copies of SurvivorPortfolio indicator math (operate on close history)
static double rsi_at(const std::vector<Bar>& b,int end,int n){ // end exclusive index = current bar +1
    if(end < n+1) return 50.0; double g=0,l=0;
    for(int i=end-n;i<end;++i){ double d=b[i].c-b[i-1].c; if(d>0)g+=d; else l-=d; }
    if(l<1e-12) return 100.0; double rs=g/l; return 100.0-100.0/(1+rs);
}
static double zscore_at(const std::vector<Bar>& b,int end,int W){
    if(end<W) return 0; double s=0; for(int i=end-W;i<end;++i)s+=b[i].c; double m=s/W;
    double v=0; for(int i=end-W;i<end;++i){double d=b[i].c-m; v+=d*d;} v/=W; double sd=std::sqrt(v);
    if(sd<1e-12) return 0; return (b[end-1].c-m)/sd;
}
// trend_dir: SMA50 slope over 25 bars, THR 0.0015 (matches engine)
static int trend_dir_at(const std::vector<Bar>& b,int end){
    const int TN=50; if(end < TN+TN/2+1) return 0;
    auto sma_back=[&](int back){ int e=end-back; double s=0; for(int i=e-TN;i<e;++i)s+=b[i].c; return s/TN; };
    double now=sma_back(0), prev=sma_back(TN/2); if(prev<=0)return 0;
    double slope=(now-prev)/prev; const double THR=0.0015;
    if(slope>THR)return +1; if(slope<-THR)return -1; return 0;
}

struct Res{ int n=0,w=0; double net=0,maxdd=0,gp=0,gl=0; };
static double PF(const Res&r){ return r.gl>1e-9? r.gp/r.gl : (r.gp>0?999:0); }

enum Fam{RSI,ZMR};
static Res sim(const std::vector<Bar>& b,Fam fam,int N,double lo,double hi,double zthr,
               double sl_m,double tp_m,int max_hold,int cooldown,double tick_usd,double lot,bool gate){
    Res r; const int ATR_N=14;
    double atr=0,prevc=0; std::vector<double> trv; bool atr_ok=false;
    bool active=false; int side=0; double entry=0,sl=0,tp=0; int entry_idx=0,last_entry=-100000;
    double eq=0,peak=0;
    for(int i=0;i<(int)b.size();++i){
        // ATR update (Wilder, after 14-seed)
        if(i==0) prevc=b[i].c;
        double tr=std::max({b[i].h-b[i].l,std::fabs(b[i].h-prevc),std::fabs(b[i].l-prevc)});
        prevc=b[i].c;
        if(!atr_ok){ if((int)trv.size()<ATR_N){ trv.push_back(tr); if((int)trv.size()==ATR_N){double s=0;for(double v:trv)s+=v; atr=s/ATR_N; atr_ok=true;} } }
        else atr=(atr*(ATR_N-1)+tr)/ATR_N;
        int end=i+1; // current bar inclusive
        // manage open
        if(active){
            int held=i-entry_idx;
            bool sl_hit = side>0? (b[i].l<=sl):(b[i].h>=sl);
            bool tp_hit = side>0? (b[i].h>=tp):(b[i].l<=tp);
            bool to = held>=max_hold;
            if(sl_hit||tp_hit||to){
                double ex = sl_hit? sl : (tp_hit? tp : b[i].c);
                double pts = side>0? (ex-entry):(entry-ex);
                double usd = pts*tick_usd*lot;
                r.net+=usd; ++r.n; if(usd>0){++r.w; r.gp+=usd;} else r.gl+=-usd;
                eq+=usd; peak=std::max(peak,eq); r.maxdd=std::max(r.maxdd,peak-eq);
                active=false; last_entry=i;
            }
        }
        // entry
        if(!active && atr_ok && atr>0 && (i-last_entry>cooldown)){
            int dir=0;
            if(fam==RSI){ double v=rsi_at(b,end,N); dir = v<lo?+1:(v>hi?-1:0); }
            else { double z=zscore_at(b,end,N); dir = z>=zthr?-1:(z<=-zthr?+1:0); }
            if(dir!=0 && gate){ int td=trend_dir_at(b,end);
                if(dir<0&&td>0)dir=0; if(dir>0&&td<0)dir=0; }
            if(dir!=0){ active=true; side=dir; entry=b[i].c; entry_idx=i;
                sl = dir>0? entry-sl_m*atr : entry+sl_m*atr;
                tp = dir>0? entry+tp_m*atr : entry-tp_m*atr; }
        }
    }
    return r;
}

static void row(const char* tag,const Res&base,const Res&g){
    printf("%-22s | BASE n=%3d W%3d net=%+9.2f PF=%4.2f DD=%7.2f || GATED n=%3d W%3d net=%+9.2f PF=%4.2f DD=%7.2f\n",
        tag, base.n,base.w,base.net,PF(base),base.maxdd, g.n,g.w,g.net,PF(g),g.maxdd);
}

int main(){
    auto ustec=load("phase1/signal_discovery/warmup_USTEC.F_H4.csv");
    auto ger  =load("phase1/signal_discovery/warmup_GER40_H4.csv");
    printf("USTEC bars=%zu  GER40 bars=%zu\n",ustec.size(),ger.size());
    printf("%-22s | %-48s || %s\n","cell","baseline (no gate)","trend-gated");
    // USTEC.F: tick_usd=20, lot=0.10 ; GER40: tick_usd=1.10, lot=0.10
    // USTEC_4h_RSI_N7 (lo30 hi70 sl1 tp2 hold30)
    row("USTEC_4h_RSI_N7",
        sim(ustec,RSI,7,30,70,0,1.0,2.0,30,1,20,0.10,false),
        sim(ustec,RSI,7,30,70,0,1.0,2.0,30,1,20,0.10,true));
    // USTEC_4h_ZMR (W20 z2.5 sl1 tp2 hold30)
    row("USTEC_4h_ZMR",
        sim(ustec,ZMR,20,0,0,2.5,1.0,2.0,30,1,20,0.10,false),
        sim(ustec,ZMR,20,0,0,2.5,1.0,2.0,30,1,20,0.10,true));
    // GER40 RSI N7 / N14 (already-disabled cells, reference)
    row("GER40_4h_RSI_N7",
        sim(ger,RSI,7,30,70,0,1.0,2.0,30,1,1.10,0.10,false),
        sim(ger,RSI,7,30,70,0,1.0,2.0,30,1,1.10,0.10,true));
    row("GER40_4h_RSI_N14",
        sim(ger,RSI,14,30,70,0,1.0,2.0,30,1,1.10,0.10,false),
        sim(ger,RSI,14,30,70,0,1.0,2.0,30,1,1.10,0.10,true));
    row("GER40_4h_ZMR",
        sim(ger,ZMR,20,0,0,2.5,1.0,2.0,30,1,1.10,0.10,false),
        sim(ger,ZMR,20,0,0,2.5,1.0,2.0,30,1,1.10,0.10,true));
    return 0;
}

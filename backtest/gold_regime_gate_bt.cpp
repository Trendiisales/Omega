// =============================================================================
// gold_regime_gate_bt.cpp -- does the D1 EMA200-slope regime gate (GoldD1TrendState)
//   PROTECT long-only gold engines in a bear without hurting them in a bull?
//
// Replicates GoldD1TrendState EXACTLY: D1 bars from H1, EMA200 of D1 closes,
//   slope over slope_lookback days = (ema_now - ema_10ago)/ema_10ago,
//   UPTREND if slope>+thr, DOWNTREND if slope<-thr, else NEUTRAL.
//   long_allowed() == regime != DOWNTREND  (UPTREND/NEUTRAL/UNKNOWN all allow).
//
// Two representative LONG signal generators (cover both engine families):
//   A) Donchian-N breakout (trend long; ~XauTurtleD1): enter long on H1 close >
//      prior DON-bar high; stop = entry - SL_ATR*ATR; exit on close < exit-Donchian
//      low OR stop OR MAX_HOLD.
//   B) RSI(14) oversold bounce (mean-rev long; ~GoldOversoldBounce): enter long
//      when RSI<30 and rising; same ATR stop; TP at RSI>55 or MAX_HOLD.
//
// For each: report UNGATED vs GATED (skip long entries when regime==DOWNTREND),
//   both halves, on whatever tape is passed. Bear tape -> gate should cut the
//   bleed. Bull tape -> gate should block ~0 entries (no-harm).
//
// build: g++ -std=c++17 -O2 gold_regime_gate_bt.cpp -o goldregime
// usage: ./goldregime h1.csv [DON=20] [SL_ATR=2.0] [MAX_HOLD=120] [cost_pts=0.6] [name]
// =============================================================================
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
using namespace std;

struct Bar { int64_t ts; double o,h,l,c; };

static vector<Bar> load(const string& p){
    vector<Bar> v; ifstream f(p); if(!f){fprintf(stderr,"no file %s\n",p.c_str());return v;}
    string ln; bool first=true;
    while(getline(f,ln)){ if(first){first=false; if(!ln.empty()&&(ln[0]<'0'||ln[0]>'9'))continue;}
        stringstream ss(ln); string t; vector<string> k; while(getline(ss,t,','))k.push_back(t);
        if(k.size()<5)continue; Bar b; double ts=atof(k[0].c_str());
        b.ts=(int64_t)(ts>1e11? ts/1000.0 : ts);   // accept ms or sec
        b.o=atof(k[1].c_str()); b.h=atof(k[2].c_str()); b.l=atof(k[3].c_str()); b.c=atof(k[4].c_str());
        if(b.h>0)v.push_back(b);
    } return v;
}
static int64_t utc_day(int64_t s){return s/86400;}

// ---- GoldD1TrendState faithful replica: regime per H1 index (-1 until warm) ----
// regime codes: 0 UNKNOWN, 1 UPTREND, 2 DOWNTREND, 3 NEUTRAL
struct RegimeTrack {
    int ema_period=200, slope_lb=10; double thr=0.0005;
    double ema=0; int ema_count=0; deque<double> hist;
    int regime=0; double slope=0;
    void on_d1_close(double c){
        double a=2.0/(ema_period+1);
        if(ema_count==0)ema=c; else ema=a*c+(1-a)*ema; ++ema_count;
        hist.push_back(ema); while((int)hist.size()>slope_lb+1)hist.pop_front();
        if((int)hist.size()>=slope_lb+1 && ema_count>=ema_period){
            double cur=hist.back(), old=hist.front(); slope=(cur-old)/old;
            if(slope>thr)regime=1; else if(slope<-thr)regime=2; else regime=3;
        }
    }
    bool long_allowed() const { return regime!=2; }   // block only confirmed DOWNTREND
};

int main(int argc,char**argv){
    if(argc<2){printf("usage: %s h1.csv [DON=20] [SL_ATR=2.0] [MAX_HOLD=120] [cost=0.6] [name]\n",argv[0]);return 1;}
    string path=argv[1]; int DON=argc>2?atoi(argv[2]):20; double SLA=argc>3?atof(argv[3]):2.0;
    int MAXH=argc>4?atoi(argv[4]):120; double COST=argc>5?atof(argv[5]):0.6; string NAME=argc>6?argv[6]:path;
    int PERSIST=argc>7?atoi(argv[7]):100;   // H1 bars EMA200 must be falling for sustained-bear
    auto h1=load(path); if((int)h1.size()<500){printf("[%s] few bars (%zu)\n",NAME.c_str(),h1.size());return 1;}

    int N=(int)h1.size();
    // ATR(14) on H1
    vector<double> atr(N,0); { double s=0; int n=0; deque<double> tr;
        for(int i=0;i<N;++i){ double t=h1[i].h-h1[i].l; if(i>0)t=max(t,max(fabs(h1[i].h-h1[i-1].c),fabs(h1[i].l-h1[i-1].c)));
            tr.push_back(t); s+=t; if((int)tr.size()>14){s-=tr.front();tr.pop_front();} atr[i]=tr.empty()?0:s/tr.size(); (void)n; } }
    // RSI(14) on H1 close
    vector<double> rsi(N,50); { double ag=0,al=0; for(int i=1;i<N;++i){ double d=h1[i].c-h1[i-1].c; double g=d>0?d:0, l=d<0?-d:0;
        if(i<=14){ag+=g;al+=l; if(i==14){ag/=14;al/=14; rsi[i]=al==0?100:100-100/(1+ag/al);}}
        else{ ag=(ag*13+g)/14; al=(al*13+l)/14; rsi[i]=al==0?100:100-100/(1+ag/al);} } }
    // D1 regime per H1 index: build D1 closes from H1, classify, map forward
    vector<int> reg(N,0); { RegimeTrack rt; int64_t cd=-1; double dclose=0;
        for(int i=0;i<N;++i){ int64_t d=utc_day(h1[i].ts);
            if(cd<0){cd=d;dclose=h1[i].c;}
            else if(d!=cd){ rt.on_d1_close(dclose); cd=d; dclose=h1[i].c; }
            else dclose=h1[i].c;
            reg[i]=rt.regime;   // regime as known at this H1 bar (prior D1 closes only)
        } }
    // H1 EMA200/EMA50 + IBS-style sustained-bear flag (faster-release alt gate):
    //   bear = close<EMA200 && EMA200 falling over PERSIST H1 bars && EMA50<EMA200.
    vector<double> e200(N,0), e50(N,0); vector<char> sbear(N,0);
    { double k2=2.0/201, k5=2.0/51;
      for(int i=0;i<N;++i){ e200[i]= i==0? h1[i].c : e200[i-1]+k2*(h1[i].c-e200[i-1]);
                            e50[i] = i==0? h1[i].c : e50[i-1]+k5*(h1[i].c-e50[i-1]); }
      for(int i=0;i<N;++i){ if(i>=PERSIST+200)
          sbear[i]=(h1[i].c<e200[i] && e200[i]<e200[i-PERSIST] && e50[i]<e200[i])?1:0; } }
    // D1-based sustained-bear (SLOWER, cleaner macro regime): build D1 closes, D1
    //   EMA200/EMA50, flag = D1close<D1EMA200 && D1EMA200 falling PERSIST_D days &&
    //   D1EMA50<D1EMA200. Mapped forward to each H1 bar.
    const int PERSIST_D=20, EMA_D_SLOW=50, EMA_D_FAST=20;   // ~50-day & 20-day D1 spine
    vector<char> sbearD(N,0);
    { double eS=0,eF=0; bool have=false; deque<double> hS; int dcount=0;
      int64_t cd=-1; double dclose=0; char flag=0;
      double kS=2.0/(EMA_D_SLOW+1), kF=2.0/(EMA_D_FAST+1);
      for(int i=0;i<N;++i){ int64_t d=utc_day(h1[i].ts);
        if(cd<0){cd=d;dclose=h1[i].c;}
        else if(d!=cd){ // D1 close
            if(!have){eS=dclose;eF=dclose;have=true;} else {eS+=kS*(dclose-eS);eF+=kF*(dclose-eF);}
            hS.push_back(eS); while((int)hS.size()>PERSIST_D+1)hS.pop_front(); ++dcount;
            flag=0; if(dcount>=EMA_D_SLOW+PERSIST_D && (int)hS.size()>=PERSIST_D+1){
                double old=hS.front(); if(dclose<eS && eS<old && eF<eS) flag=1; }
            cd=d; dclose=h1[i].c;
        } else dclose=h1[i].c;
        sbearD[i]=flag;
      } }
    // Donchian high/low (prior DON bars)
    auto don_hi=[&](int i){ double v=-1e18; for(int k=max(0,i-DON);k<i;++k)v=max(v,h1[k].h); return v; };
    auto don_lo=[&](int i){ double v=1e18; for(int k=max(0,i-DON);k<i;++k)v=min(v,h1[k].l); return v; };

    struct T{double pnl;bool win;int half;};
    // signal: 0=Donchian breakout long, 1=RSI oversold long. gate: false=ungated true=gated
    // gatemode: 0=none, 1=D1-slope-DOWNTREND (GoldD1TrendState), 2=sustained-bear-H1 (IBS-style)
    auto run=[&](int sigtype,int gatemode,const char* tag){
        vector<T> tr; bool inpos=false; double entry=0,stop=0; int held=0; int entry_i=0;
        int blocked=0;
        for(int i=DON+1;i<N;++i){ if(atr[i]<=0)continue;
            if(inpos){
                held++;
                double ex=0; bool out=false;
                if(h1[i].l<=stop){ex=stop;out=true;}
                else if(sigtype==0 && h1[i].c<don_lo(i)){ex=h1[i].c;out=true;}     // Donchian exit
                else if(sigtype==1 && rsi[i]>55){ex=h1[i].c;out=true;}             // RSI target
                else if(held>=MAXH){ex=h1[i].c;out=true;}
                if(out){ double pnl=(ex-entry)-COST; tr.push_back({pnl,pnl>0, i<N/2?0:1}); inpos=false; }
                continue;
            }
            bool sig=false;
            if(sigtype==0) sig = h1[i].c>don_hi(i);                                // breakout long
            else           sig = rsi[i]<30 && rsi[i]>rsi[i-1];                     // oversold turning up
            if(!sig)continue;
            if(gatemode==1 && reg[i]==2){ blocked++; continue; }                    // GATE: D1-slope DOWNTREND
            if(gatemode==2 && sbear[i]){ blocked++; continue; }                     // GATE: sustained-bear (IBS-style)
            if(gatemode==3 && sbearD[i]){ blocked++; continue; }                    // GATE: D1-sustained-bear (macro)
            entry=h1[i].c; stop=entry-SLA*atr[i]; inpos=true; held=0; entry_i=i; (void)entry_i;
        }
        int n=tr.size(),w=0,h1c=0,h2c=0; double net=0,gw=0,gl=0,net1=0,net2=0;
        for(auto&t:tr){ if(t.win)w++; net+=t.pnl; if(t.pnl>0)gw+=t.pnl; else gl+=-t.pnl;
            if(t.half==0){net1+=t.pnl;h1c++;}else{net2+=t.pnl;h2c++;} }
        double pf=gl>0?gw/gl:(gw>0?99:0);
        printf("  %-22s n=%3d WR=%4.1f%% PF=%4.2f net=%9.1fpt | H1 net=%+8.1f(n%d) H2 net=%+8.1f(n%d) | blocked=%d\n",
            tag,n,n?100.0*w/n:0,pf,net,net1,h1c,net2,h2c,blocked);
    };
    // regime distribution
    int up=0,dn=0,ne=0,un=0; for(int i=0;i<N;++i){int r=reg[i]; if(r==1)up++;else if(r==2)dn++;else if(r==3)ne++;else un++;}
    printf("[%s] H1bars=%d DON=%d SL=%.1fA MAXH=%d cost=%.2f | regime bars: UP=%d DOWN=%d NEUTRAL=%d UNKNOWN=%d\n",
        NAME.c_str(),N,DON,SLA,MAXH,COST,up,dn,ne,un);
    printf(" --- Donchian breakout long (trend, ~XauTurtleD1) ---\n");
    run(0,0,"UNGATED"); run(0,1,"D1-SLOPE-GATED"); run(0,2,"H1-SUSTBEAR-GATED"); run(0,3,"D1-SUSTBEAR-GATED");
    printf(" --- RSI oversold long (mean-rev, ~GoldOversoldBounce) ---\n");
    run(1,0,"UNGATED"); run(1,1,"D1-SLOPE-GATED"); run(1,2,"H1-SUSTBEAR-GATED"); run(1,3,"D1-SUSTBEAR-GATED");
    return 0;
}

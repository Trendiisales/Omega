// =============================================================================
// gold_scalp_v1_sweep.cpp -- from-scratch gold momentum-continuation scalper.
// Real XAUUSD M5 (2yr, /Users/jo/Tick), real round-trip spread cost (arg, default
// $0.22 measured from L2 VPS recording). Momentum/breakout continuation (NOT
// reversion -- reversion proven dead for gold). No-lookahead: signal on bar close,
// fill at NEXT bar open. Full lever sweep; robustness metrics per config
// (both-halves split, fat-tail net-excl-top5, maxDD, avgR).
//
// BUILD: c++ -std=c++17 -O2 backtest/gold_scalp_v1_sweep.cpp -o backtest/gold_scalp_v1_sweep
// RUN:   ./backtest/gold_scalp_v1_sweep [spread] 2> rows.csv   (stdout unused)
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>
using namespace std;

struct Bar { long ts; double o,h,l,c; };

static vector<Bar> load(const char* p){
    vector<Bar> v; FILE* f=fopen(p,"r"); if(!f){perror(p);return v;}
    char ln[256]; bool first=true; double po=0;
    while(fgets(ln,sizeof ln,f)){
        if(first){first=false; if(ln[0]<'0'||ln[0]>'9') continue;}
        long ts; double o,h,l,c;
        if(sscanf(ln,"%ld,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5) continue;
        if(o<=0||h<=0||l<=0||c<=0) continue;
        if(po>0 && (o>po*1.5||o<po*0.5)) continue;       // x1000 / glitch guard
        if(h-l > 0.2*c) continue;                         // absurd single-bar range guard
        v.push_back({ts,o,h,l,c}); po=c;
    }
    fclose(f); return v;
}

struct P {
    int ema_f, ema_s;   // 0,0 = no trend filter
    int donch;          // breakout lookback (closed bars)
    double imp;         // impulse: signal-bar range >= imp*ATR (0=off)
    double buf;         // breakout buffer in ATR
    double stop;        // initial stop in ATR
    int    exit_mode;   // 0=fixedTP, 1=trail, 2=hold-time
    double tp_R;        // fixedTP target in R (R=stop dist)
    double trail;       // trail dist in ATR
    int    be;          // 0/1 breakeven ratchet
    double be_arm;      // arm BE when mfe >= be_arm * stop_dist
    int    dir;         // 0=long only, 1=both
    int    sess;        // 0=liquid 7-20 UTC, 1=all
    int    maxbars;     // time stop
};

struct Res { int n,win; double gw,gl,net,h1,h2; double top[5]; double worst; double maxdd; double sumR; };

static Res run(const vector<Bar>& B, const P& p, double cost, long tmid){
    Res R{0,0,0,0,0,0,0,{0,0,0,0,0},0,0,0};
    int N=B.size();
    // indicators
    double ema_f=0,ema_s=0; bool ef=false,es=false;
    double kf=p.ema_f?2.0/(p.ema_f+1):0, ks=p.ema_s?2.0/(p.ema_s+1):0;
    double atr=0; bool atrw=false; double atrsum=0; int atrc=0; const int AP=14;
    double prevc=0;
    // position
    bool open=false,islong=false; double entry=0,stopd=0,stopl=0,tp=0,eatr=0; int held=0; double mfe=0; double extreme=0;
    long entry_ts=0;
    bool pending=false; bool plong=false;
    double eq=0, peak=0;

    auto donch_hi=[&](int i)->double{ double m=-1e18; for(int k=i-p.donch;k<i;k++){ if(k<0)continue; if(B[k].h>m)m=B[k].h;} return m; };
    auto donch_lo=[&](int i)->double{ double m=1e18; for(int k=i-p.donch;k<i;k++){ if(k<0)continue; if(B[k].l<m)m=B[k].l;} return m; };

    for(int i=0;i<N;i++){
        const Bar& b=B[i];
        // 1) pending entry -> fill at this bar's open
        if(pending && !open){
            if(eatr>0){ // need atr ready
                open=true; islong=plong; entry=b.o; held=0; mfe=0; extreme=b.o; entry_ts=b.ts;
                stopd=p.stop*eatr;
                stopl = islong? entry-stopd : entry+stopd;
                tp    = islong? entry+p.tp_R*stopd : entry-p.tp_R*stopd;
            }
        }
        pending=false;
        // 2) manage open on this bar's range
        if(open){
            held++;
            double fav = islong? (b.h-entry) : (entry-b.l);
            if(fav>mfe) mfe=fav;
            if(islong){ if(b.h>extreme)extreme=b.h; } else { if(b.l<extreme)extreme=b.l; }
            // BE ratchet
            if(p.be && mfe >= p.be_arm*stopd){
                double be = islong? entry+cost : entry-cost;
                if(islong) stopl=max(stopl,be); else stopl=min(stopl,be);
            }
            // trail
            if(p.exit_mode==1){
                double t = islong? extreme-p.trail*eatr : extreme+p.trail*eatr;
                if(islong) stopl=max(stopl,t); else stopl=min(stopl,t);
            }
            bool ex=false; double xp=0;
            // conservative: stop checked before tp
            if(islong){
                if(b.l<=stopl){ ex=true; xp=stopl; }
                else if(p.exit_mode==0 && b.h>=tp){ ex=true; xp=tp; }
            } else {
                if(b.h>=stopl){ ex=true; xp=stopl; }
                else if(p.exit_mode==0 && b.l<=tp){ ex=true; xp=tp; }
            }
            if(!ex && held>=p.maxbars){ ex=true; xp=b.c; }
            if(ex){
                double pnl = (islong? (xp-entry):(entry-xp)) - cost;
                double Rm = stopd>0? pnl/stopd : 0;
                R.n++; R.net+=pnl; R.sumR+=Rm;
                if(pnl>0){R.win++;R.gw+=pnl;} else R.gl+=-pnl;
                if(entry_ts<tmid) R.h1+=pnl; else R.h2+=pnl;
                if(pnl<R.worst)R.worst=pnl;
                // top5
                for(int t=0;t<5;t++){ if(pnl>R.top[t]){ for(int u=4;u>t;u--)R.top[u]=R.top[u-1]; R.top[t]=pnl; break; } }
                eq+=pnl; if(eq>peak)peak=eq; double dd=peak-eq; if(dd>R.maxdd)R.maxdd=dd;
                open=false;
            }
        }
        // 3) update indicators with bar close
        if(prevc>0){
            double tr=max(b.h-b.l,max(fabs(b.h-prevc),fabs(b.l-prevc)));
            if(!atrw){ atrsum+=tr; if(++atrc==AP){atr=atrsum/AP;atrw=true;} }
            else atr=(atr*(AP-1)+tr)/AP;
        }
        ema_f = ef? ema_f+kf*(b.c-ema_f) : b.c; ef=true;
        ema_s = es? ema_s+ks*(b.c-ema_s) : b.c; es=true;
        // 4) signal from this closed bar -> pending for next open
        if(!open && atrw && atr>0 && i>=p.donch){
            int hr = (int)((b.ts/3600)%24);
            bool sess_ok = p.sess==1 || (hr>=7 && hr<20);
            bool trend_up = (p.ema_f==0) || (ema_f>ema_s);
            bool trend_dn = (p.ema_f==0) || (ema_f<ema_s);
            bool imp_ok = (p.imp<=0) || ((b.h-b.l) >= p.imp*atr);
            double dh=donch_hi(i), dl=donch_lo(i);
            bool brk_up = b.c > dh + p.buf*atr;
            bool brk_dn = b.c < dl - p.buf*atr;
            if(sess_ok && imp_ok){
                if(trend_up && brk_up){ pending=true; plong=true; eatr=atr; }
                else if(p.dir==1 && trend_dn && brk_dn){ pending=true; plong=false; eatr=atr; }
            }
        }
        prevc=b.c;
    }
    return R;
}

int main(int argc,char**argv){
    double cost = argc>1? atof(argv[1]) : 0.22;
    auto B=load("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.m5.csv");
    fprintf(stderr,"loaded %zu M5 bars  cost=$%.2f\n",B.size(),cost);
    long tmid=(B.front().ts+B.back().ts)/2;

    int emaF[]={0,20,20,50}; int emaS[]={0,50,100,200}; // paired
    int donchs[]={10,20,40};
    double imps[]={0.0,1.0};
    double bufs[]={0.0,0.1};
    double stops[]={1.0,1.5,2.0};
    // exit configs: mode,tp_R,trail
    struct EX{int m;double tp;double tr;} exs[]={{0,1.0,0},{0,1.5,0},{0,2.5,0},{1,2.0,2.0},{1,3.0,3.0},{2,0,0}};
    int bes[]={0,1};
    int dirs[]={0,1};
    int sesss[]={0};            // liquid-only (proven); add 1 via env
    int maxb=48;

    fprintf(stderr,"ROW,emaF,emaS,donch,imp,buf,stop,exitmode,tpR,trail,be,dir,sess,n,wr,pf,net,avgR,h1,h2,net_ex_top5,maxdd,worst\n");
    for(int e=0;e<4;e++)for(int d:donchs)for(double im:imps)for(double bf:bufs)for(double st:stops)
    for(auto&x:exs)for(int be:bes)for(int dr:dirs)for(int ss:sesss){
        P p{emaF[e],emaS[e],d,im,bf,st,x.m,x.tp,x.tr,be,1.0,dr,ss,maxb};
        Res R=run(B,p,cost,tmid);
        if(R.n<10) continue;
        double pf=R.gl>0?R.gw/R.gl:(R.gw>0?99:0);
        double wr=100.0*R.win/R.n;
        double avgR=R.sumR/R.n;
        double tops=R.top[0]+R.top[1]+R.top[2]+R.top[3]+R.top[4];
        fprintf(stderr,"ROW,%d,%d,%d,%.1f,%.1f,%.1f,%d,%.1f,%.1f,%d,%d,%d,%d,%.1f,%.2f,%.1f,%.3f,%.1f,%.1f,%.1f,%.1f,%.2f\n",
            emaF[e],emaS[e],d,im,bf,st,x.m,x.tp,x.tr,be,dr,ss,
            R.n,wr,pf,R.net,avgR,R.h1,R.h2,R.net-tops,R.maxdd,R.worst);
    }
    return 0;
}

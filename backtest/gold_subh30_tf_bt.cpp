// =============================================================================
// gold_subh30_tf_bt.cpp — SUB-30m (1m/5m/10m/15m) BOTH-WAYS gold mechanism
// study at the IBKR MGC cost basis. S-2026-07-14ba.
//
// Operator: "i also still want a shorter timeframe gold engine check 1m, 5m,
// 10 min, 15 min and show me results use same last 6 month data".
// Extends S-2026-07-14ax (backtest/gold_shorttf_bothways_bt.cpp, findings
// GOLD_SHORTTF_BOTHWAYS_2026H1_FINDINGS.md): same mechanisms, same grids,
// same cost model, same gate — pushed down to sub-30m bars.
//
// DATA (honest substitution): no MGC bars finer than 30m exist locally, so
// signals+fills use SPOT XAUUSD 1m bars (dukascopy/histdata splice, see
// backtest/xau_1m_splice_2026h1.py) with the MGC COST basis (0.41pt RT,
// $10/pt per 1 MGC). Spot/MGC levels track within a few pt of basis; point
// PnL on a trend mechanism is comparable. Stated in the findings.
//
// Mechanisms (symmetric long/short, identical logic to the ax harness):
//   KELT — Keltner(EMA20, k*ATR20) close-through breakout, 2xATR stop,
//          trail*ATR trail, 2-trading-day time stop (bar count scaled per TF).
//   DON  — Donchian Nin/Nout close-through, 3xATR adverse hard stop.
//   TF   — dual-EMA trend follow + 0.5*ATR(3-bar) impulse, 2xATR stop,
//          trail*ATR trail, reversal exit.
// No LOSS_CUT anywhere (registry §7: LC kills MGC intrabar); ATR stops are
// the protection. Adverse-first intrabar, GAP-HONEST fills (bar opening
// beyond the stop fills at the open).
//
// Windows: 6MO = 2026-01-14..2026-07-15, WF split 2026-04-14,
//          FULL = 2024-06-01.. (3mo indicator warmup discarded) for context.
// Cost: COST_RT env (default 0.41pt); every row also reports 2x stress net.
// Cost-share = total RT cost / gross winning pts (pre-cost) — how much of the
// raw edge the friction eats.
//
// BUILD: c++ -std=c++17 -O2 backtest/gold_subh30_tf_bt.cpp \
//            -o backtest/gold_subh30_tf_bt
// RUN:   ./backtest/gold_subh30_tf_bt /Users/jo/Tick/xau_1m_spliced_2024_2026.csv
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

static double COST_RT = 0.41;          // points round-trip, 1x basis
static const double USD_PER_PT = 10.0; // 1 MGC = 10oz, $10 per 1.0pt

struct Bar { int64_t ts; double o,h,l,c; };

static std::vector<Bar> load_csv(const char* path){
    std::vector<Bar> v; std::ifstream f(path);
    if(!f.is_open()){ std::fprintf(stderr,"cannot open %s\n",path); std::exit(1); }
    std::string line;
    while(std::getline(f,line)){
        Bar b; long long ts;
        if(std::sscanf(line.c_str(),"%lld,%lf,%lf,%lf,%lf",&ts,&b.o,&b.h,&b.l,&b.c)==5){
            b.ts=ts; v.push_back(b);
        }
    }
    return v;
}

static std::vector<Bar> resample(const std::vector<Bar>& m1, int minutes){
    if(minutes<=1) return m1;
    std::vector<Bar> out; int64_t step = 60LL*minutes;
    for(const auto& b : m1){
        int64_t bucket = (b.ts/step)*step;
        if(out.empty() || out.back().ts != bucket)
            out.push_back({bucket, b.o, b.h, b.l, b.c});
        else {
            Bar& x = out.back();
            x.h = std::max(x.h,b.h); x.l = std::min(x.l,b.l); x.c = b.c;
        }
    }
    return out;
}

// ---- indicators (identical to ax harness) -----------------------------------
static std::vector<double> ema(const std::vector<Bar>& bars, int n){
    std::vector<double> e(bars.size(), 0.0);
    if(bars.empty()) return e;
    double k = 2.0/(n+1); e[0]=bars[0].c;
    for(size_t i=1;i<bars.size();++i) e[i]=bars[i].c*k + e[i-1]*(1-k);
    return e;
}
static std::vector<double> atr_wilder(const std::vector<Bar>& bars, int n){
    std::vector<double> a(bars.size(), 0.0);
    if(bars.size()<2) return a;
    double acc=0;
    for(size_t i=1;i<bars.size();++i){
        double tr = std::max({bars[i].h-bars[i].l,
                              std::fabs(bars[i].h-bars[i-1].c),
                              std::fabs(bars[i].l-bars[i-1].c)});
        if((int)i<=n){ acc+=tr; a[i]=acc/(double)i; }
        else a[i]=(a[i-1]*(n-1)+tr)/n;
    }
    return a;
}
static void donchian(const std::vector<Bar>& bars, int n,
                     std::vector<double>& hh, std::vector<double>& ll){
    size_t N=bars.size(); hh.assign(N, 1e18); ll.assign(N, -1e18);
    // O(N) monotonic deques (1m series is ~900k bars)
    std::vector<int> qh, ql; size_t qh0=0, ql0=0;
    for(size_t i=0;i<N;++i){
        // maintain deques over the PRIOR n bars [i-n, i-1]: push i-1 on arrival at i
        if(i>0){
            int j=(int)i-1;
            while(qh0<qh.size() && bars[qh.back()].h <= bars[j].h) qh.pop_back();
            qh.push_back(j);
            while(ql0<ql.size() && bars[ql.back()].l >= bars[j].l) ql.pop_back();
            ql.push_back(j);
        }
        while(qh0<qh.size() && qh[qh0] < (int)i-n) qh0++;
        while(ql0<ql.size() && ql[ql0] < (int)i-n) ql0++;
        if((int)i>=n && qh0<qh.size() && ql0<ql.size()){
            hh[i]=bars[qh[qh0]].h; ll[i]=bars[ql[ql0]].l;
        }
    }
}

// ---- trade record / stats ---------------------------------------------------
struct Trade { int64_t ets, xts; int dir; double entry, exit, pts_net; };

struct Stat {
    int n=0, wins=0; double net=0, gw=0, gl=0, worst=0, peak=0, cur=0, mdd=0;
    double gw_pre=0;   // gross winning pts*$ BEFORE cost (cost-share denom)
    void rec(double usd, double usd_pre){
        n++; net+=usd; if(usd>=0){wins++; gw+=usd;} else gl+=-usd;
        if(usd_pre>0) gw_pre+=usd_pre;
        if(usd<worst) worst=usd;
        cur+=usd; if(cur>peak)peak=cur; if(peak-cur>mdd)mdd=peak-cur;
    }
    double pf() const { return gl>0? gw/gl : (gw>0?999.0:0.0); }
    double wr() const { return n? 100.0*wins/n : 0.0; }
};

// window bounds (UTC seconds) — same as ax harness
static const int64_t T_FULL0 = 1717200000; // 2024-06-01 (post-warmup, data 2024-03..)
static const int64_t T_6MO0  = 1768348800; // 2026-01-14
static const int64_t T_MID   = 1776124800; // 2026-04-14
static const int64_t T_END   = 1784073600; // 2026-07-15

struct Report {
    const char* tf; const char* mech; std::string cfg;
    Stat full, w6, wf1, wf2, w6_2x, lng, sht;
};

static void finish(Report& r, const std::vector<Trade>& trades){
    for(const auto& t : trades){
        double pre  = t.dir*(t.exit - t.entry) * USD_PER_PT;   // before cost
        double usd1 = t.pts_net * USD_PER_PT;
        double usd2 = (t.pts_net - COST_RT) * USD_PER_PT;      // 2x total cost
        if(t.ets >= T_FULL0) r.full.rec(usd1, pre);
        if(t.ets >= T_6MO0 && t.ets < T_END){
            r.w6.rec(usd1, pre); r.w6_2x.rec(usd2, pre);
            if(t.ets < T_MID) r.wf1.rec(usd1, pre); else r.wf2.rec(usd1, pre);
            if(t.dir>0) r.lng.rec(usd1, pre); else r.sht.rec(usd1, pre);
        }
    }
}

// ---- generic position shell (identical to ax) --------------------------------
struct Pos {
    bool active=false; int dir=0;
    double entry=0, stop=0, tp=0, extreme=0;
    int64_t ets=0; int bars_held=0;
    bool has_tp=false;
};

static void close_trade(std::vector<Trade>& out, Pos& p, double px, int64_t ts){
    double pts = p.dir * (px - p.entry) - COST_RT;
    out.push_back({p.ets, ts, p.dir, p.entry, px, pts});
    p.active=false;
}

static bool manage_intrabar(std::vector<Trade>& out, Pos& p, const Bar& b){
    if(!p.active) return false;
    if(p.dir>0){
        if(b.o <= p.stop){ close_trade(out,p,b.o,b.ts); return true; }
        if(b.l <= p.stop){ close_trade(out,p,p.stop,b.ts); return true; }
        if(p.has_tp && b.o >= p.tp){ close_trade(out,p,b.o,b.ts); return true; }
        if(p.has_tp && b.h >= p.tp){ close_trade(out,p,p.tp,b.ts); return true; }
    } else {
        if(b.o >= p.stop){ close_trade(out,p,b.o,b.ts); return true; }
        if(b.h >= p.stop){ close_trade(out,p,p.stop,b.ts); return true; }
        if(p.has_tp && b.o <= p.tp){ close_trade(out,p,b.o,b.ts); return true; }
        if(p.has_tp && b.l <= p.tp){ close_trade(out,p,p.tp,b.ts); return true; }
    }
    return false;
}

// ---- mechanisms (logic identical to ax harness) ------------------------------
static std::vector<Trade> run_kelt(const std::vector<Bar>& bars, double k,
                                   double trail, int tmax_bars){
    auto mid=ema(bars,20), atr=atr_wilder(bars,20);
    std::vector<Trade> out; Pos p;
    for(size_t i=100;i<bars.size();++i){
        const Bar& b=bars[i];
        if(p.active){
            p.bars_held++;
            if(manage_intrabar(out,p,b)) goto entry;
            if(p.dir>0){ p.extreme=std::max(p.extreme,b.h);
                p.stop=std::max(p.stop,p.extreme-trail*atr[i]); }
            else { p.extreme=std::min(p.extreme,b.l);
                p.stop=std::min(p.stop,p.extreme+trail*atr[i]); }
            if(p.active && p.bars_held>=tmax_bars) close_trade(out,p,b.c,b.ts);
        }
entry:
        if(!p.active && atr[i]>0 && i>0){
            double up=mid[i]+k*atr[i], dn=mid[i]-k*atr[i];
            double pup=mid[i-1]+k*atr[i-1], pdn=mid[i-1]-k*atr[i-1];
            bool lg = b.c>up && bars[i-1].c<=pup;
            bool sh = b.c<dn && bars[i-1].c>=pdn;
            if(lg||sh){
                p.active=true; p.dir=lg?+1:-1; p.entry=b.c; p.ets=b.ts;
                p.extreme=b.c; p.bars_held=0; p.has_tp=false;
                p.stop=b.c-p.dir*2.0*atr[i];
            }
        }
    }
    if(p.active) close_trade(out,p,bars.back().c,bars.back().ts);
    return out;
}

static std::vector<Trade> run_don(const std::vector<Bar>& bars, int nin, int nout){
    std::vector<double> hhin,llin,hhout,llout;
    donchian(bars,nin,hhin,llin); donchian(bars,nout,hhout,llout);
    auto atr=atr_wilder(bars,14);
    std::vector<Trade> out; Pos p;
    for(size_t i=(size_t)nin+1;i<bars.size();++i){
        const Bar& b=bars[i];
        if(p.active){
            p.bars_held++;
            if(manage_intrabar(out,p,b)) goto entry;
            if(p.dir>0 && b.c<llout[i]) close_trade(out,p,b.c,b.ts);
            else if(p.dir<0 && b.c>hhout[i]) close_trade(out,p,b.c,b.ts);
        }
entry:
        if(!p.active && atr[i]>0 && hhin[i]<1e17){
            bool lg = b.c>hhin[i];
            bool sh = b.c<llin[i];
            if(lg||sh){
                p.active=true; p.dir=lg?+1:-1; p.entry=b.c; p.ets=b.ts;
                p.extreme=b.c; p.has_tp=false; p.bars_held=0;
                p.stop=b.c-p.dir*3.0*atr[i];
            }
        }
    }
    if(p.active) close_trade(out,p,bars.back().c,bars.back().ts);
    return out;
}

static std::vector<Trade> run_tf(const std::vector<Bar>& bars, int fN, int sN,
                                 double trail_mult, double imp_atr){
    auto ef=ema(bars,fN), es=ema(bars,sN), atr=atr_wilder(bars,14);
    std::vector<Trade> out; Pos p;
    int warm = std::max(sN*3, 100);
    for(size_t i=(size_t)warm;i<bars.size();++i){
        const Bar& b=bars[i];
        if(p.active){
            p.bars_held++;
            if(manage_intrabar(out,p,b)) goto entry;
            if(p.dir>0){ p.extreme=std::max(p.extreme,b.h);
                p.stop=std::max(p.stop, p.extreme - trail_mult*atr[i]); }
            else { p.extreme=std::min(p.extreme,b.l);
                p.stop=std::min(p.stop, p.extreme + trail_mult*atr[i]); }
            if((p.dir>0 && ef[i]<es[i]) || (p.dir<0 && ef[i]>es[i]))
                close_trade(out,p,b.c,b.ts);
        }
entry:
        if(!p.active && i>=3 && atr[i]>0){
            double impulse=b.c-bars[i-3].c;
            bool lg = ef[i]>es[i] && b.c>ef[i] && impulse>= imp_atr*atr[i];
            bool sh = ef[i]<es[i] && b.c<ef[i] && impulse<=-imp_atr*atr[i];
            if(lg||sh){
                p.active=true; p.dir=lg?+1:-1; p.entry=b.c; p.ets=b.ts;
                p.extreme=b.c; p.has_tp=false; p.bars_held=0;
                p.stop=b.c - p.dir*2.0*atr[i];
            }
        }
    }
    if(p.active) close_trade(out,p,bars.back().c,bars.back().ts);
    return out;
}

// =============================================================================
static void print_report(Report& r){
    auto& w=r.w6;
    double wk = w.n/26.0;
    double cost1x = w.n * COST_RT * USD_PER_PT;
    double cshare = w.gw_pre>0 ? 100.0*cost1x/w.gw_pre : 999.0;
    bool pass = w.net>0 && r.w6_2x.net>0 && w.pf()>=1.3
                && r.wf1.net>0 && r.wf2.net>0;
    bool legs = r.lng.net>0 && r.sht.net>0;
    std::printf("%-3s %-4s %-18s | n=%-5d (%5.1f/wk) net1x=$%+9.0f net2x=$%+9.0f PF=%5.2f WR=%4.1f%% cost%%=%5.1f worst=$%+7.0f DD=$%7.0f | L $%+8.0f(n%d) S $%+8.0f(n%d) | WF $%+7.0f/$%+7.0f | %s%s | FULL n=%d $%+.0f PF %.2f\n",
        r.tf, r.mech, r.cfg.c_str(),
        w.n, wk, w.net, r.w6_2x.net, w.pf(), w.wr(), cshare, w.worst, w.mdd,
        r.lng.net, r.lng.n, r.sht.net, r.sht.n,
        r.wf1.net, r.wf2.net,
        pass?"GATE-PASS":"fail",
        pass? (legs?"+LEGS":"(leg-)") : "",
        r.full.n, r.full.net, r.full.pf());
}

int main(int argc, char** argv){
    const char* path = argc>1? argv[1] : "/Users/jo/Tick/xau_1m_spliced_2024_2026.csv";
    if(getenv("COST_RT")) COST_RT=atof(getenv("COST_RT"));
    auto m1=load_csv(path);
    std::printf("[GOLD-SUB30M] 1m bars=%zu  COST_RT=%.2fpt ($%.2f/RT per 1 MGC)  2x=%.2fpt  (SPOT bars, MGC cost basis)\n",
        m1.size(), COST_RT, COST_RT*USD_PER_PT, 2*COST_RT);

    int tfs[4] = {1,5,10,15};
    const char* tfn[4] = {"1m","5m","10m","15m"};
    char buf[128];
    for(int t=0;t<4;t++){
        auto bars = resample(m1, tfs[t]);
        int tmax = 2*1440/tfs[t];   // 2-trading-day time stop, scaled per TF
        std::printf("---- TF %s: %zu bars, KELT time stop %d bars ----\n",
                    tfn[t], bars.size(), tmax);
        // KELT grid (ax winner family): k x trail
        for(double k : {1.0,1.25,1.5}) for(double tr : {2.0,2.5,3.0}){
            Report r; r.tf=tfn[t]; r.mech="KELT";
            std::snprintf(buf,sizeof buf,"k%.2f trail%.1f",k,tr);
            r.cfg=buf; finish(r, run_kelt(bars,k,tr,tmax)); print_report(r);
        }
        // DON grid
        struct { int in,out; } dons[] = {{20,10},{40,20},{55,27}};
        for(auto& d : dons){
            Report r; r.tf=tfn[t]; r.mech="DON";
            std::snprintf(buf,sizeof buf,"%d/%d stop3ATR",d.in,d.out);
            r.cfg=buf; finish(r, run_don(bars,d.in,d.out)); print_report(r);
        }
        // TF grid
        struct { int f,s; } emas[] = {{10,40},{20,100},{10,20}};
        for(auto& e : emas) for(double tr : {2.0,2.5}){
            Report r; r.tf=tfn[t]; r.mech="TF";
            std::snprintf(buf,sizeof buf,"ema%d/%d trail%.1f",e.f,e.s,tr);
            r.cfg=buf; finish(r, run_tf(bars,e.f,e.s,tr,0.5)); print_report(r);
        }
        // DON15_SWEEP=1: fine neighbourhood sweep around the 15m DON 55/27
        // GATE-PASS (plateau-or-fluke check, findings caveat 2).
        if(tfs[t]==15 && getenv("DON15_SWEEP")){
            std::printf("---- DON 15m fine sweep (Nin x Nout) ----\n");
            for(int nin : {45,50,55,60,65,70})
                for(int nout : {20,23,27,31,35}){
                    Report r; r.tf=tfn[t]; r.mech="DONf";
                    std::snprintf(buf,sizeof buf,"%d/%d stop3ATR",nin,nout);
                    r.cfg=buf; finish(r, run_don(bars,nin,nout)); print_report(r);
                }
        }
    }
    return 0;
}

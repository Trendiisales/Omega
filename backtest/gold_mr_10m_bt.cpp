// =============================================================================
// gold_mr_10m_bt.cpp — MEAN-REVERSION / FADE gold mechanism study at 10m (5m/15m
// context), IBKR MGC cost basis. S-2026-07-15u.
//
// WHY: the sub-30m trend/breakout study (gold_subh30_tf_bt.cpp,
// GOLD_SUB30M_2026H1_FINDINGS.md) swept ONLY trend families (Keltner/Donchian/
// dual-EMA TF) and explicitly left the "ORB-family/scalp hands-off list
// untouched" (findings L70). Trend at 10m is exhausted: KELT dead, TF no-pass,
// only the slow-exit DON survived -> already wired (GoldDon10m_30_35, S-14bi).
// This harness tests the MECHANISTICALLY OPPOSITE class -- mean-reversion / fade
// -- which (a) was never swept sub-30m and (b) the existing gold MR/scalp/ORB
// engines were all judged under the OLD pessimistic SPOT cost (1.60pt RT); the
// MGC futures basis is 4x cheaper (0.41pt RT), which already flipped one dead
// trend family (KELT M30) dead->viable. A legitimate NEW-basis re-open.
// A fade book would be anti-correlated to the wired DON10m breakout -> diversifying.
//
// DATA (same honest substitution as the sub-30m study): no MGC bars <30m exist,
// so signals+fills use SPOT XAUUSD 1m bars (certified splice
// /Users/jo/Tick/xau_1m_spliced_2024_2026.csv) at the MGC cost basis (0.41pt RT,
// $10/pt per 1 MGC). Stated in the findings.
//
// MECHANISMS (symmetric long/short FADE):
//   BBFADE  — close beyond Bollinger(n, k*sd) -> fade back to the (moving) SMA
//             mean (has_tp). Adverse-first ATR stop, time stop.
//   ZREV    — z=(c-SMA)/sd; |z|>=z_in against the move -> enter; exit when z
//             reverts to |z|<=z_out. ATR stop, time stop.
//   KFADE   — close beyond Keltner(EMA20, k*ATR20) -> fade back to EMA20 mean.
//             (the exact OPPOSITE of the run_kelt breakout entry.)
//   RSI2    — Connors RSI(2): <lo -> long, >hi -> short; exit on RSI cross 50.
//             ATR stop, time stop.
// No LOSS_CUT (registry §7: LC kills MGC intrabar); ATR stops are the protection,
// adverse-first intrabar, GAP-HONEST fills (bar opening beyond stop fills at open).
// MR time stop is SHORT by design (a fade that hasn't reverted = wrong thesis).
//
// Windows / cost / gate: IDENTICAL to gold_subh30_tf_bt.cpp (the certified all-6):
//   PASS = net>0 @1x AND @2x, PF>=1.3, both WF halves +, both legs +.
//
// BUILD: c++ -std=c++17 -O2 backtest/gold_mr_10m_bt.cpp -o backtest/gold_mr_10m_bt
// RUN:   ./backtest/gold_mr_10m_bt /Users/jo/Tick/xau_1m_spliced_2024_2026.csv
//        MR_TMAX=<bars>  COST_RT=<pt>  FINE=1 (wider grid)
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
static int MR_TMAX = 36;               // MR time stop in BARS (default 36 = 6h @10m)

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

// ---- indicators -------------------------------------------------------------
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
// rolling SMA + population std over the trailing n closes (O(N))
static void sma_std(const std::vector<Bar>& bars, int n,
                    std::vector<double>& sma, std::vector<double>& sd){
    size_t N=bars.size(); sma.assign(N,0.0); sd.assign(N,0.0);
    double s=0, s2=0;
    for(size_t i=0;i<N;++i){
        double c=bars[i].c; s+=c; s2+=c*c;
        if((int)i>=n){ double o=bars[i-n].c; s-=o; s2-=o*o; }
        int cnt = (int)i+1 < n ? (int)i+1 : n;
        if((int)i>=n-1){
            double m=s/cnt; double var=s2/cnt - m*m; if(var<0)var=0;
            sma[i]=m; sd[i]=std::sqrt(var);
        }
    }
}
// Wilder RSI(n)
static std::vector<double> rsi_wilder(const std::vector<Bar>& bars, int n){
    size_t N=bars.size(); std::vector<double> r(N,50.0);
    if(N<2) return r;
    double ag=0, al=0;
    for(size_t i=1;i<N;++i){
        double ch=bars[i].c-bars[i-1].c, g=ch>0?ch:0, l=ch<0?-ch:0;
        if((int)i<=n){ ag+=g; al+=l; if((int)i==n){ ag/=n; al/=n; r[i]= al<1e-12?100.0:100.0-100.0/(1.0+ag/al);} }
        else { ag=(ag*(n-1)+g)/n; al=(al*(n-1)+l)/n; r[i]= al<1e-12?100.0:100.0-100.0/(1.0+ag/al); }
    }
    return r;
}

// ---- trade record / stats (identical to sub-30m harness) --------------------
struct Trade { int64_t ets, xts; int dir; double entry, exit, pts_net; };
struct Stat {
    int n=0, wins=0; double net=0, gw=0, gl=0, worst=0, peak=0, cur=0, mdd=0, gw_pre=0;
    void rec(double usd, double usd_pre){
        n++; net+=usd; if(usd>=0){wins++; gw+=usd;} else gl+=-usd;
        if(usd_pre>0) gw_pre+=usd_pre;
        if(usd<worst) worst=usd;
        cur+=usd; if(cur>peak)peak=cur; if(peak-cur>mdd)mdd=peak-cur;
    }
    double pf() const { return gl>0? gw/gl : (gw>0?999.0:0.0); }
    double wr() const { return n? 100.0*wins/n : 0.0; }
};
static const int64_t T_FULL0 = 1717200000; // 2024-06-01
static const int64_t T_6MO0  = 1768348800; // 2026-01-14
static const int64_t T_MID   = 1776124800; // 2026-04-14
static const int64_t T_END   = 1784073600; // 2026-07-15
struct Report {
    const char* tf; const char* mech; std::string cfg;
    Stat full, w6, wf1, wf2, w6_2x, lng, sht;
};
static void finish(Report& r, const std::vector<Trade>& trades){
    for(const auto& t : trades){
        double pre  = t.dir*(t.exit - t.entry) * USD_PER_PT;
        double usd1 = t.pts_net * USD_PER_PT;
        double usd2 = (t.pts_net - COST_RT) * USD_PER_PT;
        if(t.ets >= T_FULL0) r.full.rec(usd1, pre);
        if(t.ets >= T_6MO0 && t.ets < T_END){
            r.w6.rec(usd1, pre); r.w6_2x.rec(usd2, pre);
            if(t.ets < T_MID) r.wf1.rec(usd1, pre); else r.wf2.rec(usd1, pre);
            if(t.dir>0) r.lng.rec(usd1, pre); else r.sht.rec(usd1, pre);
        }
    }
}

// ---- position shell (identical intrabar/gap-honest logic) --------------------
struct Pos {
    bool active=false; int dir=0;
    double entry=0, stop=0, tp=0; int64_t ets=0; int bars_held=0; bool has_tp=false;
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

// ---- MEAN-REVERSION mechanisms (both-ways FADE) ------------------------------
// BBFADE: close beyond Bollinger band -> fade to the moving SMA mean.
static std::vector<Trade> run_bbfade(const std::vector<Bar>& bars, int n, double k,
                                     double stop_mult){
    std::vector<double> sma,sd; sma_std(bars,n,sma,sd);
    auto atr=atr_wilder(bars,14);
    std::vector<Trade> out; Pos p;
    for(size_t i=(size_t)n+1;i<bars.size();++i){
        const Bar& b=bars[i];
        if(p.active){
            p.bars_held++;
            p.tp = sma[i];                       // target the moving mean
            if(manage_intrabar(out,p,b)) goto entry;
            if(p.bars_held>=MR_TMAX) close_trade(out,p,b.c,b.ts);
        }
entry:
        if(!p.active && atr[i]>0 && sd[i]>0){
            double up=sma[i]+k*sd[i], dn=sma[i]-k*sd[i];
            double pup=sma[i-1]+k*sd[i-1], pdn=sma[i-1]-k*sd[i-1];
            bool sh = b.c>up && bars[i-1].c<=pup;   // above upper -> FADE short
            bool lg = b.c<dn && bars[i-1].c>=pdn;   // below lower -> FADE long
            if(lg||sh){
                p.active=true; p.dir=lg?+1:-1; p.entry=b.c; p.ets=b.ts;
                p.bars_held=0; p.has_tp=true; p.tp=sma[i];
                p.stop=b.c - p.dir*stop_mult*atr[i];
            }
        }
    }
    if(p.active) close_trade(out,p,bars.back().c,bars.back().ts);
    return out;
}

// KFADE: close beyond Keltner band -> fade to EMA20 mean (opposite of run_kelt).
static std::vector<Trade> run_kfade(const std::vector<Bar>& bars, double k,
                                    double stop_mult){
    auto mid=ema(bars,20), atr=atr_wilder(bars,20);
    std::vector<Trade> out; Pos p;
    for(size_t i=100;i<bars.size();++i){
        const Bar& b=bars[i];
        if(p.active){
            p.bars_held++;
            p.tp = mid[i];
            if(manage_intrabar(out,p,b)) goto entry;
            if(p.bars_held>=MR_TMAX) close_trade(out,p,b.c,b.ts);
        }
entry:
        if(!p.active && atr[i]>0){
            double up=mid[i]+k*atr[i], dn=mid[i]-k*atr[i];
            double pup=mid[i-1]+k*atr[i-1], pdn=mid[i-1]-k*atr[i-1];
            bool sh = b.c>up && bars[i-1].c<=pup;
            bool lg = b.c<dn && bars[i-1].c>=pdn;
            if(lg||sh){
                p.active=true; p.dir=lg?+1:-1; p.entry=b.c; p.ets=b.ts;
                p.bars_held=0; p.has_tp=true; p.tp=mid[i];
                p.stop=b.c - p.dir*stop_mult*atr[i];
            }
        }
    }
    if(p.active) close_trade(out,p,bars.back().c,bars.back().ts);
    return out;
}

// ZREV: |z|>=z_in against the move -> enter; exit when z reverts to |z|<=z_out.
static std::vector<Trade> run_zrev(const std::vector<Bar>& bars, int n, double z_in,
                                   double z_out, double stop_mult){
    std::vector<double> sma,sd; sma_std(bars,n,sma,sd);
    auto atr=atr_wilder(bars,14);
    std::vector<Trade> out; Pos p;
    for(size_t i=(size_t)n+1;i<bars.size();++i){
        const Bar& b=bars[i];
        double z = sd[i]>0 ? (b.c-sma[i])/sd[i] : 0.0;
        if(p.active){
            p.bars_held++;
            if(manage_intrabar(out,p,b)) goto entry;           // ATR stop only (no tp)
            bool reverted = (p.dir>0 && z >= -z_out) || (p.dir<0 && z <= z_out);
            if(p.active && (reverted || p.bars_held>=MR_TMAX))
                close_trade(out,p,b.c,b.ts);
        }
entry:
        if(!p.active && atr[i]>0 && sd[i]>0){
            bool sh = z >=  z_in;   // stretched high -> fade short
            bool lg = z <= -z_in;   // stretched low  -> fade long
            if(lg||sh){
                p.active=true; p.dir=lg?+1:-1; p.entry=b.c; p.ets=b.ts;
                p.bars_held=0; p.has_tp=false;
                p.stop=b.c - p.dir*stop_mult*atr[i];
            }
        }
    }
    if(p.active) close_trade(out,p,bars.back().c,bars.back().ts);
    return out;
}

// RSI2: Connors RSI(2) reversion; exit on RSI cross 50.
static std::vector<Trade> run_rsi2(const std::vector<Bar>& bars, double lo, double hi,
                                   double stop_mult){
    auto r=rsi_wilder(bars,2); auto atr=atr_wilder(bars,14);
    std::vector<Trade> out; Pos p;
    for(size_t i=15;i<bars.size();++i){
        const Bar& b=bars[i];
        if(p.active){
            p.bars_held++;
            if(manage_intrabar(out,p,b)) goto entry;
            bool reverted = (p.dir>0 && r[i]>=50.0) || (p.dir<0 && r[i]<=50.0);
            if(p.active && (reverted || p.bars_held>=MR_TMAX))
                close_trade(out,p,b.c,b.ts);
        }
entry:
        if(!p.active && atr[i]>0){
            bool lg = r[i] < lo;    // oversold -> long
            bool sh = r[i] > hi;    // overbought -> short
            if(lg||sh){
                p.active=true; p.dir=lg?+1:-1; p.entry=b.c; p.ets=b.ts;
                p.bars_held=0; p.has_tp=false;
                p.stop=b.c - p.dir*stop_mult*atr[i];
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
    bool pass = w.net>0 && r.w6_2x.net>0 && w.pf()>=1.3 && r.wf1.net>0 && r.wf2.net>0;
    bool legs = r.lng.net>0 && r.sht.net>0;
    std::printf("%-3s %-6s %-16s | n=%-5d (%5.1f/wk) net1x=$%+9.0f net2x=$%+9.0f PF=%5.2f WR=%4.1f%% cost%%=%5.1f worst=$%+7.0f DD=$%7.0f | L $%+8.0f(n%d) S $%+8.0f(n%d) | WF $%+7.0f/$%+7.0f | %s%s | FULL n=%d $%+.0f PF %.2f\n",
        r.tf, r.mech, r.cfg.c_str(),
        w.n, wk, w.net, r.w6_2x.net, w.pf(), w.wr(), cshare, w.worst, w.mdd,
        r.lng.net, r.lng.n, r.sht.net, r.sht.n, r.wf1.net, r.wf2.net,
        pass?"GATE-PASS":"fail", pass?(legs?"+LEGS":"(leg-)"):"",
        r.full.n, r.full.net, r.full.pf());
}

int main(int argc, char** argv){
    const char* path = argc>1? argv[1] : "/Users/jo/Tick/xau_1m_spliced_2024_2026.csv";
    if(getenv("COST_RT")) COST_RT=atof(getenv("COST_RT"));
    if(getenv("MR_TMAX")) MR_TMAX=atoi(getenv("MR_TMAX"));
    bool FINE = getenv("FINE") && atoi(getenv("FINE"))!=0;
    auto m1=load_csv(path);
    std::printf("[GOLD-MR] 1m bars=%zu  COST_RT=%.2fpt ($%.2f/RT per 1 MGC)  2x=%.2fpt  MR_TMAX=%d bars  (SPOT bars, MGC cost basis)\n",
        m1.size(), COST_RT, COST_RT*USD_PER_PT, 2*COST_RT, MR_TMAX);
    std::printf("gate: net>0 @1x AND @2x, PF>=1.3, both WF halves +, both legs + (identical to sub-30m trend study)\n");

    int tfs[3] = {5,10,15};
    const char* tfn[3] = {"5m","10m","15m"};
    char buf[128];
    for(int t=0;t<3;t++){
        auto bars = resample(m1, tfs[t]);
        std::printf("---- TF %s: %zu bars, MR time stop %d bars (%.1fh) ----\n",
                    tfn[t], bars.size(), MR_TMAX, MR_TMAX*tfs[t]/60.0);
        // BBFADE grid
        for(int n : (FINE? std::vector<int>{15,20,30,40,50} : std::vector<int>{20,40}))
            for(double k : (FINE? std::vector<double>{1.5,2.0,2.5,3.0} : std::vector<double>{2.0,2.5,3.0}))
                for(double sm : {2.5,3.0}){
                    Report r; r.tf=tfn[t]; r.mech="BBFADE";
                    std::snprintf(buf,sizeof buf,"n%d k%.1f s%.1f",n,k,sm);
                    r.cfg=buf; finish(r, run_bbfade(bars,n,k,sm)); print_report(r);
                }
        // KFADE grid
        for(double k : {1.5,2.0,2.5}) for(double sm : {2.5,3.0}){
            Report r; r.tf=tfn[t]; r.mech="KFADE";
            std::snprintf(buf,sizeof buf,"k%.1f s%.1f",k,sm);
            r.cfg=buf; finish(r, run_kfade(bars,k,sm)); print_report(r);
        }
        // ZREV grid
        for(int n : {20,50}) for(double zi : {2.0,2.5}) for(double zo : {0.5,0.0}){
            Report r; r.tf=tfn[t]; r.mech="ZREV";
            std::snprintf(buf,sizeof buf,"n%d zi%.1f zo%.1f",n,zi,zo);
            r.cfg=buf; finish(r, run_zrev(bars,n,zi,zo,3.0)); print_report(r);
        }
        // RSI2 grid
        for(auto lh : std::vector<std::pair<double,double>>{{10,90},{5,95}}){
            Report r; r.tf=tfn[t]; r.mech="RSI2";
            std::snprintf(buf,sizeof buf,"lo%.0f hi%.0f",lh.first,lh.second);
            r.cfg=buf; finish(r, run_rsi2(bars,lh.first,lh.second,3.0)); print_report(r);
        }
    }
    return 0;
}

// =============================================================================
// gold_shorttf_bothways_bt.cpp — short-timeframe (M30/H1) BOTH-WAYS gold
// mechanism study at the IBKR MGC cost basis. S-2026-07-14ax.
//
// Question (operator): with MGC round-trip ~0.41pt (vs spot ~1.4pt), what
// quicker-turnover gold mechanism is viable LONG *and* SHORT over the very
// volatile last 6 months (2026-01-14 .. 2026-07-14)?
//
// Mechanisms (all symmetric long/short):
//   TF1H   — H1 EMA trend-follow (XauTrendFollow-family style): EMA fast/slow
//            alignment + ATR impulse entry, 2xATR stop, ATR trail, reversal exit.
//            LOSS_CUT deliberately absent (registry §7 trap: spot LC kills MGC).
//   KELT   — M30 Keltner(EMA20, k*ATR20) close-through breakout; exits fixed-R
//            (2xATR stop, TP R multiple) or ATR trail. Re-open of the 2026-06-20
//            spot-cost falsification at the new futures cost basis.
//   DON    — Donchian Nin/Nout close-through breakout, M30 and H1, with 3xATR
//            adverse hard stop (MgcFastDonchian-family protection).
//   VXF    — H1 vol-expansion FADE: bar range > k*ATR closing near its extreme
//            -> fade toward mean; hard stop 1.5xATR beyond the spike extreme,
//            fixed ATR take-profit, 12-bar time stop.
//   VXB    — same trigger traded WITH the spike (continuation), trail exit.
//
// FIDELITY:
//   - data: backtest/data/mgc_30m_spliced_2024_2026.csv (ts,o,h,l,c; ts sec)
//     CERTIFIED CLEAN by data_integrity_gate.py 2026-07-14 (see mgc_30m_splice.py).
//   - signals evaluated on BAR CLOSE; position managed from the NEXT bar.
//   - intrabar ADVERSE-FIRST: stop checked before target within a bar (both hit
//     in one bar => stop fill, conservative). Trail/reversal evaluated at close.
//   - stop fills AT the stop level; slippage is inside the RT cost figure.
//   - cost: fixed COST_RT points per round trip, default 0.41
//     ($2.08 MGC commission = 0.208pt + 0.10 spread + 0.10 slip; certified
//     GoldExecSpreadBasis). 2x stress = 0.82. $ = pts * $10 per 1 MGC (10oz).
//   - windows: FULL = 2024-09-01.. (3mo indicator warmup discarded);
//     6MO = 2026-01-14..2026-07-14; WF halves at 2026-04-14.
//
// BUILD: c++ -std=c++17 -O2 backtest/gold_shorttf_bothways_bt.cpp \
//            -o backtest/gold_shorttf_bothways_bt
// RUN:   ./backtest/gold_shorttf_bothways_bt [csv]   (env COST_RT=0.41)
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

static double COST_RT = 0.41;         // points round-trip, 1x basis
static const double USD_PER_PT = 10.0; // 1 MGC = 10oz, $10 per 1.0pt
// S-2026-07-14bb (mimic validation): env DUMP_ENTRIES=1 -> print one
// "ENTRY|mech|cfg|dir|entry_px|entry_ts" line per trade so the BE-mimic
// harness (gold_newengine_mimic_bt.cpp) can consume the REAL entry stream
// instead of re-implementing signals. Default OFF -> output unchanged.
static bool DUMP_ENTRIES = false;

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

static std::vector<Bar> to_h1(const std::vector<Bar>& m30){
    std::vector<Bar> out;
    for(const auto& b : m30){
        int64_t bucket = (b.ts/3600)*3600;
        if(out.empty() || out.back().ts != bucket){
            out.push_back({bucket, b.o, b.h, b.l, b.c});
        } else {
            Bar& x = out.back();
            x.h = std::max(x.h, b.h); x.l = std::min(x.l, b.l); x.c = b.c;
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
// rolling highest-high / lowest-low of the PRIOR n bars (excludes current)
static void donchian(const std::vector<Bar>& bars, int n,
                     std::vector<double>& hh, std::vector<double>& ll){
    size_t N=bars.size(); hh.assign(N, 1e18); ll.assign(N, -1e18);
    for(size_t i=0;i<N;++i){
        if((int)i<n){ continue; }
        double h=-1e18,l=1e18;
        for(int j=1;j<=n;++j){ h=std::max(h,bars[i-j].h); l=std::min(l,bars[i-j].l); }
        hh[i]=h; ll[i]=l;
    }
}

// ---- trade record / stats ---------------------------------------------------
struct Trade { int64_t ets, xts; int dir; double entry, exit, pts_net; };

struct Stat {
    int n=0, wins=0; double net=0, gw=0, gl=0, worst=0, peak=0, cur=0, mdd=0;
    void rec(double usd){
        n++; net+=usd; if(usd>=0){wins++; gw+=usd;} else gl+=-usd;
        if(usd<worst) worst=usd;
        cur+=usd; if(cur>peak)peak=cur; if(peak-cur>mdd)mdd=peak-cur;
    }
    double pf() const { return gl>0? gw/gl : (gw>0?999.0:0.0); }
    double wr() const { return n? 100.0*wins/n : 0.0; }
};

// window bounds (UTC seconds)
static const int64_t T_FULL0 = 1725148800; // 2024-09-01 (post-warmup)
static const int64_t T_6MO0  = 1768348800; // 2026-01-14
static const int64_t T_MID   = 1776124800; // 2026-04-14
static const int64_t T_END   = 1784073600; // 2026-07-15

struct Report {
    const char* mech; std::string cfg;
    Stat full, w6, wf1, wf2, w6_2x, lng, sht, flng, fsht;
    double monthly[7] = {0,0,0,0,0,0,0}; // Jan14-, Feb, Mar, Apr, May, Jun, Jul
    int    monthn[7]  = {0,0,0,0,0,0,0};
};

static int month_slot(int64_t ts){
    // slots: 0:[01-14,02-01) 1:Feb 2:Mar 3:Apr 4:May 5:Jun 6:Jul(..07-15)
    static const int64_t edges[8] = {
        1768348800, // 2026-01-14
        1769904000, // 2026-02-01
        1772323200, // 2026-03-01
        1774998400, // 2026-04-01 (1774998000? computed below in main check)
        1777590000, // placeholder (fixed in main)
        0,0,0 };
    (void)edges;
    // robust: derive from civil date instead
    time_t t=(time_t)ts; struct tm g; gmtime_r(&t,&g);
    if(g.tm_year+1900!=2026) return -1;
    int m=g.tm_mon+1; // 1..12
    if(m==1) return (g.tm_mday>=14)?0:-1;
    if(m>=2 && m<=7) return m-1;
    return -1;
}

static void finish(Report& r, const std::vector<Trade>& trades){
    if(DUMP_ENTRIES)
        for(const auto& t : trades)
            std::printf("ENTRY|%s|%s|%+d|%.2f|%lld\n",
                        r.mech, r.cfg.c_str(), t.dir, t.entry, (long long)t.ets);
    for(const auto& t : trades){
        double usd1 = (t.pts_net) * USD_PER_PT;
        double usd2 = (t.pts_net - COST_RT) * USD_PER_PT; // extra cost => 2x total
        if(t.ets >= T_FULL0){ r.full.rec(usd1);
            if(t.dir>0) r.flng.rec(usd1); else r.fsht.rec(usd1); }
        if(t.ets >= T_6MO0 && t.ets < T_END){
            r.w6.rec(usd1); r.w6_2x.rec(usd2);
            if(t.ets < T_MID) r.wf1.rec(usd1); else r.wf2.rec(usd1);
            if(t.dir>0) r.lng.rec(usd1); else r.sht.rec(usd1);
            int s=month_slot(t.ets); if(s>=0){ r.monthly[s]+=usd1; r.monthn[s]++; }
        }
    }
}

// ---- generic position shell -------------------------------------------------
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

// intrabar manage: adverse-first, GAP-HONEST (a bar opening beyond the stop
// fills at the open, not the stop level). returns true if exited.
static bool manage_intrabar(std::vector<Trade>& out, Pos& p, const Bar& b){
    if(!p.active) return false;
    if(p.dir>0){
        if(b.o <= p.stop){ close_trade(out,p,b.o,b.ts); return true; }   // gap through stop
        if(b.l <= p.stop){ close_trade(out,p,p.stop,b.ts); return true; }
        if(p.has_tp && b.o >= p.tp){ close_trade(out,p,b.o,b.ts); return true; } // favorable gap: real fill = open
        if(p.has_tp && b.h >= p.tp){ close_trade(out,p,p.tp,b.ts); return true; }
    } else {
        if(b.o >= p.stop){ close_trade(out,p,b.o,b.ts); return true; }
        if(b.h >= p.stop){ close_trade(out,p,p.stop,b.ts); return true; }
        if(p.has_tp && b.o <= p.tp){ close_trade(out,p,b.o,b.ts); return true; }
        if(p.has_tp && b.l <= p.tp){ close_trade(out,p,p.tp,b.ts); return true; }
    }
    return false;
}

// =============================================================================
// Mechanism runners. Each returns the trade list for one config.
// =============================================================================

// TF1H: H1 EMA trend-follow both ways.
static std::vector<Trade> run_tf(const std::vector<Bar>& h1, int fN, int sN,
                                 double trail_mult, double imp_atr){
    auto ef=ema(h1,fN), es=ema(h1,sN), atr=atr_wilder(h1,14);
    std::vector<Trade> out; Pos p;
    int warm = std::max(sN*3, 100);
    for(size_t i=(size_t)warm;i<h1.size();++i){
        const Bar& b=h1[i];
        if(p.active){
            p.bars_held++;
            if(manage_intrabar(out,p,b)) goto entry;
            // close-based: update extreme, trail, reversal
            if(p.dir>0){ p.extreme=std::max(p.extreme,b.h);
                p.stop=std::max(p.stop, p.extreme - trail_mult*atr[i]); }
            else { p.extreme=std::min(p.extreme,b.l);
                p.stop=std::min(p.stop, p.extreme + trail_mult*atr[i]); }
            if((p.dir>0 && ef[i]<es[i]) || (p.dir<0 && ef[i]>es[i]))
                close_trade(out,p,b.c,b.ts);   // reversal exit at close
        }
entry:
        if(!p.active && i>=3 && atr[i]>0){
            double impulse=b.c-h1[i-3].c;
            bool lg = ef[i]>es[i] && b.c>ef[i] && impulse>= imp_atr*atr[i];
            bool sh = ef[i]<es[i] && b.c<ef[i] && impulse<=-imp_atr*atr[i];
            if(lg||sh){
                p.active=true; p.dir=lg?+1:-1; p.entry=b.c; p.ets=b.ts;
                p.extreme=b.c; p.has_tp=false; p.bars_held=0;
                p.stop=b.c - p.dir*2.0*atr[i];
            }
        }
    }
    if(p.active) close_trade(out,p,h1.back().c,h1.back().ts);
    return out;
}

// KELT: M30 Keltner breakout. exit_mode: 0 => fixed-R with tp_R, 1 => trail 2.5ATR
static std::vector<Trade> run_kelt(const std::vector<Bar>& m30, double k,
                                   int exit_mode, double tp_R){
    auto mid=ema(m30,20), atr=atr_wilder(m30,20);
    std::vector<Trade> out; Pos p;
    for(size_t i=100;i<m30.size();++i){
        const Bar& b=m30[i];
        if(p.active){
            p.bars_held++;
            if(manage_intrabar(out,p,b)) goto entry;
            if(exit_mode==1){
                if(p.dir>0){ p.extreme=std::max(p.extreme,b.h);
                    p.stop=std::max(p.stop,p.extreme-2.5*atr[i]); }
                else { p.extreme=std::min(p.extreme,b.l);
                    p.stop=std::min(p.stop,p.extreme+2.5*atr[i]); }
            }
            if(p.active && p.bars_held>=96) close_trade(out,p,b.c,b.ts); // 2d time stop
        }
entry:
        if(!p.active && atr[i]>0 && i>0){
            double up=mid[i]+k*atr[i], dn=mid[i]-k*atr[i];
            double pup=mid[i-1]+k*atr[i-1], pdn=mid[i-1]-k*atr[i-1];
            bool lg = b.c>up && m30[i-1].c<=pup;
            bool sh = b.c<dn && m30[i-1].c>=pdn;
            if(lg||sh){
                p.active=true; p.dir=lg?+1:-1; p.entry=b.c; p.ets=b.ts;
                p.extreme=b.c; p.bars_held=0;
                double sd=2.0*atr[i];
                p.stop=b.c-p.dir*sd;
                if(exit_mode==0){ p.has_tp=true; p.tp=b.c+p.dir*tp_R*sd; }
                else p.has_tp=false;
            }
        }
    }
    if(p.active) close_trade(out,p,m30.back().c,m30.back().ts);
    return out;
}

// DON: Donchian Nin/Nout close-through breakout with 3xATR hard stop.
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
            // channel exit at close
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

// VXF/VXB: H1 vol-expansion. fade=true -> counter-trend, else continuation.
static std::vector<Trade> run_vx(const std::vector<Bar>& h1, double k,
                                 bool fade, double tp_atr){
    auto atr=atr_wilder(h1,14);
    std::vector<Trade> out; Pos p;
    for(size_t i=100;i<h1.size();++i){
        const Bar& b=h1[i];
        if(p.active){
            p.bars_held++;
            if(manage_intrabar(out,p,b)) goto entry;
            if(!fade){ // continuation: trail 2.5 ATR
                if(p.dir>0){ p.extreme=std::max(p.extreme,b.h);
                    p.stop=std::max(p.stop,p.extreme-2.5*atr[i]); }
                else { p.extreme=std::min(p.extreme,b.l);
                    p.stop=std::min(p.stop,p.extreme+2.5*atr[i]); }
            }
            int tmax = fade?12:24;
            if(p.active && p.bars_held>=tmax) close_trade(out,p,b.c,b.ts);
        }
entry:
        if(!p.active && atr[i]>0){
            double rng=b.h-b.l;
            if(rng > k*atr[i]){
                double posr=(b.c-b.l)/rng; // close location 0..1
                int spike_dir = 0;
                if(posr>=0.70) spike_dir=+1; else if(posr<=0.30) spike_dir=-1;
                if(spike_dir!=0){
                    int dir = fade ? -spike_dir : spike_dir;
                    p.active=true; p.dir=dir; p.entry=b.c; p.ets=b.ts;
                    p.extreme=b.c; p.bars_held=0;
                    if(fade){
                        // stop beyond the spike extreme +1.5 ATR headroom
                        p.stop = (spike_dir>0)? b.h+1.5*atr[i] : b.l-1.5*atr[i];
                        p.has_tp=true; p.tp=b.c+dir*tp_atr*atr[i];
                    } else {
                        p.stop=b.c-dir*1.5*atr[i]; p.has_tp=false;
                    }
                }
            }
        }
    }
    if(p.active) close_trade(out,p,h1.back().c,h1.back().ts);
    return out;
}

// =============================================================================
static void print_report(Report& r){
    auto& w=r.w6;
    double wk = w.n/26.0;
    std::printf("%-5s %-24s | n=%-4d (%4.1f/wk) net1x=$%+8.1f net2x=$%+8.1f PF=%5.2f WR=%4.1f%% worst=$%+7.1f DD=$%7.1f | L $%+8.1f(n%d) S $%+8.1f(n%d) | WF $%+7.0f/$%+7.0f %s | FULL(22mo) n=%d $%+.0f PF %.2f\n",
        r.mech, r.cfg.c_str(),
        w.n, wk, w.net, r.w6_2x.net, w.pf(), w.wr(), w.worst, w.mdd,
        r.lng.net, r.lng.n, r.sht.net, r.sht.n,
        r.wf1.net, r.wf2.net,
        (w.net>0 && r.wf1.net>0 && r.wf2.net>0 && r.w6_2x.net>0)?"WF+2x PASS":"----",
        r.full.n, r.full.net, r.full.pf());
    std::printf("      FULL L $%+8.1f(n%d PF%.2f)  FULL S $%+8.1f(n%d PF%.2f)\n",
        r.flng.net, r.flng.n, r.flng.pf(), r.fsht.net, r.fsht.n, r.fsht.pf());
}
static void print_monthly(const Report& r){
    static const char* mn[7]={"Jan14+","Feb","Mar","Apr","May","Jun","Jul1-14"};
    std::printf("      monthly[%s %s]:", r.mech, r.cfg.c_str());
    for(int i=0;i<7;i++) std::printf("  %s $%+.0f(n%d)", mn[i], r.monthly[i], r.monthn[i]);
    std::printf("\n");
}

int main(int argc, char** argv){
    const char* path = argc>1? argv[1] : "backtest/data/mgc_30m_spliced_2024_2026.csv";
    if(getenv("COST_RT")) COST_RT=atof(getenv("COST_RT"));
    DUMP_ENTRIES = getenv("DUMP_ENTRIES") && atoi(getenv("DUMP_ENTRIES"))!=0;
    auto m30=load_csv(path);
    auto h1=to_h1(m30);
    std::printf("[GOLD-SHORTTF-BOTHWAYS] bars: m30=%zu h1=%zu  COST_RT=%.2fpt ($%.2f/RT per 1 MGC)  2x=%.2fpt\n",
        m30.size(), h1.size(), COST_RT, COST_RT*USD_PER_PT, 2*COST_RT);
    std::vector<Report> reps;
    char buf[128];

    // TF1H grid
    struct { int f,s; } emas[] = {{20,50},{10,40},{20,100}};
    for(auto& e : emas) for(double tr : {2.0,2.5,3.0}){
        Report r; r.mech="TF1H";
        std::snprintf(buf,sizeof buf,"ema%d/%d trail%.1f imp0.5",e.f,e.s,tr);
        r.cfg=buf; finish(r, run_tf(h1,e.f,e.s,tr,0.5)); reps.push_back(r);
    }
    // KELT grid
    for(double k : {1.25,1.5,2.0}){
        for(double R : {2.0,3.0}){
            Report r; r.mech="KELT";
            std::snprintf(buf,sizeof buf,"m30 k%.2f fixedR tp%.0fR",k,R);
            r.cfg=buf; finish(r, run_kelt(m30,k,0,R)); reps.push_back(r);
        }
        Report r; r.mech="KELT";
        std::snprintf(buf,sizeof buf,"m30 k%.2f trail2.5",k);
        r.cfg=buf; finish(r, run_kelt(m30,k,1,0)); reps.push_back(r);
    }
    // DON grid
    struct { int in,out; } dons[] = {{20,10},{40,20},{55,27}};
    for(auto& d : dons){
        { Report r; r.mech="DON";
          std::snprintf(buf,sizeof buf,"m30 %d/%d stop3ATR",d.in,d.out);
          r.cfg=buf; finish(r, run_don(m30,d.in,d.out)); reps.push_back(r); }
        { Report r; r.mech="DON";
          std::snprintf(buf,sizeof buf,"h1 %d/%d stop3ATR",d.in,d.out);
          r.cfg=buf; finish(r, run_don(h1,d.in,d.out)); reps.push_back(r); }
    }
    // VXF / VXB grids
    for(double k : {2.0,2.5,3.0}) for(double tp : {1.0,1.5}){
        Report r; r.mech="VXF";
        std::snprintf(buf,sizeof buf,"h1 k%.1f tp%.1fATR stop1.5",k,tp);
        r.cfg=buf; finish(r, run_vx(h1,k,true,tp)); reps.push_back(r);
    }
    for(double k : {2.0,2.5,3.0}){
        Report r; r.mech="VXB";
        std::snprintf(buf,sizeof buf,"h1 k%.1f trail2.5",k);
        r.cfg=buf; finish(r, run_vx(h1,k,false,0)); reps.push_back(r);
    }

    std::printf("\n== 6MO window 2026-01-14..2026-07-14 (net $ per 1 MGC; 2x = double cost) ==\n");
    for(auto& r : reps) print_report(r);
    std::printf("\n== monthly breakdown (6mo window) ==\n");
    for(auto& r : reps) if(r.w6.net>0) print_monthly(r);
    return 0;
}

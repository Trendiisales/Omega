// =============================================================================
// goldpanic_intrabar_vs_h1.cpp
// -----------------------------------------------------------------------------
// Question: does an INTRA-BAR exit check beat the current H1-BAR-CLOSE-only exit
// for g_gold_panic_bounce (GoldPanicBounceEngine)?
//
// The live engine's manage (chandelier ATR-trail + structural init-stop + time-
// stop) runs ONLY via _on_bar_close, called by on_tick solely on the H1 boundary
// (GoldPanicBounceEngine.hpp L131-136 -> _on_bar_close L186 -> _manage L246;
// BAR_SECS=3600). So stops are blind between H1 bars: a mid-hour reversal isn't
// caught until the hour closes.
//
// This harness replicates the engine EXACTLY (params + _manage logic, cited
// below) and runs it two ways, holding entries + params identical:
//   H1-CLOSE  : _manage tested only at H1 close (uses the H1 bar's own H/L).
//   INTRA-BAR : SAME stop levels tested against the finer intra-H1 series
//               (m5 for bull, m15 for bear) in time order; stop "touched" when a
//               fine bar's low <= eff -> exit at eff (or at the fine bar's OPEN on
//               a gap-through). Chandelier hh + eff recomputed at fine cadence.
// ATR stays an H1 quantity in BOTH modes (engine computes ATR on H1 closes).
//
// Faithful params (GoldPanicBounceEngine.hpp):
//   ATR_N=24 (L61), DD_LOOKBACK=250 (L62), DROP_K=8.0 (L63), TRAIL_ATR=4.5 (L64),
//   SL_ATR=2.0 (L65), MAX_HOLD_BARS=240 (L66), COOLDOWN_BARS=24 (L67),
//   ATR_FLOOR=3.0/ATR_CAP=80.0 (L85), RISK_DOLLARS=50, USD_PER_PT_LOT=100,
//   LOT_MIN=0.01/LOT_MAX=0.05 (L83-84).
//   Entry (L214-229): depth=(trailing DD_LOOKBACK-high - low)/ATR >= DROP_K, AND
//     turn = c>o && h>h_prev && c_prev<o_prev; init_stop=min(l[i],l[i-1],l[i-2])
//     if < entry else entry-SL_ATR*ATR; size=clamp(50/(stop_dist*100),0.01,0.05).
//   Manage (L246-276): hh=max(hh,curH); eff=max(init_stop, hh - TRAIL_ATR*ATR);
//     exit if low<=eff (TRAIL/SL) else time-stop at MAX_HOLD bars (TIME).
//   NOTE the engine bumps hh with the CURRENT bar's high BEFORE checking the low
//   (L250-257) -- the intra-bar mode applies the SAME hh-then-low convention at
//   fine cadence, so the ONLY thing that changes between modes is the check bar
//   size. (Entry cost gate L232, macro long-block L201, TREND_GATE L207 are OFF/
//   feed-absent in BT and identical across modes -> no delta.)
//
// COST (registry: real IBKR gold, NOT engine_init COST_RT_PTS 0.40):
//   cost_rt_pts = spread + 0.00015*2*entry  (1.5bps/side commission + measured
//   spread/oz). USD = ((exit-entry) - cost_rt_pts) * size * lot_mult(100).
//
// Build: clang++ -O3 -std=c++17 -o backtest/goldpanic_intrabar_vs_h1 backtest/goldpanic_intrabar_vs_h1.cpp
// Run:   backtest/goldpanic_intrabar_vs_h1
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cctype>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

struct Bar { int64_t ts; double o,h,l,c; };

static std::vector<Bar> load(const char* path){
    std::vector<Bar> v; FILE* f=std::fopen(path,"r");
    if(!f){ std::fprintf(stderr,"cannot open %s\n",path); return v; }
    char line[512];
    while(std::fgets(line,sizeof(line),f)){
        if(!std::isdigit((unsigned char)line[0])) continue;   // skip header
        Bar b; char* p=line; char* e;
        b.ts=std::strtoll(p,&e,10); if(*e!=',')continue; p=e+1;
        b.o=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
        b.h=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
        b.l=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
        b.c=std::strtod(p,&e);
        if(b.o>0&&b.h>0&&b.l>0&&b.c>0) v.push_back(b);
    }
    std::fclose(f);
    return v;
}

// ---- engine-faithful constants ----
static const int    ATR_N        = 24;
static const int    DD_LOOKBACK  = 250;
static const double DROP_K       = 8.0;
static const double TRAIL_ATR    = 4.5;
static const double SL_ATR       = 2.0;
static const int    MAX_HOLD     = 240;
static const int    COOLDOWN     = 24;
static const double ATR_FLOOR    = 3.0, ATR_CAP = 80.0;
static const double RISK_DOLLARS = 50.0, USD_PER_PT_LOT = 100.0;
static const double LOT_MIN = 0.01, LOT_MAX = 0.05;
static const double LOT_MULT = 100.0; // XAU tick_value multiplier (ledger applies this)

struct Trade { int64_t ts_in; double entry, exit, size, pnl_usd; int hold; const char* reason; bool whipsaw; };

struct Stats {
    int n=0, wins=0, losses=0, trail=0, stopn=0, timen=0, whip=0;
    double net=0, gw=0, gl=0, maxdd=0, worst=1e18;
    double pf() const { return gl>1e-9 ? gw/gl : (gw>0?999:0); }
    double wr() const { return n ? 100.0*wins/n : 0; }
};

// Group fine-bar indices by their parent-H1 key (ts/3600).
static std::map<int64_t,std::vector<int>> group_fine(const std::vector<Bar>& F){
    std::map<int64_t,std::vector<int>> m;
    for(int j=0;j<(int)F.size();++j) m[F[j].ts/3600].push_back(j);
    return m;
}

// intrabar=false -> H1-close manage; intrabar=true -> fine-cadence manage.
static void run(const std::vector<Bar>& B, const std::vector<Bar>& F, bool intrabar,
                double spread, Stats& st, std::vector<Trade>* out=nullptr)
{
    const int n=(int)B.size();
    std::map<int64_t,std::vector<int>> fine = intrabar ? group_fine(F) : std::map<int64_t,std::vector<int>>{};

    // rolling-mean ATR on H1 (matches _push_bar L163-177)
    std::vector<double> atr(n,0.0), tr(n,0.0);
    double tr_sum=0;
    for(int i=1;i<n;i++){
        tr[i]=std::max(B[i].h-B[i].l, std::max(std::fabs(B[i].h-B[i-1].c), std::fabs(B[i].l-B[i-1].c)));
    }
    for(int i=1;i<n;i++){
        tr_sum+=tr[i]; if(i>ATR_N) tr_sum-=tr[i-ATR_N];
        int w=std::min(i,ATR_N); atr[i]= w>0? tr_sum/w : 0;
    }

    bool inpos=false; double entry=0,init_stop=0,hh=0,size=0,atr_entry=0; int entry_i=0;
    int last_exit=-100000;
    double cum=0,peak=0;

    for(int i=std::max(1,DD_LOOKBACK+2); i<n; ++i){
        const double A = atr[i];

        if(inpos){
            const double Ae = (A>0? A : atr_entry);
            double exit_px=0; const char* why=nullptr; bool whip=false;

            if(!intrabar){
                // ---- H1-CLOSE manage (faithful _manage L246-258) ----
                const double curH=B[i].h, curL=B[i].l, curC=B[i].c;
                if(curH>hh) hh=curH;
                const double eff = std::max(init_stop, hh - TRAIL_ATR*Ae);
                if(curL <= eff){ exit_px=eff; why=(eff>init_stop?"TRAIL":"SL"); }
                else if(i-entry_i >= MAX_HOLD){ exit_px=curC; why="TIME"; }
            } else {
                // ---- INTRA-BAR manage: SAME stop levels, checked at fine cadence ----
                auto it = fine.find(B[i].ts/3600);
                if(it!=fine.end()){
                    for(int fj : it->second){
                        const Bar& g=F[fj];
                        if(g.h>hh) hh=g.h;                       // hh-then-low, same convention as engine
                        const double eff = std::max(init_stop, hh - TRAIL_ATR*Ae);
                        if(g.l <= eff){
                            exit_px = (g.o <= eff ? g.o : eff);  // gap-through -> fill at fine-bar open
                            why=(eff>init_stop?"TRAIL":"SL");
                            whip = (B[i].c > exit_px);           // shaken out mid-hour but H1 closed favorable
                            break;
                        }
                    }
                } else {
                    // no fine coverage for this H1 -> fall back to H1 bar (conservative)
                    const double curH=B[i].h, curL=B[i].l;
                    if(curH>hh) hh=curH;
                    const double eff = std::max(init_stop, hh - TRAIL_ATR*Ae);
                    if(curL<=eff){ exit_px=eff; why=(eff>init_stop?"TRAIL":"SL"); }
                }
                if(!why && i-entry_i >= MAX_HOLD){ exit_px=B[i].c; why="TIME"; }
            }

            if(why){
                const double cost_rt = spread + 0.00015*2.0*entry;   // real IBKR gold RT (pts)
                const double pnl_pts = (exit_px - entry) - cost_rt;
                const double pnl_usd = pnl_pts * size * LOT_MULT;
                st.n++; st.net+=pnl_usd;
                if(pnl_usd>0){ st.wins++; st.gw+=pnl_usd; } else { st.losses++; st.gl+=-pnl_usd; }
                if(why[0]=='T'&&why[1]=='R') st.trail++; else if(why[0]=='S') st.stopn++; else st.timen++;
                if(whip) st.whip++;
                if(pnl_usd<st.worst) st.worst=pnl_usd;
                cum+=pnl_usd; if(cum>peak)peak=cum; if(peak-cum>st.maxdd)st.maxdd=peak-cum;
                if(out) out->push_back({B[entry_i].ts,entry,exit_px,size,pnl_usd,i-entry_i,why,whip});
                inpos=false; last_exit=i;
            }
            continue;   // no re-entry on the same bar we manage
        }

        // ---- ENTRY eval at H1 close (faithful _on_bar_close L186-239) ----
        if(A < ATR_FLOOR || A > ATR_CAP) continue;   // L190 (skip entry; would manage but flat)
        if(i-last_exit < COOLDOWN) continue;          // L195
        // MONITOR: rolling drawdown depth (L214-217)
        double peakH=0; for(int k=i-DD_LOOKBACK;k<=i;k++) if(B[k].h>peakH)peakH=B[k].h;
        const double depth=(peakH - B[i].l)/A;
        if(depth < DROP_K) continue;
        // TURN (L220)
        const bool turn = B[i].c>B[i].o && B[i].h>B[i-1].h && B[i-1].c<B[i-1].o;
        if(!turn) continue;
        // size from structural stop (L224-229)
        entry=B[i].c;
        double swing_lo=std::min(B[i].l,std::min(B[i-1].l,B[i-2].l));
        init_stop = (swing_lo<entry)? swing_lo : (entry - SL_ATR*A);
        const double stop_dist = entry-init_stop;
        size = (stop_dist>1e-6)? (RISK_DOLLARS/(stop_dist*USD_PER_PT_LOT)) : LOT_MIN;
        size = std::clamp(size, LOT_MIN, LOT_MAX);
        hh=B[i].h; atr_entry=A; entry_i=i; inpos=true;
    }
}

static void hdr(const char* regime){
    std::printf("\n=== %s ===\n",regime);
    std::printf("%-26s %4s %6s %11s %6s %11s %11s %6s %s\n",
        "mode","N","WR%","net($)","PF","worst($)","maxDD($)","whip","exits(TR/SL/TI)");
}
static void row(const char* nm, const Stats& s){
    std::printf("%-26s %4d %6.1f %+11.2f %6.2f %+11.2f %11.2f %6d %d/%d/%d\n",
        nm,s.n,s.wr(),s.net,s.pf(),(s.n?s.worst:0.0),s.maxdd,s.whip,s.trail,s.stopn,s.timen);
}

struct Reg { const char* name; const char* h1; const char* fine; const char* fine_label; double spread; };

int main(){
    std::printf("GoldPanicBounce  INTRA-BAR vs H1-CLOSE exit-cadence test\n");
    std::printf("params: ATR24 DDLB250 DROP8 TRAIL4.5xATR SL2 MAXHOLD240 COOLDOWN24  cost=spread+1.5bps/side*2\n");

    Reg regs[] = {
        {"BULL / recent (2024-03..2026-04)","/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv","/Users/jo/Tick/2yr_XAUUSD_tick_fresh.m5.csv","m5",0.30},
        {"BULL / recent  [m30 sensitivity]","/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv","/Users/jo/Tick/2yr_XAUUSD_tick_fresh.m30.csv","m30",0.30},
        {"BEAR / 2022-23 (2022-01..2023-12)","/Users/jo/Tick/XAUUSD_2022_2023.h1.csv","/Users/jo/Tick/XAUUSD_2022_2023.m15.csv","m15",0.30},
    };

    for(auto& r : regs){
        auto B=load(r.h1); auto F=load(r.fine);
        if(B.size()<300){ std::printf("[skip %s: H1 too small]\n",r.name); continue; }
        Stats h1c, ib;
        std::vector<Trade> th, ti;
        run(B,F,false,r.spread,h1c,&th);
        run(B,F,true ,r.spread,ib ,&ti);
        hdr(r.name);
        std::printf("   (H1 bars=%zu, fine=%s bars=%zu)\n",B.size(),r.fine_label,F.size());
        row("H1-CLOSE (live)",h1c);
        char lab[48]; std::snprintf(lab,sizeof(lab),"INTRA-BAR (%s proxy)",r.fine_label);
        row(lab,ib);
        double d_usd = ib.net - h1c.net;
        double d_pct = std::fabs(h1c.net)>1e-9 ? 100.0*d_usd/std::fabs(h1c.net) : 0.0;
        std::printf("   delta (intrabar - h1close): %+.2f USD  (%+.1f%% of |h1close net|)\n", d_usd, d_pct);
        std::printf("   worst-trade: h1=%.2f  intrabar=%.2f   maxDD: h1=%.2f intrabar=%.2f   intrabar whipsaws=%d\n",
                    (h1c.n?h1c.worst:0.0),(ib.n?ib.worst:0.0),h1c.maxdd,ib.maxdd,ib.whip);
    }
    std::printf("\nVERDICT printed above per regime. Whipsaw = intrabar TRAIL/SL exit where the H1 bar CLOSED above the exit (shaken out, hour recovered).\n");
    return 0;
}

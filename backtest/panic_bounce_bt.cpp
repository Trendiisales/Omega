// =============================================================================
// panic_bounce_bt.cpp -- "big reversal day" V-bounce engine sweep (indices+gold)
// -----------------------------------------------------------------------------
// Concept (long-only, all-weather bounce catcher -- same family as the validated
// CapitulationEngine V-reversal and the Peachy long-only bounce edge):
//   MONITOR every H1 bar: rolling drawdown depth in ATR units from the trailing
//   N-bar high. When price is DEEP in a selloff (>= DROP_K ATR) AND prints a TURN
//   (bullish bar reclaiming the prior bar's high after a down bar) -> go LONG the
//   bounce. Exit = aggressive chandelier ATR-trail (no fixed TP -- ride the V),
//   structural initial stop at the selloff low.
//
// Cost-inclusive (round-trip pts), long-only (bounded loss), walk-forward halves,
// per-instrument + pooled + cross-regime (2022 bear) breakdown.
//
// Build: clang++ -O3 -std=c++17 -o backtest/panic_bounce_bt backtest/panic_bounce_bt.cpp
// Run:   backtest/panic_bounce_bt index   |   backtest/panic_bounce_bt gold
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
#include <ctime>

struct Bar { int64_t ts; double o,h,l,c; };

static std::vector<Bar> load(const char* path) {
    std::vector<Bar> v; std::ifstream f(path);
    if (!f.is_open()) { std::fprintf(stderr,"  cannot open %s\n",path); return v; }
    std::string line;
    while (std::getline(f,line)) {
        if (line.empty()) continue;
        if (!std::isdigit((unsigned char)line[0])) continue;   // skip header
        Bar b; char* p=(char*)line.c_str(); char* e;
        b.ts=std::strtoll(p,&e,10); if(*e!=',')continue; p=e+1;
        b.o=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
        b.h=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
        b.l=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
        b.c=std::strtod(p,&e);
        if(b.o>0&&b.h>0&&b.l>0&&b.c>0) v.push_back(b);
    }
    return v;
}

struct Params {
    int    atr_n   = 24;     // ATR window (H1 bars; 24 ~ 1 trading day)
    int    dd_lb   = 120;    // trailing-high lookback (~1 week of H1)
    double drop_k  = 5.0;    // selloff depth threshold, in ATR
    bool   req_down_prior = true;   // require prior bar red (capitulation->turn)
    double sl_atr  = 2.0;    // fallback initial stop distance (ATR) if struct stop invalid
    bool   struct_stop = true;      // initial stop = recent swing low
    double trail_atr = 3.0;  // chandelier trail width (ATR)
    int    arm_bars = 0;     // bars before trail arms (0 = immediate)
    int    max_hold = 240;   // time stop (H1 bars ~ 10 days)
    double cost_pts = 0.6;   // round-trip FIXED cost in points (spread component; instrument-specific)
    double comm_frac_side = 0.0;  // IBKR commission as fraction of notional PER SIDE (gold: 0.00015 = 1.5bps); 0 = none
    int    cooldown = 24;    // bars to wait after an exit before re-arming
    // capitulation-velocity gate: the drop must be SHARP & RECENT, not a slow grind
    bool   use_vel = false;
    int    vel_lb  = 12;     // recent window (H1 bars)
    double vel_k   = 3.0;    // recent fall over vel_lb must be >= vel_k ATR
    double exp_k   = 1.5;    // range-expansion: down-leg bar range >= exp_k * ATR
};

struct Trade { double pnl; int hold; const char* reason; int64_t ts_in; double entry; double exit; };

struct Stats {
    int n=0,wins=0,losses=0,trail=0,stopn=0,timen=0;
    double net=0,gw=0,gl=0,maxdd=0;
    double pf() const { return gl>1e-9? gw/gl : (gw>0?999:0); }
    double wr() const { return n? 100.0*wins/n : 0; }
    double avgw() const { return wins? gw/wins:0; }
    double avgl() const { return losses? -gl/losses:0; }
};

// Run strategy over [lo,hi) of bars. Accumulate into st. tick_value scales pnl->USD-ish (1.0 = points).
static void run(const std::vector<Bar>& B, const Params& P, Stats& st, int lo, int hi,
                std::vector<Trade>* out=nullptr) {
    int n=(int)B.size(); if(hi>n)hi=n;
    std::vector<double> atr(n,0.0);
    // simple ATR (avg true range)
    double tr_sum=0; std::vector<double> tr(n,0.0);
    for(int i=1;i<n;i++){
        double t=std::max(B[i].h-B[i].l, std::max(std::fabs(B[i].h-B[i-1].c), std::fabs(B[i].l-B[i-1].c)));
        tr[i]=t;
    }
    for(int i=1;i<n;i++){
        tr_sum+=tr[i];
        if(i>P.atr_n) tr_sum-=tr[i-P.atr_n];
        int w=std::min(i,P.atr_n);
        atr[i]= w>0? tr_sum/w : 0;
    }
    double cum=0,peak=0;
    bool inpos=false; double entry=0,init_stop=0,hh=0; int entry_i=0;
    int last_exit=-100000;
    for(int i=std::max(lo,P.dd_lb+2); i<hi; ++i){
        double A=atr[i]; if(A<=0) continue;
        if(!inpos){
            if(i-last_exit < P.cooldown) continue;
            // trailing high over lookback
            double peakH=0, troughL=1e18;
            for(int k=i-P.dd_lb;k<=i;k++){ if(B[k].h>peakH)peakH=B[k].h; if(B[k].l<troughL)troughL=B[k].l; }
            double depth=(peakH - B[i].l)/A;                 // how deep below the high, in ATR
            if(depth < P.drop_k) continue;                   // not a big enough selloff
            (void)troughL;
            if(P.use_vel){
                // sharp recent fall: close vel_lb bars ago vs recent trough low
                double recent_hi=0; for(int k=i-P.vel_lb;k<=i;k++) if(B[k].h>recent_hi)recent_hi=B[k].h;
                double recent_fall=(recent_hi - B[i].l)/A;
                if(recent_fall < P.vel_k) continue;
                // range expansion somewhere in the recent down-leg (panic bar)
                bool expanded=false;
                for(int k=i-P.vel_lb;k<=i;k++) if((B[k].h-B[k].l) >= P.exp_k*A){expanded=true;break;}
                if(!expanded) continue;
            }
            // TURN: bullish bar reclaiming prior high, after a down bar
            bool turn = B[i].c>B[i].o && B[i].h>B[i-1].h;
            if(P.req_down_prior) turn = turn && (B[i-1].c < B[i-1].o);
            if(!turn) continue;
            // enter long at close
            entry=B[i].c;
            double swing_lo = std::min(B[i].l, std::min(B[i-1].l,B[i-2].l));
            init_stop = P.struct_stop ? swing_lo : (entry - P.sl_atr*A);
            if(init_stop >= entry) init_stop = entry - P.sl_atr*A;   // guard
            hh=B[i].h; entry_i=i; inpos=true;
            continue;
        }
        // in position
        if(B[i].h>hh) hh=B[i].h;
        double A_e=atr[i];
        double trail_stop = hh - P.trail_atr*A_e;
        double eff = init_stop;
        if(i-entry_i >= P.arm_bars && trail_stop>eff) eff=trail_stop;
        double exit_px=0; const char* why=nullptr;
        if(B[i].l <= eff){ exit_px=eff; why = (eff>init_stop?"TRAIL":"STOP"); }
        else if(i-entry_i >= P.max_hold){ exit_px=B[i].c; why="TIME"; }
        if(why){
            // IBKR cost: price-proportional commission (2 sides) + fixed spread, per oz
            double cost_rt = P.cost_pts + P.comm_frac_side * 2.0 * entry;
            double pnl = (exit_px - entry) - cost_rt;
            st.n++; st.net+=pnl;
            if(pnl>0){st.wins++; st.gw+=pnl;} else {st.losses++; st.gl+=-pnl;}
            if(why[0]=='T'&&why[1]=='R')st.trail++; else if(why[0]=='S')st.stopn++; else st.timen++;
            cum+=pnl; if(cum>peak)peak=cum; if(peak-cum>st.maxdd)st.maxdd=peak-cum;
            if(out) out->push_back({pnl,i-entry_i,why,B[entry_i].ts,entry,exit_px});
            inpos=false; last_exit=i;
        }
    }
}

struct Series { const char* name; const char* path; double cost; double comm_side=0.0; };

static Stats run_series(const Series& s, const Params& base, int half /*0=all,1=H1,2=H2*/){
    auto B=load(s.path); Stats st; if(B.size()<200) return st;
    Params P=base; P.cost_pts=s.cost; P.comm_frac_side=s.comm_side;
    int n=(int)B.size();
    int lo=0,hi=n;
    if(half==1){lo=0;hi=n/2;} else if(half==2){lo=n/2;hi=n;}
    run(B,P,st,lo,hi);
    return st;
}

static void hdr(){
    std::printf("%-16s %5s %6s %9s %6s %8s %8s %8s  %s\n",
        "series","N","WR%","net(pts)","PF","avgW","avgL","maxDD","exits(TR/ST/TI)");
}
static void row(const char* nm, const Stats& s){
    std::printf("%-16s %5d %6.1f %+9.1f %6.2f %+8.1f %+8.1f %8.1f  %d/%d/%d\n",
        nm,s.n,s.wr(),s.net,s.pf(),s.avgw(),s.avgl(),s.maxdd,s.trail,s.stopn,s.timen);
}

int main(int argc,char**argv){
    std::string mode = argc>1? argv[1]:"index";

    // ---- trade-dump mode: ./panic_bounce_bt trades <csv> [drop lb trail cost] ----
    if(mode=="trades"){
        const char* path = argc>2? argv[2] : "/Users/jo/Tick/XAU2022_bear_h1.csv";
        Params P; P.struct_stop=true;
        P.drop_k = argc>3? atof(argv[3]) : 8.0;
        P.dd_lb  = argc>4? atoi(argv[4]) : 250;
        P.trail_atr = argc>5? atof(argv[5]) : 4.5;
        P.cost_pts  = argc>6? atof(argv[6]) : 0.37;
        auto B=load(path);
        std::printf("=== TRADE DUMP %s  (drop=%.1f lb=%d trail=%.1f cost=%.2f) bars=%zu ===\n",
                    path,P.drop_k,P.dd_lb,P.trail_atr,P.cost_pts,B.size());
        Stats st; std::vector<Trade> tr;
        run(B,P,st,0,(int)B.size(),&tr);
        std::printf("%-22s %10s %10s %9s %6s %s\n","entry_utc","entry","exit","pnl(pt)","hold","reason");
        for(auto&t:tr){
            // recompute entry/exit from pnl is lossy; print pnl+meta (entry price tracked via ts)
            time_t tt=(time_t)t.ts_in; char buf[32]; struct tm* g=gmtime(&tt);
            std::strftime(buf,sizeof(buf),"%Y-%m-%d %H:%M",g);
            std::printf("%-22s %10.1f %10.1f %+9.1f %6d %s\n",buf,t.entry,t.exit,t.pnl,t.hold,t.reason);
        }
        std::printf("---- SUMMARY n=%d WR=%.1f%% net=%+.1fpt PF=%.2f avgW=%+.1f avgL=%+.1f maxDD=%.1f ----\n",
                    st.n,st.wr(),st.net,st.pf(),st.avgw(),st.avgl(),st.maxdd);
        return 0;
    }


    // instrument-specific round-trip cost (pts): IBKR-ish CFD spreads
    std::vector<Series> idx = {
        {"SPX(24-26)","/Users/jo/Tick/SPXUSD_merged.h1.csv",0.6},
        {"NAS(24-26)","/Users/jo/Tick/NSXUSD_merged.h1.csv",2.0},
        {"GER40(25)", "/Users/jo/Tick/GER40_merged.h1.csv",1.5},
    };
    std::vector<Series> idx_bear = {
        {"NAS2022bear","/Users/jo/Tick/NAS2022_bear_h1.csv",2.0},
    };
    // IBKR XAUUSD spot: commission 1.5bps/side (comm_side=0.00015, price-proportional)
    // + measured spread/oz (env GSPR override; default 0.30 ~ measured mean). NOT the
    // old flat 0.37 (that was BlackBull/under-cost; ~4x too low at $4700 gold).
    double gspr = getenv("GSPR")? atof(getenv("GSPR")) : 0.30;
    std::vector<Series> gold = {
        {"XAU(24-26)","/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv",gspr,0.00015},
    };
    std::vector<Series> gold_bear = {
        {"XAU2022bear","/Users/jo/Tick/XAU2022_bear_h1.csv",gspr,0.00015},
    };

    std::vector<Series>& main_set   = (mode=="gold")? gold : idx;
    std::vector<Series>& bear_set   = (mode=="gold")? gold_bear : idx_bear;

    // ---- sweep grid ----
    std::vector<double> DROP={4.0,6.0,8.0};
    std::vector<int>    DDLB={120,250};
    std::vector<double> TRAIL={2.0,3.0,4.5};
    std::vector<double> SLM={2.0};
    std::vector<int>    VEL={0,1};               // capitulation-velocity gate off/on

    std::printf("=== PANIC-BOUNCE sweep  mode=%s ===\n",mode.c_str());
    std::printf("long-only V-reversal | structural stop | chandelier ATR-trail (no TP) | cost-incl\n");
    std::printf("ROBUST = every instrument both-halves net>0 AND 2022-bear net>0\n\n");

    struct Row{Params P;Stats pooled;double bear_pf;double bear_net;int worst_half_sign;bool robust;};
    std::vector<Row> rows;
    for(int vv:VEL)for(double dk:DROP)for(int lb:DDLB)for(double tr:TRAIL)for(double sl:SLM){
        Params P; P.drop_k=dk; P.dd_lb=lb; P.trail_atr=tr; P.sl_atr=sl; P.struct_stop=true;
        P.use_vel=(vv==1);
        Stats pool; bool robust=true;
        for(auto&s:main_set){ Stats x=run_series(s,P,0);
            Stats h1=run_series(s,P,1), h2=run_series(s,P,2);
            if(h1.net<=0 || h2.net<=0 || x.n<8) robust=false;
            pool.n+=x.n;pool.wins+=x.wins;pool.losses+=x.losses;pool.net+=x.net;
            pool.gw+=x.gw;pool.gl+=x.gl;pool.trail+=x.trail;pool.stopn+=x.stopn;pool.timen+=x.timen;
            if(x.maxdd>pool.maxdd)pool.maxdd=x.maxdd; }
        Stats bear; for(auto&s:bear_set){ Stats b=run_series(s,P,0);
            bear.net+=b.net;bear.gw+=b.gw;bear.gl+=b.gl;bear.n+=b.n; }
        if(bear.net<=0) robust=false;
        rows.push_back({P,pool,bear.pf(),bear.net,0,robust});
    }
    std::sort(rows.begin(),rows.end(),[](const Row&a,const Row&b){
        if(a.robust!=b.robust)return a.robust>b.robust; return a.pooled.net>b.pooled.net;});
    std::printf("--- TOP CONFIGS (pooled across %s instruments) ---\n",mode.c_str());
    std::printf("%-40s %5s %6s %9s %6s %8s %8s %s\n","cfg(vel/drop/ddlb/trail/sl)","N","WR%","net","PF","maxDD","bearPF","ROBUST");
    for(int i=0;i<(int)rows.size() && i<14;i++){
        auto&r=rows[i]; char lab[80];
        std::snprintf(lab,sizeof(lab),"%s drop%.0f/lb%d/tr%.1f/sl%.0f",r.P.use_vel?"VEL":"   ",r.P.drop_k,r.P.dd_lb,r.P.trail_atr,r.P.sl_atr);
        std::printf("%-40s %5d %6.1f %+9.1f %6.2f %8.1f %8.2f %s\n",lab,r.pooled.n,r.pooled.wr(),r.pooled.net,r.pooled.pf(),r.pooled.maxdd,r.bear_pf,r.robust?"YES":"-");
    }
    std::printf("\n");

    // ---- deep-dive on the best pooled config ----
    Params best = rows.empty()? Params{} : rows.front().P;
    std::printf("=== DEEP DIVE best cfg: drop=%.1f ddlb=%d trail=%.1f sl=%.1f ===\n",
                best.drop_k,best.dd_lb,best.trail_atr,best.sl_atr);
    hdr();
    Stats agg;
    for(auto&s:main_set){
        Stats a=run_series(s,best,0); row(s.name,a);
        Stats h1=run_series(s,best,1); Stats h2=run_series(s,best,2);
        char b1[32],b2[32]; std::snprintf(b1,sizeof(b1),"  %s.H1",s.name); std::snprintf(b2,sizeof(b2),"  %s.H2",s.name);
        row(b1,h1); row(b2,h2);
        agg.n+=a.n;agg.wins+=a.wins;agg.losses+=a.losses;agg.net+=a.net;agg.gw+=a.gw;agg.gl+=a.gl;
        agg.trail+=a.trail;agg.stopn+=a.stopn;agg.timen+=a.timen; if(a.maxdd>agg.maxdd)agg.maxdd=a.maxdd;
    }
    std::printf("------ pooled (in-sample regime) ------\n"); row("POOLED",agg);
    std::printf("------ CROSS-REGIME (2022 bear, out-of-sample) ------\n");
    for(auto&s:bear_set){ Stats a=run_series(s,best,0); row(s.name,a); }
    std::printf("\n");
    return 0;
}

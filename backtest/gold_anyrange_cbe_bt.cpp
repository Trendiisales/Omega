// gold_anyrange_cbe_bt.cpp — FAITHFUL cert sweep for the session-agnostic
// generalization of the certified GoldDailyCbeEngine ("AnyRange CBE",
// S-2026-07-23, operator "go"). Extension of backtest/gold_daily_cbe_bt.cpp:
// data loading, M1 aggregation, DST-correct COMEX day roll, cost model and the
// certified management sim are reused VERBATIM. Only the ENTRY state machine
// changes: the fixed Asian-session (00:00-08:00 UTC) range is replaced by a
// ROLLING trailing-K-hour consolidation range so the same
// break -> retrace 25% -> M1-close-confirm mechanic can fire off ANY session's
// consolidation (London lunch, NY afternoon), not just the overnight one.
//
// VARIANT SPEC (operator order):
//   * Range = trailing ROLLK-hour rolling high/low of M1 bars (window EXCLUDES
//     the current bar; timestamp-windowed so weekend/session gaps age out).
//     Window must be essentially full (>=75% of ROLLK*60 bars) to be valid.
//   * "Quiet" consolidation gate, checked on the window AT THE BREAK BAR:
//       - width >= MINRNG_PCT of price (0.4%, kept verbatim from baseline)
//       - optionally width <= CAPX x ATR14(D1) (CAPX=0 ablates; 1.0 tested)
//         to avoid trending pseudo-ranges.
//   * Break = M1 high > rolling range high  -> range FREEZES (RH, RL, rng).
//   * Retrace = M1 low <= RH - 0.25*rng. Confirm/entry = first M1 close > RH.
//   * LONG only (SHORT certified-dead on the parent, PF<=0.60).
//   * Re-arm: REARM=0 -> max 1 trade/day (baseline-like);
//             REARM=1 -> re-arm after each completed trade with a fresh
//             rolling range (any-session true). Both are cells.
//   * State machine resets at the COMEX day roll (verbatim baseline policy).
//   * Gates VERBATIM: prev D1 close > EMA200(D1); ATR14(D1) in P10-P90 of
//     trailing 120d (ATRBAND=1).
//   * Management VERBATIM (certified cell): SL 1.75xATR14(D1), 50% partial at
//     +1R, 2.0xATR trail off peak M1 close ratcheted on D1 closes, x0.75
//     tighten after +2R, NO BE ratchet (BE variants certified-dead:
//     PF 2.39 -> 1.61-1.85), multi-day hold (TIMEEXIT=0 default).
//
// COSTS: the parent harness's side_cost is authoritative and copied verbatim
//   (IBKR spot: 1.5bp/side with US$2.00 per-order MINIMUM + $0.17 measured
//   half-spread + $0.03 slip). MINCOMM=0 drops the $2 minimum to reproduce the
//   EXACT 2026-07-22 cert-era schedule (RT ~= $1.64/oz at $4131) so cells can
//   be compared 1:1 against the recorded baseline PF 2.39. COST_MULT=2 stress.
//
// ENV: ROLLK(6) CAPX(0|1.0) REARM(0|1) TIMEEXIT(0) SLMULT(1.75) TRAILMULT(2.0)
//      BE_TRIG(999999=off) BE_ATRX(0) BE_LOCK(0.30) MINRNG_PCT(0.4) ATRBAND(1)
//      RETR(0.25) COST_MULT(1.0) MINCOMM(1)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>

struct M1 { long ts; double o,h,l,c; };
static std::vector<M1> tape;

static int wday_of(int y,int m,int d){ struct tm t{}; t.tm_year=y-1900;t.tm_mon=m-1;t.tm_mday=d;t.tm_hour=12; time_t tt=timegm(&t); return gmtime(&tt)->tm_wday; }
static int nth_sunday(int y,int m,int nth){ int c=0; for(int d=1;d<=31;d++){ if(wday_of(y,m,d)==0){ c++; if(c==nth) return d; } } return -1; }
static long dkey(int y,int m,int d){ return y*10000L+m*100+d; }
static int et_off_utc(const struct tm& g){        // ET = UTC + off (negative)
    int y=g.tm_year+1900;
    long us_s=dkey(y,3,nth_sunday(y,3,2)), us_e=dkey(y,11,nth_sunday(y,11,1));
    long k=dkey(y,g.tm_mon+1,g.tm_mday);
    return (k>=us_s&&k<us_e)?-4:-5;
}

struct Ema { double v=0; bool init=false; double k; int n_seen=0;
    Ema(int n):k(2.0/(n+1)){}
    void push(double x){ if(!init){v=x;init=true;} else v+=k*(x-v); n_seen++; } };
struct Atr { std::deque<double> tr; double prev_c=0; bool has=false; int n;
    Atr(int n):n(n){}
    void push(double h,double l,double c){ double t=h-l; if(has) t=std::max(t,std::max(std::fabs(h-prev_c),std::fabs(l-prev_c))); tr.push_back(t); if((int)tr.size()>n) tr.pop_front(); prev_c=c; has=true; }
    bool ready() const { return (int)tr.size()==n; }
    double v() const { double s=0; for(double x:tr) s+=x; return s/tr.size(); } };

int main(int argc,char**argv){
    const char* path = argc>1?argv[1]:"XAUUSD_2022_2026.m1.csv";
    const char* gc_daily = argc>2?argv[2]:"/Users/jo/Tick/GC_F_daily_2016_2026_yahoo.csv";
    const int   ROLLK    = atoi(getenv("ROLLK")?getenv("ROLLK"):"6");       // trailing window, hours
    const double CAPX    = atof(getenv("CAPX")?getenv("CAPX"):"0");        // 0=off; width <= CAPX*ATR14(D1)
    const int   REARM    = atoi(getenv("REARM")?getenv("REARM"):"0");      // 0=1 trade/day, 1=re-arm after close
    const int   TIMEEXIT = atoi(getenv("TIMEEXIT")?getenv("TIMEEXIT"):"0");
    const double SLMULT  = atof(getenv("SLMULT")?getenv("SLMULT"):"1.75");
    const double TRAILM  = atof(getenv("TRAILMULT")?getenv("TRAILMULT"):"2.0");
    const double BE_TRIG = atof(getenv("BE_TRIG")?getenv("BE_TRIG"):"999999"); // BE certified-dead: OFF by default
    const double BE_ATRX = atof(getenv("BE_ATRX")?getenv("BE_ATRX"):"0");
    const double BE_LOCK = atof(getenv("BE_LOCK")?getenv("BE_LOCK"):"0.30");
    const double MINRNG  = atof(getenv("MINRNG_PCT")?getenv("MINRNG_PCT"):"0.4")/100.0;
    const int   ATRBAND  = atoi(getenv("ATRBAND")?getenv("ATRBAND"):"1");
    const double RETR    = atof(getenv("RETR")?getenv("RETR"):"0.25");
    const double COSTM   = atof(getenv("COST_MULT")?getenv("COST_MULT"):"1.0");
    const int   MINCOMM  = atoi(getenv("MINCOMM")?getenv("MINCOMM"):"1");  // 0 = 2026-07-22 cert-era schedule

    // ---- load M1 tape (ts,o,h,l,c) ---- (VERBATIM parent)
    { FILE* f=fopen(path,"r"); if(!f){ fprintf(stderr,"no tape %s\n",path); return 1; }
      char ln[256];
      while(fgets(ln,sizeof ln,f)){ M1 b; if(sscanf(ln,"%ld,%lf,%lf,%lf,%lf",&b.ts,&b.o,&b.h,&b.l,&b.c)==5) tape.push_back(b); }
      fclose(f); }
    if(tape.size()<100000){ fprintf(stderr,"tape too small %zu\n",tape.size()); return 1; }

    // ---- daily indicator state, seeded from GC daily (VERBATIM parent) ----
    Ema ema200(200); Atr atr14(14);
    std::deque<double> atr_hist;
    long first_tape_day; { struct tm g=*gmtime(&tape[0].ts); first_tape_day=dkey(g.tm_year+1900,g.tm_mon+1,g.tm_mday); }
    { FILE* f=fopen(gc_daily,"r"); if(!f){ fprintf(stderr,"no gc daily %s\n",gc_daily); return 1; }
      char ln[256]; fgets(ln,sizeof ln,f);          // header
      while(fgets(ln,sizeof ln,f)){ int y,m,d; double o,h,l,c;
          if(sscanf(ln,"%d-%d-%d,%lf,%lf,%lf,%lf",&y,&m,&d,&o,&h,&l,&c)!=7) continue;
          if(dkey(y,m,d)>=first_tape_day) break;
          ema200.push(c); atr14.push(h,l,c);
          if(atr14.ready()){ atr_hist.push_back(atr14.v()); if(atr_hist.size()>120) atr_hist.pop_front(); } }
      fclose(f); }
    fprintf(stderr,"[seed] GC daily: ema200 n=%d v=%.1f atr14 %s v=%.2f\n",
            ema200.n_seen, ema200.v, atr14.ready()?"ready":"cold", atr14.ready()?atr14.v():0.0);

    // ---- engine state (VERBATIM parent mgmt state) ----
    struct Pos { bool open=false; bool is_long; double entry, sl, r_dist, size;
                 double peak_close; bool be_done=false, partial_done=false;
                 long entry_ts; double atr_at_entry; };
    Pos pos;
    double prev_day_close=0;
    double ema_prev=0; bool ema_valid=false;
    double atr_prev=0; bool atr_valid=false;
    double day_o=0, day_h=0, day_l=0, day_c=0; bool day_live=false;
    long   cur_day_key=-1;
    bool   traded_today=false;
    double trail_lvl=0; bool trail_on=false;

    // ---- rolling K-hour consolidation window (the VARIANT part) ----
    // monotonic deques over (ts, px); window = bars in (cur_ts - ROLLK*3600, cur_ts)
    // i.e. EXCLUDES the current bar (pushed after evaluation).
    const long WSEC = (long)ROLLK*3600;
    const int  WMIN_BARS = (int)(ROLLK*60*3/4);   // require >=75%-full window
    std::deque<std::pair<long,double>> mxq, mnq;  // monotonic max/min of highs/lows
    std::deque<long> wts;                          // all window timestamps (count)
    double frz_h=0, frz_rng=0;                     // frozen range at break
    bool   broke=false, retraced=false;

    // results (VERBATIM parent)
    struct Tr { long ets, xts; double netR; int year; };
    std::vector<Tr> trades; double bankR=0;
    auto book=[&](long ets,long xts,double netR){ struct tm g=*gmtime(&xts); trades.push_back({ets,xts,netR,g.tm_year+1900}); bankR+=netR; };

    // cost model — parent's authoritative schedule, VERBATIM; MINCOMM=0 drops
    // the $2/order minimum = the exact 2026-07-22 cert-era basis (RT $1.64@4131).
    auto side_cost=[&](double px){
        double comm = MINCOMM ? std::max(2.0, px*0.00015) : px*0.00015;
        return COSTM*(comm + 0.17 + 0.03); };

    // per-trade close hook for REARM: fresh range after each completed trade
    auto on_flat=[&](){
        if(REARM){ broke=false; retraced=false; }
    };

    for(size_t i=0;i<tape.size();++i){
        const M1& b=tape[i];
        struct tm g=*gmtime(&b.ts);
        const int et=et_off_utc(g);
        long et_ts=b.ts+(long)et*3600; struct tm ge=*gmtime(&et_ts);
        long dk=dkey(ge.tm_year+1900,ge.tm_mon+1,ge.tm_mday);
        if(ge.tm_hour>=17) { // after 17:00 ET -> belongs to next COMEX day
            long nx=et_ts+7*3600; struct tm gn=*gmtime(&nx); dk=dkey(gn.tm_year+1900,gn.tm_mon+1,gn.tm_mday);
        }
        if(dk!=cur_day_key){
            if(day_live){                            // D1 close (VERBATIM parent)
                prev_day_close=day_c;
                ema200.push(day_c); atr14.push(day_h,day_l,day_c);
                ema_valid = ema200.n_seen>=200; ema_prev=ema200.v;
                if(atr14.ready()){ atr_prev=atr14.v(); atr_valid=true;
                    atr_hist.push_back(atr_prev); if(atr_hist.size()>120) atr_hist.pop_front(); }
                if(pos.open && trail_on){
                    double m=TRAILM;
                    double gain = pos.is_long ? (day_c-pos.entry) : (pos.entry-day_c);
                    if(gain > 2.0*pos.r_dist) m*=0.75;
                    double lvl = pos.is_long ? pos.peak_close - m*pos.atr_at_entry
                                             : pos.peak_close + m*pos.atr_at_entry;
                    if(pos.is_long) trail_lvl=std::max(trail_lvl,lvl); else trail_lvl = trail_lvl==0?lvl:std::min(trail_lvl,lvl);
                }
            }
            cur_day_key=dk; day_live=false; traded_today=false;
            broke=false; retraced=false;             // day-roll state reset (baseline policy)
        }
        if(!day_live){ day_o=b.o; day_h=b.h; day_l=b.l; day_c=b.c; day_live=true; }
        else { day_h=std::max(day_h,b.h); day_l=std::min(day_l,b.l); day_c=b.c; }

        // ---- rolling window maintenance: age out bars older than K hours ----
        while(!wts.empty() && wts.front() <= b.ts - WSEC) wts.pop_front();
        while(!mxq.empty() && mxq.front().first <= b.ts - WSEC) mxq.pop_front();
        while(!mnq.empty() && mnq.front().first <= b.ts - WSEC) mnq.pop_front();
        const bool win_valid = (int)wts.size() >= WMIN_BARS;
        const double roll_h = mxq.empty()?0:mxq.front().second;
        const double roll_l = mnq.empty()?0:mnq.front().second;

        // ================= position management (VERBATIM parent) =================
        if(pos.open){
            const double R=pos.r_dist;
            double gain = pos.is_long ? (b.h-pos.entry) : (pos.entry-b.l);
            const double be_trig_eff = (BE_ATRX>0.0) ? BE_ATRX*pos.atr_at_entry : BE_TRIG;
            if(!pos.be_done && gain>=be_trig_eff){
                pos.sl = pos.is_long ? pos.entry+BE_LOCK : pos.entry-BE_LOCK;
                pos.be_done=true;
            }
            if(!pos.partial_done && gain>=R){
                double px = pos.is_long ? pos.entry+R : pos.entry-R;
                double netoz = (pos.is_long?px-pos.entry:pos.entry-px) - side_cost(pos.entry) - side_cost(px);
                book(pos.entry_ts,b.ts, 0.5*netoz/R);
                pos.size=0.5; pos.partial_done=true; trail_on=true;
                pos.peak_close=b.c;
                trail_lvl = pos.is_long ? pos.entry - TRAILM*pos.atr_at_entry
                                        : pos.entry + TRAILM*pos.atr_at_entry;
            }
            if(trail_on){ if(pos.is_long) pos.peak_close=std::max(pos.peak_close,b.c); else pos.peak_close=std::min(pos.peak_close,b.c); }
            double stop = pos.sl;
            if(trail_on) stop = pos.is_long ? std::max(pos.sl,trail_lvl) : std::min(pos.sl,trail_lvl);
            bool hit = pos.is_long ? (b.l<=stop) : (b.h>=stop);
            if(hit){
                double px = pos.is_long ? std::min(stop,b.o) : std::max(stop,b.o);
                double netoz=(pos.is_long?px-pos.entry:pos.entry-px)-side_cost(pos.entry)-side_cost(px);
                book(pos.entry_ts,b.ts,pos.size*netoz/R);
                pos.open=false; trail_on=false; on_flat();
            }
            else if(TIMEEXIT){
                struct tm ge2=*gmtime(&et_ts);
                if(ge2.tm_hour>=16 && g.tm_min>=55){
                    double px=b.c;
                    double netoz=(pos.is_long?px-pos.entry:pos.entry-px)-side_cost(pos.entry)-side_cost(px);
                    book(pos.entry_ts,b.ts,pos.size*netoz/R);
                    pos.open=false; trail_on=false; on_flat();
                }
            }
        }

        // ================= AnyRange entry state machine (LONG only) =================
        const bool day_cap_ok = REARM ? true : !traded_today;
        if(!pos.open && day_cap_ok && ema_valid && atr_valid){
            struct tm ge3=*gmtime(&et_ts);
            bool guard_ok = !(TIMEEXIT && ge3.tm_hour>=16);   // NY-close guard (parity)
            if(guard_ok && prev_day_close>ema_prev){          // trend filter VERBATIM
                bool band_ok=true;
                if(ATRBAND && atr_hist.size()>=60){           // ATR band VERBATIM
                    std::vector<double> s(atr_hist.begin(),atr_hist.end());
                    std::sort(s.begin(),s.end());
                    double p10=s[s.size()/10], p90=s[s.size()*9/10];
                    if(atr_prev<p10||atr_prev>p90) band_ok=false;
                }
                if(band_ok){
                    if(!broke){
                        // break of a VALID quiet rolling range freezes it
                        if(win_valid && b.h>roll_h){
                            const double rng=roll_h-roll_l;
                            const bool quiet = rng >= MINRNG*b.c &&
                                               (CAPX<=0.0 || rng <= CAPX*atr_prev);
                            if(quiet){ frz_h=roll_h; frz_rng=rng; broke=true; retraced=false; }
                        }
                    }
                    else if(!retraced){ if(b.l<=frz_h-RETR*frz_rng) retraced=true; }
                    if(broke && retraced && b.c>frz_h){
                        double entry=b.c;
                        double sl = entry - SLMULT*atr_prev;
                        double R=entry-sl;
                        if(R>0){
                            pos={true,true,entry,sl,R,1.0,b.c,false,false,b.ts,atr_prev};
                            trail_on=false; trail_lvl=0; traded_today=true;
                            broke=false; retraced=false;      // consumed; fresh range next time
                        }
                    }
                }
            }
        }

        // ---- push current bar into the rolling window (AFTER evaluation) ----
        wts.push_back(b.ts);
        while(!mxq.empty() && mxq.back().second<=b.h) mxq.pop_back();
        mxq.push_back({b.ts,b.h});
        while(!mnq.empty() && mnq.back().second>=b.l) mnq.pop_back();
        mnq.push_back({b.ts,b.l});
    }
    // force-close any open position at tape end (VERBATIM parent)
    if(pos.open){ const M1&b=tape.back();
        double px=b.c, R=pos.r_dist;
        double netoz=(pos.is_long?px-pos.entry:pos.entry-px)-side_cost(pos.entry)-side_cost(px);
        book(pos.entry_ts,b.ts,pos.size*netoz/R); pos.open=false; }

    // ---- report (VERBATIM parent) ----
    auto agg=[&](long t0,long t1,const char*tag){
        double net=0,gw=0,gl=0,peak=0,dd=0,run=0; int n=0,w=0;
        for(auto&t:trades){ if(t.xts<t0||t.xts>=t1) continue; n++; net+=t.netR;
            if(t.netR>0){w++;gw+=t.netR;} else gl-=t.netR;
            run+=t.netR; if(run>peak)peak=run; if(peak-run>dd)dd=peak-run; }
        printf("  %-14s n=%4d netR=%+8.2f PF=%5.2f WR=%3.0f%% maxDD=%6.2fR\n",
               tag,n,net,gl>0?gw/gl:(gw>0?99.0:0.0),n?100.0*w/n:0.0,dd);
    };
    printf("[gold_anyrange_cbe] ROLLK=%dh CAPX=%.2f REARM=%d TIMEEXIT=%d SLMULT=%.2f TRAIL=%.2f BE_TRIG=%.0f MINRNG=%.2f%% ATRBAND=%d RETR=%.2f COSTM=%.1f MINCOMM=%d\n",
           ROLLK,CAPX,REARM,TIMEEXIT,SLMULT,TRAILM,BE_TRIG,MINRNG*100,ATRBAND,RETR,COSTM,MINCOMM);
    printf("  TOTAL bankR=%+.2f trades=%zu\n",bankR,trades.size());
    agg(0,4102444800L,"ALL");
    agg(1640995200L,1672531200L,"2022bear");
    agg(1672531200L,1704067200L,"2023chop");
    agg(1704067200L,4102444800L,"2024-26bull");
    agg(1640995200L,1712000000L,"WF-H1(22-24.3)");
    agg(1712000000L,4102444800L,"WF-H2(24.3-26)");
    return 0;
}

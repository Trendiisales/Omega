// gold_daily_cbe_bt.cpp — faithful backtest of the operator's "Gold Daily
// Engine: Cost-Covered BE + ATR Management + Anti-Whipsaw Trail" spec
// (2026-07-22, pasted spec). M1-grade fills, daily decision cadence.
//
// SPEC (as given):
//   * Asian range 00:00-08:00 UTC (current day's Asian session).
//   * LONG: price breaks above Asian high AFTER the session, then retraces
//     25% back INTO the range (level = AH - 0.25*range), then momentum
//     confirmation = M1 close back above the Asian high -> enter LONG there.
//     SHORT symmetric below Asian low.
//   * Trend filter: prev daily close > EMA200(D1) for longs; < for shorts.
//   * Range filter: Asian range >= MINRNG_PCT of price (spec ~0.4%).
//   * ATR band filter: daily ATR within [P10,P90] of its trailing 120-day
//     distribution ("avoid extremes" — spec is vague; knob ATRBAND=0 ablates).
//   * Initial SL: SLMULT x ATR14(D1) from entry (spec 1.5-2x), or
//     STRUCT=1: beyond the Asian extreme - 0.25*ATR buffer, whichever WIDER.
//   * BE: at +BE_TRIG $/oz move SL to entry + BE_LOCK (spec: trig $1.00 =
//     "10 pips", lock $0.30). Cost-honest variant: trig $3.20 (2x measured RT).
//   * Partial: 50% off at +1R (R = entry-SL distance). Rest = runner.
//   * Runner trail: TRAILMULT x ATR14(D1) off the peak M1 close, updated on
//     each D1 close (daily engine); tightens to 0.75*TRAILMULT after +2R.
//     Trail only active after BE+partial (per spec).
//   * Time exit: TIMEEXIT=1 -> flat at NY close (21:00 ET-DST-correct UTC)
//     same day (spec "close all by end of NY session"); TIMEEXIT=0 -> hold
//     runner on trail across days (spec's "runner" reading — both tested).
//   * Max 1 trade per direction per day. One position at a time per engine.
//   * LONG and SHORT run as SEPARATE engines (DIR env).
//
// COSTS (IBKR XAU real, same convention as gold_session_tp_bt.cpp):
//   commission 1.5bp/side + $0.30 spread (half per side) + $0.03/side slip.
//   COST_MULT env scales all cost legs (2x stress). R-normalized accounting:
//   risk-per-trade = 1R at entry, netR = sum of R multiples, costs inside fills.
//
// DAILY BARS: COMEX-convention day roll at the 17:00-ET tape close
//   (21:00/22:00 UTC DST-correct), matching the tape's own session gap.
//   EMA200/ATR14 warmup pre-2022 seeded from GC_F_daily_2016_2026_yahoo.csv
//   (GC futures ~= spot for a 200-day trend filter; documented deviation).
//
// ENV: DIR(LONG|SHORT) TIMEEXIT(1|0) SLMULT(1.75) TRAILMULT(2.0) BE_TRIG(1.0)
//      BE_LOCK(0.30) MINRNG_PCT(0.4) ATRBAND(1|0) STRUCT(0|1) COST_MULT(1.0)
//      CONFIRM(1|0: ablate the retrace-confirm, enter on raw break)
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
static int last_sunday(int y,int m){ int last=-1; for(int d=1;d<=31;d++){ struct tm t{}; t.tm_year=y-1900;t.tm_mon=m-1;t.tm_mday=d;t.tm_hour=12; time_t tt=timegm(&t); struct tm*g=gmtime(&tt); if(g->tm_mon==m-1&&g->tm_wday==0) last=d; } return last; }
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
    const bool  LONG_DIR = std::string(getenv("DIR")?getenv("DIR"):"LONG")=="LONG";
    const int   TIMEEXIT = atoi(getenv("TIMEEXIT")?getenv("TIMEEXIT"):"1");
    const double SLMULT  = atof(getenv("SLMULT")?getenv("SLMULT"):"1.75");
    const double TRAILM  = atof(getenv("TRAILMULT")?getenv("TRAILMULT"):"2.0");
    const double BE_TRIG = atof(getenv("BE_TRIG")?getenv("BE_TRIG"):"1.0");
    const double BE_ATRX = atof(getenv("BE_ATRX")?getenv("BE_ATRX"):"0");
    const double BE_LOCK = atof(getenv("BE_LOCK")?getenv("BE_LOCK"):"0.30");
    const double MINRNG  = atof(getenv("MINRNG_PCT")?getenv("MINRNG_PCT"):"0.4")/100.0;
    const int   ATRBAND  = atoi(getenv("ATRBAND")?getenv("ATRBAND"):"1");
    const int   STRUCT   = atoi(getenv("STRUCT")?getenv("STRUCT"):"0");
    const double COSTM   = atof(getenv("COST_MULT")?getenv("COST_MULT"):"1.0");
    const int   CONFIRM  = atoi(getenv("CONFIRM")?getenv("CONFIRM"):"1");
    // ── MIMIC x2 companion (S-22i follow-up): SEPARATE INDEPENDENT book, judged
    // STANDALONE (never vs parent ride). BE-floor foundation: legs spawn PENDING
    // at parent entry, OPEN only when close >= trig*(1+MCONF) (BE-ENTRY, anchored
    // le = that close), pre-arm drawdown-cancel at MLC, post-arm BE-floor + gb
    // trail, reclip=0, flush at parent exit. Honest fills at M1 close (real tail
    // on gap-through, no clamp). Cost = same spot side_cost, debited per clip.
    const int    MIMIC  = atoi(getenv("MIMIC")?getenv("MIMIC"):"0");
    const double MCONF  = atof(getenv("MCONF")?getenv("MCONF"):"0.2")/100.0;   // confirm above trig
    const double T_ARM  = atof(getenv("T_ARM")?getenv("T_ARM"):"0.5")/100.0;
    const double T_GB   = atof(getenv("T_GB")?getenv("T_GB"):"0.5");
    const double W_ARM  = atof(getenv("W_ARM")?getenv("W_ARM"):"2.0")/100.0;
    const double W_GB   = atof(getenv("W_GB")?getenv("W_GB"):"0.75");
    const double MLC    = atof(getenv("MLC")?getenv("MLC"):"1.0")/100.0;       // pre-arm cancel

    // ---- load M1 tape (ts,o,h,l,c) ----
    { FILE* f=fopen(path,"r"); if(!f){ fprintf(stderr,"no tape %s\n",path); return 1; }
      char ln[256];
      while(fgets(ln,sizeof ln,f)){ M1 b; if(sscanf(ln,"%ld,%lf,%lf,%lf,%lf",&b.ts,&b.o,&b.h,&b.l,&b.c)==5) tape.push_back(b); }
      fclose(f); }
    if(tape.size()<100000){ fprintf(stderr,"tape too small %zu\n",tape.size()); return 1; }

    // ---- daily indicator state, seeded from GC daily (2016 -> first tape day) ----
    Ema ema200(200); Atr atr14(14);
    std::deque<double> atr_hist;                    // trailing ATR values for the band filter
    long first_tape_day; { struct tm g=*gmtime(&tape[0].ts); first_tape_day=dkey(g.tm_year+1900,g.tm_mon+1,g.tm_mday); }
    { FILE* f=fopen(gc_daily,"r"); if(!f){ fprintf(stderr,"no gc daily %s\n",gc_daily); return 1; }
      char ln[256]; fgets(ln,sizeof ln,f);          // header
      while(fgets(ln,sizeof ln,f)){ int y,m,d; double o,h,l,c;
          if(sscanf(ln,"%d-%d-%d,%lf,%lf,%lf,%lf",&y,&m,&d,&o,&h,&l,&c)!=7) continue;
          if(dkey(y,m,d)>=first_tape_day) break;    // tape takes over from here
          ema200.push(c); atr14.push(h,l,c);
          if(atr14.ready()){ atr_hist.push_back(atr14.v()); if(atr_hist.size()>120) atr_hist.pop_front(); } }
      fclose(f); }
    fprintf(stderr,"[seed] GC daily: ema200 n=%d v=%.1f atr14 %s v=%.2f\n",
            ema200.n_seen, ema200.v, atr14.ready()?"ready":"cold", atr14.ready()?atr14.v():0.0);

    // ---- engine state ----
    struct Pos { bool open=false; bool is_long; double entry, sl, r_dist, size;   // size in R fraction (1.0 -> 0.5 after partial)
                 double peak_close; bool be_done=false, partial_done=false;
                 long entry_ts; double atr_at_entry; };
    Pos pos;
    double prev_day_close=0;                        // prev D1 close (tape)
    double ema_prev=0; bool ema_valid=false;        // EMA200 as of prev D1 close
    double atr_prev=0; bool atr_valid=false;        // ATR14 as of prev D1 close
    double day_o=0, day_h=0, day_l=0, day_c=0; bool day_live=false;
    long   cur_day_key=-1;                          // COMEX-roll day key
    double asian_h=0, asian_l=0; bool asian_live=false, asian_done=false;
    long   asian_day=-1;
    bool   broke=false, retraced=false;             // long-side state machine (or short: below AL)
    bool   traded_today=false;
    double trail_lvl=0; bool trail_on=false;

    // results
    struct Tr { long ets, xts; double netR; int year; };
    std::vector<Tr> trades; double bankR=0;
    auto book=[&](long ets,long xts,double netR){ struct tm g=*gmtime(&xts); trades.push_back({ets,xts,netR,g.tm_year+1900}); bankR+=netR; };

    // cost model ($/oz legs; R-normalize at close) — moved above the mimic
    // lambdas that capture it. IBKR SPOT GOLD venue (operator: not MGC):
    // 1.5bp/side comm + half measured spread $0.17 + $0.03 slip.
    auto side_cost=[&](double px){
        // IBKR spot gold REAL schedule: 1.5bp/side with US$2.00 MINIMUM per order
        // (the minimum dominates at 1-oz size) + measured half-spread + slip.
        return COSTM*(std::max(2.0, px*0.00015) + 0.17 + 0.03); };

    // mimic legs + standalone mimic book (net fraction of leg notional per clip)
    struct MLeg { bool pending=true, open=false, dead=false; double arm, gb, trig, le=0, mfe=0; long ets=0; };
    std::vector<MLeg> mlegs;
    struct MTr { long xts; double net; };
    std::vector<MTr> mtr;
    auto mbook=[&](MLeg& L, double fill, long ts, const char*){
        const double rt_pct = (side_cost(L.le)+side_cost(fill)) / L.le;
        mtr.push_back({ts, (fill/L.le - 1.0) - rt_pct});
        L.dead=true; L.open=false;
    };
    auto step_mimics=[&](double c, long ts){
        for(auto& L : mlegs){
            if(L.dead) continue;
            if(L.pending){
                if(c >= L.trig*(1.0+MCONF)){ L.pending=false; L.open=true; L.le=c; L.mfe=0; L.ets=ts; }
                continue;
            }
            const double fav=(c-L.le)/L.le;
            if(fav>L.mfe) L.mfe=fav;
            const bool armed = L.mfe>=L.arm;
            if(!armed && fav<=-MLC){ mbook(L,c,ts,"MIM_LC"); continue; }         // pre-arm cancel (honest tail)
            if(armed && fav<=0.0)  { mbook(L,c,ts,"BE_FLOOR"); continue; }        // floored (fill=close, honest)
            if(armed && fav<=L.mfe*(1.0-L.gb)) mbook(L,c,ts,"MIM_GB");
        }
    };
    auto flush_mimics=[&](double mark, long ts){
        for(auto& L : mlegs) if(!L.dead && L.open) mbook(L,mark,ts,"FLUSH");
        mlegs.clear();
    };

    // (side_cost defined above, before the mimic lambdas)

    for(size_t i=0;i<tape.size();++i){
        const M1& b=tape[i];
        struct tm g=*gmtime(&b.ts);
        const int et=et_off_utc(g);
        // COMEX day roll: tape closes 21:00/22:00 UTC == 17:00 ET; roll at that boundary
        long et_ts=b.ts+(long)et*3600; struct tm ge=*gmtime(&et_ts);
        long dk=dkey(ge.tm_year+1900,ge.tm_mon+1,ge.tm_mday);
        if(ge.tm_hour>=17) { // after 17:00 ET -> belongs to next COMEX day
            long nx=et_ts+7*3600; struct tm gn=*gmtime(&nx); dk=dkey(gn.tm_year+1900,gn.tm_mon+1,gn.tm_mday);
        }
        if(dk!=cur_day_key){
            if(day_live){                            // D1 close: update daily indicators
                prev_day_close=day_c;
                ema200.push(day_c); atr14.push(day_h,day_l,day_c);
                ema_valid = ema200.n_seen>=200; ema_prev=ema200.v;
                if(atr14.ready()){ atr_prev=atr14.v(); atr_valid=true;
                    atr_hist.push_back(atr_prev); if(atr_hist.size()>120) atr_hist.pop_front(); }
                // runner trail updates on D1 close (daily engine)
                if(pos.open && trail_on){
                    double m=TRAILM;
                    double gain = pos.is_long ? (day_c-pos.entry) : (pos.entry-day_c);
                    if(gain > 2.0*pos.r_dist) m*=0.75;                  // tighten after +2R
                    double lvl = pos.is_long ? pos.peak_close - m*pos.atr_at_entry
                                             : pos.peak_close + m*pos.atr_at_entry;
                    if(pos.is_long) trail_lvl=std::max(trail_lvl,lvl); else trail_lvl = trail_lvl==0?lvl:std::min(trail_lvl,lvl);
                }
            }
            cur_day_key=dk; day_live=false; traded_today=false;
            asian_live=false; asian_done=false; broke=false; retraced=false;
        }
        if(!day_live){ day_o=b.o; day_h=b.h; day_l=b.l; day_c=b.c; day_live=true; }
        else { day_h=std::max(day_h,b.h); day_l=std::min(day_l,b.l); day_c=b.c; }

        // ---- Asian range on UTC clock (spec literal 00:00-08:00 UTC), per UTC day ----
        long udk=dkey(g.tm_year+1900,g.tm_mon+1,g.tm_mday);
        if(g.tm_hour<8){
            if(asian_day!=udk){ asian_day=udk; asian_h=b.h; asian_l=b.l; asian_live=true; asian_done=false; broke=false; retraced=false; }
            else { asian_h=std::max(asian_h,b.h); asian_l=std::min(asian_l,b.l); }
        } else if(asian_live && asian_day==udk && !asian_done){
            asian_done=true;                        // session closed; range frozen
        }

        // ================= position management (M1 granularity) =================
        if(pos.open){
            const double R=pos.r_dist;
            double gain = pos.is_long ? (b.h-pos.entry) : (pos.entry-b.l);
            // BE move (uses favorable extreme within the bar). BE_ATRX>0 scales the
            // trigger to the instrument (trig = BE_ATRX*ATR@entry) instead of $-fixed.
            const double be_trig_eff = (BE_ATRX>0.0) ? BE_ATRX*pos.atr_at_entry : BE_TRIG;
            if(!pos.be_done && gain>=be_trig_eff){
                pos.sl = pos.is_long ? pos.entry+BE_LOCK : pos.entry-BE_LOCK;
                pos.be_done=true;
            }
            // partial 50% at +1R (fill at the 1R level, worse-of within bar not needed: level fill)
            if(!pos.partial_done && gain>=R){
                double px = pos.is_long ? pos.entry+R : pos.entry-R;
                double netoz = (pos.is_long?px-pos.entry:pos.entry-px) - side_cost(pos.entry) - side_cost(px);
                book(pos.entry_ts,b.ts, 0.5*netoz/R);
                pos.size=0.5; pos.partial_done=true; trail_on=true;
                pos.peak_close=b.c;
                trail_lvl = pos.is_long ? pos.entry - TRAILM*pos.atr_at_entry
                                        : pos.entry + TRAILM*pos.atr_at_entry;   // seeded; D1 close ratchets
            }
            if(trail_on){ if(pos.is_long) pos.peak_close=std::max(pos.peak_close,b.c); else pos.peak_close=std::min(pos.peak_close,b.c); }
            // stop / trail hit — worse-of: gap-through fills at bar open beyond level
            double stop = pos.sl;
            if(trail_on) stop = pos.is_long ? std::max(pos.sl,trail_lvl) : std::min(pos.sl,trail_lvl);
            bool hit = pos.is_long ? (b.l<=stop) : (b.h>=stop);
            if(hit){
                double px = pos.is_long ? std::min(stop,b.o) : std::max(stop,b.o);
                double netoz=(pos.is_long?px-pos.entry:pos.entry-px)-side_cost(pos.entry)-side_cost(px);
                book(pos.entry_ts,b.ts,pos.size*netoz/R);
                pos.open=false; trail_on=false;
                if(MIMIC) flush_mimics(px,b.ts);
            }
            // time exit at NY close (17:00 ET == tape session close)
            else if(TIMEEXIT){
                struct tm ge2=*gmtime(&et_ts);
                if(ge2.tm_hour>=16 && g.tm_min>=55){ // 16:55 ET -> flatten before close
                    double px=b.c;
                    double netoz=(pos.is_long?px-pos.entry:pos.entry-px)-side_cost(pos.entry)-side_cost(px);
                    book(pos.entry_ts,b.ts,pos.size*netoz/R);
                    pos.open=false; trail_on=false;
                    if(MIMIC) flush_mimics(px,b.ts);
                }
            }
        }
        if(MIMIC && !mlegs.empty()) step_mimics(b.c, b.ts);

        // ================= entry state machine =================
        if(!pos.open && !traded_today && asian_done && g.tm_hour>=8 && ema_valid && atr_valid){
            const double rng=asian_h-asian_l;
            if(rng < MINRNG*b.c) continue;                       // min range filter
            if(ATRBAND && atr_hist.size()>=60){                  // ATR "normal band" P10-P90
                std::vector<double> s(atr_hist.begin(),atr_hist.end());
                std::sort(s.begin(),s.end());
                double p10=s[s.size()/10], p90=s[s.size()*9/10];
                if(atr_prev<p10||atr_prev>p90) continue;
            }
            // NY-close guard: no fresh entries in the final hour
            struct tm ge3=*gmtime(&et_ts);
            if(TIMEEXIT && ge3.tm_hour>=16) continue;
            if(LONG_DIR){
                if(prev_day_close<=ema_prev) continue;           // trend filter
                if(!broke){ if(b.h>asian_h) broke=true; }
                else if(CONFIRM && !retraced){ if(b.l<=asian_h-0.25*rng) retraced=true; }
                if(broke && (!CONFIRM || retraced) && b.c>asian_h){
                    double entry=b.c+side_cost(b.c)*0;           // cost applied in pnl; entry at close
                    double sl = entry - SLMULT*atr_prev;
                    if(STRUCT) sl = std::min(sl, asian_l-0.25*atr_prev);
                    double R=entry-sl; if(R<=0) continue;
                    pos={true,true,entry,sl,R,1.0,b.c,false,false,b.ts,atr_prev};
                    trail_on=false; trail_lvl=0; traded_today=true;
                    if(MIMIC){ mlegs.clear();
                        mlegs.push_back({true,false,false,T_ARM,T_GB,entry});
                        mlegs.push_back({true,false,false,W_ARM,W_GB,entry}); }
                }
            } else {
                if(prev_day_close>=ema_prev) continue;
                if(!broke){ if(b.l<asian_l) broke=true; }
                else if(CONFIRM && !retraced){ if(b.h>=asian_l+0.25*rng) retraced=true; }
                if(broke && (!CONFIRM || retraced) && b.c<asian_l){
                    double entry=b.c;
                    double sl = entry + SLMULT*atr_prev;
                    if(STRUCT) sl = std::max(sl, asian_h+0.25*atr_prev);
                    double R=sl-entry; if(R<=0) continue;
                    pos={true,false,entry,sl,R,1.0,b.c,false,false,b.ts,atr_prev};
                    trail_on=false; trail_lvl=0; traded_today=true;
                }
            }
        }
    }
    // force-close any open position at tape end
    if(pos.open){ const M1&b=tape.back();
        double px=b.c, R=pos.r_dist;
        double netoz=(pos.is_long?px-pos.entry:pos.entry-px)-side_cost(pos.entry)-side_cost(px);
        book(pos.entry_ts,b.ts,pos.size*netoz/R); pos.open=false;
        if(MIMIC) flush_mimics(px,b.ts); }

    // ---- report ----
    auto agg=[&](long t0,long t1,const char*tag){
        double net=0,gw=0,gl=0,peak=0,dd=0,run=0; int n=0,w=0;
        for(auto&t:trades){ if(t.xts<t0||t.xts>=t1) continue; n++; net+=t.netR;
            if(t.netR>0){w++;gw+=t.netR;} else gl-=t.netR;
            run+=t.netR; if(run>peak)peak=run; if(peak-run>dd)dd=peak-run; }
        printf("  %-14s n=%4d netR=%+8.2f PF=%5.2f WR=%3.0f%% maxDD=%6.2fR\n",
               tag,n,net,gl>0?gw/gl:(gw>0?99.0:0.0),n?100.0*w/n:0.0,dd);
    };
    printf("[gold_daily_cbe] DIR=%s TIMEEXIT=%d SLMULT=%.2f TRAIL=%.2f BE_TRIG=%.2f MINRNG=%.2f%% ATRBAND=%d STRUCT=%d CONFIRM=%d COSTM=%.1f\n",
           LONG_DIR?"LONG":"SHORT",TIMEEXIT,SLMULT,TRAILM,BE_TRIG,MINRNG*100,ATRBAND,STRUCT,CONFIRM,COSTM);
    printf("  TOTAL bankR=%+.2f trades=%zu\n",bankR,trades.size());
    agg(0,4102444800L,"ALL");
    agg(1640995200L,1672531200L,"2022bear");
    agg(1672531200L,1704067200L,"2023chop");
    agg(1704067200L,4102444800L,"2024-26bull");
    // WF halves by trade count midpoint of the tape span
    agg(1640995200L,1712000000L,"WF-H1(22-24.3)");
    agg(1712000000L,4102444800L,"WF-H2(24.3-26)");
    if(MIMIC){
        auto magg=[&](long t0,long t1,const char*tag){
            double net=0,gw=0,gl=0,peak=0,dd=0,run=0; int n=0,w=0;
            for(auto&t:mtr){ if(t.xts<t0||t.xts>=t1) continue; n++; net+=t.net;
                if(t.net>0){w++;gw+=t.net;} else gl-=t.net;
                run+=t.net; if(run>peak)peak=run; if(peak-run>dd)dd=peak-run; }
            printf("  [MIMIC]%-11s n=%4d net%%=%+8.2f PF=%5.2f WR=%3.0f%% maxDD=%6.2f%%\n",
                   tag,n,net*100.0,gl>0?gw/gl:(gw>0?99.0:0.0),n?100.0*w/n:0.0,dd*100.0);
        };
        printf("[MIMIC x2 STANDALONE] conf=%.2f%% T(arm %.2f/gb %.2f) W(arm %.2f/gb %.2f) lc=%.2f%%\n",
               MCONF*100,T_ARM*100,T_GB,W_ARM*100,W_GB,MLC*100);
        magg(0,4102444800L,"ALL");
        magg(1640995200L,1672531200L,"2022bear");
        magg(1672531200L,1704067200L,"2023chop");
        magg(1704067200L,4102444800L,"24-26bull");
        magg(1640995200L,1712000000L,"WF-H1");
        magg(1712000000L,4102444800L,"WF-H2");
    }
    return 0;
}

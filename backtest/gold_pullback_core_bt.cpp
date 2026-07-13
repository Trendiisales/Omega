// gold_pullback_core_bt.cpp — Gold Structural Campaign Engine, STAGE A: CORE-only
// first-pullback-continuation detector (13u Part 2 family 1, TOP PRIORITY), symmetric L/S.
//
// Pipeline (build order steps 1-3+7-8 in one streaming pass):
//   ticks (ts_ms,bid,ask; integrity-CERTIFIED) -> 1s slices (normalizer)
//   -> per-30min-slot trailing-20-day time-of-day baselines (tick rate, spread)
//   -> first-pullback detector state machine (impulse -> pullback -> break entry)
//   -> CORE replay: real ask/bid fills (spread paid) + pad grid -> effective ~8/10/12/16bp RT
//   -> MODE=RANDOM: matched-random control (same hour-of-day, direction, stop dist, mgmt), N seeds
//
// Spec (SESSION_HANDOFF_2026-07-13u.md Part 2):
//   impulse 20-45bp in 30-300s, efficiency >= EFF, tick activity >= ACT x same-slot baseline,
//   spread <= SPR_MAX x slot median; pullback retrace 20-45% on lower activity, no low < impulse
//   anchor, no full micro-VWAP failure; entry on local-high break + up-tick imbalance + VWAP
//   reclaim + expected remaining >= 32bp; STRUCTURAL stop = pullback trough - buffer; trail =
//   structural (rolling TRAILSEC low) armed at +ARM bp; one CORE per structural event.
//
// ADVERSE-PROTECTION: research harness (not a live engine). Structural stop at pullback trough
// (~10-20bp) on every trade from entry; verdict feeds the live build gate later.
//
// FAMILY=1 (default): first-pullback continuation. FAMILY=2: compression breakout —
//   RNG_WIN-sec range in bottom RNG_Q of same-slot 20-day range distribution, short-ATR falling,
//   no recent >EXT_MAX bp extension, spread stable -> break +BRKBUF, HOLD beyond boundary
//   HOLDSEC (never first touch), velocity+imbalance up, height >= HMIN -> entry; STRUCTURAL
//   stop = range midpoint; TP = boundary + 2x height (TPMODE=1) or structural trail.
//
// H4/D1 RE-ANCHOR (2026-07-13w operator decision): input may be a ts,o,h,l,c,spr M1 bar file
// (auto-detected via "spr" in header). Then fills = close -/+ spr/2, slice activity = bar range
// bp (no tick counts in bars), and the scale knobs UPQ_WIN / EFF_STRIDE / GAPCLOSE let the same
// detectors run on 100-250bp hours-long structures. Defaults reproduce tick-scale behavior.
//
// Usage: gold_pullback_core_bt <tick_csv|m1_csv> [MODE=STRAT|RANDOM] (params via env)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <map>

static double envd(const char* k, double d){ const char* v=getenv(k); return v? atof(v): d; }
static int    envi(const char* k, int d){ const char* v=getenv(k); return v? atoi(v): d; }

// ---------- params ----------
static double IMP_LO, IMP_HI, EFF, ACT, SPR_MAX, RET_LO, RET_HI, PB_ACT, UPR, REM, ARM, STOPBUF, VWTOL;
static int DUR_LO, DUR_HI, PB_TIMEOUT, TRAILSEC, BREAKSEC, MAXHOLD, WARMUP_DAYS, SEEDS;
static int FAMILY, RNG_WIN, HOLDSEC, ARM_TTL; static double RNG_Q, BRKBUF, EXT_MAX, HMIN;
static int UPQ_WIN, EFF_STRIDE, GAPCLOSE;  // H4/D1 re-anchor knobs (defaults = tick-scale behavior)
static int STOPMODE;                       // 0=pullback trough (spec default); 1=impulse anchor L0
                                           //   (full structural event as risk unit — the crypto-parent
                                           //    geometry: cost 3-8% of stop instead of 25-40%)
static int M1MODE=0;                       // input is ts,o,h,l,c,spr M1 bars (auto-detected)
static std::vector<double> PADS; // extra RT cost bp on top of real spread

// ---------- 1s slice ----------
struct Sec {
    int32_t ts;          // unix sec
    float bidc, askc;    // close of second
    float lo, hi;        // mid low/high within second
    uint16_t n, nup, ndn;
    float sprsum;        // sum of spread
};
static std::vector<Sec> S;   // all active seconds, time-ordered

// activity of one slice: tick count (tick mode) or range bp (M1 mode — no tick counts in bars).
// Same-slot baselines + all activity ratios use this uniformly, so detector semantics are
// unchanged: "activity >= ACT x same-slot baseline" etc.
static inline double ACTV(const Sec& s){
    return M1MODE? (double)(s.hi-s.lo)/(0.5*(double)(s.hi+s.lo))*1e4 : (double)s.n;
}

// ---------- time-of-day baseline (48 x 30-min slots, trailing 20 days) ----------
struct SlotBase {
    std::deque<double> rate;   // ticks/sec daily means
    std::deque<double> spr;    // daily median-ish (mean) spread
    std::deque<double> rng;    // daily mean RNG_WIN-window range (bp)
    double med_rate=0, med_spr=0, q30_rng=0; int days=0;
    void push(double r, double s, double g, double q){
        rate.push_back(r); spr.push_back(s); if(g>0) rng.push_back(g);
        if(rate.size()>20){ rate.pop_front(); spr.pop_front(); }
        if(rng.size()>20) rng.pop_front();
        days++;
        auto med=[](std::deque<double> d){ std::sort(d.begin(),d.end()); return d.empty()?0.0:d[d.size()/2]; };
        med_rate=med(rate); med_spr=med(spr);
        if(!rng.empty()){ std::deque<double> t=rng; std::sort(t.begin(),t.end());
            q30_rng=t[(size_t)(q*(t.size()-1))]; }
    }
};

struct Trade {
    double ts_in, ts_out;
    int side;            // +1 long, -1 short
    double px_in, px_out;   // actual fills (spread crossed), no pad
    double gross_bp;        // signed, fills-based
    double stop_bp;         // entry->stop distance bp (for random matching)
    int hod;                // hour of day (UTC)
    double act_ratio;
    int month;              // yyyymm
    double mfe_bp, mae_bp;  // vs entry fill, favourable positive
};
static double g_mfe, g_mae;  // set by run_exit

// simulate exit from second index i0 given entry px (fill w/ spread), returns exit fill px + ts
// mgmt identical for STRAT and RANDOM: hard stop, structural trail (rolling TRAILSEC low/high) after ARM
static int TPMODE; static double TP_PX; // TPMODE=1: fixed stop + take-profit at TP_PX (no trail)
static void run_exit(int side, size_t i0, double entry_px, double stop_px, double& out_px, double& out_ts){
    double hwm = entry_px;
    double stop = stop_px;
    bool armed=false;
    g_mfe=0; g_mae=0;
    double t_in = S[i0].ts;
    std::deque<std::pair<int32_t,float>> trailq; // (ts, extreme) monotonic for rolling low(long)/high(short)
    for(size_t i=i0+1;i<S.size();++i){
        const Sec& s=S[i];
        double mid_lo=s.lo, mid_hi=s.hi;
        if(side>0){ g_mfe=std::max(g_mfe,(mid_hi-entry_px)/entry_px*1e4); g_mae=std::max(g_mae,(entry_px-mid_lo)/entry_px*1e4); }
        else      { g_mfe=std::max(g_mfe,(entry_px-mid_lo)/entry_px*1e4); g_mae=std::max(g_mae,(mid_hi-entry_px)/entry_px*1e4); }
        // update trail queue
        float ext = side>0? s.lo : s.hi;
        while(!trailq.empty() && (side>0? trailq.back().second>=ext : trailq.back().second<=ext)) trailq.pop_back();
        trailq.push_back({s.ts, ext});
        while(!trailq.empty() && trailq.front().first < s.ts - TRAILSEC) trailq.pop_front();
        if(TPMODE){
            if(side>0){
                if(mid_lo<=stop){ out_px=std::min((double)s.bidc,stop); out_ts=s.ts; return; }
                if(mid_hi>=TP_PX){ out_px=TP_PX; out_ts=s.ts; return; }
            } else {
                if(mid_hi>=stop){ out_px=std::max((double)s.askc,stop); out_ts=s.ts; return; }
                if(mid_lo<=TP_PX){ out_px=TP_PX; out_ts=s.ts; return; }
            }
            if(s.ts - t_in > MAXHOLD){ out_px = side>0? s.bidc : s.askc; out_ts=s.ts; return; }
            if(i+1<S.size() && S[i+1].ts - s.ts > GAPCLOSE){ out_px = side>0? s.bidc : s.askc; out_ts=s.ts; return; }
            continue;
        }
        if(side>0){
            if(mid_hi>hwm) hwm=mid_hi;
            if(!armed && (hwm-entry_px)/entry_px*1e4 >= ARM) armed=true;
            if(armed && !trailq.empty()){
                double tl = trailq.front().second*(1.0-STOPBUF*1e-4);
                if(tl>stop) stop=tl;
            }
            if(mid_lo<=stop){ out_px=std::min((double)s.bidc, stop); out_ts=s.ts; return; }
        } else {
            if(mid_lo<hwm||hwm==entry_px) hwm=std::min(hwm,mid_lo);
            if(!armed && (entry_px-hwm)/entry_px*1e4 >= ARM) armed=true;
            if(armed && !trailq.empty()){
                double tl = trailq.front().second*(1.0+STOPBUF*1e-4);
                if(tl<stop) stop=tl;
            }
            if(mid_hi>=stop){ out_px=std::max((double)s.askc, stop); out_ts=s.ts; return; }
        }
        if(s.ts - t_in > MAXHOLD){ out_px = side>0? s.bidc : s.askc; out_ts=s.ts; return; }
        // weekend gap: if next second jumps > 12h, close at last quote
        if(i+1<S.size() && S[i+1].ts - s.ts > GAPCLOSE){ out_px = side>0? s.bidc : s.askc; out_ts=s.ts; return; }
    }
    out_px = side>0? S.back().bidc : S.back().askc; out_ts=S.back().ts;
}

static double grossbp(int side, double in, double out){ return side>0? (out-in)/in*1e4 : (in-out)/in*1e4; }

int main(int argc, char** argv){
    if(argc<2){ fprintf(stderr,"usage: %s <tick_csv>\n", argv[0]); return 1; }
    const char* mode = getenv("MODE")? getenv("MODE"): "STRAT";
    IMP_LO=envd("IMP_LO",20); IMP_HI=envd("IMP_HI",45);
    DUR_LO=envi("DUR_LO",30); DUR_HI=envi("DUR_HI",300);
    EFF=envd("EFF",0.35); ACT=envd("ACT",2.0); SPR_MAX=envd("SPR_MAX",1.5);
    RET_LO=envd("RET_LO",0.20); RET_HI=envd("RET_HI",0.45); PB_ACT=envd("PB_ACT",0.85);
    PB_TIMEOUT=envi("PB_TIMEOUT",600); UPR=envd("UPR",0.55); REM=envd("REM",32);
    ARM=envd("ARM",15); TRAILSEC=envi("TRAILSEC",180); BREAKSEC=envi("BREAKSEC",30);
    STOPBUF=envd("STOPBUF",2); MAXHOLD=envi("MAXHOLD",14400); VWTOL=envd("VWTOL",10);
    WARMUP_DAYS=envi("WARMUP_DAYS",5); SEEDS=envi("SEEDS",100); TPMODE=envi("TPMODE",0);
    FAMILY=envi("FAMILY",1); RNG_WIN=envi("RNG_WIN",600); HOLDSEC=envi("HOLDSEC",45);
    ARM_TTL=envi("ARM_TTL",3600); RNG_Q=envd("RNG_Q",0.30); BRKBUF=envd("BRKBUF",3);
    EXT_MAX=envd("EXT_MAX",40); HMIN=envd("HMIN",16);
    UPQ_WIN=envi("UPQ_WIN",60); EFF_STRIDE=envi("EFF_STRIDE",1); GAPCLOSE=envi("GAPCLOSE",43200);
    STOPMODE=envi("STOPMODE",0);
    { const char* p=getenv("PADS")? getenv("PADS"):"6,8,10,14";
      char buf[256]; strncpy(buf,p,255); buf[255]=0;
      for(char* t=strtok(buf,","); t; t=strtok(nullptr,",")) PADS.push_back(atof(t)); }

    // ---------- pass 1: ticks -> 1s slices ----------
    FILE* f=fopen(argv[1],"r");
    if(!f){ fprintf(stderr,"cannot open %s\n", argv[1]); return 1; }
    char line[256]; if(!fgets(line,sizeof line,f)) return 1; // header
    M1MODE = strstr(line,"spr")!=nullptr;   // ts,o,h,l,c,spr bar file vs timestamp,bid,ask ticks
    S.reserve(16u<<20);
    Sec cur{}; cur.ts=-1; double prevmid=0;
    if(M1MODE){
        while(fgets(line,sizeof line,f)){
            long long ts; double o,h,l,c,spr;
            if(sscanf(line,"%lld,%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c,&spr)!=6) continue;
            if(c<=0 || spr<0 || h<l) continue;
            Sec s{}; s.ts=(int32_t)ts;
            s.bidc=(float)(c-0.5*spr); s.askc=(float)(c+0.5*spr);
            s.lo=(float)l; s.hi=(float)h;
            s.n=1; s.nup = c>o? 1:0; s.ndn = c<o? 1:0; s.sprsum=(float)spr;
            S.push_back(s);
        }
        fclose(f);
        fprintf(stderr,"[norm] M1 mode: %zu bars\n", S.size());
        goto detect;
    }
    while(fgets(line,sizeof line,f)){
        long long ms; double bid, ask;
        if(sscanf(line,"%lld,%lf,%lf",&ms,&bid,&ask)!=3) continue;
        int32_t ts=(int32_t)(ms/1000);
        double mid=0.5*(bid+ask), spr=ask-bid;
        if(spr<0 || bid<=0) continue;
        if(ts!=cur.ts){
            if(cur.ts>=0) S.push_back(cur);
            cur=Sec{}; cur.ts=ts; cur.lo=cur.hi=(float)mid;
        }
        cur.bidc=(float)bid; cur.askc=(float)ask;
        if(mid<cur.lo)cur.lo=(float)mid; if(mid>cur.hi)cur.hi=(float)mid;
        cur.n++; cur.sprsum+=(float)spr;
        if(prevmid>0){ if(mid>prevmid)cur.nup++; else if(mid<prevmid)cur.ndn++; }
        prevmid=mid;
    }
    if(cur.ts>=0) S.push_back(cur);
    fclose(f);
    fprintf(stderr,"[norm] %zu active seconds, %s .. %s", S.size(), "", "\n");

    detect:;
    // ---------- pass 2: baselines + detector ----------
    SlotBase base[48];
    // per-day accumulators per slot
    double day_ticks[48]={0}, day_secs[48]={0}, day_spr[48]={0}, day_rng[48]={0}, day_rngn[48]={0}, day_sprn[48]={0};
    int cur_day=-1; int days_seen=0;
    // family 2 ext window = 3x the range window (tick default 600*3=1800: unchanged) so the
    // "no recent extension" check keeps meaning at hours-scale RNG_WIN instead of ext==height.
    const int WINCOV = FAMILY==2? std::max(3*RNG_WIN,1800) : DUR_HI;

    // rolling ring of recent seconds (indices into S) covering the detector window
    std::deque<size_t> win;           // seconds within last WINCOV
    std::deque<size_t> brk;           // seconds within last BREAKSEC (for local-high break)
    std::deque<size_t> upq;           // last 60s for up-ratio

    struct Ev { int state=0; int side=0; double L0=0, P=0, T=0; int32_t t0=0, tpk=0;
                double imp_rate=0, vwap_n=0, vwap_s=0; double pb_ticks=0, pb_secs=0; double act_ratio=0;
                double pbdn_ticks=0, pbdn_secs=0; };   // frozen at trough: down-leg activity only
    Ev ev; int32_t last_exit_ts=0, last_anchor_used=0;

    std::vector<Trade> trades;
    bool in_pos=false;
    long long c_cand=0,c_move=0,c_eff=0,c_act=0,c_spr=0,c_imp=0,c_pb=0,
              c_ab_ret=0,c_ab_str=0,c_ab_vwap=0,c_ab_to=0,
              c_g_pbact=0,c_g_brk=0,c_g_upr=0,c_g_vwap=0,c_g_rem=0;
    long long c2_chk=0,c2_q=0,c2_atr=0,c2_ext=0,c2_arm=0,c2_brk=0,
              c2_g_hmin=0,c2_g_vel=0,c2_g_upr=0,c2_g_rem=0;
    int cvstate=0, cvdir=0, cvfails=0; double cvRH=0, cvRL=0;
    int32_t cv_tarm=0, cv_tbrk=0, cv_next_arm=0;

    for(size_t i=0;i<S.size();++i){
        const Sec& s=S[i];
        int32_t dayn = s.ts/86400, slot=(s.ts%86400)/1800;
        if((int)dayn!=cur_day){
            if(cur_day>=0){ for(int k=0;k<48;k++) if(day_secs[k]>0)
                base[k].push(day_ticks[k]/day_secs[k], day_spr[k]/std::max(day_sprn[k],1.0),
                             day_rngn[k]>0? day_rng[k]/day_rngn[k] : 0, RNG_Q);
                days_seen++; }
            memset(day_ticks,0,sizeof day_ticks); memset(day_secs,0,sizeof day_secs); memset(day_spr,0,sizeof day_spr);
            memset(day_rng,0,sizeof day_rng); memset(day_rngn,0,sizeof day_rngn); memset(day_sprn,0,sizeof day_sprn);
            cur_day=dayn;
        }
        day_ticks[slot]+=ACTV(s); day_secs[slot]+=1; day_spr[slot]+=s.sprsum; day_sprn[slot]+=s.n;

        // maintain windows
        win.push_back(i); while(!win.empty() && S[win.front()].ts < s.ts-WINCOV) win.pop_front();
        brk.push_back(i); while(!brk.empty() && S[brk.front()].ts < s.ts-BREAKSEC) brk.pop_front();
        upq.push_back(i); while(!upq.empty() && S[upq.front()].ts < s.ts-UPQ_WIN) upq.pop_front();

        if(FAMILY==2 && s.ts%60==0 && !win.empty()){   // sample RNG_WIN range for slot baseline
            double h=-1e18,l=1e18;
            for(size_t k=0;k<win.size();++k){ const Sec& w=S[win[k]];
                if(w.ts < s.ts-RNG_WIN) continue;
                h=std::max(h,(double)w.hi); l=std::min(l,(double)w.lo); }
            if(h>l){ day_rng[slot]+= (h-l)/(0.5*(h+l))*1e4; day_rngn[slot]+=1; }
        }

        if(days_seen<WARMUP_DAYS) continue;
        if(base[slot].med_rate<=0) continue;
        double mid = 0.5*(s.bidc+s.askc);
        double sprnow = s.n? s.sprsum/s.n : (s.askc-s.bidc);

        if(in_pos) continue;   // one CORE at a time (overlap control)

        if(FAMILY==2){
            if(strcmp(mode,"STRAT")!=0) continue;
            if(cvstate==0){
                if(s.ts < cv_next_arm || base[slot].q30_rng<=0) continue;
                if(s.ts%10) continue;                       // arm check every 10s
                double h=-1e18,l=1e18,H=-1e18,L=1e18,atr1=0,atr0=0,pm1=-1,pm0=-1;
                for(size_t k=0;k<win.size();++k){ const Sec& w=S[win[k]];
                    double c=0.5*(w.bidc+w.askc);
                    H=std::max(H,(double)w.hi); L=std::min(L,(double)w.lo);
                    if(w.ts >= s.ts-RNG_WIN){ h=std::max(h,(double)w.hi); l=std::min(l,(double)w.lo); }
                    if(w.ts >= s.ts-180){ if(pm1>0) atr1+=fabs(c-pm1); pm1=c; }
                    else if(w.ts >= s.ts-360){ if(pm0>0) atr0+=fabs(c-pm0); pm0=c; }
                }
                if(h<=l) continue;
                double height_bp=(h-l)/mid*1e4, ext_bp=(H-L)/mid*1e4;
                c2_chk++;
                if(height_bp > base[slot].q30_rng) continue;          c2_q++;
                if(height_bp < 4) continue;                           // degenerate/holiday
                if(atr0<=0 || atr1>=atr0) continue;                   c2_atr++;
                if(ext_bp >= EXT_MAX) continue;                       c2_ext++;
                if(sprnow > SPR_MAX*std::max(base[slot].med_spr,1e-9)) continue; c2_arm++;
                cvRH=h; cvRL=l; cv_tarm=s.ts; cvfails=0; cvstate=10;
            }
            else if(cvstate==10){
                if(s.ts-cv_tarm > ARM_TTL){ cvstate=0; cv_next_arm=s.ts+600; continue; }
                if(s.hi > cvRH*(1+BRKBUF*1e-4)){ cvdir=+1; cv_tbrk=s.ts; cvstate=11; c2_brk++; }
                else if(s.lo < cvRL*(1-BRKBUF*1e-4)){ cvdir=-1; cv_tbrk=s.ts; cvstate=11; c2_brk++; }
            }
            else if(cvstate==11){
                double B = cvdir>0? cvRH : cvRL;
                double tol = 0.25*(cvRH-cvRL);   // retest into range up to 25% of height is allowed (spec: breakout+retest)
                bool back_in = cvdir>0? (s.lo < cvRH-tol) : (s.hi > cvRL+tol);
                if(back_in){ cvfails++; if(cvfails>1){ cvstate=0; cv_next_arm=s.ts+600; } else cvstate=10; continue; }
                if(s.ts-cv_tbrk < HOLDSEC) continue;
                if(s.ts-cv_tbrk > HOLDSEC+std::max(120,HOLDSEC)){ cvstate=0; cv_next_arm=s.ts+600; continue; }
                double height=(cvRH-cvRL), height_bp=height/mid*1e4;
                if(height_bp < HMIN){ c2_g_hmin++; continue; }
                double nt=0,ns=0,nup=0,ndn=0;
                for(size_t k=0;k<upq.size();++k){ nt+=ACTV(S[upq[k]]); ns+=1; nup+=S[upq[k]].nup; ndn+=S[upq[k]].ndn; }
                double vel = (ns>0 && base[slot].med_rate>0)? (nt/ns)/base[slot].med_rate : 0;
                if(vel < ACT){ c2_g_vel++; continue; }
                double upr=(nup+ndn)>0? (cvdir>0? nup/(nup+ndn) : ndn/(nup+ndn)) : 0;
                if(upr < UPR){ c2_g_upr++; continue; }
                double entry_px = cvdir>0? s.askc : s.bidc;
                double target = cvdir>0? B + 2*height : B - 2*height;
                double rem_bp = cvdir>0? (target-entry_px)/entry_px*1e4 : (entry_px-target)/entry_px*1e4;
                if(rem_bp < REM){ c2_g_rem++; continue; }
                double mid_rng=0.5*(cvRH+cvRL);
                double stop_px = cvdir>0? mid_rng*(1.0-STOPBUF*1e-4) : mid_rng*(1.0+STOPBUF*1e-4);
                double stop_bp = fabs(entry_px-stop_px)/entry_px*1e4;
                double opx, ots; in_pos=true; TP_PX=target;
                run_exit(cvdir, i, entry_px, stop_px, opx, ots);
                in_pos=false; last_exit_ts=(int32_t)ots;
                Trade t; t.ts_in=s.ts; t.ts_out=ots; t.side=cvdir; t.px_in=entry_px; t.px_out=opx;
                t.gross_bp=grossbp(cvdir,entry_px,opx); t.stop_bp=stop_bp;
                t.mfe_bp=g_mfe; t.mae_bp=g_mae;
                t.hod=(s.ts%86400)/3600; t.act_ratio=vel;
                time_t tt=(time_t)s.ts; struct tm g; gmtime_r(&tt,&g); t.month=(g.tm_year+1900)*100+(g.tm_mon+1);
                trades.push_back(t);
                cvstate=0; cv_next_arm=(int32_t)ots+600;
            }
            continue;
        }

        if(ev.state==0){
            if(strcmp(mode,"STRAT")!=0) continue; // RANDOM mode: no detection needed
            // scan window for impulse anchor (both directions)
            double wmin=1e18, wmax=-1e18; int32_t tmin=0, tmax=0; size_t imin=0, imax=0;
            for(size_t k=0;k<win.size();++k){ const Sec& w=S[win[k]];
                if(w.lo<wmin){wmin=w.lo; tmin=w.ts; imin=k;}
                if(w.hi>wmax){wmax=w.hi; tmax=w.ts; imax=k;} }
            // LONG impulse: rise from wmin
            for(int dir=0; dir<2; ++dir){
                double a = dir==0? wmin : wmax; int32_t ta = dir==0? tmin : tmax; size_t ia = dir==0? imin : imax;
                if(ta==last_anchor_used) continue;           // one CORE per structural event
                if(ta < last_exit_ts) continue;              // new event must postdate last exit
                int el = s.ts - ta;
                if(el<DUR_LO || el>DUR_HI) continue;
                c_cand++;
                double move_bp = dir==0? (mid-a)/a*1e4 : (a-mid)/a*1e4;
                if(move_bp<IMP_LO || move_bp>IMP_HI) continue;
                c_move++;
                // efficiency + activity over [ta, now]
                // EFF_STRIDE>1: path sampled at stride boundaries so efficiency semantics stay
                // resolution-comparable when the impulse spans hours (H4/D1 re-anchor) instead
                // of seconds — per-slice path length grows with duration, net move doesn't.
                double path=0, nt=0, ns=0, vwn=0; double pm=-1; int32_t pts=-2000000000;
                for(size_t k=ia;k<win.size();++k){ const Sec& w=S[win[k]];
                    double c=0.5*(w.bidc+w.askc);
                    if(w.ts >= pts+EFF_STRIDE){ if(pm>0) path+=fabs(c-pm); pm=c; pts=w.ts; }
                    nt+=ACTV(w); ns+=1; vwn+=w.n; }
                double eff = path>0? fabs(mid - a)/ (path) : 0;
                double rate = ns>0? nt/ns : 0;
                double ratio = rate / base[slot].med_rate;
                if(eff<EFF) continue;
                c_eff++;
                if(ratio<ACT) continue;
                c_act++;
                if(sprnow > SPR_MAX*std::max(base[slot].med_spr,1e-9)) continue;
                c_spr++; c_imp++;
                ev=Ev{}; ev.state=1; ev.side= dir==0? +1 : -1; ev.L0=a; ev.P= dir==0? s.hi : s.lo;
                ev.t0=ta; ev.tpk=s.ts; ev.imp_rate=rate; ev.act_ratio=ratio;
                ev.vwap_n=vwn; ev.vwap_s=0; // vwap accumulates from here w/ existing window
                for(size_t k=ia;k<win.size();++k){ const Sec& w=S[win[k]]; ev.vwap_s += 0.5*(w.bidc+w.askc)*w.n; }
                last_anchor_used=ta;
                break;
            }
        }
        else if(ev.state==1){ // impulse riding: extend peak, detect pullback start
            ev.vwap_s += mid*s.n; ev.vwap_n += s.n;
            double Sz = fabs(ev.P-ev.L0);
            if(ev.side>0){ if(s.hi>ev.P){ ev.P=s.hi; ev.tpk=s.ts; } }
            else         { if(s.lo<ev.P||ev.P==0){ ev.P=std::min((double)s.lo, ev.P); ev.tpk=s.ts; } }
            double ret = ev.side>0? (ev.P-s.lo)/std::max(Sz,1e-9) : (s.hi-ev.P)/std::max(Sz,1e-9);
            if(ret >= RET_LO){ ev.state=2; c_pb++; ev.T= ev.side>0? s.lo : s.hi; ev.pb_ticks=ACTV(s); ev.pb_secs=1;
                               ev.pbdn_ticks=ACTV(s); ev.pbdn_secs=1; }
            if(s.ts-ev.tpk > PB_TIMEOUT) ev.state=0;
            double sz_bp = Sz/ev.L0*1e4; if(sz_bp>IMP_HI*1.6) ev.state=0; // runaway, not our structure
        }
        else if(ev.state==2){ // pullback: track trough, wait for break entry
            ev.vwap_s += mid*s.n; ev.vwap_n += s.n;
            ev.pb_ticks+=ACTV(s); ev.pb_secs+=1;
            double Sz = fabs(ev.P-ev.L0);
            bool newT=false;
            if(ev.side>0 && s.lo<ev.T){ ev.T=s.lo; newT=true; }
            if(ev.side<0 && s.hi>ev.T){ ev.T=s.hi; newT=true; }
            if(newT){ ev.pbdn_ticks=ev.pb_ticks; ev.pbdn_secs=ev.pb_secs; }
            double ret = fabs(ev.P-ev.T)/std::max(Sz,1e-9);
            double vwap = ev.vwap_n>0? ev.vwap_s/ev.vwap_n : mid;
            bool structural_fail = ev.side>0? (ev.T<=ev.L0) : (ev.T>=ev.L0);
            bool vwap_fail = ev.side>0? (ev.T < vwap*(1-VWTOL*1e-4)) : (ev.T > vwap*(1+VWTOL*1e-4));
            if(ret>RET_HI || structural_fail || vwap_fail || s.ts-ev.tpk>PB_TIMEOUT){
                if(ret>RET_HI)c_ab_ret++; else if(structural_fail)c_ab_str++; else if(vwap_fail)c_ab_vwap++; else c_ab_to++;
                ev.state=0; continue; }
            if(ret<RET_LO) continue;
            // pullback on lower activity
            double pbrate = ev.pbdn_secs>0? ev.pbdn_ticks/ev.pbdn_secs : 0;
            if(pbrate > PB_ACT*ev.imp_rate){ c_g_pbact++; continue; }
            // entry trigger: local-extreme break + up-imbalance + vwap reclaim + remaining >= REM
            double brk_ext = ev.side>0? -1e18 : 1e18;
            for(size_t k=0;k+1<brk.size();++k){ const Sec& w=S[brk[k]];
                if(ev.side>0) brk_ext=std::max(brk_ext,(double)w.hi); else brk_ext=std::min(brk_ext,(double)w.lo); }
            bool broke = ev.side>0? (s.hi>brk_ext && brk_ext>-1e17) : (s.lo<brk_ext && brk_ext<1e17);
            if(!broke){ c_g_brk++; continue; }
            double nup=0, ndn=0; for(size_t k=0;k<upq.size();++k){ nup+=S[upq[k]].nup; ndn+=S[upq[k]].ndn; }
            double upr = (nup+ndn)>0? (ev.side>0? nup/(nup+ndn) : ndn/(nup+ndn)) : 0;
            if(upr<UPR){ c_g_upr++; continue; }
            bool reclaim = ev.side>0? (mid>=vwap) : (mid<=vwap);
            if(!reclaim){ c_g_vwap++; continue; }
            double entry_px = ev.side>0? s.askc : s.bidc;
            double target = ev.side>0? ev.T + Sz : ev.T - Sz;   // measured-move
            double rem_bp = ev.side>0? (target-entry_px)/entry_px*1e4 : (entry_px-target)/entry_px*1e4;
            if(rem_bp < REM){ c_g_rem++; continue; }
            double sref = STOPMODE? ev.L0 : ev.T;
            double stop_px = ev.side>0? sref*(1.0-STOPBUF*1e-4) : sref*(1.0+STOPBUF*1e-4);
            double stop_bp = fabs(entry_px-stop_px)/entry_px*1e4;
            double opx, ots; in_pos=true;
            TP_PX = target;
            run_exit(ev.side, i, entry_px, stop_px, opx, ots);
            in_pos=false; last_exit_ts=(int32_t)ots;
            Trade t; t.ts_in=s.ts; t.ts_out=ots; t.side=ev.side; t.px_in=entry_px; t.px_out=opx;
            t.gross_bp=grossbp(ev.side,entry_px,opx); t.stop_bp=stop_bp;
            t.mfe_bp=g_mfe; t.mae_bp=g_mae;
            t.hod=(s.ts%86400)/3600; t.act_ratio=ev.act_ratio;
            time_t tt=(time_t)s.ts; struct tm g; gmtime_r(&tt,&g); t.month=(g.tm_year+1900)*100+(g.tm_mon+1);
            trades.push_back(t);
            ev.state=0;
        }
    }

    // ---------- reporting ----------
    auto report=[&](const char* tag, std::vector<Trade>& T){
        if(T.empty()){ printf("%s n=0\n", tag); return; }
        std::sort(T.begin(),T.end(),[](const Trade&a,const Trade&b){return a.ts_in<b.ts_in;});
        double t_mid = T.front().ts_in + (T.back().ts_in - T.front().ts_in)/2;
        for(double pad : PADS){
            double sum=0,wg=0,lg=0,h1=0,h2=0,worst=1e18,peak=0,dd=0,cum=0; int nw=0;
            std::map<int,double> bymo;
            for(auto& t: T){ double nb=t.gross_bp-pad; sum+=nb;
                if(nb>0){wg+=nb;nw++;} else lg-=nb;
                if(t.ts_in<t_mid) h1+=nb; else h2+=nb;
                worst=std::min(worst,nb); cum+=nb; peak=std::max(peak,cum); dd=std::max(dd,peak-cum);
                bymo[t.month]+=nb; }
            double pf = lg>0? wg/lg : 99;
            printf("%s pad%.0f n=%zu net=%.0fbp avg=%.1f PF=%.2f win%%=%.0f worst=%.0f maxDD=%.0f h1=%.0f h2=%.0f\n",
                   tag, pad, T.size(), sum, sum/T.size(), pf, 100.0*nw/T.size(), worst, dd, h1, h2);
            if(pad==PADS[0]){
                std::vector<double> mfes, maes, stops; double hold=0;
                for(auto&t:T){ mfes.push_back(t.mfe_bp); maes.push_back(t.mae_bp); stops.push_back(t.stop_bp); hold+=t.ts_out-t.ts_in; }
                std::sort(mfes.begin(),mfes.end()); std::sort(maes.begin(),maes.end()); std::sort(stops.begin(),stops.end());
                printf("  medMFE=%.1f medMAE=%.1f medStop=%.1f p75MFE=%.1f avgHold=%.0fs\n",
                       mfes[mfes.size()/2], maes[maes.size()/2], stops[stops.size()/2], mfes[mfes.size()*3/4], hold/T.size());
                printf("  bymonth:"); for(auto&m:bymo) printf(" %d:%.0f",m.first,m.second); printf("\n");
                std::map<int,double> byhod; std::map<int,int> nhod;
                for(auto&t:T){ byhod[t.hod]+=t.gross_bp-pad; nhod[t.hod]++; }
                printf("  byhod:"); for(auto&h:byhod) printf(" %02d:%.0f/%d",h.first,h.second,nhod[h.first]); printf("\n"); }
        }
    };

    if(getenv("DEBUG") && FAMILY==2)
        fprintf(stderr,"[funnel2] chk=%lld q=%lld atr=%lld ext=%lld arm=%lld brk=%lld | g_hmin=%lld g_vel=%lld g_upr=%lld g_rem=%lld -> trades=%zu\n",
                c2_chk,c2_q,c2_atr,c2_ext,c2_arm,c2_brk,c2_g_hmin,c2_g_vel,c2_g_upr,c2_g_rem,trades.size());
    if(getenv("DEBUG") && FAMILY==1)
        fprintf(stderr,"[funnel] cand=%lld move=%lld eff=%lld act=%lld spr=%lld imp=%lld pb=%lld | "
                "ab_ret=%lld ab_str=%lld ab_vwap=%lld ab_to=%lld | g_pbact=%lld g_brk=%lld g_upr=%lld g_vwap=%lld g_rem=%lld -> trades=%zu\n",
                c_cand,c_move,c_eff,c_act,c_spr,c_imp,c_pb,c_ab_ret,c_ab_str,c_ab_vwap,c_ab_to,
                c_g_pbact,c_g_brk,c_g_upr,c_g_vwap,c_g_rem,trades.size());

    if(!strcmp(mode,"STRAT")){
        std::vector<Trade> L,Sh;
        for(auto&t:trades) (t.side>0? L:Sh).push_back(t);
        printf("== %s | trades=%zu (L=%zu S=%zu) days_seen=%d ==\n", argv[1], trades.size(), L.size(), Sh.size(), days_seen);
        report("LONG ", L); report("SHORT", Sh);
        report("ALL  ", trades);
        // dump entries for RANDOM matching
        if(getenv("DUMP")){ FILE* d=fopen(getenv("DUMP"),"w");
            fprintf(d,"ts_in,side,stop_bp,hod,gross_bp,month\n");  // cols 5+ ignored by RANDOM reader
            for(auto&t:trades) fprintf(d,"%.0f,%d,%.2f,%d,%.2f,%d\n",t.ts_in,t.side,t.stop_bp,t.hod,t.gross_bp,t.month);
            fclose(d); }
    } else {
        // RANDOM: read entry spec, sample matched entries per seed
        const char* spec=getenv("SPEC"); if(!spec){ fprintf(stderr,"RANDOM needs SPEC=entries.csv\n"); return 1; }
        FILE* d=fopen(spec,"r"); if(!d){ fprintf(stderr,"no spec\n"); return 1; }
        char l2[128]; std::vector<std::array<double,3>> es; // side, stop_bp, hod
        if(fgets(l2,sizeof l2,d)){}
        while(fgets(l2,sizeof l2,d)){ double a,b,c,e; if(sscanf(l2,"%lf,%lf,%lf,%lf",&a,&b,&c,&e)==4) es.push_back({b,c,e}); }
        fclose(d);
        // index seconds by hour-of-day (post-warmup only: approximate = skip first WARMUP_DAYS days)
        int32_t d0=S.front().ts/86400;
        std::vector<std::vector<size_t>> byhod(24);
        for(size_t i=0;i<S.size();++i){ if(S[i].ts/86400 - d0 < WARMUP_DAYS) continue; byhod[(S[i].ts%86400)/3600].push_back(i); }
        double pad = PADS.size()? PADS[0]: 8;
        std::vector<double> nets;
        for(int seed=1; seed<=SEEDS; ++seed){
            uint64_t rng = 88172645463325252ull ^ (uint64_t)seed*2654435761u;
            auto rnd=[&](){ rng^=rng<<13; rng^=rng>>7; rng^=rng<<17; return rng; };
            // sample entry per real entry, sort, run non-overlapping
            std::vector<std::array<double,3>> picks; // secidx, side, stop_bp
            for(auto&e:es){ int h=(int)e[2]; auto& pool=byhod[h]; if(pool.empty()) continue;
                size_t idx=pool[rnd()%pool.size()]; picks.push_back({(double)idx, e[0], e[1]}); }
            std::sort(picks.begin(),picks.end(),[](auto&a,auto&b){return a[0]<b[0];});
            double net=0; double busy_until=0;
            for(auto&p:picks){ size_t i0=(size_t)p[0]; if(S[i0].ts<busy_until) continue;
                int side=(int)p[1]; double entry= side>0? S[i0].askc : S[i0].bidc;
                double stop = side>0? entry*(1.0-p[2]*1e-4) : entry*(1.0+p[2]*1e-4);
                double opx, ots; run_exit(side,i0,entry,stop,opx,ots);
                net += grossbp(side,entry,opx)-pad; busy_until=ots; }
            nets.push_back(net);
        }
        if(getenv("SEEDNETS")){ FILE* d=fopen(getenv("SEEDNETS"),"w");
            for(double x:nets) fprintf(d,"%.2f\n",x); fclose(d); }
        double m=0; for(double x:nets)m+=x; m/=nets.size();
        double v=0; for(double x:nets)v+=(x-m)*(x-m); v=sqrt(v/std::max<size_t>(1,nets.size()-1));
        double stratnet=envd("STRAT_NET",0);
        printf("RANDOM seeds=%d pad%.0f mean=%.0fbp sd=%.0fbp strat=%.0fbp z=%+.2f\n",
               SEEDS, pad, m, v, stratnet, v>0? (stratnet-m)/v : 0);
    }
    return 0;
}

// peachy_orb_nas.cpp — "one-candle confirmation" ORB-retest, NAS (NSXUSD).
//
// Distilled from the Peachy/PG one-candle-theory walkthrough on NQ:
//   1. Opening-Range = first OR_MIN minutes of the NY cash session (UTC window).
//   2. Breakout candle (5m) must (a) CLOSE beyond the OR level, (b) be a strong
//      trend-setting body: body/range >= BODY_FRAC and the wick in the breakout
//      direction <= PUSH_WICK*range, and (c) carry RISING relative volume:
//      vol > max(prev VOL_LB bars). Volume = TICK COUNT per 5m bar (NSXUSD spot
//      has no real volume — tick count is the standard intraday proxy; this is
//      the load-bearing approximation, flagged).
//   3. Entry = RETEST: limit at RETRACE into the breakout candle (0.5 of its
//      range; for a large impulse, range > BIG_ATR*ATR, retrace DEEP=0.75).
//      Stop = impulse-candle extreme (low for long). TP = TP_R * risk.
//   4. One shot/day, flat at FLAT_MIN. long_only toggle (counter-trend shorts
//      usually die on index up-drift — swept).
//
// Costs in points, round-trip. Walk-forward both-halves. Self-bars 5m+tickvol
// directly from the raw tick stream (ts_ms,askPrice,bidPrice).
//
// build: g++ -std=c++17 -O2 peachy_orb_nas.cpp -o peachy_orb
// run:   ./peachy_orb <ticks.csv> [OR_MIN=15] [sess_start=1330] [flat=1600]
//                     [body=0.6] [volLB=2] [retrace=0.5] [tp_r=2.0]
//                     [cost=1.0] [long_only=1] [name=NAS]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
using namespace std;

struct Bar { int64_t ts; double o,h,l,c; long vol; };  // ts = bar-start unix sec, vol = tick count

// Stream the raw tick file (timestamp_ms,askPrice,bidPrice) -> 5m mid bars + tick count.
static vector<Bar> bars_from_ticks(const string& p, int barSec){
    vector<Bar> v; ifstream f(p);
    if(!f){ fprintf(stderr,"no file %s\n",p.c_str()); return v; }
    string ln; bool first=true; int64_t W=barSec;
    int64_t cur=-1; Bar b{};
    while(getline(f,ln)){
        if(first){ first=false; if(!ln.empty() && (ln[0]<'0'||ln[0]>'9')) continue; }
        // parse three comma fields without alloc.
        const char* s=ln.c_str();
        char* e=nullptr;
        long long ms=strtoll(s,&e,10); if(e==s||*e!=',') continue;
        double ask=strtod(e+1,&e); if(*e!=',') continue;
        double bid=strtod(e+1,&e);
        if(ask<=0||bid<=0) continue;
        double mid=(ask+bid)*0.5;
        int64_t ts=ms/1000;
        int64_t g=(ts/W)*W;
        if(g!=cur){ if(cur>=0) v.push_back(b); cur=g; b.ts=g; b.o=b.h=b.l=b.c=mid; b.vol=1; }
        else { if(mid>b.h)b.h=mid; if(mid<b.l)b.l=mid; b.c=mid; b.vol++; }
    }
    if(cur>=0) v.push_back(b);
    return v;
}
// Load a ready-made 6-col bar file: ts,o,h,l,c,v  (ts sec|ms; v = REAL volume).
// Used for IBKR NQ 5m pulls (tools/nq_pull_history.py) -> real CME volume.
static vector<Bar> bars_from_barfile(const string& p){
    vector<Bar> v; ifstream f(p); if(!f) return v;
    string ln; bool first=true;
    while(getline(f,ln)){
        if(first){ first=false; if(!ln.empty() && (ln[0]<'0'||ln[0]>'9')) continue; }
        const char* s=ln.c_str(); char* e=nullptr;
        long long ts=strtoll(s,&e,10); if(e==s||*e!=',') continue;
        if(ts>100000000000LL) ts/=1000;                 // accept ms
        Bar b; b.ts=ts;
        b.o=strtod(e+1,&e); if(*e!=',')continue;
        b.h=strtod(e+1,&e); if(*e!=',')continue;
        b.l=strtod(e+1,&e); if(*e!=',')continue;
        b.c=strtod(e+1,&e);
        long vol = (*e==',') ? strtol(e+1,&e,10) : 0;
        b.vol = vol>0 ? vol : 1;
        if(b.h>0) v.push_back(b);
    }
    return v;
}
// Decide loader by peeking the first data line: 5 commas -> bar file, else tick.
static vector<Bar> load_any(const string& p){
    ifstream f(p); string ln; bool got=false;
    while(getline(f,ln)){ if(ln.empty())continue; if(ln[0]<'0'||ln[0]>'9'){continue;} got=true; break; }
    int commas=0; if(got) for(char ch:ln) if(ch==',') commas++;
    if(commas>=5) return bars_from_barfile(p);          // ts,o,h,l,c,v
    return bars_from_ticks(p, 300);                     // raw ticks -> 5m
}
static int  utc_hourmin(int64_t ts){ int s=(int)(((ts%86400)+86400)%86400); return (s/3600)*100 + (s%3600)/60; }
static int64_t utc_day(int64_t ts){ return ts/86400; }
static double atrAt(const vector<Bar>& b, int i, int n){
    if(i<1) return 0; int lo=max(1,i-n+1); double s=0; int c=0;
    for(int k=lo;k<=i;++k){ double tr=max(b[k].h-b[k].l, max(fabs(b[k].h-b[k-1].c), fabs(b[k].l-b[k-1].c))); s+=tr; c++; }
    return c? s/c : 0;
}

int main(int argc, char** argv){
    if(argc<2){ printf("usage: %s file [OR=15] [sess=1330] [flat=1600] [body=0.6] [volLB=2] [retr=0.5] [tpR=2.0] [cost=1.0] [LO=1]\n"
                       "        [maxStop=0] [closeBuf=0] [volRatio=1.0] [trendEMA=0] [minStop=0] [maxTrade=1] [BE_R=0] [trailATR=0]\n"
                       "        [recency=0] [dowMask=127] [pushWick=0.40] [atrMaxPts=0] [name]\n", argv[0]); return 1; }
    string path = argv[1];
    int    OR_MIN   = argc>2 ? atoi(argv[2]) : 15;
    int    SESS     = argc>3 ? atoi(argv[3]) : 1330;   // NY cash open, UTC hhmm (EDT)
    int    FLAT     = argc>4 ? atoi(argv[4]) : 1600;   // ~noon ET, UTC hhmm
    double BODY     = argc>5 ? atof(argv[5]) : 0.6;    // body/range min
    int    VOL_LB   = argc>6 ? atoi(argv[6]) : 2;      // relative-vol lookback bars
    double RETR     = argc>7 ? atof(argv[7]) : 0.5;    // retrace fraction into breakout candle
    double TP_R     = argc>8 ? atof(argv[8]) : 2.0;
    double COST     = argc>9 ? atof(argv[9]) : 1.0;    // round-trip points
    int    LONGONLY = argc>10? atoi(argv[10]): 1;
    // ---- HER DISCRETION (emulation layer; 0 = off) ----
    double MAXSTOP  = argc>11? atof(argv[11]): 0.0;  // reject setup if stop > MAXSTOP*ATR (her 20-30pt risk cap)
    double CLOSEBUF = argc>12? atof(argv[12]): 0.0;  // breakout candle must close beyond level by >= CLOSEBUF*ATR (convincing close)
    double VOLRATIO = argc>13? atof(argv[13]): 1.0;  // vol >= VOLRATIO * avg(prev VOL_LB)  (1.0 = just "greater")
    int    TRENDEMA = argc>14? atoi(argv[14]): 0;    // only trade with EMA(TRENDEMA) slope (0 = off)
    // ---- FULL LIFECYCLE LEVERS (0/default = off) ----
    double MINSTOP  = argc>15? atof(argv[15]): 0.0;  // reject setup if stop < MINSTOP*ATR (kill degenerate tight stops)
    int    MAXTRADE = argc>16? atoi(argv[16]): 1;    // max entries per day (re-arm after a close while < cap)
    double BE_R     = argc>17? atof(argv[17]): 0.0;  // move SL to breakeven once trade reaches BE_R*risk (0 off)
    double TRAILATR = argc>18? atof(argv[18]): 0.0;  // >0 => NO fixed TP, trail SL by TRAILATR*ATR (runner exit)
    int    RECENCY  = argc>19? atoi(argv[19]): 0;    // breakout must occur within RECENCY 5m bars of OR end (0 = no cap)
    int    DOWMASK  = argc>20? atoi(argv[20]): 127;  // weekday bitmask Mon=bit0..Sun=bit6 (127 = all)
    double PUSH_WICK= argc>21? atof(argv[21]): 0.40; // max wick on the push side, frac of range
    double ATRMAXP  = argc>22? atof(argv[22]): 0.0;  // skip if ATR > this many points (vol-regime cap; 0 off)
    string NAME     = argc>23? argv[23] : "NAS";

    const double BIG_ATR   = 2.0;    // candle range > BIG_ATR*ATR -> use deep retrace
    const double DEEP      = 0.75;   // deep retrace frac for large impulse candles
    const int    OR_END    = (SESS/100)*60 + (SESS%100) + OR_MIN;   // OR end, minute-of-day
    const int    SESS_MOD  = (SESS/100)*60 + (SESS%100);
    const int    FLAT_MOD  = (FLAT/100)*60 + (FLAT%100);

    auto bars = load_any(path);   // raw ticks -> 5m, OR a 6-col ts,o,h,l,c,v bar file (real volume)
    if((int)bars.size()<500){ printf("[%s] too few bars (%d)\n",NAME.c_str(),(int)bars.size()); return 1; }

    struct Trade{ int64_t t; int dir; double entry, sl, tp, rpts, netPts, netR; bool win; int regime; };
    vector<Trade> trades;

    // macro-regime classifier: daily last-close series; UP if today's close > close
    // REGIME_LB trading-days ago, else DOWN. Answers the bull-vs-bear question.
    const int REGIME_LB = 20;          // ~1 trading month
    vector<double> dayCloses;          // one per completed UTC day
    double dayLastClose = 0;

    // trend EMA over 5m closes (for HER context filter)
    double ema=0; bool emaInit=false; const double emaK = TRENDEMA>0 ? 2.0/(TRENDEMA+1) : 0;
    double emaPrev=0;

    // per-day state
    int64_t curDay=-1; double orHi=0, orLo=1e18; bool orDone=false; int tradesDay=0; int orEndIdx=-1;
    // pending breakout-candle retest order
    bool armed=false; int armDir=0; double armEntry=0, armSL=0;
    // open position
    bool inTrade=false; Trade cur{};

    auto minOfDay=[](int64_t ts){ int s=(int)(((ts%86400)+86400)%86400); return s/60; };
    auto weekday =[](int64_t ts){ return (int)(((utc_day(ts)+3)%7+7)%7); };   // Mon=0..Sun=6

    for(size_t i=0;i<bars.size();++i){
        const Bar& B = bars[i];
        int64_t d = utc_day(B.ts);
        int mod = minOfDay(B.ts);
        double atr = atrAt(bars,(int)i,14);
        // roll trend EMA on every closed 5m bar
        if(TRENDEMA>0){ emaPrev=ema; if(!emaInit){ema=B.c;emaInit=true;} else ema += emaK*(B.c-ema); }
        if(d!=curDay){ if(curDay>=0 && dayLastClose>0) dayCloses.push_back(dayLastClose);
                       curDay=d; orHi=0; orLo=1e18; orDone=false; tradesDay=0; orEndIdx=-1; armed=false; inTrade=false; }
        dayLastClose = B.c;

        // ---- manage open position intrabar (this 5m bar's range) ----
        if(inTrade){
            // breakeven ratchet: once favorable move reaches BE_R*risk, lock SL at entry
            if(BE_R>0 && cur.rpts>0){
                double fav = cur.dir>0 ? (B.h-cur.entry) : (cur.entry-B.l);
                if(fav >= BE_R*cur.rpts){ if(cur.dir>0 && cur.sl<cur.entry) cur.sl=cur.entry;
                                          if(cur.dir<0 && cur.sl>cur.entry) cur.sl=cur.entry; }
            }
            // runner trail: no fixed TP, drag SL by TRAILATR*ATR off the close
            if(TRAILATR>0 && atr>0){
                double t = cur.dir>0 ? (B.c-TRAILATR*atr) : (B.c+TRAILATR*atr);
                if(cur.dir>0 && t>cur.sl) cur.sl=t;
                if(cur.dir<0 && t<cur.sl) cur.sl=t;
            }
            bool hitSL = cur.dir>0 ? (B.l<=cur.sl) : (B.h>=cur.sl);
            bool hitTP = (TRAILATR>0) ? false : (cur.dir>0 ? (B.h>=cur.tp) : (B.l<=cur.tp));
            if(hitSL || hitTP){
                double ex = hitSL ? cur.sl : cur.tp;     // SL first (conservative); if both, SL
                double pts = cur.dir>0 ? (ex-cur.entry) : (cur.entry-ex);
                pts -= COST;
                cur.netPts=pts; cur.netR = cur.rpts>0? pts/cur.rpts:0; cur.win=pts>0;
                trades.push_back(cur); inTrade=false;
            } else if(mod>=FLAT_MOD){
                double pts = cur.dir>0 ? (B.c-cur.entry) : (cur.entry-B.c);
                pts -= COST; cur.netPts=pts; cur.netR=cur.rpts>0?pts/cur.rpts:0; cur.win=pts>0;
                trades.push_back(cur); inTrade=false;
            }
            continue;   // one position at a time
        }

        // ---- accumulate opening range ----
        if(mod>=SESS_MOD && mod<OR_END){ if(B.h>orHi)orHi=B.h; if(B.l<orLo)orLo=B.l; continue; }
        if(mod>=OR_END && !orDone && orHi>0 && orLo<1e18){ orDone=true; orEndIdx=(int)i; }

        // outside trade window -> nothing
        if(!orDone || tradesDay>=MAXTRADE || mod<OR_END || mod>=FLAT_MOD) continue;
        if(atr<=0) continue;

        // ---- try to fill an armed retest order on THIS bar ----
        if(armed){
            bool fill = armDir>0 ? (B.l<=armEntry) : (B.h>=armEntry);
            if(fill){
                double entry=armEntry;
                double rpts = armDir>0 ? (entry-armSL) : (armSL-entry);
                if(rpts>0.05*atr){
                    cur.t=B.ts; cur.dir=armDir; cur.entry=entry; cur.sl=armSL;
                    cur.tp = entry + armDir*TP_R*rpts; cur.rpts=rpts; cur.netPts=0; cur.win=false;
                    cur.regime = ((int)dayCloses.size()>=REGIME_LB && B.c < dayCloses[dayCloses.size()-REGIME_LB]) ? -1 : +1;
                    inTrade=true; tradesDay++; armed=false;
                    // re-check immediate SL/TP on same bar after fill (gap-through)
                    bool hitSL = cur.dir>0 ? (B.l<=cur.sl) : (B.h>=cur.sl);
                    bool hitTP = cur.dir>0 ? (B.h>=cur.tp) : (B.l<=cur.tp);
                    if(hitSL||hitTP){ double ex=hitSL?cur.sl:cur.tp; double pts=cur.dir>0?(ex-cur.entry):(cur.entry-ex);
                        pts-=COST; cur.netPts=pts; cur.netR=cur.rpts>0?pts/cur.rpts:0; cur.win=pts>0;
                        trades.push_back(cur); inTrade=false; }
                    continue;
                }
                armed=false;
            }
            // retest orders persist until filled or EOD (no expiry beyond session)
        }

        // ---- look for a fresh breakout candle (only if not already armed/in-trade) ----
        if(armed) continue;
        if(i<(size_t)(VOL_LB+1)) continue;
        // lifecycle gates: day-of-week, vol-regime cap, breakout recency
        if(!(DOWMASK & (1<<weekday(B.ts)))) continue;
        if(ATRMAXP>0 && atr>ATRMAXP) continue;
        if(RECENCY>0 && orEndIdx>=0 && (int)i-orEndIdx>RECENCY) continue;
        double rng = B.h-B.l; if(rng<=0) continue;
        double body = fabs(B.c-B.o);
        double bodyFrac = body/rng;
        if(bodyFrac < BODY) continue;

        // relative volume: vol >= VOLRATIO * avg(prev VOL_LB)  (expanding into the break)
        double vsum=0; for(int k=1;k<=VOL_LB;k++) vsum+=bars[i-k].vol;
        double vavg = vsum/VOL_LB;
        if(B.vol < VOLRATIO*vavg) continue;

        int dir=0;
        // long: CONVINCING close above OR high (clear by CLOSEBUF*ATR), bullish body, small UPPER wick
        if(B.c > orHi + CLOSEBUF*atr && B.c>B.o){
            double upWick = B.h-B.c;
            if(upWick <= PUSH_WICK*rng) dir=+1;
        }
        // short: convincing close below OR low
        else if(!LONGONLY && B.c < orLo - CLOSEBUF*atr && B.c<B.o){
            double dnWick = B.c-B.l;
            if(dnWick <= PUSH_WICK*rng) dir=-1;
        }
        if(dir==0) continue;

        // HER context filter: only trade with the trend (EMA rising for long, falling for short)
        if(TRENDEMA>0){
            bool up = ema>emaPrev;
            if(dir>0 && !up) continue;
            if(dir<0 &&  up) continue;
        }

        // arm retest order into the breakout candle
        double frac = (rng > BIG_ATR*atr) ? DEEP : RETR;
        double entry = dir>0 ? (B.h - frac*rng) : (B.l + frac*rng);
        double sl    = dir>0 ? B.l : B.h;

        // HER risk filter: skip the setup if the structural stop is too wide (she wants ~20-30pt risk,
        // rejects 100pt-impulse setups). stop dist measured from the retest entry to the impulse extreme.
        double sd = dir>0 ? (entry-sl) : (sl-entry);
        if(MAXSTOP>0 && sd > MAXSTOP*atr) continue;
        if(MINSTOP>0 && sd < MINSTOP*atr) continue;   // kill degenerate too-tight stops
        armed=true; armDir=dir; armEntry=entry; armSL=sl;
    }

    // ---- metrics ----
    auto report=[&](const char* tag,int lo,int hi){
        int n=0,w=0; double net=0,gw=0,gl=0,sumR=0,peak=0,cum=0,dd=0;
        for(int i=lo;i<hi;++i){ auto&t=trades[i]; n++; net+=t.netPts; sumR+=t.netR;
            if(t.win){w++;gw+=t.netPts;} else gl+=-t.netPts;
            cum+=t.netPts; if(cum>peak)peak=cum; if(peak-cum>dd)dd=peak-cum; }
        double pf=gl>0?gw/gl:(gw>0?99:0);
        printf("  %-5s n=%4d  WR=%4.1f%%  PF=%4.2f  net=%10.1fpt  avgR=%+5.2f  maxDD=%8.1fpt  exp=%+6.2fpt\n",
               tag,n,n?100.0*w/n:0,pf,net,n?sumR/n:0,dd,n?net/n:0);
    };
    printf("[%s] OR=%dm body=%.2f volLB=%d retr=%.2f tpR=%.1f cost=%.1f LO=%d | maxStop=%.1fA closeBuf=%.2fA volRatio=%.2f trendEMA=%d\n"
           "      minStop=%.1fA maxTrade=%d BE=%.1fR trail=%.1fA recency=%d dow=%d pushWick=%.2f atrMaxP=%.0f | bars=%d trades=%d\n",
           NAME.c_str(),OR_MIN,BODY,VOL_LB,RETR,TP_R,COST,LONGONLY,MAXSTOP,CLOSEBUF,VOLRATIO,TRENDEMA,
           MINSTOP,MAXTRADE,BE_R,TRAILATR,RECENCY,DOWMASK,PUSH_WICK,ATRMAXP,(int)bars.size(),(int)trades.size());
    if(trades.empty()){ printf("  (no trades)\n"); return 0; }
    report("ALL",0,(int)trades.size());
    int mid=(int)trades.size()/2;
    report("H1",0,mid);
    report("H2",mid,(int)trades.size());
    // ---- macro regime split (UP vs DOWN vs the 20-day trend) ----
    auto reportSel=[&](const char* tag,int wantReg){
        int n=0,w=0; double net=0,gw=0,gl=0,sumR=0,peak=0,cum=0,dd=0;
        for(auto&t:trades){ if(t.regime!=wantReg) continue; n++; net+=t.netPts; sumR+=t.netR;
            if(t.win){w++;gw+=t.netPts;} else gl+=-t.netPts;
            cum+=t.netPts; if(cum>peak)peak=cum; if(peak-cum>dd)dd=peak-cum; }
        double pf=gl>0?gw/gl:(gw>0?99:0);
        printf("  %-5s n=%4d  WR=%4.1f%%  PF=%4.2f  net=%10.1fpt  avgR=%+5.2f  maxDD=%8.1fpt  exp=%+6.2fpt\n",
               tag,n,n?100.0*w/n:0,pf,net,n?sumR/n:0,dd,n?net/n:0);
    };
    printf("  -- macro regime (price vs 20-day-ago close) --\n");
    reportSel("UP",  +1);
    reportSel("DOWN",-1);
    return 0;
}

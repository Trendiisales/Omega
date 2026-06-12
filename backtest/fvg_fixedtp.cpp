// fvg_continuation.cpp — mechanized ICT "continuation model" backtest.
//   Step 1: Draw-on-Liquidity (DOL) direction filter — nearest UNTAPPED
//           external level in trend direction (prior-day H/L + 15m swing).
//   Step 2: HTF (5m or 15m) FVG in the DOL direction = entry zone; price
//           must retrace into the gap.
//   Step 3: LTF (3m) IFVG "V-shape" reclaim inside the zone = trigger.
//           Stop = V extreme; target = the DOL. Optional BE at 1R.
// Costs in points (per instrument), walk-forward both-halves, session filter.
// build: g++ -std=c++17 -O2 fvg_continuation.cpp -o fvgc
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
using namespace std;

struct Bar { int64_t ts; double o,h,l,c; };

// ---- ISO ts parse: 2026-05-11T00:00:00Z -> unix seconds (UTC) ----
static int64_t iso2unix(const string& s){
    // days since epoch via civil calendar
    int Y=atoi(s.substr(0,4).c_str()), M=atoi(s.substr(5,2).c_str()), D=atoi(s.substr(8,2).c_str());
    int h=atoi(s.substr(11,2).c_str()), mi=atoi(s.substr(14,2).c_str()), se=atoi(s.substr(17,2).c_str());
    // days_from_civil (Howard Hinnant)
    int y = Y - (M <= 2);
    int era = (y>=0?y:y-399)/400;
    unsigned yoe = (unsigned)(y - era*400);
    unsigned doy = (153*(M + (M>2?-3:9)) + 2)/5 + D-1;
    unsigned doe = yoe*365 + yoe/4 - yoe/100 + doy;
    int64_t days = (int64_t)era*146097 + (int)doe - 719468;
    return days*86400 + h*3600 + mi*60 + se;
}
static vector<Bar> load(const string& p){
    vector<Bar> v; ifstream f(p); if(!f){fprintf(stderr,"no file %s\n",p.c_str());return v;}
    string ln; bool first=true;
    while(getline(f,ln)){
        if(first){first=false; if(!ln.empty() && (ln[0]<'0'||ln[0]>'9')) continue;}
        stringstream ss(ln); string t; vector<string> k;
        while(getline(ss,t,',')) k.push_back(t);
        if(k.size()<5) continue;
        Bar b; b.ts = (k[0].find('T')!=string::npos) ? iso2unix(k[0]) : (int64_t)atoll(k[0].c_str()); b.o=atof(k[1].c_str()); b.h=atof(k[2].c_str());
        b.l=atof(k[3].c_str()); b.c=atof(k[4].c_str());
        if(b.h>0) v.push_back(b);
    }
    return v;
}
// aggregate 1m bars to N-minute bars (group by floor(ts/(N*60)))
static vector<Bar> agg(const vector<Bar>& m1, int N){
    vector<Bar> out; int64_t W=N*60; int64_t cur=-1; Bar b{};
    for(auto& x: m1){
        int64_t g = (x.ts/W)*W;
        if(g!=cur){ if(cur>=0) out.push_back(b); cur=g; b.ts=g; b.o=x.o; b.h=x.h; b.l=x.l; b.c=x.c; }
        else { b.h=max(b.h,x.h); b.l=min(b.l,x.l); b.c=x.c; }
    }
    if(cur>=0) out.push_back(b);
    return out;
}
static int utc_hourmin(int64_t ts){ int s=(int)(ts%86400); return (s/3600)*100 + (s%3600)/60; }
static int64_t utc_day(int64_t ts){ return ts/86400; }

// ATR on a bar series (Wilder-ish simple) at index i, lookback n
static double atrAt(const vector<Bar>& b, int i, int n){
    if(i<1) return 0; int lo=max(1,i-n+1); double s=0; int c=0;
    for(int k=lo;k<=i;++k){ double tr=max(b[k].h-b[k].l, max(fabs(b[k].h-b[k-1].c), fabs(b[k].l-b[k-1].c))); s+=tr; c++; }
    return c? s/c : 0;
}

struct FVG { int dir; double lo, hi; int64_t ts; int barIdx; bool used; };

int main(int argc, char** argv){
    if(argc<2){ printf("usage: %s m1.csv [HTF=15] [sess=ny|all|lon] [costPts=0.5] [beR=0(off)|1] [name]\n",argv[0]); return 1; }
    string path=argv[1];
    int HTF   = argc>2? atoi(argv[2]) : 15;
    string SESS = argc>3? argv[3] : "ny";
    double COST = argc>4? atof(argv[4]) : 0.5;   // round-trip cost in points
    double BE_R = argc>5? atof(argv[5]) : 0.0;   // move to BE at this R (0=off)
    double TPR  = argc>6? atof(argv[6]) : 0.0;   // FIXED take-profit in R (0=off -> hold-to-DOL base)
    string NAME = argc>7? argv[7] : path;

    auto m1 = load(path);
    if((int)m1.size()<500){ printf("[%s] too few bars\n",NAME.c_str()); return 1; }
    auto htf = agg(m1, HTF);
    auto m3  = agg(m1, 3);

    // ---- DOL: prior-day high/low per UTC calendar day ----
    // build day -> (hi,lo); PDH/PDL(day) = hi/lo of (day-1)
    vector<int64_t> days; vector<double> dhi, dlo;
    {
        int64_t cd=-1; double hi=0,lo=0;
        for(auto& x:m1){ int64_t d=utc_day(x.ts);
            if(d!=cd){ if(cd>=0){days.push_back(cd);dhi.push_back(hi);dlo.push_back(lo);} cd=d; hi=x.h; lo=x.l; }
            else { hi=max(hi,x.h); lo=min(lo,x.l);} }
        if(cd>=0){days.push_back(cd);dhi.push_back(hi);dlo.push_back(lo);}
    }
    auto pdh=[&](int64_t ts)->double{ int64_t d=utc_day(ts); for(int i=(int)days.size()-1;i>=0;--i) if(days[i]<d) return dhi[i]; return -1; };
    auto pdl=[&](int64_t ts)->double{ int64_t d=utc_day(ts); for(int i=(int)days.size()-1;i>=0;--i) if(days[i]<d) return dlo[i]; return -1; };

    // map m1 index -> htf index already closed, and m3 index already closed
    auto inSession=[&](int64_t ts)->bool{
        int hm=utc_hourmin(ts);
        if(SESS=="all") return true;
        if(SESS=="ny")  return hm>=1330 && hm<=1500;   // 9:30-11:00 EDT
        if(SESS=="lon") return hm>=700  && hm<=1000;   // London AM
        return true;
    };

    // Detect HTF FVGs as they close; queue pending. (bar i uses i-2,i-1,i)
    // We iterate over m1 time; when an HTF bar closes, scan for new FVG.
    // Precompute HTF FVGs with close-time = htf[i].ts + HTF*60 (bar close).
    vector<FVG> htfFvg;
    for(int i=2;i<(int)htf.size();++i){
        double a=atrAt(htf,i,14); if(a<=0) continue;
        // bull: gap between candle1 high and candle3 low
        if(htf[i-2].h < htf[i].l){ double g=htf[i].l-htf[i-2].h; if(g>=0.05*a && g<=6*a)
            htfFvg.push_back({+1, htf[i-2].h, htf[i].l, htf[i].ts + (int64_t)HTF*60, i, false}); }
        if(htf[i-2].l > htf[i].h){ double g=htf[i-2].l-htf[i].h; if(g>=0.05*a && g<=6*a)
            htfFvg.push_back({-1, htf[i].h, htf[i-2].l, htf[i].ts + (int64_t)HTF*60, i, false}); }
    }

    // m3 close times for IFVG trigger
    // We'll walk m3 bars in the trade loop.

    // ---- trade sim ----
    struct Trade{ int64_t entT; double entry, sl, tp; int dir; double rpts; double netR; double netPts; bool win; };
    vector<Trade> trades;
    double atrHtfLast=0;

    // index pointers
    size_t fi=0;            // next htfFvg to activate by close-time
    vector<FVG> active;     // currently valid HTF FVGs (unmitigated, not expired)
    const int FVG_TTL_HTF = 24;   // HTF bars before an unmitigated FVG expires

    // We drive the sim on m3 bars (entry granularity = 3m), checking DOL+zone+IFVG.
    // For each m3 bar, "now" = bar close.
    bool inTrade=false; Trade cur{};
    double minLowSinceTag=0, maxHiSinceTag=0;

    // helper: nearest untapped DOL above/below price using PDH/PDL + 15m swing high/low (last 48 htf bars)
    auto htfIdxAt=[&](int64_t t)->int{ int idx=-1; for(int i=0;i<(int)htf.size();++i){ if(htf[i].ts + (int64_t)HTF*60 <= t) idx=i; else break;} return idx; };

    for(size_t bi=0; bi<m3.size(); ++bi){
        int64_t now = m3[bi].ts + 3*60;     // m3 bar close time
        double px = m3[bi].c;

        // activate HTF FVGs whose close-time <= now
        while(fi<htfFvg.size() && htfFvg[fi].ts<=now){ active.push_back(htfFvg[fi]); fi++; }
        // expire old / mitigated
        int hidx = htfIdxAt(now);
        atrHtfLast = hidx>0? atrAt(htf,hidx,14) : atrHtfLast;
        for(auto& f: active){ if(!f.used && hidx-f.barIdx>FVG_TTL_HTF) f.used=true; }

        // ---- manage open trade on this m3 bar ----
        if(inTrade){
            // BE
            if(BE_R>0){ double moved = cur.dir>0? (m3[bi].h-cur.entry) : (cur.entry-m3[bi].l);
                if(moved >= BE_R*cur.rpts){ if(cur.dir>0 && cur.sl<cur.entry) cur.sl=cur.entry;
                                            if(cur.dir<0 && cur.sl>cur.entry) cur.sl=cur.entry; } }
            bool hitSL = cur.dir>0? (m3[bi].l<=cur.sl) : (m3[bi].h>=cur.sl);
            bool hitTP = cur.dir>0? (m3[bi].h>=cur.tp) : (m3[bi].l<=cur.tp);
            if(hitSL || hitTP){
                double exit = hitSL? cur.sl : cur.tp;            // SL checked first (conservative)
                if(hitSL && hitTP) exit=cur.sl;
                double pts = cur.dir>0? (exit-cur.entry) : (cur.entry-exit);
                pts -= COST;
                cur.netPts=pts; cur.netR = cur.rpts>0? pts/cur.rpts : 0; cur.win = pts>0;
                trades.push_back(cur); inTrade=false;
            }
            continue;   // one position at a time
        }

        // ---- look for entry ----
        if(!inSession(now)) continue;
        if(atrHtfLast<=0) continue;

        // DOL: nearest untapped level in each direction
        double ph=pdh(now), pl=pdl(now);
        // 15m swing high/low over last 48 htf bars (excluding most recent 2)
        double swHi=-1, swLo=-1;
        if(hidx>=4){ int lo=max(0,hidx-48); for(int k=lo;k<=hidx-2;++k){ swHi=max(swHi,htf[k].h); if(swLo<0||htf[k].l<swLo) swLo=htf[k].l; } }
        double dolUp = -1, dolDn = -1;
        for(double lvl : {ph, swHi}) if(lvl>px+0.5*atrHtfLast) dolUp = (dolUp<0? lvl : min(dolUp,lvl));
        for(double lvl : {pl, swLo}) if(lvl>0 && lvl<px-0.5*atrHtfLast) dolDn = (dolDn<0? lvl : max(dolDn,lvl));

        // scan active HTF FVGs for a setup
        for(auto& f : active){
            if(f.used) continue;
            // continuation direction must point toward an untapped DOL
            if(f.dir>0 && dolUp<0) continue;
            if(f.dir<0 && dolDn<0) continue;
            // price must have retraced INTO the FVG zone on this m3 bar
            bool tagged = (m3[bi].l<=f.hi && m3[bi].h>=f.lo);
            if(!tagged) continue;

            // IFVG "V-shape" reclaim trigger on 3m: prior bar closed against trend,
            // this bar closes back through it (2-bar V) AND closes in trend dir.
            if(bi<2) continue;
            bool vlong  = f.dir>0 && m3[bi-1].c<m3[bi-1].o && m3[bi].c>m3[bi-1].h && m3[bi].c>m3[bi].o;
            bool vshort = f.dir<0 && m3[bi-1].c>m3[bi-1].o && m3[bi].c<m3[bi-1].l && m3[bi].c<m3[bi].o;
            if(!(vlong||vshort)) continue;

            double entry = m3[bi].c;
            // stop = V extreme (min low / max high of last 2 m3 bars)
            double slPx = f.dir>0? min(m3[bi].l,m3[bi-1].l) : max(m3[bi].h,m3[bi-1].h);
            double rpts = f.dir>0? (entry-slPx) : (slPx-entry);
            if(rpts<=0.1*atrHtfLast) continue;          // reject degenerate / too-tight
            // TARGET: TPR>0 => FIXED take-profit at TPR*R (the "8-12 tick / trade-with-
            // trend" scalp exit). The DOL-direction gate above still applies, so we
            // only take continuation setups pointing at untapped liquidity -- but we
            // bank a fixed multiple instead of holding all the way to the DOL.
            // TPR==0 => original hold-to-DOL base (for fidelity / A-B comparison).
            double tp;
            if(TPR>0){
                tp = f.dir>0? (entry + TPR*rpts) : (entry - TPR*rpts);
            } else {
                tp = f.dir>0? dolUp : dolDn;
                double rr = f.dir>0? (tp-entry)/rpts : (entry-tp)/rpts;
                if(rr<1.0) continue;                     // need >=1R to DOL
                if(rr>15) continue;                      // sanity
            }

            cur=Trade{now, entry, slPx, tp, f.dir, rpts, 0,0,false};
            inTrade=true; f.used=true;                   // consume the FVG
            break;
        }
    }

    // ---- metrics ----
    auto report=[&](const char* tag, int lo, int hi){
        int n=0,w=0; double net=0, gw=0, gl=0, sumR=0, peak=0, cum=0, dd=0;
        for(int i=lo;i<hi;++i){ auto&t=trades[i]; n++; net+=t.netPts; sumR+=t.netR;
            if(t.win){w++; gw+=t.netPts;} else gl+=-t.netPts;
            cum+=t.netPts; if(cum>peak)peak=cum; if(peak-cum>dd)dd=peak-cum; }
        double pf = gl>0? gw/gl : (gw>0?99:0);
        printf("  %-10s n=%3d  WR=%4.1f%%  PF=%4.2f  net=%9.1fpt  avgR=%+5.2f  maxDD=%7.1fpt  exp=%+5.2fpt\n",
               tag, n, n?100.0*w/n:0, pf, net, n?sumR/n:0, dd, n?net/n:0);
    };
    printf("[%s] HTF=%dm sess=%s cost=%.2fpt BE=%.0fR  | trades=%d\n",
           NAME.c_str(), HTF, SESS.c_str(), COST, BE_R, (int)trades.size());
    if(trades.empty()){ printf("  (no trades)\n"); return 0; }
    report("ALL", 0, (int)trades.size());
    int mid=(int)trades.size()/2;
    report("H1", 0, mid);
    report("H2", mid, (int)trades.size());
    return 0;
}

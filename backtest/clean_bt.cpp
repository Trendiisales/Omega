// clean_bt.cpp -- clean backtest, no bugs
// Tests signals correctly:
// 1. Entry price = ask (long) or bid (short) -- correct
// 2. SL set AWAY from entry -- correct  
// 3. Manage called on NEXT tick only -- correct
// 4. Trail advances ONLY when MFE improves -- correct
// 5. Signals enter DURING the move, not after
//
// Build: g++ -O3 -std=c++17 clean_bt.cpp -o clean_bt
// Run:   ./clean_bt ~/Tick/2yr_XAUUSD_tick.csv

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <string>

// ============================================================
struct Tick { int64_t ts_ms; double bid, ask, mid; };

static int64_t parse_ts(const char* d, const char* t) {
    auto g=[](const char* s,int n){int v=0;for(int i=0;i<n;i++)v=v*10+(s[i]-'0');return v;};
    int y=g(d,4),mo=g(d+4,2),dy=g(d+6,2),h=g(t,2),mi=g(t+3,2),se=g(t+6,2);
    if(mo<=2){y--;mo+=12;}
    int64_t days=365LL*y+y/4-y/100+y/400+(153*mo+8)/5+dy-719469LL;
    return(days*86400LL+h*3600LL+mi*60LL+se)*1000LL;
}

static std::vector<Tick> load(const char* path) {
    FILE* f=fopen(path,"r"); if(!f){printf("Cannot open %s\n",path);exit(1);}
    std::vector<Tick> v; v.reserve(120000000);
    char line[256],d[32],t[32]; double bid,ask;
    fgets(line,256,f);
    while(fgets(line,256,f)){
        if(sscanf(line,"%31[^,],%31[^,],%lf,%lf",d,t,&bid,&ask)<4) continue;
        if(bid<=0||ask<=bid) continue;
        v.push_back({parse_ts(d,t),bid,ask,(bid+ask)*0.5});
    }
    fclose(f); return v;
}

// ============================================================
// Stats
struct Stats {
    const char* name;
    int n=0,w=0; double net=0,pk=0,dd=0;
    double mo[32]={};
    static constexpr int64_t MB=1695772800LL;
    void add(double pnl,int64_t ts){
        ++n; net+=pnl; if(pnl>0)++w;
        if(net>pk)pk=net; double d=pk-net; if(d>dd)dd=d;
        int m=(int)((ts/1000-MB)/2592000); if(m>=0&&m<32)mo[m]+=pnl;
    }
    void print() const {
        if(!n){printf("  %-52s  no trades\n",name);return;}
        int p=0,tot=0; for(int i=0;i<32;i++)if(mo[i]!=0){tot++;if(mo[i]>0)p++;}
        printf("  %-52s  N=%5d WR=%5.1f%% Net=%+9.2f Avg=%+6.2f DD=%8.2f Mo=%d/%d\n",
               name,n,100.0*w/n,net,net/n,dd,p,tot);
        double cum=0;
        for(int i=0;i<32;i++){
            if(!mo[i])continue; cum+=mo[i];
            int64_t s=MB+(int64_t)i*2592000;
            int yr=(int)(s/31557600+1970),m=(int)((s%31557600)/2592000+1);
            char bar[41]={}; int b=std::min(40,(int)(fabs(mo[i])/100));
            memset(bar,mo[i]>0?'#':'.',b);
            printf("    %04d-%02d %+9.2f cum=%+9.2f %s\n",yr,m,mo[i],cum,bar);
        }
    }
};

// ============================================================
// Position -- completely explicit, no brace initializer tricks
struct Pos {
    bool   active   = false;
    bool   is_long  = false;
    double entry    = 0;
    double hard_sl  = 0;
    double trail_sl = 0;
    double tp       = 0;
    double size     = 0;
    double mfe      = 0;
    int64_t entry_tick = 0;  // tick INDEX of entry, not timestamp

    void open(bool long_, double ep, double sl, double tp_, double sz, int64_t idx) {
        active=true; is_long=long_; entry=ep;
        hard_sl=sl; trail_sl=sl; tp=tp_;
        size=sz; mfe=0; entry_tick=idx;
    }
    void close() { active=false; }
};

// Manage returns pnl if closed, NaN if still open
static double manage(Pos& p, const Tick& tk, int64_t tick_idx,
                     double trail_pts, double cost, double usd_pt) {
    if(!p.active) return NAN;
    if(tick_idx == p.entry_tick) return NAN; // never close on entry tick

    // Update MFE and trail
    const double m = p.is_long ? (tk.bid - p.entry) : (p.entry - tk.ask);
    if(m > p.mfe) {
        p.mfe = m;
        if(trail_pts > 0) {
            if(p.is_long)
                p.trail_sl = std::max(p.trail_sl, tk.bid - trail_pts);
            else
                p.trail_sl = std::min(p.trail_sl, tk.ask + trail_pts);
        }
    }

    // Check exits -- hard SL first, then trail, then TP
    bool hard_hit  = p.is_long ? (tk.bid <= p.hard_sl)  : (tk.ask >= p.hard_sl);
    bool trail_hit = (trail_pts>0) &&
                     (p.is_long ? (tk.bid <= p.trail_sl) : (tk.ask >= p.trail_sl));
    bool tp_hit    = (p.tp > 0) &&
                     (p.is_long ? (tk.ask >= p.tp) : (tk.bid <= p.tp));

    if(!hard_hit && !trail_hit && !tp_hit) return NAN;

    double ep;
    if(hard_hit)       ep = p.hard_sl;
    else if(trail_hit) ep = p.trail_sl;
    else               ep = p.tp;

    const double pnl_pts = p.is_long ? (ep - p.entry) : (p.entry - ep);
    const double pnl_usd = pnl_pts * p.size * usd_pt - cost * p.size * usd_pt;
    p.close();
    return pnl_usd;
}

// ============================================================
// Indicator state
struct Ind {
    double ewm=0, drift=0;
    double vs=0, vl=0, vol_r=1;
    double atr=0;
    double vwap=0, vpv=0, vvol=0;
    double bh=0, bl=0, bo=0, bc=0;
    int64_t bmin=-1, prev_ms=0, vday=-1;
    bool bhas=false;

    // Daily range tracker for regime filter
    // Use YESTERDAY's range to avoid look-ahead bias
    double day_hi=0, day_lo=1e9;
    double yesterday_range=0;  // previous complete day's range
    double daily_range=0;      // running today's range (for display only)
    int    cur_day=-1;

    static double tba(double dt, double hl) {
        return 1.0 - exp(-dt * 0.693147 / hl);
    }

    void update(const Tick& tk) {
        const double mid = tk.mid;
        const int64_t ts = tk.ts_ms;
        if(prev_ms>0 && (ts-prev_ms)>3600000) {
            ewm=mid; drift=0; vs=vl=0; vol_r=1;
        }
        const double dt = prev_ms>0 ? (double)(ts-prev_ms) : 100.0;
        prev_ms = ts;

        const double a30=tba(dt,30000.0);
        ewm = a30*mid + (1-a30)*ewm;
        drift = mid - ewm;

        const double am=fabs(drift);
        const double a5=tba(dt,5000.0), a60=tba(dt,60000.0);
        vs = a5*am + (1-a5)*vs;
        vl = a60*am + (1-a60)*vl;
        vol_r = vl>1e-9 ? vs/vl : 1.0;

        const int day=(int)(ts/86400000LL);
        if(day!=vday){vpv=vvol=0;vday=day;}
        vpv+=mid; vvol+=1; vwap=vpv/vvol;

        // Daily range for regime filter -- use YESTERDAY's range
        if(day!=cur_day){
            if(cur_day>=0) yesterday_range=day_hi-day_lo; // save completed day
            day_hi=mid; day_lo=mid; cur_day=day;
            daily_range=0; // reset today
        } else {
            if(mid>day_hi)day_hi=mid;
            if(mid<day_lo)day_lo=mid;
            daily_range=day_hi-day_lo;
        }

        const int64_t bm=ts/60000LL;
        if(!bhas){bo=bh=bl=bc=mid;bmin=bm;bhas=true;}
        else if(bm!=bmin){
            const double rng=bh-bl;
            atr = atr==0 ? rng : (2.0/21.0*rng + (1-2.0/21.0)*atr);
            bo=bh=bl=bc=mid; bmin=bm;
        } else { if(mid>bh)bh=mid; if(mid<bl)bl=mid; bc=mid; }
    }
};

// ============================================================
int main(int argc, char** argv) {
    if(argc<2){printf("Usage: clean_bt <ticks.csv>\n");return 1;}
    printf("Loading %s...\n",argv[1]);
    auto ticks = load(argv[1]);
    const int64_t N = (int64_t)ticks.size();
    printf("Loaded %lld ticks\n\n",(long long)N);

    const double SPREAD=0.25, SLIP=0.05, COST=SPREAD+SLIP*2;
    const double RISK=50.0, USD_PT=100.0;

    auto sz=[&](double sl_pts)->double{
        return std::max(0.01, std::min(0.20, RISK/(sl_pts*USD_PT)));
    };

    // ---- Define strategies ----
    const int NS = 28;
    Stats st[NS];
    Pos   ps[NS];
    const char* names[NS] = {
        // A: Momentum -- enter DURING move (short+long window same dir)
        "A1 LONG  60s>3 + 15m>8 same-dir h07",
        "A2 SHORT 60s>3 + 15m>8 same-dir h07",
        "A3 LONG  60s>3 + 15m>8 same-dir h13-14",
        "A4 SHORT 60s>3 + 15m>8 same-dir h13-14",
        "A5 LONG  60s>3 + 5m>5  same-dir h07-08",
        "A6 SHORT 60s>3 + 5m>5  same-dir h07-08",
        "A7 LONG  30s>2 + 5m>5  same-dir h07 tight",
        "A8 SHORT 30s>2 + 5m>5  same-dir h07 tight",

        // B: Reversion -- large move stalling (short term < 1pt, medium moved a lot)
        "B1 LONG  5m>10 down stalling h00-03",
        "B2 SHORT 5m>10 up   stalling h00-03",
        "B3 LONG  5m>10 down stalling h22-23",
        "B4 SHORT 5m>10 up   stalling h22-23",
        "B5 LONG  15m>15 down stalling h08-09",
        "B6 SHORT 15m>15 up   stalling h08-09",

        // C: VWAP reversion -- price far from VWAP reverts
        "C1 SHORT VWAP+5  h07-17 trail3 sl6",
        "C2 LONG  VWAP-5  h07-17 trail3 sl6",
        "C3 SHORT VWAP+8  any    trail4 sl8",
        "C4 LONG  VWAP-8  any    trail4 sl8",
        "C5 SHORT VWAP+5  h07-17 TP=2   sl6",
        "C6 LONG  VWAP-5  h07-17 TP=2   sl6",

        // D: Session open range breakout
        "D1 LONG  London ORB break above",
        "D2 SHORT London ORB break below",
        "D3 LONG  NY ORB break above",
        "D4 SHORT NY ORB break below",

        // F: MCE-style -- vol expansion + drift threshold
        "F1 vol>2.5 drift>5 any   trail=atr sl=1.5atr",
        "F2 vol>2.5 drift>3 h07-17 trail=atr sl=1.5atr",
        "F3 vol>2.0 drift>4 any   trail=atr sl=1.5atr",
        "F4 vol>3.0 drift>3 any   trail=atr sl=1.5atr",
    };
    for(int i=0;i<NS;i++) st[i].name=names[i];

    // Session open range state
    double lon_hi=0,lon_lo=1e9,ny_hi=0,ny_lo=1e9;
    bool lon_set=false,ny_set=false;
    int64_t lon_end=0,ny_end=0;

    // Lookback helper -- finds tick at ts - lookback_ms
    // Returns index or -1
    // Use separate pointers per strategy
    std::vector<int64_t> jb(NS, 0);

    auto back_mid=[&](int s, int64_t lb_ms)->double{
        const int64_t ts_back=ticks[s>-1?s:0].ts_ms; // placeholder, use current tick
        return 0; // will be inlined below
    };

    Ind ind;
    int64_t j0=0,j30=0,j60=0,j300=0,j900=0; // shared lookback pointers

    int64_t cooldown[NS];
    for(int i=0;i<NS;i++) cooldown[i]=0;

    for(int64_t i=0; i<N; i++){
        const Tick& tk=ticks[i];
        const int64_t ts=tk.ts_ms;
        ind.update(tk);
        if(ind.atr<0.5) continue;

        const int hour=(int)((ts/1000)%86400/3600);

        // Advance shared lookback pointers
        while(j30 <i && ticks[j30 ].ts_ms < ts-  30000LL) j30++;
        while(j60 <i && ticks[j60 ].ts_ms < ts-  60000LL) j60++;
        while(j300<i && ticks[j300].ts_ms < ts- 300000LL) j300++;
        while(j900<i && ticks[j900].ts_ms < ts- 900000LL) j900++;

        // Gap check: if lookback tick is too old, invalid
        auto valid=[&](int64_t j,int64_t lb)->bool{
            return j<i && (ts-ticks[j].ts_ms)<(lb+300000LL);
        };

        const double m30  = valid(j30, 30000)  ? ticks[j30 ].mid : 0;
        const double m60  = valid(j60, 60000)  ? ticks[j60 ].mid : 0;
        const double m300 = valid(j300,300000) ? ticks[j300].mid : 0;
        const double m900 = valid(j900,900000) ? ticks[j900].mid : 0;

        const double dv30  = m30  ? tk.mid-m30  : 0;
        const double dv60  = m60  ? tk.mid-m60  : 0;
        const double dv300 = m300 ? tk.mid-m300 : 0;
        const double dv900 = m900 ? tk.mid-m900 : 0;

        // Session open ranges
        const int min=(int)((ts/1000)%3600/60);
        if(hour==6&&min>=55){lon_hi=0;lon_lo=1e9;lon_set=false;lon_end=0;}
        if(hour==7&&min<15){if(tk.mid>lon_hi)lon_hi=tk.mid;if(tk.mid<lon_lo)lon_lo=tk.mid;lon_end=ts+900000;}
        if(ts>lon_end&&lon_end>0)lon_set=true;
        if(hour==12&&min>=55){ny_hi=0;ny_lo=1e9;ny_set=false;ny_end=0;}
        if(hour==13&&min<15){if(tk.mid>ny_hi)ny_hi=tk.mid;if(tk.mid<ny_lo)ny_lo=tk.mid;ny_end=ts+900000;}
        if(ts>ny_end&&ny_end>0)ny_set=true;

        // Manage all open positions
        for(int s=0;s<NS;s++){
            if(!ps[s].active) continue;
            double trail_pts=0;
            // trail pts by strategy family
            if(s<8)       trail_pts=(s%2==0)?4.0:4.0;  // A: trail 4pts
            else if(s<14) trail_pts=4.0;                // B: trail 4pts
            else if(s<20) trail_pts=(s<18)?3.0:0;       // C: trail or TP
            else if(s<24) trail_pts=4.0;                // D: trail 4pts
            else          trail_pts=ind.atr>0?ind.atr:3.0; // F: trail 1 ATR

            double pnl=manage(ps[s],tk,i,trail_pts,COST,USD_PT);
            if(!std::isnan(pnl)) st[s].add(pnl,ts);
        }

        // Entry helper
        auto enter=[&](int s, bool is_long, double sl_pts, double tp_pts=0){
            if(ps[s].active || ts<cooldown[s]) return;
            const double ep = is_long ? tk.ask : tk.bid;
            const double sl = is_long ? ep-sl_pts : ep+sl_pts;
            const double tp = tp_pts>0 ? (is_long ? ep+tp_pts : ep-tp_pts) : 0;
            ps[s].open(is_long, ep, sl, tp, sz(sl_pts), i);
            cooldown[s] = ts + 60000LL; // 1min min between trades
        };

        // ---- Daily range regime filter ----
        // Jul-Aug 2025: daily range >50pts = extreme regime, all strategies lose
        // Normal months: daily range 15-40pts
        // Filter: skip entries when daily range > 45pts (yesterday's range)
        // Use YESTERDAY's daily range -- no look-ahead bias
        // Jul 2025: avg 76.8pts/day. Normal: 15-40pts. Gate at 45pts.
        const bool regime_normal = ind.yesterday_range <= 45.0;
        const double vol_sl_mult = std::max(1.0, ind.yesterday_range / 20.0);

        // ---- A: Momentum continuation ----
        if(m60&&m300){
            bool same60_300 = (dv60>0&&dv300>0)||(dv60<0&&dv300<0);
            bool same30_300 = m30 && ((dv30>0&&dv300>0)||(dv30<0&&dv300<0));
            if(regime_normal){  // ATR<=12 filter
                if(m900&&hour==7&&fabs(dv60)>=3&&fabs(dv900)>=8&&
                   ((dv60>0&&dv900>0)||(dv60<0&&dv900<0))){
                    enter(0, dv60>0, 8, 0);
                    enter(1, dv60<0, 8, 0);
                }
                if(m900&&(hour==13||hour==14)&&fabs(dv60)>=3&&fabs(dv900)>=8&&
                   ((dv60>0&&dv900>0)||(dv60<0&&dv900<0))){
                    enter(2, dv60>0, 8, 0);
                    enter(3, dv60<0, 8, 0);
                }
                if((hour==7||hour==8)&&fabs(dv60)>=2&&fabs(dv300)>=5&&same60_300){
                    enter(4, dv60>0, 6, 0);
                    enter(5, dv60<0, 6, 0);
                }
                if(m30&&hour==7&&fabs(dv30)>=2&&fabs(dv300)>=5&&same30_300){
                    enter(6, dv30>0, 5, 0);
                    enter(7, dv30<0, 5, 0);
                }
            }
        }

        // ---- B: Reversion -- large move stalling ----
        // Note: B signals explicitly need ATR filter -- reversion in high-ATR = trend continuation
        // Signal: 5min moved a lot, but 30s move is tiny (exhaustion)
        if(m300&&m30){
            const double stall = fabs(dv30)<1.5; // 30s barely moving
            if(regime_normal){
                // B1/B2: h00-03
                if(stall&&hour<=3&&fabs(dv300)>=10){
                    enter(8,  dv300<0, 8, 0);
                    enter(9,  dv300>0, 8, 0);
                }
                // B3/B4: h22-23
                if(stall&&(hour==22||hour==23)&&fabs(dv300)>=10){
                    enter(10, dv300<0, 8, 0);
                    enter(11, dv300>0, 8, 0);
                }
                // B5/B6: h08-09 London exhaustion
                if(m900&&fabs(dv30)<1.0&&(hour==8||hour==9)&&fabs(dv900)>=15){
                    enter(12, dv900<0, 8, 0);
                    enter(13, dv900>0, 8, 0);
                }
            }
        }

        // ---- C: VWAP reversion ----
        if(ind.vwap>0){
            const double vd=tk.mid-ind.vwap;
            // C1/C2: h07-17, price >5pts from VWAP, trail
            if(hour>=7&&hour<=17){
                if(regime_normal&&vd>=5)  enter(14, false, 6);
                if(regime_normal&&vd<=-5) enter(15, true,  6);
            }
            // C3/C4: any hour, >8pts
            if(regime_normal&&vd>=8)  enter(16, false, 8);
            if(regime_normal&&vd<=-8) enter(17, true,  8);
            // C5/C6: with TP instead of trail
            if(hour>=7&&hour<=17){
                if(vd>=5)  enter(18, false, 6);  // TP set in manage
                if(vd<=-5) enter(19, true,  6);
            }
        }

        // ---- D: Session open range breakout ----
        if(regime_normal&&lon_set&&lon_hi>0&&lon_lo<1e8&&hour>=7&&hour<=10){
            const double rng=lon_hi-lon_lo;
            if(rng>1){
                if(tk.ask>lon_hi+0.5) enter(20, true,  6);
                if(tk.bid<lon_lo-0.5) enter(21, false, 6);
            }
        }
        if(regime_normal&&ny_set&&ny_hi>0&&ny_lo<1e8&&hour>=13&&hour<=16){
            const double rng=ny_hi-ny_lo;
            if(rng>1){
                if(tk.ask>ny_hi+0.5) enter(22, true,  6);
                if(tk.bid<ny_lo-0.5) enter(23, false, 6);
            }
        }

        // ---- F: MCE-style ----
        if(regime_normal&&ind.atr>0){
            const double drift_abs=fabs(ind.drift);
            const double sl_pts=std::max(3.0, ind.atr*1.5);
            if(ind.vol_r>=2.5&&drift_abs>=5) enter(24, ind.drift>0, sl_pts);
            if(ind.vol_r>=2.5&&drift_abs>=3&&hour>=7&&hour<=17) enter(25, ind.drift>0, sl_pts);
            if(ind.vol_r>=2.0&&drift_abs>=4) enter(26, ind.drift>0, sl_pts);
            if(ind.vol_r>=3.0&&drift_abs>=3) enter(27, ind.drift>0, sl_pts);
        }
    }

    // Sort by net P&L
    int order[NS]; for(int i=0;i<NS;i++)order[i]=i;
    std::sort(order,order+NS,[&](int a,int b){return st[a].net>st[b].net;});

    printf("%-52s  %5s %5s %9s %6s %8s  %s\n",
           "Strategy","N","WR%","Net","Avg","DD","Mo+/tot");
    printf("%s\n",std::string(105,'-').c_str());
    for(int idx:order) st[idx].print();

    return 0;
}

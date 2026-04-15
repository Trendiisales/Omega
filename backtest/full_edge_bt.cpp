// full_edge_bt.cpp -- comprehensive edge discovery across multiple signal types
// Tests: momentum continuation, mean reversion, VWAP reversion,
//        session open breakout, overnight gap fill, vol expansion
//
// Build: g++ -O3 -std=c++17 full_edge_bt.cpp -o full_edge_bt
// Run:   ./full_edge_bt ~/Tick/2yr_XAUUSD_tick.csv

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <array>

struct Tick { int64_t ts_ms; double bid, ask; };

static int64_t parse_ts(const char* d, const char* t) {
    auto gi=[](const char* s,int n){int v=0;for(int i=0;i<n;i++)v=v*10+(s[i]-'0');return v;};
    int y=gi(d,4),mo=gi(d+4,2),dy=gi(d+6,2),h=gi(t,2),mi=gi(t+3,2),se=gi(t+6,2);
    if(mo<=2){y--;mo+=12;}
    int64_t days=365LL*y+y/4-y/100+y/400+(153*mo+8)/5+dy-719469LL;
    return(days*86400LL+h*3600LL+mi*60LL+se)*1000LL;
}

static std::vector<Tick> load(const char* path) {
    FILE* f=fopen(path,"r");if(!f){printf("Cannot open %s\n",path);exit(1);}
    std::vector<Tick> v;v.reserve(120000000);
    char line[256],d[32],t[32];double bid,ask;
    fgets(line,256,f);
    while(fgets(line,256,f)){
        if(sscanf(line,"%[^,],%[^,],%lf,%lf",d,t,&bid,&ask)<4)continue;
        if(bid<=0||ask<=bid)continue;
        v.push_back({parse_ts(d,t),bid,ask});
    }
    fclose(f);return v;
}

static inline double tba(double dt,double hl){return 1.0-exp(-dt*0.693147/hl);}

// Shared indicators computed once per tick
struct Ind {
    double ewm=0,drift=0,vs=0,vl=0,vol_r=1;
    double atr=0,vwap=0,vpv=0,vvol=0;
    double bo=0,bh=0,bl=0,bc=0;
    int64_t bmin=-1,prev_ms=0,vday=-1;
    bool bhas=false;

    // Session open range (reset at London 07:00 and NY 13:00)
    double london_open_hi=0,london_open_lo=1e9;
    double ny_open_hi=0,   ny_open_lo=1e9;
    bool   london_range_set=false, ny_range_set=false;
    int64_t london_range_end=0, ny_range_end=0;

    // Previous day close
    double prev_close=0, today_open=0;
    int    prev_day=-1;

    void update(double bid, double ask, int64_t ts) {
        const double mid=(bid+ask)*0.5;
        if(prev_ms>0&&(ts-prev_ms)>3600000){
            ewm=mid;drift=0;vs=vl=0;vol_r=1;
        }
        const double dt=prev_ms>0?(double)(ts-prev_ms):100.0;
        prev_ms=ts;

        // EWM drift 30s
        const double a30=tba(dt,30000.0);
        ewm=a30*mid+(1-a30)*ewm; drift=mid-ewm;

        // Vol ratio 5s/60s
        const double am=fabs(drift);
        vs=tba(dt,5000.0)*am+(1-tba(dt,5000.0))*vs;
        vl=tba(dt,60000.0)*am+(1-tba(dt,60000.0))*vl;
        vol_r=vl>1e-9?vs/vl:1.0;

        // VWAP daily reset
        const int day=(int)(ts/86400000LL);
        if(day!=vday){
            if(vday>=0) prev_close=bc; // save yesterday's close
            vpv=vvol=0; vday=day;
            today_open=mid;
        }
        vpv+=mid; vvol+=1; vwap=vpv/vvol;

        // ATR M1
        const int64_t bm=ts/60000LL;
        if(!bhas){bo=bh=bl=bc=mid;bmin=bm;bhas=true;}
        else if(bm!=bmin){
            const double rng=bh-bl;
            if(atr==0)atr=rng; else{atr=2.0/21.0*rng+(1-2.0/21.0)*atr;}
            bo=bh=bl=bc=mid; bmin=bm;
        } else{if(mid>bh)bh=mid;if(mid<bl)bl=mid;bc=mid;}

        // Session open ranges
        const int h=(int)((ts/1000)%86400/3600);
        const int m=(int)((ts/1000)%3600/60);

        // London open: build range 07:00-07:15, set flag at 07:15
        if(h==7&&m<15){
            if(mid>london_open_hi)london_open_hi=mid;
            if(mid<london_open_lo)london_open_lo=mid;
            london_range_end=ts+900000LL; // 07:15
        }
        if(ts>london_range_end&&london_range_end>0) london_range_set=true;

        // NY open: build range 13:00-13:15
        if(h==13&&m<15){
            if(mid>ny_open_hi)ny_open_hi=mid;
            if(mid<ny_open_lo)ny_open_lo=mid;
            ny_range_end=ts+900000LL;
        }
        if(ts>ny_range_end&&ny_range_end>0) ny_range_set=true;

        // Reset session ranges daily
        if(h==6&&m==55){london_open_hi=0;london_open_lo=1e9;london_range_set=false;london_range_end=0;}
        if(h==12&&m==55){ny_open_hi=0;ny_open_lo=1e9;ny_range_set=false;ny_range_end=0;}
    }
};

struct Stats {
    int n=0,wins=0;
    double net=0,peak=0,dd=0;
    double monthly[32]={};
    static constexpr int64_t MB=1695772800LL;

    void add(double pnl,int64_t ts){
        n++;net+=pnl;if(pnl>0)wins++;
        if(net>peak)peak=net;
        double d=peak-net;if(d>dd)dd=d;
        int mo=(int)((ts/1000-MB)/2592000);
        if(mo>=0&&mo<32)monthly[mo]+=pnl;
    }
    void print(const char* lbl) const {
        if(!n){printf("  %-48s no trades\n",lbl);return;}
        int pos=0,tot=0;
        for(int i=0;i<32;i++)if(monthly[i]!=0){tot++;if(monthly[i]>0)pos++;}
        printf("  %-48s N=%5d WR=%5.1f%% Net=%+9.2f Avg=%+6.2f DD=%8.2f Mo=%d/%d\n",
               lbl,n,100.0*wins/n,net,net/n,dd,pos,tot);
        if(net>0||true){ // show monthly for all
            double cum=0;
            for(int i=0;i<32;i++){
                if(!monthly[i])continue;cum+=monthly[i];
                int64_t sec=MB+(int64_t)i*2592000;
                int yr=(int)(sec/31557600+1970),mo=(int)((sec%31557600)/2592000+1);
                char bar[41]={};int b=std::min(40,(int)(fabs(monthly[i])/100));
                memset(bar,monthly[i]>0?'#':'.',b);
                printf("    %04d-%02d %+8.2f cum=%+9.2f %s\n",yr,mo,monthly[i],cum,bar);
            }
        }
    }
};

struct Pos {
    bool   active=false,is_long=false;
    double entry=0,sl=0,trail_sl=0,tp=0,size=0,mfe=0;
    int64_t entry_ts=0,cooldown=0;
    int64_t opened_ts=0; // ts when opened -- skip manage on same tick
};

// Simple position manager: trail stop + hard SL + optional TP
static bool manage(Pos& p, double bid, double ask, int64_t ts,
                   double trail_pts, Stats& st,
                   const double COST, const double USD_PT) {
    if(!p.active) return false;
    if(p.opened_ts==ts) return false; // skip manage on entry tick
    const double mid=(bid+ask)*0.5;
    const double m=p.is_long?(bid-p.entry):(p.entry-ask);
    if(m>p.mfe){
        p.mfe=m;
        if(trail_pts>0){
            if(p.is_long) p.trail_sl=std::max(p.trail_sl,bid-trail_pts);
            else          p.trail_sl=std::min(p.trail_sl,ask+trail_pts);
        }
    }
    const bool sl_hit =(p.is_long&&bid<=p.sl )   ||(!p.is_long&&ask>=p.sl);
    const bool tr_hit =(trail_pts>0)&&
                       ((p.is_long&&bid<=p.trail_sl)||(!p.is_long&&ask>=p.trail_sl));
    const bool tp_hit =(p.tp>0)&&
                       ((p.is_long&&ask>=p.tp)     ||(!p.is_long&&bid<=p.tp));
    if(sl_hit||tr_hit||tp_hit){
        double ep=sl_hit?p.sl:(tp_hit?p.tp:(p.is_long?p.trail_sl:p.trail_sl));
        double pnl=(p.is_long?(ep-p.entry):(p.entry-ep))*p.size*USD_PT - COST*p.size*USD_PT;
        st.add(pnl,ts);
        p.active=false; p.cooldown=ts+60000LL; // 1min cooldown
        return true;
    }
    return false;
}

int main(int argc,char** argv){
    if(argc<2){printf("Usage: full_edge_bt <ticks.csv>\n");return 1;}
    printf("Loading %s...\n",argv[1]);
    auto ticks=load(argv[1]);
    const size_t N=ticks.size();
    printf("Loaded %zu ticks\n\n",N);

    const double SPREAD=0.25,SLIP=0.05,COST=SPREAD+SLIP*2;
    const double RISK=50.0,USD_PT=100.0;

    // =========================================================================
    // Define all strategies
    // =========================================================================

    // Each strategy has its own Pos and Stats
    // Signal families:
    //   A: Momentum continuation (large move -> continue)
    //   B: Mean reversion (large move -> reverse)
    //   C: VWAP reversion (price far from VWAP -> revert)
    //   D: Session open breakout (break above/below 15min open range)
    //   E: Gap fill (London open gaps from prior close)
    //   F: Vol expansion breakout (vol_ratio spike -> trade in direction)

    const int NS=32;
    Stats st[NS]; Pos  ps[NS];
    const char* labels[NS]={
        // A: Momentum continuation
        "A1: 15m>20pt h07 cont trail8 sl15",
        "A2: 15m>20pt h13-14 cont trail8 sl15",
        "A3: 30s>8pt h07 cont trail3 sl6",
        "A4: 5m>10pt h07 cont trail4 sl8",

        // B: Mean reversion (enter AGAINST prior move)
        "B1: 30s>8pt h00-03 REVERT trail4 sl8",
        "B2: 30s>8pt h22-23 REVERT trail4 sl8",
        "B3: 30s>6pt h08-09 REVERT trail3 sl6",
        "B4: 15m>20pt h02-03 REVERT trail8 sl15",
        "B5: 15m>15pt h00 REVERT trail6 sl12",
        "B6: 60s>8pt h00-03 REVERT trail4 sl8",
        "B7: 5m>15pt h22-23 REVERT trail6 sl12",
        "B8: 15m>10pt h08-09 REVERT trail5 sl10",

        // C: VWAP reversion
        "C1: VWAP+5pts SHORT trail3 sl6",
        "C2: VWAP+8pts SHORT trail4 sl8",
        "C3: VWAP-5pts LONG  trail3 sl6",
        "C4: VWAP-8pts LONG  trail4 sl8",
        "C5: VWAP+5pts h07-17 SHORT trail3 sl6",
        "C6: VWAP-5pts h07-17 LONG  trail3 sl6",

        // D: Session open range breakout
        "D1: London ORB LONG  trail4 sl6",
        "D2: London ORB SHORT trail4 sl6",
        "D3: NY ORB LONG  trail4 sl6",
        "D4: NY ORB SHORT trail4 sl6",
        "D5: London ORB LONG  trail6 sl10 (wider)",
        "D6: NY ORB SHORT trail6 sl10 (wider)",

        // E: Gap fill
        "E1: London gap>5pt UP   -> SHORT (gap fill down)",
        "E2: London gap>5pt DOWN -> LONG  (gap fill up)",
        "E3: London gap>3pt UP   -> SHORT",
        "E4: London gap>3pt DOWN -> LONG",

        // F: Vol expansion in drift direction
        "F1: vol_r>3 drift>3 all-hours trail3 sl6",
        "F2: vol_r>3 drift>3 h07-17 trail3 sl6",
        "F3: vol_r>4 drift>4 all-hours trail4 sl8",
        "F4: vol_r>3 drift>5 all-hours trail4 sl8",
    };

    Ind ind;
    // Track lookback for momentum signals
    std::array<size_t,NS> jb; jb.fill(0);

    for(size_t i=0;i<N;i++){
        const Tick& tk=ticks[i];
        const double bid=tk.bid,ask=tk.ask;
        const double mid=(bid+ask)*0.5;
        const int64_t ts=tk.ts_ms;
        ind.update(bid,ask,ts);
        if(ind.atr<0.5) continue;

        const int hour=(int)((ts/1000)%86400/3600);
        auto calc_sz=[&](double sl_pts)->double{
            return std::max(0.01,std::min(0.20,RISK/(sl_pts*USD_PT)));
        };

        // Manage all open positions first
        for(int s=0;s<NS;s++) manage(ps[s],bid,ask,ts,0,st[s],COST,USD_PT);

        // Helper: enter long
        auto enter=[&](int s,bool is_long,double sl_pts,double trail_pts,double tp_pts=0){
            if(ps[s].active||ts<ps[s].cooldown) return;
            const double ep=is_long?ask:bid;
            const double sl=is_long?ep-sl_pts:ep+sl_pts;
            const double tp=tp_pts>0?(is_long?ep+tp_pts:ep-tp_pts):0;
            const double sz=calc_sz(sl_pts);
            ps[s]={true,is_long,ep,sl,sl,tp,sz,0,ts,ps[s].cooldown,ts};
        };

        // Lookback helper
        auto back_mid=[&](int s,int64_t lb_ms)->double{
            const int64_t ts_back=ts-lb_ms;
            while(jb[s]<i&&ticks[jb[s]].ts_ms<ts_back) jb[s]++;
            if(jb[s]>=i) return 0;
            if(ts-ticks[jb[s]].ts_ms>(lb_ms+300000LL)) return 0;
            return (ticks[jb[s]].bid+ticks[jb[s]].ask)*0.5;
        };

        // ---- A: Momentum continuation ----
        {
            double m15=back_mid(0,900000); if(m15>0){double mv=mid-m15;
                if(fabs(mv)>=20&&hour==7) enter(0,mv>0,15,8);}
            double m15b=back_mid(1,900000); if(m15b>0){double mv=mid-m15b;
                if(fabs(mv)>=20&&(hour==13||hour==14)) enter(1,mv>0,15,8);}
            double m30=back_mid(2,30000); if(m30>0){double mv=mid-m30;
                if(fabs(mv)>=8&&hour==7) enter(2,mv>0,6,3);}
            double m5=back_mid(3,300000); if(m5>0){double mv=mid-m5;
                if(fabs(mv)>=10&&hour==7) enter(3,mv>0,8,4);}
        }

        // ---- B: Mean reversion (AGAINST prior move) ----
        {
            double m30=back_mid(4,30000); if(m30>0){double mv=mid-m30;
                if(fabs(mv)>=8&&(hour<=3)) enter(4,mv<0,8,4);} // revert: mv>0 -> SHORT
            double m30b=back_mid(5,30000); if(m30b>0){double mv=mid-m30b;
                if(fabs(mv)>=8&&(hour==22||hour==23)) enter(5,mv<0,8,4);}
            double m30c=back_mid(6,30000); if(m30c>0){double mv=mid-m30c;
                if(fabs(mv)>=6&&(hour==8||hour==9)) enter(6,mv<0,6,3);}
            double m15c=back_mid(7,900000); if(m15c>0){double mv=mid-m15c;
                if(fabs(mv)>=20&&(hour==2||hour==3)) enter(7,mv<0,15,8);}
            double m15d=back_mid(8,900000); if(m15d>0){double mv=mid-m15d;
                if(fabs(mv)>=15&&hour==0) enter(8,mv<0,12,6);}
            double m60=back_mid(9,60000); if(m60>0){double mv=mid-m60;
                if(fabs(mv)>=8&&hour<=3) enter(9,mv<0,8,4);}
            double m5b=back_mid(10,300000); if(m5b>0){double mv=mid-m5b;
                if(fabs(mv)>=15&&(hour==22||hour==23)) enter(10,mv<0,12,6);}
            double m15e=back_mid(11,900000); if(m15e>0){double mv=mid-m15e;
                if(fabs(mv)>=10&&(hour==8||hour==9)) enter(11,mv<0,10,5);}
        }

        // ---- C: VWAP reversion ----
        if(ind.vwap>0){
            const double vd=mid-ind.vwap;
            if(vd>=5)  enter(12,false,6,3);
            if(vd>=8)  enter(13,false,8,4);
            if(vd<=-5) enter(14,true, 6,3);
            if(vd<=-8) enter(15,true, 8,4);
            if(vd>=5&&hour>=7&&hour<=17)  enter(16,false,6,3);
            if(vd<=-5&&hour>=7&&hour<=17) enter(17,true, 6,3);
        }

        // ---- D: Session open range breakout ----
        if(ind.london_range_set&&ind.london_open_hi>0&&ind.london_open_lo<1e8){
            const double rng=ind.london_open_hi-ind.london_open_lo;
            if(rng>1.0&&hour>=7&&hour<=10){
                if(ask>ind.london_open_hi+0.5) enter(18,true, 6,4);
                if(bid<ind.london_open_lo-0.5) enter(19,false,6,4);
                if(ask>ind.london_open_hi+0.5) enter(20,true, 10,6);
            }
        }
        if(ind.ny_range_set&&ind.ny_open_hi>0&&ind.ny_open_lo<1e8){
            const double rng=ind.ny_open_hi-ind.ny_open_lo;
            if(rng>1.0&&hour>=13&&hour<=16){
                if(ask>ind.ny_open_hi+0.5) enter(21,true, 6,4);
                if(bid<ind.ny_open_lo-0.5) enter(22,false,6,4);
                if(bid<ind.ny_open_lo-0.5) enter(23,false,10,6);
            }
        }

        // ---- E: Gap fill ----
        if(ind.prev_close>0&&hour==7&&hour*60+((int)((ts/1000)%3600/60))<75){
            const double gap=ind.today_open-ind.prev_close;
            if(gap>=5)  enter(24,false,8,4); // gap up -> fade down
            if(gap<=-5) enter(25,true, 8,4); // gap down -> fade up
            if(gap>=3)  enter(26,false,6,3);
            if(gap<=-3) enter(27,true, 6,3);
        }

        // ---- F: Vol expansion ----
        {
            if(ind.vol_r>=3&&fabs(ind.drift)>=3) enter(28,ind.drift>0,6,3);
            if(ind.vol_r>=3&&fabs(ind.drift)>=3&&hour>=7&&hour<=17) enter(29,ind.drift>0,6,3);
            if(ind.vol_r>=4&&fabs(ind.drift)>=4) enter(30,ind.drift>0,8,4);
            if(ind.vol_r>=3&&fabs(ind.drift)>=5) enter(31,ind.drift>0,8,4);
        }
    }

    // Sort by net P&L
    int order[NS]; for(int i=0;i<NS;i++)order[i]=i;
    std::sort(order,order+NS,[&](int a,int b){return st[a].net>st[b].net;});

    printf("%-48s %5s %5s %9s %6s %8s %s\n","Strategy","N","WR%","Net","Avg","DD","Mo+/tot");
    printf("%s\n",std::string(100,'-').c_str());
    for(int idx:order) st[idx].print(labels[idx]);

    return 0;
}

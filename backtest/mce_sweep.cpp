// mce_sweep.cpp -- MCE threshold sweep in C++
// Finds optimal ATR/drift/vol thresholds for MacroCrashEngine.
// Mirrors MCE logic exactly: EWM drift 30s halflife, vol ratio 5s/60s,
// ATR M1 EMA-20. Entry on expansion + threshold breach. Trail SL.
//
// Build: g++ -O3 -std=c++17 mce_sweep.cpp -o mce_sweep
// Run:   ./mce_sweep ~/Tick/2yr_XAUUSD_tick.csv

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <vector>
#include <array>
#include <algorithm>

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

// Time-based EWM alpha
static inline double tba(double dt_ms, double hl_ms) {
    return 1.0 - exp(-dt_ms * 0.693147 / hl_ms);
}

struct Indicators {
    double ewm_mid=0, ewm_drift=0;
    double vs=0, vl=0, vol_ratio=1.0;
    double atr=0;
    double bo=0,bh=0,bl=0,bc=0; int64_t bmin=-1; bool bhas=false;
    int64_t prev_ms=0;
    static constexpr int64_t GAP_MS=3600000LL;

    void update(double bid, double ask, int64_t ts) {
        const double mid=(bid+ask)*0.5;
        if(prev_ms>0 && (ts-prev_ms)>GAP_MS) {
            ewm_mid=mid; ewm_drift=0; vs=vl=0; vol_ratio=1;
        }
        const double dt=prev_ms>0?(double)(ts-prev_ms):100.0;
        prev_ms=ts;

        // EWM drift 30s halflife
        const double a30=tba(dt,30000.0);
        ewm_mid=a30*mid+(1-a30)*ewm_mid;
        ewm_drift=mid-ewm_mid;

        // Vol ratio 5s/60s
        const double am=fabs(ewm_drift);
        const double a5=tba(dt,5000.0), a60=tba(dt,60000.0);
        vs=a5*am+(1-a5)*vs; vl=a60*am+(1-a60)*vl;
        vol_ratio=vl>1e-9?vs/vl:1.0;

        // ATR M1 EMA-20
        const int64_t bm=ts/60000LL;
        if(!bhas){bo=bh=bl=bc=mid;bmin=bm;bhas=true;}
        else if(bm!=bmin){
            const double rng=bh-bl;
            if(atr==0)atr=rng; else{const double a=2.0/21.0;atr=a*rng+(1-a)*atr;}
            bo=bh=bl=bc=mid;bmin=bm;
        } else{if(mid>bh)bh=mid;if(mid<bl)bl=mid;bc=mid;}
    }

    bool expansion() const { return vol_ratio>2.0 && fabs(ewm_drift)>2.0; }
};

struct Config {
    double atr_min, drift_min, vol_min;
    double sl_atr_mult;   // SL = entry +/- atr * mult
    double trail_atr_mult; // trail at atr * mult behind MFE
    int    max_hold_s;    // force close after N seconds
    bool   session_gate;  // block 17-22 UTC
    const char* label;
};

struct Stats {
    int n=0, wins=0;
    double net=0, peak=0, dd=0;
    double monthly[32]={};
    static constexpr int64_t MO_BASE=1695772800LL;

    void add(double pnl, int64_t ts_ms) {
        n++; net+=pnl; if(pnl>0)wins++;
        if(net>peak)peak=net;
        double d=peak-net; if(d>dd)dd=d;
        int mo=(int)((ts_ms/1000-MO_BASE)/2592000);
        if(mo>=0&&mo<32)monthly[mo]+=pnl;
    }
    void print(const char* lbl) const {
        if(!n){printf("  %-40s no trades\n",lbl);return;}
        int pos=0,tot=0; double ms=0;
        for(int i=0;i<32;i++)if(monthly[i]!=0){
            tot++; if(monthly[i]>0)pos++; ms+=monthly[i];
        }
        printf("  %-40s N=%4d WR=%5.1f%% Net=%+9.2f Avg=%+6.2f DD=%8.2f Mo=%d/%d\n",
               lbl,n,100.0*wins/n,net,net/n,dd,pos,tot);
        double cum=0;
        for(int i=0;i<32;i++){
            if(!monthly[i])continue; cum+=monthly[i];
            int64_t sec=MO_BASE+(int64_t)i*2592000;
            int yr=(int)(sec/31557600+1970),mo=(int)((sec%31557600)/2592000+1);
            char bar[41]={}; int b=std::min(40,(int)(fabs(monthly[i])/100));
            memset(bar,monthly[i]>0?'#':'.',b);
            printf("    %04d-%02d %+9.2f cum=%+9.2f %s\n",yr,mo,monthly[i],cum,bar);
        }
    }
};

int main(int argc, char** argv) {
    if(argc<2){printf("Usage: mce_sweep <ticks.csv>\n");return 1;}
    printf("Loading %s...\n",argv[1]);
    auto ticks=load(argv[1]);
    const size_t N=ticks.size();
    printf("Loaded %zu ticks\n\n",N);

    const double SPREAD=0.25, SLIP=0.05, COST=SPREAD+SLIP*2;
    const double RISK=50.0, USD_PT=100.0;
    const int64_t COOLDOWN_MS=120000LL; // 2min cooldown

    Config configs[]={
        // Original proven config
        {6.0, 5.0, 2.5, 1.5, 1.0, 3600, false, "ORIGINAL atr6 d5 v2.5"},
        // Slightly lower
        {5.0, 4.5, 2.0, 1.5, 1.0, 3600, false, "atr5.0 d4.5 v2.0"},
        {5.0, 4.0, 2.0, 1.5, 1.0, 3600, false, "atr5.0 d4.0 v2.0"},
        {4.5, 4.0, 2.0, 1.5, 1.0, 3600, false, "atr4.5 d4.0 v2.0"},
        {4.5, 3.5, 2.0, 1.5, 1.0, 3600, false, "atr4.5 d3.5 v2.0"},
        {4.0, 4.0, 2.0, 1.5, 1.0, 3600, false, "atr4.0 d4.0 v2.0"},
        {4.0, 3.5, 2.0, 1.5, 1.0, 3600, false, "atr4.0 d3.5 v2.0"},
        {4.0, 3.0, 2.0, 1.5, 1.0, 3600, false, "atr4.0 d3.0 v2.0"},
        {4.0, 3.0, 1.8, 1.5, 1.0, 3600, false, "atr4.0 d3.0 v1.8"},
        {3.5, 3.0, 1.8, 1.5, 1.0, 3600, false, "atr3.5 d3.0 v1.8"},
        // Session gated (block 17-22 UTC)
        {5.0, 4.0, 2.0, 1.5, 1.0, 3600, true,  "atr5.0 d4.0 v2.0 SESS"},
        {4.5, 3.5, 2.0, 1.5, 1.0, 3600, true,  "atr4.5 d3.5 v2.0 SESS"},
        {4.0, 3.0, 1.8, 1.5, 1.0, 3600, true,  "atr4.0 d3.0 v1.8 SESS"},
        // Wider SL -- survive volatility
        {5.0, 4.0, 2.0, 2.0, 1.5, 3600, false, "atr5.0 d4.0 sl2x trail1.5x"},
        {4.5, 3.5, 2.0, 2.0, 1.5, 3600, false, "atr4.5 d3.5 sl2x trail1.5x"},
        {4.0, 3.0, 2.0, 2.0, 1.5, 3600, false, "atr4.0 d3.0 sl2x trail1.5x"},
        // Tighter SL -- higher WR but more SL hits
        {5.0, 4.0, 2.0, 1.0, 0.8, 3600, false, "atr5.0 d4.0 sl1x trail0.8x"},
        {4.5, 3.5, 2.0, 1.0, 0.8, 3600, false, "atr4.5 d3.5 sl1x trail0.8x"},
    };
    const int NCFG=sizeof(configs)/sizeof(configs[0]);

    struct Pos {
        bool active=false, is_long=false;
        double entry=0, trail_sl=0, hard_sl=0, mfe=0, size=0;
        int64_t cooldown_until=0;
    };
    std::vector<Pos> pos(NCFG);
    std::vector<Stats> stats(NCFG);
    Indicators ind;

    for(size_t i=0;i<N;i++){
        const Tick& tk=ticks[i];
        const double mid=(tk.bid+tk.ask)*0.5;
        ind.update(tk.bid,tk.ask,tk.ts_ms);
        if(ind.atr<0.5) continue; // warmup

        const int hour=(int)((tk.ts_ms/1000)%86400/3600);

        for(int c=0;c<NCFG;c++){
            const Config& cfg=configs[c];
            Pos& p=pos[c];

            // Manage open position
            if(p.active){
                const double m=p.is_long?(tk.bid-p.entry):(p.entry-tk.ask);
                if(m>p.mfe){
                    p.mfe=m;
                    if(p.is_long) p.trail_sl=std::max(p.trail_sl, tk.bid-cfg.trail_atr_mult*ind.atr);
                    else          p.trail_sl=std::min(p.trail_sl, tk.ask+cfg.trail_atr_mult*ind.atr);
                }
                const bool max_hold=(tk.ts_ms-p.cooldown_until+COOLDOWN_MS)>(int64_t)cfg.max_hold_s*1000;
                const bool trail_hit=(p.is_long&&tk.bid<=p.trail_sl)||(!p.is_long&&tk.ask>=p.trail_sl);
                const bool hard_hit =(p.is_long&&tk.bid<=p.hard_sl )||(!p.is_long&&tk.ask>=p.hard_sl );
                if(trail_hit||hard_hit||max_hold){
                    double ep=hard_hit?p.hard_sl:(trail_hit?p.trail_sl:mid);
                    double pnl_pts=p.is_long?(ep-p.entry):(p.entry-ep);
                    double pnl_usd=pnl_pts*p.size*USD_PT-COST*p.size*USD_PT;
                    stats[c].add(pnl_usd,tk.ts_ms);
                    p.active=false;
                    p.cooldown_until=tk.ts_ms+COOLDOWN_MS;
                }
                continue;
            }

            // Entry gates
            if(tk.ts_ms<p.cooldown_until) continue;
            if(cfg.session_gate && hour>=17 && hour<22) continue;
            if(ind.atr < cfg.atr_min)    continue;
            if(fabs(ind.ewm_drift) < cfg.drift_min) continue;
            if(ind.vol_ratio < cfg.vol_min) continue;
            if(!ind.expansion()) continue;

            // Enter in direction of drift
            const bool is_long=ind.ewm_drift>0;
            const double ep=is_long?tk.ask:tk.bid;
            const double sl_pts=cfg.sl_atr_mult*ind.atr;
            const double hard_sl=is_long?ep-sl_pts:ep+sl_pts;
            const double trail_sl=hard_sl; // starts at hard SL
            const double sz=std::max(0.01,std::min(0.20,RISK/(sl_pts*USD_PT)));

            p.active=true; p.is_long=is_long; p.entry=ep;
            p.trail_sl=trail_sl; p.hard_sl=hard_sl;
            p.mfe=0; p.size=sz;
            // store entry time in cooldown_until temporarily
            p.cooldown_until=tk.ts_ms; // will be overwritten on close
        }
    }

    // Print results sorted by net P&L
    std::vector<int> order(NCFG);
    for(int i=0;i<NCFG;i++)order[i]=i;
    std::sort(order.begin(),order.end(),[&](int a,int b){return stats[a].net>stats[b].net;});

    printf("%-40s %4s %5s %9s %6s %8s %s\n","Config","N","WR%","Net","Avg","DD","Mo+/tot");
    printf("%s\n",std::string(85,'-').c_str());
    for(int idx:order) stats[idx].print(configs[idx].label);

    return 0;
}

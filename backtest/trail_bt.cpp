// trail_bt.cpp -- momentum continuation with trailing stop
// The edge: after a 20pt move in 15min, price continues.
// Extract it with: enter in direction, trail stop behind MFE, no time exit.
//
// Build: g++ -O3 -std=c++17 trail_bt.cpp -o trail_bt
// Run:   ./trail_bt ~/Tick/2yr_XAUUSD_tick.csv

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <vector>
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
    FILE* f=fopen(path,"r");if(!f){printf("Cannot open\n");exit(1);}
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

struct Config {
    int    lookback_s;   // measure move over this window
    double move_min;     // minimum pts move to trigger
    double trail_pts;    // trail stop distance in pts
    double sl_pts;       // hard SL (wider safety net)
    int    hour_min, hour_max;
    const char* label;
};

struct Stats {
    int n=0,wins=0;
    double net=0,peak=0,dd=0;
    double monthly[32]={};
    static constexpr int64_t MO_BASE=1695772800LL;

    void add(double pnl, int64_t ts) {
        n++;net+=pnl;if(pnl>0)wins++;
        if(net>peak)peak=net;
        double d=peak-net;if(d>dd)dd=d;
        int mo=(int)((ts/1000-MO_BASE)/2592000);
        if(mo>=0&&mo<32)monthly[mo]+=pnl;
    }
    void print(const char* lbl) const {
        if(!n){printf("  %-45s no trades\n",lbl);return;}
        int pos=0,tot=0;double ms=0;
        for(int i=0;i<32;i++)if(monthly[i]!=0){tot++;if(monthly[i]>0)pos++;ms+=monthly[i];}
        printf("  %-45s N=%4d WR=%5.1f%% Net=%+8.2f Avg=%+6.2f DD=%7.2f Mo=%d/%d\n",
               lbl,n,100.0*wins/n,net,net/n,dd,pos,tot);
        double cum=0;
        for(int i=0;i<32;i++){
            if(!monthly[i])continue;cum+=monthly[i];
            int64_t sec=MO_BASE+(int64_t)i*2592000;
            int yr=(int)(sec/31557600+1970),mo=(int)((sec%31557600)/2592000+1);
            char bar[41]={};int b=std::min(40,(int)(fabs(monthly[i])/100));
            memset(bar,monthly[i]>0?'#':'.',b);
            printf("    %04d-%02d %+9.2f cum=%+9.2f %s\n",yr,mo,monthly[i],cum,bar);
        }
    }
};

int main(int argc,char** argv){
    if(argc<2){printf("Usage: trail_bt <ticks.csv>\n");return 1;}
    printf("Loading...\n");
    auto ticks=load(argv[1]);
    const size_t N=ticks.size();
    printf("Loaded %zu ticks\n\n",N);

    const double SPREAD=0.25,SLIP=0.05,COST=SPREAD+SLIP*2;
    const double RISK=50.0,USD_PT=100.0;

    Config configs[]={
        // The proven signals from edge_refine_v4
        // Trail at 50% of initial move, hard SL at move size
        {900, 20.0,  8.0, 15.0,  7,  7, "15m>20pt h07 trail8 sl15"},
        {900, 20.0,  6.0, 15.0,  7,  7, "15m>20pt h07 trail6 sl15"},
        {900, 20.0, 10.0, 20.0,  7,  7, "15m>20pt h07 trail10 sl20"},
        {900, 20.0,  8.0, 15.0, 17, 17, "15m>20pt h17 trail8 sl15"},
        {900, 20.0,  8.0, 15.0, 23, 23, "15m>20pt h23 trail8 sl15"},
        {900, 20.0,  8.0, 15.0,  1,  1, "15m>20pt h01 trail8 sl15"},
        {900, 20.0,  8.0, 15.0,  0, 23, "15m>20pt all-hours trail8"},
        {900, 10.0,  5.0,  8.0,  7,  8, "15m>10pt h07-08 trail5 sl8"},
        {900, 10.0,  5.0,  8.0,  1,  2, "15m>10pt h01-02 trail5 sl8"},
        {900, 10.0,  5.0,  8.0, 13, 14, "15m>10pt h13-14 trail5 sl8"},
        { 30,  8.0,  3.0,  6.0,  7,  8, "30s>8pt h07-08 trail3 sl6"},
        { 30,  8.0,  3.0,  6.0, 13, 14, "30s>8pt h13-14 trail3 sl6"},
        // Wider trail -- let winners run more
        {900, 20.0, 15.0, 25.0,  7,  7, "15m>20pt h07 trail15 sl25"},
        {900, 20.0, 15.0, 25.0, 17, 17, "15m>20pt h17 trail15 sl25"},
        {900, 20.0, 15.0, 25.0, 23, 23, "15m>20pt h23 trail15 sl25"},
        // All active hours combined
        {900, 20.0,  8.0, 15.0,  6, 19, "15m>20pt h06-19 trail8"},
        {900, 10.0,  5.0,  8.0,  6, 19, "15m>10pt h06-19 trail5"},
    };
    const int NCFG=sizeof(configs)/sizeof(configs[0]);

    std::vector<Stats> stats(NCFG);
    struct Pos{
        bool active=false,is_long=false;
        double entry=0,trail_sl=0,hard_sl=0,mfe=0,size=0;
        int64_t entry_ts=0,last_close=0;
    };
    std::vector<Pos> pos(NCFG);
    std::vector<size_t> jb(NCFG,0);

    for(size_t i=0;i<N;i++){
        const Tick& tk=ticks[i];
        const double mid=(tk.bid+tk.ask)*0.5;
        const int hour=(int)((tk.ts_ms/1000)%86400/3600);

        for(int c=0;c<NCFG;c++){
            const Config& cfg=configs[c];
            Pos& p=pos[c];

            if(p.active){
                const double m=p.is_long?(tk.bid-p.entry):(p.entry-tk.ask);
                if(m>p.mfe){
                    p.mfe=m;
                    // Advance trail stop
                    if(p.is_long) p.trail_sl=std::max(p.trail_sl, tk.bid-cfg.trail_pts);
                    else          p.trail_sl=std::min(p.trail_sl, tk.ask+cfg.trail_pts);
                }
                bool trail_hit=(p.is_long&&tk.bid<=p.trail_sl)||(!p.is_long&&tk.ask>=p.trail_sl);
                bool hard_hit =(p.is_long&&tk.bid<=p.hard_sl )||(!p.is_long&&tk.ask>=p.hard_sl );
                if(trail_hit||hard_hit){
                    double ep=trail_hit?p.trail_sl:p.hard_sl;
                    double pnl_pts=p.is_long?(ep-p.entry):(p.entry-ep);
                    double pnl_usd=pnl_pts*p.size*USD_PT-COST*p.size*USD_PT;
                    stats[c].add(pnl_usd,tk.ts_ms);
                    p.active=false;p.last_close=tk.ts_ms;
                }
                continue;
            }

            if(hour<cfg.hour_min||hour>cfg.hour_max)continue;
            if(tk.ts_ms-p.last_close<(int64_t)cfg.lookback_s*500)continue;

            const int64_t ts_back=tk.ts_ms-(int64_t)cfg.lookback_s*1000;
            while(jb[c]<i&&ticks[jb[c]].ts_ms<ts_back)jb[c]++;
            if(jb[c]>=i)continue;
            if(tk.ts_ms-ticks[jb[c]].ts_ms>(int64_t)(cfg.lookback_s+300)*1000)continue;

            const double mid_back=(ticks[jb[c]].bid+ticks[jb[c]].ask)*0.5;
            const double move=mid-mid_back;
            if(fabs(move)<cfg.move_min)continue;

            const bool is_long=move>0;
            const double ep=is_long?tk.ask:tk.bid;
            const double sz=std::max(0.01,std::min(0.20,RISK/(cfg.sl_pts*USD_PT)));
            const double hard_sl=is_long?ep-cfg.sl_pts:ep+cfg.sl_pts;
            // Trail starts at hard SL, advances with price
            const double trail_sl=hard_sl;

            p={true,is_long,ep,trail_sl,hard_sl,0.0,sz,tk.ts_ms,p.last_close};
        }
    }

    printf("%-45s %4s %5s %9s %6s %7s %s\n","Config","N","WR%","Net","Avg","DD","Mo+/tot");
    printf("%s\n",std::string(90,'-').c_str());

    std::vector<int> order(NCFG);
    for(int i=0;i<NCFG;i++)order[i]=i;
    std::sort(order.begin(),order.end(),[&](int a,int b){return stats[a].net>stats[b].net;});
    for(int idx:order)stats[idx].print(configs[idx].label);

    return 0;
}

// momentum_engine_bt.cpp
// Pure C++ backtest of momentum continuation signal.
//
// Signal (from edge_refine_v4 results):
//   When gold moves > MOVE_MIN pts in LOOKBACK_S seconds,
//   enter in the direction of that move.
//   Exit after FORWARD_S seconds or SL hit.
//
// Best signals found:
//   30s move>8pt  -> 60s continuation: h07 EV+5.58, h13 EV+1.06
//   15m move>20pt -> 15m continuation: h07 EV+4.57, h14 EV+2.74, h17 EV+12.12
//
// Build: g++ -O3 -std=c++17 momentum_engine_bt.cpp -o momentum_engine_bt
// Run:   ./momentum_engine_bt ~/Tick/2yr_XAUUSD_tick.csv

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
    auto gi = [](const char* s, int n){ int v=0; for(int i=0;i<n;i++) v=v*10+(s[i]-'0'); return v; };
    int y=gi(d,4),mo=gi(d+4,2),dy=gi(d+6,2),h=gi(t,2),mi=gi(t+3,2),se=gi(t+6,2);
    if(mo<=2){y--;mo+=12;}
    int64_t days=365LL*y+y/4-y/100+y/400+(153*mo+8)/5+dy-719469LL;
    return (days*86400LL+h*3600LL+mi*60LL+se)*1000LL;
}

static std::vector<Tick> load(const char* path) {
    FILE* f=fopen(path,"r"); if(!f){printf("Cannot open %s\n",path);exit(1);}
    std::vector<Tick> v; v.reserve(120000000);
    char line[256],d[32],t[32]; double bid,ask;
    fgets(line,256,f);
    while(fgets(line,256,f)){
        if(sscanf(line,"%[^,],%[^,],%lf,%lf",d,t,&bid,&ask)<4) continue;
        if(bid<=0||ask<=bid) continue;
        v.push_back({parse_ts(d,t),bid,ask});
    }
    fclose(f); return v;
}

struct Config {
    int    lookback_s;   // measure move over this window
    double move_min;     // minimum pts move to trigger
    int    hold_s;       // hold time (forward window)
    double sl_pts;       // stop loss in pts (0 = no SL, time exit only)
    int    hour_min;     // UTC hour filter min (inclusive)
    int    hour_max;     // UTC hour filter max (inclusive)
    const char* label;
};

struct TradeStats {
    int n=0, wins=0;
    double net=0, peak=0, dd=0;
    double monthly[32]={};
    static constexpr int64_t MO_BASE=1695772800LL;

    void add(double pnl, int64_t exit_ts) {
        n++; net+=pnl; if(pnl>0)wins++;
        if(net>peak)peak=net;
        double d=peak-net; if(d>dd)dd=d;
        int mo=(int)((exit_ts/1000-MO_BASE)/2592000);
        if(mo>=0&&mo<32)monthly[mo]+=pnl;
    }
    void print(const char* label) const {
        if(!n){printf("  %-50s no trades\n",label);return;}
        double wr=100.0*wins/n;
        int pos_mo=0; double mo_sum=0;
        for(int i=0;i<32;i++) if(monthly[i]!=0){
            if(monthly[i]>0)pos_mo++;
            mo_sum+=monthly[i];
        }
        int total_mo=0; for(int i=0;i<32;i++) if(monthly[i]!=0)total_mo++;
        printf("  %-50s N=%5d WR=%5.1f%% Net=$%+8.2f Avg=$%+6.2f DD=$%7.2f Mo=%d/%d\n",
               label,n,wr,net,net/n,dd,pos_mo,total_mo);
        // Monthly breakdown
        double cum=0;
        for(int i=0;i<32;i++){
            if(monthly[i]==0)continue; cum+=monthly[i];
            int64_t sec=MO_BASE+(int64_t)i*2592000;
            int yr=(int)(sec/31557600+1970),mo=(int)((sec%31557600)/2592000+1);
            char bar[41]={}; int b=std::min(40,(int)(fabs(monthly[i])/50));
            memset(bar,monthly[i]>0?'#':'.',b);
            printf("    %04d-%02d  %+8.2f  cum=%+9.2f  %s\n",yr,mo,monthly[i],cum,bar);
        }
    }
};

int main(int argc, char** argv) {
    if(argc<2){printf("Usage: momentum_engine_bt <ticks.csv>\n");return 1;}
    printf("Loading %s...\n",argv[1]);
    auto ticks = load(argv[1]);
    const size_t N = ticks.size();
    printf("Loaded %zu ticks\n\n",N);

    const double SPREAD  = 0.25;  // cost per side
    const double SLIP    = 0.05;  // slippage
    const double COST    = SPREAD + SLIP*2;
    const double RISK    = 30.0;  // USD risk per trade
    const double USD_PT  = 100.0; // USD per point per lot

    // Test configs derived from edge_refine_v4 *** signals
    Config configs[] = {
        // Short-term: 30s move>8pt at London/NY open
        { 30,  8.0,  60,  3.0,  6, 14, "30s>8pt h06-14 SL3"},
        { 30,  8.0,  60,  4.0,  6, 14, "30s>8pt h06-14 SL4"},
        { 30,  8.0,  60,  6.0,  6, 14, "30s>8pt h06-14 SL6"},
        { 30,  8.0,  60,  0.0,  6, 14, "30s>8pt h06-14 no-SL"},
        // London open specifically
        { 30,  8.0,  60,  4.0,  7,  8, "30s>8pt h07-08 SL4"},
        { 30,  6.0,  120, 3.0,  7,  8, "30s>6pt h07-08 SL3"},
        // NY open specifically
        { 30,  8.0,  60,  4.0, 12, 14, "30s>8pt h12-14 SL4"},
        { 30,  8.0,  60,  4.0, 13, 13, "30s>8pt h13 SL4"},
        // Medium-term: 15m move>20pt
        { 900, 20.0, 900, 8.0,  0, 23, "15m>20pt all-hours SL8"},
        { 900, 20.0, 900, 8.0,  6, 19, "15m>20pt h06-19 SL8"},
        { 900, 20.0, 900,10.0,  7,  7, "15m>20pt h07 SL10"},
        { 900, 20.0, 900, 8.0, 14, 14, "15m>20pt h14 SL8"},
        { 900, 20.0, 900, 8.0, 17, 17, "15m>20pt h17 SL8"},
        { 900, 20.0, 900, 8.0, 15, 19, "15m>20pt h15-19 SL8"},
        // Medium-term: 15m move>10pt
        { 900, 10.0, 900, 5.0,  1,  2, "15m>10pt h01-02 SL5"},
        { 900, 10.0, 900, 5.0,  5,  6, "15m>10pt h05-06 SL5"},
        { 900, 10.0, 900, 5.0, 15, 19, "15m>10pt h15-19 SL5"},
        // 5m move>10pt
        {  300, 10.0, 300, 4.0,  7,  8, "5m>10pt h07-08 SL4"},
        {  300, 10.0, 300, 4.0, 11, 11, "5m>10pt h11 SL4"},
        {  300, 10.0, 300, 4.0, 17, 17, "5m>10pt h17 SL4"},
        {  300, 10.0, 300, 4.0, 19, 19, "5m>10pt h19 SL4"},
        {  300, 10.0, 300, 4.0, 23, 23, "5m>10pt h23 SL4"},
    };

    const int NCFG = sizeof(configs)/sizeof(configs[0]);
    std::vector<TradeStats> stats(NCFG);

    // Per-config position state
    struct Pos {
        bool active=false; bool is_long=false;
        double entry=0,sl=0,tp=0,size=0;
        int64_t entry_ts=0, exit_ts_target=0;
        int64_t last_close=0;
    };
    std::vector<Pos> positions(NCFG);

    // Two-pointer: lookback index per config
    std::vector<size_t> j_back(NCFG, 0);

    printf("Running %d configs...\n", NCFG);

    for(size_t i=0; i<N; i++) {
        const Tick& tk = ticks[i];
        const double mid = (tk.bid+tk.ask)*0.5;
        const int hour = (int)((tk.ts_ms/1000)%86400/3600);

        for(int c=0; c<NCFG; c++) {
            const Config& cfg = configs[c];
            Pos& pos = positions[c];
            TradeStats& st = stats[c];

            // Manage open position
            if(pos.active) {
                const double m = pos.is_long ? (tk.bid - pos.entry) : (pos.entry - tk.ask);
                bool sl_hit   = cfg.sl_pts>0 && ((pos.is_long && tk.bid<=pos.sl) || (!pos.is_long && tk.ask>=pos.sl));
                bool time_exit = tk.ts_ms >= pos.exit_ts_target;
                if(sl_hit || time_exit) {
                    double ep = sl_hit ? pos.sl : (pos.is_long ? tk.bid : tk.ask);
                    double pnl_pts = pos.is_long ? (ep-pos.entry) : (pos.entry-ep);
                    double pnl_usd = pnl_pts*pos.size*USD_PT - COST*pos.size*USD_PT;
                    st.add(pnl_usd, tk.ts_ms);
                    pos.active=false; pos.last_close=tk.ts_ms;
                }
                continue;
            }

            // Hour filter
            if(hour < cfg.hour_min || hour > cfg.hour_max) continue;

            // Cooldown: 2x hold time between trades
            if(tk.ts_ms - pos.last_close < (int64_t)cfg.hold_s*1000) continue;

            // Advance lookback pointer
            const int64_t ts_back = tk.ts_ms - (int64_t)cfg.lookback_s*1000;
            while(j_back[c] < i && ticks[j_back[c]].ts_ms < ts_back) j_back[c]++;

            if(j_back[c] >= i) continue;

            // Gap check: back tick must be within window (not a session gap)
            if(tk.ts_ms - ticks[j_back[c]].ts_ms > (int64_t)(cfg.lookback_s+300)*1000) continue;

            const double mid_back = (ticks[j_back[c]].bid+ticks[j_back[c]].ask)*0.5;
            const double move = mid - mid_back;  // signed pts

            if(fabs(move) < cfg.move_min) continue;

            // Entry in direction of move
            const bool is_long = move > 0;
            const double ep = is_long ? tk.ask : tk.bid;
            double sl_px = 0;
            if(cfg.sl_pts > 0)
                sl_px = is_long ? ep - cfg.sl_pts : ep + cfg.sl_pts;

            // Size by risk
            double sl_pts_eff = cfg.sl_pts > 0 ? cfg.sl_pts : 5.0;
            double sz = RISK / (sl_pts_eff * USD_PT);
            sz = floor(sz/0.001)*0.001;
            sz = fmax(0.01, fmin(0.20, sz));

            pos = {true, is_long, ep, sl_px, 0, sz,
                   tk.ts_ms, tk.ts_ms + (int64_t)cfg.hold_s*1000, pos.last_close};
        }
    }

    printf("\n%-50s %5s %5s %10s %7s %8s %s\n",
           "Config","N","WR%","Net","Avg","DD","Mo+/tot");
    printf("%s\n", std::string(95,'-').c_str());

    // Sort by net P&L
    std::vector<int> order(NCFG);
    for(int i=0;i<NCFG;i++) order[i]=i;
    std::sort(order.begin(),order.end(),[&](int a,int b){return stats[a].net>stats[b].net;});

    for(int idx : order) {
        stats[idx].print(configs[idx].label);
    }

    // Detailed monthly for profitable configs
    printf("\n\nDETAILED MONTHLY (profitable configs):\n");
    for(int idx : order) {
        if(stats[idx].net <= 0 || stats[idx].n < 10) continue;
        printf("\n%s:\n", configs[idx].label);
        double cum=0;
        for(int i=0;i<32;i++){
            if(stats[idx].monthly[i]==0)continue;
            cum+=stats[idx].monthly[i];
            int64_t sec=TradeStats::MO_BASE+(int64_t)i*2592000;
            int yr=(int)(sec/31557600+1970),mo=(int)((sec%31557600)/2592000+1);
            char bar[41]={}; int b=std::min(40,(int)(fabs(stats[idx].monthly[i])/50));
            memset(bar,stats[idx].monthly[i]>0?'#':'.',b);
            printf("  %04d-%02d  %+8.2f  cum=%+9.2f  %s\n",yr,mo,stats[idx].monthly[i],cum,bar);
        }
    }

    return 0;
}

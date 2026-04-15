#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

struct Tick { int64_t ts; double bid, ask; int hour; };

static inline int fast_int(const char* s, int n) {
    int v=0; for(int i=0;i<n;++i) v=v*10+(s[i]-'0'); return v;
}
static int64_t parse_ts(const char* d, const char* t) {
    int y=fast_int(d,4),mo=fast_int(d+4,2),dy=fast_int(d+6,2);
    int h=fast_int(t,2),mi=fast_int(t+3,2),se=fast_int(t+6,2);
    if(mo<=2){--y;mo+=12;}
    int64_t days=365LL*y+y/4-y/100+y/400+(153LL*mo+8)/5+dy-719469LL;
    return days*86400LL+h*3600LL+mi*60LL+se;
}
static inline double mid(const Tick& x){ return 0.5*(x.bid+x.ask); }

static std::vector<Tick> load_csv(const char* path) {
    FILE* f=fopen(path,"r"); if(!f){perror(path);exit(1);}
    std::vector<Tick> v; v.reserve(120000000);
    char line[256],d[32],t[32]; double bid,ask;
    fgets(line,sizeof(line),f);
    while(fgets(line,sizeof(line),f)){
        if(sscanf(line,"%31[^,],%31[^,],%lf,%lf",d,t,&bid,&ask)!=4) continue;
        if(bid<=0.0||ask<=bid) continue;
        int hour=(t[0]-'0')*10+(t[1]-'0');
        v.push_back({parse_ts(d,t),bid,ask,hour});
    }
    fclose(f); return v;
}

struct Stats {
    long long n=0,wins=0;
    double sum_gross=0,sum_net=0,sum_mfe=0,sum_mae=0;
    std::vector<double> nets;
};
static double median(std::vector<double> v){
    if(v.empty())return 0;
    std::sort(v.begin(),v.end());
    size_t n=v.size();
    return (n&1)?v[n/2]:0.5*(v[n/2-1]+v[n/2]);
}

int main(int argc, char** argv){
    if(argc<2){printf("usage: pullback_entry_test file.csv\n");return 1;}

    // FIX: cost in POINTS not dollars
    const double cost_pts = 0.30; // 0.25 spread + 0.05 slippage

    const int    lookbacks[]      = {300, 600, 900};
    const double move_min_pts[]   = {10.0, 20.0, 30.0};
    const double pullback_fracs[] = {0.20, 0.30, 0.40};
    const int    pb_waits[]       = {300, 600, 900};
    const int    holds[]          = {300, 600, 900};

    std::vector<Tick> ticks = load_csv(argv[1]);
    printf("ticks: %zu\n", ticks.size());
    if(ticks.size()<1000) return 0;
    const size_t N = ticks.size();

    for(int lookback_s : lookbacks){
    for(double move_min : move_min_pts){
    for(double pb_frac : pullback_fracs){
    for(int pb_wait_s : pb_waits){
    for(int hold_s : holds){

        Stats by_hour[24];

        // FIX: reset all pointers per config
        size_t j=0, p=0, k=0;

        for(size_t i=0; i<N; ++i){
            // Advance lookback pointer
            while(j<i && ticks[j].ts < ticks[i].ts - lookback_s) ++j;
            if(j>=i) continue;

            double m0 = mid(ticks[j]);
            double m1 = mid(ticks[i]);
            double move_pts = m1 - m0;
            if(fabs(move_pts) < move_min) continue;

            // Gap filter
            if(ticks[i].ts - ticks[j].ts > lookback_s + 3600) continue;

            bool long_dir = (move_pts > 0.0);
            double signal_mid  = m1;
            double retrace_pts = fabs(move_pts) * pb_frac;
            double wanted_mid  = long_dir ? (signal_mid - retrace_pts)
                                          : (signal_mid + retrace_pts);

            // Find pullback entry within pb_wait_s
            // FIX: use local search pointer (not shared p) to avoid bleed
            size_t pe = i+1;
            bool found = false;
            while(pe < N && ticks[pe].ts <= ticks[i].ts + pb_wait_s){
                double pmid = mid(ticks[pe]);
                if((long_dir && pmid <= wanted_mid) ||
                   (!long_dir && pmid >= wanted_mid)){
                    found = true;
                    break;
                }
                ++pe;
            }
            if(!found) continue;

            size_t entry_idx  = pe;
            double entry_price = long_dir ? ticks[entry_idx].ask
                                          : ticks[entry_idx].bid;

            // Find exit after hold_s
            size_t ke = entry_idx+1;
            while(ke < N && ticks[ke].ts <= ticks[entry_idx].ts + hold_s) ++ke;
            if(ke >= N) continue;
            size_t kk = ke-1;
            if(kk <= entry_idx) continue;

            // Compute MFE/MAE/final P&L
            double best=-1e18, worst=1e18, final_pnl=0;
            for(size_t u=entry_idx+1; u<=kk; ++u){
                double pnl = long_dir ? (ticks[u].bid - entry_price)
                                      : (entry_price - ticks[u].ask);
                if(pnl>best)  best=pnl;
                if(pnl<worst) worst=pnl;
                final_pnl = pnl;
            }

            double net = final_pnl - cost_pts;
            int h = ticks[i].hour;
            Stats& s = by_hour[h];
            s.n++;
            if(net>0) s.wins++;
            s.sum_gross += final_pnl;
            s.sum_net   += net;
            s.sum_mfe   += best;
            s.sum_mae   += worst;
            s.nets.push_back(net);
        }

        // Print only hours with interesting results
        bool any = false;
        for(int h=0;h<24;h++){
            const Stats& s = by_hour[h];
            if(s.n < 50) continue;
            double wr      = 100.0*s.wins/s.n;
            double avg_net = s.sum_net/s.n;
            double med_net = median(s.nets);
            if(avg_net > 0.5 || med_net > 1.0 || wr > 58.0){
                if(!any){
                    printf("\nLB %ds MOVE %.0fpt PB %.0f%% PBWAIT %ds HOLD %ds\n",
                           lookback_s,move_min,pb_frac*100,pb_wait_s,hold_s);
                    any=true;
                }
                printf("  h%02d n=%4lld WR=%5.1f%% avg_gross=%+7.2f avg_net=%+7.2f "
                       "med_net=%+7.2f mfe=%+7.2f mae=%+7.2f\n",
                       h, s.n, wr,
                       s.sum_gross/s.n, avg_net, med_net,
                       s.sum_mfe/s.n, s.sum_mae/s.n);
            }
        }

    }}}}}

    return 0;
}

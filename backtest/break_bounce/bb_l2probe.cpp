// bb_l2probe.cpp -- does gold L2 imbalance PREDICT forward price? (the real test)
//
// Uses the recorded VPS gold L2 (l2_ticks_XAUUSD_*.csv). Depth/volume cols are
// dead (all zero) but l2_imb carries real signal (0..1, sd ~0.20). Two things:
//
//   1) PREDICTIVE POWER: bucket rows by l2_imb, report mean forward mid-return
//      at horizons 30/60/300s. If bid-heavy (imb>0.5) -> price up and ask-heavy
//      -> down, imbalance leads price (momentum) and a tight-TF L2 engine is
//      possible. If the reverse, it's a reversion signal. If flat, no edge.
//
//   2) TRADEABLE SIM: trade the signal with REAL spread (enter long@ask /
//      short@bid, exit @bid/ask after HOLD s), both directions, and report
//      PF / WR / Sharpe / net AFTER cost. This is the does-it-actually-work.
//
// Columns: ts_ms(0) mid(1) bid(2) ask(3) l2_imb(4) ... vpin(13) ... ewm_drift(16)
//
// Build: g++ -O3 -std=c++17 bb_l2probe.cpp -o bbl2probe
// Run:   ./bbl2probe <l2_day1.csv> [l2_day2.csv ...]

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

struct Row { int64_t ts; double mid, bid, ask, imb, vpin, drift; };

static std::vector<Row> load(const char* path) {
    std::vector<Row> v; std::ifstream in(path); if(!in.is_open()) return v;
    std::string line; std::getline(in,line); // header
    while(std::getline(in,line)){
        // split first 17 cols
        double f[17]; int c=0; const char* p=line.c_str(); char* e;
        while(c<17){ f[c]=std::strtod(p,&e); if(e==p)break; c++; if(*e==',')p=e+1; else break; }
        if(c<17) continue;
        Row r{(int64_t)f[0], f[1],f[2],f[3],f[4],f[13],f[16]};
        if(r.bid>0&&r.ask>=r.bid) v.push_back(r);
    }
    return v;
}

int main(int argc,char** argv){
    if(argc<2){ std::printf("usage: bbl2probe <l2.csv ...>\n"); return 1; }
    std::vector<std::vector<Row>> days;
    size_t total=0;
    for(int i=1;i<argc;i++){ auto d=load(argv[i]); if(d.size()>100){ total+=d.size(); days.push_back(std::move(d)); } }
    std::printf("loaded %zu rows across %zu days\n", total, days.size());
    if(days.empty()) return 1;

    // ---- 1) Predictive power: bucket by imb, mean forward return @ horizons ----
    const int64_t H[3]={30000,60000,300000}; const char* HL[3]={"30s","60s","5m"};
    struct B{const char* l;double lo,hi;} bk[]={{"imb<0.30 (ask-heavy)",0,0.30},{"0.30-0.45",0.30,0.45},
        {"0.45-0.55 (neutral)",0.45,0.55},{"0.55-0.70",0.55,0.70},{"imb>0.70 (bid-heavy)",0.70,1.01}};
    for(int h=0;h<3;h++){
        double sum[5]={0}; long cnt[5]={0};
        for(auto& d:days){ size_t j=0;
            for(size_t i=0;i<d.size();i++){
                if(j<i)j=i; while(j<d.size() && d[j].ts < d[i].ts+H[h]) j++;
                if(j>=d.size())break;
                double fwd=d[j].mid-d[i].mid;
                for(int b=0;b<5;b++) if(d[i].imb>=bk[b].lo&&d[i].imb<bk[b].hi){ sum[b]+=fwd; cnt[b]++; break; }
            } }
        std::printf("\nforward mid-return @%s (price units, mean):\n", HL[h]);
        for(int b=0;b<5;b++) std::printf("  %-22s n=%-8ld mean=%+.4f\n", bk[b].l, cnt[b], cnt[b]?sum[b]/cnt[b]:0);
    }

    // ---- 2) Tradeable sim: REVERSION (the proven direction) + spread gate ----
    // Reversion: bid-heavy (imb>=HI) -> SHORT; ask-heavy (imb<=LO) -> LONG.
    // spmax = only enter when ask-bid <= spmax (simulates tight-spread futures).
    auto sim=[&](double HI,double LO,int64_t HOLD_ms,double spmax){
        int n=0,w=0; double net=0,gw=0,gl=0,sum=0,sum2=0;
        for(auto& d:days){ size_t i=0;
            while(i<d.size()){
                if((d[i].ask-d[i].bid) > spmax){ i++; continue; }
                int dir=0;
                if(d[i].imb>=HI) dir=-1;        // bid-heavy -> price falls -> short
                else if(d[i].imb<=LO) dir=+1;   // ask-heavy -> price rises -> long
                if(dir==0){ i++; continue; }
                double entry = dir>0? d[i].ask : d[i].bid;
                int64_t t0=d[i].ts; size_t j=i;
                while(j<d.size() && d[j].ts < t0+HOLD_ms) j++;
                if(j>=d.size()) break;
                double exit = dir>0? d[j].bid : d[j].ask;
                double pnl = dir>0? (exit-entry):(entry-exit);
                n++; net+=pnl; sum+=pnl; sum2+=pnl*pnl; if(pnl>0){w++;gw+=pnl;}else gl+=-pnl;
                i=j;
            } }
        double pf=gl>0?gw/gl:(gw>0?99:0); double wr=n?100.0*w/n:0;
        double m=n?sum/n:0, v=n>1?(sum2-sum*sum/n)/(n-1):0; double sd=v>0?std::sqrt(v):0;
        double sh=sd>0?(m/sd)*std::sqrt((double)n):0;
        std::printf("  spmax<=%.2f HOLD=%llds : n=%-6d WR=%.1f%% PF=%.2f net=%+.1f avg=%+.4f tSh=%.2f\n",
            spmax,(long long)(HOLD_ms/1000),n,wr,pf,net,m,sh);
    };
    std::printf("\n=== TRADEABLE: REVERSION (bid-heavy->short), spread-gated ===\n");
    std::printf("(edge ~0.12/30s vs avg spread 0.52 -> only tradeable if spread is tight, e.g. futures)\n");
    for(double sp : {0.60,0.20,0.15,0.12}){
        for(int64_t hold : {30000LL,60000LL}) sim(0.70,0.30,hold,sp);
    }
    return 0;
}

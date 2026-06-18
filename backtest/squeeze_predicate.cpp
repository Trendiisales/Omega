// squeeze_predicate.cpp — does a SQUEEZE-FIRE precede a bigger / cleaner forward move
// than baseline? Tests the squeeze as a VOL-REGIME GATE (timing), separate from the
// dead standalone-directional engine. Uses the REAL lookahead-clean squeeze::Evaluator.
//
// fire event = squeeze_on(prev) && !squeeze_on(cur)  (compression released this bar).
// For each fire at bar i, measure forward over K bars:
//   vol-expansion : |close[i+K]-close[i]| / close[i]           (abs % move)
//   follow-through: dir * (close[i+K]-close[i]) / close[i]      (dir = momentum sign at fire)
// Baseline = same forward stats over ALL bars (every bar as a pseudo-event).
// Verdict: fire vol-expansion >> baseline => timing gate has value; follow-through >55%
// => directional gate too. Else the squeeze is dead for us at every layer.
//
// build: clang++ -O2 -std=c++17 -Iinclude -o /tmp/sqzpred backtest/squeeze_predicate.cpp
// run:   /tmp/sqzpred <nas_tick.csv> [tf_sec=3600] [K_bars=8]
#include "SqueezeSlingshotCore.hpp"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

using squeeze::Bar; using squeeze::Params; using squeeze::Evaluator; using squeeze::BarSignal;

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <tick.csv> [tf_sec] [K]\n", argv[0]); return 2; }
    const int tf = argc > 2 ? atoi(argv[2]) : 3600;
    const int K  = argc > 3 ? atoi(argv[3]) : 8;
    std::ifstream f(argv[1]);
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }

    // aggregate ticks -> OHLC bars on tf-sec buckets. format: "YYYYMMDD HHMMSSmmm,bid,ask[,vol]"
    std::vector<Bar> bars;
    int64_t cur_bucket = -1; Bar b{};
    auto ymd_to_epoch = [](long ymd, long hms_ms)->int64_t{
        int y=ymd/10000, mo=(ymd/100)%100, d=ymd%100;
        int hh=(hms_ms/10000000), mm=(hms_ms/100000)%100, ss=(hms_ms/1000)%100;
        static const int cum[12]={0,31,59,90,120,151,181,212,243,273,304,334};
        long days=(y-1970)*365L+(y-1969)/4 - (y-1901)/100 + (y-1601)/400;
        days+=cum[mo-1]+(d-1); if(mo>2 && (y%4==0&&(y%100!=0||y%400==0))) days+=1;
        return days*86400LL + hh*3600LL + mm*60LL + ss;
    };
    std::string line;
    while (std::getline(f, line)) {
        if (line.size() < 10) continue;
        char* p = line.data();
        long ymd = strtol(p, &p, 10); while (*p==' ') ++p;
        long hms = strtol(p, &p, 10); if (*p==',') ++p;
        double bid = strtod(p, &p); if (*p==',') ++p;
        double ask = strtod(p, &p);
        double mid = (bid>0&&ask>0)?(bid+ask)*0.5:(bid>0?bid:ask);
        if (mid<=0) continue;
        int64_t ts = ymd_to_epoch(ymd, hms);
        int64_t bk = ts/tf;
        if (bk != cur_bucket) {
            if (cur_bucket>=0) bars.push_back(b);
            cur_bucket = bk; b.open=b.high=b.low=b.close=mid; b.volume=0;
        } else {
            if (mid>b.high) b.high=mid; if (mid<b.low) b.low=mid; b.close=mid;
        }
    }
    if (cur_bucket>=0) bars.push_back(b);
    fprintf(stderr, "# %zu %ds bars\n", bars.size(), tf);
    if ((int)bars.size() < K+60) { fprintf(stderr,"too few bars\n"); return 1; }

    Params pp; // prod H1 defaults (bb20/kc20, ema8/21/34/55, mom20)
    Evaluator ev(pp);
    std::vector<BarSignal> sig(bars.size());
    for (size_t i=0;i<bars.size();++i) sig[i]=ev.update(bars[i]);

    // forward-move helper
    auto fwd = [&](size_t i)->double{ return (bars[i+K].close-bars[i].close)/bars[i].close; };

    // baseline over all bars with K ahead
    double base_abs=0; long base_n=0;
    for (size_t i=0;i+K<bars.size();++i){ base_abs+=std::fabs(fwd(i)); ++base_n; }
    base_abs/=std::max(1L,base_n);

    // fire events
    double fire_abs=0, fire_dir_sum=0; long fire_n=0, ft_pos=0;
    double fire_abs_t2=0; long fire_n_t2=0;
    for (size_t i=1;i+K<bars.size();++i){
        if (sig[i-1].squeeze_on && !sig[i].squeeze_on) {       // fired this bar
            double a=std::fabs(fwd(i)); fire_abs+=a; ++fire_n;
            int dir = sig[i].momentum>0?1:(sig[i].momentum<0?-1:0);
            double ft = dir*fwd(i); fire_dir_sum+=ft; if(ft>0)++ft_pos;
            if (sig[i-1].squeeze_tier>=2){ fire_abs_t2+=a; ++fire_n_t2; }
        }
    }
    auto pct=[](double x){return x*100.0;};
    printf("\n=== SQUEEZE-FIRE PREDICATE (tf=%ds, K=%d fwd bars) ===\n", tf, K);
    printf("  bars=%zu  fire-events=%ld  (%.1f%% of bars)\n", bars.size(), fire_n, 100.0*fire_n/bars.size());
    printf("  fwd |move|  : fire=%.3f%%   baseline=%.3f%%   ratio=%.2fx  %s\n",
           pct(fire_abs/std::max(1L,fire_n)), pct(base_abs),
           (fire_abs/std::max(1L,fire_n))/std::max(1e-9,base_abs),
           (fire_abs/std::max(1L,fire_n))>base_abs*1.15 ? "<- fires precede BIGGER moves" : "<- no vol-expansion edge");
    printf("  tier>=2 fire: fwd |move|=%.3f%%  n=%ld\n", pct(fire_abs_t2/std::max(1L,fire_n_t2)), fire_n_t2);
    printf("  follow-through (momentum-dir): mean=%.3f%%  win=%.1f%%  %s\n",
           pct(fire_dir_sum/std::max(1L,fire_n)), 100.0*ft_pos/std::max(1L,fire_n),
           (100.0*ft_pos/std::max(1L,fire_n))>55.0 ? "<- directional gate value" : "<- direction = coinflip (no dir edge)");
    printf("\n  READ: vol-ratio>1.15x = timing gate worth wiring; follow-through>55%% = directional gate.\n");
    return 0;
}

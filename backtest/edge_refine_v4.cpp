// edge_refine_v4.cpp
// Find momentum continuation/reversal edges in raw tick data.
// Tests: over the next N seconds after a M-point move, does price continue or reverse?
// No Python, no engine code, just raw tick math.
//
// Build: g++ -O3 -std=c++17 edge_refine_v4.cpp -o edge_refine_v4
// Run:   ./edge_refine_v4 ~/Tick/2yr_XAUUSD_tick.csv

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <vector>
#include <algorithm>

struct Tick { int64_t ts_ms; double bid, ask; };

static int64_t parse_ts(const char* d, const char* t) {
    // d = YYYYMMDD, t = HH:MM:SS
    auto gi = [](const char* s, int n){ int v=0; for(int i=0;i<n;i++) v=v*10+(s[i]-'0'); return v; };
    int y=gi(d,4), mo=gi(d+4,2), dy=gi(d+6,2);
    int h=gi(t,2), mi=gi(t+3,2), se=gi(t+6,2);
    if(mo<=2){y--;mo+=12;}
    int64_t days=365LL*y+y/4-y/100+y/400+(153*mo+8)/5+dy-719469LL;
    return (days*86400LL + h*3600LL + mi*60LL + se) * 1000LL;
}

int main(int argc, char** argv) {
    if(argc < 2) { printf("Usage: edge_refine_v4 <ticks.csv>\n"); return 1; }

    printf("Loading %s...\n", argv[1]);
    FILE* f = fopen(argv[1], "r");
    if(!f) { printf("Cannot open\n"); return 1; }

    std::vector<Tick> ticks;
    ticks.reserve(120000000);
    char line[256], d[32], t[32];
    double bid, ask, x3, x4;
    fgets(line, 256, f); // header
    while(fgets(line, 256, f)) {
        if(sscanf(line, "%[^,],%[^,],%lf,%lf", d, t, &bid, &ask) < 4) continue;
        if(bid <= 0 || ask <= bid) continue;
        int64_t ts = parse_ts(d, t);
        ticks.push_back({ts, bid, ask});
    }
    fclose(f);
    printf("Loaded %zu ticks\n\n", ticks.size());

    const size_t N = ticks.size();
    const double SPREAD = 0.25; // cost per side in pts

    // For each test: look back LOOKBACK_S seconds, measure move in pts.
    // If move > threshold, check what happens in next FORWARD_S seconds.
    // Count: continuation (same direction) vs reversal.
    // Also track: avg pts moved in fwd window (gross), EV after spread.

    struct Config {
        int lookback_s;  // how far back to measure the "signal" move
        int forward_s;   // how far forward to measure the outcome
        double move_min; // minimum price move to qualify (pts)
        const char* label;
    };

    Config configs[] = {
        // Short-term momentum
        {  30,  60,  2.0, "30s move>2pt -> 60s fwd"},
        {  30,  60,  4.0, "30s move>4pt -> 60s fwd"},
        {  30,  60,  8.0, "30s move>8pt -> 60s fwd"},
        {  60, 120,  3.0, "60s move>3pt -> 120s fwd"},
        {  60, 120,  6.0, "60s move>6pt -> 120s fwd"},
        {  60, 300,  5.0, "60s move>5pt -> 300s fwd"},
        // Medium-term
        { 300, 300,  5.0, "5m move>5pt -> 5m fwd"},
        { 300, 300, 10.0, "5m move>10pt -> 5m fwd"},
        { 300, 300, 20.0, "5m move>20pt -> 5m fwd"},
        { 900, 900, 10.0, "15m move>10pt -> 15m fwd"},
        { 900, 900, 20.0, "15m move>20pt -> 15m fwd"},
        // Reversal after extreme move
        {  60, 120,  8.0, "60s move>8pt -> 120s fwd (extreme)"},
        { 300, 300, 30.0, "5m move>30pt -> 5m fwd (extreme)"},
    };

    for(const auto& cfg : configs) {
        // Per-hour stats
        struct HourStat {
            int cont=0, rev=0, n=0;
            double sum_fwd=0, sum_fwd_cont=0, sum_fwd_rev=0;
        } hours[24];

        // Build fast forward index: for each tick i, find tick at ts+delta
        // Use two-pointer approach
        size_t j_back = 0, j_fwd = 0;

        for(size_t i = 0; i < N; i++) {
            const int64_t ts = ticks[i].ts_ms;
            const int64_t ts_back = ts - (int64_t)cfg.lookback_s * 1000;
            const int64_t ts_fwd  = ts + (int64_t)cfg.forward_s  * 1000;

            // Find tick at ts - lookback_s (advance j_back)
            while(j_back < i && ticks[j_back].ts_ms < ts_back) j_back++;
            // Find tick at ts + forward_s
            if(j_fwd < i) j_fwd = i;
            while(j_fwd < N-1 && ticks[j_fwd].ts_ms < ts_fwd) j_fwd++;

            // Need both to be within 20% of target time
            if(abs(ticks[j_back].ts_ms - ts_back) > (int64_t)cfg.lookback_s * 200) continue;
            if(abs(ticks[j_fwd].ts_ms  - ts_fwd)  > (int64_t)cfg.forward_s  * 200) continue;

            const double mid_back = (ticks[j_back].bid + ticks[j_back].ask) * 0.5;
            const double mid_now  = (ticks[i].bid + ticks[i].ask) * 0.5;
            const double mid_fwd  = (ticks[j_fwd].bid + ticks[j_fwd].ask) * 0.5;

            const double move = mid_now - mid_back;  // signed pts
            if(fabs(move) < cfg.move_min) continue;

            // Gap filter: skip if lookback or forward spans a gap >1hr
            if(ts - ticks[j_back].ts_ms > (int64_t)(cfg.lookback_s + 3600) * 1000) continue;
            if(ticks[j_fwd].ts_ms - ts  > (int64_t)(cfg.forward_s  + 3600) * 1000) continue;

            const double fwd_move = mid_fwd - mid_now; // what happens next
            const int dir = move > 0 ? 1 : -1;
            // continuation = fwd_move in same direction as move
            const double aligned = fwd_move * dir; // positive = continuation

            const int hour = (int)((ts / 1000) % 86400 / 3600);
            hours[hour].n++;
            hours[hour].sum_fwd += aligned;
            if(aligned > 0) { hours[hour].cont++; hours[hour].sum_fwd_cont += aligned; }
            else             { hours[hour].rev++;  hours[hour].sum_fwd_rev  += aligned; }
        }

        // Print results for this config
        // Aggregate
        int total_n=0, total_cont=0;
        double total_sum=0;
        for(int h=0;h<24;h++){
            total_n    += hours[h].n;
            total_cont += hours[h].cont;
            total_sum  += hours[h].sum_fwd;
        }
        if(total_n < 50) continue;

        const double wr  = (double)total_cont/total_n;
        const double avg = total_sum/total_n;
        const double ev  = avg - SPREAD;

        printf("%-38s  N=%7d  WR=%5.1f%%  avg=%+6.3f  EV=%+6.3f%s\n",
               cfg.label, total_n, wr*100, avg, ev,
               (fabs(ev) > SPREAD*0.5 && total_n > 200) ? " <--" : "");

        // Per-hour breakdown for interesting configs
        if(fabs(ev) > 0.10 && total_n > 200) {
            for(int h=0;h<24;h++){
                if(hours[h].n < 50) continue;
                const double hwr = (double)hours[h].cont/hours[h].n;
                const double havg = hours[h].sum_fwd/hours[h].n;
                const double hev  = havg - SPREAD;
                if(fabs(hev) > 0.15) {
                    printf("  h%02d: N=%5d WR=%5.1f%% avg=%+6.3f EV=%+6.3f%s\n",
                           h, hours[h].n, hwr*100, havg, hev,
                           hev > 0.20 ? " ***" : "");
                }
            }
        }
    }

    return 0;
}

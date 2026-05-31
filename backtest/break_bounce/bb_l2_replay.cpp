// bb_l2_replay.cpp -- replay the L2 profit-protect over CAPTURED live data.
//
// Reads outputs/breakbounce_l2_capture.csv (written by the shadow engine while
// each BreakBounce position is open) and, per trade (grouped by entry_ms),
// simulates the L2 protect with the REAL order-book imbalance stream:
//
//   arm once fav >= ARM_R * risk; track the favourable-side price peak; when
//   imbalance turns hostile AND price gives back >= GIVEBACK_ATR * atr from the
//   peak, snap a lock at peak -/+ LOCK_ATR * atr; the protect "exits" the first
//   later sample that trades through the lock.
//
// Compares, per trade, the protect exit fav vs the last-captured fav (the
// closest proxy for the un-protected outcome until the ledger join lands). This
// is the A/B we run on real data to decide whether to flip USE_L2_PROTECT on.
//
// Build: g++ -O2 -std=c++17 bb_l2_replay.cpp -o bbreplay
// Run:   ./bbreplay outputs/breakbounce_l2_capture.csv [arm_r giveback_atr lock_atr hostile_imb]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

struct Sample { int64_t now; double bid, ask, imb, fav, sl, risk, atr; int is_long; };

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: bbreplay <capture.csv> [arm_r giveback_atr lock_atr hostile_imb]\n"); return 1; }
    const double ARM_R    = argc>2 ? std::atof(argv[2]) : 1.0;
    const double GIVEBACK = argc>3 ? std::atof(argv[3]) : 0.40;
    const double LOCK     = argc>4 ? std::atof(argv[4]) : 0.50;
    const double HOSTILE  = argc>5 ? std::atof(argv[5]) : 0.35;

    std::ifstream in(argv[1]);
    if (!in.is_open()) { std::printf("cannot open %s\n", argv[1]); return 1; }

    std::map<int64_t, std::vector<Sample>> trades;   // entry_ms -> samples
    std::string line; std::getline(in, line);        // header (skipped if present)
    {
        // If the first line was data (no header), re-handle it.
        long long em, nm; double bid,ask,imb,fav,sl,risk,atr; int isl;
        if (std::sscanf(line.c_str(), "%lld,%lld,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%d",
                        &em,&nm,&bid,&ask,&imb,&fav,&sl,&risk,&atr,&isl) == 10)
            trades[em].push_back({nm,bid,ask,imb,fav,sl,risk,atr,isl});
    }
    while (std::getline(in, line)) {
        long long em, nm; double bid,ask,imb,fav,sl,risk,atr; int isl;
        if (std::sscanf(line.c_str(), "%lld,%lld,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%d",
                        &em,&nm,&bid,&ask,&imb,&fav,&sl,&risk,&atr,&isl) == 10)
            trades[em].push_back({nm,bid,ask,imb,fav,sl,risk,atr,isl});
    }
    if (trades.empty()) { std::printf("no captured samples in %s\n", argv[1]); return 0; }

    int n=0, triggered=0; double base_sum=0, prot_sum=0;
    for (auto& kv : trades) {
        auto& s = kv.second;
        if (s.size() < 2) continue;
        std::sort(s.begin(), s.end(), [](const Sample&a,const Sample&b){return a.now<b.now;});
        const bool is_long = s.front().is_long != 0;
        const double risk = s.front().risk, atr = s.front().atr;
        if (risk <= 0 || atr <= 0) continue;
        const double entry = is_long ? s.front().bid - s.front().fav
                                     : s.front().ask + s.front().fav;

        bool armed=false; double peak = is_long ? -1e18 : 1e18; double lock=0; bool locked=false;
        double prot_fav = s.back().fav;             // default: rode to last sample
        for (auto& x : s) {
            if (x.fav >= ARM_R * risk) armed = true;
            if (!armed) continue;
            const double px = is_long ? x.bid : x.ask;
            peak = is_long ? std::max(peak, px) : std::min(peak, px);
            const bool hostile = is_long ? (x.imb <= HOSTILE) : (x.imb >= 1.0 - HOSTILE);
            const double give = is_long ? (peak - px) : (px - peak);
            if (hostile && give >= GIVEBACK * atr) {
                const double L = is_long ? peak - LOCK*atr : peak + LOCK*atr;
                if (!locked) { lock = L; locked = true; }
                else lock = is_long ? std::max(lock, L) : std::min(lock, L);
            }
            if (locked) {
                const bool through = is_long ? (x.bid <= lock) : (x.ask >= lock);
                if (through) { prot_fav = is_long ? (lock - entry) : (entry - lock); triggered++; break; }
            }
        }
        base_sum += s.back().fav;
        prot_sum += prot_fav;
        n++;
    }

    std::printf("captured trades=%zu  usable=%d  protect triggered=%d\n", trades.size(), n, triggered);
    std::printf("baseline(last-sample) fav sum = %.2f\n", base_sum);
    std::printf("protect-replay        fav sum = %.2f\n", prot_sum);
    std::printf("delta (protect - baseline)     = %+.2f  (params arm=%.1fR give=%.2fatr lock=%.2fatr host=%.2f)\n",
        prot_sum - base_sum, ARM_R, GIVEBACK, LOCK, HOSTILE);
    std::printf("\nNote: baseline = last captured sample fav (proxy). For the exact\n"
                "A/B, join by entry_ms with the ledger's realized exit pnl.\n");
    return 0;
}

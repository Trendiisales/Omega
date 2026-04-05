#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <cmath>
#include <map>
#include <string>
#include <algorithm>
#include <numeric>

struct Tick { long long ts; double bid, ask; };

struct EventStats {
    int count = 0;
    double sum = 0, wins = 0;
    double sum_sq = 0;
    void add(double r) {
        count++; sum += r; sum_sq += r*r;
        if (r > 0) wins++;
    }
    double mean()    const { return count ? sum/count : 0; }
    double winrate() const { return count ? wins/count*100.0 : 0; }
    double stddev()  const {
        if (count < 2) return 0;
        double m = mean();
        return std::sqrt(sum_sq/count - m*m);
    }
    double sharpe()  const {
        double s = stddev();
        return s > 0 ? mean()/s*std::sqrt((double)count) : 0;
    }
};

static bool parse(const std::string& line, Tick& t) {
    const char* p = line.c_str();
    char* nx;
    // try ask-first (Dukascopy format)
    t.ts  = std::strtoll(p, &nx, 10); if (*nx!=',') return false; p=nx+1;
    t.ask = std::strtod(p, &nx);      if (*nx!=',') return false; p=nx+1;
    t.bid = std::strtod(p, &nx);
    if (t.ts <= 0 || t.bid <= 0 || t.ask <= t.bid) {
        // try bid-first
        p = line.c_str();
        t.ts  = std::strtoll(p, &nx, 10); if (*nx!=',') return false; p=nx+1;
        t.bid = std::strtod(p, &nx);      if (*nx!=',') return false; p=nx+1;
        t.ask = std::strtod(p, &nx);
        if (t.bid <= 0 || t.ask < t.bid) return false;
    }
    return true;
}

// Session from timestamp
static int session(long long ts_ms) {
    int h = (int)((ts_ms/1000/3600) % 24);
    if (h >= 21 || h < 1)  return 0; // dead
    if (h >= 1  && h < 7)  return 6; // asia
    if (h >= 7  && h < 9)  return 1; // london open
    if (h >= 9  && h < 13) return 2; // london core
    if (h >= 13 && h < 17) return 3; // overlap
    if (h >= 17 && h < 19) return 4; // ny
    return 5;                         // ny late
}

static const char* sess_name(int s) {
    switch(s) {
        case 0: return "DEAD";
        case 1: return "LONDON_OPEN";
        case 2: return "LONDON_CORE";
        case 3: return "OVERLAP";
        case 4: return "NY";
        case 5: return "NY_LATE";
        case 6: return "ASIA";
    }
    return "?";
}

int main(int argc, char** argv) {
    if (argc < 2) { std::cout << "usage: EdgeDiscovery ticks.csv\n"; return 0; }

    std::ifstream f(argv[1]);
    if (!f) { std::cout << "cannot open " << argv[1] << "\n"; return 1; }

    std::vector<double> price;
    std::vector<long long> ts;
    std::string line;
    Tick t;
    int skipped = 0;

    while (std::getline(f, line)) {
        if (line[0] < '0' || line[0] > '9') continue; // skip header
        if (!parse(line, t)) { skipped++; continue; }
        price.push_back((t.bid + t.ask) * 0.5);
        ts.push_back(t.ts);
    }
    std::cout << "Loaded " << price.size() << " ticks  skipped=" << skipped << "\n";

    const int N = (int)price.size();
    const int LOOK = 100; // lookback
    const int FWD  = 20;  // forward
    const int FWD2 = 50;

    // Per-session stats for all 9 combinations
    std::map<std::string, EventStats> stats;

    // Cost: gold spread ~0.22pt = 0.11pt one-way. Round trip = 0.22pt.
    // An edge is meaningful when mean > 0.22 (covers spread) + 0.13 (slip) = 0.35pt
    const double COST = 0.35;

    for (int i = LOOK; i < N - FWD2; i++) {
        double p = price[i];
        int sess = session(ts[i]);

        // === FEATURES ===

        // 1. ATR proxy: mean abs move over last 100 ticks
        double atr = 0;
        for (int k = i-LOOK; k < i; k++)
            atr += std::fabs(price[k+1] - price[k]);
        atr /= LOOK;
        if (atr < 0.001) continue; // skip dead tape

        // 2. Short momentum (last 5 ticks)
        double mom5  = price[i] - price[i-5];
        double mom20 = price[i] - price[i-20];
        double mom50 = price[i] - price[i-50];

        // 3. Volatility ratio (recent vs baseline)
        double vol5  = std::fabs(price[i] - price[i-5]);
        double vol20 = 0;
        for (int k=i-20; k<i; k++) vol20 += std::fabs(price[k+1]-price[k]);
        vol20 /= 20;
        double vol_ratio = (vol20 > 0.001) ? vol5 / vol20 : 1.0;

        // 4. Range compression (last 20 ticks)
        double lo = price[i], hi = price[i];
        for (int k = i-20; k < i; k++) {
            if (price[k] < lo) lo = price[k];
            if (price[k] > hi) hi = price[k];
        }
        double range20 = hi - lo;
        bool compressed = range20 < atr * 1.0;  // range < 1 ATR = compression

        // 5. Drift persistence: how many of last 20 ticks moved in same direction
        int up = 0, dn = 0;
        for (int k = i-20; k < i; k++) {
            if (price[k+1] > price[k]) up++;
            else if (price[k+1] < price[k]) dn++;
        }
        double persist = (double)std::max(up,dn) / 20.0;
        bool strong_persist = persist >= 0.70; // 14/20 ticks same direction

        // 6. Momentum direction
        bool bull = mom5 > atr * 0.3;
        bool bear = mom5 < -atr * 0.3;
        bool overext_up = mom50 > atr * 3.0;
        bool overext_dn = mom50 < -atr * 3.0;

        // Forward returns
        double fwd20 = price[i+20] - p;
        double fwd50 = price[i+50] - p;

        // === EVENT TESTS ===
        std::string sess_str = sess_name(sess);

        // A. Pure momentum by session
        if (bull) stats["BULL_" + sess_str].add(fwd20);
        if (bear) stats["BEAR_" + sess_str].add(-fwd20); // flip sign: bear means going down is a win

        // B. Compression + expansion (the breakout setup)
        if (compressed && vol_ratio > 1.5) {
            stats["COMPRESSION_BREAK"].add(std::fabs(fwd20)); // absolute move
            if (bull) stats["COMPRESSION_BREAK_BULL"].add(fwd20);
            if (bear) stats["COMPRESSION_BREAK_BEAR"].add(-fwd20);
        }

        // C. Strong drift persistence
        if (strong_persist) {
            if (up > dn) stats["PERSIST_BULL_" + sess_str].add(fwd20);
            else         stats["PERSIST_BEAR_" + sess_str].add(-fwd20);
        }

        // D. Overextension = fade candidate
        if (overext_up) stats["FADE_SHORT"].add(-fwd20); // fade up = short
        if (overext_dn) stats["FADE_LONG"].add(fwd20);   // fade down = long

        // E. Overextension by session
        if (overext_up) stats["FADE_SHORT_" + sess_str].add(-fwd20);
        if (overext_dn) stats["FADE_LONG_"  + sess_str].add(fwd20);

        // F. Vol regime
        if (vol_ratio > 2.0) {
            if (bull) stats["HIGH_VOL_BULL"].add(fwd20);
            if (bear) stats["HIGH_VOL_BEAR"].add(-fwd20);
        }

        // G. Combined: persist + compression break = GoldFlow archetype
        if (strong_persist && compressed && vol_ratio > 1.5) {
            if (up > dn) stats["GF_ARCHETYPE_BULL"].add(fwd20);
            else         stats["GF_ARCHETYPE_BEAR"].add(-fwd20);
        }
    }

    // === OUTPUT ===
    std::cout << "\n=== EDGE RESULTS (cost threshold = " << COST << "pt) ===\n\n";
    std::cout << "  Signal                          Count    Mean    WinRate  Sharpe  VsCost\n";
    std::cout << "  " << std::string(80,'-') << "\n";

    // Sort by mean descending
    std::vector<std::pair<std::string,EventStats>> sorted(stats.begin(), stats.end());
    std::sort(sorted.begin(), sorted.end(),
        [](auto& a, auto& b){ return a.second.mean() > b.second.mean(); });

    int profitable = 0;
    for (auto& kv : sorted) {
        auto& s = kv.second;
        if (s.count < 5000) continue; // need sufficient sample
        double vs = s.mean() - COST;
        char flag = vs > 0 ? '*' : ' ';
        if (vs > 0) profitable++;
        printf("  %-32s  %7d  %+6.3f  %5.1f%%  %6.2f  %+.3f %c\n",
               kv.first.c_str(), s.count,
               s.mean(), s.winrate(), s.sharpe(), vs, flag);
    }
    printf("\n  * = mean > cost (%.2f pt). Profitable signals: %d\n", COST, profitable);

    return 0;
}

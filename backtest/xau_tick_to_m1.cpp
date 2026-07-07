// xau_tick_to_m1.cpp — tick csv -> 1m bars (mid OHLC + median spread per bar).
// Handles both duka layouts: "timestamp,askPrice,bidPrice" (2024-26 combined)
// and "timestamp,bid,ask" (xau_2022bear_tick). ts in ms.
// Usage: xau_tick_to_m1 <in.csv> <out.csv>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s in.csv out.csv\n", argv[0]); return 1; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror("in"); return 1; }
    FILE* out = fopen(argv[2], "wb");
    if (!out) { perror("out"); return 1; }
    char line[256];
    if (!fgets(line, sizeof line, f)) return 1;
    bool ask_first = strstr(line, "ask") && (strstr(line, "ask") < strstr(line, "bid"));
    fprintf(out, "ts,o,h,l,c,spr\n");
    long long cur_min = -1;
    double o = 0, h = 0, l = 0, c = 0;
    std::vector<double> sprs; sprs.reserve(4096);
    long long rows = 0, bars = 0;
    auto flush = [&]() {
        if (cur_min < 0) return;
        double spr = 0;
        if (!sprs.empty()) {
            size_t m = sprs.size() / 2;
            std::nth_element(sprs.begin(), sprs.begin() + m, sprs.end());
            spr = sprs[m];
        }
        fprintf(out, "%lld,%.4f,%.4f,%.4f,%.4f,%.4f\n", cur_min * 60, o, h, l, c, spr);
        ++bars;
    };
    while (fgets(line, sizeof line, f)) {
        long long ts; double a, b;
        if (sscanf(line, "%lld,%lf,%lf", &ts, &a, &b) != 3) continue;
        double ask = ask_first ? a : b, bid = ask_first ? b : a;
        if (bid <= 0 || ask <= 0 || ask < bid) continue;
        double mid = 0.5 * (bid + ask);
        long long mn = (ts / 1000) / 60;
        if (mn != cur_min) { flush(); cur_min = mn; o = h = l = c = mid; sprs.clear(); }
        if (mid > h) h = mid;
        if (mid < l) l = mid;
        c = mid;
        sprs.push_back(ask - bid);
        ++rows;
    }
    flush();
    fclose(f); fclose(out);
    fprintf(stderr, "ticks=%lld bars=%lld\n", rows, bars);
    return 0;
}

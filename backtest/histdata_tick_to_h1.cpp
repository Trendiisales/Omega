// histdata_tick_to_h1.cpp — stream HISTDATA ASCII tick files ("YYYYMMDD HHMMSSmmm,bid,ask,x")
// into an H1 OHLC CSV (ts,o,h,l,c; ts = epoch sec of bar start, histdata local clock treated
// as fixed-offset — bars shift uniformly, detector math unaffected).
// Usage: histdata_tick_to_h1 out.h1.csv in1.csv [in2.csv ...]   (inputs must be chronological)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <ctime>

static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + (int64_t)doe - 719468LL;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s out.h1.csv in1.csv [in2.csv ...]\n", argv[0]); return 1; }
    FILE* out = std::fopen(argv[1], "w");
    if (!out) { std::perror(argv[1]); return 1; }
    int64_t bar_ts = -1; double o = 0, h = 0, l = 0, c = 0;
    long long ticks = 0, bars = 0;
    char line[256];
    for (int a = 2; a < argc; ++a) {
        FILE* in = std::fopen(argv[a], "r");
        if (!in) { std::perror(argv[a]); continue; }
        while (std::fgets(line, sizeof line, in)) {
            // YYYYMMDD HHMMSSmmm,bid,ask,...
            if (std::strlen(line) < 22 || line[8] != ' ') continue;
            int Y = (line[0]-'0')*1000 + (line[1]-'0')*100 + (line[2]-'0')*10 + (line[3]-'0');
            int M = (line[4]-'0')*10 + (line[5]-'0');
            int D = (line[6]-'0')*10 + (line[7]-'0');
            int hh = (line[9]-'0')*10 + (line[10]-'0');
            if (Y < 2000 || M < 1 || M > 12 || D < 1 || D > 31 || hh < 0 || hh > 23) continue;
            const char* p = std::strchr(line, ',');
            if (!p) continue;
            char* end = nullptr;
            const double bid = std::strtod(p + 1, &end);
            if (!end || *end != ',') continue;
            const double ask = std::strtod(end + 1, &end);
            if (bid <= 0 || ask <= 0) continue;
            const double mid = (bid + ask) * 0.5;
            const int64_t ts = days_from_civil(Y, M, D) * 86400LL + hh * 3600LL;
            if (ts != bar_ts) {
                if (bar_ts >= 0) { std::fprintf(out, "%lld,%.3f,%.3f,%.3f,%.3f\n", (long long)bar_ts, o, h, l, c); ++bars; }
                bar_ts = ts; o = h = l = c = mid;
            } else {
                if (mid > h) h = mid;
                if (mid < l) l = mid;
                c = mid;
            }
            ++ticks;
        }
        std::fclose(in);
        std::fprintf(stderr, "[h1] %s done (%lld ticks so far)\n", argv[a], ticks);
    }
    if (bar_ts >= 0) { std::fprintf(out, "%lld,%.3f,%.3f,%.3f,%.3f\n", (long long)bar_ts, o, h, l, c); ++bars; }
    std::fclose(out);
    std::fprintf(stderr, "[h1] wrote %lld bars from %lld ticks\n", bars, ticks);
    return 0;
}

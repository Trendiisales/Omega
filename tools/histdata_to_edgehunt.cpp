// =============================================================================
//  histdata_to_edgehunt.cpp -- HISTDATA T-format CSV → edge_hunt format.
//                              (S35-P6 2026-05-12)
// =============================================================================
//
//  PURPOSE
//
//      Convert HISTDATA.com T-format ("tick") CSVs into edge_hunt's
//      "ts_ms,bid,ask" format, splitting output by period (2025H1 /
//      2025H2 / 2026 partial). Used by S35-Part-3 to prepare 16 months
//      of NSXUSD + 17 months of SPXUSD tick data for the 3-period
//      intersection test that selected cells for UstecTrendFollowHtfEngine.
//
//  HISTDATA T-FORMAT
//
//      YYYYMMDD HHMMSSmmm,bid,ask,vol
//
//      Example: 20250101 180000700,21070.242000,21073.681000,0
//
//      Timestamps are in EST/EDT (US Eastern). Volume is always 0 in
//      HISTDATA (tick volume is not exposed). No header row.
//
//  EDGE_HUNT FORMAT
//
//      ts_ms,bid,ask        (header included)
//
//      ts_ms is UTC milliseconds since epoch.
//
//  DST HANDLING
//
//      Hardcoded transitions for 2024-2026:
//          2024-03-10 02:00 ET → DST on  (EDT = UTC-4)  [S36-P2 2026-05-12]
//          2024-11-03 02:00 ET → DST off (EST = UTC-5)  [S36-P2 2026-05-12]
//          2025-03-09 02:00 ET → DST on  (EDT = UTC-4)
//          2025-11-02 02:00 ET → DST off (EST = UTC-5)
//          2026-03-08 02:00 ET → DST on
//          2026-11-01 02:00 ET → DST off (out of our data range)
//
//      The half-hour around each transition is ambiguous in any local-time
//      stream; HISTDATA tags wall-clock so the adjustment is exact in
//      retrospect (we evaluate which side of the transition each tick is on
//      using the EST-shifted UTC view).
//
//  USAGE
//
//      g++ -std=c++17 -O2 -Wall histdata_to_edgehunt.cpp -o /tmp/h2e
//
//      /tmp/h2e INPUT.csv OUT_DIR PREFIX
//
//          INPUT.csv = HISTDATA T-format CSV, or '-' for stdin
//          OUT_DIR   = directory to write OUT_DIR/PREFIX_PERIOD.csv
//          PREFIX    = filename prefix (e.g. nsx, spx)
//
//      Output files: OUT_DIR/{PREFIX}_2024H1.csv  [S36-P2 2026-05-12]
//                    OUT_DIR/{PREFIX}_2024H2.csv  [S36-P2 2026-05-12]
//                    OUT_DIR/{PREFIX}_2025H1.csv
//                    OUT_DIR/{PREFIX}_2025H2.csv
//                    OUT_DIR/{PREFIX}_2026.csv
//
//      Files are opened in append-binary mode so multiple invocations
//      accumulate across all monthly inputs into the right period bucket.
//      Caller deletes pre-existing files between full re-runs.
//
//  PERFORMANCE
//
//      ~5 million ticks per second on a single core (tested on the
//      sandbox VM, 89.6M NSXUSD ticks converted in 18 seconds; 25.7M
//      SPXUSD ticks in 4 seconds). Manual line parsing (no scanf) for
//      the date/time field; fprintf for output.
// =============================================================================

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

inline int64_t naive_utc_ms(int yr, int mo, int dy, int hh, int mm, int ss, int ms) {
    struct tm t{};
    t.tm_year = yr - 1900;
    t.tm_mon  = mo - 1;
    t.tm_mday = dy;
    t.tm_hour = hh;
    t.tm_min  = mm;
    t.tm_sec  = ss;
    // timegm interprets struct tm as UTC; available on glibc + macOS.
    time_t epoch = timegm(&t);
    return (int64_t)epoch * 1000 + ms;
}

// DST transitions for US/Eastern, valid for our data range (2024-2026).
// Each span is the UTC-time interval during which DST is in effect.
//   spring forward 2025: 2025-03-09 02:00 EST → 03:00 EDT  ⇒ UTC = 2025-03-09 07:00 UTC
//   fall back     2025: 2025-11-02 02:00 EDT → 01:00 EST  ⇒ UTC = 2025-11-02 06:00 UTC
//   spring forward 2026: 2026-03-08 02:00 EST → 03:00 EDT  ⇒ UTC = 2026-03-08 07:00 UTC
struct DstSpan { int64_t start_utc_ms; int64_t end_utc_ms; };
static const DstSpan DST_SPANS[] = {
    {1710054000000LL, 1730613600000LL},   // 2024-03-10 07:00 UTC → 2024-11-03 06:00 UTC  [S36-P2 2026-05-12]
    {1741417200000LL, 1762063200000LL},   // 2025-03-09 07:00 UTC → 2025-11-02 06:00 UTC
    {1773471600000LL, 9999999999999LL},   // 2026-03-08 07:00 UTC → end of data
};

// Given a NAIVE EST/EDT clock, return the UTC ms accounting for DST.
inline int64_t est_to_utc_ms(int yr, int mo, int dy, int hh, int mm, int ss, int ms) {
    int64_t naive = naive_utc_ms(yr, mo, dy, hh, mm, ss, ms);
    // During EDT (UTC-4), real_utc = naive + 4h.
    // During EST (UTC-5), real_utc = naive + 5h.
    // Determine DST by whether (naive + 5h) falls inside any DST span.
    int64_t prov_utc_est = naive + 5LL * 3600LL * 1000LL;  // assume EST first
    bool is_dst = false;
    for (const auto& s : DST_SPANS) {
        if (prov_utc_est >= s.start_utc_ms && prov_utc_est < s.end_utc_ms) {
            is_dst = true;
            break;
        }
    }
    return naive + (is_dst ? 4LL : 5LL) * 3600LL * 1000LL;
}

// Choose period bucket for a given ms.
//   Returns "2025H1" / "2025H2" / "2026" / "OTHER".
const char* bucket_for_ms(int64_t ms) {
    time_t t = (time_t)(ms / 1000);
    struct tm utc{};
#if defined(__APPLE__) || defined(__linux__)
    gmtime_r(&t, &utc);
#else
    gmtime_s(&utc, &t);
#endif
    int year = utc.tm_year + 1900;
    int mon  = utc.tm_mon + 1;
    if (year == 2024 && mon >= 1 && mon <= 6)  return "2024H1";   // [S36-P2 2026-05-12]
    if (year == 2024 && mon >= 7 && mon <= 12) return "2024H2";   // [S36-P2 2026-05-12]
    if (year == 2025 && mon >= 1 && mon <= 6)  return "2025H1";
    if (year == 2025 && mon >= 7 && mon <= 12) return "2025H2";
    if (year == 2026)                          return "2026";
    return "OTHER";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr,
            "usage: %s INPUT.csv OUT_DIR PREFIX\n"
            "  INPUT.csv = HISTDATA T-format CSV, or '-' for stdin\n"
            "  OUT_DIR   = directory to write OUT_DIR/PREFIX_PERIOD.csv\n"
            "  PREFIX    = filename prefix (e.g. nsx)\n", argv[0]);
        return 2;
    }
    const char* inpath  = argv[1];
    const std::string outdir = argv[2];
    const std::string prefix = argv[3];

    FILE* in = (std::strcmp(inpath, "-") == 0) ? stdin : std::fopen(inpath, "rb");
    if (!in) { std::fprintf(stderr, "cannot open %s\n", inpath); return 1; }

    // Open one output file per bucket, append-mode (caller deletes pre-existing).
    auto open_out = [&](const std::string& bucket) -> FILE* {
        std::string path = outdir + "/" + prefix + "_" + bucket + ".csv";
        FILE* f = std::fopen(path.c_str(), "ab");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); std::exit(1); }
        // Write header if file is empty.
        std::fseek(f, 0, SEEK_END);
        if (std::ftell(f) == 0) std::fputs("ts_ms,bid,ask\n", f);
        return f;
    };
    FILE* f24H1 = open_out("2024H1");   // [S36-P2 2026-05-12]
    FILE* f24H2 = open_out("2024H2");   // [S36-P2 2026-05-12]
    FILE* fH1 = open_out("2025H1");
    FILE* fH2 = open_out("2025H2");
    FILE* f26 = open_out("2026");

    char line[256];
    int64_t n_total = 0, n_24h1 = 0, n_24h2 = 0, n_h1 = 0, n_h2 = 0, n_2026 = 0, n_other = 0;
    while (std::fgets(line, sizeof(line), in)) {
        // Parse "YYYYMMDD HHMMSSmmm,bid,ask,vol"  -- ultra-fast manual parse.
        if (line[0] < '0' || line[0] > '9') continue;
        int yr = (line[0]-'0')*1000 + (line[1]-'0')*100 + (line[2]-'0')*10 + (line[3]-'0');
        int mo = (line[4]-'0')*10 + (line[5]-'0');
        int dy = (line[6]-'0')*10 + (line[7]-'0');
        // line[8] == ' '
        int hh = (line[9]-'0')*10  + (line[10]-'0');
        int mm = (line[11]-'0')*10 + (line[12]-'0');
        int ss = (line[13]-'0')*10 + (line[14]-'0');
        int ms = (line[15]-'0')*100 + (line[16]-'0')*10 + (line[17]-'0');
        // line[18] == ','
        const char* p = line + 19;
        const char* bid_start = p;
        while (*p && *p != ',') ++p;
        if (*p != ',') continue;
        const char* ask_start = p + 1;
        const char* ask_end = ask_start;
        while (*ask_end && *ask_end != ',' && *ask_end != '\n' && *ask_end != '\r') ++ask_end;

        int64_t ts_ms = est_to_utc_ms(yr, mo, dy, hh, mm, ss, ms);
        const char* bucket = bucket_for_ms(ts_ms);
        // bucket positions:
        //   "2024H1" -> [0]='2' [1]='0' [2]='2' [3]='4' [4]='H' [5]='1'   [S36-P2 2026-05-12]
        //   "2024H2" -> [0]='2' [1]='0' [2]='2' [3]='4' [4]='H' [5]='2'   [S36-P2 2026-05-12]
        //   "2025H1" -> [0]='2' [1]='0' [2]='2' [3]='5' [4]='H' [5]='1'
        //   "2025H2" -> [0]='2' [1]='0' [2]='2' [3]='5' [4]='H' [5]='2'
        //   "2026"   -> [0]='2' [1]='0' [2]='2' [3]='6'
        FILE* out = nullptr;
        if      (bucket[3]=='4' && bucket[4]=='H' && bucket[5]=='1') { out = f24H1; ++n_24h1; }
        else if (bucket[3]=='4' && bucket[4]=='H' && bucket[5]=='2') { out = f24H2; ++n_24h2; }
        else if (bucket[3]=='5' && bucket[4]=='H' && bucket[5]=='1') { out = fH1; ++n_h1; }
        else if (bucket[3]=='5' && bucket[4]=='H' && bucket[5]=='2') { out = fH2; ++n_h2; }
        else if (bucket[3]=='6')                                      { out = f26; ++n_2026; }
        else { ++n_other; }
        if (out) {
            std::fprintf(out, "%lld,%.*s,%.*s\n",
                (long long)ts_ms,
                (int)(p - bid_start), bid_start,
                (int)(ask_end - ask_start), ask_start);
        }
        ++n_total;
    }
    std::fclose(f24H1); std::fclose(f24H2);
    std::fclose(fH1); std::fclose(fH2); std::fclose(f26);
    if (in != stdin) std::fclose(in);
    std::fprintf(stderr,
        "[CONV] %s: total=%lld  2024H1=%lld  2024H2=%lld  2025H1=%lld  2025H2=%lld  2026=%lld  other=%lld\n",
        inpath, (long long)n_total, (long long)n_24h1, (long long)n_24h2,
        (long long)n_h1, (long long)n_h2,
        (long long)n_2026, (long long)n_other);
    return 0;
}

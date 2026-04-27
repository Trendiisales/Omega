// =============================================================================
// EventExtractor.cpp -- one-pass tick-stream → bar-close panel extractor.
//
// Run:
//   ./OmegaEdgeFinderExtract --in <ticks.csv> --out <panel.bin>
//                            [--from YYYY-MM-DD] [--to YYYY-MM-DD]
//                            [--verbose]
//
// Streams the CSV (DUKA / TS_BA / TS_OHLCV formats — same sniff as
// OmegaSweepHarness) once. Maintains BarState (per-bar accumulator + trailing
// rolling state) plus a ForwardTracker (pending rows awaiting forward-return
// fills). Emits one PanelRow per closed bar, fully populated.
//
// Output: flat binary panel file (PanelWriter format). See PanelSchema.hpp.
// =============================================================================

#include "PanelSchema.hpp"
#include "BarState.hpp"
#include "ForwardTracker.hpp"
#include "PanelWriter.hpp"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <chrono>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

using namespace edgefinder;

// -----------------------------------------------------------------------------
// Memory-mapped file reader -- same approach as OmegaSweepHarness, copied to
// keep this target self-contained.
// -----------------------------------------------------------------------------
struct MemMappedFile {
    const char* data = nullptr;
    size_t      size = 0;
#if defined(_WIN32)
    HANDLE fh = INVALID_HANDLE_VALUE;
    HANDLE mh = nullptr;
#else
    int    fd = -1;
#endif

    bool open(const char* path) {
#if defined(_WIN32)
        fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fh == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz; if (!GetFileSizeEx(fh, &sz)) return false;
        size = static_cast<size_t>(sz.QuadPart);
        mh = CreateFileMappingA(fh, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mh) return false;
        data = static_cast<const char*>(MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0));
        return data != nullptr;
#else
        fd = ::open(path, O_RDONLY);
        if (fd < 0) return false;
        struct stat st; if (fstat(fd, &st) != 0) return false;
        size = static_cast<size_t>(st.st_size);
        data = static_cast<const char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
        return data != MAP_FAILED;
#endif
    }
    void close() {
#if defined(_WIN32)
        if (data)            UnmapViewOfFile(data);
        if (mh)              CloseHandle(mh);
        if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
#else
        if (data && data != MAP_FAILED) munmap((void*)data, size);
        if (fd >= 0) ::close(fd);
#endif
        data = nullptr; size = 0;
    }
};

// -----------------------------------------------------------------------------
// CSV parsing helpers — same fast_f / fast_i64 as OmegaSweepHarness.
// -----------------------------------------------------------------------------
static inline int64_t fast_i64(const char* p, const char** out_p) {
    int64_t v = 0; bool neg = false;
    if (*p == '-') { neg = true; ++p; }
    while (*p >= '0' && *p <= '9') { v = v*10 + (*p - '0'); ++p; }
    *out_p = p;
    return neg ? -v : v;
}
static inline double fast_f(const char* p, const char** out_p) {
    double sign = 1.0;
    if (*p == '-') { sign = -1.0; ++p; }
    double whole = 0.0;
    while (*p >= '0' && *p <= '9') { whole = whole*10.0 + (*p - '0'); ++p; }
    double frac = 0.0, scale = 1.0;
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            frac = frac*10.0 + (*p - '0');
            scale *= 10.0;
            ++p;
        }
    }
    *out_p = p;
    return sign * (whole + frac / scale);
}

enum class Fmt { TS_BA, TS_OHLCV, DUKA };

static Fmt sniff_format(const char* p, const char* end) {
    const char* q = p; int commas = 0;
    while (q < end && *q != '\n') { if (*q == ',') ++commas; ++q; }
    bool has_dot_in_first = false;
    const char* r = p;
    while (r < end && *r != ',' && *r != '\n') {
        if (*r == '.') { has_dot_in_first = true; break; }
        ++r;
    }
    if (has_dot_in_first && commas >= 4) return Fmt::DUKA;
    if (commas >= 5) return Fmt::TS_OHLCV;
    return Fmt::TS_BA;
}

// DUKA timestamp: YYYY.MM.DD,HH:MM:SS.mmm
static int64_t duka_ts(const char* dp, const char* tp) {
    int Y = (dp[0]-'0')*1000 + (dp[1]-'0')*100 + (dp[2]-'0')*10 + (dp[3]-'0');
    int M = (dp[5]-'0')*10 + (dp[6]-'0');
    int D = (dp[8]-'0')*10 + (dp[9]-'0');
    int h = (tp[0]-'0')*10 + (tp[1]-'0');
    int m = (tp[3]-'0')*10 + (tp[4]-'0');
    int s = (tp[6]-'0')*10 + (tp[7]-'0');
    int ms = 0;
    if (tp[8]=='.') ms = (tp[9]-'0')*100 + (tp[10]-'0')*10 + (tp[11]-'0');
    struct tm ti{}; ti.tm_year=Y-1900; ti.tm_mon=M-1; ti.tm_mday=D;
    ti.tm_hour=h; ti.tm_min=m; ti.tm_sec=s;
#ifdef _WIN32
    int64_t epoch = static_cast<int64_t>(_mkgmtime(&ti));
#else
    int64_t epoch = static_cast<int64_t>(timegm(&ti));
#endif
    return epoch * 1000LL + ms;
}

static int64_t parse_date_arg(const char* s) {
    if (!s || std::strlen(s) < 10) return 0;
    int Y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    int M = (s[5]-'0')*10 + (s[6]-'0');
    int D = (s[8]-'0')*10 + (s[9]-'0');
    struct tm ti{}; ti.tm_year=Y-1900; ti.tm_mon=M-1; ti.tm_mday=D;
#ifdef _WIN32
    int64_t epoch = static_cast<int64_t>(_mkgmtime(&ti));
#else
    int64_t epoch = static_cast<int64_t>(timegm(&ti));
#endif
    return epoch * 1000LL;
}

// -----------------------------------------------------------------------------
// Main loop
// -----------------------------------------------------------------------------
struct Args {
    const char* in_path  = nullptr;
    const char* out_path = nullptr;
    int64_t     from_ms  = 0;
    int64_t     to_ms    = 0;
    bool        verbose  = false;
};

static void usage(const char* argv0) {
    std::fprintf(stderr,
        "OmegaEdgeFinder Extract\n"
        "Usage: %s --in <ticks.csv> --out <panel.bin>\n"
        "          [--from YYYY-MM-DD] [--to YYYY-MM-DD] [--verbose]\n",
        argv0);
}

static bool parse_args(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const char* k = argv[i];
        auto next = [&]() -> const char* { return (i+1 < argc) ? argv[++i] : nullptr; };
        if (!std::strcmp(k, "--in"))           a.in_path  = next();
        else if (!std::strcmp(k, "--out"))     a.out_path = next();
        else if (!std::strcmp(k, "--from"))    a.from_ms  = parse_date_arg(next());
        else if (!std::strcmp(k, "--to"))      a.to_ms    = parse_date_arg(next());
        else if (!std::strcmp(k, "--verbose")) a.verbose  = true;
        else if (!std::strcmp(k, "-h") || !std::strcmp(k, "--help")) return false;
        else { std::fprintf(stderr, "unknown arg: %s\n", k); return false; }
    }
    return a.in_path && a.out_path;
}

// Emit callback for ForwardTracker -> PanelWriter
struct EmitCtx {
    PanelWriter* writer;
    int64_t      rows_written = 0;
};

static void emit_row(const PanelRow& r, void* ud) {
    auto* ctx = static_cast<EmitCtx*>(ud);
    ctx->writer->write_row(r);
    ++ctx->rows_written;
}

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) { usage(argv[0]); return 2; }

    if (args.verbose) {
        std::printf("OmegaEdgeFinder Extract\n");
        std::printf("  in:  %s\n", args.in_path);
        std::printf("  out: %s\n", args.out_path);
        if (args.from_ms) std::printf("  from_ms: %lld\n", static_cast<long long>(args.from_ms));
        if (args.to_ms)   std::printf("  to_ms:   %lld\n", static_cast<long long>(args.to_ms));
        std::printf("  schema_version: %d, row_size: %zu\n",
                    PANEL_SCHEMA_VERSION, sizeof(PanelRow));
    }

    MemMappedFile mm;
    if (!mm.open(args.in_path)) {
        std::fprintf(stderr, "ERROR: failed to open input %s\n", args.in_path);
        return 1;
    }

    PanelWriter writer;
    if (!writer.open(args.out_path)) {
        std::fprintf(stderr, "ERROR: failed to open output %s\n", args.out_path);
        mm.close();
        return 1;
    }

    EmitCtx ctx{&writer, 0};
    BarState        bar;
    ForwardTracker  fwd(emit_row, &ctx);

    const char* p   = mm.data;
    const char* end = mm.data + mm.size;

    // Skip header line if first char isn't a digit.
    bool ts_ba_ask_first = false;
    if (p < end && (*p < '0' || *p > '9')) {
        const char* hdr_start = p;
        while (p < end && *p != '\n') ++p;
        const char* hdr_end = p;
        if (p < end) ++p;
        // Detect "ts,ask,bid" vs "ts,bid,ask".
        auto find_token = [hdr_start, hdr_end](const char* tok, size_t tl) -> const char* {
            for (const char* q = hdr_start; q + tl <= hdr_end; ++q) {
                bool eq = true;
                for (size_t i = 0; i < tl; ++i) {
                    char c = q[i];
                    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                    if (c != tok[i]) { eq = false; break; }
                }
                if (eq) return q;
            }
            return nullptr;
        };
        const char* ask_pos = find_token("ask", 3);
        const char* bid_pos = find_token("bid", 3);
        if (ask_pos && bid_pos && ask_pos < bid_pos) ts_ba_ask_first = true;
    }

    Fmt fmt = sniff_format(p, end);
    if (args.verbose) {
        const char* fname = (fmt==Fmt::DUKA?"DUKA":fmt==Fmt::TS_OHLCV?"TS_OHLCV":"TS_BA");
        std::printf("  format: %s%s\n", fname,
                    (fmt==Fmt::TS_BA && ts_ba_ask_first) ? " (ask-first)" : "");
        std::fflush(stdout);
    }

    int64_t ticks_in       = 0;
    int64_t ticks_dropped  = 0;
    int64_t skipped_before = 0;
    int64_t skipped_after  = 0;

    auto t0 = std::chrono::steady_clock::now();

    while (p < end) {
        int64_t ts_ms; double bid, ask;
        const char* nx;

        if (fmt == Fmt::DUKA) {
            // YYYY.MM.DD,HH:MM:SS.mmm,bid,ask,vol
            const char* dp = p;
            while (p < end && *p != ',') ++p;
            if (p >= end) break;
            const char* tp = p + 1;       // start of time field
            ++p;                          // step past the comma after the date
            while (p < end && *p != ',') ++p;   // scan time field to its trailing comma
            if (p >= end) break;
            ts_ms = duka_ts(dp, tp);
            ++p;                          // step past the comma after the time
            bid = fast_f(p, &nx); p = nx;
            if (p >= end || *p != ',') break;
            ++p;
            ask = fast_f(p, &nx); p = nx;
        } else if (fmt == Fmt::TS_OHLCV) {
            ts_ms = fast_i64(p, &nx); p = nx;
            if (p >= end || *p != ',') break; ++p;
            double o = fast_f(p, &nx); p = nx; if (p >= end || *p != ',') break; ++p;
            double h = fast_f(p, &nx); p = nx; if (p >= end || *p != ',') break; ++p;
            double l = fast_f(p, &nx); p = nx; if (p >= end || *p != ',') break; ++p;
            double c = fast_f(p, &nx); p = nx;
            (void)o; (void)c;
            const double mid = 0.5 * (h + l);
            bid = mid - 0.05; ask = mid + 0.05;
        } else {
            ts_ms = fast_i64(p, &nx); p = nx;
            if (p >= end || *p != ',') break; ++p;
            const double v1 = fast_f(p, &nx); p = nx;
            if (p >= end || *p != ',') break; ++p;
            const double v2 = fast_f(p, &nx); p = nx;
            if (ts_ba_ask_first) { ask = v1; bid = v2; }
            else                 { bid = v1; ask = v2; }
        }

        // skip rest of line
        while (p < end && *p != '\n') ++p;
        if (p < end) ++p;

        if (ts_ms <= 0 || bid <= 0 || ask <= 0 || bid >= ask) { ++ticks_dropped; continue; }
        if (args.from_ms > 0 && ts_ms <  args.from_ms) { ++skipped_before; continue; }
        if (args.to_ms   > 0 && ts_ms >= args.to_ms)   { ++skipped_after;  continue; }

        ++ticks_in;

        // Drive bar accumulator. If a bar closed, register it with ForwardTracker.
        PanelRow closed_row;
        BarState::CloseResult cr = bar.on_tick(bid, ask, ts_ms, closed_row);
        if (cr.emitted) {
            fwd.on_bar_close(closed_row, cr.close_mid);
        }
        // Always: feed THIS tick to the forward tracker so any pending rows
        // get their fwd-return / bracket cells updated.
        fwd.on_tick(bid, ask, ts_ms);

        if (args.verbose && (ticks_in % 10'000'000LL) == 0) {
            const auto t = std::chrono::steady_clock::now();
            const double sec = std::chrono::duration<double>(t - t0).count();
            std::printf("  ticks=%lld  bars=%lld  pending=%zu  emitted=%lld  rate=%.0fk/s\n",
                        static_cast<long long>(ticks_in),
                        static_cast<long long>(bar.bars_emitted()),
                        fwd.pending_count(),
                        static_cast<long long>(ctx.rows_written),
                        ticks_in / sec / 1000.0);
            std::fflush(stdout);
        }
    }

    // Close in-progress bar.
    {
        PanelRow last_row;
        const bool had_partial = bar.finalise(last_row);
        // If a partial bar was sealed, register it with the forward tracker so
        // it gets flushed (with NaN forward returns) by flush_remaining below.
        if (had_partial) {
            fwd.on_bar_close(last_row, last_row.close);
        }
    }
    fwd.flush_remaining();
    writer.finish();
    mm.close();

    const auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();

    std::printf("=== OmegaEdgeFinder Extract done ===\n");
    std::printf("  elapsed:           %.1f s\n", sec);
    std::printf("  ticks read:        %lld\n", static_cast<long long>(ticks_in));
    std::printf("  ticks dropped:     %lld (bad)  %lld (before)  %lld (after)\n",
                static_cast<long long>(ticks_dropped),
                static_cast<long long>(skipped_before),
                static_cast<long long>(skipped_after));
    std::printf("  bars closed:       %lld\n", static_cast<long long>(bar.bars_emitted()));
    std::printf("  rows written:      %lld\n", static_cast<long long>(writer.rows_written()));
    std::printf("  panel size:        %.1f MB\n",
                writer.rows_written() * sizeof(PanelRow) / 1024.0 / 1024.0);
    std::fflush(stdout);
    return 0;
}

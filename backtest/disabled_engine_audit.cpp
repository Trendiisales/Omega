// =============================================================================
// disabled_engine_audit.cpp -- multi-engine CRTP audit harness.
//
// Built 2026-05-28 (S37-Z / task #9) -- single-pass replay of the 2yr XAUUSD
// tick corpus through N production engines concurrently. Each engine wraps
// in a CRTP Driver providing a uniform feed_tick interface. Drivers stored
// in std::tuple at compile time; dispatch is a fold expression so the
// compiler unrolls the loop.
//
// Usage:
//   cmake --build build --target disabled_engine_audit -j
//   ./build/disabled_engine_audit /Users/jo/Tick/2yr_XAUUSD_tick_fresh.csv [report.txt]
// =============================================================================

#include "OmegaTimeShim.hpp"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <tuple>
#include <chrono>

#include "disabled_engine_audit_drivers.hpp"

namespace dea = omega::dea;

// ---------------------------------------------------------------------------
// Tick parser -- DukasCopy YYYYMMDD,HH:MM:SS,bid,ask
// ---------------------------------------------------------------------------
struct Tick { int64_t ts_ms; double bid; double ask; };

static bool parse_duka_row(const char* line, Tick& out) {
    int Y, M, D, h, m, s;
    double bid, ask;
    if (std::sscanf(line, "%4d%2d%2d,%d:%d:%d,%lf,%lf",
                    &Y, &M, &D, &h, &m, &s, &bid, &ask) != 8) return false;
    if (bid <= 0.0 || ask <= 0.0 || ask < bid) return false;
    std::tm tm{};
    tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D;
    tm.tm_hour = h;        tm.tm_min  = m;    tm.tm_sec  = s;
    const time_t epoch_s = timegm(&tm);
    if (epoch_s <= 0) return false;
    out.ts_ms = static_cast<int64_t>(epoch_s) * 1000LL;
    out.bid = bid; out.ask = ask;
    return true;
}

// ---------------------------------------------------------------------------
// Silence engine std::cout so 154M ticks * N engines don't generate 100GB log.
// Identical pattern to bracket_gold_sweep CoutSilencer.
// ---------------------------------------------------------------------------
struct CoutSilencer {
    std::streambuf* prev;
    CoutSilencer() : prev(std::cout.rdbuf(nullptr)) {}
    ~CoutSilencer() { std::cout.rdbuf(prev); }
};

// ---------------------------------------------------------------------------
// Report helpers
// ---------------------------------------------------------------------------
static void print_block(FILE* rpt, const char* label, const dea::Stats& s) {
    std::fprintf(rpt, "  %-15s n=%-6d  WR=%6.2f%%  PF=%6.3f  gross=%+10.3f  MaxDD=%9.3f\n",
                 label, s.n_trades, s.wr(), s.pf(), s.gross, s.max_dd);
}

template <typename Driver>
static void emit_per_engine(FILE* rpt, const Driver& d) {
    const dea::Stats& s = d.stats;
    std::fprintf(rpt, "\n============================================================\n");
    std::fprintf(rpt, "  ENGINE: %s\n", d.name());
    std::fprintf(rpt, "============================================================\n");
    print_block(rpt, "ALL",     s);

    dea::Stats by_sess_asia;   by_sess_asia.n_trades   = s.n_asia;   by_sess_asia.gross   = s.g_asia;
    dea::Stats by_sess_london; by_sess_london.n_trades = s.n_london; by_sess_london.gross = s.g_london;
    dea::Stats by_sess_ny;     by_sess_ny.n_trades     = s.n_ny;     by_sess_ny.gross     = s.g_ny;
    dea::Stats by_sess_late;   by_sess_late.n_trades   = s.n_late;   by_sess_late.gross   = s.g_late;

    std::fprintf(rpt, "  BY SESSION (UTC)\n");
    std::fprintf(rpt, "    Asia    n=%-6d gross=%+10.3f\n", s.n_asia,   s.g_asia);
    std::fprintf(rpt, "    London  n=%-6d gross=%+10.3f\n", s.n_london, s.g_london);
    std::fprintf(rpt, "    NY      n=%-6d gross=%+10.3f\n", s.n_ny,     s.g_ny);
    std::fprintf(rpt, "    Late_NY n=%-6d gross=%+10.3f\n", s.n_late,   s.g_late);

    if (!s.g_by_month.empty()) {
        std::fprintf(rpt, "  BY MONTH\n");
        for (const auto& kv : s.g_by_month) {
            auto it_n = s.n_by_month.find(kv.first);
            const int nm = (it_n != s.n_by_month.end()) ? it_n->second : 0;
            std::fprintf(rpt, "    %s n=%-5d gross=%+10.3f\n", kv.first.c_str(), nm, kv.second);
        }
    }
}

// ---------------------------------------------------------------------------
// Main -- compile-time driver tuple, single fold-expression dispatch
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <xau_tick_csv> [report_file]\n", argv[0]);
        return 1;
    }
    const std::string path = argv[1];
    const std::string report_path = (argc >= 3) ? std::string(argv[2])
                                                : std::string("/tmp/disabled_engine_audit_report.txt");
    FILE* RPT = std::fopen(report_path.c_str(), "w");
    if (!RPT) { std::fprintf(stderr, "ERROR: cannot open %s\n", report_path.c_str()); return 3; }
    std::fprintf(stderr, "[DEA] report -> %s\n", report_path.c_str());

    CoutSilencer cout_kill;
    // Engines also use printf() / fprintf(stdout) directly (e.g. COST-GUARD
    // BLOCKED lines from OmegaCostGuard). std::cout silencer doesn't cover
    // those. Redirect FD 1 to /dev/null so all stdout-bound writes vanish.
    // RPT is a separate FILE* so the report still lands.
    std::freopen("/dev/null", "w", stdout);

    // Compile-time driver tuple. To add an engine: include its driver in
    // disabled_engine_audit_drivers.hpp and append the type here. The fold
    // expression below dispatches every tick to all drivers with no runtime
    // virtual call overhead.
    using DriverTuple = std::tuple<
        dea::GoldBracketDriver,
        dea::XauusdFvgDriver,
        dea::DonchianDriver
    >;
    DriverTuple drivers;

    // Tape replay.
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) { std::fprintf(stderr, "ERROR: cannot open %s\n", path.c_str()); std::fclose(RPT); return 2; }

    char line[1024];
    int64_t lines = 0, parsed = 0, t0 = 0, t1 = 0;
    constexpr int64_t PROGRESS_EVERY = 5'000'000;
    const auto wall_t0 = std::chrono::steady_clock::now();

    while (std::fgets(line, sizeof(line), f)) {
        ++lines;
        Tick t;
        if (!parse_duka_row(line, t)) continue;
        ++parsed;
        if (t0 == 0) t0 = t.ts_ms;
        t1 = t.ts_ms;

        // Fold-expression dispatch over the driver tuple.
        std::apply([&](auto&... d) {
            (d.feed_tick(t.bid, t.ask, t.ts_ms), ...);
        }, drivers);

        if (lines % PROGRESS_EVERY == 0) {
            const auto wall_now = std::chrono::steady_clock::now();
            const double wall_s = std::chrono::duration<double>(wall_now - wall_t0).count();
            std::fprintf(stderr,
                "\r[DEA] %lldM lines  %.1fs wall  rate=%.1fM/s  ",
                (long long)(lines / 1'000'000), wall_s, (lines / 1e6) / wall_s);
        }
    }
    std::fclose(f);

    const double hours = (t1 > t0) ? (t1 - t0) / 3600000.0 : 0.0;
    const auto wall_t1 = std::chrono::steady_clock::now();
    const double wall_total_s = std::chrono::duration<double>(wall_t1 - wall_t0).count();
    std::fprintf(stderr, "\n[DEA] done. %lld lines, %lld parsed, %.1fd simulated, %.1fs wall\n",
                 (long long)lines, (long long)parsed, hours / 24.0, wall_total_s);

    // Report header + per-engine sections via fold expression.
    std::fprintf(RPT, "============================================================\n");
    std::fprintf(RPT, "  disabled_engine_audit  --  %s\n", path.c_str());
    std::fprintf(RPT, "  corpus: %lld lines, %.1fd simulated, %.1fs wall\n",
                 (long long)parsed, hours / 24.0, wall_total_s);
    std::fprintf(RPT, "============================================================\n");

    std::apply([&](const auto&... d) {
        (emit_per_engine(RPT, d), ...);
    }, drivers);

    std::fclose(RPT);
    std::fprintf(stderr, "[DEA] report written to %s\n", report_path.c_str());
    return 0;
}

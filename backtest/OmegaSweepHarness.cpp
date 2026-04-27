// =============================================================================
// OmegaSweepHarness.cpp -- Pairwise 2-factor parameter sweep (S51 X3)
// =============================================================================
//
// PURPOSE
//   Run a pairwise 2-factor parameter sweep across HBG, AsianRange,
//   VWAPStretch, and EMACross. For each engine, vary 2 of its 5 parameters
//   over a 7x7 geometric grid (0.5x..2.0x of default) while holding the
//   other 3 at default. Across all C(5,2) = 10 param pairs, that yields
//   10 * 49 = 490 combinations per engine.
//
//   This shape was chosen over the original 7^5 = 16807-combo full grid
//   because:
//     * 16807 instantiations OOMs the compiler at scale (template instance
//       memory grows super-linearly in tuple width past ~2000).
//     * Most of the variance in trading-engine PnL is captured by 2-way
//       interactions; pure 3+-way interactions are rare in practice.
//     * 490 combos per engine compiles in ~2-3 minutes per engine on Mac
//       (measured: 100 instantiations -> 27s; near-linear scaling).
//
//   DXY is skipped at the run-list level (no DXY tick stream is wired
//   into the backtest input).
//
// AUTHORITY
//   Created S51 X3 (2026-04-27) under explicit user authorisation. Replaces
//   S51 X2 OmegaSweepHarness which used runtime Params dispatch + std::function
//   callback (measured 0.3 combos/sec, infeasibly slow). X3 fixes:
//     * Per-tick std::function construction at call site -> templated Sink&
//     * Per-tick std::time / steady_clock::now() syscalls in 3 engines ->
//       routed through omega::bt::g_sim_now_ms via OmegaTimeShim
//     * O(n) std::deque scan in HBG -> O(1) MinMaxCircularBuffer
//     * Runtime Params struct dispatch -> compile-time class templates
//
// DESIGN
//   * mmap the tick CSV once; share read-only across threads (OS page-share).
//   * Single tick decoder shared with OmegaBacktest (Dukascopy + ts/ba/ohlcv).
//   * Per-engine: build a std::tuple<HBG_T<I>...> over an index_sequence of
//     490 entries. Each I maps via constexpr lookup tables to the (p1..p5)
//     values for that combo.
//   * One worker thread per engine (4 threads max). Each thread walks the
//     full tick stream once, dispatching every tick into all 490 tuple
//     entries via fold-expression. Aggregates per-combo PnL, win rate, and
//     per-quarter PnL for stability scoring.
//   * Output: sweep_results/sweep_<engine>.csv with all combos ranked by
//     score = stability * total_pnl, where stability = 1 / (1 + stddev_q).
//
// USAGE
//   OmegaSweepHarness <ticks.csv> [options]
//     --engine <list>   comma list (default: hbg,asianrange,vwapstretch,emacross)
//                       Available: hbg, asianrange, vwapstretch, emacross
//     --outdir <dir>    output directory (default: sweep_results)
//     --warmup <n>      ticks to skip before recording trades (default: 5000)
//     --from-date <d>   skip ticks before YYYY-MM-DD (e.g. 2024-09-01)
//     --to-date <d>     skip ticks at/after YYYY-MM-DD
//     --verbose         print per-engine progress
//
// OUTPUT
//   <outdir>/sweep_<engine>.csv   one CSV per engine with all 490 combos
//   <outdir>/sweep_summary.txt    human-readable top-50 per engine
//
// =============================================================================

// OmegaTimeShim.hpp is force-included by CMakeLists (-include / /FI), so
// OMEGA_BT_SHIM_ACTIVE is defined throughout this TU.

#include "BtBarEngine.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cinttypes>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <tuple>
#include <utility>
#include <array>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <io.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#include "../include/OmegaTradeLedger.hpp"
#include "../include/SweepableEngines.hpp"

// =============================================================================
// MemMappedFile -- byte-identical to OmegaBacktest.cpp's implementation
// =============================================================================
class MemMappedFile {
public:
    const char* data = nullptr;
    size_t      size = 0;

    bool open(const char* path) noexcept {
#ifdef _WIN32
        hFile_ = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile_ == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{}; GetFileSizeEx(hFile_, &sz);
        size  = static_cast<size_t>(sz.QuadPart);
        hMap_ = CreateFileMappingA(hFile_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!hMap_) { CloseHandle(hFile_); return false; }
        data  = static_cast<const char*>(MapViewOfFile(hMap_, FILE_MAP_READ, 0, 0, 0));
        return data != nullptr;
#else
        fd_ = ::open(path, O_RDONLY);
        if (fd_ < 0) return false;
        struct stat st{}; fstat(fd_, &st);
        size = static_cast<size_t>(st.st_size);
        data = static_cast<const char*>(
            mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd_, 0));
        if (data == reinterpret_cast<const char*>(-1)) {
            data = nullptr; ::close(fd_); return false;
        }
        madvise(const_cast<char*>(data), size, MADV_SEQUENTIAL);
        return true;
#endif
    }

    ~MemMappedFile() {
#ifdef _WIN32
        if (data)  UnmapViewOfFile(data);
        if (hMap_) CloseHandle(hMap_);
        if (hFile_ && hFile_ != INVALID_HANDLE_VALUE) CloseHandle(hFile_);
#else
        if (data && size) munmap(const_cast<char*>(data), size);
        if (fd_ >= 0) ::close(fd_);
#endif
    }

private:
#ifdef _WIN32
    HANDLE hFile_ = nullptr;
    HANDLE hMap_  = nullptr;
#else
    int fd_ = -1;
#endif
};

// =============================================================================
// Fast number parsers (lifted from OmegaBacktest.cpp pattern)
// =============================================================================
static inline int64_t fast_i64(const char* p, const char** nx) noexcept {
    int64_t v = 0; bool neg = false;
    if (*p == '-') { neg = true; ++p; }
    while (*p >= '0' && *p <= '9') { v = v*10 + (*p - '0'); ++p; }
    *nx = p; return neg ? -v : v;
}
static inline double fast_f(const char* p, const char** nx) noexcept {
    double v = 0; bool neg = false;
    if (*p == '-') { neg = true; ++p; }
    while (*p >= '0' && *p <= '9') { v = v*10 + (*p - '0'); ++p; }
    if (*p == '.') {
        ++p; double f = 0.1;
        while (*p >= '0' && *p <= '9') { v += (*p - '0') * f; f *= 0.1; ++p; }
    }
    *nx = p; return neg ? -v : v;
}
// Dukascopy timestamp: YYYY.MM.DD,HH:MM:SS.mmm
static inline int64_t duka_ts(const char* dp, const char* tp) noexcept {
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

// Parse a YYYY-MM-DD argument to epoch-ms at 00:00:00 UTC.
static int64_t parse_date_arg(const char* s) noexcept {
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

// =============================================================================
// Tick row + parser (subset of OmegaBacktest -- only what the harness uses)
// =============================================================================
struct TickRow { int64_t ts_ms; double bid; double ask; };

enum class Fmt { TS_BA, TS_OHLCV, DUKA };
static Fmt sniff_format(const char* p, const char* end) {
    const char* q = p;
    int commas = 0;
    while (q < end && *q != '\n') { if (*q == ',') ++commas; ++q; }
    if (p < end) {
        bool has_dot_in_first = false;
        const char* r = p;
        while (r < end && *r != ',' && *r != '\n') {
            if (*r == '.') { has_dot_in_first = true; break; }
            ++r;
        }
        if (has_dot_in_first && commas >= 4) return Fmt::DUKA;
    }
    if (commas >= 5) return Fmt::TS_OHLCV;
    return Fmt::TS_BA;
}

static std::vector<TickRow> parse_csv(const MemMappedFile& f,
                                       int64_t from_ms, int64_t to_ms,
                                       bool verbose) {
    std::vector<TickRow> v;
    v.reserve(static_cast<size_t>(160'000'000));

    const char* p   = f.data;
    const char* end = f.data + f.size;
    if (p >= end) return v;

    // Skip a possible header line if the first line doesn't start with a digit.
    // Also capture the header so we can detect column order for TS_BA-style
    // files that ship as "timestamp,askPrice,bidPrice" instead of the assumed
    // "ts,bid,ask" ordering.
    bool ts_ba_ask_first = false;  // true if header indicates ask precedes bid
    if (*p < '0' || *p > '9') {
        const char* hdr_start = p;
        while (p < end && *p != '\n') ++p;
        const char* hdr_end = p;
        if (p < end) ++p;

        // Search the header for "ask" and "bid" (case-insensitive). If "ask"
        // appears before "bid", flag column-order swap for the TS_BA branch.
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
    if (verbose) {
        const char* fname = (fmt==Fmt::DUKA?"DUKA":fmt==Fmt::TS_OHLCV?"TS_OHLCV":"TS_BA");
        std::printf("  format detected: %s%s\n", fname,
                    (fmt == Fmt::TS_BA && ts_ba_ask_first) ? " (ask-first column order)" : "");
        std::fflush(stdout);
    }

    int64_t skipped_before = 0;
    int64_t skipped_after  = 0;

    while (p < end) {
        TickRow r{};
        const char* nx;

        if (fmt == Fmt::DUKA) {
            // YYYY.MM.DD,HH:MM:SS.mmm,bid,ask,vol
            const char* dp = p;
            while (p < end && *p != ',') ++p;
            if (p >= end) break;
            const char* tp = p + 1;
            while (p < end && *p != ',') ++p;
            if (p >= end) break;
            r.ts_ms = duka_ts(dp, tp);
            ++p;
            r.bid = fast_f(p, &nx); p = nx;
            if (p >= end || *p != ',') break;
            ++p;
            r.ask = fast_f(p, &nx); p = nx;
        } else if (fmt == Fmt::TS_OHLCV) {
            // ts,open,high,low,close,vol -> mid = (h+l)/2 effectively
            r.ts_ms = fast_i64(p, &nx); p = nx;
            if (p >= end || *p != ',') { break; } ++p;
            double o = fast_f(p, &nx); p = nx; if (p >= end || *p != ',') { break; } ++p;
            double h = fast_f(p, &nx); p = nx; if (p >= end || *p != ',') { break; } ++p;
            double l = fast_f(p, &nx); p = nx; if (p >= end || *p != ',') { break; } ++p;
            double c = fast_f(p, &nx); p = nx;
            (void)o; (void)c;
            // synthesise bid/ask from mid +- typical spread
            double mid = 0.5 * (h + l);
            r.bid = mid - 0.05;
            r.ask = mid + 0.05;
        } else {
            // TS_BA family: timestamp + bid + ask in some order
            r.ts_ms = fast_i64(p, &nx); p = nx;
            if (p >= end || *p != ',') { break; } ++p;
            const double v1 = fast_f(p, &nx); p = nx;
            if (p >= end || *p != ',') { break; } ++p;
            const double v2 = fast_f(p, &nx); p = nx;
            if (ts_ba_ask_first) {
                // ts,ask,bid
                r.ask = v1; r.bid = v2;
            } else {
                // ts,bid,ask  (default / OmegaBacktest convention)
                r.bid = v1; r.ask = v2;
            }
        }

        // Skip to end of line
        while (p < end && *p != '\n') ++p;
        if (p < end) ++p;

        if (r.ts_ms <= 0 || r.bid <= 0 || r.ask <= 0 || r.bid >= r.ask) continue;

        if (from_ms > 0 && r.ts_ms < from_ms) { ++skipped_before; continue; }
        if (to_ms   > 0 && r.ts_ms >= to_ms)  { ++skipped_after;  continue; }

        v.push_back(r);
    }

    if (verbose) {
        std::printf("  ticks loaded: %zu  (skipped before=%" PRId64 " after=%" PRId64 ")\n",
                    v.size(), skipped_before, skipped_after);
        std::fflush(stdout);
    }
    return v;
}

// =============================================================================
// Sweep grid -- 7-value geometric grid 0.5x..2.0x of default
// =============================================================================
// 7 multipliers: 0.5, 0.66, 0.87, 1.0, 1.15, 1.52, 2.0
// Geometric: ratio per step = 4^(1/6) ~= 1.2599
static constexpr double GRID_MULT[7] = {
    0.5, 0.6299605249474366, 0.7937005259840998,
    1.0, 1.2599210498948732, 1.5874010519681994, 2.0
};

// =============================================================================
// Param-pair indexing
// For a 5-param engine, C(5,2) = 10 pairs:
//   pair 0: (p0, p1)
//   pair 1: (p0, p2)
//   pair 2: (p0, p3)
//   pair 3: (p0, p4)
//   pair 4: (p1, p2)
//   pair 5: (p1, p3)
//   pair 6: (p1, p4)
//   pair 7: (p2, p3)
//   pair 8: (p2, p4)
//   pair 9: (p3, p4)
// Each pair gets 7x7 = 49 combos -> 490 total per engine.
// =============================================================================
static constexpr int N_PAIRS  = 10;
static constexpr int PAIR_GRID = 7;
static constexpr int N_COMBOS = N_PAIRS * PAIR_GRID * PAIR_GRID;  // 490

// Pair (p_a, p_b) for pair index k in [0..9].
constexpr std::pair<int,int> pair_indices(int k) {
    constexpr std::pair<int,int> P[10] = {
        {0,1}, {0,2}, {0,3}, {0,4},
        {1,2}, {1,3}, {1,4},
        {2,3}, {2,4},
        {3,4}
    };
    return P[k];
}

// For combo index I in [0..489], compute (pair_idx, ix_a, ix_b).
constexpr int combo_pair (int I) { return I / 49; }
constexpr int combo_ix_a (int I) { return (I % 49) / 7; }
constexpr int combo_ix_b (int I) { return (I % 49) % 7; }

// Multiplier for a given (pair, slot, axis) -- slot is the per-axis grid index
// (0..6), axis is 'a' or 'b'. For a given combo I, we pick:
//   mult_a = GRID_MULT[combo_ix_a(I)]
//   mult_b = GRID_MULT[combo_ix_b(I)]
// and apply them to the engine's params at positions pair_indices(combo_pair(I)).
// All 5 params have a "base value" (the live default); non-paired params stay
// at base.

// Compute the multiplier that applies to param p (0..4) for combo I.
// (Default 1.0 if param p is not one of the two paired params for I.)
constexpr double mult_for_param(int I, int p) {
    const int pi = combo_pair(I);
    const auto pp = pair_indices(pi);
    if (pp.first  == p) return GRID_MULT[combo_ix_a(I)];
    if (pp.second == p) return GRID_MULT[combo_ix_b(I)];
    return 1.0;
}

// =============================================================================
// Result aggregation
// =============================================================================
struct ComboResult {
    int    combo_id   = 0;
    double p[5]       = {0};      // resolved param values
    int    n_trades   = 0;
    int    wins       = 0;
    double total_pnl  = 0.0;
    double q_pnl[4]   = {0};      // per-quarter PnL (chronological quartiles)
    int    q_trades[4]= {0};
    double stddev_q   = 0.0;
    double stability  = 0.0;
    double score      = 0.0;
};

static int quarter_of(int64_t cur_idx, int64_t total_ticks) noexcept {
    if (total_ticks <= 0) return 0;
    int q = static_cast<int>((cur_idx * 4) / total_ticks);
    if (q < 0) q = 0;
    if (q > 3) q = 3;
    return q;
}

static void finalise_score(ComboResult& r) noexcept {
    double mean = 0.25 * (r.q_pnl[0] + r.q_pnl[1] + r.q_pnl[2] + r.q_pnl[3]);
    double var  = 0.0;
    for (int i = 0; i < 4; ++i) {
        double d = r.q_pnl[i] - mean;
        var += d * d;
    }
    var /= 4.0;
    r.stddev_q  = std::sqrt(var);
    r.stability = 1.0 / (1.0 + r.stddev_q);
    r.score     = r.stability * r.total_pnl;
}

// =============================================================================
// Per-combo Sink -- aggregates trades into a ComboResult.
// Templated callable; on_close is called by the engine when a trade closes.
// =============================================================================
struct ComboSink {
    ComboResult* out  = nullptr;
    int64_t      cur_idx     = 0;
    int64_t      warmup_ticks = 0;
    int64_t      total_ticks  = 0;

    void operator()(const omega::TradeRecord& tr) noexcept {
        if (!out) return;
        if (tr.entryTs <= 0) return;
        if (cur_idx < warmup_ticks) return;
        const int q = quarter_of(cur_idx, total_ticks);
        ++out->n_trades;
        if (tr.pnl > 0) ++out->wins;
        out->total_pnl += tr.pnl;
        out->q_pnl[q]  += tr.pnl;
        ++out->q_trades[q];
    }
};

// =============================================================================
// SnapshotBuilder -- builds GoldSnapshot from raw bid/ask for AsianRange,
// VWAPStretch, DXYDivergence (which take a snapshot, not raw bid/ask).
// Minimal version: tracks VWAP, volatility (rolling stddev), session by hour.
// =============================================================================
class SnapshotBuilder {
    double vwap_num_ = 0.0;
    double vwap_den_ = 0.0;
    int    vwap_day_ = -1;

    static constexpr int VOL_N = 64;
    double vol_buf_[VOL_N] = {};
    int    vol_h_ = 0, vol_c_ = 0;

    double prev_mid_ = 0.0;

    static omega::sweep::SessionType session_for_hour(int h) noexcept {
        if (h >= 0 && h <  7) return omega::sweep::SessionType::ASIAN;
        if (h >= 7 && h < 12) return omega::sweep::SessionType::LONDON;
        if (h >= 12 && h < 16) return omega::sweep::SessionType::OVERLAP;
        if (h >= 16 && h < 21) return omega::sweep::SessionType::NEWYORK;
        return omega::sweep::SessionType::UNKNOWN;
    }

public:
    void update(omega::sweep::GoldSnapshot& snap,
                double bid, double ask, int64_t ts_ms) noexcept {
        snap.bid = bid; snap.ask = ask;
        snap.mid = 0.5 * (bid + ask);
        snap.spread = ask - bid;

        const int64_t ts_s = ts_ms / 1000;
        struct tm ti{};
        time_t tt = static_cast<time_t>(ts_s);
#ifdef _WIN32
        gmtime_s(&ti, &tt);
#else
        gmtime_r(&tt, &ti);
#endif
        const int h = ti.tm_hour;
        const int yday = ti.tm_yday;

        // Daily VWAP reset
        if (yday != vwap_day_) {
            vwap_num_ = 0.0; vwap_den_ = 0.0;
            vwap_day_ = yday;
        }
        vwap_num_ += snap.mid;
        vwap_den_ += 1.0;
        snap.vwap = (vwap_den_ > 0.0) ? (vwap_num_ / vwap_den_) : snap.mid;

        // Rolling volatility (stddev over VOL_N mids)
        vol_buf_[vol_h_ % VOL_N] = snap.mid;
        ++vol_h_;
        if (vol_c_ < VOL_N) ++vol_c_;
        if (vol_c_ >= 8) {
            double sum = 0.0;
            for (int i = 0; i < vol_c_; ++i) sum += vol_buf_[i];
            const double mean = sum / vol_c_;
            double sq = 0.0;
            for (int i = 0; i < vol_c_; ++i) { double d = vol_buf_[i] - mean; sq += d*d; }
            snap.volatility = std::sqrt(sq / vol_c_);
        } else {
            snap.volatility = 0.0;
        }

        snap.trend = snap.mid - prev_mid_;
        snap.prev_mid = prev_mid_;
        prev_mid_ = snap.mid;

        snap.session = session_for_hour(h);
        snap.supervisor_regime = "";
        snap.dx_mid = 0.0;  // no DXY feed in this harness
    }
};

// =============================================================================
// Per-engine tuple machinery
// =============================================================================
// Each engine has a 5-param "base default" tuple. For combo I, we resolve
// the actual params via mult_for_param(I, p). Because non-type template
// parameters require constexpr values, we compute them at compile time
// via the constexpr mult_for_param() above.
//
// HBG params: MIN_RANGE=6.0, MAX_RANGE=25.0, SL_FRAC=0.5, TP_RR=2.0, TRAIL=0.25
// AsianRange params: BUFFER=0.50, MIN_RANGE=3.0, MAX_RANGE=50.0, SL=80, TP=200
// VWAPStretch params: SL=40, TP=88, COOLDOWN=300, SIGMA=2.0, VOL_WINDOW=40
// EMACross params: FAST=9, SLOW=15, RSI_LO=40.0, RSI_HI=50.0, SL_MULT=1.5
// =============================================================================

// ---- HBG -------------------------------------------------------------------
// Param positions: 0=MIN_RANGE, 1=MAX_RANGE, 2=SL_FRAC, 3=TP_RR, 4=TRAIL_FRAC
template <std::size_t I>
using HBG_AT = omega::sweep::HBG_T<
    6.0  * mult_for_param(static_cast<int>(I), 0),
    25.0 * mult_for_param(static_cast<int>(I), 1),
    0.5  * mult_for_param(static_cast<int>(I), 2),
    2.0  * mult_for_param(static_cast<int>(I), 3),
    0.25 * mult_for_param(static_cast<int>(I), 4)
>;

template <std::size_t... I>
auto make_hbg_tuple_impl(std::index_sequence<I...>) {
    return std::tuple<HBG_AT<I>...>{};
}
inline auto make_hbg_tuple() {
    return make_hbg_tuple_impl(std::make_index_sequence<N_COMBOS>{});
}

template <std::size_t... I, typename Tup, typename SinkArr>
inline void hbg_run_tick_impl(Tup& tup, SinkArr& sinks,
                              double bid, double ask, int64_t now_ms,
                              std::index_sequence<I...>) {
    (std::get<I>(tup).on_tick(bid, ask, now_ms,
                              true, false, false, 0, sinks[I]), ...);
}
template <typename Tup, typename SinkArr>
inline void hbg_run_tick(Tup& tup, SinkArr& sinks,
                         double bid, double ask, int64_t now_ms) {
    hbg_run_tick_impl(tup, sinks, bid, ask, now_ms,
                      std::make_index_sequence<N_COMBOS>{});
}

// Resolve the runtime param values for combo I (for output CSV).
static void hbg_params_for(int I, double out[5]) noexcept {
    out[0] = 6.0  * mult_for_param(I, 0);
    out[1] = 25.0 * mult_for_param(I, 1);
    out[2] = 0.5  * mult_for_param(I, 2);
    out[3] = 2.0  * mult_for_param(I, 3);
    out[4] = 0.25 * mult_for_param(I, 4);
}

// ---- EMACross --------------------------------------------------------------
// Param positions: 0=FAST_PERIOD, 1=SLOW_PERIOD, 2=RSI_LO, 3=RSI_HI, 4=SL_MULT
// FAST/SLOW are ints; we round the multiplied value at compile time.
constexpr int ema_int_param(int I, int p, int base) {
    return static_cast<int>(static_cast<double>(base) * mult_for_param(I, p) + 0.5);
}

template <std::size_t I>
using EMA_AT = omega::sweep::EMACrossT<
    ema_int_param(static_cast<int>(I), 0, 9),
    ema_int_param(static_cast<int>(I), 1, 15),
    40.0 * mult_for_param(static_cast<int>(I), 2),
    50.0 * mult_for_param(static_cast<int>(I), 3),
    1.5  * mult_for_param(static_cast<int>(I), 4)
>;

template <std::size_t... I>
auto make_ema_tuple_impl(std::index_sequence<I...>) {
    return std::tuple<EMA_AT<I>...>{};
}
inline auto make_ema_tuple() {
    return make_ema_tuple_impl(std::make_index_sequence<N_COMBOS>{});
}

template <std::size_t... I, typename Tup, typename SinkArr>
inline void ema_run_tick_impl(Tup& tup, SinkArr& sinks,
                              double bid, double ask, int64_t now_ms,
                              std::index_sequence<I...>) {
    (std::get<I>(tup).on_tick(bid, ask, now_ms, sinks[I]), ...);
}
template <typename Tup, typename SinkArr>
inline void ema_run_tick(Tup& tup, SinkArr& sinks,
                         double bid, double ask, int64_t now_ms) {
    ema_run_tick_impl(tup, sinks, bid, ask, now_ms,
                      std::make_index_sequence<N_COMBOS>{});
}

template <std::size_t... I, typename Tup>
inline void ema_run_bar_impl(Tup& tup,
                             double bar_close, double bar_atr, double bar_rsi,
                             int64_t now_ms,
                             std::index_sequence<I...>) {
    (std::get<I>(tup).on_bar(bar_close, bar_atr, bar_rsi, now_ms), ...);
}
template <typename Tup>
inline void ema_run_bar(Tup& tup,
                        double bar_close, double bar_atr, double bar_rsi,
                        int64_t now_ms) {
    ema_run_bar_impl(tup, bar_close, bar_atr, bar_rsi, now_ms,
                     std::make_index_sequence<N_COMBOS>{});
}

static void ema_params_for(int I, double out[5]) noexcept {
    out[0] = static_cast<double>(ema_int_param(I, 0, 9));
    out[1] = static_cast<double>(ema_int_param(I, 1, 15));
    out[2] = 40.0 * mult_for_param(I, 2);
    out[3] = 50.0 * mult_for_param(I, 3);
    out[4] = 1.5  * mult_for_param(I, 4);
}

// ---- AsianRange ------------------------------------------------------------
// Param positions: 0=BUFFER, 1=MIN_RANGE, 2=MAX_RANGE, 3=SL_TICKS, 4=TP_TICKS
constexpr int asian_int_param(int I, int p, int base) {
    return static_cast<int>(static_cast<double>(base) * mult_for_param(I, p) + 0.5);
}

template <std::size_t I>
using ASIAN_AT = omega::sweep::AsianRangeT<
    0.50 * mult_for_param(static_cast<int>(I), 0),
    3.0  * mult_for_param(static_cast<int>(I), 1),
    50.0 * mult_for_param(static_cast<int>(I), 2),
    asian_int_param(static_cast<int>(I), 3, 80),
    asian_int_param(static_cast<int>(I), 4, 200)
>;

template <std::size_t... I>
auto make_asian_tuple_impl(std::index_sequence<I...>) {
    return std::tuple<ASIAN_AT<I>...>{};
}
inline auto make_asian_tuple() {
    return make_asian_tuple_impl(std::make_index_sequence<N_COMBOS>{});
}

static void asian_params_for(int I, double out[5]) noexcept {
    out[0] = 0.50 * mult_for_param(I, 0);
    out[1] = 3.0  * mult_for_param(I, 1);
    out[2] = 50.0 * mult_for_param(I, 2);
    out[3] = static_cast<double>(asian_int_param(I, 3, 80));
    out[4] = static_cast<double>(asian_int_param(I, 4, 200));
}

// ---- VWAPStretch -----------------------------------------------------------
// Param positions: 0=SL_TICKS, 1=TP_TICKS, 2=COOLDOWN_SEC, 3=SIGMA_ENTRY, 4=VOL_WINDOW
constexpr int vwap_int_param(int I, int p, int base) {
    return static_cast<int>(static_cast<double>(base) * mult_for_param(I, p) + 0.5);
}

template <std::size_t I>
using VWAP_AT = omega::sweep::VWAPStretchT<
    vwap_int_param(static_cast<int>(I), 0, 40),
    vwap_int_param(static_cast<int>(I), 1, 88),
    vwap_int_param(static_cast<int>(I), 2, 300),
    2.0 * mult_for_param(static_cast<int>(I), 3),
    vwap_int_param(static_cast<int>(I), 4, 40)
>;

template <std::size_t... I>
auto make_vwap_tuple_impl(std::index_sequence<I...>) {
    return std::tuple<VWAP_AT<I>...>{};
}
inline auto make_vwap_tuple() {
    return make_vwap_tuple_impl(std::make_index_sequence<N_COMBOS>{});
}

static void vwap_params_for(int I, double out[5]) noexcept {
    out[0] = static_cast<double>(vwap_int_param(I, 0, 40));
    out[1] = static_cast<double>(vwap_int_param(I, 1, 88));
    out[2] = static_cast<double>(vwap_int_param(I, 2, 300));
    out[3] = 2.0 * mult_for_param(I, 3);
    out[4] = static_cast<double>(vwap_int_param(I, 4, 40));
}

// AsianRange/VWAPStretch are signal-emitters; the harness owns SL/TP simulation.
// We loop over the tuple indices manually for those two because they need
// pre-tuple setup (ATR/RSI for EMACross via on_bar comes from BtBarEngine).

// =============================================================================
// HBG runner -- single-thread, walks all ticks, fans into 490-tuple
// =============================================================================
static void run_hbg_sweep(const std::vector<TickRow>& ticks,
                          int64_t warmup_ticks,
                          std::vector<ComboResult>& results,
                          bool verbose) {
    auto engines = make_hbg_tuple();
    std::array<ComboSink, N_COMBOS> sinks{};
    results.assign(N_COMBOS, ComboResult{});

    const int64_t N = static_cast<int64_t>(ticks.size());

    for (int i = 0; i < N_COMBOS; ++i) {
        results[i].combo_id = i;
        hbg_params_for(i, results[i].p);
        sinks[i].out          = &results[i];
        sinks[i].cur_idx      = 0;
        sinks[i].warmup_ticks = warmup_ticks;
        sinks[i].total_ticks  = N;
    }

    int64_t progress_step = N / 20;
    if (progress_step < 1) progress_step = 1;

    for (int64_t k = 0; k < N; ++k) {
        const TickRow& r = ticks[k];
        omega::bt::set_sim_time(r.ts_ms);

        // Update per-sink cur_idx so warmup gating works
        for (int i = 0; i < N_COMBOS; ++i) sinks[i].cur_idx = k;

        hbg_run_tick(engines, sinks, r.bid, r.ask, r.ts_ms);

        if (verbose && (k % progress_step == 0)) {
            std::printf("  HBG  %5.1f%%  (%" PRId64 "/%" PRId64 ")\n",
                        100.0 * static_cast<double>(k) / static_cast<double>(N),
                        k, N);
            std::fflush(stdout);
        }
    }

    for (auto& res : results) finalise_score(res);
}

// =============================================================================
// EMACross runner -- needs bar-driven ATR/RSI updates alongside ticks.
// We use BtBarEngine to build 1-minute bars and feed on_bar() once per bar
// close; on_tick() runs every tick.
// =============================================================================
static void run_ema_sweep(const std::vector<TickRow>& ticks,
                          int64_t warmup_ticks,
                          std::vector<ComboResult>& results,
                          bool verbose) {
    auto engines = make_ema_tuple();
    std::array<ComboSink, N_COMBOS> sinks{};
    results.assign(N_COMBOS, ComboResult{});

    const int64_t N = static_cast<int64_t>(ticks.size());

    for (int i = 0; i < N_COMBOS; ++i) {
        results[i].combo_id = i;
        ema_params_for(i, results[i].p);
        sinks[i].out          = &results[i];
        sinks[i].cur_idx      = 0;
        sinks[i].warmup_ticks = warmup_ticks;
        sinks[i].total_ticks  = N;
    }

    omega::bt::BtBarEngine<1> bars;
    int64_t progress_step = N / 20;
    if (progress_step < 1) progress_step = 1;

    for (int64_t k = 0; k < N; ++k) {
        const TickRow& r = ticks[k];
        omega::bt::set_sim_time(r.ts_ms);

        // Update bar engine; if a bar closed AND indicators are ready, feed
        // on_bar to all 490 instances.
        const double mid = 0.5 * (r.bid + r.ask);
        const bool bar_closed = bars.on_tick(mid, r.ts_ms);
        if (bar_closed && bars.indicators_ready()) {
            const double bar_close = bars.last_closed_bar().close;
            const double bar_atr   = bars.atr14();
            const double bar_rsi   = bars.rsi14();
            ema_run_bar(engines, bar_close, bar_atr, bar_rsi, r.ts_ms);
        }

        for (int i = 0; i < N_COMBOS; ++i) sinks[i].cur_idx = k;
        ema_run_tick(engines, sinks, r.bid, r.ask, r.ts_ms);

        if (verbose && (k % progress_step == 0)) {
            std::printf("  EMA  %5.1f%%  (%" PRId64 "/%" PRId64 ")\n",
                        100.0 * static_cast<double>(k) / static_cast<double>(N),
                        k, N);
            std::fflush(stdout);
        }
    }

    for (auto& res : results) finalise_score(res);
}

// =============================================================================
// AsianRange runner -- emits Signals; harness simulates SL/TP based on
// SL_TICKS / TP_TICKS as price points (ticks of 0.01 -> price units).
// One open position per combo (matches live engine cooldown semantics).
// =============================================================================
struct ManagedPos {
    bool   active = false;
    bool   is_long = false;
    double entry  = 0.0;
    double sl     = 0.0;
    double tp     = 0.0;
    int64_t entry_ts = 0;
};

static inline double tick_to_price(double ticks) noexcept {
    // XAUUSD tick = 0.01 price units
    return ticks * 0.01;
}

static void close_managed(ComboResult& out, ManagedPos& pos,
                          double exit_px, int64_t now_s, int64_t cur_idx,
                          int64_t warmup_ticks, int64_t total_ticks) noexcept {
    if (!pos.active) return;
    const double pnl_pts = pos.is_long ? (exit_px - pos.entry)
                                       : (pos.entry - exit_px);
    const double pnl = pnl_pts * 0.01;  // size 0.01 lot; pnl in (price * lot)
    if (cur_idx >= warmup_ticks) {
        const int q = quarter_of(cur_idx, total_ticks);
        ++out.n_trades;
        if (pnl > 0) ++out.wins;
        out.total_pnl += pnl;
        out.q_pnl[q]  += pnl;
        ++out.q_trades[q];
    }
    pos = ManagedPos{};
    (void)now_s;
}

template <typename EngineTup, std::size_t... I>
static void asian_run_tick_impl(EngineTup& tup,
                                std::array<omega::sweep::Signal, N_COMBOS>& sigs,
                                const omega::sweep::GoldSnapshot& snap,
                                std::index_sequence<I...>) {
    ((sigs[I] = std::get<I>(tup).process(snap)), ...);
}
template <typename EngineTup>
static void asian_run_tick(EngineTup& tup,
                           std::array<omega::sweep::Signal, N_COMBOS>& sigs,
                           const omega::sweep::GoldSnapshot& snap) {
    asian_run_tick_impl(tup, sigs, snap, std::make_index_sequence<N_COMBOS>{});
}

static void run_asian_sweep(const std::vector<TickRow>& ticks,
                            int64_t warmup_ticks,
                            std::vector<ComboResult>& results,
                            bool verbose) {
    auto engines = make_asian_tuple();
    results.assign(N_COMBOS, ComboResult{});
    std::array<ManagedPos, N_COMBOS> positions{};
    std::array<omega::sweep::Signal, N_COMBOS> sigs{};

    const int64_t N = static_cast<int64_t>(ticks.size());

    for (int i = 0; i < N_COMBOS; ++i) {
        results[i].combo_id = i;
        asian_params_for(i, results[i].p);
    }

    SnapshotBuilder sb;
    omega::sweep::GoldSnapshot snap;
    int64_t progress_step = N / 20;
    if (progress_step < 1) progress_step = 1;

    for (int64_t k = 0; k < N; ++k) {
        const TickRow& r = ticks[k];
        omega::bt::set_sim_time(r.ts_ms);
        sb.update(snap, r.bid, r.ask, r.ts_ms);
        const int64_t now_s = r.ts_ms / 1000;

        // First: manage any open positions (SL/TP check against current bid/ask)
        for (int i = 0; i < N_COMBOS; ++i) {
            ManagedPos& p = positions[i];
            if (!p.active) continue;
            const double sl_dist = tick_to_price(results[i].p[3]);
            const double tp_dist = tick_to_price(results[i].p[4]);
            const double tp_px = p.is_long ? (p.entry + tp_dist) : (p.entry - tp_dist);
            const double sl_px = p.is_long ? (p.entry - sl_dist) : (p.entry + sl_dist);
            const bool tp_hit = p.is_long ? (r.ask >= tp_px) : (r.bid <= tp_px);
            const bool sl_hit = p.is_long ? (r.bid <= sl_px) : (r.ask >= sl_px);
            if (tp_hit) close_managed(results[i], p, tp_px, now_s, k,
                                       warmup_ticks, N);
            else if (sl_hit) close_managed(results[i], p, sl_px, now_s, k,
                                            warmup_ticks, N);
        }

        // Then: run the engines; entries open new positions
        asian_run_tick(engines, sigs, snap);
        for (int i = 0; i < N_COMBOS; ++i) {
            const omega::sweep::Signal& s = sigs[i];
            if (!s.valid || positions[i].active) continue;
            ManagedPos& p = positions[i];
            p.active = true;
            p.is_long = (s.side == omega::sweep::TradeSide::LONG);
            p.entry   = s.entry;
            p.entry_ts = now_s;
        }

        if (verbose && (k % progress_step == 0)) {
            std::printf("  ASN  %5.1f%%  (%" PRId64 "/%" PRId64 ")\n",
                        100.0 * static_cast<double>(k) / static_cast<double>(N),
                        k, N);
            std::fflush(stdout);
        }
    }

    for (auto& res : results) finalise_score(res);
}

// =============================================================================
// VWAPStretch runner -- same shape as AsianRange (signal-emitter, harness
// manages SL/TP).
// =============================================================================
template <typename EngineTup, std::size_t... I>
static void vwap_run_tick_impl(EngineTup& tup,
                               std::array<omega::sweep::Signal, N_COMBOS>& sigs,
                               const omega::sweep::GoldSnapshot& snap,
                               std::index_sequence<I...>) {
    ((sigs[I] = std::get<I>(tup).process(snap)), ...);
}
template <typename EngineTup>
static void vwap_run_tick(EngineTup& tup,
                          std::array<omega::sweep::Signal, N_COMBOS>& sigs,
                          const omega::sweep::GoldSnapshot& snap) {
    vwap_run_tick_impl(tup, sigs, snap, std::make_index_sequence<N_COMBOS>{});
}

static void run_vwap_sweep(const std::vector<TickRow>& ticks,
                           int64_t warmup_ticks,
                           std::vector<ComboResult>& results,
                           bool verbose) {
    auto engines = make_vwap_tuple();
    results.assign(N_COMBOS, ComboResult{});
    std::array<ManagedPos, N_COMBOS> positions{};
    std::array<omega::sweep::Signal, N_COMBOS> sigs{};

    const int64_t N = static_cast<int64_t>(ticks.size());

    for (int i = 0; i < N_COMBOS; ++i) {
        results[i].combo_id = i;
        vwap_params_for(i, results[i].p);
    }

    SnapshotBuilder sb;
    omega::sweep::GoldSnapshot snap;
    int64_t progress_step = N / 20;
    if (progress_step < 1) progress_step = 1;

    for (int64_t k = 0; k < N; ++k) {
        const TickRow& r = ticks[k];
        omega::bt::set_sim_time(r.ts_ms);
        sb.update(snap, r.bid, r.ask, r.ts_ms);
        const int64_t now_s = r.ts_ms / 1000;

        for (int i = 0; i < N_COMBOS; ++i) {
            ManagedPos& p = positions[i];
            if (!p.active) continue;
            const double sl_dist = tick_to_price(results[i].p[0]);
            const double tp_dist = tick_to_price(results[i].p[1]);
            const double tp_px = p.is_long ? (p.entry + tp_dist) : (p.entry - tp_dist);
            const double sl_px = p.is_long ? (p.entry - sl_dist) : (p.entry + sl_dist);
            const bool tp_hit = p.is_long ? (r.ask >= tp_px) : (r.bid <= tp_px);
            const bool sl_hit = p.is_long ? (r.bid <= sl_px) : (r.ask >= sl_px);
            if (tp_hit) close_managed(results[i], p, tp_px, now_s, k,
                                       warmup_ticks, N);
            else if (sl_hit) close_managed(results[i], p, sl_px, now_s, k,
                                            warmup_ticks, N);
        }

        vwap_run_tick(engines, sigs, snap);
        for (int i = 0; i < N_COMBOS; ++i) {
            const omega::sweep::Signal& s = sigs[i];
            if (!s.valid || positions[i].active) continue;
            ManagedPos& p = positions[i];
            p.active = true;
            p.is_long = (s.side == omega::sweep::TradeSide::LONG);
            p.entry   = s.entry;
            p.entry_ts = now_s;
        }

        if (verbose && (k % progress_step == 0)) {
            std::printf("  VWP  %5.1f%%  (%" PRId64 "/%" PRId64 ")\n",
                        100.0 * static_cast<double>(k) / static_cast<double>(N),
                        k, N);
            std::fflush(stdout);
        }
    }

    for (auto& res : results) finalise_score(res);
}

// =============================================================================
// CSV output
// =============================================================================
static void write_csv(const std::string& path,
                      const std::vector<ComboResult>& results,
                      const std::array<const char*, 5>& param_names) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "ERROR: could not open %s for write\n", path.c_str());
        return;
    }
    std::fprintf(f, "combo_id,%s,%s,%s,%s,%s,n_trades,wins,win_rate,total_pnl,"
                    "q1_pnl,q2_pnl,q3_pnl,q4_pnl,q1_n,q2_n,q3_n,q4_n,"
                    "stddev_q,stability,score\n",
                 param_names[0], param_names[1], param_names[2],
                 param_names[3], param_names[4]);
    for (const auto& r : results) {
        const double wr = r.n_trades > 0
            ? 100.0 * static_cast<double>(r.wins) / static_cast<double>(r.n_trades)
            : 0.0;
        std::fprintf(f, "%d,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d,%.4f,%.4f,"
                        "%.4f,%.4f,%.4f,%.4f,%d,%d,%d,%d,"
                        "%.6f,%.6f,%.6f\n",
                     r.combo_id,
                     r.p[0], r.p[1], r.p[2], r.p[3], r.p[4],
                     r.n_trades, r.wins, wr, r.total_pnl,
                     r.q_pnl[0], r.q_pnl[1], r.q_pnl[2], r.q_pnl[3],
                     r.q_trades[0], r.q_trades[1], r.q_trades[2], r.q_trades[3],
                     r.stddev_q, r.stability, r.score);
    }
    std::fclose(f);
}

static void append_summary(FILE* sf, const char* engine_name,
                           const std::vector<ComboResult>& results,
                           const std::array<const char*, 5>& param_names) {
    std::vector<ComboResult> ranked = results;
    std::sort(ranked.begin(), ranked.end(),
              [](const ComboResult& a, const ComboResult& b){
                  return a.score > b.score;
              });
    std::fprintf(sf, "\n========== %s top-50 by score ==========\n", engine_name);
    std::fprintf(sf, "%-6s %-9s %-9s %-9s %-9s %-9s %7s %5s %10s %10s %10s\n",
                 "combo",
                 param_names[0], param_names[1], param_names[2],
                 param_names[3], param_names[4],
                 "trades", "wr%", "total_pnl", "stddev_q", "score");
    const int top = std::min(50, static_cast<int>(ranked.size()));
    for (int i = 0; i < top; ++i) {
        const auto& r = ranked[i];
        const double wr = r.n_trades > 0
            ? 100.0 * static_cast<double>(r.wins) / static_cast<double>(r.n_trades)
            : 0.0;
        std::fprintf(sf, "%-6d %-9.4f %-9.4f %-9.4f %-9.4f %-9.4f %7d %5.1f %10.2f %10.4f %10.4f\n",
                     r.combo_id,
                     r.p[0], r.p[1], r.p[2], r.p[3], r.p[4],
                     r.n_trades, wr, r.total_pnl, r.stddev_q, r.score);
    }
}

// =============================================================================
// Directory creation -- portable mkdir
// =============================================================================
static bool ensure_dir(const std::string& path) noexcept {
#ifdef _WIN32
    return CreateDirectoryA(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    if (mkdir(path.c_str(), 0755) == 0) return true;
    return errno == EEXIST;
#endif
}

// =============================================================================
// Args
// =============================================================================
struct Args {
    std::string ticks_path;
    std::string engines = "hbg,asianrange,vwapstretch,emacross";
    std::string outdir  = "sweep_results";
    int64_t     warmup  = 5000;
    int64_t     from_ms = 0;
    int64_t     to_ms   = 0;
    bool        verbose = false;
};

static void usage() {
    std::printf(
        "OmegaSweepHarness <ticks.csv> [options]\n"
        "  --engine <list>   comma list (default: hbg,asianrange,vwapstretch,emacross)\n"
        "                    Available: hbg, asianrange, vwapstretch, emacross\n"
        "  --outdir <dir>    output directory (default: sweep_results)\n"
        "  --warmup <n>      ticks to skip before recording trades (default: 5000)\n"
        "  --from-date <d>   skip ticks before YYYY-MM-DD\n"
        "  --to-date <d>     skip ticks at/after YYYY-MM-DD\n"
        "  --verbose         print per-engine progress\n"
    );
}

static bool parse_args(int argc, char** argv, Args& a) {
    if (argc < 2) { usage(); return false; }
    a.ticks_path = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--engine" && i+1 < argc) { a.engines = argv[++i]; }
        else if (s == "--outdir" && i+1 < argc) { a.outdir = argv[++i]; }
        else if (s == "--warmup" && i+1 < argc) { a.warmup = std::atoll(argv[++i]); }
        else if (s == "--from-date" && i+1 < argc) { a.from_ms = parse_date_arg(argv[++i]); }
        else if (s == "--to-date" && i+1 < argc) { a.to_ms = parse_date_arg(argv[++i]); }
        else if (s == "--verbose") { a.verbose = true; }
        else if (s == "--help" || s == "-h") { usage(); return false; }
        else {
            std::fprintf(stderr, "unknown arg: %s\n", s.c_str());
            usage();
            return false;
        }
    }
    return true;
}

static bool engine_in_list(const std::string& list, const char* name) {
    std::string s = "," + list + ",";
    std::string n = std::string(",") + name + ",";
    return s.find(n) != std::string::npos;
}

// =============================================================================
// main()
// =============================================================================
int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) return 1;

    std::printf("Omega Sweep Harness (S51 X3, pairwise 2-factor, 490 combos/engine)\n");
    std::printf("  ticks:   %s\n", args.ticks_path.c_str());
    std::printf("  engines: %s\n", args.engines.c_str());
    std::printf("  outdir:  %s\n", args.outdir.c_str());
    std::printf("  warmup:  %" PRId64 "\n", args.warmup);
    if (args.from_ms > 0) std::printf("  from_ms: %" PRId64 "\n", args.from_ms);
    if (args.to_ms   > 0) std::printf("  to_ms:   %" PRId64 "\n", args.to_ms);
    std::fflush(stdout);

    if (!ensure_dir(args.outdir)) {
        std::fprintf(stderr, "ERROR: cannot create outdir %s\n", args.outdir.c_str());
        return 1;
    }

    // -------- Load ticks once (shared across engines) ------------------------
    MemMappedFile mm;
    if (!mm.open(args.ticks_path.c_str())) {
        std::fprintf(stderr, "ERROR: cannot open %s\n", args.ticks_path.c_str());
        return 1;
    }
    std::printf("loading ticks (%.2f GB)...\n",
                static_cast<double>(mm.size) / (1024.0*1024.0*1024.0));
    std::fflush(stdout);
    auto t0 = std::chrono::steady_clock::now();
    const auto ticks = parse_csv(mm, args.from_ms, args.to_ms, args.verbose);
    auto t1 = std::chrono::steady_clock::now();
    const double load_sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("loaded %zu ticks in %.1fs\n", ticks.size(), load_sec);
    std::fflush(stdout);

    if (ticks.empty()) {
        std::fprintf(stderr, "ERROR: no ticks parsed\n");
        return 1;
    }

    // -------- Per-engine threads --------------------------------------------
    std::vector<ComboResult> hbg_res, ema_res, asian_res, vwap_res;
    std::vector<std::thread> threads;
    std::atomic<int> done{0};

    auto launch = [&](const char* name, auto&& fn) {
        if (engine_in_list(args.engines, name)) {
            std::printf("launching %s sweep thread\n", name);
            std::fflush(stdout);
            threads.emplace_back([fn, &done]() { fn(); ++done; });
        } else {
            std::printf("skipping %s (not in --engine list)\n", name);
        }
    };

    auto t_start = std::chrono::steady_clock::now();

    launch("hbg", [&]() {
        run_hbg_sweep(ticks, args.warmup, hbg_res, args.verbose);
    });
    launch("emacross", [&]() {
        run_ema_sweep(ticks, args.warmup, ema_res, args.verbose);
    });
    launch("asianrange", [&]() {
        run_asian_sweep(ticks, args.warmup, asian_res, args.verbose);
    });
    launch("vwapstretch", [&]() {
        run_vwap_sweep(ticks, args.warmup, vwap_res, args.verbose);
    });

    for (auto& th : threads) th.join();

    auto t_end = std::chrono::steady_clock::now();
    const double total_sec = std::chrono::duration<double>(t_end - t_start).count();
    std::printf("all sweeps complete in %.1fs (%.1f min)\n",
                total_sec, total_sec / 60.0);

    // -------- Output --------------------------------------------------------
    const std::string summary_path = args.outdir + "/sweep_summary.txt";
    FILE* sf = std::fopen(summary_path.c_str(), "w");
    if (!sf) {
        std::fprintf(stderr, "ERROR: cannot write %s\n", summary_path.c_str());
    } else {
        std::fprintf(sf, "Omega Sweep Harness S51 X3 -- pairwise 2-factor (490 combos/engine)\n");
        std::fprintf(sf, "ticks: %s\n", args.ticks_path.c_str());
        std::fprintf(sf, "n_ticks: %zu\n", ticks.size());
        std::fprintf(sf, "wall_time: %.1fs (%.1f min)\n", total_sec, total_sec / 60.0);
    }

    if (!hbg_res.empty()) {
        write_csv(args.outdir + "/sweep_hbg.csv", hbg_res,
                  {"min_range","max_range","sl_frac","tp_rr","trail_frac"});
        if (sf) append_summary(sf, "HBG", hbg_res,
                               {"min_range","max_range","sl_frac","tp_rr","trail_frac"});
        std::printf("wrote sweep_hbg.csv (%zu combos)\n", hbg_res.size());
    }
    if (!ema_res.empty()) {
        write_csv(args.outdir + "/sweep_emacross.csv", ema_res,
                  {"fast_period","slow_period","rsi_lo","rsi_hi","sl_mult"});
        if (sf) append_summary(sf, "EMACross", ema_res,
                               {"fast_period","slow_period","rsi_lo","rsi_hi","sl_mult"});
        std::printf("wrote sweep_emacross.csv (%zu combos)\n", ema_res.size());
    }
    if (!asian_res.empty()) {
        write_csv(args.outdir + "/sweep_asianrange.csv", asian_res,
                  {"buffer","min_range","max_range","sl_ticks","tp_ticks"});
        if (sf) append_summary(sf, "AsianRange", asian_res,
                               {"buffer","min_range","max_range","sl_ticks","tp_ticks"});
        std::printf("wrote sweep_asianrange.csv (%zu combos)\n", asian_res.size());
    }
    if (!vwap_res.empty()) {
        write_csv(args.outdir + "/sweep_vwapstretch.csv", vwap_res,
                  {"sl_ticks","tp_ticks","cooldown_sec","sigma_entry","vol_window"});
        if (sf) append_summary(sf, "VWAPStretch", vwap_res,
                               {"sl_ticks","tp_ticks","cooldown_sec","sigma_entry","vol_window"});
        std::printf("wrote sweep_vwapstretch.csv (%zu combos)\n", vwap_res.size());
    }

    if (sf) std::fclose(sf);
    std::printf("summary: %s\n", summary_path.c_str());
    return 0;
}

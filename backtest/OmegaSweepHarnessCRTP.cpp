// =============================================================================
// OmegaSweepHarnessCRTP.cpp -- Pairwise sweep harness on CRTP engines
// =============================================================================
//
// PURPOSE
//   Run the same pairwise 2-factor parameter sweep as OmegaSweepHarness.cpp
//   but driving CRTP engine types (omega::sweep_crtp::*_CRTP<Traits<I>>)
//   instead of non-type-template engines (omega::sweep::*_T<...>). Output
//   contract is identical: sweep_<engine>.csv files in --outdir, ranked by
//   stability x total_pnl, with the same column schema and the same G2
//   determinism self-test.
//
//   This binary coexists with OmegaSweepHarness (the non-type-template
//   harness). They are independent executables with no shared TUs.
//
// AUTHORITY
//   Created S51 1A.1.b post-G4 (2026-04-29) under explicit user authorisation
//   ("build the CRTP I asked for"). Mirrors the existing OmegaSweepHarness
//   code paths line-for-line for the parts that don't depend on engine
//   architecture (tick loader, snapshot builder, scoring, CSV output, G2
//   self-test). The engine dispatch fold-expressions are rewritten to call
//   the CRTP types' on_tick / process methods.
//
// USAGE (identical to OmegaSweepHarness)
//   OmegaSweepHarnessCRTP <ticks.csv> [options]
//     --engine <list>   comma list (default: hbg,asianrange,vwapstretch,emacross)
//     --outdir <dir>    output directory (default: sweep_results_crtp)
//     --warmup <n>      ticks to skip before recording trades (default: 5000)
//     --from-date <d>   skip ticks before YYYY-MM-DD
//     --to-date <d>     skip ticks at/after YYYY-MM-DD
//     --verbose         print per-engine progress
//     --no-selftest     skip the G2 determinism self-test at startup
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
#include <cerrno>
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
#include <memory>
#include <fstream>
#include <sstream>
#include <limits>

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
#include "../include/SweepableEnginesCRTP.hpp"

namespace sc = omega::sweep_crtp;

// =============================================================================
// MemMappedFile -- byte-identical to OmegaSweepHarness.cpp
// =============================================================================
class MemMappedFile {
public:
    const char* data = nullptr;
    std::size_t size = 0;

    bool open(const char* path) noexcept {
#ifdef _WIN32
        hFile_ = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile_ == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{}; GetFileSizeEx(hFile_, &sz);
        size  = static_cast<std::size_t>(sz.QuadPart);
        hMap_ = CreateFileMappingA(hFile_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!hMap_) { CloseHandle(hFile_); return false; }
        data  = static_cast<const char*>(MapViewOfFile(hMap_, FILE_MAP_READ, 0, 0, 0));
        return data != nullptr;
#else
        fd_ = ::open(path, O_RDONLY);
        if (fd_ < 0) return false;
        struct stat st{}; fstat(fd_, &st);
        size = static_cast<std::size_t>(st.st_size);
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
// Number / timestamp parsers (same as OmegaSweepHarness.cpp)
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
// Tick row + parser
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
    v.reserve(static_cast<std::size_t>(160'000'000));

    const char* p   = f.data;
    const char* end = f.data + f.size;
    if (p >= end) return v;

    bool ts_ba_ask_first = false;
    if (*p < '0' || *p > '9') {
        const char* hdr_start = p;
        while (p < end && *p != '\n') ++p;
        const char* hdr_end = p;
        if (p < end) ++p;

        auto find_token = [hdr_start, hdr_end](const char* tok, std::size_t tl) -> const char* {
            for (const char* q = hdr_start; q + tl <= hdr_end; ++q) {
                bool eq = true;
                for (std::size_t i = 0; i < tl; ++i) {
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
            r.ts_ms = fast_i64(p, &nx); p = nx;
            if (p >= end || *p != ',') { break; } ++p;
            double o = fast_f(p, &nx); p = nx; if (p >= end || *p != ',') { break; } ++p;
            double h = fast_f(p, &nx); p = nx; if (p >= end || *p != ',') { break; } ++p;
            double l = fast_f(p, &nx); p = nx; if (p >= end || *p != ',') { break; } ++p;
            double c = fast_f(p, &nx); p = nx;
            (void)o; (void)c;
            const double mid = 0.5 * (h + l);
            r.bid = mid - 0.05;
            r.ask = mid + 0.05;
        } else {
            r.ts_ms = fast_i64(p, &nx); p = nx;
            if (p >= end || *p != ',') { break; } ++p;
            const double v1 = fast_f(p, &nx); p = nx;
            if (p >= end || *p != ',') { break; } ++p;
            const double v2 = fast_f(p, &nx); p = nx;
            if (ts_ba_ask_first) { r.ask = v1; r.bid = v2; }
            else                 { r.bid = v1; r.ask = v2; }
        }

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
// Combo result + scoring (identical schema to OmegaSweepHarness.cpp)
// =============================================================================
struct ComboResult {
    int    combo_id   = 0;
    double p[5]       = {0};
    int    n_trades   = 0;
    int    wins       = 0;
    double total_pnl  = 0.0;
    double q_pnl[4]   = {0};
    int    q_trades[4]= {0};
    double stddev_q   = 0.0;
    double stability  = 0.0;
    double score      = 0.0;
    bool   degenerate = false;
};

static int quarter_of(int64_t cur_idx, int64_t total_ticks) noexcept {
    if (total_ticks <= 0) return 0;
    int q = static_cast<int>((cur_idx * 4) / total_ticks);
    if (q < 0) q = 0;
    if (q > 3) q = 3;
    return q;
}

static void finalise_score(ComboResult& r) noexcept {
    if (r.degenerate) {
        r.stddev_q  = 0.0;
        r.stability = 0.0;
        r.score     = -std::numeric_limits<double>::infinity();
        return;
    }
    const double mean = 0.25 * (r.q_pnl[0] + r.q_pnl[1] + r.q_pnl[2] + r.q_pnl[3]);
    double var = 0.0;
    for (int i = 0; i < 4; ++i) {
        const double d = r.q_pnl[i] - mean;
        var += d * d;
    }
    var /= 4.0;
    r.stddev_q  = std::sqrt(var);
    r.stability = 1.0 / (1.0 + r.stddev_q);
    r.score     = r.stability * r.total_pnl;
}

struct ComboSink {
    ComboResult* out          = nullptr;
    int64_t      cur_idx      = 0;
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
// Snapshot builder (same as OmegaSweepHarness.cpp, but typed against
// sweep_crtp::GoldSnapshot rather than sweep::GoldSnapshot).
// =============================================================================
class SnapshotBuilder {
    double vwap_num_ = 0.0;
    double vwap_den_ = 0.0;
    int    vwap_day_ = -1;

    static constexpr int VOL_N = 64;
    double vol_buf_[VOL_N] = {};
    int    vol_h_ = 0, vol_c_ = 0;

    double prev_mid_ = 0.0;

    static sc::SessionType session_for_hour(int h) noexcept {
        if (h >= 0 && h <  7) return sc::SessionType::ASIAN;
        if (h >= 7 && h < 12) return sc::SessionType::LONDON;
        if (h >= 12 && h < 16) return sc::SessionType::OVERLAP;
        if (h >= 16 && h < 21) return sc::SessionType::NEWYORK;
        return sc::SessionType::UNKNOWN;
    }

public:
    void update(sc::GoldSnapshot& snap,
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

        if (yday != vwap_day_) {
            vwap_num_ = 0.0; vwap_den_ = 0.0;
            vwap_day_ = yday;
        }
        vwap_num_ += snap.mid;
        vwap_den_ += 1.0;
        snap.vwap = (vwap_den_ > 0.0) ? (vwap_num_ / vwap_den_) : snap.mid;

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
        snap.dx_mid = 0.0;
    }
};

// =============================================================================
// Per-engine type aliases. Each is a tuple of N_COMBOS distinct CRTP
// derived classes -- one per combo index. The tuple element type for combo
// I is EngineX_CRTP<EngineX_Traits<I>>.
// =============================================================================
constexpr int N_COMBOS = sc::N_COMBOS;  // 490

// HBG ------------------------------------------------------------------------
template <std::size_t I>
using HBG_AT = sc::HBG_CRTP<sc::HBGTraits<I>>;

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

static void hbg_params_for(int I, double out[5]) noexcept {
    out[0] = sc::HBGBaseParams::MIN_RANGE     * sc::mult_for_param_crtp(I, 0);
    out[1] = sc::HBGBaseParams::MAX_RANGE     * sc::mult_for_param_crtp(I, 1);
    out[2] = sc::HBGBaseParams::SL_FRAC       * sc::mult_for_param_crtp(I, 2);
    out[3] = sc::HBGBaseParams::TP_RR         * sc::mult_for_param_crtp(I, 3);
    out[4] = sc::HBGBaseParams::MFE_LOCK_FRAC * sc::mult_for_param_crtp(I, 4);
}

// EMACross -------------------------------------------------------------------
template <std::size_t I>
using EMA_AT = sc::EMACrossCRTP<sc::EMACrossTraits<I>>;

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
    const int fast = static_cast<int>(static_cast<double>(sc::EMACrossBaseParams::FAST_PERIOD) * sc::mult_for_param_crtp(I, 0) + 0.5);
    const int slow = static_cast<int>(static_cast<double>(sc::EMACrossBaseParams::SLOW_PERIOD) * sc::mult_for_param_crtp(I, 1) + 0.5);
    out[0] = static_cast<double>(fast);
    out[1] = static_cast<double>(slow);
    out[2] = sc::EMACrossBaseParams::RSI_LO  * sc::mult_for_param_crtp(I, 2);
    out[3] = sc::EMACrossBaseParams::RSI_HI  * sc::mult_for_param_crtp(I, 3);
    out[4] = sc::EMACrossBaseParams::SL_MULT * sc::mult_for_param_crtp(I, 4);
}

// AsianRange -----------------------------------------------------------------
template <std::size_t I>
using ASIAN_AT = sc::AsianRangeCRTP<sc::AsianRangeTraits<I>>;

template <std::size_t... I>
auto make_asian_tuple_impl(std::index_sequence<I...>) {
    return std::tuple<ASIAN_AT<I>...>{};
}
inline auto make_asian_tuple() {
    return make_asian_tuple_impl(std::make_index_sequence<N_COMBOS>{});
}

static void asian_params_for(int I, double out[5]) noexcept {
    out[0] = sc::AsianRangeBaseParams::BUFFER    * sc::mult_for_param_crtp(I, 0);
    out[1] = sc::AsianRangeBaseParams::MIN_RANGE * sc::mult_for_param_crtp(I, 1);
    out[2] = sc::AsianRangeBaseParams::MAX_RANGE * sc::mult_for_param_crtp(I, 2);
    const int sl = static_cast<int>(
        static_cast<double>(sc::AsianRangeBaseParams::SL_TICKS) * sc::mult_for_param_crtp(I, 3) + 0.5);
    const int tp = static_cast<int>(
        static_cast<double>(sc::AsianRangeBaseParams::TP_TICKS) * sc::mult_for_param_crtp(I, 4) + 0.5);
    out[3] = static_cast<double>(sl);
    out[4] = static_cast<double>(tp);
}

// VWAPStretch ----------------------------------------------------------------
template <std::size_t I>
using VWAP_AT = sc::VWAPStretchCRTP<sc::VWAPStretchTraits<I>>;

template <std::size_t... I>
auto make_vwap_tuple_impl(std::index_sequence<I...>) {
    return std::tuple<VWAP_AT<I>...>{};
}
inline auto make_vwap_tuple() {
    return make_vwap_tuple_impl(std::make_index_sequence<N_COMBOS>{});
}

static void vwap_params_for(int I, double out[5]) noexcept {
    const int sl = static_cast<int>(
        static_cast<double>(sc::VWAPStretchBaseParams::SL_TICKS)     * sc::mult_for_param_crtp(I, 0) + 0.5);
    const int tp = static_cast<int>(
        static_cast<double>(sc::VWAPStretchBaseParams::TP_TICKS)     * sc::mult_for_param_crtp(I, 1) + 0.5);
    const int cd = static_cast<int>(
        static_cast<double>(sc::VWAPStretchBaseParams::COOLDOWN_SEC) * sc::mult_for_param_crtp(I, 2) + 0.5);
    const int vw = static_cast<int>(
        static_cast<double>(sc::VWAPStretchBaseParams::VOL_WINDOW)   * sc::mult_for_param_crtp(I, 4) + 0.5);
    out[0] = static_cast<double>(sl);
    out[1] = static_cast<double>(tp);
    out[2] = static_cast<double>(cd);
    out[3] = sc::VWAPStretchBaseParams::SIGMA_ENTRY * sc::mult_for_param_crtp(I, 3);
    out[4] = static_cast<double>(vw);
}

// =============================================================================
// HBG runner -- single thread, walks all ticks, fans into 490-tuple
// =============================================================================
static void run_hbg_sweep(const std::vector<TickRow>& ticks,
                          int64_t warmup_ticks,
                          std::vector<ComboResult>& results,
                          bool verbose) {
    using TupleT = decltype(make_hbg_tuple());
    auto engines_p = std::make_unique<TupleT>();
    auto& engines  = *engines_p;
    auto sinks_p   = std::make_unique<std::array<ComboSink, N_COMBOS>>();
    auto& sinks    = *sinks_p;
    results.assign(N_COMBOS, ComboResult{});

    const int64_t N = static_cast<int64_t>(ticks.size());

    for (int i = 0; i < N_COMBOS; ++i) {
        results[i].combo_id   = i;
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

        for (int i = 0; i < N_COMBOS; ++i) sinks[i].cur_idx = k;

        hbg_run_tick(engines, sinks, r.bid, r.ask, r.ts_ms);

        if (verbose && (k % progress_step == 0)) {
            std::printf("  HBG-CRTP %5.1f%%  (%" PRId64 "/%" PRId64 ")\n",
                        100.0 * static_cast<double>(k) / static_cast<double>(N),
                        k, N);
            std::fflush(stdout);
        }
    }

    for (auto& res : results) finalise_score(res);
}

// =============================================================================
// EMACross runner -- bar-driven plus tick-driven
// =============================================================================
static void run_ema_sweep(const std::vector<TickRow>& ticks,
                          int64_t warmup_ticks,
                          std::vector<ComboResult>& results,
                          bool verbose) {
    using TupleT = decltype(make_ema_tuple());
    auto engines_p = std::make_unique<TupleT>();
    auto& engines  = *engines_p;
    auto sinks_p   = std::make_unique<std::array<ComboSink, N_COMBOS>>();
    auto& sinks    = *sinks_p;
    results.assign(N_COMBOS, ComboResult{});

    const int64_t N = static_cast<int64_t>(ticks.size());

    for (int i = 0; i < N_COMBOS; ++i) {
        results[i].combo_id   = i;
        ema_params_for(i, results[i].p);
        results[i].degenerate = (results[i].p[0] >= results[i].p[1]);
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
            std::printf("  EMA-CRTP %5.1f%%  (%" PRId64 "/%" PRId64 ")\n",
                        100.0 * static_cast<double>(k) / static_cast<double>(N),
                        k, N);
            std::fflush(stdout);
        }
    }

    for (auto& res : results) finalise_score(res);
}

// =============================================================================
// Signal-emitter runner shared shape (AsianRange, VWAPStretch)
// =============================================================================
struct ManagedPos {
    bool   active   = false;
    bool   is_long  = false;
    double entry    = 0.0;
    double sl       = 0.0;
    double tp       = 0.0;
    int64_t entry_ts = 0;
};

static inline double tick_to_price(double ticks) noexcept {
    return ticks * 0.01;  // XAUUSD: 1 tick = 0.01 price units
}

static void close_managed(ComboResult& out, ManagedPos& pos,
                          double exit_px, int64_t now_s, int64_t cur_idx,
                          int64_t warmup_ticks, int64_t total_ticks) noexcept {
    if (!pos.active) return;
    const double pnl_pts = pos.is_long ? (exit_px - pos.entry)
                                       : (pos.entry - exit_px);
    const double pnl = pnl_pts * 0.01;
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
                                std::array<sc::Signal, N_COMBOS>& sigs,
                                const sc::GoldSnapshot& snap,
                                std::index_sequence<I...>) {
    ((sigs[I] = std::get<I>(tup).process(snap)), ...);
}
template <typename EngineTup>
static void asian_run_tick(EngineTup& tup,
                           std::array<sc::Signal, N_COMBOS>& sigs,
                           const sc::GoldSnapshot& snap) {
    asian_run_tick_impl(tup, sigs, snap, std::make_index_sequence<N_COMBOS>{});
}

static void run_asian_sweep(const std::vector<TickRow>& ticks,
                            int64_t warmup_ticks,
                            std::vector<ComboResult>& results,
                            bool verbose) {
    using TupleT = decltype(make_asian_tuple());
    auto engines_p   = std::make_unique<TupleT>();
    auto& engines    = *engines_p;
    auto positions_p = std::make_unique<std::array<ManagedPos, N_COMBOS>>();
    auto& positions  = *positions_p;
    auto sigs_p      = std::make_unique<std::array<sc::Signal, N_COMBOS>>();
    auto& sigs       = *sigs_p;
    results.assign(N_COMBOS, ComboResult{});

    const int64_t N = static_cast<int64_t>(ticks.size());

    for (int i = 0; i < N_COMBOS; ++i) {
        results[i].combo_id = i;
        asian_params_for(i, results[i].p);
    }

    SnapshotBuilder sb;
    sc::GoldSnapshot snap;
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
            const double sl_dist = tick_to_price(results[i].p[3]);
            const double tp_dist = tick_to_price(results[i].p[4]);
            const double tp_px = p.is_long ? (p.entry + tp_dist) : (p.entry - tp_dist);
            const double sl_px = p.is_long ? (p.entry - sl_dist) : (p.entry + sl_dist);
            const bool tp_hit = p.is_long ? (r.ask >= tp_px) : (r.bid <= tp_px);
            const bool sl_hit = p.is_long ? (r.bid <= sl_px) : (r.ask >= sl_px);
            if      (tp_hit) close_managed(results[i], p, tp_px, now_s, k, warmup_ticks, N);
            else if (sl_hit) close_managed(results[i], p, sl_px, now_s, k, warmup_ticks, N);
        }

        asian_run_tick(engines, sigs, snap);
        for (int i = 0; i < N_COMBOS; ++i) {
            const sc::Signal& s = sigs[i];
            if (!s.valid || positions[i].active) continue;
            ManagedPos& p = positions[i];
            p.active   = true;
            p.is_long  = (s.side == sc::TradeSide::LONG);
            p.entry    = s.entry;
            p.entry_ts = now_s;
        }

        if (verbose && (k % progress_step == 0)) {
            std::printf("  ASN-CRTP %5.1f%%  (%" PRId64 "/%" PRId64 ")\n",
                        100.0 * static_cast<double>(k) / static_cast<double>(N),
                        k, N);
            std::fflush(stdout);
        }
    }

    for (auto& res : results) finalise_score(res);
}

template <typename EngineTup, std::size_t... I>
static void vwap_run_tick_impl(EngineTup& tup,
                               std::array<sc::Signal, N_COMBOS>& sigs,
                               const sc::GoldSnapshot& snap,
                               std::index_sequence<I...>) {
    ((sigs[I] = std::get<I>(tup).process(snap)), ...);
}
template <typename EngineTup>
static void vwap_run_tick(EngineTup& tup,
                          std::array<sc::Signal, N_COMBOS>& sigs,
                          const sc::GoldSnapshot& snap) {
    vwap_run_tick_impl(tup, sigs, snap, std::make_index_sequence<N_COMBOS>{});
}

static void run_vwap_sweep(const std::vector<TickRow>& ticks,
                           int64_t warmup_ticks,
                           std::vector<ComboResult>& results,
                           bool verbose) {
    using TupleT = decltype(make_vwap_tuple());
    auto engines_p   = std::make_unique<TupleT>();
    auto& engines    = *engines_p;
    auto positions_p = std::make_unique<std::array<ManagedPos, N_COMBOS>>();
    auto& positions  = *positions_p;
    auto sigs_p      = std::make_unique<std::array<sc::Signal, N_COMBOS>>();
    auto& sigs       = *sigs_p;
    results.assign(N_COMBOS, ComboResult{});

    const int64_t N = static_cast<int64_t>(ticks.size());

    for (int i = 0; i < N_COMBOS; ++i) {
        results[i].combo_id = i;
        vwap_params_for(i, results[i].p);
    }

    SnapshotBuilder sb;
    sc::GoldSnapshot snap;
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
            if      (tp_hit) close_managed(results[i], p, tp_px, now_s, k, warmup_ticks, N);
            else if (sl_hit) close_managed(results[i], p, sl_px, now_s, k, warmup_ticks, N);
        }

        vwap_run_tick(engines, sigs, snap);
        for (int i = 0; i < N_COMBOS; ++i) {
            const sc::Signal& s = sigs[i];
            if (!s.valid || positions[i].active) continue;
            ManagedPos& p = positions[i];
            p.active   = true;
            p.is_long  = (s.side == sc::TradeSide::LONG);
            p.entry    = s.entry;
            p.entry_ts = now_s;
        }

        if (verbose && (k % progress_step == 0)) {
            std::printf("  VWP-CRTP %5.1f%%  (%" PRId64 "/%" PRId64 ")\n",
                        100.0 * static_cast<double>(k) / static_cast<double>(N),
                        k, N);
            std::fflush(stdout);
        }
    }

    for (auto& res : results) finalise_score(res);
}

// =============================================================================
// CSV output (identical schema to OmegaSweepHarness.cpp)
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
                    "stddev_q,stability,score,degenerate\n",
                 param_names[0], param_names[1], param_names[2],
                 param_names[3], param_names[4]);
    for (const auto& r : results) {
        const double wr = r.n_trades > 0
            ? 100.0 * static_cast<double>(r.wins) / static_cast<double>(r.n_trades)
            : 0.0;
        if (r.degenerate) {
            std::fprintf(f, "%d,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d,%.4f,%.4f,"
                            "%.4f,%.4f,%.4f,%.4f,%d,%d,%d,%d,"
                            "%.6f,%.6f,%s,%d\n",
                         r.combo_id,
                         r.p[0], r.p[1], r.p[2], r.p[3], r.p[4],
                         r.n_trades, r.wins, wr, r.total_pnl,
                         r.q_pnl[0], r.q_pnl[1], r.q_pnl[2], r.q_pnl[3],
                         r.q_trades[0], r.q_trades[1], r.q_trades[2], r.q_trades[3],
                         r.stddev_q, r.stability, "-1e308", 1);
        } else {
            std::fprintf(f, "%d,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d,%.4f,%.4f,"
                            "%.4f,%.4f,%.4f,%.4f,%d,%d,%d,%d,"
                            "%.6f,%.6f,%.6f,%d\n",
                         r.combo_id,
                         r.p[0], r.p[1], r.p[2], r.p[3], r.p[4],
                         r.n_trades, r.wins, wr, r.total_pnl,
                         r.q_pnl[0], r.q_pnl[1], r.q_pnl[2], r.q_pnl[3],
                         r.q_trades[0], r.q_trades[1], r.q_trades[2], r.q_trades[3],
                         r.stddev_q, r.stability, r.score, 0);
        }
    }
    std::fclose(f);
}

static void append_summary(FILE* sf, const char* engine_name,
                           const std::vector<ComboResult>& results,
                           const std::array<const char*, 5>& param_names) {
    std::vector<ComboResult> ranked;
    ranked.reserve(results.size());
    int n_degen = 0;
    for (const auto& r : results) {
        if (r.degenerate) { ++n_degen; continue; }
        ranked.push_back(r);
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const ComboResult& a, const ComboResult& b){
                  return a.score > b.score;
              });
    std::fprintf(sf, "\n========== %s top-50 by score ==========\n", engine_name);
    if (n_degen > 0) {
        std::fprintf(sf, "(filtered %d degenerate combos; %zu evaluated)\n",
                     n_degen, ranked.size());
    }
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
// Args
// =============================================================================
struct Args {
    std::string ticks_path;
    std::string engines = "hbg,asianrange,vwapstretch,emacross";
    std::string outdir  = "sweep_results_crtp";
    int64_t     warmup  = 5000;
    int64_t     from_ms = 0;
    int64_t     to_ms   = 0;
    bool        verbose = false;
    bool        no_selftest = false;
};

static void usage() {
    std::printf(
        "OmegaSweepHarnessCRTP <ticks.csv> [options]\n"
        "  --engine <list>   comma list (default: hbg,asianrange,vwapstretch,emacross)\n"
        "                    Available: hbg, asianrange, vwapstretch, emacross\n"
        "  --outdir <dir>    output directory (default: sweep_results_crtp)\n"
        "  --warmup <n>      ticks to skip before recording trades (default: 5000)\n"
        "  --from-date <d>   skip ticks before YYYY-MM-DD\n"
        "  --to-date <d>     skip ticks at/after YYYY-MM-DD\n"
        "  --verbose         print per-engine progress\n"
        "  --no-selftest     skip the G2 determinism self-test at startup\n"
    );
}

static bool parse_args(int argc, char** argv, Args& a) {
    if (argc < 2) { usage(); return false; }
    a.ticks_path = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string s = argv[i];
        if      (s == "--engine"    && i+1 < argc) { a.engines = argv[++i]; }
        else if (s == "--outdir"    && i+1 < argc) { a.outdir  = argv[++i]; }
        else if (s == "--warmup"    && i+1 < argc) { a.warmup  = std::atoll(argv[++i]); }
        else if (s == "--from-date" && i+1 < argc) { a.from_ms = parse_date_arg(argv[++i]); }
        else if (s == "--to-date"   && i+1 < argc) { a.to_ms   = parse_date_arg(argv[++i]); }
        else if (s == "--verbose")        { a.verbose     = true; }
        else if (s == "--no-selftest")    { a.no_selftest = true; }
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
// G2 -- Determinism self-test (same protocol as OmegaSweepHarness.cpp)
// =============================================================================
static constexpr int64_t SELFTEST_TICKS = 20000;

static int combo_fingerprint_diff(const ComboResult& a,
                                  const ComboResult& b) noexcept {
    if (a.n_trades != b.n_trades) return 1;
    if (a.wins     != b.wins)     return 2;
    if (std::memcmp(&a.total_pnl, &b.total_pnl, sizeof(double)) != 0) return 3;
    if (std::memcmp(a.q_pnl,    b.q_pnl,    sizeof(a.q_pnl))    != 0) return 4;
    if (std::memcmp(a.q_trades, b.q_trades, sizeof(a.q_trades)) != 0) return 5;
    if (std::memcmp(&a.stddev_q, &b.stddev_q, sizeof(double)) != 0) return 6;
    if (std::memcmp(&a.score,    &b.score,    sizeof(double)) != 0) return 7;
    return 0;
}

static const char* fingerprint_field_name(int diff) noexcept {
    switch (diff) {
        case 1: return "n_trades";
        case 2: return "wins";
        case 3: return "total_pnl";
        case 4: return "q_pnl[]";
        case 5: return "q_trades[]";
        case 6: return "stddev_q";
        case 7: return "score";
        default: return "(none)";
    }
}

static void print_fingerprint_mismatch(const char* engine,
                                       int combo_id,
                                       int diff,
                                       const ComboResult& a,
                                       const ComboResult& b) {
    std::fprintf(stderr,
        "DETERMINISM FAULT: %s combo %d produced different fingerprints across\n"
        "                   the two G2 self-test runs. Diverging field: %s\n",
        engine, combo_id, fingerprint_field_name(diff));
    std::fprintf(stderr,
        "  run1: n_trades=%d wins=%d total_pnl=%.10f stddev_q=%.10f score=%.10f\n",
        a.n_trades, a.wins, a.total_pnl, a.stddev_q, a.score);
    std::fprintf(stderr,
        "  run2: n_trades=%d wins=%d total_pnl=%.10f stddev_q=%.10f score=%.10f\n",
        b.n_trades, b.wins, b.total_pnl, b.stddev_q, b.score);
    std::fprintf(stderr,
        "  run1 q_pnl=[%.6f,%.6f,%.6f,%.6f] q_trades=[%d,%d,%d,%d]\n",
        a.q_pnl[0], a.q_pnl[1], a.q_pnl[2], a.q_pnl[3],
        a.q_trades[0], a.q_trades[1], a.q_trades[2], a.q_trades[3]);
    std::fprintf(stderr,
        "  run2 q_pnl=[%.6f,%.6f,%.6f,%.6f] q_trades=[%d,%d,%d,%d]\n",
        b.q_pnl[0], b.q_pnl[1], b.q_pnl[2], b.q_pnl[3],
        b.q_trades[0], b.q_trades[1], b.q_trades[2], b.q_trades[3]);
    std::fprintf(stderr,
        "  This indicates a harness bug. Real sweep results cannot be trusted. Aborting.\n");
    std::fflush(stderr);
}

static bool compare_results(const char* engine,
                            const std::vector<ComboResult>& r1,
                            const std::vector<ComboResult>& r2) {
    if (r1.size() != r2.size()) {
        std::fprintf(stderr,
            "DETERMINISM FAULT: %s produced different combo counts across\n"
            "                   G2 runs (%zu vs %zu). Aborting.\n",
            engine, r1.size(), r2.size());
        std::fflush(stderr);
        return false;
    }
    for (std::size_t i = 0; i < r1.size(); ++i) {
        int diff = combo_fingerprint_diff(r1[i], r2[i]);
        if (diff != 0) {
            print_fingerprint_mismatch(engine,
                                       static_cast<int>(i), diff,
                                       r1[i], r2[i]);
            return false;
        }
    }
    return true;
}

static bool run_selftest_determinism(const std::vector<TickRow>& ticks,
                                     const Args& args) {
    if (ticks.empty()) {
        std::fprintf(stderr, "G2 self-test: no ticks; skipping.\n");
        return true;
    }

    const int64_t n = std::min<int64_t>(SELFTEST_TICKS,
                                        static_cast<int64_t>(ticks.size()));
    std::vector<TickRow> sub(ticks.begin(), ticks.begin() + n);

    const int64_t st_warmup = std::min<int64_t>(args.warmup, n / 4);

    auto t0 = std::chrono::steady_clock::now();
    std::printf("G2 self-test (CRTP): running each requested engine TWICE on %"
                PRId64 " ticks (warmup=%" PRId64 ") to verify determinism\n",
                n, st_warmup);
    std::fflush(stdout);

    std::vector<ComboResult> hbg_r1, hbg_r2;
    std::vector<ComboResult> ema_r1, ema_r2;
    std::vector<ComboResult> asn_r1, asn_r2;
    std::vector<ComboResult> vwp_r1, vwp_r2;

    const bool want_hbg = engine_in_list(args.engines, "hbg");
    const bool want_ema = engine_in_list(args.engines, "emacross");
    const bool want_asn = engine_in_list(args.engines, "asianrange");
    const bool want_vwp = engine_in_list(args.engines, "vwapstretch");

    {
        std::vector<std::thread> threads;
        if (want_hbg) threads.emplace_back([&]() { run_hbg_sweep  (sub, st_warmup, hbg_r1, false); });
        if (want_ema) threads.emplace_back([&]() { run_ema_sweep  (sub, st_warmup, ema_r1, false); });
        if (want_asn) threads.emplace_back([&]() { run_asian_sweep(sub, st_warmup, asn_r1, false); });
        if (want_vwp) threads.emplace_back([&]() { run_vwap_sweep (sub, st_warmup, vwp_r1, false); });
        for (auto& th : threads) th.join();
    }

    {
        std::vector<std::thread> threads;
        if (want_hbg) threads.emplace_back([&]() { run_hbg_sweep  (sub, st_warmup, hbg_r2, false); });
        if (want_ema) threads.emplace_back([&]() { run_ema_sweep  (sub, st_warmup, ema_r2, false); });
        if (want_asn) threads.emplace_back([&]() { run_asian_sweep(sub, st_warmup, asn_r2, false); });
        if (want_vwp) threads.emplace_back([&]() { run_vwap_sweep (sub, st_warmup, vwp_r2, false); });
        for (auto& th : threads) th.join();
    }

    bool ok = true;
    if (want_hbg) ok = compare_results("HBG-CRTP",         hbg_r1, hbg_r2) && ok;
    if (want_ema) ok = compare_results("EMACross-CRTP",    ema_r1, ema_r2) && ok;
    if (want_asn) ok = compare_results("AsianRange-CRTP",  asn_r1, asn_r2) && ok;
    if (want_vwp) ok = compare_results("VWAPStretch-CRTP", vwp_r1, vwp_r2) && ok;

    auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    if (ok) {
        std::printf("G2 self-test (CRTP): PASS in %.2fs (engines=%s%s%s%s)\n",
                    sec,
                    want_hbg ? "HBG " : "",
                    want_ema ? "EMA " : "",
                    want_asn ? "ASN " : "",
                    want_vwp ? "VWP" : "");
        std::fflush(stdout);
    }
    return ok;
}

// =============================================================================
// Directory creation
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
// main()
// =============================================================================
int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) return 1;

    std::printf("Omega Sweep Harness CRTP (S51 1A.1.b post-G4, pairwise 2-factor, 490 combos/engine)\n");
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

    if (!args.no_selftest) {
        if (!run_selftest_determinism(ticks, args)) {
            std::fprintf(stderr, "G2 self-test: FAIL -- aborting before real sweep.\n");
            std::fflush(stderr);
            return 2;
        }
    } else {
        std::printf("G2 self-test: SKIPPED (--no-selftest)\n");
        std::fflush(stdout);
    }

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

    launch("hbg",         [&]() { run_hbg_sweep  (ticks, args.warmup, hbg_res,   args.verbose); });
    launch("emacross",    [&]() { run_ema_sweep  (ticks, args.warmup, ema_res,   args.verbose); });
    launch("asianrange",  [&]() { run_asian_sweep(ticks, args.warmup, asian_res, args.verbose); });
    launch("vwapstretch", [&]() { run_vwap_sweep (ticks, args.warmup, vwap_res,  args.verbose); });

    for (auto& th : threads) th.join();

    auto t_end = std::chrono::steady_clock::now();
    const double total_sec = std::chrono::duration<double>(t_end - t_start).count();
    std::printf("all sweeps complete in %.1fs (%.1f min)\n",
                total_sec, total_sec / 60.0);

    const std::string summary_path = args.outdir + "/sweep_summary.txt";
    FILE* sf = std::fopen(summary_path.c_str(), "w");
    if (!sf) {
        std::fprintf(stderr, "ERROR: cannot write %s\n", summary_path.c_str());
    } else {
        std::fprintf(sf, "Omega Sweep Harness CRTP -- pairwise 2-factor (490 combos/engine)\n");
        std::fprintf(sf, "ticks: %s\n", args.ticks_path.c_str());
        std::fprintf(sf, "n_ticks: %zu\n", ticks.size());
        std::fprintf(sf, "wall_time: %.1fs (%.1f min)\n", total_sec, total_sec / 60.0);
    }

    if (!hbg_res.empty()) {
        write_csv(args.outdir + "/sweep_hbg.csv", hbg_res,
                  {"min_range","max_range","sl_frac","tp_rr","mfe_lock_frac"});
        if (sf) append_summary(sf, "HBG-CRTP", hbg_res,
                               {"min_range","max_range","sl_frac","tp_rr","mfe_lock_frac"});
        std::printf("wrote sweep_hbg.csv (%zu combos)\n", hbg_res.size());
    }
    if (!ema_res.empty()) {
        write_csv(args.outdir + "/sweep_emacross.csv", ema_res,
                  {"fast_period","slow_period","rsi_lo","rsi_hi","sl_mult"});
        if (sf) append_summary(sf, "EMACross-CRTP", ema_res,
                               {"fast_period","slow_period","rsi_lo","rsi_hi","sl_mult"});
        std::printf("wrote sweep_emacross.csv (%zu combos)\n", ema_res.size());
    }
    if (!asian_res.empty()) {
        write_csv(args.outdir + "/sweep_asianrange.csv", asian_res,
                  {"buffer","min_range","max_range","sl_ticks","tp_ticks"});
        if (sf) append_summary(sf, "AsianRange-CRTP", asian_res,
                               {"buffer","min_range","max_range","sl_ticks","tp_ticks"});
        std::printf("wrote sweep_asianrange.csv (%zu combos)\n", asian_res.size());
    }
    if (!vwap_res.empty()) {
        write_csv(args.outdir + "/sweep_vwapstretch.csv", vwap_res,
                  {"sl_ticks","tp_ticks","cooldown_sec","sigma_entry","vol_window"});
        if (sf) append_summary(sf, "VWAPStretch-CRTP", vwap_res,
                               {"sl_ticks","tp_ticks","cooldown_sec","sigma_entry","vol_window"});
        std::printf("wrote sweep_vwapstretch.csv (%zu combos)\n", vwap_res.size());
    }

    if (sf) std::fclose(sf);
    std::printf("summary: %s\n", summary_path.c_str());
    return 0;
}

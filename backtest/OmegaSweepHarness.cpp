// =============================================================================
// OmegaSweepHarness.cpp -- Parameter-sweep harness for SweepableEngines.hpp
// =============================================================================
//
// PURPOSE
//   Run a 5-parameter geometric grid sweep (0.5x..2.0x of default, 7 values
//   per param -> 16,807 combos per engine) for HBG, AsianRange, VWAPStretch,
//   and EMACross, ranked by stability-weighted PnL. DXYDivergence is wired
//   but skipped at the run-list level because no DXY tick stream is wired
//   into the backtest input.
//
// AUTHORITY
//   Created S51 X2 (2026-04-27) under explicit user authorisation. Only the
//   sweep harness; live engines untouched. Builds as a separate CMake target
//   so existing OmegaBacktest behaviour is unaffected.
//
// DESIGN
//   * mmap the tick CSV once (reuses the OmegaBacktest mmap pattern)
//   * Single tick decoder: same parser as OmegaBacktest.cpp
//   * For each engine in priority order:
//       - Generate the 7^5 = 16807 parameter combinations
//       - Spawn N worker threads (default = hardware concurrency)
//       - Each worker pulls a combo, instantiates the engine, replays all
//         ticks against it, records aggregate stats + per-quarter PnL for
//         stability scoring
//       - All threads share the mmap'd tick stream (read-only; OS page-share)
//       - Per-engine result CSV: combo_id, p1..p5, n_trades, win_rate,
//         total_pnl, q1..q4_pnl, stddev, stability, score
//   * Final ranking by score = stability * total_pnl (where stability =
//     1.0 / (1.0 + stddev_across_quarters))
//
// USAGE
//   OmegaSweepHarness <ticks.csv> [options]
//     --engine <list>   comma list (default: hbg,asianrange,vwapstretch,emacross)
//                       Available: hbg, asianrange, vwapstretch, emacross, dxy
//                       NOTE: dxy will produce zero trades without a DXY feed.
//     --threads <n>     worker threads (default: hw concurrency)
//     --outdir <dir>    output directory (default: sweep_results)
//     --warmup <n>      ticks to skip before recording trades (default: 5000)
//     --verbose         print per-combo progress
//
// OUTPUT
//   <outdir>/sweep_<engine>.csv   one CSV per engine with all combos ranked
//   <outdir>/sweep_summary.txt    human-readable top-50 per engine
//
// =============================================================================

#include "OmegaTimeShim.hpp"
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

// =============================================================================
// Tick row + parser (subset of OmegaBacktest -- only what the harness uses)
// =============================================================================
struct TickRow { int64_t ts_ms; double bid; double ask; };

enum class Fmt { TS_BA, TS_OHLCV, DUKA };
static Fmt sniff_format(const char* p, const char* end) {
    // Count commas on the first data line
    const char* q = p;
    int commas = 0;
    while (q < end && *q != '\n') { if (*q == ',') ++commas; ++q; }
    // DUKA is YYYY.MM.DD,HH:MM:SS.mmm,bid,ask,vol -> first field has dots
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

static std::vector<TickRow> parse_csv(const MemMappedFile& f) {
    std::vector<TickRow> v;
    v.reserve(static_cast<size_t>(130'000'000));
    const char* p   = f.data;
    const char* end = p + f.size;
    if (f.size>=3 && (uint8_t)p[0]==0xEF && (uint8_t)p[1]==0xBB && (uint8_t)p[2]==0xBF) p+=3;

    bool ask_first = false;
    if (p < end && (*p < '0' || *p > '9')) {
        // Header line present -- detect column order
        const char* ask_pos = nullptr;
        const char* bid_pos = nullptr;
        while (p < end && *p != '\n') {
            if (!ask_pos && (p[0]=='a'||p[0]=='A') && (p[1]=='s'||p[1]=='S') && (p[2]=='k'||p[2]=='K')) ask_pos=p;
            if (!bid_pos && (p[0]=='b'||p[0]=='B') && (p[1]=='i'||p[1]=='I') && (p[2]=='d'||p[2]=='D')) bid_pos=p;
            ++p;
        }
        if (ask_pos && bid_pos && ask_pos < bid_pos) ask_first = true;
        if (p < end) ++p;
    }
    if (ask_first) fprintf(stderr, "[CSV] ask-first format detected -- swapping columns\n");

    Fmt fmt = sniff_format(p, end);
    fprintf(stderr, "[CSV] format = %s\n",
            fmt==Fmt::DUKA ? "DUKA" : fmt==Fmt::TS_OHLCV ? "TS_OHLCV" : "TS_BA");

    while (p < end) {
        while (p < end && (*p == '\r' || *p == '\n')) ++p;
        if (p >= end) break;
        TickRow r{0, 0.0, 0.0};
        const char* nx;
        if (fmt == Fmt::DUKA) {
            const char* dp = p;
            while (p < end && *p != ',') ++p;
            if (p >= end) break;
            ++p;
            const char* tp = p;
            while (p < end && *p != ',') ++p;
            if (p >= end) break;
            ++p;
            r.ts_ms = duka_ts(dp, tp);
            r.bid = fast_f(p, &nx); p = nx; if (p < end && *p == ',') ++p;
            r.ask = fast_f(p, &nx); p = nx;
        } else if (fmt == Fmt::TS_OHLCV) {
            r.ts_ms = fast_i64(p, &nx); p = nx; if (p < end && *p == ',') ++p;
            fast_f(p, &nx); p = nx; if (p < end && *p == ',') ++p;
            fast_f(p, &nx); p = nx; if (p < end && *p == ',') ++p;
            fast_f(p, &nx); p = nx; if (p < end && *p == ',') ++p;
            double cl = fast_f(p, &nx); p = nx;
            r.bid = cl - 0.15; r.ask = cl + 0.15;
        } else {
            r.ts_ms = fast_i64(p, &nx); p = nx; if (p < end && *p == ',') ++p;
            double c1 = fast_f(p, &nx); p = nx; if (p < end && *p == ',') ++p;
            double c2 = fast_f(p, &nx); p = nx;
            if (ask_first) { r.ask = c1; r.bid = c2; } else { r.bid = c1; r.ask = c2; }
        }
        while (p < end && *p != '\n') ++p;
        if (r.ts_ms > 0 && r.bid > 0.0 && r.ask > r.bid) v.push_back(r);
    }
    return v;
}

// =============================================================================
// Snapshot builder -- mirrors GoldEngineStack::update minus regime/sweep
//
// Because the harness drives engines INDIVIDUALLY (not via GoldEngineStack),
// it has to compute its own VWAP, volatility, session, and trend. This
// implementation is byte-faithful to GoldEngineStack::update() lines 190-231:
//   - VWAP: spread-weighted, daily-reset at UTC yday boundary
//   - volatility: stddev of price_window (last N mids)
//   - session: classify_session() UTC hour buckets
//   - trend: 0.95*trend + 0.05*(mid - vwap)
//
// dx_mid is left at 0 -- DXY engines skip out at the s.dx_mid<=0 check.
// supervisor_regime is left empty -- DXY engines treat empty != trending.
// =============================================================================
struct SnapshotBuilder {
    // VWAP
    double cum_pv_ = 0.0, cum_vol_ = 0.0, vwap_ = 0.0;
    int last_reset_yday_ = -1;
    // Volatility
    std::vector<double> price_window_;
    double last_price_ = 0.0;
    double tick_vol_ = 0.0;  // unused by sweep engines but kept for parity
    // Trend
    double trend_ = 0.0;
    // Sweep size
    double sweep_hi_ = 0.0, sweep_lo_ = 0.0;
    int64_t tick_counter_ = 0;

    static constexpr size_t PRICE_WINDOW_CAP = 50;

    static omega::sweep::SessionType session_for_ts(int64_t ts_ms) noexcept {
        const time_t t = static_cast<time_t>(ts_ms / 1000);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        int mins = ti.tm_hour * 60 + ti.tm_min;
        if (mins >= 420 && mins < 630)  return omega::sweep::SessionType::LONDON;   // 07:00-10:30
        if (mins >= 630 && mins < 780)  return omega::sweep::SessionType::OVERLAP;  // 10:30-13:00
        if (mins >= 780 && mins < 1080) return omega::sweep::SessionType::NEWYORK;  // 13:00-18:00
        return omega::sweep::SessionType::ASIAN;
    }

    static int yday_for_ts(int64_t ts_ms) noexcept {
        const time_t t = static_cast<time_t>(ts_ms / 1000);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        return ti.tm_yday;
    }

    void update(omega::sweep::GoldSnapshot& snap, double bid, double ask, int64_t ts_ms) {
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // Daily VWAP reset
        const int yday = yday_for_ts(ts_ms);
        if (yday != last_reset_yday_) {
            cum_pv_ = 0.0; cum_vol_ = 0.0; vwap_ = 0.0;
            last_reset_yday_ = yday;
        }

        // VWAP -- spread-weighted (tight spread ticks count more)
        const double w = (spread > 1e-10) ? (1.0 / spread) : 1.0;
        cum_pv_  += mid * w;
        cum_vol_ += w;
        vwap_ = (cum_vol_ > 0) ? cum_pv_ / cum_vol_ : mid;

        // Tick-to-tick volatility (EWM, parity with live)
        if (last_price_ > 0.0) {
            const double move = std::fabs(mid - last_price_);
            tick_vol_ = 0.9 * tick_vol_ + 0.1 * move;
        }
        last_price_ = mid;

        // Price window for stddev
        price_window_.push_back(mid);
        if (price_window_.size() > PRICE_WINDOW_CAP)
            price_window_.erase(price_window_.begin());

        // Sweep size
        if (tick_counter_ == 0) { sweep_hi_ = mid; sweep_lo_ = mid; }
        if (tick_counter_++ % 50 == 0 && tick_counter_ > 1) {
            sweep_hi_ = mid; sweep_lo_ = mid;
        }
        if (mid > sweep_hi_) sweep_hi_ = mid;
        if (mid < sweep_lo_) sweep_lo_ = mid;
        snap.sweep_size = (mid > (sweep_hi_ + sweep_lo_) * 0.5)
            ? (mid - sweep_lo_) : -(sweep_hi_ - mid);

        // Trend
        trend_ = 0.95 * trend_ + 0.05 * (mid - vwap_);

        snap.bid     = bid;
        snap.ask     = ask;
        snap.mid     = mid;
        snap.spread  = spread;
        snap.vwap    = vwap_;
        snap.trend   = trend_;
        snap.session = session_for_ts(ts_ms);
        snap.dx_mid  = 0.0;       // no DXY feed wired
        snap.supervisor_regime = "";

        // Volatility (stddev of recent mids)
        if (price_window_.size() >= 2) {
            double sum = 0.0;
            for (double v : price_window_) sum += v;
            const double mean = sum / price_window_.size();
            double sq = 0.0;
            for (double v : price_window_) { double d = v - mean; sq += d*d; }
            const double sd = std::sqrt(sq / (price_window_.size() - 1));
            snap.volatility = (sd > 0.1) ? sd : 1.0;
        } else {
            snap.volatility = 1.0;
        }
    }
};

// =============================================================================
// Sweep grid -- 7 geometric values per param 0.5x..2.0x of default
// =============================================================================
static constexpr int N_GRID = 7;

static const double GRID_MULT[N_GRID] = {
    0.5000, 0.6300, 0.7937, 1.0000, 1.2599, 1.5874, 2.0000
};

// Snap a multiplied integer parameter to nearest int >= 1
static inline int snap_int(int base, double mult) noexcept {
    int v = static_cast<int>(std::round(base * mult));
    return (v < 1) ? 1 : v;
}

// =============================================================================
// Per-combo result with quarter-stratified PnL for stability scoring
// =============================================================================
struct ComboResult {
    int      combo_id   = 0;
    int      i1=0,i2=0,i3=0,i4=0,i5=0;  // grid indices (0..6)
    double   p1=0,p2=0,p3=0,p4=0,p5=0;  // actual parameter values
    int64_t  n_trades   = 0;
    int64_t  wins       = 0;
    double   total_pnl  = 0.0;
    double   q_pnl[4]   = {0,0,0,0};
    int64_t  q_trades[4]= {0,0,0,0};
    double   stddev     = 0.0;
    double   stability  = 0.0;
    double   score      = 0.0;
    double   win_rate() const { return n_trades ? 100.0 * wins / n_trades : 0.0; }
};

// =============================================================================
// Quarter index for a tick timestamp -- splits the full data range into 4
// chronological quarters by tick *count* (not calendar). Computed from the
// tick index relative to total tick count so the quarters are equal in
// ticks-processed regardless of weekend gaps.
// =============================================================================
static inline int quarter_of(int64_t tick_idx, int64_t total_ticks) noexcept {
    if (total_ticks <= 0) return 0;
    int64_t q = (tick_idx * 4) / total_ticks;
    if (q < 0) q = 0;
    if (q > 3) q = 3;
    return static_cast<int>(q);
}

// =============================================================================
// Compute stability + score AFTER all trades are recorded
// =============================================================================
static void finalise_score(ComboResult& r) noexcept {
    // Stddev of q_pnl[0..3]
    double mean = (r.q_pnl[0] + r.q_pnl[1] + r.q_pnl[2] + r.q_pnl[3]) * 0.25;
    double var  = 0.0;
    for (int q = 0; q < 4; ++q) { double d = r.q_pnl[q] - mean; var += d*d; }
    var /= 4.0;
    r.stddev    = std::sqrt(var);
    r.stability = 1.0 / (1.0 + r.stddev);
    // Score: stability * total_pnl. Negative-PnL combos get amplified
    // negatives (still ranked properly). Top of the list = stable winners.
    r.score = r.stability * r.total_pnl;
}

// =============================================================================
// Engine drivers -- one per templated engine class. Each implements:
//   void run(const ParamPack&, const std::vector<TickRow>&, ComboResult&)
// driving its engine over the full tick stream and writing aggregates back
// to the result struct.
// =============================================================================

// ---- HBG ---------------------------------------------------------------------
struct HBGParamPack {
    double min_range, max_range, sl_frac, tp_rr, trail_frac;
};
static void run_hbg(const HBGParamPack& pp,
                    const std::vector<TickRow>& ticks,
                    int64_t warmup_ticks,
                    ComboResult& out)
{
    omega::sweep::HBG_T eng({ pp.min_range, pp.max_range, pp.sl_frac,
                              pp.tp_rr, pp.trail_frac });
    eng.shadow_mode = false;

    const int64_t N = static_cast<int64_t>(ticks.size());

    // We need to know the current tick index inside the close callback.
    // Use a counter captured by reference.
    int64_t cur_idx = 0;
    auto cb = [&](const omega::TradeRecord& tr){
        if (tr.entryTs <= 0) return;
        if (cur_idx < warmup_ticks) return;
        const int q = quarter_of(cur_idx, N);
        out.n_trades++;
        if (tr.pnl > 0) out.wins++;
        out.total_pnl += tr.pnl;
        out.q_pnl[q]  += tr.pnl;
        out.q_trades[q]++;
    };

    for (cur_idx = 0; cur_idx < N; ++cur_idx) {
        const TickRow& r = ticks[cur_idx];
        omega::bt::set_sim_time(r.ts_ms);
        eng.on_tick(r.bid, r.ask, r.ts_ms,
                    /*can_enter=*/true,
                    /*flow_live=*/false,
                    /*flow_be_locked=*/false,
                    /*flow_trail_stage=*/0,
                    cb);
    }
    finalise_score(out);
}

// ---- AsianRange --------------------------------------------------------------
struct AsianParamPack {
    double buffer, min_range, max_range; int sl_ticks, tp_ticks;
};
static void run_asianrange(const AsianParamPack& pp,
                           const std::vector<TickRow>& ticks,
                           int64_t warmup_ticks,
                           ComboResult& out)
{
    omega::sweep::AsianRangeT eng(omega::sweep::AsianRangeT::Params{
        pp.buffer, pp.min_range, pp.max_range, pp.sl_ticks, pp.tp_ticks
    });

    SnapshotBuilder sb;
    omega::sweep::GoldSnapshot snap;

    const int64_t N = static_cast<int64_t>(ticks.size());

    // Open-trade tracking (engine emits Signal but does NOT manage exits;
    // harness owns SL/TP via tick_ticks-based price units).
    struct Pos { bool active=false; bool is_long=false; double entry=0; double sl_dist=0; double tp_dist=0; int64_t entry_ts=0; };
    Pos pos;

    auto close_pos = [&](double exit_px, int64_t /*ts_ms*/, int64_t cur_idx) {
        if (!pos.active) return;
        const double pnl_pts = pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px);
        if (cur_idx >= warmup_ticks) {
            const int q = quarter_of(cur_idx, N);
            out.n_trades++;
            if (pnl_pts > 0) out.wins++;
            out.total_pnl += pnl_pts * 0.01;  // size = 0.01
            out.q_pnl[q]  += pnl_pts * 0.01;
            out.q_trades[q]++;
        }
        pos.active = false;
    };

    // SL/TP are in "ticks" of 0.01 USD per the engine's signal.tp/sl values.
    // Convert ticks -> price units: 1 tick = 0.01 (XAUUSD).
    constexpr double TICK_SIZE = 0.01;

    for (int64_t i = 0; i < N; ++i) {
        const TickRow& r = ticks[i];
        omega::bt::set_sim_time(r.ts_ms);
        sb.update(snap, r.bid, r.ask, r.ts_ms);

        if (pos.active) {
            const double px_long_ref  = (r.bid + r.ask) * 0.5;
            const double tp_hit = pos.is_long
                ? (r.ask >= pos.entry + pos.tp_dist)
                : (r.bid <= pos.entry - pos.tp_dist);
            const double sl_hit = pos.is_long
                ? (r.bid <= pos.entry - pos.sl_dist)
                : (r.ask >= pos.entry + pos.sl_dist);
            if (tp_hit) {
                const double exit_px = pos.is_long ? (pos.entry + pos.tp_dist) : (pos.entry - pos.tp_dist);
                close_pos(exit_px, r.ts_ms, i);
                continue;
            }
            if (sl_hit) {
                const double exit_px = pos.is_long ? (pos.entry - pos.sl_dist) : (pos.entry + pos.sl_dist);
                close_pos(exit_px, r.ts_ms, i);
                continue;
            }
            (void)px_long_ref;
            continue;
        }

        omega::sweep::Signal sig = eng.process(snap);
        if (sig.valid) {
            pos.active   = true;
            pos.is_long  = sig.is_long();
            pos.entry    = sig.entry;
            pos.sl_dist  = sig.sl * TICK_SIZE;
            pos.tp_dist  = sig.tp * TICK_SIZE;
            pos.entry_ts = r.ts_ms;
        }
    }
    finalise_score(out);
}

// ---- VWAPStretch -------------------------------------------------------------
struct VWAPParamPack {
    int sl_ticks, tp_ticks, cooldown_sec; double sigma_entry; int vol_window;
};
static void run_vwapstretch(const VWAPParamPack& pp,
                            const std::vector<TickRow>& ticks,
                            int64_t warmup_ticks,
                            ComboResult& out)
{
    omega::sweep::VWAPStretchT eng(omega::sweep::VWAPStretchT::Params{
        pp.sl_ticks, pp.tp_ticks, pp.cooldown_sec, pp.sigma_entry, pp.vol_window
    });

    omega::bt::BtBarEngine<1> m1;  // for ema9/ema50 trend filter
    SnapshotBuilder sb;
    omega::sweep::GoldSnapshot snap;

    const int64_t N = static_cast<int64_t>(ticks.size());
    constexpr double TICK_SIZE = 0.01;

    // EWM drift (30s halflife, mirrors live)
    double ewm = 0.0; bool ewm_init = false;
    const double EWM_ALPHA = 1.0 - std::exp(std::log(0.5) / 30.0);  // halflife 30s in *ticks*; approx for high-frequency

    struct Pos { bool active=false; bool is_long=false; double entry=0; double sl_dist=0; double tp_dist=0; };
    Pos pos;

    auto close_pos = [&](double exit_px, int64_t cur_idx){
        if (!pos.active) return;
        const double pnl_pts = pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px);
        if (cur_idx >= warmup_ticks) {
            const int q = quarter_of(cur_idx, N);
            out.n_trades++;
            if (pnl_pts > 0) out.wins++;
            out.total_pnl += pnl_pts * 0.01;
            out.q_pnl[q]  += pnl_pts * 0.01;
            out.q_trades[q]++;
        }
        pos.active = false;
    };

    for (int64_t i = 0; i < N; ++i) {
        const TickRow& r = ticks[i];
        omega::bt::set_sim_time(r.ts_ms);
        sb.update(snap, r.bid, r.ask, r.ts_ms);

        const double mid = (r.bid + r.ask) * 0.5;
        if (!ewm_init) { ewm = mid; ewm_init = true; }
        else            { ewm = EWM_ALPHA * mid + (1.0 - EWM_ALPHA) * ewm; }

        const bool m1_closed = m1.on_tick(mid, r.ts_ms);
        (void)m1_closed;
        if (m1.indicators_ready()) {
            eng.set_ema_trend(m1.ema9(), m1.ema50());
            // ewm_drift: live uses (mid - ewm); harness mirrors that
            eng.set_ewm_drift(mid - ewm);
        }

        if (pos.active) {
            const bool tp_hit = pos.is_long
                ? (r.ask >= pos.entry + pos.tp_dist)
                : (r.bid <= pos.entry - pos.tp_dist);
            const bool sl_hit = pos.is_long
                ? (r.bid <= pos.entry - pos.sl_dist)
                : (r.ask >= pos.entry + pos.sl_dist);
            if (tp_hit) { close_pos(pos.is_long ? (pos.entry + pos.tp_dist) : (pos.entry - pos.tp_dist), i); continue; }
            if (sl_hit) { close_pos(pos.is_long ? (pos.entry - pos.sl_dist) : (pos.entry + pos.sl_dist), i); continue; }
            continue;
        }

        omega::sweep::Signal sig = eng.process(snap);
        if (sig.valid) {
            pos.active  = true;
            pos.is_long = sig.is_long();
            pos.entry   = sig.entry;
            pos.sl_dist = sig.sl * TICK_SIZE;
            pos.tp_dist = sig.tp * TICK_SIZE;
        }
    }
    finalise_score(out);
}

// ---- DXY (no-op data path; engine wired but data missing) -------------------
struct DXYParamPack {
    int sl_ticks, tp_ticks, cooldown_sec, window; double div_threshold;
};
static void run_dxy(const DXYParamPack& pp,
                    const std::vector<TickRow>& /*ticks*/,
                    int64_t /*warmup_ticks*/,
                    ComboResult& out)
{
    // No DXY tick stream wired -- engine would early-return on every tick.
    // We still finalise so the result has well-defined fields (zeros).
    (void)pp;
    finalise_score(out);
}

// ---- EMACross ----------------------------------------------------------------
struct EMAParamPack {
    int fast_period, slow_period; double rsi_lo, rsi_hi, sl_mult;
};
static void run_emacross(const EMAParamPack& pp,
                         const std::vector<TickRow>& ticks,
                         int64_t warmup_ticks,
                         ComboResult& out)
{
    omega::sweep::EMACrossT eng(omega::sweep::EMACrossT::Params{
        pp.fast_period, pp.slow_period, pp.rsi_lo, pp.rsi_hi, pp.sl_mult
    });
    eng.shadow_mode = false;

    omega::bt::BtBarEngine<1> m1;
    const int64_t N = static_cast<int64_t>(ticks.size());

    int64_t cur_idx = 0;
    auto cb = [&](const omega::TradeRecord& tr){
        if (tr.entryTs <= 0) return;
        if (cur_idx < warmup_ticks) return;
        const int q = quarter_of(cur_idx, N);
        out.n_trades++;
        if (tr.pnl > 0) out.wins++;
        out.total_pnl += tr.pnl;
        out.q_pnl[q]  += tr.pnl;
        out.q_trades[q]++;
    };

    for (cur_idx = 0; cur_idx < N; ++cur_idx) {
        const TickRow& r = ticks[cur_idx];
        omega::bt::set_sim_time(r.ts_ms);
        const double mid = (r.bid + r.ask) * 0.5;
        const bool m1_closed = m1.on_tick(mid, r.ts_ms);
        if (m1_closed && m1.indicators_ready()) {
            eng.on_bar(m1.last_closed_bar().close, m1.atr14(), m1.rsi14(), r.ts_ms);
        }
        eng.on_tick(r.bid, r.ask, r.ts_ms, cb);
    }
    finalise_score(out);
}

// =============================================================================
// Combo iterators -- generate the 7^5 = 16807 combos for each engine
// =============================================================================
struct ComboGen {
    int total = 0;
    void reserve(int n) { total = n; }
    // index decomposition: combo_id in [0, 16807) -> (i1,i2,i3,i4,i5) in [0,7)^5
    void decompose(int id, int* o) const {
        for (int k = 0; k < 5; ++k) { o[k] = id % N_GRID; id /= N_GRID; }
    }
};

// =============================================================================
// Worker pool
// =============================================================================
struct SweepJob {
    std::string engine_name;
    int total_combos = 0;
    std::vector<ComboResult>* results = nullptr;
    const std::vector<TickRow>* ticks = nullptr;
    int64_t warmup_ticks = 5000;
    bool verbose = false;

    std::atomic<int> next_combo{0};
    std::atomic<int> done_combos{0};
    std::chrono::steady_clock::time_point t_start;

    // Engine-specific runner -- bound to the right engine via lambda
    std::function<void(int /*combo_id*/, ComboResult&)> runner;
};

static void worker_loop(SweepJob* job) {
    while (true) {
        const int id = job->next_combo.fetch_add(1, std::memory_order_relaxed);
        if (id >= job->total_combos) break;

        ComboResult& r = (*job->results)[id];
        r.combo_id = id;

        job->runner(id, r);

        const int done = job->done_combos.fetch_add(1, std::memory_order_relaxed) + 1;
        if (job->verbose && (done % 50 == 0 || done == job->total_combos)) {
            const auto now = std::chrono::steady_clock::now();
            const double elapsed_s = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - job->t_start).count() / 1000.0;
            const double rate = done / std::max(1e-6, elapsed_s);
            const double eta_s = (job->total_combos - done) / std::max(1e-6, rate);
            fprintf(stderr, "[%s] %d/%d combos  (%.1f c/s, ETA %.0fs)\n",
                    job->engine_name.c_str(), done, job->total_combos, rate, eta_s);
        }
    }
}

// =============================================================================
// Run a sweep for one engine, write per-engine CSV
// =============================================================================
template <typename ParamFromIdx>
static void run_sweep(const std::string& engine_name,
                      int total_combos,
                      const std::vector<TickRow>& ticks,
                      int n_threads,
                      int64_t warmup_ticks,
                      bool verbose,
                      const std::string& outdir,
                      ParamFromIdx make_params,
                      std::function<void(const ComboResult&, ComboResult&)> /*unused*/)
{
    fprintf(stderr, "\n=== Sweep: %s ===\n", engine_name.c_str());
    fprintf(stderr, "  combos: %d\n", total_combos);
    fprintf(stderr, "  threads: %d\n", n_threads);

    std::vector<ComboResult> results(total_combos);
    SweepJob job;
    job.engine_name  = engine_name;
    job.total_combos = total_combos;
    job.results      = &results;
    job.ticks        = &ticks;
    job.warmup_ticks = warmup_ticks;
    job.verbose      = verbose;
    job.t_start      = std::chrono::steady_clock::now();

    // The runner closure is engine-specific -- caller binds via make_params.
    job.runner = [&](int combo_id, ComboResult& r){
        make_params(combo_id, r);
    };

    std::vector<std::thread> threads;
    threads.reserve(n_threads);
    for (int i = 0; i < n_threads; ++i) threads.emplace_back(worker_loop, &job);
    for (auto& t : threads) t.join();

    const auto t_end = std::chrono::steady_clock::now();
    const double elapsed_s = std::chrono::duration_cast<std::chrono::milliseconds>(
        t_end - job.t_start).count() / 1000.0;
    fprintf(stderr, "[%s] DONE %d combos in %.1fs (%.1f c/s)\n",
            engine_name.c_str(), total_combos, elapsed_s, total_combos / std::max(1e-6, elapsed_s));

    // Sort by score descending
    std::vector<ComboResult> sorted = results;
    std::sort(sorted.begin(), sorted.end(),
              [](const ComboResult& a, const ComboResult& b){ return a.score > b.score; });

    // Write CSV
    const std::string path = outdir + "/sweep_" + engine_name + ".csv";
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) { fprintf(stderr, "[%s] FAILED to open %s for write\n", engine_name.c_str(), path.c_str()); return; }
    std::fprintf(f,
        "rank,combo_id,p1,p2,p3,p4,p5,n_trades,win_rate,total_pnl,"
        "q1_pnl,q2_pnl,q3_pnl,q4_pnl,stddev,stability,score\n");
    int rank = 0;
    for (const auto& r : sorted) {
        ++rank;
        std::fprintf(f,
            "%d,%d,%g,%g,%g,%g,%g,%lld,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f,%.6f,%.4f\n",
            rank, r.combo_id, r.p1, r.p2, r.p3, r.p4, r.p5,
            (long long)r.n_trades, r.win_rate(), r.total_pnl,
            r.q_pnl[0], r.q_pnl[1], r.q_pnl[2], r.q_pnl[3],
            r.stddev, r.stability, r.score);
    }
    std::fclose(f);
    fprintf(stderr, "[%s] wrote %s\n", engine_name.c_str(), path.c_str());

    // Append top-50 to summary
    const std::string sumpath = outdir + "/sweep_summary.txt";
    FILE* fs = std::fopen(sumpath.c_str(), "a");
    if (fs) {
        std::fprintf(fs, "\n=== %s -- top 50 by score ===\n", engine_name.c_str());
        std::fprintf(fs, "%-4s %-7s  %-8s %-8s %-8s %-8s %-8s  %6s %5s %10s %10s %10s\n",
                     "rank","combo","p1","p2","p3","p4","p5","n","wr%","pnl","stddev","score");
        const int top = (int)std::min<size_t>(50, sorted.size());
        for (int k = 0; k < top; ++k) {
            const auto& r = sorted[k];
            std::fprintf(fs, "%-4d %-7d  %-8g %-8g %-8g %-8g %-8g  %6lld %5.1f %+10.2f %10.4f %+10.4f\n",
                         k+1, r.combo_id, r.p1, r.p2, r.p3, r.p4, r.p5,
                         (long long)r.n_trades, r.win_rate(), r.total_pnl,
                         r.stddev, r.score);
        }
        std::fclose(fs);
    }
}

// =============================================================================
// Engine-specific sweep launchers
// =============================================================================
static void sweep_hbg(const std::vector<TickRow>& ticks, int n_threads,
                      int64_t warmup_ticks, bool verbose, const std::string& outdir)
{
    constexpr int N = N_GRID*N_GRID*N_GRID*N_GRID*N_GRID;
    auto make = [&](int combo_id, ComboResult& r){
        int idx[5]; for (int k=0,id=combo_id;k<5;++k){idx[k]=id%N_GRID;id/=N_GRID;}
        const double base_min  = 6.0,  base_max  = 25.0;
        const double base_slf  = 0.5,  base_rr   = 2.0,  base_tr = 0.25;
        HBGParamPack pp{
            base_min * GRID_MULT[idx[0]],
            base_max * GRID_MULT[idx[1]],
            base_slf * GRID_MULT[idx[2]],
            base_rr  * GRID_MULT[idx[3]],
            base_tr  * GRID_MULT[idx[4]]
        };
        r.i1=idx[0]; r.i2=idx[1]; r.i3=idx[2]; r.i4=idx[3]; r.i5=idx[4];
        r.p1=pp.min_range; r.p2=pp.max_range; r.p3=pp.sl_frac; r.p4=pp.tp_rr; r.p5=pp.trail_frac;
        run_hbg(pp, ticks, warmup_ticks, r);
    };
    run_sweep("hbg", N, ticks, n_threads, warmup_ticks, verbose, outdir, make, nullptr);
}

static void sweep_asianrange(const std::vector<TickRow>& ticks, int n_threads,
                             int64_t warmup_ticks, bool verbose, const std::string& outdir)
{
    constexpr int N = N_GRID*N_GRID*N_GRID*N_GRID*N_GRID;
    auto make = [&](int combo_id, ComboResult& r){
        int idx[5]; for (int k=0,id=combo_id;k<5;++k){idx[k]=id%N_GRID;id/=N_GRID;}
        const double base_buf=0.50, base_min=3.0, base_max=50.0;
        const int    base_sl=80,    base_tp=200;
        AsianParamPack pp{
            base_buf * GRID_MULT[idx[0]],
            base_min * GRID_MULT[idx[1]],
            base_max * GRID_MULT[idx[2]],
            snap_int(base_sl, GRID_MULT[idx[3]]),
            snap_int(base_tp, GRID_MULT[idx[4]])
        };
        r.i1=idx[0]; r.i2=idx[1]; r.i3=idx[2]; r.i4=idx[3]; r.i5=idx[4];
        r.p1=pp.buffer; r.p2=pp.min_range; r.p3=pp.max_range; r.p4=pp.sl_ticks; r.p5=pp.tp_ticks;
        run_asianrange(pp, ticks, warmup_ticks, r);
    };
    run_sweep("asianrange", N, ticks, n_threads, warmup_ticks, verbose, outdir, make, nullptr);
}

static void sweep_vwapstretch(const std::vector<TickRow>& ticks, int n_threads,
                              int64_t warmup_ticks, bool verbose, const std::string& outdir)
{
    constexpr int N = N_GRID*N_GRID*N_GRID*N_GRID*N_GRID;
    auto make = [&](int combo_id, ComboResult& r){
        int idx[5]; for (int k=0,id=combo_id;k<5;++k){idx[k]=id%N_GRID;id/=N_GRID;}
        const int    base_sl=40, base_tp=88, base_cd=300, base_vw=40;
        const double base_se=2.0;
        VWAPParamPack pp{
            snap_int(base_sl, GRID_MULT[idx[0]]),
            snap_int(base_tp, GRID_MULT[idx[1]]),
            snap_int(base_cd, GRID_MULT[idx[2]]),
            base_se * GRID_MULT[idx[3]],
            snap_int(base_vw, GRID_MULT[idx[4]])
        };
        r.i1=idx[0]; r.i2=idx[1]; r.i3=idx[2]; r.i4=idx[3]; r.i5=idx[4];
        r.p1=pp.sl_ticks; r.p2=pp.tp_ticks; r.p3=pp.cooldown_sec; r.p4=pp.sigma_entry; r.p5=pp.vol_window;
        run_vwapstretch(pp, ticks, warmup_ticks, r);
    };
    run_sweep("vwapstretch", N, ticks, n_threads, warmup_ticks, verbose, outdir, make, nullptr);
}

static void sweep_dxy(const std::vector<TickRow>& ticks, int n_threads,
                      int64_t warmup_ticks, bool verbose, const std::string& outdir)
{
    fprintf(stderr, "[dxy] WARNING: no DXY tick stream wired into harness; all combos will produce 0 trades.\n");
    constexpr int N = N_GRID*N_GRID*N_GRID*N_GRID*N_GRID;
    auto make = [&](int combo_id, ComboResult& r){
        int idx[5]; for (int k=0,id=combo_id;k<5;++k){idx[k]=id%N_GRID;id/=N_GRID;}
        const int    base_sl=60, base_tp=120, base_cd=3600, base_w=20;
        const double base_dt=2.50;
        DXYParamPack pp{
            snap_int(base_sl, GRID_MULT[idx[0]]),
            snap_int(base_tp, GRID_MULT[idx[1]]),
            snap_int(base_cd, GRID_MULT[idx[2]]),
            snap_int(base_w,  GRID_MULT[idx[3]]),
            base_dt * GRID_MULT[idx[4]]
        };
        r.i1=idx[0]; r.i2=idx[1]; r.i3=idx[2]; r.i4=idx[3]; r.i5=idx[4];
        r.p1=pp.sl_ticks; r.p2=pp.tp_ticks; r.p3=pp.cooldown_sec; r.p4=pp.window; r.p5=pp.div_threshold;
        run_dxy(pp, ticks, warmup_ticks, r);
    };
    run_sweep("dxy", N, ticks, n_threads, warmup_ticks, verbose, outdir, make, nullptr);
}

static void sweep_emacross(const std::vector<TickRow>& ticks, int n_threads,
                           int64_t warmup_ticks, bool verbose, const std::string& outdir)
{
    constexpr int N = N_GRID*N_GRID*N_GRID*N_GRID*N_GRID;
    auto make = [&](int combo_id, ComboResult& r){
        int idx[5]; for (int k=0,id=combo_id;k<5;++k){idx[k]=id%N_GRID;id/=N_GRID;}
        const int    base_fp=9, base_sp=15;
        const double base_lo=40.0, base_hi=50.0, base_sm=1.5;
        EMAParamPack pp{
            snap_int(base_fp, GRID_MULT[idx[0]]),
            snap_int(base_sp, GRID_MULT[idx[1]]),
            base_lo * GRID_MULT[idx[2]],
            base_hi * GRID_MULT[idx[3]],
            base_sm * GRID_MULT[idx[4]]
        };
        // Guard slow > fast (otherwise the engine logic is degenerate)
        if (pp.slow_period <= pp.fast_period) pp.slow_period = pp.fast_period + 1;
        r.i1=idx[0]; r.i2=idx[1]; r.i3=idx[2]; r.i4=idx[3]; r.i5=idx[4];
        r.p1=pp.fast_period; r.p2=pp.slow_period; r.p3=pp.rsi_lo; r.p4=pp.rsi_hi; r.p5=pp.sl_mult;
        run_emacross(pp, ticks, warmup_ticks, r);
    };
    run_sweep("emacross", N, ticks, n_threads, warmup_ticks, verbose, outdir, make, nullptr);
}

// =============================================================================
// Config + main
// =============================================================================
struct Cfg {
    const char* csv     = nullptr;
    const char* outdir  = "sweep_results";
    int         threads = 0;       // 0 -> hw concurrency
    int64_t     warmup  = 5000;
    bool        verbose = false;
    bool        do_hbg=true, do_asianrange=true, do_vwapstretch=true,
                do_emacross=true, do_dxy=false;
};

static Cfg parse_args(int argc, char** argv) {
    Cfg c;
    if (argc < 2) {
        fprintf(stderr,
            "Usage: OmegaSweepHarness <ticks.csv> [options]\n"
            "  --engine <list>  comma list (default: hbg,asianrange,vwapstretch,emacross)\n"
            "                   available: hbg,asianrange,vwapstretch,emacross,dxy\n"
            "  --threads <n>    worker threads (default: hw concurrency)\n"
            "  --outdir <dir>   output dir (default: sweep_results)\n"
            "  --warmup <n>     ticks before recording trades (default: 5000)\n"
            "  --verbose        print per-combo progress\n");
        std::exit(1);
    }
    c.csv = argv[1];
    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        auto take = [&](const char*& dst){
            if (i+1 >= argc) { fprintf(stderr, "missing value for %s\n", a); std::exit(2); }
            dst = argv[++i];
        };
        auto take_i = [&](int& dst){
            if (i+1 >= argc) { fprintf(stderr, "missing value for %s\n", a); std::exit(2); }
            dst = std::atoi(argv[++i]);
        };
        auto take_i64 = [&](int64_t& dst){
            if (i+1 >= argc) { fprintf(stderr, "missing value for %s\n", a); std::exit(2); }
            dst = std::atoll(argv[++i]);
        };
        if      (std::strcmp(a, "--engine")  == 0) {
            const char* list = nullptr; take(list);
            c.do_hbg = c.do_asianrange = c.do_vwapstretch = c.do_emacross = c.do_dxy = false;
            std::string s(list);
            size_t pos = 0;
            while (pos < s.size()) {
                size_t comma = s.find(',', pos);
                std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                if      (tok == "hbg")          c.do_hbg = true;
                else if (tok == "asianrange")   c.do_asianrange = true;
                else if (tok == "vwapstretch")  c.do_vwapstretch = true;
                else if (tok == "emacross")     c.do_emacross = true;
                else if (tok == "dxy")          c.do_dxy = true;
                else fprintf(stderr, "unknown engine: %s\n", tok.c_str());
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
        else if (std::strcmp(a, "--threads") == 0) take_i(c.threads);
        else if (std::strcmp(a, "--outdir")  == 0) take(c.outdir);
        else if (std::strcmp(a, "--warmup")  == 0) take_i64(c.warmup);
        else if (std::strcmp(a, "--verbose") == 0) c.verbose = true;
        else { fprintf(stderr, "unknown arg: %s\n", a); std::exit(2); }
    }
    return c;
}

static bool ensure_dir(const char* path) {
#ifdef _WIN32
    CreateDirectoryA(path, nullptr);
    return true;
#else
    struct stat st{};
    if (stat(path, &st) == 0) return (st.st_mode & S_IFDIR) != 0;
    return mkdir(path, 0755) == 0;
#endif
}

int main(int argc, char** argv) {
    Cfg cfg = parse_args(argc, argv);

    if (!ensure_dir(cfg.outdir)) {
        fprintf(stderr, "Failed to create outdir %s\n", cfg.outdir);
        return 3;
    }

    int n_threads = cfg.threads;
    if (n_threads <= 0) n_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (n_threads <= 0) n_threads = 4;

    fprintf(stderr, "OmegaSweepHarness -- S51 X2\n");
    fprintf(stderr, "  csv:     %s\n", cfg.csv);
    fprintf(stderr, "  outdir:  %s\n", cfg.outdir);
    fprintf(stderr, "  threads: %d\n", n_threads);
    fprintf(stderr, "  warmup:  %lld ticks\n", (long long)cfg.warmup);
    fprintf(stderr, "  engines: %s%s%s%s%s\n",
            cfg.do_hbg          ? "hbg "         : "",
            cfg.do_asianrange   ? "asianrange "  : "",
            cfg.do_vwapstretch  ? "vwapstretch " : "",
            cfg.do_emacross     ? "emacross "    : "",
            cfg.do_dxy          ? "dxy "         : "");

    // ---- Load ticks ---------------------------------------------------------
    fprintf(stderr, "[csv] mmap %s ... ", cfg.csv);
    MemMappedFile mf;
    if (!mf.open(cfg.csv)) { fprintf(stderr, "FAILED\n"); return 4; }
    fprintf(stderr, "%.2f GB\n", mf.size / 1e9);

    fprintf(stderr, "[csv] parsing ticks ... ");
    auto t0 = std::chrono::steady_clock::now();
    std::vector<TickRow> ticks = parse_csv(mf);
    auto t1 = std::chrono::steady_clock::now();
    const double parse_s = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0;
    fprintf(stderr, "%lld ticks in %.1fs (%.1fM t/s)\n",
            (long long)ticks.size(), parse_s, ticks.size() / std::max(1e-6, parse_s) / 1e6);

    if (ticks.empty()) { fprintf(stderr, "No ticks parsed -- aborting.\n"); return 5; }

    // Truncate summary file
    {
        const std::string p = std::string(cfg.outdir) + "/sweep_summary.txt";
        FILE* fs = std::fopen(p.c_str(), "w");
        if (fs) {
            std::fprintf(fs, "OmegaSweepHarness summary -- S51 X2 -- 2026-04-27\n");
            std::fprintf(fs, "ticks: %lld   threads: %d   warmup: %lld\n",
                         (long long)ticks.size(), n_threads, (long long)cfg.warmup);
            std::fclose(fs);
        }
    }

    // ---- Run priority order: hbg -> dxy -> asianrange -> vwapstretch -> emacross
    if (cfg.do_hbg)         sweep_hbg(ticks,         n_threads, cfg.warmup, cfg.verbose, cfg.outdir);
    if (cfg.do_dxy)         sweep_dxy(ticks,         n_threads, cfg.warmup, cfg.verbose, cfg.outdir);
    if (cfg.do_asianrange)  sweep_asianrange(ticks,  n_threads, cfg.warmup, cfg.verbose, cfg.outdir);
    if (cfg.do_vwapstretch) sweep_vwapstretch(ticks, n_threads, cfg.warmup, cfg.verbose, cfg.outdir);
    if (cfg.do_emacross)    sweep_emacross(ticks,    n_threads, cfg.warmup, cfg.verbose, cfg.outdir);

    fprintf(stderr, "\nALL SWEEPS COMPLETE.\n");
    fprintf(stderr, "  see %s/sweep_*.csv and %s/sweep_summary.txt\n", cfg.outdir, cfg.outdir);
    return 0;
}

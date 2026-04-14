// =============================================================================
// cfe_bt.cpp -- High-performance CandleFlowEngine backtest
//
// Design:
//   - mmap file read: zero-copy, OS page cache, MADV_SEQUENTIAL
//   - Fast scalar CSV parser: no stringstream, no strtod, no allocations
//   - Format E auto-detected: YYYYMMDD,HH:MM:SS,bid,ask[,*]
//   - CRTP strategy pattern: zero virtual dispatch in hot path
//   - All indicators in flat fixed arrays: no deque, no heap
//   - OmegaTimeShim: simulated clock so all engine cooldowns are correct
//   - Gap detection: resets EWM/drift/bar state on gaps > 1 hour
//   - Full CFE reimplementation: exact mirror of CandleFlowEngine.hpp
//
// Build (Mac/Linux):
//   g++ -O3 -std=c++17 -o cfe_bt cfe_bt.cpp
//
// Run:
//   ./cfe_bt ~/Tick/2yr_XAUUSD_tick.csv
//   ./cfe_bt ~/Tick/2yr_XAUUSD_tick.csv --no-gates          # baseline
//   ./cfe_bt ~/Tick/2yr_XAUUSD_tick.csv --start 2024-01-01 --end 2024-07-01
//   ./cfe_bt ~/Tick/2yr_XAUUSD_tick.csv --trades out.csv --report rep.csv
//
// Expected throughput: ~15-25M ticks/sec on M-series Mac
// 111M ticks: ~5-8 seconds
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cassert>
#include <cinttypes>
#include <algorithm>
#include <vector>
#include <string>
#include <array>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

// =============================================================================
// mmap file
// =============================================================================
struct MMapFile {
    const char* data = nullptr;
    size_t      size = 0;
#ifdef _WIN32
    HANDLE hFile = INVALID_HANDLE_VALUE, hMap = nullptr;
#else
    int fd = -1;
#endif

    bool open(const char* path) noexcept {
#ifdef _WIN32
        hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{}; GetFileSizeEx(hFile, &sz);
        size  = (size_t)sz.QuadPart;
        hMap  = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!hMap) { CloseHandle(hFile); return false; }
        data  = (const char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
        return data != nullptr;
#else
        fd = ::open(path, O_RDONLY);
        if (fd < 0) return false;
        struct stat st{};
        fstat(fd, &st);
        size = (size_t)st.st_size;
        data = (const char*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (data == (const char*)-1) { data = nullptr; ::close(fd); return false; }
        madvise((void*)data, size, MADV_SEQUENTIAL);
        return true;
#endif
    }

    ~MMapFile() {
#ifdef _WIN32
        if (data) UnmapViewOfFile(data);
        if (hMap) CloseHandle(hMap);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
#else
        if (data) munmap((void*)data, size);
        if (fd >= 0) ::close(fd);
#endif
    }
};

// =============================================================================
// Fast scalar parsers -- no allocations, no locale, branchless digit scan
// =============================================================================
static inline double fast_f(const char* s, const char** e) noexcept {
    while (*s == ' ') ++s;
    bool neg = (*s == '-'); if (neg) ++s;
    double v = 0;
    while ((unsigned)(*s - '0') < 10u) v = v * 10.0 + (*s++ - '0');
    if (*s == '.') {
        ++s; double f = 0.1;
        while ((unsigned)(*s - '0') < 10u) { v += (*s++ - '0') * f; f *= 0.1; }
    }
    if (e) *e = s;
    return neg ? -v : v;
}

static inline int fast_int(const char* s, int n) noexcept {
    int v = 0;
    for (int i = 0; i < n; ++i) v = v * 10 + (s[i] - '0');
    return v;
}

// Convert YYYYMMDD,HH:MM:SS to epoch ms (UTC)
// Uses Gregorian proleptic calendar arithmetic
static int64_t ymdhms_to_ms(const char* d, const char* t) noexcept {
    int y  = fast_int(d, 4);
    int mo = fast_int(d+4, 2);
    int dy = fast_int(d+6, 2);
    int h  = fast_int(t, 2);
    int mi = fast_int(t+3, 2);
    int se = fast_int(t+6, 2);
    // days since Unix epoch (1970-01-01)
    if (mo <= 2) { --y; mo += 12; }
    int64_t days = 365LL*y + y/4 - y/100 + y/400
                 + (153*mo + 8)/5 + dy - 719469LL;
    return (days*86400LL + h*3600LL + mi*60LL + se)*1000LL;
}

// =============================================================================
// Tick row
// =============================================================================
struct Tick {
    int64_t ts_ms;
    double  bid, ask;
};

// =============================================================================
// Parse CSV -- Format E: YYYYMMDD,HH:MM:SS,bid,ask[,bid_dup,vol]
// Handles blank lines, weekends (just gaps in timestamps)
// =============================================================================
static std::vector<Tick> parse_csv(const MMapFile& f,
                                    int64_t start_ms, int64_t end_ms) {
    std::vector<Tick> v;
    v.reserve(120'000'000);

    const char* p   = f.data;
    const char* end = p + f.size;

    // Skip UTF-8 BOM
    if (f.size >= 3 && (uint8_t)p[0]==0xEF && (uint8_t)p[1]==0xBB && (uint8_t)p[2]==0xBF)
        p += 3;

    // Skip header if present (starts with non-digit)
    if (p < end && !((unsigned)(*p - '0') < 10u)) {
        while (p < end && *p != '\n') ++p;
        if (p < end) ++p;
    }

    while (p < end) {
        // Skip blank lines
        while (p < end && (*p == '\r' || *p == '\n')) ++p;
        if (p >= end) break;

        // Need at least YYYYMMDD = 8 digits
        if ((size_t)(end - p) < 8) break;

        // Format E: YYYYMMDD,HH:MM:SS,bid,ask,...
        // date = 8 chars, comma, time = 8 chars, comma, bid, comma, ask
        const char* date_p = p;
        // advance past date
        while (p < end && *p != ',') ++p;
        if (p >= end) break;
        ++p; // skip comma

        const char* time_p = p;
        while (p < end && *p != ',') ++p;
        if (p >= end) break;
        ++p; // skip comma

        const char* nx;
        double bid = fast_f(p, &nx); p = nx;
        if (p >= end || *p != ',') { while (p<end && *p!='\n') ++p; continue; }
        ++p;
        double ask = fast_f(p, &nx); p = nx;

        // skip rest of line
        while (p < end && *p != '\n') ++p;

        if (bid <= 0 || ask <= bid) continue;

        int64_t ts = ymdhms_to_ms(date_p, time_p);
        if (ts <= 0) continue;
        if (start_ms > 0 && ts < start_ms) continue;
        if (end_ms   > 0 && ts >= end_ms)  break;

        v.push_back({ts, bid, ask});
    }

    printf("[CSV] Parsed %zu ticks\n", v.size());
    return v;
}

// =============================================================================
// CFE constants -- exact mirror of CandleFlowEngine.hpp
// =============================================================================
static constexpr double  CFE_BODY_RATIO_MIN          = 0.60;
static constexpr double  CFE_COST_SLIPPAGE            = 0.10;
static constexpr double  CFE_COMMISSION_PTS           = 0.10;
static constexpr double  CFE_COST_MULT                = 2.0;
static constexpr int64_t CFE_STAGNATION_MS_ASIA       = 90'000;
static constexpr int64_t CFE_STAGNATION_MS_LONDON     = 180'000;
static constexpr double  CFE_STAGNATION_MULT          = 1.0;
static constexpr double  CFE_RISK_DOLLARS             = 30.0;
static constexpr double  CFE_MIN_LOT                  = 0.01;
static constexpr double  CFE_MAX_LOT                  = 0.20;
static constexpr int     CFE_RSI_PERIOD               = 30;
static constexpr int     CFE_RSI_EMA_N                = 10;
static constexpr double  CFE_RSI_THRESH               = 6.0;
static constexpr double  CFE_DFE_RSI_LEVEL_LONG_MIN   = 35.0;
static constexpr double  CFE_DFE_RSI_LEVEL_SHORT_MAX  = 65.0;
static constexpr int     CFE_DFE_DRIFT_PERSIST_TICKS  = 2;
static constexpr double  CFE_DFE_DRIFT_SUSTAINED_THRESH = 0.8;
static constexpr int64_t CFE_DFE_DRIFT_SUSTAINED_MS   = 45'000;
static constexpr double  CFE_BAR_TREND_BLOCK_DRIFT    = 0.5;
static constexpr int64_t CFE_BAR_TREND_BLOCK_MS       = 45'000;
static constexpr int     CFE_DFE_PRICE_CONFIRM_TICKS  = 3;
static constexpr double  CFE_DFE_PRICE_CONFIRM_MIN    = 0.05;
static constexpr int64_t CFE_IMB_MIN_HOLD_MS          = 20'000;
static constexpr double  CFE_DFE_DRIFT_THRESH         = 1.5;
static constexpr double  CFE_DFE_DRIFT_ACCEL          = 0.2;
static constexpr double  CFE_DFE_RSI_THRESH           = 3.0;
static constexpr double  CFE_DFE_RSI_TREND_MAX        = 12.0;
static constexpr double  CFE_DFE_SL_MULT              = 0.7;
static constexpr double  CFE_MAX_ATR_ENTRY            = 6.0;
static constexpr int64_t CFE_DFE_COOLDOWN_MS          = 120'000;
static constexpr double  CFE_DFE_MIN_SPREAD_MULT      = 1.5;
static constexpr int64_t CFE_OPPOSITE_DIR_COOLDOWN_MS = 60'000;
static constexpr int64_t CFE_WINNER_COOLDOWN_MS       = 30'000;
static constexpr double  CHOP_VOL_RATIO_THRESH        = 1.2;
static constexpr double  CHOP_DRIFT_ABS_THRESH        = 1.0;
static constexpr int64_t GAP_RESET_MS                 = 3'600'000; // 1 hour

// EWM drift: span=300 ticks
static constexpr double EWM_ALPHA = 2.0 / (300.0 + 1.0);
// ATR: 20-bar EMA
static constexpr double ATR_ALPHA = 2.0 / (20.0 + 1.0);
// RSI slope EMA
static constexpr double RSI_SLOPE_ALPHA = 2.0 / (CFE_RSI_EMA_N + 1.0);
// Vol ratio EMAs
static constexpr double VOL_SHORT_ALPHA = 0.04;
static constexpr double VOL_LONG_ALPHA  = 0.004;

// =============================================================================
// Trade record
// =============================================================================
struct TradeRec {
    int64_t entry_ts_ms, exit_ts_ms;
    double  entry, exit_px, sl, size, pnl_pts, pnl_usd, mfe;
    int     hold_s;
    bool    is_long;
    char    reason[24];   // SL_HIT, TRAIL_SL, PARTIAL_TP, STAGNATION, FORCE_CLOSE
    char    entry_type[4]; // BAR, DFE, SUS
};

// =============================================================================
// CRTP base: indicator bundle shared across strategies
// Flat arrays only. No heap allocation after construction.
// =============================================================================
struct Indicators {
    // RSI state -- circular buffer
    double rsi_gains[CFE_RSI_PERIOD] = {};
    double rsi_losses[CFE_RSI_PERIOD] = {};
    int    rsi_idx    = 0;
    int    rsi_count  = 0;
    double rsi_cur    = 50.0;
    double rsi_prev   = 50.0;
    double rsi_trend  = 0.0;
    bool   rsi_warmed = false;
    double rsi_prev_mid = 0.0;

    // EWM drift
    double ewm_mid   = 0.0;
    double ewm_drift = 0.0;
    bool   ewm_init  = false;

    // ATR (M1 bar EMA)
    double atr      = 0.0;
    bool   atr_init = false;

    // M1 bar builder
    double bar_open = 0.0, bar_high = 0.0, bar_low = 0.0, bar_close = 0.0;
    int64_t bar_ts  = 0;
    bool   bar_has  = false;
    // completed bar (valid for one tick)
    double cb_open = 0.0, cb_high = 0.0, cb_low = 0.0, cb_close = 0.0;
    int64_t cb_ts  = 0;
    bool   cb_valid = false;

    // VWAP
    double vwap_pv  = 0.0, vwap_vol = 0.0;
    int    vwap_day = -1;
    double vwap     = 0.0;

    // Vol ratio
    double vol_short = 0.0, vol_long = 0.0;

    // Recent mid ring buffer (CFE_DFE_PRICE_CONFIRM_TICKS+2)
    static constexpr int RECENT_N = CFE_DFE_PRICE_CONFIRM_TICKS + 2;
    double recent_mid[RECENT_N] = {};
    int    recent_idx  = 0;
    int    recent_cnt  = 0;

    // Gap detection
    int64_t last_ts_ms = 0;

    void reset_on_gap(double mid) noexcept {
        ewm_mid   = mid;
        ewm_drift = 0.0;
        bar_has   = false;
        cb_valid  = false;
        recent_cnt = 0;
        recent_idx = 0;
    }

    // Returns true if a new M1 bar was completed
    bool update(double bid, double ask, int64_t ts_ms) noexcept {
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        cb_valid = false;

        // Gap detection
        if (last_ts_ms > 0 && (ts_ms - last_ts_ms) > GAP_RESET_MS) {
            reset_on_gap(mid);
        }
        last_ts_ms = ts_ms;

        // EWM drift
        if (!ewm_init) { ewm_mid = mid; ewm_init = true; }
        ewm_mid   = EWM_ALPHA * mid + (1.0 - EWM_ALPHA) * ewm_mid;
        ewm_drift = mid - ewm_mid;

        // RSI
        if (rsi_prev_mid != 0.0) {
            const double chg = mid - rsi_prev_mid;
            rsi_gains [rsi_idx] = chg > 0 ?  chg : 0.0;
            rsi_losses[rsi_idx] = chg < 0 ? -chg : 0.0;
            rsi_idx = (rsi_idx + 1) % CFE_RSI_PERIOD;
            if (rsi_count < CFE_RSI_PERIOD) ++rsi_count;
            if (rsi_count >= CFE_RSI_PERIOD) {
                double ag = 0, al = 0;
                for (int i = 0; i < CFE_RSI_PERIOD; ++i) { ag += rsi_gains[i]; al += rsi_losses[i]; }
                ag /= CFE_RSI_PERIOD; al /= CFE_RSI_PERIOD;
                rsi_prev = rsi_cur;
                rsi_cur  = (al == 0.0) ? 100.0 : 100.0 - 100.0 / (1.0 + ag / al);
                const double slope = rsi_cur - rsi_prev;
                if (!rsi_warmed) { rsi_trend = slope; rsi_warmed = true; }
                else rsi_trend = RSI_SLOPE_ALPHA * slope + (1.0 - RSI_SLOPE_ALPHA) * rsi_trend;
            }
        }
        rsi_prev_mid = mid;

        // Recent mid ring buffer
        recent_mid[recent_idx] = mid;
        recent_idx = (recent_idx + 1) % RECENT_N;
        if (recent_cnt < RECENT_N) ++recent_cnt;

        // VWAP (daily reset)
        {
            const int day = (int)(ts_ms / 86'400'000LL);
            if (day != vwap_day) { vwap_pv = 0; vwap_vol = 0; vwap_day = day; }
            vwap_pv  += mid; vwap_vol += 1.0;
            vwap = vwap_pv / vwap_vol;
        }

        // Vol ratio
        const double abs_move = std::fabs(ewm_drift);
        vol_short = VOL_SHORT_ALPHA * abs_move + (1.0 - VOL_SHORT_ALPHA) * vol_short;
        vol_long  = VOL_LONG_ALPHA  * abs_move + (1.0 - VOL_LONG_ALPHA)  * vol_long;

        // M1 bar builder
        const int64_t bar_min = ts_ms / 60'000LL;
        if (!bar_has) {
            bar_open = bar_high = bar_low = bar_close = mid;
            bar_ts   = bar_min;
            bar_has  = true;
        } else if (bar_min != bar_ts) {
            // complete the bar
            cb_open  = bar_open;  cb_high = bar_high;
            cb_low   = bar_low;   cb_close = bar_close;
            cb_ts    = bar_ts * 60'000LL;
            cb_valid = true;
            // ATR update
            const double rng = bar_high - bar_low;
            if (!atr_init) { atr = rng; atr_init = true; }
            else atr = ATR_ALPHA * rng + (1.0 - ATR_ALPHA) * atr;
            // start new bar
            bar_open = bar_high = bar_low = bar_close = mid;
            bar_ts   = bar_min;
        } else {
            if (mid > bar_high) bar_high = mid;
            if (mid < bar_low)  bar_low  = mid;
            bar_close = mid;
        }

        return cb_valid;
    }

    double mid() const noexcept { return (ewm_mid + ewm_drift); } // approximation
    int    rsi_dir() const noexcept {
        if (!rsi_warmed) return 0;
        return (rsi_trend > CFE_RSI_THRESH) ? 1 : (rsi_trend < -CFE_RSI_THRESH) ? -1 : 0;
    }
    double vol_ratio() const noexcept {
        return (vol_long > 1e-9) ? vol_short / vol_long : 1.0;
    }
    double oldest_recent() const noexcept {
        if (recent_cnt < RECENT_N) return recent_mid[0];
        return recent_mid[recent_idx]; // oldest slot in ring
    }
};

// =============================================================================
// Open position
// =============================================================================
struct OpenPos {
    double  entry     = 0.0;
    double  sl        = 0.0;
    double  trail_sl  = 0.0;
    double  size      = 0.0;
    double  full_size = 0.0;
    double  cost_pts  = 0.0;
    double  atr_pts   = 0.0;
    double  mfe       = 0.0;
    int64_t entry_ts  = 0;
    bool    is_long   = false;
    bool    trail_on  = false;
    bool    partial   = false;
    bool    active    = false;
    char    entry_type[4] = "BAR";
};

// =============================================================================
// CRTP CFE strategy
// Derived classes can override entry/exit conditions via static methods.
// All state in flat POD. No virtual, no heap.
// =============================================================================
template<typename Derived>
struct CFEStrategy {
    Indicators ind;
    OpenPos    pos;

    // Phase: 0=IDLE, 1=LIVE, 2=COOLDOWN
    int     phase             = 0;
    int64_t cooldown_start_ms = 0;
    int64_t cooldown_ms       = 15'000;
    int64_t dfe_cool_until    = 0;
    int     last_closed_dir   = 0;
    int64_t last_closed_ms    = 0;

    // DFE state
    bool   dfe_warmed      = false;
    double prev_ewm_drift  = 0.0;
    double dfe_eff_thresh  = CFE_DFE_DRIFT_THRESH;
    int    dfe_persist_tks = 0;
    int    dfe_persist_dir = 0;

    // Sustained drift
    int64_t sus_start_ms = 0;
    int     sus_dir      = 0;

    // Adverse excursion
    double  adv_exit_px  = 0.0;
    int     adv_dir      = 0;
    double  adv_atr      = 0.0;
    bool    adv_block    = false;

    int trade_id = 0;

    // Trade output
    std::vector<TradeRec> trades;

    bool gates = true;  // can be disabled for baseline

    // ── CRTP hooks (Derived can specialise) ──────────────────────────────────
    // Default: no extra gate
    static bool extra_entry_gate(const Indicators&, bool /*is_long*/) noexcept { return true; }

    // ── Hot path ─────────────────────────────────────────────────────────────
    void on_tick(double bid, double ask, int64_t ts_ms) noexcept {
        const bool bar_complete = ind.update(bid, ask, ts_ms);
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const double drift  = ind.ewm_drift;
        const double atr    = (ind.atr > 0.0) ? ind.atr : spread * 3.0;

        // Session
        const int utc_h = (int)((ts_ms / 1000LL % 86400LL) / 3600LL);
        const bool in_asia  = (utc_h >= 22 || utc_h < 7);
        const bool post_ny  = (utc_h >= 19 && utc_h < 22);

        // Sustained drift state
        _update_sustained(drift, ts_ms);
        const int64_t sus_ms = (sus_dir != 0 && sus_start_ms > 0)
            ? (ts_ms - sus_start_ms) : 0;

        // Manage live position
        if (phase == 1) {
            _manage(bid, ask, mid, ts_ms, atr, in_asia);
            return;
        }

        // Cooldown
        if (phase == 2) {
            if (ts_ms - cooldown_start_ms >= cooldown_ms) phase = 0;
            else return;
        }

        // ── IDLE ─────────────────────────────────────────────────────────────

        // Adverse excursion block
        if (gates && adv_block && adv_dir != 0) {
            const double dist = (adv_dir == +1) ? (adv_exit_px - mid) : (mid - adv_exit_px);
            const bool same = (drift > 0 && adv_dir == +1) || (drift < 0 && adv_dir == -1);
            if (same && dist > adv_atr * 0.5) return;
            adv_block = false;
        }

        // Opposite direction cooldown
        if (last_closed_ms > 0 && last_closed_dir != 0) {
            if (ts_ms - last_closed_ms < CFE_OPPOSITE_DIR_COOLDOWN_MS) {
                const int intd = (drift > 0) ? +1 : -1;
                if (intd != last_closed_dir) return;
            }
        }

        // DFE effective threshold
        dfe_eff_thresh = in_asia
            ? std::max(4.0, atr * 0.40)
            : std::max(CFE_DFE_DRIFT_THRESH, atr * 0.30);

        // DFE persist tracking
        if (std::fabs(drift) >= dfe_eff_thresh) {
            const int d = (drift > 0) ? 1 : -1;
            if (d == dfe_persist_dir) ++dfe_persist_tks;
            else { dfe_persist_tks = 1; dfe_persist_dir = d; }
        } else { dfe_persist_tks = 0; dfe_persist_dir = 0; }

        // ── DFE path ─────────────────────────────────────────────────────────
        if (ind.rsi_warmed && std::fabs(drift) >= dfe_eff_thresh) {
            const double delta = drift - prev_ewm_drift;
            const bool accel = dfe_warmed &&
                ((drift > 0 && delta >= CFE_DFE_DRIFT_ACCEL) ||
                 (drift < 0 && delta <= -CFE_DFE_DRIFT_ACCEL));
            prev_ewm_drift = drift;
            dfe_warmed = true;
            const bool dlong = (drift > 0);
            const bool rsi_ok = dlong
                ? (ind.rsi_trend > CFE_DFE_RSI_THRESH && ind.rsi_trend < CFE_DFE_RSI_TREND_MAX)
                : (ind.rsi_trend < -CFE_DFE_RSI_THRESH && ind.rsi_trend > -CFE_DFE_RSI_TREND_MAX);
            const bool rlvl  = dlong ? (ind.rsi_cur >= CFE_DFE_RSI_LEVEL_LONG_MIN)
                                     : (ind.rsi_cur <= CFE_DFE_RSI_LEVEL_SHORT_MAX);
            const bool pers  = (dfe_persist_tks >= CFE_DFE_DRIFT_PERSIST_TICKS);
            // Price confirm
            bool pconf = true;
            if (ind.recent_cnt >= CFE_DFE_PRICE_CONFIRM_TICKS) {
                const double net = mid - ind.oldest_recent();
                pconf = dlong ? (net >= CFE_DFE_PRICE_CONFIRM_MIN)
                              : (net <= -CFE_DFE_PRICE_CONFIRM_MIN);
            }
            const double cost = spread + CFE_COST_SLIPPAGE*2 + CFE_COMMISSION_PTS*2;
            const bool sp_ok = spread < cost * CFE_DFE_MIN_SPREAD_MULT;
            const bool cd_ok = ts_ms >= dfe_cool_until;
            const bool at_ok = atr <= CFE_MAX_ATR_ENTRY;
            const bool xg = Derived::extra_entry_gate(ind, dlong);
            if (accel && rsi_ok && rlvl && pers && pconf && sp_ok && cd_ok && at_ok && xg) {
                _enter(dlong, bid, ask, spread, atr, ts_ms, "DFE");
                return;
            }
        }

        // ── Sustained-drift path ─────────────────────────────────────────────
        if (!in_asia && sus_ms >= CFE_DFE_DRIFT_SUSTAINED_MS) {
            const bool sl = (sus_dir == 1);
            const bool r2 = sl ? (ind.rsi_trend > 0) : (ind.rsi_trend < 0);
            const bool r3 = sl ? (ind.rsi_cur >= 40) : (ind.rsi_cur <= 60);
            const double cost = spread + CFE_COST_SLIPPAGE*2 + CFE_COMMISSION_PTS*2;
            const bool sp2 = spread < cost * CFE_DFE_MIN_SPREAD_MULT;
            const bool cd2 = ts_ms >= dfe_cool_until;
            const bool at2 = atr <= CFE_MAX_ATR_ENTRY;
            const bool xg2 = Derived::extra_entry_gate(ind, sl);
            if (r2 && r3 && sp2 && cd2 && at2 && xg2) {
                sus_start_ms = ts_ms; // reset to avoid re-fire
                _enter(sl, bid, ask, spread, atr, ts_ms, "SUS");
                return;
            }
        }

        // ── Bar-close path ───────────────────────────────────────────────────
        if (!bar_complete) return;
        if (!ind.rsi_warmed) return;

        // Post-NY gate
        if (gates && post_ny) return;

        // Drift minimum gate
        if (gates && std::fabs(drift) < 0.3) return;

        // Trend context gate
        if (sus_ms >= CFE_BAR_TREND_BLOCK_MS) {
            if (sus_dir == -1 && ind.cb_close > ind.cb_open) return;
            if (sus_dir == +1 && ind.cb_close < ind.cb_open) return;
        }

        // RSI direction
        const int rd = ind.rsi_dir();
        if (rd == 0) return;

        // RSI/drift agreement gate
        if (gates) {
            if (rd == +1 && drift < 0.0) return;
            if (rd == -1 && drift > 0.0) return;
        }

        // Candle body
        const double body   = ind.cb_close - ind.cb_open;
        const double brange = ind.cb_high  - ind.cb_low;
        if (brange == 0.0) return;
        const double bratio = std::fabs(body) / brange;
        if (bratio < CFE_BODY_RATIO_MIN) return;
        if (rd == +1 && body <= 0) return;
        if (rd == -1 && body >= 0) return;

        // Cost coverage
        const double cost_pts = spread + CFE_COST_SLIPPAGE*2 + CFE_COMMISSION_PTS*2;
        if (brange < CFE_COST_MULT * cost_pts) return;

        // ATR cap
        if (atr > CFE_MAX_ATR_ENTRY) return;

        // VWAP filter
        if (gates && ind.vwap > 0 && std::fabs(drift) < 1.0) {
            const bool ilong = (drift > 0);
            if (ilong  && mid > ind.vwap) return;
            if (!ilong && mid < ind.vwap) return;
        }

        // Chop gate
        if (gates && ind.vol_ratio() > CHOP_VOL_RATIO_THRESH
                  && std::fabs(drift) < CHOP_DRIFT_ABS_THRESH) return;

        // Extra gate (CRTP hook)
        const bool ilong = (rd == +1);
        if (!Derived::extra_entry_gate(ind, ilong)) return;

        _enter(ilong, bid, ask, spread, atr, ts_ms, "BAR");
    }

    // ── Enter ─────────────────────────────────────────────────────────────────
    void _enter(bool is_long, double bid, double ask,
                double spread, double atr, int64_t ts_ms,
                const char* etype) noexcept {
        const double ep   = is_long ? ask : bid;
        const double slpt = (atr > 0) ? atr : spread * 5.0;
        const double slpx = is_long ? (ep - slpt) : (ep + slpt);
        double sz = CFE_RISK_DOLLARS / (slpt * 100.0);
        sz = std::floor(sz / 0.001) * 0.001;
        sz = std::max(CFE_MIN_LOT, std::min(CFE_MAX_LOT, sz));
        const double cost = spread + CFE_COST_SLIPPAGE*2 + CFE_COMMISSION_PTS*2;

        pos.active    = true;
        pos.is_long   = is_long;
        pos.entry     = ep;
        pos.sl        = slpx;
        pos.trail_sl  = slpx;
        pos.size      = sz;
        pos.full_size = sz;
        pos.cost_pts  = cost;
        pos.atr_pts   = atr;
        pos.mfe       = 0.0;
        pos.entry_ts  = ts_ms;
        pos.trail_on  = false;
        pos.partial   = false;
        strncpy(pos.entry_type, etype, 3); pos.entry_type[3] = 0;
        ++trade_id;
        phase = 1;
    }

    // ── Manage position ───────────────────────────────────────────────────────
    void _manage(double bid, double ask, double mid,
                 int64_t ts_ms, double atr, bool in_asia) noexcept {
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;
        const int64_t hold_ms = ts_ms - pos.entry_ts;

        // Partial TP at MFE >= 2x cost
        if (!pos.partial && pos.mfe >= pos.cost_pts * 2.0) {
            const double px  = pos.is_long ? bid : ask;
            const double pnl = (pos.is_long ? (px - pos.entry) : (pos.entry - px))
                               * pos.full_size * 0.5;
            _record(px, "PARTIAL_TP", ts_ms, pos.full_size * 0.5, pnl, hold_ms);
            pos.partial = true;
            pos.size    = pos.full_size * 0.5;
        }

        // Trail SL: engage at MFE >= 2x ATR
        if (pos.mfe >= pos.atr_pts * 2.0) {
            const double td  = pos.atr_pts * 0.5;
            const double ntr = pos.is_long ? (mid - td) : (mid + td);
            if (!pos.trail_on) {
                if ((pos.is_long && ntr > pos.sl) || (!pos.is_long && ntr < pos.sl)) {
                    pos.trail_sl = ntr;
                    pos.trail_on = true;
                }
            } else {
                if ((pos.is_long && ntr > pos.trail_sl) ||
                    (!pos.is_long && ntr < pos.trail_sl))
                    pos.trail_sl = ntr;
            }
        }

        // SL hit
        const double eff_sl = pos.trail_on ? pos.trail_sl : pos.sl;
        if ((pos.is_long && bid <= eff_sl) || (!pos.is_long && ask >= eff_sl)) {
            const double px  = pos.is_long ? bid : ask;
            const double pnl = (pos.is_long ? (px - pos.entry) : (pos.entry - px)) * pos.size;
            _close(px, pos.trail_on ? "TRAIL_SL" : "SL_HIT", ts_ms, pnl, hold_ms);
            return;
        }

        // Stagnation
        const int64_t stag = in_asia ? CFE_STAGNATION_MS_ASIA : CFE_STAGNATION_MS_LONDON;
        if (hold_ms >= stag && pos.mfe < pos.cost_pts * CFE_STAGNATION_MULT) {
            const double px  = pos.is_long ? bid : ask;
            const double pnl = (pos.is_long ? (px - pos.entry) : (pos.entry - px)) * pos.size;
            _close(px, "STAGNATION", ts_ms, pnl, hold_ms);
        }
    }

    // ── Record partial ────────────────────────────────────────────────────────
    void _record(double exit_px, const char* reason, int64_t ts_ms,
                 double sz, double pnl, int64_t hold_ms) noexcept {
        TradeRec t{};
        t.entry_ts_ms = pos.entry_ts;
        t.exit_ts_ms  = ts_ms;
        t.entry       = pos.entry;
        t.exit_px     = exit_px;
        t.sl          = pos.sl;
        t.size        = sz;
        t.pnl_pts     = pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px);
        t.pnl_usd     = pnl * 100.0;
        t.mfe         = pos.mfe;
        t.hold_s      = (int)(hold_ms / 1000);
        t.is_long     = pos.is_long;
        strncpy(t.reason,     reason,         23); t.reason[23]     = 0;
        strncpy(t.entry_type, pos.entry_type,  3); t.entry_type[3]  = 0;
        trades.push_back(t);
    }

    // ── Close ─────────────────────────────────────────────────────────────────
    void _close(double exit_px, const char* reason, int64_t ts_ms,
                double pnl, int64_t hold_ms) noexcept {
        _record(exit_px, reason, ts_ms, pos.size, pnl, hold_ms);

        // Adverse excursion recording
        const double adverse = pos.is_long ? (pos.entry - exit_px) : (exit_px - pos.entry);
        if (adverse > 0 && pos.atr_pts > 0 && adverse >= pos.atr_pts * 0.5) {
            adv_exit_px = exit_px;
            adv_dir     = pos.is_long ? +1 : -1;
            adv_atr     = pos.atr_pts;
            adv_block   = true;
        }

        last_closed_dir = pos.is_long ? +1 : -1;
        last_closed_ms  = ts_ms;

        const bool is_win = (strcmp(reason,"PARTIAL_TP")==0 ||
                             strcmp(reason,"TRAIL_SL")==0);
        if      (strcmp(reason,"STAGNATION") == 0)  cooldown_ms = 60'000;
        else if (strcmp(reason,"FORCE_CLOSE")== 0)  cooldown_ms = 300'000;
        else if (strcmp(reason,"IMB_EXIT")   == 0)  cooldown_ms = 30'000;
        else if (is_win)                             cooldown_ms = 30'000;
        else                                         cooldown_ms = 15'000;

        if      (strcmp(reason,"FORCE_CLOSE")== 0)  dfe_cool_until = ts_ms + 300'000;
        else if (strcmp(reason,"SL_HIT")     == 0 ||
                 strcmp(reason,"TRAIL_SL")   == 0)  dfe_cool_until = ts_ms + CFE_DFE_COOLDOWN_MS;
        else if (is_win)                             dfe_cool_until = ts_ms + CFE_WINNER_COOLDOWN_MS;

        pos   = OpenPos{};
        phase = 2;
        cooldown_start_ms = ts_ms;
    }

    void _update_sustained(double drift, int64_t ts_ms) noexcept {
        const int nd = (drift >=  CFE_DFE_DRIFT_SUSTAINED_THRESH) ?  1
                     : (drift <= -CFE_DFE_DRIFT_SUSTAINED_THRESH) ? -1 : 0;
        if (nd != 0 && nd == sus_dir) return; // continuing
        if (nd != 0) { sus_dir = nd; sus_start_ms = ts_ms; }
        else         { sus_dir = 0;  sus_start_ms = 0; }
    }

    // Force close any open position
    void flush(int64_t ts_ms) noexcept {
        if (phase != 1) return;
        const double mid = pos.entry; // use entry as proxy
        const double pnl = 0.0;
        _close(mid, "FORCE_CLOSE", ts_ms, pnl, ts_ms - pos.entry_ts);
    }
};

// =============================================================================
// Concrete strategies via CRTP
// =============================================================================

// Standard CFE with all gates
struct CFE_Gates : CFEStrategy<CFE_Gates> {
    static bool extra_entry_gate(const Indicators&, bool) noexcept { return true; }
};

// Baseline: no gates (pure RSI+candle signal)
struct CFE_NoGates : CFEStrategy<CFE_NoGates> {
    static bool extra_entry_gate(const Indicators&, bool) noexcept { return true; }
};

// =============================================================================
// Statistics engine
// =============================================================================
struct Stats {
    const char* name = "";
    int64_t n = 0, wins = 0;
    double  pnl = 0, peak = 0, dd = 0;
    int64_t hold_sum = 0;
    // Monthly buckets [0..24]
    double  monthly[25] = {};
    int     by_hour[24] = {};
    double  by_hour_pnl[24] = {};
    int     by_reason_n[8]  = {};
    double  by_reason_p[8]  = {};
    const char* reason_names[8] = {
        "SL_HIT","TRAIL_SL","PARTIAL_TP","STAGNATION","FORCE_CLOSE","IMB_EXIT","","" };

    void add(const TradeRec& t) {
        if (strcmp(t.reason, "PARTIAL_TP") == 0) {
            // count partial separately but include in PnL
            pnl += t.pnl_usd;
            if (pnl > peak) peak = pnl;
            const double d = peak - pnl; if (d > dd) dd = d;
            return;
        }
        ++n;
        if (t.pnl_usd > 0) ++wins;
        pnl += t.pnl_usd;
        hold_sum += t.hold_s;
        if (pnl > peak) peak = pnl;
        const double d = peak - pnl; if (d > dd) dd = d;

        // Monthly
        const int64_t sec = t.exit_ts_ms / 1000;
        const int yr  = (int)((sec / 31557600LL) - 2 + 1972); // approx
        // simpler: use day index
        const int mo_idx = (int)((t.exit_ts_ms / 1000 - 1693180800LL) / 2592000LL); // approx
        if (mo_idx >= 0 && mo_idx < 25) monthly[mo_idx] += t.pnl_usd;

        // By UTC hour
        const int h = (int)((t.entry_ts_ms / 1000 % 86400) / 3600);
        if (h >= 0 && h < 24) { by_hour[h]++; by_hour_pnl[h] += t.pnl_usd; }

        // By reason
        for (int i = 0; i < 6; ++i) {
            if (strcmp(t.reason, reason_names[i]) == 0) {
                by_reason_n[i]++; by_reason_p[i] += t.pnl_usd; break;
            }
        }
    }

    void print(FILE* out = stdout) const {
        const double wr  = n ? 100.0*wins/n : 0;
        const double avg = n ? pnl/n : 0;
        const double avgh = n ? (double)hold_sum/n : 0;

        fprintf(out, "\n%s\n", std::string(70,'=').c_str());
        fprintf(out, "%-30s\n", name);
        fprintf(out, "%s\n", std::string(70,'=').c_str());
        fprintf(out, "Trades:       %lld\n", (long long)n);
        fprintf(out, "Net P&L:      $%.2f\n", pnl);
        fprintf(out, "Win rate:     %.1f%%\n", wr);
        fprintf(out, "Avg trade:    $%.2f\n", avg);
        fprintf(out, "Avg hold:     %.0fs\n", avgh);
        fprintf(out, "Max drawdown: $%.2f\n", dd);

        fprintf(out, "\n-- BY EXIT REASON --\n");
        for (int i = 0; i < 6; ++i) {
            if (by_reason_n[i] > 0)
                fprintf(out, "  %-15s  n=%5d  pnl=$%+8.2f\n",
                        reason_names[i], by_reason_n[i], by_reason_p[i]);
        }

        fprintf(out, "\n-- BY UTC HOUR --\n");
        for (int h = 0; h < 24; ++h) {
            if (by_hour[h] == 0) continue;
            const double wr_h = 0;
            fprintf(out, "  %02d:00  n=%4d  pnl=$%+7.2f\n",
                    h, by_hour[h], by_hour_pnl[h]);
        }
        fprintf(out, "%s\n", std::string(70,'=').c_str());
    }
};

// =============================================================================
// CSV writers
// =============================================================================
static void write_trades(const char* path, const std::vector<TradeRec>& trades,
                          const char* label) {
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[WARN] cannot write %s\n", path); return; }
    fprintf(f, "engine,side,entry_type,entry_ts_ms,exit_ts_ms,entry,exit_px,"
               "sl,size,pnl_pts,pnl_usd,hold_s,mfe,reason\n");
    for (const auto& t : trades)
        fprintf(f, "%s,%s,%s,%lld,%lld,%.3f,%.3f,%.3f,%.3f,%.4f,%.2f,%d,%.4f,%s\n",
                label,
                t.is_long ? "LONG" : "SHORT",
                t.entry_type,
                (long long)t.entry_ts_ms, (long long)t.exit_ts_ms,
                t.entry, t.exit_px, t.sl, t.size,
                t.pnl_pts, t.pnl_usd, t.hold_s, t.mfe, t.reason);
    fclose(f);
    printf("[OUTPUT] %zu trades -> %s\n", trades.size(), path);
}

static void write_equity(const char* path, const std::vector<TradeRec>& trades) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "date,daily_pnl,cumulative_pnl\n");
    double cum = 0; int cur_day = -1; double day_pnl = 0;
    for (const auto& t : trades) {
        const int day = (int)(t.exit_ts_ms / 86'400'000LL);
        if (day != cur_day) {
            if (cur_day >= 0) {
                cum += day_pnl;
                // approximate date string from epoch days
                const int64_t s = (int64_t)cur_day * 86400;
                fprintf(f, "%lld,%.2f,%.2f\n", (long long)s, day_pnl, cum);
            }
            cur_day = day; day_pnl = 0;
        }
        day_pnl += t.pnl_usd;
    }
    if (cur_day >= 0) { cum += day_pnl; fprintf(f, "%d,%.2f,%.2f\n", cur_day*86400, day_pnl, cum); }
    fclose(f);
    printf("[OUTPUT] Equity curve -> %s\n", path);
}

// =============================================================================
// Parse date arg YYYY-MM-DD -> epoch ms
// =============================================================================
static int64_t parse_date(const char* s) {
    if (!s || strlen(s) < 10) return 0;
    char buf[9] = {s[0],s[1],s[2],s[3], s[5],s[6], s[8],s[9], 0};
    const char* t = "00:00:00";
    return ymdhms_to_ms(buf, t);
}

// =============================================================================
// main
// =============================================================================
int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: cfe_bt <ticks.csv> [options]\n"
            "  --no-gates         disable all gates (baseline)\n"
            "  --start YYYY-MM-DD filter start date\n"
            "  --end   YYYY-MM-DD filter end date\n"
            "  --trades  <file>   trade CSV output      (default: cfe_bt_trades.csv)\n"
            "  --report  <file>   stats report          (default: cfe_bt_report.txt)\n"
            "  --equity  <file>   daily equity curve    (default: cfe_bt_equity.csv)\n");
        return 1;
    }

    const char* csv_path    = argv[1];
    const char* trades_path = "cfe_bt_trades.csv";
    const char* report_path = "cfe_bt_report.txt";
    const char* equity_path = "cfe_bt_equity.csv";
    bool   no_gates  = false;
    int64_t start_ms = 0, end_ms = 0;

    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--no-gates"))                      no_gates = true;
        else if (!strcmp(argv[i], "--start") && i+1 < argc) start_ms = parse_date(argv[++i]);
        else if (!strcmp(argv[i], "--end")   && i+1 < argc) end_ms   = parse_date(argv[++i]);
        else if (!strcmp(argv[i], "--trades")&& i+1 < argc) trades_path = argv[++i];
        else if (!strcmp(argv[i], "--report")&& i+1 < argc) report_path = argv[++i];
        else if (!strcmp(argv[i], "--equity")&& i+1 < argc) equity_path = argv[++i];
    }

    printf("[CFE_BT] Opening %s\n", csv_path);
    MMapFile f;
    if (!f.open(csv_path)) {
        fprintf(stderr, "Cannot open %s\n", csv_path);
        return 1;
    }
    printf("[CFE_BT] File size: %.1f MB\n", f.size / 1e6);

    // Parse
    std::vector<Tick> ticks = parse_csv(f, start_ms, end_ms);
    if (ticks.empty()) { fprintf(stderr, "No ticks parsed\n"); return 1; }

    printf("[CFE_BT] Running engine (gates=%s) on %zu ticks...\n",
           no_gates ? "OFF" : "ON", ticks.size());

    // Run CFE with gates
    CFE_Gates   eng_gates;
    CFE_NoGates eng_base;
    eng_gates.gates = true;
    eng_base.gates  = false;

    const size_t N = ticks.size();
    for (size_t i = 0; i < N; ++i) {
        const Tick& tk = ticks[i];
        if (!no_gates) eng_gates.on_tick(tk.bid, tk.ask, tk.ts_ms);
        else           eng_base.on_tick(tk.bid, tk.ask, tk.ts_ms);
        if (i % 10'000'000 == 0 && i > 0)
            printf("  %zu M ticks processed\n", i/1'000'000);
    }

    // Flush open positions
    if (!ticks.empty()) {
        const int64_t last_ts = ticks.back().ts_ms;
        if (!no_gates) eng_gates.flush(last_ts);
        else           eng_base.flush(last_ts);
    }

    auto& trades = no_gates ? eng_base.trades : eng_gates.trades;
    printf("[CFE_BT] Done. %zu trade records.\n", trades.size());

    // Stats
    Stats s;
    s.name = no_gates ? "CFE_BASELINE (no gates)" : "CFE_GATES (all gates)";
    for (const auto& t : trades) s.add(t);

    FILE* rep = fopen(report_path, "w");
    s.print(stdout);
    if (rep) { s.print(rep); fclose(rep); }
    printf("[OUTPUT] Report -> %s\n", report_path);

    write_trades(trades_path, trades, no_gates ? "CFE_BASELINE" : "CFE_GATES");
    write_equity(equity_path, trades);

    return 0;
}

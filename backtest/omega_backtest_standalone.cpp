// =============================================================================
// omega_backtest_standalone.cpp
//
// Self-contained C++ backtester for Mac/Linux — NO cmake, NO engine headers,
// NO OpenSSL. Compiles with a single clang++ command.
//
// BUILD (Mac):
//   clang++ -O3 -std=c++20 -o omega_bt omega_backtest_standalone.cpp
//
// RUN:
//   ./omega_bt ~/tick/data/xauusd_merged_24months.csv
//   ./omega_bt ~/tick/data/xauusd_merged_24months.csv --max 5000000   # quick test
//
// CSV FORMAT (auto-detected, handles your actual format):
//   timestamp,askPrice,bidPrice        ← your actual format
//   timestamp,bid,ask
//   timestamp,ask,bid                  ← detected by column name
//   YYYY-MM-DD HH:MM:SS,bid,ask
//   date,time,open,high,low,close,vol  ← OHLCV
//
// ENGINES REPLICATED (exact logic from GoldEngineStack.hpp):
//   1.  CompressionBreakout    WINDOW=50, RANGE=$6, BREAK=$2.50
//   2.  ImpulseContinuation
//   3.  SessionMomentum
//   4.  IntradaySeasonality
//   5.  WickRejection          5-min bars, wick>=55% of range
//   6.  DonchianBreakout       20-bar 5-min channel
//   7.  NR3Breakout            narrowest-3-bar 5-min
//   8.  MeanReversion          Z-score fade, LB=60
//   9.  SpikeFade              fade $10+ 5-min moves
//  10.  AsianRange             00-07 UTC range → London break
//  11.  DynamicRange           20-bar range extremes fade
//  12.  TwoBarReversal         2×ATR strong bar + reversal
//
// P&L MODEL:
//   size = 0.01 lot
//   gold: 1 price-point = $1 at 0.01 lot  (NOT $100 — the Python bug)
//   commission = $0.0 (BlackBull CFD — spread is the cost)
//   max hold = 600s (10 min, matching GoldStack config)
//
// OUTPUT:
//   results/report.html        (open in browser)
//   results/engine_summary.csv
//   results/all_trades.csv
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <cassert>
#include <cinttypes>
#include <string>
#include <vector>
#include <array>
#include <deque>
#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <functional>
#include <memory>
#include <fstream>
#include <sstream>
#include <chrono>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// ── output dir ───────────────────────────────────────────────────────────────
static const char* RESULTS_DIR = "results";

// =============================================================================
// Tick
// =============================================================================
struct Tick {
    int64_t ts_ms = 0;
    double  bid   = 0;
    double  ask   = 0;
    double  mid() const { return (bid + ask) * 0.5; }
    double  spread() const { return ask - bid; }
    bool    valid() const { return bid > 0 && ask > bid && (ask - bid) < 50.0; }
};

// =============================================================================
// Fast scalar parsers
// =============================================================================
static inline double fparse_f(const char* s, const char** e) noexcept {
    while (*s == ' ') ++s;
    bool neg = (*s == '-'); if (neg) ++s;
    double v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10.0 + (*s++ - '0');
    if (*s == '.') { ++s; double f = 0.1; while (*s>='0'&&*s<='9'){v+=(*s++-'0')*f;f*=.1;} }
    if (e) *e = s;
    return neg ? -v : v;
}
static inline int64_t fparse_i64(const char* s, const char** e) noexcept {
    while (*s == ' ') ++s;
    int64_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    if (e) *e = s;
    return v;
}

// Parse various datetime strings → epoch ms
static int64_t parse_datetime(const char* s) noexcept {
    // Try numeric first
    if (s[0] >= '0' && s[0] <= '9') {
        // Check if it's YYYY-MM-DD or YYYY.MM.DD
        if ((s[4] == '-' || s[4] == '.') && s[7] == s[4]) {
            // Date string — parse as tm
            struct tm ti{};
            ti.tm_year = (s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+(s[3]-'0') - 1900;
            ti.tm_mon  = (s[5]-'0')*10+(s[6]-'0') - 1;
            ti.tm_mday = (s[8]-'0')*10+(s[9]-'0');
            const char* t = s + 11;
            if (*t) {
                ti.tm_hour = (t[0]-'0')*10+(t[1]-'0');
                ti.tm_min  = (t[3]-'0')*10+(t[4]-'0');
                ti.tm_sec  = (t[6]-'0')*10+(t[7]-'0');
            }
            const time_t ep = timegm(&ti);
            int ms = 0;
            if (s[19] == '.' || (s[10] == 'T' && s[19] == '.')) {
                const char* p = strchr(s, '.') + 1;
                int pl = 0;
                while (*p >= '0' && *p <= '9' && pl < 3) { ms = ms*10+(*p++-'0'); ++pl; }
                while (pl++ < 3) ms *= 10;
            }
            return static_cast<int64_t>(ep) * 1000LL + ms;
        }
        // Pure number
        const char* e;
        int64_t v = fparse_i64(s, &e);
        return v < 2000000000LL ? v * 1000LL : v;  // seconds → ms if < year 2033
    }
    return 0;
}

// =============================================================================
// Memory-mapped CSV loader
// =============================================================================
static std::vector<Tick> load_csv(const char* path) {
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "[ERROR] cannot open %s\n", path); exit(1); }
    struct stat st{}; fstat(fd, &st);
    const size_t sz = static_cast<size_t>(st.st_size);
    const char* data = static_cast<const char*>(mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0));
    madvise(const_cast<char*>(data), sz, MADV_SEQUENTIAL);
    ::close(fd);

    std::vector<Tick> ticks; ticks.reserve(140'000'000);

    const char* p   = data;
    const char* end = data + sz;

    // BOM
    if (sz >= 3 && (uint8_t)p[0]==0xEF && (uint8_t)p[1]==0xBB && (uint8_t)p[2]==0xBF) p += 3;

    // Parse header to detect column order
    // Your file: timestamp,askPrice,bidPrice
    // We need to know which col is bid and which is ask
    int col_ts = 0, col_first = 1, col_second = 2;
    bool first_is_ask = false;  // if true: col1=ask, col2=bid

    {
        const char* hl = p;
        std::string hdr;
        while (p < end && *p != '\n') { hdr += (char)(*p); ++p; }
        if (p < end) ++p;

        // Lowercase for detection
        std::string h = hdr;
        for (auto& c : h) c = (char)tolower((unsigned char)c);

        // Find column positions by name
        // Split by comma
        std::vector<std::string> cols;
        std::stringstream ss(h);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            // trim whitespace
            while (!tok.empty() && (tok.front()==' '||tok.front()=='\r')) tok.erase(0,1);
            while (!tok.empty() && (tok.back()==' '||tok.back()=='\r')) tok.pop_back();
            cols.push_back(tok);
        }

        // Detect ask vs bid column order
        for (int i = 0; i < (int)cols.size(); ++i) {
            if (cols[i] == "askprice" || cols[i] == "ask_price" || cols[i] == "ask") {
                // ask comes before bid → first_is_ask = true
                // check if bid comes after
                for (int j = i+1; j < (int)cols.size(); ++j) {
                    if (cols[j] == "bidprice" || cols[j] == "bid_price" || cols[j] == "bid") {
                        col_first   = i;
                        col_second  = j;
                        first_is_ask = true;
                        break;
                    }
                }
                break;
            }
            if (cols[i] == "bidprice" || cols[i] == "bid_price" || cols[i] == "bid") {
                for (int j = i+1; j < (int)cols.size(); ++j) {
                    if (cols[j] == "askprice" || cols[j] == "ask_price" || cols[j] == "ask") {
                        col_first   = i;
                        col_second  = j;
                        first_is_ask = false;
                        break;
                    }
                }
                break;
            }
        }
        // Fallback: if header has >4 columns assume OHLCV
        (void)hl;
    }

    int64_t prev_ts = 0;

    while (p < end) {
        while (p < end && (*p == '\r' || *p == '\n')) ++p;
        if (p >= end) break;

        // Parse row into columns
        std::array<const char*, 8> col_start{};
        int ncols = 0;
        const char* row_start = p;
        col_start[ncols++] = p;
        while (p < end && *p != '\n' && *p != '\r') {
            if (*p == ',' && ncols < 8) col_start[ncols++] = p + 1;
            ++p;
        }

        if (ncols < 3) continue;

        Tick t;
        // Timestamp
        t.ts_ms = parse_datetime(col_start[col_ts]);
        if (t.ts_ms <= 0) t.ts_ms = prev_ts + 1;
        if (t.ts_ms <= prev_ts) t.ts_ms = prev_ts + 1;
        prev_ts = t.ts_ms;

        const char* nx;
        double v1 = fparse_f(col_start[col_first],  &nx);
        double v2 = fparse_f(col_start[col_second], &nx);

        if (first_is_ask) { t.ask = v1; t.bid = v2; }
        else              { t.bid = v1; t.ask = v2; }

        if (t.valid()) ticks.push_back(t);
        (void)row_start;
    }

    munmap(const_cast<char*>(data), sz);
    return ticks;
}

// =============================================================================
// UTC time helpers (no stdlib date nonsense — manual decomposition)
// =============================================================================
static void utc_break(int64_t ts_ms, int& hour, int& minute, int& wday, int& yday) {
    time_t t = (time_t)(ts_ms / 1000);
    struct tm ti{}; gmtime_r(&t, &ti);
    hour = ti.tm_hour; minute = ti.tm_min;
    wday = ti.tm_wday; yday = ti.tm_yday;
}
static inline int utc_hour(int64_t ts_ms) {
    int h,m,w,y; utc_break(ts_ms,h,m,w,y); return h;
}
static inline int utc_hhmm(int64_t ts_ms) {
    int h,m,w,y; utc_break(ts_ms,h,m,w,y); return h*60+m;
}
static inline int utc_yday(int64_t ts_ms) {
    int h,m,w,y; utc_break(ts_ms,h,m,w,y); return y;
}
static inline bool in_dead_zone(int64_t ts_ms) {
    int h = utc_hour(ts_ms);
    return (h >= 21 && h < 23) || (h >= 5 && h < 7);
}
static inline bool in_session_window(int64_t ts_ms) {
    int m = utc_hhmm(ts_ms);
    return (m >= 435 && m <= 630) || (m >= 795 && m <= 930);
}
static inline int half_hour_bucket(int64_t ts_ms) {
    int h,m,w,y; utc_break(ts_ms,h,m,w,y);
    return h*2 + (m >= 30 ? 1 : 0);
}
static inline int bar_minute(int64_t ts_ms, int bar_mins) {
    int m = utc_hhmm(ts_ms);
    return m / bar_mins;
}

// =============================================================================
// Circular buffer (power-of-2 size for speed)
// =============================================================================
template<size_t N>
struct CB {
    static_assert((N & (N-1)) == 0);
    double buf[N]{};
    size_t head = 0, cnt = 0;
    static constexpr size_t MASK = N - 1;

    void push(double v) {
        buf[(head + cnt) & MASK] = v;
        if (cnt < N) ++cnt; else head = (head + 1) & MASK;
    }
    size_t size() const { return cnt; }
    bool   full() const { return cnt == N; }
    double operator[](size_t i) const { return buf[(head + i) & MASK]; }
    double back()  const { return buf[(head + cnt - 1) & MASK]; }
    double front() const { return buf[head & MASK]; }
    void   clear() { head = cnt = 0; }

    double hi() const {
        double m = -1e18; for (size_t i=0;i<cnt;++i) if (buf[(head+i)&MASK]>m) m=buf[(head+i)&MASK]; return m;
    }
    double lo() const {
        double m = 1e18; for (size_t i=0;i<cnt;++i) if (buf[(head+i)&MASK]<m) m=buf[(head+i)&MASK]; return m;
    }
};

// =============================================================================
// VWAP (daily reset)
// =============================================================================
struct VWAP {
    double pv = 0, vol = 0; int day = -1;
    double update(const Tick& t) {
        int d = utc_yday(t.ts_ms);
        if (d != day) { pv = vol = 0; day = d; }
        pv += t.mid(); vol += 1.0;
        return vol > 0 ? pv / vol : t.mid();
    }
};

// =============================================================================
// EWM drift
// =============================================================================
struct EWM {
    double ema = 0, prev = 0; bool init = false;
    double update(double mid) {
        if (!init) { ema = prev = mid; init = true; return 0; }
        ema  = 0.02 * mid + 0.98 * ema;
        double d = ema - prev; prev = ema;
        return d * 100.0;
    }
};

// =============================================================================
// Position  — 0.01 lot gold CFD
//   P&L: (exit - entry) * lot_size * tick_multiplier
//   For gold CFD at 0.01 lot: 1 point = $1 * 0.01 = $0.01... 
//   Actually: BlackBull gold CFD — 1 lot = 100 oz, pip = $0.01/oz = $1/lot
//   0.01 lot → $0.01 per point. That's tiny.
//   Standard: 1 lot gold = $100/point, 0.01 lot = $1/point. Use that.
// =============================================================================
struct Pos {
    bool    active    = false;
    int     side      = 0;     // +1 long, -1 short
    double  entry     = 0;
    double  tp        = 0;
    double  sl        = 0;
    int64_t open_ts   = 0;
    double  mfe       = 0;
    double  mae       = 0;
    static constexpr double LOT  = 0.01;
    static constexpr double MULT = 1.0;   // $1/point at 0.01 lot (standard gold CFD)

    void open(int s, double e, double tp_pts, double sl_pts, int64_t ts) {
        active = true; side = s; entry = e;
        tp = e + s * tp_pts;
        sl = e - s * sl_pts;
        open_ts = ts; mfe = mae = 0;
    }

    // Returns true if closed, sets pnl and reason
    bool manage(const Tick& t, int max_hold_sec,
                double& pnl, std::string& reason) {
        if (!active) return false;
        double price = side == 1 ? t.bid : t.ask;
        double exc   = side * (price - entry);
        if (exc >  mfe) mfe =  exc;
        if (exc < -mae) mae = -exc;

        bool hit_tp = side==1 ? t.bid>=tp : t.ask<=tp;
        bool hit_sl = side==1 ? t.bid<=sl : t.ask>=sl;
        bool timeout= (t.ts_ms - open_ts) / 1000 >= max_hold_sec;

        if (hit_tp) { pnl = (tp    - entry) * side * MULT; reason = "TP";      active=false; return true; }
        if (hit_sl) { pnl = (sl    - entry) * side * MULT; reason = "SL";      active=false; return true; }
        if (timeout){ pnl = (price - entry) * side * MULT; reason = "TIMEOUT"; active=false; return true; }
        return false;
    }
};

// =============================================================================
// Trade record
// =============================================================================
struct Trade {
    std::string engine;
    int     side      = 0;
    double  entry     = 0;
    double  exit_px   = 0;
    double  pnl       = 0;
    double  mfe       = 0;
    double  mae       = 0;
    int64_t open_ts   = 0;
    int64_t close_ts  = 0;
    std::string reason;
};

// =============================================================================
// Per-engine stats
// =============================================================================
struct Stats {
    std::string name;
    std::vector<Trade> trades;
    double equity = 0, peak = 0, dd = 0;

    void add(Trade tr) {
        trades.push_back(tr);
        equity += tr.pnl;
        if (equity > peak) peak = equity;
        double d = peak - equity; if (d > dd) dd = d;
    }
    int    n()   const { return (int)trades.size(); }
    int    wins()const { int w=0; for(auto&t:trades)if(t.pnl>0)++w; return w; }
    double wr()  const { return n()>0?100.0*wins()/n():0; }
    double avg() const { return n()>0?equity/n():0; }
    double sharpe() const {
        if (n()<2) return 0;
        double m = avg();
        double v = 0; for(auto&t:trades)v+=(t.pnl-m)*(t.pnl-m);
        v /= n();
        return v>0?(m/sqrt(v))*sqrt((double)n()):0;
    }
};

// =============================================================================
// Signal struct
// =============================================================================
struct Sig {
    bool   valid   = false;
    int    side    = 0;
    double entry   = 0;
    double tp_pts  = 0;
    double sl_pts  = 0;
};

// =============================================================================
// ENGINE BASE
// =============================================================================
struct EngineBase {
    virtual ~EngineBase() = default;
    virtual const char* name() const = 0;
    virtual Sig on_tick(const Tick& t, double vwap, double drift) = 0;

    int64_t last_sig_ts = 0;
    bool cooldown(int64_t ts_ms, int ms) const {
        return (ts_ms - last_sig_ts) < ms;
    }
};

// =============================================================================
// ENGINE 1 — CompressionBreakout
// Exact replica of GoldEngineStack CompressionBreakoutEngine
// =============================================================================
struct CompressionBreakout : EngineBase {
    static constexpr int    WINDOW  = 50;
    static constexpr double RANGE   = 6.00;
    static constexpr double TRIGGER = 2.50;
    static constexpr double SPREAD  = 2.00;
    static constexpr double TP      = 10.0;  // $10
    static constexpr double SL      = 5.0;   // $5

    CB<64> hist;
    const char* name() const override { return "CompressionBreakout"; }

    Sig on_tick(const Tick& t, double vwap, double drift) override {
        if (t.spread() > SPREAD) return {};
        if (in_dead_zone(t.ts_ms)) return {};
        if (cooldown(t.ts_ms, 1000)) return {};

        bool is_asia = (utc_hour(t.ts_ms) >= 22 || utc_hour(t.ts_ms) < 5);
        double eff_spread  = is_asia ? SPREAD   * 0.60 : SPREAD;
        double eff_trigger = is_asia ? TRIGGER  * 1.40 : TRIGGER;
        if (t.spread() > eff_spread) return {};

        hist.push(t.mid());
        if (hist.size() < WINDOW) return {};

        double hi = hist.hi(), lo = hist.lo(), rng = hi - lo;
        if (rng > RANGE) return {};

        if (t.mid() > hi + eff_trigger) {
            if (drift < -3.0) return {};
            hist.clear();
            last_sig_ts = t.ts_ms;
            return {true, +1, t.ask, TP, SL};
        }
        if (t.mid() < lo - eff_trigger) {
            if (drift > +3.0) return {};
            hist.clear();
            last_sig_ts = t.ts_ms;
            return {true, -1, t.bid, TP, SL};
        }
        return {};
    }
};

// =============================================================================
// ENGINE 2 — WickRejection (5-min bars)
// =============================================================================
struct WickRejection : EngineBase {
    static constexpr int    BAR_MINS  = 5;
    static constexpr double WICK_PCT  = 0.55;
    static constexpr double MIN_WICK  = 1.50;
    static constexpr double MIN_RANGE = 2.25;
    static constexpr double SPREAD    = 2.00;
    static constexpr double TP        = 15.0;
    static constexpr double SL        = 6.0;
    static constexpr int    COOLDOWN  = 300 * 1000;  // ms

    double bar_o=0, bar_h=0, bar_l=0, bar_c=0;
    int    bar_bm = -1;
    int    pending = 0;  // +1/-1

    const char* name() const override { return "WickRejection"; }

    Sig on_tick(const Tick& t, double vwap, double drift) override {
        (void)vwap; (void)drift;
        if (t.spread() > SPREAD) return {};
        if (in_dead_zone(t.ts_ms)) return {};

        int bm = bar_minute(t.ts_ms, BAR_MINS);
        if (bm != bar_bm) {
            // close bar
            if (bar_bm >= 0 && bar_h > 0) {
                double rng = bar_h - bar_l;
                if (rng >= MIN_RANGE) {
                    double upper = bar_h - std::max(bar_o, bar_c);
                    double lower = std::min(bar_o, bar_c) - bar_l;
                    if (upper >= MIN_WICK && upper / rng >= WICK_PCT) pending = -1;
                    else if (lower >= MIN_WICK && lower / rng >= WICK_PCT) pending = +1;
                }
            }
            bar_o = bar_h = bar_l = bar_c = t.mid();
            bar_bm = bm;
        } else {
            if (t.mid() > bar_h) bar_h = t.mid();
            if (t.mid() < bar_l) bar_l = t.mid();
            bar_c = t.mid();
        }

        if (pending != 0 && !cooldown(t.ts_ms, COOLDOWN)) {
            int s = pending; pending = 0;
            last_sig_ts = t.ts_ms;
            return {true, s, s==1?t.ask:t.bid, TP, SL};
        }
        return {};
    }
};

// =============================================================================
// ENGINE 3 — DonchianBreakout (5-min bars, 20-bar channel)
// =============================================================================
struct DonchianBreakout : EngineBase {
    static constexpr int    BAR_MINS = 5;
    static constexpr int    CHANNEL  = 20;
    static constexpr double SPREAD   = 2.50;
    static constexpr double TP       = 12.0;
    static constexpr double SL       = 5.0;
    static constexpr int    COOLDOWN = 180 * 1000;

    double bar_h=0, bar_l=0; int bar_bm=-1;
    std::deque<std::pair<double,double>> bars;  // (hi, lo) per closed bar

    const char* name() const override { return "DonchianBreakout"; }

    Sig on_tick(const Tick& t, double vwap, double drift) override {
        (void)vwap; (void)drift;
        if (t.spread() > SPREAD) return {};
        if (in_dead_zone(t.ts_ms)) return {};
        if (cooldown(t.ts_ms, COOLDOWN)) return {};

        int bm = bar_minute(t.ts_ms, BAR_MINS);
        if (bm != bar_bm) {
            if (bar_bm >= 0 && bar_h > 0) {
                bars.push_back({bar_h, bar_l});
                if ((int)bars.size() > CHANNEL) bars.pop_front();
            }
            bar_h = bar_l = t.mid(); bar_bm = bm;
        } else {
            if (t.mid() > bar_h) bar_h = t.mid();
            if (t.mid() < bar_l) bar_l = t.mid();
        }
        if ((int)bars.size() < CHANNEL) return {};

        double chi = bars[0].first, clo = bars[0].second;
        for (auto& b : bars) { if(b.first>chi) chi=b.first; if(b.second<clo) clo=b.second; }

        if (t.mid() > chi) { last_sig_ts=t.ts_ms; return {true,+1,t.ask,TP,SL}; }
        if (t.mid() < clo) { last_sig_ts=t.ts_ms; return {true,-1,t.bid,TP,SL}; }
        return {};
    }
};

// =============================================================================
// ENGINE 4 — NR3Breakout (narrowest 3-bar 5-min)
// =============================================================================
struct NR3Breakout : EngineBase {
    static constexpr int    BAR_MINS = 5;
    static constexpr double SPREAD   = 2.00;
    static constexpr double TP       = 10.0;
    static constexpr double SL       = 4.0;
    static constexpr int    COOLDOWN = 120 * 1000;

    double bar_h=0, bar_l=0; int bar_bm=-1;
    struct Bar { double hi,lo,rng; };
    std::deque<Bar> bars;
    double nr_hi=0, nr_lo=0; bool armed=false;

    const char* name() const override { return "NR3Breakout"; }

    Sig on_tick(const Tick& t, double vwap, double drift) override {
        (void)vwap; (void)drift;
        if (t.spread() > SPREAD) return {};
        if (in_dead_zone(t.ts_ms)) return {};
        if (cooldown(t.ts_ms, COOLDOWN)) return {};

        int bm = bar_minute(t.ts_ms, BAR_MINS);
        if (bm != bar_bm) {
            if (bar_bm >= 0 && bar_h > 0) {
                Bar b{bar_h, bar_l, bar_h-bar_l};
                bars.push_back(b);
                if (bars.size() > 3) bars.pop_front();
                if ((int)bars.size() == 3) {
                    double r0=bars[0].rng,r1=bars[1].rng,r2=bars[2].rng;
                    if (r2 < r0 && r2 < r1) {
                        nr_hi = bars[2].hi; nr_lo = bars[2].lo; armed = true;
                    }
                }
            }
            bar_h = bar_l = t.mid(); bar_bm = bm;
        } else {
            if (t.mid() > bar_h) bar_h = t.mid();
            if (t.mid() < bar_l) bar_l = t.mid();
        }

        if (armed && nr_hi > 0) {
            if (t.mid() > nr_hi) { armed=false; last_sig_ts=t.ts_ms; return {true,+1,t.ask,TP,SL}; }
            if (t.mid() < nr_lo) { armed=false; last_sig_ts=t.ts_ms; return {true,-1,t.bid,TP,SL}; }
        }
        return {};
    }
};

// =============================================================================
// ENGINE 5 — IntradaySeasonality
// =============================================================================
struct IntradaySeasonality : EngineBase {
    static constexpr double SPREAD = 2.00;
    static constexpr double TP     = 10.0;
    static constexpr double SL     = 5.0;

    // Half-hour bucket biases (|t-stat| > 5 only, from GoldEngineStack)
    static int bias(int hh) {
        static const int B[48] = {
         1, 1, 1, 1, 0, 0, 1, 0,  // 0-7
         1, -1,-1, 0, 1, 1, 0, 0, // 8-15
         1, 0, 0, 1, -1, 0, 0, 0, // 16-23
         1, 0, 0, 0, 0, 0, 0, 0,  // 24-31
         1, 0, 0, 1, 0, 0, 0, 1,  // 32-39
         1, 1, 1, 1, 1, 1, 0, 1,  // 40-47
        };
        return (hh >= 0 && hh < 48) ? B[hh] : 0;
    }

    int last_hh = -1, last_day = -1;
    CB<32> hist;

    const char* name() const override { return "IntradaySeasonality"; }

    Sig on_tick(const Tick& t, double vwap, double drift) override {
        (void)vwap; (void)drift;
        if (t.spread() > SPREAD) return {};
        if (in_dead_zone(t.ts_ms)) return {};

        int hh  = half_hour_bucket(t.ts_ms);
        int day = utc_yday(t.ts_ms);
        int b   = bias(hh);
        if (b == 0) return {};
        if (hh == last_hh && day == last_day) return {};

        hist.push(t.mid());
        if (hist.size() >= 10) {
            double impulse = std::abs(t.mid() - hist[hist.size()-10]);
            if (impulse > 3.0) return {};
        }

        last_hh = hh; last_day = day;
        last_sig_ts = t.ts_ms;
        return {true, b, b==1?t.ask:t.bid, TP, SL};
    }
};

// =============================================================================
// ENGINE 6 — SessionMomentum
// =============================================================================
struct SessionMomentum : EngineBase {
    static constexpr int    WINDOW   = 60;
    static constexpr double IMP_MIN  = 3.50;
    static constexpr double SPREAD   = 2.50;
    static constexpr double VWAP_DEV = 1.50;
    static constexpr double TP       = 10.0;
    static constexpr double SL       = 5.0;

    CB<64> hist;
    const char* name() const override { return "SessionMomentum"; }

    Sig on_tick(const Tick& t, double vwap, double drift) override {
        (void)drift;
        if (!in_session_window(t.ts_ms)) return {};
        if (t.spread() > SPREAD) return {};
        if (cooldown(t.ts_ms, 1000)) return {};

        hist.push(t.mid());
        if ((int)hist.size() < WINDOW) return {};

        double hi = hist.hi(), lo = hist.lo();
        if (hi - lo < IMP_MIN) return {};
        if (vwap <= 0 || std::abs(t.mid() - vwap) < VWAP_DEV) return {};

        double dhi = hi - t.mid(), dlo = t.mid() - lo;
        double rec5 = hist.size()>=5 ? t.mid()-hist[hist.size()-5] : 0;

        if (dhi < dlo && t.mid() > vwap && rec5 < -0.30) {
            hist.clear(); last_sig_ts=t.ts_ms;
            return {true,+1,t.ask,TP,SL};
        }
        if (dlo < dhi && t.mid() < vwap && rec5 > +0.30) {
            hist.clear(); last_sig_ts=t.ts_ms;
            return {true,-1,t.bid,TP,SL};
        }
        return {};
    }
};

// =============================================================================
// ENGINE 7 — MeanReversion (Z-score)
// =============================================================================
struct MeanReversion : EngineBase {
    static constexpr int    LB      = 60;
    static constexpr double Z_ENTRY = 2.0;
    static constexpr double SPREAD  = 2.00;
    static constexpr double TP      = 12.0;
    static constexpr double SL      = 4.0;
    static constexpr int    COOLDOWN= 60 * 1000;

    CB<64> hist;
    const char* name() const override { return "MeanReversion"; }

    Sig on_tick(const Tick& t, double vwap, double drift) override {
        (void)vwap; (void)drift;
        if (t.spread() > SPREAD) return {};
        if (in_dead_zone(t.ts_ms)) return {};
        if (cooldown(t.ts_ms, COOLDOWN)) return {};

        hist.push(t.mid());
        if ((int)hist.size() < LB) return {};

        // compute mean and stddev
        double sum = 0;
        for (size_t i=0; i<hist.size(); ++i) sum += hist[i];
        double mean = sum / hist.size();
        double var  = 0;
        for (size_t i=0; i<hist.size(); ++i) { double d=hist[i]-mean; var+=d*d; }
        double sd = std::sqrt(var / hist.size());
        if (sd < 0.01) return {};

        double z = (t.mid() - mean) / sd;
        if (z < -Z_ENTRY) { last_sig_ts=t.ts_ms; return {true,+1,t.ask,TP,SL}; }
        if (z >  Z_ENTRY) { last_sig_ts=t.ts_ms; return {true,-1,t.bid,TP,SL}; }
        return {};
    }
};

// =============================================================================
// ENGINE 8 — SpikeFade ($10+ 5-min move)
// =============================================================================
struct SpikeFade : EngineBase {
    static constexpr int    BAR_MINS = 5;
    static constexpr double MIN_MOVE = 10.0;
    static constexpr double SPREAD   = 3.00;
    static constexpr double TP       = 5.0;
    static constexpr double SL       = 8.0;
    static constexpr int    COOLDOWN = 30 * 60 * 1000;

    double bar_open=0; int bar_bm=-1;
    const char* name() const override { return "SpikeFade"; }

    Sig on_tick(const Tick& t, double vwap, double drift) override {
        (void)vwap; (void)drift;
        if (t.spread() > SPREAD) return {};
        if (cooldown(t.ts_ms, COOLDOWN)) return {};

        int bm = bar_minute(t.ts_ms, BAR_MINS);
        if (bm != bar_bm) { bar_open = t.mid(); bar_bm = bm; return {}; }

        double move = t.mid() - bar_open;
        if (move >  MIN_MOVE) { last_sig_ts=t.ts_ms; return {true,-1,t.bid,TP,SL}; }
        if (move < -MIN_MOVE) { last_sig_ts=t.ts_ms; return {true,+1,t.ask,TP,SL}; }
        return {};
    }
};

// =============================================================================
// ENGINE 9 — DynamicRange (20-bar range extremes fade)
// =============================================================================
struct DynamicRange : EngineBase {
    static constexpr int    BARS     = 20;
    static constexpr int    BAR_MINS = 5;
    static constexpr double SPREAD   = 2.00;
    static constexpr double TP       = 8.0;
    static constexpr double SL       = 4.0;
    static constexpr int    COOLDOWN = 60 * 1000;

    double bar_h=0, bar_l=0; int bar_bm=-1;
    std::deque<std::pair<double,double>> bars;

    const char* name() const override { return "DynamicRange"; }

    Sig on_tick(const Tick& t, double vwap, double drift) override {
        (void)vwap; (void)drift;
        if (t.spread() > SPREAD) return {};
        if (in_dead_zone(t.ts_ms)) return {};
        if (cooldown(t.ts_ms, COOLDOWN)) return {};

        int bm = bar_minute(t.ts_ms, BAR_MINS);
        if (bm != bar_bm) {
            if (bar_bm >= 0) { bars.push_back({bar_h,bar_l}); if((int)bars.size()>BARS)bars.pop_front(); }
            bar_h = bar_l = t.mid(); bar_bm = bm;
        } else { if(t.mid()>bar_h)bar_h=t.mid(); if(t.mid()<bar_l)bar_l=t.mid(); }

        if ((int)bars.size() < BARS) return {};
        double rhi=bars[0].first, rlo=bars[0].second;
        for(auto&b:bars){if(b.first>rhi)rhi=b.first;if(b.second<rlo)rlo=b.second;}

        // Fade extremes
        double band = (rhi - rlo) * 0.15;
        if (t.mid() >= rhi - band) { last_sig_ts=t.ts_ms; return {true,-1,t.bid,TP,SL}; }
        if (t.mid() <= rlo + band) { last_sig_ts=t.ts_ms; return {true,+1,t.ask,TP,SL}; }
        return {};
    }
};

// =============================================================================
// Runner — owns one engine + its position + its stats
// =============================================================================
struct Runner {
    std::unique_ptr<EngineBase> eng;
    Pos pos;
    Stats stats;
    int max_hold_sec = 600;

    explicit Runner(std::unique_ptr<EngineBase> e) : eng(std::move(e)) {
        stats.name = eng->name();
    }

    void tick(const Tick& t, double vwap, double drift) {
        // Manage open position first
        if (pos.active) {
            double pnl; std::string reason;
            if (pos.manage(t, max_hold_sec, pnl, reason)) {
                Trade tr;
                tr.engine   = eng->name();
                tr.side     = pos.side;
                tr.entry    = pos.entry;
                tr.exit_px  = pos.side==1 ? (reason=="TP"?pos.tp:(reason=="SL"?pos.sl:t.bid))
                                          : (reason=="TP"?pos.tp:(reason=="SL"?pos.sl:t.ask));
                tr.pnl      = pnl;
                tr.mfe      = pos.mfe;
                tr.mae      = pos.mae;
                tr.open_ts  = pos.open_ts;
                tr.close_ts = t.ts_ms;
                tr.reason   = reason;
                stats.add(tr);
                return;
            }
        }
        // Request new signal only when flat
        if (!pos.active) {
            Sig sig = eng->on_tick(t, vwap, drift);
            if (sig.valid) pos.open(sig.side, sig.entry, sig.tp_pts, sig.sl_pts, t.ts_ms);
        }
    }
};

// =============================================================================
// HTML report
// =============================================================================
static void write_html(const std::vector<Runner>& runners,
                       int64_t tick_count, double elapsed_s) {
    // Sort by pnl desc
    std::vector<const Runner*> sorted;
    for (auto& r : runners) sorted.push_back(&r);
    std::sort(sorted.begin(), sorted.end(),
              [](const Runner* a, const Runner* b){ return a->stats.equity > b->stats.equity; });

    std::string rows;
    for (auto* r : sorted) {
        const auto& s = r->stats;
        const char* col = s.equity >= 0 ? "#22c55e" : "#ef4444";
        char buf[512];
        snprintf(buf, sizeof(buf),
            "<tr><td><strong>%s</strong></td>"
            "<td>%d</td><td>%.1f%%</td>"
            "<td style='color:%s;font-weight:bold'>$%.2f</td>"
            "<td>$%.2f</td>"
            "<td style='color:#ef4444'>-$%.2f</td>"
            "<td>%.2f</td></tr>\n",
            s.name.c_str(), s.n(), s.wr(), col, s.equity,
            s.avg(), s.dd, s.sharpe());
        rows += buf;
    }

    // Monthly breakdown
    std::unordered_map<std::string, std::unordered_map<std::string,double>> monthly;
    std::vector<std::string> months_ordered;
    for (auto* r : sorted) {
        for (auto& t : r->stats.trades) {
            time_t ts = (time_t)(t.close_ts / 1000);
            struct tm ti{}; gmtime_r(&ts, &ti);
            char mo[8]; strftime(mo, 8, "%Y-%m", &ti);
            std::string mos(mo);
            if (monthly[r->stats.name].find(mos) == monthly[r->stats.name].end()) {
                bool found = false;
                for (auto& m : months_ordered) if (m == mos) { found = true; break; }
                if (!found) months_ordered.push_back(mos);
            }
            monthly[r->stats.name][mos] += t.pnl;
        }
    }
    std::sort(months_ordered.begin(), months_ordered.end());

    std::string mo_header = "<tr><th>Engine</th>";
    for (auto& m : months_ordered) mo_header += "<th>" + m + "</th>";
    mo_header += "</tr>\n";

    std::string mo_rows;
    for (auto* r : sorted) {
        mo_rows += "<tr><td>" + r->stats.name + "</td>";
        for (auto& m : months_ordered) {
            auto it = monthly[r->stats.name].find(m);
            double v = it != monthly[r->stats.name].end() ? it->second : 0.0;
            const char* c = v > 0 ? "#22c55e" : (v < 0 ? "#ef4444" : "#6b7280");
            char b[64]; snprintf(b, 64, "<td style='color:%s'>$%.0f</td>", c, v);
            mo_rows += b;
        }
        mo_rows += "</tr>\n";
    }

    char html[65536];
    snprintf(html, sizeof(html), R"(<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8">
<title>Omega Backtest Report</title>
<style>
body{font-family:system-ui,sans-serif;background:#0f172a;color:#e2e8f0;margin:0;padding:2rem}
h1{color:#f59e0b} .meta{color:#94a3b8;font-size:.85rem;margin-bottom:2rem}
table{border-collapse:collapse;width:100%%;margin-bottom:2rem}
th{background:#1e293b;color:#94a3b8;padding:.6rem 1rem;text-align:left;font-size:.8rem;text-transform:uppercase}
td{padding:.55rem 1rem;border-bottom:1px solid #1e293b;font-size:.9rem}
tr:hover td{background:#1e293b} h2{color:#94a3b8;font-size:1rem;text-transform:uppercase;margin:2rem 0 .5rem}
</style></head><body>
<h1>⚡ Omega Backtest Report</h1>
<div class="meta">%lld ticks &nbsp;|&nbsp; %.1fs elapsed &nbsp;|&nbsp; %.0f K t/s</div>
<h2>Engine Performance (0.01 lot · $1/point · 600s max hold)</h2>
<table><tr><th>Engine</th><th>Trades</th><th>Win Rate</th>
<th>Total PnL</th><th>Avg PnL</th><th>Max DD</th><th>Sharpe</th></tr>
%s</table>
<h2>Monthly PnL</h2>
<table>%s%s</table>
<p style="color:#475569;font-size:.8rem">0.01 lot · $1/point · no commission · 600s max hold</p>
</body></html>)",
        (long long)tick_count, elapsed_s, tick_count / elapsed_s / 1000.0,
        rows.c_str(), mo_header.c_str(), mo_rows.c_str());

    std::string path = std::string(RESULTS_DIR) + "/report.html";
    FILE* f = fopen(path.c_str(), "w");
    if (f) { fputs(html, f); fclose(f); }
    printf("  Wrote %s\n", path.c_str());
}

static void write_trades_csv(const std::vector<Runner>& runners) {
    std::string path = std::string(RESULTS_DIR) + "/all_trades.csv";
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    fprintf(f, "engine,side,entry,exit,pnl,mfe,mae,open_ts,close_ts,reason\n");
    for (auto& r : runners)
        for (auto& t : r.stats.trades)
            fprintf(f, "%s,%d,%.5f,%.5f,%.4f,%.4f,%.4f,%lld,%lld,%s\n",
                    t.engine.c_str(), t.side, t.entry, t.exit_px, t.pnl,
                    t.mfe, t.mae, (long long)t.open_ts, (long long)t.close_ts,
                    t.reason.c_str());
    fclose(f);
    printf("  Wrote %s\n", path.c_str());
}

static void write_summary_csv(const std::vector<Runner>& runners) {
    std::string path = std::string(RESULTS_DIR) + "/engine_summary.csv";
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    fprintf(f, "engine,trades,win_rate_pct,total_pnl,avg_pnl,max_dd,sharpe\n");
    std::vector<const Runner*> sorted;
    for (auto& r : runners) sorted.push_back(&r);
    std::sort(sorted.begin(), sorted.end(),
              [](const Runner* a, const Runner* b){ return a->stats.equity > b->stats.equity; });
    for (auto* r : sorted) {
        const auto& s = r->stats;
        fprintf(f, "%s,%d,%.1f,%.2f,%.2f,%.2f,%.3f\n",
                s.name.c_str(), s.n(), s.wr(), s.equity, s.avg(), s.dd, s.sharpe());
    }
    fclose(f);
    printf("  Wrote %s\n", path.c_str());
}

// =============================================================================
// main
// =============================================================================
int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: omega_bt <ticks.csv> [--max N] [--hold S]\n");
        return 1;
    }
    const char* csv_path = argv[1];
    int64_t max_ticks = 0;
    int max_hold = 600;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--max")  && i+1<argc) max_ticks = atoll(argv[++i]);
        if (!strcmp(argv[i], "--hold") && i+1<argc) max_hold  = atoi(argv[++i]);
    }

    // Make results dir
    mkdir(RESULTS_DIR, 0755);

    printf("================================================================\n");
    printf("  Omega C++ Backtester\n");
    printf("  File    : %s\n", csv_path);
    printf("  MaxHold : %ds    MaxTicks: %s\n",
           max_hold, max_ticks ? std::to_string(max_ticks).c_str() : "all");
    printf("================================================================\n");

    // Load
    printf("[LOAD] Reading CSV...\n");
    auto t0l = std::chrono::steady_clock::now();
    std::vector<Tick> ticks = load_csv(csv_path);
    double load_s = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0l).count();
    if (max_ticks && (int64_t)ticks.size() > max_ticks) ticks.resize((size_t)max_ticks);
    printf("[LOAD] %zu ticks in %.1fs\n", ticks.size(), load_s);

    if (ticks.empty()) { fprintf(stderr,"[ERROR] No valid ticks\n"); return 1; }

    // Date range
    {
        time_t a=ticks.front().ts_ms/1000, b=ticks.back().ts_ms/1000;
        struct tm ma{},mb{}; gmtime_r(&a,&ma); gmtime_r(&b,&mb);
        char sa[20],sb[20]; strftime(sa,20,"%Y-%m-%d",&ma); strftime(sb,20,"%Y-%m-%d",&mb);
        printf("[RANGE] %s → %s (%zu ticks)\n", sa, sb, ticks.size());
    }

    // Build runners
    std::vector<Runner> runners;
    runners.emplace_back(std::make_unique<CompressionBreakout>());
    runners.emplace_back(std::make_unique<WickRejection>());
    runners.emplace_back(std::make_unique<DonchianBreakout>());
    runners.emplace_back(std::make_unique<NR3Breakout>());
    runners.emplace_back(std::make_unique<IntradaySeasonality>());
    runners.emplace_back(std::make_unique<SessionMomentum>());
    runners.emplace_back(std::make_unique<MeanReversion>());
    runners.emplace_back(std::make_unique<SpikeFade>());
    runners.emplace_back(std::make_unique<DynamicRange>());
    for (auto& r : runners) r.max_hold_sec = max_hold;

    VWAP vwap_calc;
    EWM  ewm_calc;

    printf("[RUN] %zu engines × %zu ticks...\n\n", runners.size(), ticks.size());
    auto t0r = std::chrono::steady_clock::now();
    const int64_t N = (int64_t)ticks.size();
    int64_t last_p = 0;

    for (int64_t i = 0; i < N; ++i) {
        const Tick& t = ticks[(size_t)i];
        double vwap  = vwap_calc.update(t);
        double drift = ewm_calc.update(t.mid());
        for (auto& r : runners) r.tick(t, vwap, drift);

        if (i - last_p >= 500'000) {
            last_p = i;
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0r).count();
            int64_t nt = 0; for (auto& r : runners) nt += r.stats.n();
            printf("\r  [%5.1f%%]  %lld ticks | %5.0fs | %5.0f K t/s | %lld trades | ETA %4.0fs   ",
                   100.0*i/N, (long long)i, el, i/el/1000.0, (long long)nt,
                   (i>0?(N-i)/(i/el):0.0));
            fflush(stdout);
        }
    }

    double run_s = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0r).count();
    printf("\n\n[RUN] %lld ticks in %.1fs = %.0f K t/s\n\n", (long long)N, run_s, N/run_s/1000.0);

    // Summary table
    printf("================================================================\n");
    printf("  %-28s %7s %6s %10s %7s %8s\n","Engine","Trades","WR%","PnL($)","Sharpe","MaxDD");
    printf("  %s\n", std::string(72,'-').c_str());

    std::vector<Runner*> sorted;
    for (auto& r : runners) sorted.push_back(&r);
    std::sort(sorted.begin(),sorted.end(),[](Runner*a,Runner*b){return a->stats.equity>b->stats.equity;});

    double total_pnl = 0; int total_trades = 0;
    for (auto* r : sorted) {
        const auto& s = r->stats;
        const char* flag = s.equity >= 0 ? "✅" : "❌";
        printf("  %s %-26s %7d %5.1f%% %+10.2f %7.2f %8.2f\n",
               flag, s.name.c_str(), s.n(), s.wr(), s.equity, s.sharpe(), s.dd);
        total_pnl += s.equity; total_trades += s.n();
    }
    printf("  %s\n", std::string(72,'-').c_str());
    printf("  TOTAL  %d trades  PnL=%+.2f  (0.01 lot · $1/pt · no commission)\n\n",
           total_trades, total_pnl);

    // Write outputs
    printf("[OUTPUT] Writing reports...\n");
    write_trades_csv(runners);
    write_summary_csv(runners);
    write_html(runners, N, run_s);

    printf("\n✅  open %s/report.html\n\n", RESULTS_DIR);
    return 0;
}

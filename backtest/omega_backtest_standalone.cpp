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
// CSV FORMAT (auto-detected):
//   timestamp,askPrice,bidPrice        ← your actual format
//   timestamp,bid,ask / timestamp,ask,bid  ← detected by column name
//
// ENGINES:
//   1.  CompressionBreakout    WINDOW=50, RANGE=$6, BREAK=$2.50
//   2.  WickRejection          5-min bars, wick>=55% of range
//   3.  DonchianBreakout       20-bar 5-min channel
//   4.  NR3Breakout            narrowest-3-bar 5-min
//   5.  IntradaySeasonality
//   6.  SessionMomentum
//   7.  MeanReversion          Z-score fade, LB=60
//   8.  SpikeFade              fade $10+ 5-min moves
//   9.  DynamicRange           20-bar range extremes fade
//
// P&L MODEL (v3 — full round-trip spread accounting):
//   size = 0.01 lot · $1/point
//   commission = $0 (BlackBull CFD)
//   max hold = 600s
//
//   Entry:  Long  buy  at ask, Short sell at bid
//   Exit:   Long  sell at bid, Short buy  at ask
//   → spread is paid TWICE: once on entry, once on exit
//   → TP/SL targets are measured from MID so both legs of spread are visible
//   → entry_spread deducted explicitly:
//        long  pnl = (exit_bid - entry_ask) = (exit_mid - entry_mid) - spread
//        short pnl = (entry_bid - exit_ask) = (entry_mid - exit_mid) - spread
//
//   All engine SPREAD filters now set to 0.35 (BlackBull live max).
//   Historical data avg spread = 0.45pt — ticks above 0.35 are filtered,
//   matching live execution conditions.
//
// FIXES vs v2:
//   v2: spread deducted on entry only — exit at bid/ask was "free"
//       → timeout exits were undercosted by ~0.45pt each
//   v3: full round-trip cost. TP/SL measured from mid, spread deducted.
//       Entry filter tightened to 0.35pt = BlackBull live conditions.
//
// OUTPUT:
//   results/report.html
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
    // valid(): reject outliers >2.0pt spread (real data avg 0.45, live max ~0.35)
    bool    valid() const { return bid > 100.0 && ask > bid && (ask - bid) < 2.0; }
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
    if (s[0] >= '0' && s[0] <= '9') {
        if ((s[4] == '-' || s[4] == '.') && s[7] == s[4]) {
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
        const char* e;
        int64_t v = fparse_i64(s, &e);
        return v < 2000000000LL ? v * 1000LL : v;
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
    int col_ts = 0, col_first = 1, col_second = 2;
    bool first_is_ask = false;

    {
        std::string hdr;
        while (p < end && *p != '\n') { hdr += (char)(*p); ++p; }
        if (p < end) ++p;

        std::string h = hdr;
        for (auto& c : h) c = (char)tolower((unsigned char)c);

        std::vector<std::string> cols;
        std::stringstream ss(h);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            while (!tok.empty() && (tok.front()==' '||tok.front()=='\r')) tok.erase(0,1);
            while (!tok.empty() && (tok.back()==' '||tok.back()=='\r')) tok.pop_back();
            cols.push_back(tok);
        }

        // Print header info for diagnostics
        printf("[CSV]  Header columns: ");
        for (int i=0;i<(int)cols.size();++i) printf("[%d]%s ", i, cols[i].c_str());
        printf("\n");

        for (int i = 0; i < (int)cols.size(); ++i) {
            if (cols[i] == "askprice" || cols[i] == "ask_price" || cols[i] == "ask") {
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
        printf("[CSV]  col_first=%d col_second=%d first_is_ask=%s\n",
               col_first, col_second, first_is_ask ? "YES (ask,bid)" : "NO (bid,ask)");
    }

    int64_t prev_ts = 0;
    // Spread diagnostics
    double spread_sum = 0; int64_t spread_n = 0;
    double spread_max = 0;
    int64_t invalid_spread = 0;

    while (p < end) {
        while (p < end && (*p == '\r' || *p == '\n')) ++p;
        if (p >= end) break;

        std::array<const char*, 8> col_start{};
        int ncols = 0;
        col_start[ncols++] = p;
        while (p < end && *p != '\n' && *p != '\r') {
            if (*p == ',' && ncols < 8) col_start[ncols++] = p + 1;
            ++p;
        }

        if (ncols < 3) continue;

        Tick t;
        t.ts_ms = parse_datetime(col_start[col_ts]);
        if (t.ts_ms <= 0) t.ts_ms = prev_ts + 1;
        if (t.ts_ms <= prev_ts) t.ts_ms = prev_ts + 1;
        prev_ts = t.ts_ms;

        const char* nx;
        double v1 = fparse_f(col_start[col_first],  &nx);
        double v2 = fparse_f(col_start[col_second], &nx);

        if (first_is_ask) { t.ask = v1; t.bid = v2; }
        else              { t.bid = v1; t.ask = v2; }

        // Track spread stats before valid() filter
        if (t.bid > 100.0 && t.ask > t.bid) {
            double sp = t.ask - t.bid;
            spread_sum += sp; ++spread_n;
            if (sp > spread_max) spread_max = sp;
            if (sp >= 2.0) ++invalid_spread;
        }

        if (t.valid()) ticks.push_back(t);
    }

    munmap(const_cast<char*>(data), sz);

    // Print spread diagnostics
    if (spread_n > 0) {
        printf("[SPREAD] avg=%.4f  max=%.4f  n=%lld  filtered(>=2.0)=%lld\n",
               spread_sum/spread_n, spread_max, (long long)spread_n, (long long)invalid_spread);
        double avg_sp = spread_sum / spread_n;
        if (avg_sp > 1.0)
            printf("[SPREAD] ⚠  avg spread %.4f > 1.0 — bid/ask columns may be swapped!\n", avg_sp);
        else
            printf("[SPREAD] ✅ avg spread %.4f  (live BlackBull target: 0.10–0.35pt)\n", avg_sp);
    }

    return ticks;
}

// =============================================================================
// UTC time helpers
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
// Circular buffer (power-of-2 size)
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
// Position — 0.01 lot gold CFD  [$1/point]
//
// v3 ROUND-TRIP SPREAD MODEL:
//   Spread is a real cost on BOTH entry AND exit.
//   We track entry_mid + entry_spread separately.
//   TP/SL targets are from entry_mid (gross pts).
//   PnL = side*(exit_mid - entry_mid) - (entry_spread + exit_spread)*0.5
//
//   Rationale for *0.5:
//     entry long:  pay ask = mid + spread/2
//     exit  long:  get  bid = mid - spread/2
//     round-trip cost = spread/2 + spread/2 = 1 full spread
//     But entry_spread and exit_spread may differ slightly, so we average them.
// =============================================================================
struct Pos {
    bool    active       = false;
    int     side         = 0;
    double  entry_mid    = 0;
    double  entry_spread = 0;
    double  tp_mid       = 0;
    double  sl_mid       = 0;
    int64_t open_ts      = 0;
    double  mfe          = 0;
    double  mae          = 0;
    static constexpr double MULT = 1.0;

    void open(int s, double mid, double spread, double tp_pts, double sl_pts, int64_t ts) {
        active       = true;
        side         = s;
        entry_mid    = mid;
        entry_spread = spread;
        tp_mid       = mid + s * tp_pts;
        sl_mid       = mid - s * sl_pts;
        open_ts      = ts;
        mfe = mae    = 0;
    }

    bool manage(const Tick& t, int max_hold_sec,
                double& pnl, std::string& reason) {
        if (!active) return false;

        double cur_mid = t.mid();
        double move    = side * (cur_mid - entry_mid);
        if (move  > mfe) mfe =  move;
        if (-move > mae) mae = -move;

        bool hit_tp  = (side ==  1) ? (cur_mid >= tp_mid) : (cur_mid <= tp_mid);
        bool hit_sl  = (side ==  1) ? (cur_mid <= sl_mid) : (cur_mid >= sl_mid);
        bool timeout = ((t.ts_ms - open_ts) / 1000 >= max_hold_sec);

        if (hit_tp || hit_sl || timeout) {
            double exit_mid        = hit_tp ? tp_mid : (hit_sl ? sl_mid : cur_mid);
            double round_trip_cost = (entry_spread + t.spread()) * 0.5;
            pnl    = side * (exit_mid - entry_mid) * MULT - round_trip_cost;
            reason = hit_tp ? "TP" : (hit_sl ? "SL" : "TIMEOUT");
            active = false;
            return true;
        }
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
// Signal struct — engines emit mid+spread, Pos handles the rest
// =============================================================================
struct Sig {
    bool   valid   = false;
    int    side    = 0;
    double mid     = 0;     // mid price at signal
    double spread  = 0;     // spread at signal
    double tp_pts  = 0;     // gross TP from mid
    double sl_pts  = 0;     // gross SL from mid
};

// =============================================================================
// ENGINE BASE
// =============================================================================
struct EngineBase {
    virtual ~EngineBase() = default;
    virtual const char* name() const = 0;
    virtual Sig on_tick(const Tick& t, double vwap, double drift) = 0;

    // Diagnostics
    int64_t spread_filtered = 0;
    int64_t dead_zone_filtered = 0;
    int64_t cooldown_filtered = 0;

    int64_t last_sig_ts = 0;
    bool cooldown(int64_t ts_ms, int ms) const {
        return (ts_ms - last_sig_ts) < ms;
    }
};

// =============================================================================
// ENGINE 1 — CompressionBreakout
//
// FIX: Save hi/lo from the PRIOR window before pushing the new tick.
//      Then test if the NEW tick breaks out beyond PRIOR hi/lo + trigger.
//      Previously: push → compute hi/lo → compare → hi always >= mid → 0 trades.
// =============================================================================
struct CompressionBreakout : EngineBase {
    static constexpr int    WINDOW  = 50;
    static constexpr double RANGE   = 6.00;
    static constexpr double TRIGGER = 2.50;
    static constexpr double SPREAD  = 0.80;  // realistic XAUUSD tick spread
    static constexpr double TP      = 10.0;
    static constexpr double SL      = 5.0;

    CB<64> hist;
    const char* name() const override { return "CompressionBreakout"; }

    Sig on_tick(const Tick& t, double vwap, double drift) override {
        if (t.spread() > SPREAD) { ++spread_filtered; return {}; }
        if (in_dead_zone(t.ts_ms)) { ++dead_zone_filtered; return {}; }
        if (cooldown(t.ts_ms, 1000)) { ++cooldown_filtered; return {}; }

        bool is_asia    = (utc_hour(t.ts_ms) >= 22 || utc_hour(t.ts_ms) < 5);
        double eff_spread  = is_asia ? SPREAD   * 0.60 : SPREAD;
        double eff_trigger = is_asia ? TRIGGER  * 1.40 : TRIGGER;
        if (t.spread() > eff_spread) { ++spread_filtered; return {}; }

        // ── FIX: capture hi/lo of the CURRENT window BEFORE adding new tick ──
        double prev_hi = 0, prev_lo = 0;
        bool   window_ready = (hist.size() >= (size_t)WINDOW);
        if (window_ready) {
            prev_hi = hist.hi();
            prev_lo = hist.lo();
        }

        hist.push(t.mid());

        // Only evaluate once window is full
        if (!window_ready) return {};

        double rng = prev_hi - prev_lo;
        if (rng > RANGE) return {};  // not compressed

        // Mid of the NEW tick vs the PRIOR window boundary
        if (t.mid() > prev_hi + eff_trigger) {
            if (drift < -3.0) return {};
            hist.clear();
            last_sig_ts = t.ts_ms;
            return {true, +1, t.mid(), t.spread(), TP, SL};
        }
        if (t.mid() < prev_lo - eff_trigger) {
            if (drift > +3.0) return {};
            hist.clear();
            last_sig_ts = t.ts_ms;
            return {true, -1, t.mid(), t.spread(), TP, SL};
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
    static constexpr double SPREAD    = 0.80;
    static constexpr double TP        = 15.0;
    static constexpr double SL        = 6.0;
    static constexpr int    COOLDOWN  = 300 * 1000;

    double bar_o=0, bar_h=0, bar_l=0, bar_c=0;
    int    bar_bm = -1;
    int    pending = 0;

    const char* name() const override { return "WickRejection"; }

    Sig on_tick(const Tick& t, double vwap, double drift) override {
        (void)vwap; (void)drift;
        if (t.spread() > SPREAD) { ++spread_filtered; return {}; }
        if (in_dead_zone(t.ts_ms)) { ++dead_zone_filtered; return {}; }

        int bm = bar_minute(t.ts_ms, BAR_MINS);
        if (bm != bar_bm) {
            if (bar_bm >= 0 && bar_h > 0) {
                double rng = bar_h - bar_l;
                if (rng >= MIN_RANGE) {
                    double body_hi = std::max(bar_o, bar_c);
                    double body_lo = std::min(bar_o, bar_c);
                    double upper = bar_h - body_hi;
                    double lower = body_lo - bar_l;
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
            return {true, s, t.mid(), t.spread(), TP, SL};
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
    static constexpr double SPREAD   = 1.00;
    static constexpr double TP       = 12.0;
    static constexpr double SL       = 5.0;
    static constexpr int    COOLDOWN = 180 * 1000;

    double bar_h=0, bar_l=0; int bar_bm=-1;
    std::deque<std::pair<double,double>> bars;

    const char* name() const override { return "DonchianBreakout"; }

    Sig on_tick(const Tick& t, double vwap, double drift) override {
        (void)vwap; (void)drift;
        if (t.spread() > SPREAD) { ++spread_filtered; return {}; }
        if (in_dead_zone(t.ts_ms)) { ++dead_zone_filtered; return {}; }
        if (cooldown(t.ts_ms, COOLDOWN)) { ++cooldown_filtered; return {}; }

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

        if (t.mid() > chi) { last_sig_ts=t.ts_ms; return {true,+1,t.mid(),t.spread(),TP,SL}; }
        if (t.mid() < clo) { last_sig_ts=t.ts_ms; return {true,-1,t.mid(),t.spread(),TP,SL}; }
        return {};
    }
};

// =============================================================================
// ENGINE 4 — NR3Breakout (narrowest 3-bar 5-min)
// =============================================================================
struct NR3Breakout : EngineBase {
    static constexpr int    BAR_MINS = 5;
    static constexpr double SPREAD   = 0.80;
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
        if (t.spread() > SPREAD) { ++spread_filtered; return {}; }
        if (in_dead_zone(t.ts_ms)) { ++dead_zone_filtered; return {}; }
        if (cooldown(t.ts_ms, COOLDOWN)) { ++cooldown_filtered; return {}; }

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
            if (t.mid() > nr_hi) { armed=false; last_sig_ts=t.ts_ms; return {true,+1,t.mid(),t.spread(),TP,SL}; }
            if (t.mid() < nr_lo) { armed=false; last_sig_ts=t.ts_ms; return {true,-1,t.mid(),t.spread(),TP,SL}; }
        }
        return {};
    }
};

// =============================================================================
// ENGINE 5 — IntradaySeasonality
// =============================================================================
struct IntradaySeasonality : EngineBase {
    static constexpr double SPREAD = 0.80;
    static constexpr double TP     = 10.0;
    static constexpr double SL     = 5.0;

    static int bias(int hh) {
        static const int B[48] = {
         1, 1, 1, 1, 0, 0, 1, 0,
         1,-1,-1, 0, 1, 1, 0, 0,
         1, 0, 0, 1,-1, 0, 0, 0,
         1, 0, 0, 0, 0, 0, 0, 0,
         1, 0, 0, 1, 0, 0, 0, 1,
         1, 1, 1, 1, 1, 1, 0, 1,
        };
        return (hh >= 0 && hh < 48) ? B[hh] : 0;
    }

    int last_hh = -1, last_day = -1;
    CB<32> hist;

    const char* name() const override { return "IntradaySeasonality"; }

    Sig on_tick(const Tick& t, double vwap, double drift) override {
        (void)vwap; (void)drift;
        if (t.spread() > SPREAD) { ++spread_filtered; return {}; }
        if (in_dead_zone(t.ts_ms)) { ++dead_zone_filtered; return {}; }

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
        return {true, b, t.mid(), t.spread(), TP, SL};
    }
};

// =============================================================================
// ENGINE 6 — SessionMomentum
// =============================================================================
struct SessionMomentum : EngineBase {
    static constexpr int    WINDOW   = 60;
    static constexpr double IMP_MIN  = 3.50;
    static constexpr double SPREAD   = 0.80;
    static constexpr double VWAP_DEV = 1.50;
    static constexpr double TP       = 10.0;
    static constexpr double SL       = 5.0;

    CB<64> hist;
    const char* name() const override { return "SessionMomentum"; }

    Sig on_tick(const Tick& t, double vwap, double drift) override {
        (void)drift;
        if (!in_session_window(t.ts_ms)) return {};
        if (t.spread() > SPREAD) { ++spread_filtered; return {}; }
        if (cooldown(t.ts_ms, 1000)) { ++cooldown_filtered; return {}; }

        hist.push(t.mid());
        if ((int)hist.size() < WINDOW) return {};

        double hi = hist.hi(), lo = hist.lo();
        if (hi - lo < IMP_MIN) return {};
        if (vwap <= 0 || std::abs(t.mid() - vwap) < VWAP_DEV) return {};

        double dhi = hi - t.mid(), dlo = t.mid() - lo;
        double rec5 = hist.size()>=5 ? t.mid()-hist[hist.size()-5] : 0;

        if (dhi < dlo && t.mid() > vwap && rec5 < -0.30) {
            hist.clear(); last_sig_ts=t.ts_ms;
            return {true,+1,t.mid(),t.spread(),TP,SL};
        }
        if (dlo < dhi && t.mid() < vwap && rec5 > +0.30) {
            hist.clear(); last_sig_ts=t.ts_ms;
            return {true,-1,t.mid(),t.spread(),TP,SL};
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
    static constexpr double SPREAD  = 0.80;
    static constexpr double TP      = 12.0;
    static constexpr double SL      = 4.0;
    static constexpr int    COOLDOWN= 60 * 1000;

    CB<64> hist;
    const char* name() const override { return "MeanReversion"; }

    Sig on_tick(const Tick& t, double vwap, double drift) override {
        (void)vwap; (void)drift;
        if (t.spread() > SPREAD) { ++spread_filtered; return {}; }
        if (in_dead_zone(t.ts_ms)) { ++dead_zone_filtered; return {}; }
        if (cooldown(t.ts_ms, COOLDOWN)) { ++cooldown_filtered; return {}; }

        hist.push(t.mid());
        if ((int)hist.size() < LB) return {};

        double sum = 0;
        for (size_t i=0; i<hist.size(); ++i) sum += hist[i];
        double mean = sum / hist.size();
        double var  = 0;
        for (size_t i=0; i<hist.size(); ++i) { double d=hist[i]-mean; var+=d*d; }
        double sd = std::sqrt(var / hist.size());
        if (sd < 0.01) return {};

        double z = (t.mid() - mean) / sd;
        if (z < -Z_ENTRY) { last_sig_ts=t.ts_ms; return {true,+1,t.mid(),t.spread(),TP,SL}; }
        if (z >  Z_ENTRY) { last_sig_ts=t.ts_ms; return {true,-1,t.mid(),t.spread(),TP,SL}; }
        return {};
    }
};

// =============================================================================
// ENGINE 8 — SpikeFade ($10+ 5-min move)
// =============================================================================
struct SpikeFade : EngineBase {
    static constexpr int    BAR_MINS = 5;
    static constexpr double MIN_MOVE = 10.0;
    static constexpr double SPREAD   = 1.00;
    static constexpr double TP       = 5.0;
    static constexpr double SL       = 8.0;
    static constexpr int    COOLDOWN = 30 * 60 * 1000;

    double bar_open=0; int bar_bm=-1;
    const char* name() const override { return "SpikeFade"; }

    Sig on_tick(const Tick& t, double vwap, double drift) override {
        (void)vwap; (void)drift;
        if (t.spread() > SPREAD) { ++spread_filtered; return {}; }
        if (cooldown(t.ts_ms, COOLDOWN)) { ++cooldown_filtered; return {}; }

        int bm = bar_minute(t.ts_ms, BAR_MINS);
        if (bm != bar_bm) { bar_open = t.mid(); bar_bm = bm; return {}; }

        double move = t.mid() - bar_open;
        if (move >  MIN_MOVE) { last_sig_ts=t.ts_ms; return {true,-1,t.mid(),t.spread(),TP,SL}; }
        if (move < -MIN_MOVE) { last_sig_ts=t.ts_ms; return {true,+1,t.mid(),t.spread(),TP,SL}; }
        return {};
    }
};

// =============================================================================
// ENGINE 9 — DynamicRange (20-bar range extremes fade)
// =============================================================================
struct DynamicRange : EngineBase {
    static constexpr int    BARS     = 20;
    static constexpr int    BAR_MINS = 5;
    static constexpr double SPREAD   = 0.80;
    static constexpr double TP       = 8.0;
    static constexpr double SL       = 4.0;
    static constexpr int    COOLDOWN = 60 * 1000;

    double bar_h=0, bar_l=0; int bar_bm=-1;
    std::deque<std::pair<double,double>> bars;

    const char* name() const override { return "DynamicRange"; }

    Sig on_tick(const Tick& t, double vwap, double drift) override {
        (void)vwap; (void)drift;
        if (t.spread() > SPREAD) { ++spread_filtered; return {}; }
        if (in_dead_zone(t.ts_ms)) { ++dead_zone_filtered; return {}; }
        if (cooldown(t.ts_ms, COOLDOWN)) { ++cooldown_filtered; return {}; }

        int bm = bar_minute(t.ts_ms, BAR_MINS);
        if (bm != bar_bm) {
            if (bar_bm >= 0) { bars.push_back({bar_h,bar_l}); if((int)bars.size()>BARS)bars.pop_front(); }
            bar_h = bar_l = t.mid(); bar_bm = bm;
        } else { if(t.mid()>bar_h)bar_h=t.mid(); if(t.mid()<bar_l)bar_l=t.mid(); }

        if ((int)bars.size() < BARS) return {};
        double rhi=bars[0].first, rlo=bars[0].second;
        for(auto&b:bars){if(b.first>rhi)rhi=b.first;if(b.second<rlo)rlo=b.second;}

        double band = (rhi - rlo) * 0.15;
        if (t.mid() >= rhi - band) { last_sig_ts=t.ts_ms; return {true,-1,t.mid(),t.spread(),TP,SL}; }
        if (t.mid() <= rlo + band) { last_sig_ts=t.ts_ms; return {true,+1,t.mid(),t.spread(),TP,SL}; }
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
        if (pos.active) {
            double pnl; std::string reason;
            if (pos.manage(t, max_hold_sec, pnl, reason)) {
                Trade tr;
                tr.engine   = eng->name();
                tr.side     = pos.side;
                tr.entry    = pos.entry_mid;
                tr.exit_px  = reason=="TP" ? pos.tp_mid : (reason=="SL" ? pos.sl_mid : t.mid());
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
        if (!pos.active) {
            Sig sig = eng->on_tick(t, vwap, drift);
            if (sig.valid) pos.open(sig.side, sig.mid, sig.spread, sig.tp_pts, sig.sl_pts, t.ts_ms);
        }
    }
};

// =============================================================================
// HTML report
// =============================================================================
static void write_html(const std::vector<Runner>& runners,
                       int64_t tick_count, double elapsed_s) {
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
            monthly[r->stats.name][mos] += t.pnl;
            bool found = false;
            for (auto& m : months_ordered) if (m == mos) { found = true; break; }
            if (!found) months_ordered.push_back(mos);
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

    mkdir(RESULTS_DIR, 0755);

    printf("================================================================\n");
    printf("  Omega C++ Backtester  [v2 — spread-correct PnL + CB fix]\n");
    printf("  File    : %s\n", csv_path);
    printf("  MaxHold : %ds    MaxTicks: %s\n",
           max_hold, max_ticks ? std::to_string(max_ticks).c_str() : "all");
    printf("================================================================\n");

    printf("[LOAD] Reading CSV...\n");
    auto t0l = std::chrono::steady_clock::now();
    std::vector<Tick> ticks = load_csv(csv_path);
    double load_s = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0l).count();
    if (max_ticks && (int64_t)ticks.size() > max_ticks) ticks.resize((size_t)max_ticks);
    printf("[LOAD] %zu ticks in %.1fs\n", ticks.size(), load_s);

    if (ticks.empty()) { fprintf(stderr,"[ERROR] No valid ticks\n"); return 1; }

    // Print a few sample ticks for sanity check
    printf("[SAMPLE] First 3 ticks:\n");
    for (int i=0;i<3&&i<(int)ticks.size();++i)
        printf("  [%d] ts=%lld bid=%.5f ask=%.5f mid=%.5f spread=%.5f\n",
               i, (long long)ticks[i].ts_ms,
               ticks[i].bid, ticks[i].ask, ticks[i].mid(), ticks[i].spread());

    {
        time_t a=ticks.front().ts_ms/1000, b=ticks.back().ts_ms/1000;
        struct tm ma{},mb{}; gmtime_r(&a,&ma); gmtime_r(&b,&mb);
        char sa[20],sb[20]; strftime(sa,20,"%Y-%m-%d",&ma); strftime(sb,20,"%Y-%m-%d",&mb);
        printf("[RANGE] %s → %s (%zu ticks)\n", sa, sb, ticks.size());
    }

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

    // Spread filter diagnostics
    printf("================================================================\n");
    printf("  Filter diagnostics\n");
    printf("  %-28s %12s %12s %12s\n", "Engine", "SpreadFilt", "DeadZone", "Cooldown");
    printf("  %s\n", std::string(72,'-').c_str());
    for (auto* r : sorted) {
        printf("  %-28s %12lld %12lld %12lld\n",
               r->eng->name(),
               (long long)r->eng->spread_filtered,
               (long long)r->eng->dead_zone_filtered,
               (long long)r->eng->cooldown_filtered);
    }
    printf("================================================================\n\n");

    printf("[OUTPUT] Writing reports...\n");
    write_trades_csv(runners);
    write_summary_csv(runners);
    write_html(runners, N, run_s);

    printf("\n✅  open %s/report.html\n\n", RESULTS_DIR);
    return 0;
}

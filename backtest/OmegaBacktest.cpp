// =============================================================================
// OmegaBacktest.cpp -- Native C++ backtester for all Omega engine families
//
// BUILD:
//   cmake --build build --target OmegaBacktest --config Release
//
// RUN:
//   OmegaBacktest.exe <ticks.csv> [options]
//
//   --latency <ms>     simulated execution latency ms    (default: 1.0)
//   --report  <file>   per-engine stats CSV              (default: bt_report.csv)
//   --trades  <file>   all trade records CSV             (default: bt_trades.csv)
//   --warmup  <n>      ticks before recording trades     (default: 5000)
//   --engine  <list>   comma list (default: gold,latency,cross)
//                      legacy:  gold, latency, cross, stoprun,
//                               ofade, omom, amom, lfade, rsirev, allnew
//                      S44 new: hybridgold, macrocrash, h1swing, h4regime,
//                               minh4, pullbackcont, pullbackprem, pdhl,
//                               rsiextreme, emacross
//                      master:  all  (= every legacy + every S44 runner)
//
// TICK CSV -- auto-detected formats:
//   A:  timestamp_ms,bid,ask
//   B:  timestamp_ms,bid,ask,vol
//   C:  YYYY.MM.DD,HH:MM:SS.mmm,bid,ask,vol   (Dukascopy)
//   D:  timestamp_ms,open,high,low,close,vol   (OHLCV -- uses close±0.15 spread)
//
// OUTPUT:
//   Console       live progress + per-engine summary table
//   bt_trades.csv all trades (compatible with scripts/shadow_analysis.py)
//   bt_report.csv per-engine aggregate stats
//
// S44 -- 2026-04-26 -- Coverage expansion
//   Added 10 runners to lift gold-engine coverage from 5/16 to 15/16
//   (TrendPullback remains shelved; structurally absent here too).
//   New runners reuse BtBarEngine.hpp for HTF bar context, PDHL tracker,
//   EWM drift, vol-ratio and M15 expansion. Defaults unchanged.
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
#include <functional>
#include <iostream>
#include <memory>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <io.h>      // _dup, _dup2, _close on Windows
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#include "../include/OmegaTradeLedger.hpp"
#include "../include/GoldEngineStack.hpp"
#include "../include/StopRunReversalEngine.hpp"
#include "../include/OverlapFadeEngine.hpp"
#include "../include/StructuralEdgeEngines.hpp"
// (GoldFlowEngine.hpp removed at S19 Stage 1B — engine culled.)
#include "../include/LatencyEdgeEngines.hpp"
#include "../include/CrossAssetEngines.hpp"
#include "../include/BracketEngine.hpp"
#include "../include/RSIReversalEngine.hpp"
// (MicroMomentumEngine.hpp removed at Batch 5V §1.2 2026-04-20.)
#include "../include/GoldHybridBracketEngine.hpp"

// S44: new engine headers required by added runners
#include "../include/MacroCrashEngine.hpp"
#include "../include/HTFSwingEngines.hpp"
#include "../include/MinimalH4Breakout.hpp"
#include "../include/PullbackContEngine.hpp"
#include "../include/PDHLReversionEngine.hpp"
#include "../include/RSIExtremeTurnEngine.hpp"
#include "../include/EMACrossEngine.hpp"

// S46: T3 -- daily-bar TSMOM engine for XAUUSD (Singha 2025-style)
#include "../include/TSMomGoldEngine.hpp"

// =============================================================================
// Memory-mapped file
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
        if (hFile_ != INVALID_HANDLE_VALUE) CloseHandle(hFile_);
#else
        if (data) munmap(const_cast<char*>(data), size);
        if (fd_ >= 0) ::close(fd_);
#endif
    }
private:
#ifdef _WIN32
    HANDLE hFile_ = INVALID_HANDLE_VALUE;
    HANDLE hMap_  = nullptr;
#else
    int fd_ = -1;
#endif
};

// =============================================================================
// Fast scalar CSV parsing
// =============================================================================
static inline double fast_f(const char* s, const char** e) noexcept {
    while (*s == ' ') ++s;
    bool n = (*s == '-'); if (n) ++s;
    double v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10.0 + (*s++ - '0');
    if (*s == '.') { ++s; double f=0.1; while (*s>='0'&&*s<='9'){v+=(*s++-'0')*f;f*=.1;} }
    if (e) *e = s;
    return n ? -v : v;
}
static inline int64_t fast_i64(const char* s, const char** e) noexcept {
    while (*s == ' ' || *s == '"') ++s;
    int64_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    if (e) *e = s;
    return v;
}

static int64_t duka_ts(const char* d, const char* t) noexcept {
    int y  = (d[0]-'0')*1000+(d[1]-'0')*100+(d[2]-'0')*10+(d[3]-'0');
    int mo = (d[5]-'0')*10+(d[6]-'0');
    int dy = (d[8]-'0')*10+(d[9]-'0');
    int h  = (t[0]-'0')*10+(t[1]-'0');
    int mi = (t[3]-'0')*10+(t[4]-'0');
    int se = (t[6]-'0')*10+(t[7]-'0');
    int ms = 0;
    if (t[8]=='.') {
        int p=0; const char* q=t+9;
        while (*q>='0'&&*q<='9'&&p<3){ms=ms*10+(*q++-'0');++p;}
        while (p++<3) ms*=10;
    }
    if (mo<=2){--y;mo+=12;}
    int64_t days = 365LL*y + y/4 - y/100 + y/400 + (153*mo+8)/5 + dy - 719469LL;
    return (days*86400LL + h*3600LL + mi*60LL + se)*1000LL + ms;
}

struct TickRow { int64_t ts_ms; double bid, ask; };
enum class Fmt  { BA, DUKA, OHLCV };

static Fmt sniff(const char* p) noexcept {
    if (p[4]=='.'||p[4]=='-') return Fmt::DUKA;
    int c=0; for(const char*q=p;*q&&*q!='\n';++q) if(*q==',')++c;
    return c>=4 ? Fmt::OHLCV : Fmt::BA;
}

static std::vector<TickRow> parse_csv(const MemMappedFile& f) {
    std::vector<TickRow> v; v.reserve(130'000'000);
    const char* p   = f.data;
    const char* end = p + f.size;
    if (f.size>=3&&(uint8_t)p[0]==0xEF&&(uint8_t)p[1]==0xBB&&(uint8_t)p[2]==0xBF) p+=3;
    bool ask_first = false;
    if (*p && (*p<'0'||*p>'9')) {
        const char* ask_pos = nullptr;
        const char* bid_pos = nullptr;
        while (p<end && *p!='\n') {
            if (!ask_pos && (p[0]=='a'||p[0]=='A') && (p[1]=='s'||p[1]=='S') && (p[2]=='k'||p[2]=='K')) ask_pos=p;
            if (!bid_pos && (p[0]=='b'||p[0]=='B') && (p[1]=='i'||p[1]=='I') && (p[2]=='d'||p[2]=='D')) bid_pos=p;
            ++p;
        }
        if (ask_pos && bid_pos && ask_pos < bid_pos) ask_first = true;
        if (p<end) ++p;
    }
    if (ask_first) fprintf(stderr, "[CSV] ask-first format detected -- swapping columns\n");
    Fmt fmt = sniff(p);

    while (p < end) {
        while (p<end&&(*p=='\r'||*p=='\n'))++p;
        if (p>=end) break;
        TickRow r{};
        const char* nx;
        if (fmt == Fmt::DUKA) {
            const char* dp=p; while(p<end&&*p!=',')++p; if(p>=end)break; ++p;
            const char* tp=p; while(p<end&&*p!=',')++p; if(p>=end)break; ++p;
            r.ts_ms=duka_ts(dp,tp);
            r.bid=fast_f(p,&nx);p=nx;if(*p==',')++p;
            r.ask=fast_f(p,&nx);p=nx;
        } else if (fmt == Fmt::OHLCV) {
            r.ts_ms=fast_i64(p,&nx);p=nx;if(*p==',')++p;
            fast_f(p,&nx);p=nx;if(*p==',')++p;
            fast_f(p,&nx);p=nx;if(*p==',')++p;
            fast_f(p,&nx);p=nx;if(*p==',')++p;
            double cl=fast_f(p,&nx);p=nx;
            r.bid=cl-0.15; r.ask=cl+0.15;
        } else {
            r.ts_ms=fast_i64(p,&nx);p=nx;if(*p==',')++p;
            double c1=fast_f(p,&nx); p=nx;if(*p==',')++p;
            double c2=fast_f(p,&nx); p=nx;
            if (ask_first) { r.ask=c1; r.bid=c2; }
            else            { r.bid=c1; r.ask=c2; }
        }
        while(p<end&&*p!='\n')++p;
        if(r.ts_ms>0&&r.bid>0&&r.ask>r.bid) v.push_back(r);
    }
    return v;
}

// =============================================================================
// Per-engine statistics
// =============================================================================
struct Stat {
    std::string name;
    int64_t n=0, wins=0;
    double  pnl=0, peak=0, dd=0, hold=0, sq=0;
    void add(double p, int64_t h) {
        ++n; if(p>0)++wins; pnl+=p; hold+=h; sq+=p*p;
        if(pnl>peak)peak=pnl;
        const double d=peak-pnl; if(d>dd)dd=d;
    }
    double wr()     const { return n?100.0*wins/n:0; }
    double avg()    const { return n?pnl/n:0; }
    double avgh()   const { return n?hold/n:0; }
    double sharpe() const {
        if(n<2)return 0;
        const double m=avg(), var=sq/n-m*m;
        return var>0?(m/std::sqrt(var))*std::sqrt((double)n):0;
    }
    void print() const {
        printf("  %-30s  %6lld T  WR=%5.1f%%  PnL=%+9.2f  Avg=%+6.2f  "
               "DD=%6.2f  Hold=%4.0fs  Sh=%.2f\n",
               name.c_str(),(long long)n,wr(),pnl,avg(),dd,avgh(),sharpe());
    }
};

// =============================================================================
// Global trade store
// =============================================================================
namespace store {
    static std::vector<omega::TradeRecord> recs;
    static std::unordered_map<std::string,Stat> stats;
    static int64_t warmup_sec = 0;

    static void add(const omega::TradeRecord& tr) {
        if (tr.entryTs < warmup_sec) return;
        recs.push_back(tr);
        auto& s = stats[tr.engine];
        if (s.name.empty()) s.name = tr.engine;
        s.add(tr.pnl, tr.exitTs - tr.entryTs);
    }
}
static auto cb() { return [](const omega::TradeRecord& t){ store::add(t); }; }

// =============================================================================
// VWAP tracker
// =============================================================================
struct BtVwap {
    double pv=0, vol=0; int day=-1;
    double update(double mid, int64_t ts_ms) noexcept {
        const time_t t = (time_t)(ts_ms/1000);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti,&t);
#else
        gmtime_r(&t,&ti);
#endif
        if (ti.tm_yday!=day){pv=vol=0;day=ti.tm_yday;}
        pv+=mid; vol+=1.0;
        return vol>0?pv/vol:mid;
    }
};

// =============================================================================
// Engine runners -- LEGACY (S43 and earlier)
// =============================================================================

struct GoldRunner {
    omega::gold::GoldEngineStack eng;
    double lat;
    double ewm_fast_ = 0.0, ewm_slow_ = 0.0;
    bool   ewm_init_ = false;
    static constexpr double ALPHA_FAST = 0.05;
    static constexpr double ALPHA_SLOW = 0.005;
    GoldRunner(double l):lat(l){}
    void tick(const TickRow& r){
        const double mid = (r.bid + r.ask) * 0.5;
        if (!ewm_init_) { ewm_fast_ = mid; ewm_slow_ = mid; ewm_init_ = true; }
        ewm_fast_ = ALPHA_FAST * mid + (1.0 - ALPHA_FAST) * ewm_fast_;
        ewm_slow_ = ALPHA_SLOW * mid + (1.0 - ALPHA_SLOW) * ewm_slow_;
        auto c=cb();
        (void)eng.on_tick(r.bid,r.ask,lat,c);
    }
};

struct LatencyRunner {
    omega::latency::LatencyEdgeStack eng;
    double lat;
    LatencyRunner(double l):lat(l){}
    void tick(const TickRow& r){ auto c=cb(); (void)eng.on_tick_gold(r.bid,r.ask,lat,c); }
};

// -----------------------------------------------------------------------------
// S44: Local M1 bar builder + Wilder ATR-14 + EMA-9/21/50.
// Mirrors include/OHLCBarEngine.hpp formulas exactly (_update_ema/_update_atr).
// Used to seed TPB with bar-scale ATR, matching live behaviour where
// seed_bar_emas() is called once g_bars_gold.m1.ind.m1_ready.
//
// Without this, atr_ in TPB is per-tick EWM (~0.05-0.10) instead of bar TR
// (~3-10), making any ATR-based gate analysis incomparable to live.
// -----------------------------------------------------------------------------
struct BtM1BarSeeder {
    // Live OHLCBarEngine constants
    static constexpr int EMA9_P  = 9;
    static constexpr int EMA21_P = 21;
    static constexpr int EMA50_P = 50;
    static constexpr int ATR_P   = 14;
    static constexpr int RSI_P   = 14;            // ready-bar count threshold (matches live)
    static constexpr int MAX_BARS = 300;          // matches live's bars_.pop_front() at >300

    // Closed-bar history (oldest -> newest)
    struct Bar { double open, high, low, close; };
    std::vector<Bar> bars;

    // Current (in-progress) bar
    int64_t cur_min = -1;
    Bar     cur{0,0,0,0};
    bool    cur_open = false;

    // Recursive accumulators (match live OHLCBarEngine private members)
    bool   ema_init = false;
    double ema9 = 0.0, ema21 = 0.0, ema50 = 0.0;
    bool   atr_init = false;
    double atr_avg = 0.0;

    bool ready() const { return static_cast<int>(bars.size()) >= RSI_P + 1; }

    // Recompute EMA9/21/50 from current bars_ — mirrors _update_ema()
    void update_ema() {
        const int n = static_cast<int>(bars.size());
        if (n < 1) return;
        if (!ema_init) {
            const int s9  = std::min(n, EMA9_P);
            const int s21 = std::min(n, EMA21_P);
            const int s50 = std::min(n, EMA50_P);
            double sum9=0, sum21=0, sum50=0;
            for (int i = n-s9;  i < n; ++i) sum9  += bars[i].close;
            for (int i = n-s21; i < n; ++i) sum21 += bars[i].close;
            for (int i = n-s50; i < n; ++i) sum50 += bars[i].close;
            ema9  = sum9  / s9;
            ema21 = sum21 / s21;
            ema50 = sum50 / s50;
            ema_init = true;
        } else {
            const double c = bars.back().close;
            ema9  += (2.0/(EMA9_P +1.0)) * (c - ema9);
            ema21 += (2.0/(EMA21_P+1.0)) * (c - ema21);
            ema50 += (2.0/(EMA50_P+1.0)) * (c - ema50);
        }
    }

    // Wilder ATR-14 — mirrors _update_atr()
    void update_atr() {
        const int n = static_cast<int>(bars.size());
        if (n < 1) return;
        if (!atr_init) {
            double sum = 0; int count = 0;
            for (int i = std::max(1, n - ATR_P); i < n; ++i) {
                const double tr = std::max({
                    bars[i].high - bars[i].low,
                    std::fabs(bars[i].high - bars[i-1].close),
                    std::fabs(bars[i].low  - bars[i-1].close)
                });
                sum += tr; ++count;
            }
            if (count == 0) {
                atr_avg = bars[0].high - bars[0].low;
                if (atr_avg <= 0.0) atr_avg = bars[0].close * 0.001;
            } else {
                atr_avg = sum / count;
            }
            atr_init = true;
        } else {
            const int i = n - 1;
            const double tr = (n >= 2) ? std::max({
                bars[i].high - bars[i].low,
                std::fabs(bars[i].high - bars[i-1].close),
                std::fabs(bars[i].low  - bars[i-1].close)
            }) : bars[i].high - bars[i].low;
            atr_avg = (atr_avg * (ATR_P-1) + tr) / ATR_P;
        }
    }

    // Update with a new tick. Returns true on minute roll (caller may want to seed).
    bool on_tick(double mid, int64_t ts_ms) {
        const int64_t bar_min = ts_ms / 60000LL;
        if (!cur_open) {
            cur = { mid, mid, mid, mid };
            cur_min = bar_min;
            cur_open = true;
            return false;
        }
        if (bar_min != cur_min) {
            // Close current bar -> push to history, recompute EMA/ATR
            bars.push_back(cur);
            if (static_cast<int>(bars.size()) > MAX_BARS) {
                bars.erase(bars.begin());
            }
            update_ema();
            update_atr();
            // Open new bar
            cur = { mid, mid, mid, mid };
            cur_min = bar_min;
            return true;
        }
        // Same minute: update H/L/C
        if (mid > cur.high) cur.high = mid;
        if (mid < cur.low ) cur.low  = mid;
        cur.close = mid;
        return false;
    }
};

struct CrossRunner {
    omega::cross::OpeningRangeEngine    orb;
    omega::cross::VWAPReversionEngine   vrev;
    omega::cross::TrendPullbackEngine   tpb;
    omega::cross::NoiseBandMomentumEngine nbe;
    BtVwap      vwap;
    BtM1BarSeeder m1seed;
    // S44 backtest harness: TPB defaults are indices-tuned (MIN_EMA_SEP=10).
    // Production main.cpp sets gold-specific values; mirror them here so the
    // 2-year XAUUSD backtest tests the live config, not the indices fallback.
    CrossRunner(){
        tpb.MIN_EMA_SEP = 5.0;  // gold: 5pt EMA stack separation (vs 10pt indices)
    }
    void tick(const TickRow& r){
        const std::string sym = "XAUUSD";
        const double mid  = (r.bid+r.ask)*0.5;
        const double vw   = vwap.update(mid, r.ts_ms);

        // S44: build M1 bars + indicators locally and seed TPB on every tick
        // once we have RSI_P+1 closed bars (mirrors live m1_ready gate at
        // tick_indices.hpp:116-121).
        m1seed.on_tick(mid, r.ts_ms);
        if (m1seed.ready()) {
            tpb.seed_bar_emas(m1seed.ema9, m1seed.ema21, m1seed.ema50, m1seed.atr_avg);
        }

        auto c=cb();
        (void)orb.on_tick(sym,r.bid,r.ask,c);
        (void)vrev.on_tick(sym,r.bid,r.ask,vw,c);
        (void)tpb.on_tick(sym,r.bid,r.ask,c);
        (void)nbe.on_tick(sym,r.bid,r.ask,c);
    }
};

struct StopRunRunner {
    omega::StopRunReversalEngine eng;
    int64_t warmup_ticks;
    int64_t tick_count = 0;
    int last_day = -1;
    StopRunRunner() { warmup_ticks = 200; }
    void tick(const TickRow& r) {
        tick_count++;
        // Session slot from timestamp
        int h = (int)((r.ts_ms/1000/3600) % 24);
        int slot = 0;
        if      (h >= 7  && h < 9)  slot = 1;
        else if (h >= 9  && h < 12) slot = 2;
        else if (h >= 12 && h < 14) slot = 3;
        else if (h >= 14 && h < 17) slot = 4;
        else if (h >= 17 && h < 22) slot = 5;
        else if (h >= 22 || h < 5)  slot = 6;
        // Daily reset
        int day = (int)(r.ts_ms / 1000 / 86400);
        if (day != last_day) { eng.reset_session(); last_day = day; }
        // Wire callback once
        if (!eng.on_close) {
            eng.on_close = [](const omega::TradeRecord& t){ store::add(t); };
        }
        eng.on_tick(r.bid, r.ask, r.ts_ms, slot);
    }
};

struct OverlapFadeRunner {
    omega::OverlapFadeEngine eng;
    int atr_tick = 0;
    int last_day = -1;
    static constexpr int BUF = 512;
    double prices[BUF] = {};
    int pidx = 0;

    void tick(const TickRow& r) {
        const double mid = (r.bid + r.ask) * 0.5;
        prices[pidx % BUF] = mid;
        pidx++;

        // Session slot
        int h = (int)((r.ts_ms/1000/3600) % 24);
        int slot = 0;
        if      (h>=7  && h<9)  slot=1;
        else if (h>=9  && h<12) slot=2;
        else if (h>=12 && h<14) slot=3;
        else if (h>=14 && h<17) slot=4;
        else if (h>=17 && h<22) slot=5;
        else if (h>=22 || h<5)  slot=6;

        // ATR = high-low range over 100 ticks (proper proxy)
        // Mean 1-tick move ~0.10pt != real ATR. Range = 1-5pt. Correct.
        if ((++atr_tick % 100) == 0 && pidx > 100) {
            int look = std::min(pidx-1, 100);
            double hi = prices[(pidx-1+BUF*4)%BUF], lo = hi;
            for (int k=1; k<look; k++) {
                double p = prices[(pidx-k+BUF*4)%BUF];
                if (p>hi) hi=p; if (p<lo) lo=p;
            }
            eng.seed_atr(hi - lo);
        }

        // Wire callback once
        if (!eng.on_close)
            eng.on_close = [](const omega::TradeRecord& t){ store::add(t); };

        eng.on_tick(r.bid, r.ask, r.ts_ms, slot);
    }
};


struct OverlapMomRunner {
    omega::OverlapMomentumEngine eng;
    void tick(const TickRow& r) {
        int h=(int)((r.ts_ms/1000/3600)%24);
        int slot=0;
        if(h>=7&&h<9)slot=1; else if(h>=9&&h<12)slot=2;
        else if(h>=12&&h<14)slot=3; else if(h>=14&&h<17)slot=4;
        else if(h>=17&&h<22)slot=5; else if(h>=22||h<5)slot=6;
        if(!eng.on_close) eng.on_close=[](const omega::TradeRecord&t){store::add(t);};
        eng.on_tick(r.bid,r.ask,r.ts_ms,slot);
    }
};
struct AsiaMomRunner {
    omega::AsiaMomentumEngine eng;
    void tick(const TickRow& r) {
        int h=(int)((r.ts_ms/1000/3600)%24);
        int slot=0;
        if(h>=7&&h<9)slot=1; else if(h>=9&&h<12)slot=2;
        else if(h>=12&&h<14)slot=3; else if(h>=14&&h<17)slot=4;
        else if(h>=17&&h<22)slot=5; else if(h>=22||h<5)slot=6;
        if(!eng.on_close) eng.on_close=[](const omega::TradeRecord&t){store::add(t);};
        eng.on_tick(r.bid,r.ask,r.ts_ms,slot);
    }
};
struct LonFadeRunner {
    omega::LondonCoreFadeEngine eng;
    void tick(const TickRow& r) {
        int h=(int)((r.ts_ms/1000/3600)%24);
        int slot=0;
        if(h>=7&&h<9)slot=1; else if(h>=9&&h<12)slot=2;
        else if(h>=12&&h<14)slot=3; else if(h>=14&&h<17)slot=4;
        else if(h>=17&&h<22)slot=5; else if(h>=22||h<5)slot=6;
        if(!eng.on_close) eng.on_close=[](const omega::TradeRecord&t){store::add(t);};
        eng.on_tick(r.bid,r.ask,r.ts_ms,slot);
    }
};
// =============================================================================
// RSIReversal runner
// =============================================================================
struct RSIRevRunner {
    // FLIPPED LOGIC: RSI extremes on tick data = CONTINUATION not reversal
    // RSI < 30 (oversold on ticks) = strong downtrend = SHORT
    // RSI > 70 (overbought on ticks) = strong uptrend = LONG
    // Achieved by swapping oversold/overbought thresholds:
    // Engine enters LONG on oversold and SHORT on overbought --
    // so setting OVERSOLD=70 and OVERBOUGHT=30 flips the direction.
    omega::RSIReversalEngine eng;
    RSIRevRunner() {
        eng.enabled        = true;
        eng.shadow_mode    = true;
        eng.RSI_OVERSOLD   = 70.0;  // FLIPPED: RSI>70 triggers LONG (continuation up)
        eng.RSI_OVERBOUGHT = 30.0;  // FLIPPED: RSI<30 triggers SHORT (continuation down)
        eng.RSI_EXIT_LONG  = 50.0;
        eng.RSI_EXIT_SHORT = 50.0;
        eng.SL_ATR_MULT    = 0.5;
        eng.TRAIL_ATR_MULT = 0.30;
        eng.BE_ATR_MULT    = 0.25;
        eng.COOLDOWN_S     = 15;
        eng.COOLDOWN_S_VACUUM = 10;
        eng.MAX_HOLD_S     = 90;
        eng.MIN_HOLD_S     = 3;
    }
    void tick(const TickRow& r) {
        eng.update_indicators(r.bid, r.ask);
        if (!eng.has_open_position()) {
            int h = (int)((r.ts_ms/1000/3600)%24);
            int slot = 0;
            if(h>=7&&h<9)slot=1; else if(h>=9&&h<12)slot=2;
            else if(h>=12&&h<14)slot=3; else if(h>=14&&h<17)slot=4;
            else if(h>=17&&h<22)slot=5; else if(h>=22||h<5)slot=6;
            eng.on_tick(r.bid, r.ask, slot, r.ts_ms,
                        0.5, false, false, false, false, false,
                        [](const omega::TradeRecord& t){ store::add(t); });
        } else {
            eng.on_tick(r.bid, r.ask, 0, r.ts_ms,
                        0.5, false, false, false, false, false,
                        [](const omega::TradeRecord& t){ store::add(t); });
        }
    }
};

// (MicroMomRunner REMOVED at Batch 5V §1.2 2026-04-20.
//  Engine retired -- real-tick backtest result was -$3.8k over 4320 trades / 2yr.
//  See wiki tombstone wiki/entities/MicroMomentumEngine.md.)

// =============================================================================
// S44 Engine runners -- 10 new runners using BtBarEngine bar library
//
// Each runner constructs a private engine instance (no globals; each --engine
// flag is independent). Bar context for HTF engines comes from local
// BtBarEngine<60> / BtBarEngine<240>. PDHL inputs come from BtPdhlTracker +
// BtEwmDrift + BtBarIndicators (M1 ATR). MacroCrash inputs come from
// BtVolRatio + BtEwmDrift + the M1 BtBarEngine.
//
// All engines run with shadow_mode=true by default (matches live engine_init);
// the runners flip shadow_mode=false explicitly so on_close/TradeRecord paths
// fire and we capture every closed trade in the store.
// =============================================================================

// -----------------------------------------------------------------------------
// HybridGoldRunner -- GoldHybridBracketEngine
//
// The engine takes (bid, ask, now_ms, can_enter, flow_live, flow_be_locked,
// flow_trail_stage, on_close, +DOM defaults). For backtest we have no flow
// engine (GoldFlow culled S19) so we feed flow_live=false and flow_be_locked
// =false. can_enter=true permits arming on every eligible tick. DOM args are
// defaulted (no L2 stream). The on_close callback receives a TradeRecord
// directly.
// -----------------------------------------------------------------------------
struct HybridGoldRunner {
    omega::GoldHybridBracketEngine eng;
    HybridGoldRunner(){
        eng.shadow_mode = false;  // log to store via on_close
    }
    void tick(const TickRow& r){
        eng.on_tick(r.bid, r.ask, (int64_t)r.ts_ms,
                    /*can_enter=*/   true,
                    /*flow_live=*/   false,
                    /*flow_be_locked=*/ false,
                    /*flow_trail_stage=*/ 0,
                    [](const omega::TradeRecord& t){ store::add(t); });
    }
};

// -----------------------------------------------------------------------------
// MacroCrashRunner -- MacroCrashEngine
//
// Inputs needed: atr (M1), vol_ratio, ewm_drift, expansion_regime, plus DOM
// defaults. We build:
//   - M1 BtBarEngine<1> -> ATR14 from M1 bars
//   - BtVolRatio        -> short(40)/long(400) tick-range ratio
//   - BtEwmDrift        -> 30s halflife mid-drift
// expansion_regime: synthesised from vol_ratio > 1.5 (mirrors GoldEngineStack's
// supervisor heuristic; without real macro regime detector this is the closest
// available signal). session_slot via bt_session_slot. RSI from M1 indicators.
//
// MacroCrash uses a TWO-callback shape:
//   on_close          (double exit_px, bool is_long, double size, std::string reason)
//   on_trade_record   (omega::TradeRecord)
// We wire on_trade_record to store::add. on_close left null (live-only side).
//
// NB: live config disables this engine at S17 (commit 9566bd6e). For backtest
// we explicitly set enabled=true so the strategy can be measured against
// April 2026 ticks; no production behaviour is changed.
// -----------------------------------------------------------------------------
struct MacroCrashRunner {
    omega::MacroCrashEngine eng;
    omega::bt::BtBarEngine<1>   m1;
    omega::bt::BtVolRatio       vol_ratio{40, 400};
    omega::bt::BtEwmDrift       drift{30.0};

    MacroCrashRunner(){
        eng.enabled     = true;   // backtest harness override (live-disabled at S17)
        eng.shadow_mode = false;  // log to store via on_trade_record
        eng.on_trade_record = [](const omega::TradeRecord& tr){ store::add(tr); };
    }
    void tick(const TickRow& r){
        const double mid = (r.bid + r.ask) * 0.5;
        m1.on_tick(mid, r.ts_ms);
        vol_ratio.update(mid);
        drift.update(mid, r.ts_ms);

        // Wait until M1 indicators ready (RSI_P+1 closed bars => 15 bars => 15 min)
        if (!m1.indicators_ready()) return;
        if (!vol_ratio.is_ready())  return;
        if (!drift.is_ready())      return;

        const double atr   = m1.atr14();
        const double vr    = vol_ratio.ratio();
        const double dr    = drift.drift(mid);
        const bool   exp_r = (vr > 1.5);
        const double rsi   = m1.rsi14();
        const int    slot  = omega::bt::bt_session_slot(r.ts_ms);

        eng.on_tick(r.bid, r.ask,
                    atr, vr, dr,
                    exp_r, (int64_t)r.ts_ms,
                    /*book_slope=*/      0.0,
                    /*vacuum_ask=*/      false,
                    /*vacuum_bid=*/      false,
                    /*microprice_bias=*/ 0.0,
                    /*rsi14=*/           rsi,
                    /*session_slot=*/    slot);
    }
};

// -----------------------------------------------------------------------------
// H1SwingRunner -- H1SwingEngine
//
// Engine fires on H1 bar closes only; tick path handles SL/TP/trail.
// We drive both via BtBarEngine<60> (H1) plus BtBarEngine<240> (H4 ctx).
// h1_trend_state and h4_trend_state are -1/0/+1 derived from EMA9-vs-EMA50
// (>0 => +1, <0 => -1).
// -----------------------------------------------------------------------------
struct H1SwingRunner {
    omega::H1SwingEngine eng;
    omega::bt::BtBarEngine<60>  h1;
    omega::bt::BtBarEngine<240> h4;

    H1SwingRunner(){
        eng.p           = omega::make_h1_gold_params();
        eng.shadow_mode = false;  // log to store via on_close
        eng.symbol      = "XAUUSD";
    }
    static int trend_from_ema(double ema9, double ema50) noexcept {
        if (ema9 > ema50 * 1.0000001) return +1;
        if (ema9 < ema50 * 0.9999999) return -1;
        return 0;
    }
    void tick(const TickRow& r){
        const double mid = (r.bid + r.ask) * 0.5;
        const bool h1_closed = h1.on_tick(mid, r.ts_ms);
        const bool h4_closed = h4.on_tick(mid, r.ts_ms);
        (void)h4_closed;

        // Tick-level management every tick once we have a position
        eng.on_tick(r.bid, r.ask, (int64_t)r.ts_ms,
                    [](const omega::TradeRecord& t){ store::add(t); });

        // Bar-level entry/manage call on H1 close
        if (h1_closed && h1.indicators_ready() && h4.indicators_ready()) {
            const int slot = omega::bt::bt_session_slot(r.ts_ms);
            const int h1_state = trend_from_ema(h1.ema9(), h1.ema50());
            const int h4_state = trend_from_ema(h4.ema9(), h4.ema50());
            eng.on_h1_bar(
                mid, r.bid, r.ask,
                h1.ema9(), h1.ema21(), h1.ema50(),
                h1.atr14(), h1.rsi14(),
                h1.adx14(), h1.adx_rising(),
                h1_state, h4_state,
                h4.adx14(), h4.atr14(),
                slot, (int64_t)r.ts_ms,
                [](const omega::TradeRecord& t){ store::add(t); });
        }
    }
};

// -----------------------------------------------------------------------------
// H4RegimeRunner -- H4RegimeEngine
//
// Engine fires on H4 bar closes; tick path handles SL/trail. Needs M15 ATR
// expansion flag, fed via BtBarEngine<15> + BtM15ExpansionTracker.
// -----------------------------------------------------------------------------
struct H4RegimeRunner {
    omega::H4RegimeEngine eng;
    omega::bt::BtBarEngine<240> h4;
    omega::bt::BtBarEngine<15>  m15;
    omega::bt::BtM15ExpansionTracker m15_exp;

    H4RegimeRunner(){
        eng.p           = omega::make_h4_gold_params();
        eng.shadow_mode = false;
        eng.symbol      = "XAUUSD";
    }
    void tick(const TickRow& r){
        const double mid = (r.bid + r.ask) * 0.5;
        const bool h4_closed  = h4.on_tick(mid, r.ts_ms);
        const bool m15_closed = m15.on_tick(mid, r.ts_ms);
        if (m15_closed && m15.indicators_ready()) {
            m15_exp.update(m15.atr14());
        }

        // Tick-level management
        eng.on_tick(r.bid, r.ask, (int64_t)r.ts_ms,
                    [](const omega::TradeRecord& t){ store::add(t); });

        if (h4_closed && h4.indicators_ready()) {
            const auto& bar = h4.last_closed_bar();
            eng.on_h4_bar(
                bar.high, bar.low, bar.close,
                mid, r.bid, r.ask,
                h4.ema9(), h4.ema50(),
                h4.atr14(), h4.rsi14(), h4.adx14(),
                m15_exp.expanding(),
                (int64_t)r.ts_ms,
                [](const omega::TradeRecord& t){ store::add(t); });
        }
    }
};

// -----------------------------------------------------------------------------
// MinimalH4Runner -- MinimalH4Breakout
//
// Pure H4 Donchian breakout. Drives off BtBarEngine<240> and only needs H4
// ATR.  on_h4_bar handles entry; on_tick handles SL/TP.
// -----------------------------------------------------------------------------
struct MinimalH4Runner {
    omega::MinimalH4Breakout eng;
    omega::bt::BtBarEngine<240> h4;

    MinimalH4Runner(){
        eng.p           = omega::make_minimal_h4_gold_params();
        eng.shadow_mode = false;
        eng.symbol      = "XAUUSD";
    }
    void tick(const TickRow& r){
        const double mid = (r.bid + r.ask) * 0.5;
        const bool h4_closed = h4.on_tick(mid, r.ts_ms);

        // Tick-level SL/TP management
        eng.on_tick(r.bid, r.ask, (int64_t)r.ts_ms,
                    [](const omega::TradeRecord& t){ store::add(t); });

        if (h4_closed && h4.indicators_ready()) {
            const auto& bar = h4.last_closed_bar();
            eng.on_h4_bar(
                bar.high, bar.low, bar.close,
                r.bid, r.ask,
                h4.atr14(),
                (int64_t)r.ts_ms,
                [](const omega::TradeRecord& t){ store::add(t); });
        }
    }
};

// -----------------------------------------------------------------------------
// TSMomRunner -- TSMomGoldEngine (S46 T3)
//
// Daily-bar Time-Series Momentum, long-only XAUUSD. Drives off
// BtBarEngine<1440>. on_daily_bar handles entry on bar close; on_tick
// handles SL / ATR-trail / weekend exits.
//
// Runner is shadow_mode=false in backtest (so TradeRecord is emitted with
// shadow=false and counts in the per-engine stats).  When lifted to live
// the production wiring should set shadow_mode=true for the validation
// window per the S45 carryover protocol for new engines.
// -----------------------------------------------------------------------------
struct TSMomRunner {
    omega::TSMomGoldEngine eng;
    omega::bt::BtBarEngine<1440> d1;

    TSMomRunner(){
        eng.p           = omega::make_tsmom_gold_params();
        eng.shadow_mode = false;
        eng.symbol      = "XAUUSD";
    }
    void tick(const TickRow& r){
        const double mid = (r.bid + r.ask) * 0.5;
        const bool d1_closed = d1.on_tick(mid, r.ts_ms);

        // Tick-level SL / trail / weekend
        eng.on_tick(r.bid, r.ask, (int64_t)r.ts_ms,
                    [](const omega::TradeRecord& t){ store::add(t); });

        // Daily-bar entry path. We need ATR14 from the bar engine, which
        // populates after RSI_P+1 bars => 15 daily bars (~3 weeks).
        if (d1_closed && d1.indicators_ready()) {
            const auto& bar = d1.last_closed_bar();
            eng.on_daily_bar(
                bar.close,
                d1.atr14(),
                r.bid, r.ask,
                (int64_t)r.ts_ms,
                [](const omega::TradeRecord& t){ store::add(t); });
        }
    }
};

// -----------------------------------------------------------------------------
// PullbackContRunner -- PullbackContEngine (live default config)
// -----------------------------------------------------------------------------
struct PullbackContRunner {
    omega::PullbackContEngine eng;
    PullbackContRunner(){
        // Mirror engine_init.hpp PullbackCont config (S44, validated)
        eng.enabled       = true;
        eng.shadow_mode   = false;  // log to store via on_trade_record
        eng.MOVE_MIN      = 20.0;
        eng.PB_FRAC       = 0.20;
        eng.LOOKBACK_S    = 300;
        eng.HOLD_S        = 600;
        eng.SL_PTS        = 6.0;
        eng.TRAIL_PTS     = 4.0;
        eng.BASE_RISK_USD = 80.0;
        eng.COOLDOWN_MS   = 120000;
        eng.on_trade_record = [](const omega::TradeRecord& base){
            // Engine emits engine="PullbackCont"; preserve as-is.
            store::add(base);
        };
    }
    void tick(const TickRow& r){
        eng.on_tick(r.bid, r.ask, (int64_t)r.ts_ms,
                    /*gold_can_enter=*/ true);
    }
};

// -----------------------------------------------------------------------------
// PullbackPremRunner -- PullbackContEngine (PREMIUM config: 30pt h07 only)
//
// Same engine class as PullbackContRunner with the premium parameters from
// engine_init.hpp lines 187-209.  We override the engine's emitted name to
// "PullbackPrem" so the per-engine stats table separates them.
// -----------------------------------------------------------------------------
struct PullbackPremRunner {
    omega::PullbackContEngine eng;
    PullbackPremRunner(){
        eng.enabled       = true;
        eng.shadow_mode   = false;
        eng.MOVE_MIN      = 30.0;   // higher bar
        eng.PB_FRAC       = 0.20;
        eng.LOOKBACK_S    = 600;
        eng.HOLD_S        = 900;
        eng.SL_PTS        = 8.0;
        eng.TRAIL_PTS     = 2.0;
        eng.BASE_RISK_USD = 160.0;
        eng.COOLDOWN_MS   = 300000;
        eng.on_trade_record = [](const omega::TradeRecord& base){
            // Rewrite engine label so summary distinguishes prem from cont.
            omega::TradeRecord tr = base;
            tr.engine = "PullbackPrem";
            store::add(tr);
        };
    }
    void tick(const TickRow& r){
        eng.on_tick(r.bid, r.ask, (int64_t)r.ts_ms,
                    /*gold_can_enter=*/ true);
    }
};

// -----------------------------------------------------------------------------
// PDHLRevRunner -- PDHLReversionEngine
//
// Inputs: pdh, pdl, atr, l2_imbalance, depth_bid, depth_ask, l2_real,
// ewm_drift, session_slot. Without L2 stream we use neutral imbalance=0.5,
// l2_real=false, depths=0; the engine then falls back to drift-fade entry
// path (DRIFT_FADE_MIN gate).
// -----------------------------------------------------------------------------
struct PDHLRevRunner {
    omega::PDHLReversionEngine eng;
    omega::bt::BtBarEngine<1>  m1;       // M1 ATR
    omega::bt::BtPdhlTracker   pdhl;
    omega::bt::BtEwmDrift      drift{30.0};

    PDHLRevRunner(){
        eng.enabled     = true;
        eng.shadow_mode = false;
    }
    void tick(const TickRow& r){
        const double mid = (r.bid + r.ask) * 0.5;
        m1.on_tick(mid, r.ts_ms);
        pdhl.update(mid, r.ts_ms);
        drift.update(mid, r.ts_ms);

        if (!m1.indicators_ready()) return;
        if (!pdhl.is_ready())       return;
        if (!drift.is_ready())      return;

        const int    slot  = omega::bt::bt_session_slot(r.ts_ms);
        const double atr   = m1.atr14();
        const double dr    = drift.drift(mid);

        eng.on_tick(r.bid, r.ask,
                    (int64_t)r.ts_ms,
                    pdhl.pdh(), pdhl.pdl(),
                    atr,
                    /*l2_imbalance=*/ 0.5,
                    /*depth_bid=*/    0,
                    /*depth_ask=*/    0,
                    /*l2_real=*/      false,
                    /*ewm_drift=*/    dr,
                    /*session_slot=*/ slot,
                    [](const omega::TradeRecord& t){ store::add(t); });
    }
};

// -----------------------------------------------------------------------------
// RSIExtremeRunner -- RSIExtremeTurnEngine
//
// Engine wants: per-tick update_indicators(bid, ask), per-bar set_bar_rsi
// (M1 RSI from BtBarEngine<1>), and on_tick(bid, ask, slot, now_ms, on_close).
// Bar RSI is fed every tick (engine itself debounces same-value updates).
// -----------------------------------------------------------------------------
struct RSIExtremeRunner {
    omega::RSIExtremeTurnEngine eng;
    omega::bt::BtBarEngine<1>   m1;

    RSIExtremeRunner(){
        eng.enabled     = true;
        eng.shadow_mode = false;
    }
    void tick(const TickRow& r){
        const double mid = (r.bid + r.ask) * 0.5;
        m1.on_tick(mid, r.ts_ms);

        // Keep tick-level indicators warm regardless of position state.
        eng.update_indicators(r.bid, r.ask);
        if (m1.indicators_ready()) {
            eng.set_bar_rsi(m1.rsi14());
        }

        const int slot = omega::bt::bt_session_slot(r.ts_ms);
        eng.on_tick(r.bid, r.ask, slot, (int64_t)r.ts_ms,
                    [](const omega::TradeRecord& t){ store::add(t); });
    }
};

// -----------------------------------------------------------------------------
// EMACrossRunner -- EMACrossEngine
//
// NOTE: live EMACrossEngine.hpp defines `static constexpr bool ECE_CULLED =
// true;` so all new entries are blocked at runtime. This runner still wires
// the engine end-to-end so coverage is complete and a future un-cull can be
// validated without touching this file. on_bar fed every M1 close, on_tick
// every tick.
// -----------------------------------------------------------------------------
struct EMACrossRunner {
    omega::EMACrossEngine eng;
    omega::bt::BtBarEngine<1> m1;

    EMACrossRunner(){
        eng.shadow_mode = false;
    }
    void tick(const TickRow& r){
        const double mid = (r.bid + r.ask) * 0.5;
        const bool m1_closed = m1.on_tick(mid, r.ts_ms);

        if (m1_closed && m1.indicators_ready()) {
            eng.on_bar(m1.last_closed_bar().close,
                       m1.atr14(), m1.rsi14(),
                       (int64_t)r.ts_ms);
        }

        eng.on_tick(r.bid, r.ask, (int64_t)r.ts_ms,
                    [](const omega::TradeRecord& t){ store::add(t); });
    }
};

// =============================================================================
// Config
// =============================================================================
struct Cfg {
    const char* csv   = nullptr;
    double      lat   = 1.0;
    const char* rep   = "bt_report.csv";
    const char* trd   = "bt_trades.csv";
    int64_t     warm  = 5000;
    // Legacy default cohort -- behaviour for `OmegaBacktest <csv>` (no flags)
    // enables gold,latency,cross. The breakout cohort was retired at S48
    // (XAUUSD_BE was an index-engine misuse losing -$8/trade; GoldBracketEngine
    // never fired on 26m gold tick data).
    bool gold=true, latency=true, cross=true, stoprun=false, ofade=false;
    bool omom=false, amom=false, lfade=false, allnew=false;
    bool rsirev=false; // (micromom removed at 5V §1.2)
    bool quiet=false;

    // S44 new runners (default OFF -- enabled by --engine list)
    bool hybridgold=false, macrocrash=false, h1swing=false, h4regime=false;
    bool minh4=false, pullbackcont=false, pullbackprem=false;
    bool pdhl=false, rsiextreme=false, emacross=false;

    // S46 new runners
    bool tsmom=false;
};
static Cfg parse(int argc, char** argv){
    Cfg c;
    if(argc<2){
        fprintf(stderr,
            "Usage: OmegaBacktest <ticks.csv> [options]\n"
            "  --latency <ms>  exec latency         (default 1.0)\n"
            "  --report  <f>   engine summary CSV    (default bt_report.csv)\n"
            "  --trades  <f>   trade records CSV     (default bt_trades.csv)\n"
            "  --warmup  <n>   warmup ticks          (default 5000)\n"
            "  --engine  <l>   comma list, default = gold,latency,cross\n"
            "                  legacy:  gold latency cross stoprun\n"
            "                           ofade omom amom lfade rsirev allnew\n"
            "                  S44 new: hybridgold macrocrash h1swing h4regime\n"
            "                           minh4 pullbackcont pullbackprem\n"
            "                           pdhl rsiextreme emacross\n"
            "                  S46 new: tsmom\n"
            "                  master:  all    (everything)\n"
            "                           clean  (everything except the 4 validated\n"
            "                                   bleeders: ofade,lfade,amom,pdhl)\n"
            "  --quiet         suppress engine log output (recommended)\n");
        exit(1);
    }
    c.csv=argv[1];
    for(int i=2;i<argc;++i){
        if(!strcmp(argv[i],"--latency")&&i+1<argc) c.lat   =atof(argv[++i]);
        else if(!strcmp(argv[i],"--report")&&i+1<argc) c.rep=argv[++i];
        else if(!strcmp(argv[i],"--trades")&&i+1<argc) c.trd=argv[++i];
        else if(!strcmp(argv[i],"--warmup")&&i+1<argc) c.warm=atoll(argv[++i]);
        else if(!strcmp(argv[i],"--quiet")) c.quiet=true;
        else if(!strcmp(argv[i],"--engine")&&i+1<argc){
            const char* e=argv[++i];
            // Master 'all' flag: union legacy default 4 + every legacy optional
            // + every S44 new runner. Use single keyword so users don't have to
            // enumerate the full list.
            const bool all_master = (!!strstr(e,"all") && !strstr(e,"allnew"));

            // S45 master 'clean' flag: enables every runner EXCEPT the four
            // validated bleeders identified in the 26m audit (LondonCoreFade,
            // OverlapFade, AsiaMomentum, PDHLReversion). These four engines
            // collectively bled $880 over 26m and have been diagnosed as
            // either anti-trend-in-trend (fade family), session-mismatch
            // (AsiaMom), or structurally-inverted-signal (PDHL). They remain
            // available via explicit --engine flags for further analysis but
            // are excluded from any 'clean' production-leaning run.
            const bool clean_master = !!strstr(e,"clean");

            // Legacy parse -- match S43 semantics exactly.
            c.gold     = !!strstr(e,"gold");
            c.latency  = !!strstr(e,"latency");
            c.cross    = !!strstr(e,"cross");
            c.stoprun  = !!strstr(e,"stoprun");
            c.ofade    = !!strstr(e,"ofade");
            c.omom     = !!strstr(e,"omom");
            c.amom     = !!strstr(e,"amom");
            c.lfade    = !!strstr(e,"lfade");
            c.rsirev   = !!strstr(e,"rsirev");
            c.allnew   = (!!strstr(e,"allnew") || all_master);

            // S44 new flags
            c.hybridgold   = !!strstr(e,"hybridgold");
            c.macrocrash   = !!strstr(e,"macrocrash");
            c.h1swing      = !!strstr(e,"h1swing");
            c.h4regime     = !!strstr(e,"h4regime");
            c.minh4        = !!strstr(e,"minh4");
            // pullbackcont must NOT match pullbackprem: substring rules give
            // "pullbackcont" matches inside "pullbackprem"=false (no overlap),
            // and vice versa is also disjoint -- substrings are unique.
            c.pullbackcont = !!strstr(e,"pullbackcont");
            c.pullbackprem = !!strstr(e,"pullbackprem");
            c.pdhl         = !!strstr(e,"pdhl");
            c.rsiextreme   = !!strstr(e,"rsiextreme");
            c.emacross     = !!strstr(e,"emacross");

            // S46 new flag
            c.tsmom        = !!strstr(e,"tsmom");

            if (all_master) {
                // Master flag enables EVERY runner. Legacy defaults already
                // matched above ('all' contains 'gold' etc.) but be explicit
                // about the optional cohort so the master flag is bullet-proof
                // even if someone renames substrings later.
                c.gold = c.latency = c.cross = true;
                c.stoprun = c.ofade = c.omom = c.amom = c.lfade = true;
                c.rsirev = true;
                c.hybridgold = c.macrocrash = c.h1swing = c.h4regime = true;
                c.minh4 = c.pullbackcont = c.pullbackprem = true;
                c.pdhl = c.rsiextreme = c.emacross = true;
                c.tsmom = true;
            }

            if (clean_master) {
                // S45: enable every runner EXCEPT the four validated bleeders.
                // Excluded: ofade (OverlapFade), lfade (LondonCoreFade),
                // amom (AsiaMomentum), pdhl (PDHLReversion).
                // Rationale captured in commit message.
                c.gold = c.latency = c.cross = true;
                c.stoprun = true;
                c.omom = true;       // OverlapMomentum kept (different class)
                c.rsirev = true;
                c.hybridgold = c.macrocrash = c.h1swing = c.h4regime = true;
                c.minh4 = c.pullbackcont = c.pullbackprem = true;
                c.rsiextreme = c.emacross = true;
                c.tsmom = true;      // S46: TSMomGold included in clean cohort
                // Explicitly DISABLE the four bleeders (in case substring
                // matched anything above).
                c.ofade = false;
                c.lfade = false;
                c.amom  = false;
                c.pdhl  = false;
            }
        }
    }
    return c;
}

// =============================================================================
// Output writers
// =============================================================================
static void write_trades(const char* path){
    FILE* f=fopen(path,"w"); if(!f){fprintf(stderr,"[WARN] cannot write %s\n",path);return;}
    fprintf(f,"entryTs,symbol,side,engine,entryPrice,exitPrice,"
              "pnl,mfe,mae,hold_sec,exitReason,spreadAtEntry,atrAtEntry,latencyMs,regime\n");
    for(const auto& t:store::recs)
        fprintf(f,"%lld,%s,%s,%s,%.5f,%.5f,%.4f,%.4f,%.4f,%lld,%s,%.4f,%.4f,%.2f,%s\n",
                (long long)t.entryTs, t.symbol.c_str(), t.side.c_str(), t.engine.c_str(),
                t.entryPrice, t.exitPrice, t.pnl, t.mfe, t.mae,
                (long long)(t.exitTs-t.entryTs), t.exitReason.c_str(),
                t.spreadAtEntry, t.atr_at_entry, t.latencyMs, t.regime.c_str());
    fclose(f);
    printf("[OUTPUT] %zu trade records -> %s\n", store::recs.size(), path);
}
static void write_report(const char* path){
    FILE* f=fopen(path,"w"); if(!f)return;
    fprintf(f,"engine,trades,win_rate_pct,gross_pnl,avg_pnl,max_dd,avg_hold_sec,sharpe\n");
    std::vector<const Stat*> v;
    for(auto& kv:store::stats) v.push_back(&kv.second);
    std::sort(v.begin(),v.end(),[](const Stat*a,const Stat*b){return a->pnl>b->pnl;});
    for(const auto* s:v)
        fprintf(f,"%s,%lld,%.2f,%.4f,%.4f,%.4f,%.1f,%.3f\n",
                s->name.c_str(),(long long)s->n,s->wr(),s->pnl,s->avg(),s->dd,s->avgh(),s->sharpe());
    fclose(f);
    printf("[OUTPUT] Engine report  -> %s\n", path);
}

// =============================================================================
// main
// =============================================================================
int main(int argc, char** argv){
    Cfg cfg = parse(argc,argv);

    // Save original stdout fd BEFORE redirecting, so we can restore it for summary.
    int stdout_saved = -1;
#ifndef _WIN32
    FILE* devnull = nullptr;
    if (cfg.quiet) {
        stdout_saved = dup(STDOUT_FILENO);
        devnull = fopen("/dev/null", "w");
        if (devnull) { fflush(stdout); dup2(fileno(devnull), STDOUT_FILENO); }
    }
#else
    if (cfg.quiet) {
        stdout_saved = _dup(_fileno(stdout));
        FILE* devnull = fopen("NUL", "w");
        if (devnull) { fflush(stdout); _dup2(_fileno(devnull), _fileno(stdout)); }
    }
#endif

    // ?? Load CSV ?????????????????????????????????????????????????????????????
    MemMappedFile mf;
    if(!mf.open(cfg.csv)){fprintf(stderr,"[ERROR] Cannot open: %s\n",cfg.csv);return 1;}

    const auto t0p = std::chrono::steady_clock_real::now();
    std::vector<TickRow> ticks = parse_csv(mf);
    const double ps = std::chrono::duration<double>(
        std::chrono::steady_clock_real::now()-t0p).count();

    if(ticks.empty()){fprintf(stderr,"[ERROR] No valid ticks parsed.\n");return 1;}

    // Date range
    time_t ta=ticks.front().ts_ms/1000, tb=ticks.back().ts_ms/1000;
    struct tm ma{},mb{};
#ifdef _WIN32
    gmtime_s(&ma,&ta); gmtime_s(&mb,&tb);
#else
    gmtime_r(&ta,&ma); gmtime_r(&tb,&mb);
#endif
    char sa[20],sb[20];
    strftime(sa,20,"%Y-%m-%d",&ma); strftime(sb,20,"%Y-%m-%d",&mb);

    // Warmup cutoff
    if(cfg.warm>0 && cfg.warm<(int64_t)ticks.size())
        store::warmup_sec = ticks[(size_t)cfg.warm].ts_ms/1000;

    // ?? Construct engines ?????????????????????????????????????????????????????
    // Legacy runners
    std::unique_ptr<GoldRunner>          rg;
    std::unique_ptr<LatencyRunner>       rl;
    std::unique_ptr<CrossRunner>         rc;
    std::unique_ptr<StopRunRunner>       rs;
    std::unique_ptr<OverlapFadeRunner>   ro;
    std::unique_ptr<OverlapMomRunner>    rom;
    std::unique_ptr<AsiaMomRunner>       ram;
    std::unique_ptr<LonFadeRunner>       rlf;
    std::unique_ptr<RSIRevRunner>        rrsi;
    // S44 new runners
    std::unique_ptr<HybridGoldRunner>    rhg;
    std::unique_ptr<MacroCrashRunner>    rmc;
    std::unique_ptr<H1SwingRunner>       rh1;
    std::unique_ptr<H4RegimeRunner>      rh4;
    std::unique_ptr<MinimalH4Runner>     rmh4;
    std::unique_ptr<PullbackContRunner>  rpc;
    std::unique_ptr<PullbackPremRunner>  rpp;
    std::unique_ptr<PDHLRevRunner>       rpd;
    std::unique_ptr<RSIExtremeRunner>    rrx;
    std::unique_ptr<EMACrossRunner>      rec;
    // S46 new runners
    std::unique_ptr<TSMomRunner>         rtsm;

    if(cfg.gold)    rg = std::make_unique<GoldRunner>(cfg.lat);
    if(cfg.latency) rl = std::make_unique<LatencyRunner>(cfg.lat);
    if(cfg.cross)   rc = std::make_unique<CrossRunner>();
    if(cfg.stoprun) rs = std::make_unique<StopRunRunner>();
    if(cfg.ofade||cfg.allnew)   ro  = std::make_unique<OverlapFadeRunner>();
    if(cfg.omom||cfg.allnew)    rom = std::make_unique<OverlapMomRunner>();
    if(cfg.amom||cfg.allnew)    ram = std::make_unique<AsiaMomRunner>();
    if(cfg.lfade||cfg.allnew)   rlf = std::make_unique<LonFadeRunner>();
    if(cfg.rsirev||cfg.allnew)  rrsi = std::make_unique<RSIRevRunner>();

    // S44 new runner construction (independent of allnew -- they have their own flags)
    if(cfg.hybridgold)   rhg  = std::make_unique<HybridGoldRunner>();
    if(cfg.macrocrash)   rmc  = std::make_unique<MacroCrashRunner>();
    if(cfg.h1swing)      rh1  = std::make_unique<H1SwingRunner>();
    if(cfg.h4regime)     rh4  = std::make_unique<H4RegimeRunner>();
    if(cfg.minh4)        rmh4 = std::make_unique<MinimalH4Runner>();
    if(cfg.pullbackcont) rpc  = std::make_unique<PullbackContRunner>();
    if(cfg.pullbackprem) rpp  = std::make_unique<PullbackPremRunner>();
    if(cfg.pdhl)         rpd  = std::make_unique<PDHLRevRunner>();
    if(cfg.rsiextreme)   rrx  = std::make_unique<RSIExtremeRunner>();
    if(cfg.emacross)     rec  = std::make_unique<EMACrossRunner>();

    // S46 new runner construction
    if(cfg.tsmom)        rtsm = std::make_unique<TSMomRunner>();

    // ?? Tick loop ?????????????????????????????????????????????????????????????
    const auto t0r  = std::chrono::steady_clock_real::now();
    const int64_t N = (int64_t)ticks.size();
    int64_t last_p  = 0;

    for(int64_t i=0; i<N; ++i){
        const TickRow& r = ticks[(size_t)i];
        omega::bt::set_sim_time(r.ts_ms);

        if(rg) rg->tick(r);
        if(rl) rl->tick(r);
        if(rc) rc->tick(r);
        if(rs) rs->tick(r);
        if(ro)  ro->tick(r);
        if(rom) rom->tick(r);
        if(ram) ram->tick(r);
        if(rlf)  rlf->tick(r);
        if(rrsi) rrsi->tick(r);

        // S44 new runners
        if(rhg)  rhg->tick(r);
        if(rmc)  rmc->tick(r);
        if(rh1)  rh1->tick(r);
        if(rh4)  rh4->tick(r);
        if(rmh4) rmh4->tick(r);
        if(rpc)  rpc->tick(r);
        if(rpp)  rpp->tick(r);
        if(rpd)  rpd->tick(r);
        if(rrx)  rrx->tick(r);
        if(rec)  rec->tick(r);

        // S46 new runners
        if(rtsm) rtsm->tick(r);

        if(i-last_p >= 500'000){
            last_p=i;
            const double el = std::chrono::duration<double>(
                std::chrono::steady_clock_real::now()-t0r).count();
            const double tps = el>0?i/el:0;
            // Progress goes to stderr so it always shows regardless of quiet
            fprintf(stderr, "\r  [%5.1f%%]  %lld ticks | %5.0fs | %5.0f K t/s | %lld trades | ETA %4.0fs   ",
                   100.0*i/N,(long long)i,el,tps/1000.0,(long long)store::recs.size(),
                   tps>0?(N-i)/tps:0);
        }
    }

    const double run_sec = std::chrono::duration<double>(
        std::chrono::steady_clock_real::now()-t0r).count();

    // ?? Restore stdout before printing summary ???????????????????????????????????????
#ifndef _WIN32
    if (cfg.quiet && stdout_saved >= 0) {
        fflush(stdout);
        dup2(stdout_saved, STDOUT_FILENO);
        close(stdout_saved);
        if (devnull) fclose(devnull);
    }
#else
    if (cfg.quiet && stdout_saved >= 0) {
        fflush(stdout);
        _dup2(stdout_saved, _fileno(stdout));
        _close(stdout_saved);
    }
#endif

    // ?? Summary table ?????????????????????????????????????????????????????????
    fprintf(stderr, "\n");
    printf("================================================================\n");
    printf("  Omega C++ Backtester\n");
    printf("  File    : %s\n", cfg.csv);
    printf("  Ticks   : %lld  in %.1fs (%.0f K t/s)\n", (long long)N, ps, N/ps/1000.0);
    printf("  Range   : %s -> %s\n", sa, sb);
    printf("  Engines : %s%s%s%s%s%s%s%s%s%s%s%s%s%s\n",
           cfg.gold?"GoldStack ":"",
           cfg.latency?"LatencyEdge ":"", cfg.cross?"CrossAsset ":"",
           cfg.hybridgold?"HybridGold ":"",
           cfg.macrocrash?"MacroCrash ":"",
           cfg.h1swing?"H1Swing ":"",
           cfg.h4regime?"H4Regime ":"",
           cfg.minh4?"MinH4 ":"",
           cfg.pullbackcont?"PullbackCont ":"",
           cfg.pullbackprem?"PullbackPrem ":"",
           cfg.pdhl?"PDHL ":"",
           cfg.rsiextreme?"RSIExtreme ":"",
           cfg.emacross?"EMACross ":"",
           cfg.tsmom?"TSMomGold ":"");
    printf("  Run     : %.1fs = %.0f K t/s\n", run_sec, N/run_sec/1000.0);
    printf("================================================================\n");
    printf("  PER-ENGINE RESULTS (warmup %lld ticks excluded)\n",(long long)cfg.warm);
    printf("================================================================\n");

    std::vector<const Stat*> sv;
    for(auto& kv:store::stats) sv.push_back(&kv.second);
    std::sort(sv.begin(),sv.end(),[](const Stat*a,const Stat*b){return a->pnl>b->pnl;});

    double tp=0; int64_t tt=0;
    for(const auto* s:sv){s->print(); tp+=s->pnl; tt+=s->n;}
    printf("  --------------------------------------------------------------\n");
    printf("  TOTAL  %lld trades  gross_pnl=%+.2f  (0.01 lot, pre-cost)\n\n",
           (long long)tt, tp);

    write_trades(cfg.trd);
    write_report(cfg.rep);

    printf("\n[DONE]  python scripts/shadow_analysis.py %s\n", cfg.trd);
    return 0;
}



// =============================================================================
// OmegaBacktest.cpp — Native C++ backtester for all Omega engine families
//
// Replaces Python backtest.py. Targets 500K–2M ticks/second vs ~21K in Python.
//
// CRITICAL: OmegaTimeShim.hpp MUST be the first include. It redirects
// std::chrono::steady_clock, system_clock, and time() so all engine cooldowns,
// session filters, and hold-time gates advance with CSV timestamps rather than
// wall clock. Without the shim, a fast backtest produces completely wrong
// trade counts — cooldown gates either never expire (wall clock too fast) or
// constantly expire, depending on the gate type.
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
//   --engine  <list>   comma list: gold,flow,latency,cross,breakout (default: all)
//
// TICK CSV — auto-detected formats:
//   A:  timestamp_ms,bid,ask
//   B:  timestamp_ms,bid,ask,vol
//   C:  YYYY.MM.DD,HH:MM:SS.mmm,bid,ask,vol   (Dukascopy)
//   D:  timestamp_ms,open,high,low,close,vol   (OHLCV — uses close±0.15 spread)
//
// OUTPUT:
//   Console       live progress + per-engine summary table
//   bt_trades.csv all trades (compatible with scripts/shadow_analysis.py)
//   bt_report.csv per-engine aggregate stats
// =============================================================================

// ── Time shim — MUST be first, before ANY chrono or engine header ────────────
#include "OmegaTimeShim.hpp"

// ── Standard library (after shim — chrono types are now simulated) ───────────
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

// ── Platform: memory-mapped I/O ──────────────────────────────────────────────
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

// ── Omega engine headers (all clock calls redirect to simulated time) ────────
#include "../include/OmegaTradeLedger.hpp"
#include "../include/GoldEngineStack.hpp"
#include "../include/GoldFlowEngine.hpp"
#include "../include/LatencyEdgeEngines.hpp"
#include "../include/CrossAssetEngines.hpp"
#include "../include/BreakoutEngine.hpp"
#include "../include/BracketEngine.hpp"

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

// YYYY.MM.DD + HH:MM:SS.mmm → epoch ms (Dukascopy)
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
    // BOM
    if (f.size>=3&&(uint8_t)p[0]==0xEF&&(uint8_t)p[1]==0xBB&&(uint8_t)p[2]==0xBF) p+=3;
    // Header row
    if (*p && (*p<'0'||*p>'9')) { while(p<end&&*p!='\n')++p; if(p<end)++p; }
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
            fast_f(p,&nx);p=nx;if(*p==',')++p;  // open
            fast_f(p,&nx);p=nx;if(*p==',')++p;  // high
            fast_f(p,&nx);p=nx;if(*p==',')++p;  // low
            double cl=fast_f(p,&nx);p=nx;
            r.bid=cl-0.15; r.ask=cl+0.15;
        } else {
            r.ts_ms=fast_i64(p,&nx);p=nx;if(*p==',')++p;
            r.bid  =fast_f(p,&nx); p=nx;if(*p==',')++p;
            r.ask  =fast_f(p,&nx); p=nx;
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
// VWAP tracker for CrossAsset engines (resets daily in simulated time)
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
// Engine runners
// =============================================================================

struct GoldRunner {
    omega::gold::GoldEngineStack eng;
    double lat;
    GoldRunner(double l):lat(l){}
    void tick(const TickRow& r){ auto c=cb(); (void)eng.on_tick(r.bid,r.ask,lat,c); }
};

struct FlowRunner {
    GoldFlowEngine eng;
    FlowRunner(){}
    void tick(const TickRow& r){
        auto c=cb();
        // l2_imb=0.5 (neutral), ewm_drift=0.0 — not available in tick CSV
        (void)eng.on_tick(r.bid,r.ask,0.5,0.0,r.ts_ms,c);
    }
};

struct LatencyRunner {
    omega::latency::LatencyEdgeStack eng;
    double lat;
    LatencyRunner(double l):lat(l){}
    void tick(const TickRow& r){ auto c=cb(); (void)eng.on_tick_gold(r.bid,r.ask,lat,c); }
};

struct CrossRunner {
    omega::cross::OpeningRangeEngine    orb;
    omega::cross::VWAPReversionEngine   vrev;
    omega::cross::TrendPullbackEngine   tpb;
    omega::cross::NoiseBandMomentumEngine nbe;
    BtVwap vwap;
    CrossRunner(){}
    void tick(const TickRow& r){
        const std::string sym = "XAUUSD";
        const double mid  = (r.bid+r.ask)*0.5;
        const double vw   = vwap.update(mid, r.ts_ms);
        auto c=cb();
        (void)orb.on_tick(sym,r.bid,r.ask,c);
        (void)vrev.on_tick(sym,r.bid,r.ask,vw,c);
        (void)tpb.on_tick(sym,r.bid,r.ask,c);
        (void)nbe.on_tick(sym,r.bid,r.ask,c);
    }
};

struct BreakRunner {
    omega::BreakoutEngine    bke{"XAUUSD"};
    omega::GoldBracketEngine gbe;
    double lat;
    BreakRunner(double l):lat(l){}
    void tick(const TickRow& r){
        auto c=cb();
        bke.update(r.bid,r.ask,lat,"UNKNOWN",c);
        (void)gbe.on_tick(r.bid,r.ask,(long long)r.ts_ms,true,"UNKNOWN",c);
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
    bool gold=true, flow=true, latency=true, cross=true, breakout=true;
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
            "  --engine  <l>   gold,flow,latency,cross,breakout\n");
        exit(1);
    }
    c.csv=argv[1];
    for(int i=2;i<argc;++i){
        if(!strcmp(argv[i],"--latency")&&i+1<argc) c.lat   =atof(argv[++i]);
        else if(!strcmp(argv[i],"--report")&&i+1<argc) c.rep=argv[++i];
        else if(!strcmp(argv[i],"--trades")&&i+1<argc) c.trd=argv[++i];
        else if(!strcmp(argv[i],"--warmup")&&i+1<argc) c.warm=atoll(argv[++i]);
        else if(!strcmp(argv[i],"--engine")&&i+1<argc){
            const char* e=argv[++i];
            c.gold=!!strstr(e,"gold"); c.flow=!!strstr(e,"flow");
            c.latency=!!strstr(e,"latency"); c.cross=!!strstr(e,"cross");
            c.breakout=!!strstr(e,"breakout");
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
              "pnl,mfe,mae,hold_sec,exitReason,spreadAtEntry,latencyMs,regime\n");
    for(const auto& t:store::recs)
        fprintf(f,"%lld,%s,%s,%s,%.5f,%.5f,%.4f,%.4f,%.4f,%lld,%s,%.4f,%.2f,%s\n",
                (long long)t.entryTs, t.symbol.c_str(), t.side.c_str(), t.engine.c_str(),
                t.entryPrice, t.exitPrice, t.pnl, t.mfe, t.mae,
                (long long)(t.exitTs-t.entryTs), t.exitReason.c_str(),
                t.spreadAtEntry, t.latencyMs, t.regime.c_str());
    fclose(f);
    printf("[OUTPUT] %zu trade records → %s\n", store::recs.size(), path);
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
    printf("[OUTPUT] Engine report  → %s\n", path);
}

// =============================================================================
// main
// =============================================================================
int main(int argc, char** argv){
    Cfg cfg = parse(argc,argv);

    printf("================================================================\n");
    printf("  Omega C++ Backtester\n");
    printf("  File    : %s\n", cfg.csv);
    printf("  Latency : %.1f ms    Warmup: %lld ticks\n", cfg.lat, (long long)cfg.warm);
    printf("  Engines : %s%s%s%s%s\n",
           cfg.gold?"GoldStack ":"", cfg.flow?"GoldFlow ":"",
           cfg.latency?"LatencyEdge ":"", cfg.cross?"CrossAsset ":"",
           cfg.breakout?"Breakout/Bracket":"");
    printf("================================================================\n");

    // ── Load CSV ─────────────────────────────────────────────────────────────
    MemMappedFile mf;
    if(!mf.open(cfg.csv)){fprintf(stderr,"[ERROR] Cannot open: %s\n",cfg.csv);return 1;}
    printf("[LOAD] %.1f MB...\n", mf.size/1e6);

    // Use real wall clock for wall-time measurements (saved alias in shim)
    const auto t0p = std::chrono::steady_clock_real::now();
    std::vector<TickRow> ticks = parse_csv(mf);
    const double ps = std::chrono::duration<double>(
        std::chrono::steady_clock_real::now()-t0p).count();

    if(ticks.empty()){fprintf(stderr,"[ERROR] No valid ticks parsed.\n");return 1;}
    printf("[LOAD] %zu ticks in %.1fs (%.0f K t/s)\n", ticks.size(),ps,ticks.size()/ps/1000.0);

    // Date range display
    {
        const time_t a=ticks.front().ts_ms/1000, b=ticks.back().ts_ms/1000;
        struct tm ma{},mb{};
#ifdef _WIN32
        gmtime_s(&ma,&a); gmtime_s(&mb,&b);
#else
        gmtime_r(&a,&ma); gmtime_r(&b,&mb);
#endif
        char sa[20],sb[20];
        strftime(sa,20,"%Y-%m-%d",&ma); strftime(sb,20,"%Y-%m-%d",&mb);
        printf("[RANGE] %s → %s\n", sa, sb);
    }

    // Warmup cutoff
    if(cfg.warm>0 && cfg.warm<(int64_t)ticks.size())
        store::warmup_sec = ticks[(size_t)cfg.warm].ts_ms/1000;

    // ── Construct engines ─────────────────────────────────────────────────────
    std::unique_ptr<GoldRunner>    rg;
    std::unique_ptr<FlowRunner>    rf;
    std::unique_ptr<LatencyRunner> rl;
    std::unique_ptr<CrossRunner>   rc;
    std::unique_ptr<BreakRunner>   rb;

    if(cfg.gold)    rg = std::make_unique<GoldRunner>(cfg.lat);
    if(cfg.flow)    rf = std::make_unique<FlowRunner>();
    if(cfg.latency) rl = std::make_unique<LatencyRunner>(cfg.lat);
    if(cfg.cross)   rc = std::make_unique<CrossRunner>();
    if(cfg.breakout)rb = std::make_unique<BreakRunner>(cfg.lat);

    // ── Tick loop ─────────────────────────────────────────────────────────────
    printf("[RUN] Starting tick loop...\n\n");
    const auto t0r  = std::chrono::steady_clock_real::now();
    const int64_t N = (int64_t)ticks.size();
    int64_t last_p  = 0;

    for(int64_t i=0; i<N; ++i){
        const TickRow& r = ticks[(size_t)i];

        // ADVANCE SIMULATED TIME — must happen before any engine call
        omega::bt::set_sim_time(r.ts_ms);

        if(rg) rg->tick(r);
        if(rf) rf->tick(r);
        if(rl) rl->tick(r);
        if(rc) rc->tick(r);
        if(rb) rb->tick(r);

        if(i-last_p >= 500'000){
            last_p=i;
            const double el = std::chrono::duration<double>(
                std::chrono::steady_clock_real::now()-t0r).count();
            const double tps = el>0?i/el:0;
            printf("\r  [%5.1f%%]  %lld ticks | %5.0fs | %5.0f K t/s | %lld trades | ETA %4.0fs   ",
                   100.0*i/N,(long long)i,el,tps/1000.0,(long long)store::recs.size(),
                   tps>0?(N-i)/tps:0);
            fflush(stdout);
        }
    }

    const double rs = std::chrono::duration<double>(
        std::chrono::steady_clock_real::now()-t0r).count();
    printf("\n\n[RUN] %lld ticks in %.1fs = %.0f K t/s\n\n",(long long)N,rs,N/rs/1000.0);

    // ── Summary table ─────────────────────────────────────────────────────────
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

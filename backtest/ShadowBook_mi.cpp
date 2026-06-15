// =============================================================================
// ShadowBookBacktest.cpp -- Faithful backtest of the LIVE-ENABLED XAUUSD engines
//
// Tests ONLY the 24 XAUUSD engines that are enabled in production, each with its
// REAL live config replicated verbatim from include/engine_init.hpp, and each
// dispatched exactly as include/tick_gold.hpp / include/on_tick.hpp drive it
// live. The ONLY deviation from engine_init is shadow_mode=false (so the
// trade-logging callback path fires -- the OmegaBacktest harness convention;
// shadow only suppresses real orders, never the ledger callback).
//
// BUILD (standalone, no CMake):
//   cd /Users/jo/Omega && clang++ -O2 -std=c++17 -DOMEGA_BACKTEST -Iinclude \
//       -o /tmp/ShadowBookBacktest backtest/ShadowBookBacktest.cpp
//
// RUN:
//   /tmp/ShadowBookBacktest /Users/jo/Tick/xau_6mo_ds10.csv
//
// Trade CSV -> /tmp/xau_book_trades.csv (same columns as OmegaBacktest).
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
#include <memory>
#include <chrono>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../include/OmegaTradeLedger.hpp"
#include "../include/OpenPositionRegistry.hpp"

// g_open_positions is `extern` in OpenPositionRegistry.hpp and normally defined
// once in globals.hpp; we don't pull in globals.hpp (winsock etc.) so define it
// here. ClusterGate (used by some cell engines) reads it via snapshot_all().
omega::OpenPositionRegistry g_open_positions;

// ---- The 24 live XAUUSD engines (real headers) ------------------------------
#include "../include/AdaptiveHullEngine.hpp"
#include "../include/SupertrendGoldEngine.hpp"
#include "../include/GoldOrbRetraceEngine.hpp"
#include "../include/GoldPanicBounceEngine.hpp"
#include "../include/GoldSeasonalEngine.hpp"
#include "../include/GoldOversoldBounceEngine.hpp"
#include "../include/BreakBounceEngine.hpp"
#include "../include/SessionMomentumEngine.hpp"
#include "../include/GoldVolBreakoutM30Engine.hpp"
#include "../include/XauStraddleM30Engine.hpp"
#include "../include/XauThreeBar30mEngine.hpp"
#include "../include/XauTrendFollow1hEngine.hpp"
#include "../include/XauTrendFollow2hEngine.hpp"
#include "../include/XauTrendFollow4hEngine.hpp"
#include "../include/XauTrendFollowD1Engine.hpp"
#include "../include/XauTurtleD1Engine.hpp"
#include "../include/XauDojiRejD1Engine.hpp"
#include "../include/XauOutsideBarD1Engine.hpp"
#include "../include/DonchianEngine.hpp"
#include "../include/EmaPullbackEngine.hpp"
#include "../include/CellEngine.hpp"
#include "../include/TsmomStrategy.hpp"

// ---- Non-XAU live-enabled engines (NAS/index/FX) ----------------------------
#include "../include/IndexBearShortEngine.hpp"
#include "../include/NasTurtleD1Engine.hpp"
#include "../include/UstecTrendFollowHtfEngine.hpp"
#include "../include/IndexIntradayDriftEngine.hpp"
#include "../include/MinimalH4US30Breakout.hpp"
#include "../include/Us30EnsembleEngine.hpp"
#include "../include/Ger40KeltnerH1Engine.hpp"
#include "../include/Ger40LondonBreakoutEngine.hpp"
#include "../include/MinimalH4GER40Breakout.hpp"
#include "../include/OrbBreakoutEngine.hpp"
#include "../include/AtrMeanRevGridEngine.hpp"
#include "../include/FxTurtleH4Engine.hpp"
#include "../include/FxCrossRevEngine.hpp"
#include "../include/EurGbpPairsEngine.hpp"
#include "../include/TrendLineBreakEngine.hpp"

// =============================================================================
// Seed path resolver -- absolute to the Omega repo so the binary runs from /tmp.
// =============================================================================
static const char* kRepo = "/Users/jo/Omega/";
static std::string SEED(const char* rel) { return std::string(kRepo) + rel; }

// =============================================================================
// Memory-mapped file (POSIX)
// =============================================================================
class MemMappedFile {
public:
    const char* data = nullptr;
    size_t      size = 0;
    bool open(const char* path) noexcept {
        fd_ = ::open(path, O_RDONLY);
        if (fd_ < 0) return false;
        struct stat st{}; fstat(fd_, &st);
        size = (size_t)st.st_size;
        data = (const char*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (data == (const char*)-1) { data = nullptr; ::close(fd_); return false; }
        madvise(const_cast<char*>(data), size, MADV_SEQUENTIAL);
        return true;
    }
    ~MemMappedFile() {
        if (data) munmap(const_cast<char*>(data), size);
        if (fd_ >= 0) ::close(fd_);
    }
private:
    int fd_ = -1;
};

// =============================================================================
// CSV parsing -- format: timestamp_ms,bid,ask (auto-detect ask-first header)
// =============================================================================
static inline double fast_f(const char* s, const char** e) noexcept {
    while (*s == ' ') ++s;
    bool n = (*s == '-'); if (n) ++s;
    double v = 0;
    while (*s >= '0' && *s <= '9') v = v*10.0 + (*s++ - '0');
    if (*s == '.') { ++s; double f=0.1; while (*s>='0'&&*s<='9'){v+=(*s++-'0')*f;f*=.1;} }
    if (e) *e = s;
    return n ? -v : v;
}
static inline int64_t fast_i64(const char* s, const char** e) noexcept {
    while (*s == ' ' || *s == '"') ++s;
    int64_t v = 0;
    while (*s >= '0' && *s <= '9') v = v*10 + (*s++ - '0');
    if (e) *e = s;
    return v;
}

struct TickRow { int64_t ts_ms; double bid, ask; };

// HISTDATA timestamp: "YYYYMMDD HHMMSSmmm" (space-separated date + time-with-ms).
// Converts to epoch milliseconds (UTC -- HistData ticks are EST but we treat the
// stamp as-is for relative bar aggregation; engines key off UTC-hour gates, so
// this matches how the production HistData->bar pipeline consumes the corpus).
static inline int64_t histdata_to_ms(int64_t yyyymmdd, int64_t hhmmssmmm) noexcept {
    const int y  = (int)(yyyymmdd / 10000);
    const int mo = (int)((yyyymmdd / 100) % 100);
    const int d  = (int)(yyyymmdd % 100);
    const int ms  = (int)(hhmmssmmm % 1000);
    const int hms = (int)(hhmmssmmm / 1000);
    const int h   = hms / 10000;
    const int mi  = (hms / 100) % 100;
    const int s   = hms % 100;
    // days-from-civil (Howard Hinnant) -> epoch days
    int yy = y - (mo <= 2);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = (unsigned)(yy - era * 400);
    const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t epoch_days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return (((epoch_days * 24 + h) * 60 + mi) * 60 + s) * 1000LL + ms;
}

// Detected CSV layout. HISTDATA: "YYYYMMDD HHMMSSmmm,bid,ask[,vol]".
// MS_TS: "ts_ms,c1,c2" with c1/c2 = bid/ask (or ask/bid if ask_first).
enum class CsvFmt { MS_TS, HISTDATA };

static std::vector<TickRow> parse_csv(const MemMappedFile& f, double price_div = 1.0) {
    std::vector<TickRow> v; v.reserve(5'000'000);
    const char* p   = f.data;
    const char* end = p + f.size;
    if (f.size>=3 && (uint8_t)p[0]==0xEF && (uint8_t)p[1]==0xBB && (uint8_t)p[2]==0xBF) p+=3;

    // --- header sniff (only consumes a leading non-numeric line) ---
    bool ask_first = false;
    if (p<end && *p && (*p<'0'||*p>'9')) {
        const char* ask_pos = nullptr; const char* bid_pos = nullptr;
        while (p<end && *p!='\n') {
            if (!ask_pos && (p[0]=='a'||p[0]=='A') && (p[1]=='s'||p[1]=='S') && (p[2]=='k'||p[2]=='K')) ask_pos=p;
            if (!bid_pos && (p[0]=='b'||p[0]=='B') && (p[1]=='i'||p[1]=='I') && (p[2]=='d'||p[2]=='D')) bid_pos=p;
            ++p;
        }
        if (ask_pos && bid_pos && ask_pos < bid_pos) ask_first = true;
        if (p<end) ++p;
    }

    // --- format sniff on first data row: a space inside the first field before
    //     the first comma => HISTDATA "YYYYMMDD HHMMSSmmm". Otherwise MS_TS. ---
    CsvFmt fmt = CsvFmt::MS_TS;
    {
        const char* q = p;
        while (q<end && *q!='\n' && *q!=',' && *q!='\r') {
            if (*q==' ') { fmt = CsvFmt::HISTDATA; break; }
            ++q;
        }
    }
    if (fmt==CsvFmt::HISTDATA) fprintf(stderr, "[CSV] HISTDATA format detected (YYYYMMDD HHMMSSmmm)\n");
    if (ask_first)             fprintf(stderr, "[CSV] ask-first detected -- swapping columns\n");

    while (p < end) {
        while (p<end && (*p=='\r'||*p=='\n')) ++p;
        if (p>=end) break;
        TickRow r{};
        const char* nx;
        if (fmt==CsvFmt::HISTDATA) {
            const int64_t ymd = fast_i64(p,&nx); p=nx;
            while (p<end && *p==' ') ++p;            // the date/time separator
            const int64_t hms = fast_i64(p,&nx); p=nx;
            if (*p==',') ++p;
            r.ts_ms = histdata_to_ms(ymd, hms);
            double c1 = fast_f(p,&nx); p=nx; if(*p==',')++p;  // bid
            double c2 = fast_f(p,&nx); p=nx;                  // ask
            // HISTDATA is always bid,ask -- but honour ask_first if a header said so
            if (ask_first) { r.ask=c1; r.bid=c2; } else { r.bid=c1; r.ask=c2; }
        } else {
            r.ts_ms = fast_i64(p,&nx); p=nx; if(*p==',')++p;
            double c1 = fast_f(p,&nx); p=nx; if(*p==',')++p;
            double c2 = fast_f(p,&nx); p=nx;
            if (ask_first) { r.ask=c1; r.bid=c2; } else { r.bid=c1; r.ask=c2; }
        }
        if (price_div != 1.0) { r.bid /= price_div; r.ask /= price_div; }
        while (p<end && *p!='\n') ++p;
        if (r.ts_ms>0 && r.bid>0 && r.ask>r.bid) v.push_back(r);
    }
    return v;
}

// =============================================================================
// Global trade store (collects TradeRecords; skips warmup window by ts)
// =============================================================================
struct Stat {
    std::string name; int64_t n=0, wins=0; double pnl=0;
    void add(double p){ ++n; if(p>0)++wins; pnl+=p; }
};
namespace store {
    static std::vector<omega::TradeRecord> recs;
    static std::unordered_map<std::string,Stat> stats;
    static int64_t warmup_sec = 0;
    static void add(const omega::TradeRecord& tr) {
        if (tr.entryTs < warmup_sec) return;       // drop cold-start / pre-window trades
        recs.push_back(tr);
        auto& s = stats[tr.engine];
        if (s.name.empty()) s.name = tr.engine.empty() ? "<blank>" : tr.engine;
        s.add(tr.pnl);
    }
}
// Generic std::function callback (all engines use std::function<void(const TradeRecord&)>)
static std::function<void(const omega::TradeRecord&)> CB() {
    return [](const omega::TradeRecord& t){ store::add(t); };
}

// =============================================================================
// Engine globals (file-scope, like globals.hpp) + tagged engine names.
// Real configs replicated from engine_init.hpp; shadow_mode=false so the
// callback path fires. enabled=true (all are live-enabled).
// =============================================================================
static omega::AdaptiveHullEngine        g_adhull_xau;
static omega::SupertrendGoldEngine      g_supertrend_gold;
static omega::GoldOrbRetraceEngine      g_gold_orb_retrace;
static omega::GoldPanicBounceEngine     g_gold_panic_bounce;
static omega::GoldSeasonalEngine        g_gold_seasonal;
static omega::GoldOversoldBounceEngine  g_gold_oversold;
static omega::BreakBounceEngine         g_xau_breakbounce;
static omega::SessionMomentumEngine     g_xau_sess_nypm;
static omega::SessionMomentumEngine     g_xau_sess_overnight;
static omega::GoldVolBreakoutM30Engine  g_gold_volbrk_m30;
static omega::XauStraddleM30Engine      g_xau_straddle_m30;
static omega::XauStraddleM30Engine      g_xau_straddle_m15;
static omega::XauThreeBar30mEngine      g_xau_threebar_30m;
static omega::XauTrendFollow1hEngine    g_xau_tf_1h;
static omega::XauTrendFollow1hEngine    g_xau_tf_m15;   // same type, fed M15
static omega::XauTrendFollow2hEngine    g_xau_tf_2h;
static omega::XauTrendFollow4hEngine    g_xau_tf_4h;
static omega::XauTrendFollowD1Engine    g_xau_tf_d1;
static omega::XauTurtleD1Engine         g_xau_turtle_d1;
static omega::XauDojiRejD1Engine        g_xau_doji_rej_d1;
static omega::XauOutsideBarD1Engine     g_xau_outside_bar_d1;
static omega::DonchianPortfolio         g_donchian;
static omega::EpbPortfolio              g_ema_pullback;
static omega::cell::TsmomPortfolioV2    g_tsmom_v2;
// ---- non-XAU engine globals (declared like above; globals.hpp not included) ----
static omega::IndexBearShortEngine      g_idx_bear_short_nas;
static omega::IndexBearShortEngine      g_idx_bear_short_sp;
static omega::GoldOrbRetraceEngine      g_nas_orb_retrace;
static omega::UstecTrendFollowHtfEngine g_ustec_tf_htf;
static omega::NasTurtleD1Engine         g_nas_turtle_d1;
static omega::IndexIntradayDriftEngine  g_idd_us30;
static omega::IndexIntradayDriftEngine  g_idd_sp;
static omega::IndexIntradayDriftEngine  g_idd_uk100;
static omega::Us30EnsembleEngine        g_us30_ensemble;
static omega::MinimalH4US30Breakout     g_minimal_h4_us30;
static omega::MinimalH4GER40Breakout    g_minimal_h4_ger40;
static omega::Ger40KeltnerH1Engine      g_ger40_kelt;
static omega::Ger40LondonBreakoutEngine g_ger40_london_brk;
static omega::AtrMeanRevGridEngine<omega::AmrTraits_EURUSD> g_amr_eurusd;
static omega::AtrMeanRevGridEngine<omega::AmrTraits_GBPUSD> g_amr_gbpusd;
static omega::FxTurtleH4Engine          g_eurusd_turtle_h4;
static omega::FxTurtleH4Engine          g_gbpusd_turtle_h4;
static omega::FxCrossRevEngine          g_fx_xrev_eurgbp{"EURGBP"};

// Tag a TradeRecord's engine field on emit (some engines don't set it; we
// stamp it so per-engine breakdown is correct).  Wrap store::add with a name.
static std::function<void(const omega::TradeRecord&)> tagCB(const char* name) {
    std::string nm = name;
    return [nm](const omega::TradeRecord& t){
        omega::TradeRecord tr = t;
        if (tr.engine.empty()) tr.engine = nm;
        store::add(tr);
    };
}

// =============================================================================
// init_all -- replicate live config + warm-seed for every engine.
// =============================================================================
static void init_all() {
    // ---- AdaptiveHullXAU (engine_init ~4978) ----
    g_adhull_xau.symbol="XAUUSD"; g_adhull_xau.engine_name="AdaptiveHullXAU";
    g_adhull_xau.TF_SEC=3600; g_adhull_xau.PMUL=2.0; g_adhull_xau.KATR=3.0;
    g_adhull_xau.SESS0=-1; g_adhull_xau.SESS1=-1;
    g_adhull_xau.shadow_mode=false; g_adhull_xau.enabled=true; g_adhull_xau.lot=0.01;
    g_adhull_xau.init();
    g_adhull_xau.seed_from_csv(SEED("phase1/signal_discovery/warmup_XAUUSD_H1.csv"));
    g_adhull_xau.on_trade_record = tagCB("AdaptiveHullXAU");

    // ---- SupertrendGold (engine_init ~5013) ----
    g_supertrend_gold.symbol="XAUUSD"; g_supertrend_gold.engine_name="SupertrendGold";
    g_supertrend_gold.TF_SEC=3600; g_supertrend_gold.ST_LEN=10; g_supertrend_gold.ST_MULT=3.0;
    g_supertrend_gold.EMA_LEN=100; g_supertrend_gold.shadow_mode=false; g_supertrend_gold.enabled=true;
    g_supertrend_gold.lot=0.01; g_supertrend_gold.init();
    g_supertrend_gold.seed_from_csv(SEED("phase1/signal_discovery/warmup_XAUUSD_H1.csv"));
    g_supertrend_gold.on_trade_record = tagCB("SupertrendGold");

    // ---- GoldOrbRetrace (engine_init ~4684) ----
    g_gold_orb_retrace.symbol="XAUUSD"; g_gold_orb_retrace.engine_name="GoldOrbRetrace";
    g_gold_orb_retrace.shadow_mode=false; g_gold_orb_retrace.enabled=false;
    g_gold_orb_retrace.verbose=false; g_gold_orb_retrace.lot=0.01;
    g_gold_orb_retrace.seed_from_csv(SEED("phase1/signal_discovery/warmup_XAUUSD_M5.csv"));
    g_gold_orb_retrace.on_trade_record = tagCB("GoldOrbRetrace");

    // ---- GoldPanicBounce (engine_init ~4711) ----
    g_gold_panic_bounce.shadow_mode=false; g_gold_panic_bounce.enabled=true;
    g_gold_panic_bounce.DROP_K=8.0; g_gold_panic_bounce.DD_LOOKBACK=250; g_gold_panic_bounce.TRAIL_ATR=4.5;
    g_gold_panic_bounce.on_close_cb = tagCB("GoldPanicBounce");
    g_gold_panic_bounce.seed_from_h1_csv(SEED("phase1/signal_discovery/warmup_XAUUSD_H1.csv"));

    // ---- GoldSeasonal (engine_init ~2457) ----
    g_gold_seasonal.shadow_mode=false; g_gold_seasonal.enabled=true;
    g_gold_seasonal.lot=0.01; g_gold_seasonal.usd_per_pt=100.0;
    g_gold_seasonal.entry_mask=(1<<1)|(1<<2); g_gold_seasonal.gate_risk_off=false;
    g_gold_seasonal.seed_from_d1_csv(SEED("phase1/signal_discovery/warmup_XAUUSD_D1.csv"));

    // ---- GoldOversoldBounce (engine_init ~2473) ----
    g_gold_oversold.shadow_mode=false; g_gold_oversold.enabled=false;
    g_gold_oversold.lot=0.01; g_gold_oversold.usd_per_pt=100.0;
    g_gold_oversold.entry_rsi=30.0; g_gold_oversold.exit_rsi=50.0;
    g_gold_oversold.max_hold_days=20; g_gold_oversold.stop_atr_mult=2.5;
    g_gold_oversold.seed_from_d1_csv(SEED("phase1/signal_discovery/warmup_XAUUSD_D1.csv"));

    // ---- BreakBounce (engine_init ~4278) ----
    g_xau_breakbounce.symbol="XAUUSD"; g_xau_breakbounce.engine_name="BreakBounce";
    g_xau_breakbounce.shadow_mode=false; g_xau_breakbounce.enabled=true;
    g_xau_breakbounce.lot=0.01; g_xau_breakbounce.MAX_SPREAD=0.60;
    g_xau_breakbounce.USE_SESSION=true; g_xau_breakbounce.USE_PROFIT_LOCK=true;
    g_xau_breakbounce.USE_L2_PROTECT=false; g_xau_breakbounce.REGIME_ADX_MIN=0.0;
    g_xau_breakbounce.init();
    g_xau_breakbounce.seed_from_csvs(
        SEED("phase1/signal_discovery/warmup_XAUUSD_D1.csv"),
        SEED("phase1/signal_discovery/warmup_XAUUSD_H1.csv"),
        SEED("phase1/signal_discovery/warmup_XAUUSD_M30.csv"));
    g_xau_breakbounce.on_trade_record = tagCB("BreakBounce");

    // ---- SessionMomentum x2 (engine_init ~2163 / ~2180) ----
    g_xau_sess_nypm.symbol="XAUUSD"; g_xau_sess_nypm.label="XauSessNYpm_h16L4_EMA200_S42";
    g_xau_sess_nypm.entry_hour=16; g_xau_sess_nypm.hold_hours=4;
    g_xau_sess_nypm.use_trend_filter=true; g_xau_sess_nypm.ema_period=200;
    g_xau_sess_nypm.sl_atr=0.0; g_xau_sess_nypm.skip_dow_mask=(1<<5);
    g_xau_sess_nypm.shadow_mode=false; g_xau_sess_nypm.enabled=true;
    g_xau_sess_nypm.lot=0.01; g_xau_sess_nypm.max_spread=2.0;
    g_xau_sess_nypm.warmup_csv_path=SEED("phase1/signal_discovery/warmup_XAUUSD_H1.csv");
    g_xau_sess_nypm.init();
    g_xau_sess_nypm.warmup_from_csv(g_xau_sess_nypm.warmup_csv_path);

    g_xau_sess_overnight.symbol="XAUUSD"; g_xau_sess_overnight.label="XauSessOvernight_h23L5_EMA200_S42";
    g_xau_sess_overnight.entry_hour=23; g_xau_sess_overnight.hold_hours=5;
    g_xau_sess_overnight.use_trend_filter=true; g_xau_sess_overnight.ema_period=200;
    g_xau_sess_overnight.sl_atr=0.0;
    g_xau_sess_overnight.shadow_mode=false; g_xau_sess_overnight.enabled=true;
    g_xau_sess_overnight.lot=0.01; g_xau_sess_overnight.max_spread=2.0;
    g_xau_sess_overnight.warmup_csv_path=SEED("phase1/signal_discovery/warmup_XAUUSD_H1.csv");
    g_xau_sess_overnight.init();
    g_xau_sess_overnight.warmup_from_csv(g_xau_sess_overnight.warmup_csv_path);

    // ---- GoldVolBreakoutM30 (engine_init ~2143) ----
    g_gold_volbrk_m30.shadow_mode=false; g_gold_volbrk_m30.enabled=true;
    g_gold_volbrk_m30.lot=0.01; g_gold_volbrk_m30.max_spread=0.80;
    g_gold_volbrk_m30.init();
    g_gold_volbrk_m30.seed_h1_from_csv (SEED("phase1/signal_discovery/warmup_XAUUSD_H1.csv"));
    g_gold_volbrk_m30.seed_m30_from_csv(SEED("phase1/signal_discovery/warmup_XAUUSD_M30.csv"));

    // ---- XauStraddleM30 (engine_init ~1882) ----
    g_xau_straddle_m30.shadow_mode=false; g_xau_straddle_m30.enabled=true;
    g_xau_straddle_m30.symbol="XAUUSD"; g_xau_straddle_m30.engine_name="XauStraddleM30";
    g_xau_straddle_m30.box_n=15; g_xau_straddle_m30.stop_atr=3.0; g_xau_straddle_m30.tp_r=1.0;
    g_xau_straddle_m30.lot=0.01; g_xau_straddle_m30.partial_frac=0.30; g_xau_straddle_m30.partial_r=0.7;
    g_xau_straddle_m30.obi_tilt=true;
    g_xau_straddle_m30.seed_from_csv(SEED("phase1/signal_discovery/warmup_XAUUSD_M30.csv"));

    // ---- XauStraddleM15 (engine_init ~1904) ----
    g_xau_straddle_m15.shadow_mode=false; g_xau_straddle_m15.enabled=false;
    g_xau_straddle_m15.symbol="XAUUSD"; g_xau_straddle_m15.engine_name="XauStraddleM15";
    g_xau_straddle_m15.box_n=15; g_xau_straddle_m15.stop_atr=3.0; g_xau_straddle_m15.tp_r=1.0;
    g_xau_straddle_m15.lot=0.01; g_xau_straddle_m15.partial_frac=0.30; g_xau_straddle_m15.partial_r=0.7;
    g_xau_straddle_m15.hold_max_bars=96; g_xau_straddle_m15.obi_tilt=true;
    g_xau_straddle_m15.seed_from_csv(SEED("phase1/signal_discovery/warmup_XAUUSD_M15.csv"));

    // ---- XauThreeBar30m (engine_init ~2760) ----
    g_xau_threebar_30m.LOSS_CUT_PCT=0.05; g_xau_threebar_30m.BE_ARM_PCT=0.03; g_xau_threebar_30m.BE_BUFFER_PCT=0.012;
    g_xau_threebar_30m.shadow_mode=false; g_xau_threebar_30m.enabled=false; g_xau_threebar_30m.long_only=true;
    g_xau_threebar_30m.lot=0.01; g_xau_threebar_30m.max_spread=1.0;
    g_xau_threebar_30m.be_trigger_atr=1.0; g_xau_threebar_30m.be_cost_buffer_pts=0.10;
    g_xau_threebar_30m.trail_after_be=true; g_xau_threebar_30m.trail_atr_mult=0.75;
    g_xau_threebar_30m.min_atr_floor=0.30; g_xau_threebar_30m.max_bars_held=0;
    g_xau_threebar_30m.daily_loss_limit=0.0; g_xau_threebar_30m.max_consec_losses=0;
    g_xau_threebar_30m.max_atr_ceil=0.0; g_xau_threebar_30m.block_hour_start=-1; g_xau_threebar_30m.block_hour_end=-1;
    g_xau_threebar_30m.use_slope_gate=true; g_xau_threebar_30m.slope_lookback_bars=12;
    g_xau_threebar_30m.use_vol_band_gate=true; g_xau_threebar_30m.vol_band_low_pct=0.30; g_xau_threebar_30m.vol_band_high_pct=0.85;
    g_xau_threebar_30m.warmup_csv_path=SEED("phase1/signal_discovery/warmup_XAUUSD_M30.csv");
    g_xau_threebar_30m.init();
    g_xau_threebar_30m.warmup_from_csv(g_xau_threebar_30m.warmup_csv_path);

    // ---- XauTrendFollow1h (engine_init ~1486) ----
    g_xau_tf_1h.shadow_mode=false; g_xau_tf_1h.enabled=true;
    g_xau_tf_1h.cell_enable_mask=0x0F; g_xau_tf_1h.lot=0.01; g_xau_tf_1h.max_spread=1.0;
    g_xau_tf_1h.use_vol_target=true; g_xau_tf_1h.vol_target_unit=0.10;
    g_xau_tf_1h.pyramid_max_adds=2; g_xau_tf_1h.pyramid_step_atr=1.0; g_xau_tf_1h.pyramid_sl_atr=3.0;
    g_xau_tf_1h.warmup_csv_path=SEED("phase1/signal_discovery/warmup_XAUUSD_H1.csv");
    g_xau_tf_1h.init();
    g_xau_tf_1h.warmup_from_csv(g_xau_tf_1h.warmup_csv_path);

    // ---- XauTrendFollow M15 (engine_init ~1527) ----
    g_xau_tf_m15.shadow_mode=false; g_xau_tf_m15.enabled=true;
    g_xau_tf_m15.cell_enable_mask=0x02; g_xau_tf_m15.lot=0.01; g_xau_tf_m15.max_spread=1.0;
    g_xau_tf_m15.use_vol_target=false; g_xau_tf_m15.pyramid_max_adds=0;
    g_xau_tf_m15.warmup_csv_path=SEED("phase1/signal_discovery/warmup_XAUUSD_M15.csv");
    g_xau_tf_m15.init();
    g_xau_tf_m15.warmup_from_csv(g_xau_tf_m15.warmup_csv_path);

    // ---- XauTrendFollow2h (engine_init ~2697) ----
    g_xau_tf_2h.shadow_mode=false; g_xau_tf_2h.enabled=true;
    g_xau_tf_2h.use_adx_gate=true; g_xau_tf_2h.adx_min=25.0; g_xau_tf_2h.cell_adx_mask=0xB;
    g_xau_tf_2h.use_vol_band_gate=true; g_xau_tf_2h.vol_band_low_pct=0.30; g_xau_tf_2h.vol_band_high_pct=0.85;
    g_xau_tf_2h.cell_vol_band_mask=0x4; g_xau_tf_2h.lot=0.01; g_xau_tf_2h.max_spread=1.0;
    g_xau_tf_2h.warmup_csv_path=SEED("phase1/signal_discovery/warmup_XAUUSD_H1.csv");
    g_xau_tf_2h.init();
    g_xau_tf_2h.warmup_from_csv(g_xau_tf_2h.warmup_csv_path);

    // ---- XauTrendFollow4h (engine_init ~1429) ----
    g_xau_tf_4h.shadow_mode=false; g_xau_tf_4h.enabled=true;
    g_xau_tf_4h.cell_enable_mask=0xC9; g_xau_tf_4h.lot=0.01; g_xau_tf_4h.max_spread=1.0;
    g_xau_tf_4h.use_vol_band_gate=true; g_xau_tf_4h.vol_band_low_pct=0.30; g_xau_tf_4h.vol_band_high_pct=0.85;
    g_xau_tf_4h.cell_vol_band_mask=0x8;
    g_xau_tf_4h.warmup_csv_path=SEED("phase1/signal_discovery/warmup_XAUUSD_H4.csv");
    g_xau_tf_4h.init();
    g_xau_tf_4h.warmup_from_csv(g_xau_tf_4h.warmup_csv_path);

    // ---- XauTrendFollowD1 (engine_init ~1661) ----
    g_xau_tf_d1.shadow_mode=false; g_xau_tf_d1.enabled=true;
    g_xau_tf_d1.use_vol_band_gate=true; g_xau_tf_d1.vol_band_low_pct=0.20; g_xau_tf_d1.vol_band_high_pct=0.90;
    g_xau_tf_d1.lot=0.01; g_xau_tf_d1.max_spread=1.0;
    g_xau_tf_d1.warmup_csv_path=SEED("phase1/signal_discovery/warmup_XAUUSD_H4.csv");
    g_xau_tf_d1.init();
    g_xau_tf_d1.warmup_from_csv(g_xau_tf_d1.warmup_csv_path);

    // ---- XauTurtleD1 (engine_init ~1720) -- internal H4->D1 + atr seed, no CSV
    g_xau_turtle_d1.p = omega::make_xau_turtle_d1_params();
    g_xau_turtle_d1.shadow_mode=false; g_xau_turtle_d1.enabled=true; g_xau_turtle_d1.symbol="XAUUSD";

    // ---- XauDojiRejD1 (engine_init ~2555) -- internal warm, no CSV
    g_xau_doji_rej_d1.p = omega::make_xau_doji_rej_d1_params();
    g_xau_doji_rej_d1.shadow_mode=false; g_xau_doji_rej_d1.enabled=true; g_xau_doji_rej_d1.symbol="XAUUSD";

    // ---- XauOutsideBarD1 (engine_init ~2564) -- internal warm, no CSV
    g_xau_outside_bar_d1.p = omega::make_xau_outside_bar_d1_params();
    g_xau_outside_bar_d1.shadow_mode=false; g_xau_outside_bar_d1.enabled=true; g_xau_outside_bar_d1.symbol="XAUUSD";

    // ---- DonchianPortfolio (engine_init ~1402) ----
    g_donchian.shadow_mode=false; g_donchian.enabled=false; g_donchian.max_concurrent=7;
    g_donchian.risk_pct=0.005; g_donchian.start_equity=10000.0; g_donchian.margin_call=1000.0;
    g_donchian.max_lot_cap=0.05; g_donchian.block_on_risk_off=true;
    g_donchian.warmup_csv_path=SEED("phase1/signal_discovery/tsmom_warmup_H1.csv");
    g_donchian.init();
    g_donchian.warmup_from_csv(g_donchian.warmup_csv_path);

    // ---- EmaPullbackPortfolio (engine_init ~2957) ----
    g_ema_pullback.shadow_mode=false; g_ema_pullback.enabled=false;
    g_ema_pullback.cell_enable_mask=0x0C; g_ema_pullback.max_concurrent=4;
    g_ema_pullback.risk_pct=0.005; g_ema_pullback.start_equity=10000.0; g_ema_pullback.margin_call=1000.0;
    g_ema_pullback.max_lot_cap=0.05; g_ema_pullback.block_on_risk_off=true;
    g_ema_pullback.LOSS_CUT_PCT=0.10; g_ema_pullback.BE_ARM_PCT=0.40; g_ema_pullback.BE_BUFFER_PCT=0.05;
    g_ema_pullback.warmup_csv_path=SEED("phase1/signal_discovery/tsmom_warmup_H1.csv");
    g_ema_pullback.init();
    g_ema_pullback.warmup_from_csv(g_ema_pullback.warmup_csv_path);

    // ---- TsmomPortfolioV2 (engine_init ~1374) ----
    g_tsmom_v2.shadow_mode=false; g_tsmom_v2.enabled=true;
    g_tsmom_v2.max_concurrent=50; g_tsmom_v2.max_positions_per_cell=1;
    g_tsmom_v2.risk_pct=0.005; g_tsmom_v2.start_equity=10000.0; g_tsmom_v2.margin_call=1000.0;
    g_tsmom_v2.max_lot_cap=0.05; g_tsmom_v2.usd_per_pt_per_lot=100.0;
    g_tsmom_v2.block_on_risk_off=true; g_tsmom_v2.symbol="XAUUSD"; g_tsmom_v2.regime_label="TSMOM_V2";
    omega::cell::build_default_tsmom_topology(g_tsmom_v2);
    g_tsmom_v2.warmup_csv_path=SEED("phase1/signal_discovery/tsmom_warmup_H1.csv");
    g_tsmom_v2.init();
    g_tsmom_v2.warmup_from_csv(g_tsmom_v2.warmup_csv_path);
}

// =============================================================================
// Bar aggregation -- mirrors tick_gold.hpp's OHLC accumulation exactly.
// =============================================================================
struct AggBar { int64_t start_min; double open, high, low, close; };

// External-bar BtBarEngine instances for the engines that need indicator ctx /
// external ATR (tf_1h, tf_4h, tf_d1, tf_2h, m15 tf, threebar M30).
static omega::bt::BtBarEngine<60>   bt_h1;   // H1 ATR for tf_1h
static omega::bt::BtBarEngine<240>  bt_h4;   // H4 ATR for tf_4h
static omega::bt::BtBarEngine<30>   bt_m30;  // M30 ATR for threebar
static omega::bt::BtBarEngine<15>   bt_m15;  // M15 ATR for tf_m15

// =============================================================================
// NON-XAU engines: faithful config from engine_init.hpp + dispatch from
// tick_indices.hpp / tick_fx.hpp. 3-arg on_tick engines emit via on_close_cb /
// on_trade_record (set here); 4-arg engines take the cb param. trendline_break
// (needs on_h4_bar) deferred.
// =============================================================================
static std::string g_inst = "XAUUSD";

static void init_nonxau() {
    // ---- NAS100 ----
    g_idx_bear_short_nas.symbol="NAS100"; g_idx_bear_short_nas.engine_name="IndexBearShortNAS";
    g_idx_bear_short_nas.COST_PTS=2.0; g_idx_bear_short_nas.lot=1.0; g_idx_bear_short_nas.USE_RISKOFF_GATE=false;
    g_idx_bear_short_nas.shadow_mode=false; g_idx_bear_short_nas.enabled=true;
    g_idx_bear_short_nas.on_close_cb=tagCB("IndexBearShortNAS");

    g_nas_orb_retrace.symbol="NAS100"; g_nas_orb_retrace.engine_name="NasOrbRetrace"; g_nas_orb_retrace.tag="NASORB";
    g_nas_orb_retrace.or_start_et=570; g_nas_orb_retrace.or_end_et=600; g_nas_orb_retrace.ema_len=50;
    g_nas_orb_retrace.retr=0.382; g_nas_orb_retrace.trail_win=3; g_nas_orb_retrace.max_spread=5.0;
    g_nas_orb_retrace.lot=1.0; g_nas_orb_retrace.verbose=false;
    g_nas_orb_retrace.shadow_mode=false; g_nas_orb_retrace.enabled=true;
    g_nas_orb_retrace.on_trade_record=tagCB("NasOrbRetrace");

    g_ustec_tf_htf.lot=0.1; g_ustec_tf_htf.max_spread=5.0;
    g_ustec_tf_htf.be_trigger_atr=1.0; g_ustec_tf_htf.be_cost_buffer_pts=0.50; g_ustec_tf_htf.trail_after_be=true;
    g_ustec_tf_htf.trail_atr_mult=0.75; g_ustec_tf_htf.min_atr_floor=5.0;
    g_ustec_tf_htf.shadow_mode=false; g_ustec_tf_htf.enabled=true;

    g_nas_turtle_d1.symbol="NAS100"; g_nas_turtle_d1.shadow_mode=false; g_nas_turtle_d1.enabled=true;

    // ---- DJ30 ----
    g_idd_us30.symbol="DJ30.F"; g_idd_us30.ENTRY_HOUR_UTC=0; g_idd_us30.EXIT_HOUR_UTC=23; g_idd_us30.SAFETY_SL_PCT=2.5;
    g_idd_us30.shadow_mode=false; g_idd_us30.enabled=true;

    g_us30_ensemble.lot=0.01; g_us30_ensemble.max_spread=10.0;
    g_us30_ensemble.be_trigger_atr=0.0; g_us30_ensemble.trail_after_be=false; g_us30_ensemble.min_atr_floor=10.0;
    g_us30_ensemble.shadow_mode=false; g_us30_ensemble.enabled=true; g_us30_ensemble.init();

    g_minimal_h4_us30.symbol="DJ30.F"; g_minimal_h4_us30.shadow_mode=false; g_minimal_h4_us30.enabled=true;

    // ---- US500 ----
    g_idd_sp.symbol="US500.F"; g_idd_sp.ENTRY_HOUR_UTC=0; g_idd_sp.EXIT_HOUR_UTC=23; g_idd_sp.SAFETY_SL_PCT=2.5;
    g_idd_sp.shadow_mode=false; g_idd_sp.enabled=true;

    g_idx_bear_short_sp.symbol="US500.F"; g_idx_bear_short_sp.engine_name="IndexBearShortSP";
    g_idx_bear_short_sp.COST_PTS=0.6; g_idx_bear_short_sp.lot=1.0; g_idx_bear_short_sp.USE_RISKOFF_GATE=false;
    g_idx_bear_short_sp.shadow_mode=false; g_idx_bear_short_sp.enabled=true;
    g_idx_bear_short_sp.on_close_cb=tagCB("IndexBearShortSP");

    // ---- GER40 ----
    g_ger40_kelt.lot=0.01; g_ger40_kelt.max_spread=5.0;
    g_ger40_kelt.shadow_mode=false; g_ger40_kelt.enabled=true; g_ger40_kelt.init();

    g_ger40_london_brk.symbol="GER40"; g_ger40_london_brk.lot=0.01; g_ger40_london_brk.max_spread=4.0;
    g_ger40_london_brk.ASIA_START_HOUR=21; g_ger40_london_brk.ASIA_END_HOUR=7;
    g_ger40_london_brk.ENTRY_START_HOUR=7; g_ger40_london_brk.ENTRY_START_MIN=0;
    g_ger40_london_brk.ENTRY_END_HOUR=9; g_ger40_london_brk.ENTRY_END_MIN=0;
    g_ger40_london_brk.TP_MULT=0.75; g_ger40_london_brk.SL_MULT=0.50; g_ger40_london_brk.MAX_HOLD_SEC=14400;
    g_ger40_london_brk.shadow_mode=false; g_ger40_london_brk.enabled=true;
    g_ger40_london_brk.on_trade_record=tagCB("Ger40LondonBrk");

    g_minimal_h4_ger40.symbol="GER40"; g_minimal_h4_ger40.shadow_mode=false; g_minimal_h4_ger40.enabled=true;

    // ---- UK100 ----
    g_idd_uk100.symbol="UK100"; g_idd_uk100.ENTRY_HOUR_UTC=0; g_idd_uk100.EXIT_HOUR_UTC=23; g_idd_uk100.SAFETY_SL_PCT=2.5;
    g_idd_uk100.shadow_mode=false; g_idd_uk100.enabled=true;

    // ---- FX ----
    g_amr_eurusd.shadow_mode=false; g_amr_eurusd.enabled=true;
    g_amr_eurusd.on_close_cb=tagCB("AmrEURUSD");
    g_amr_gbpusd.shadow_mode=false; g_amr_gbpusd.enabled=true;
    g_amr_gbpusd.on_close_cb=tagCB("AmrGBPUSD");

    g_eurusd_turtle_h4.p=omega::make_eurusd_turtle_h4_params(); g_eurusd_turtle_h4.symbol="EURUSD";
    g_eurusd_turtle_h4.shadow_mode=false; g_eurusd_turtle_h4.enabled=true;
    g_gbpusd_turtle_h4.p=omega::make_gbpusd_turtle_h4_params(); g_gbpusd_turtle_h4.symbol="GBPUSD";
    g_gbpusd_turtle_h4.shadow_mode=false; g_gbpusd_turtle_h4.enabled=true;

    g_fx_xrev_eurgbp.p.z_window=60; g_fx_xrev_eurgbp.p.z_in=2.0; g_fx_xrev_eurgbp.p.z_out=0.4;
    g_fx_xrev_eurgbp.p.hold_timeout=20; g_fx_xrev_eurgbp.p.require_hook=false;
    g_fx_xrev_eurgbp.shadow_mode=false; g_fx_xrev_eurgbp.enabled=true;
}

static inline void dispatch_nonxau(double bid, double ask, int64_t now_ms) {
    if (g_inst=="NAS100") {
        g_idx_bear_short_nas.on_tick(bid, ask, now_ms);
        g_nas_orb_retrace.on_tick(bid, ask, now_ms);
        g_ustec_tf_htf.on_tick(bid, ask, now_ms, tagCB("UstecTfHtf"));
        g_nas_turtle_d1.on_tick(bid, ask, now_ms, tagCB("NasTurtleD1"));
    } else if (g_inst=="DJ30") {
        g_idd_us30.on_tick(bid, ask, now_ms, tagCB("IddUS30"));
        g_us30_ensemble.on_tick(bid, ask, now_ms, tagCB("Us30Ensemble"));
        g_minimal_h4_us30.on_tick(bid, ask, now_ms, tagCB("MinimalH4US30"));
    } else if (g_inst=="US500") {
        g_idd_sp.on_tick(bid, ask, now_ms, tagCB("IddSP"));
        g_idx_bear_short_sp.on_tick(bid, ask, now_ms);
    } else if (g_inst=="GER40") {
        g_ger40_kelt.on_tick(bid, ask, now_ms, tagCB("Ger40Keltner"));
        g_ger40_london_brk.on_tick(bid, ask, now_ms);
        g_minimal_h4_ger40.on_tick(bid, ask, now_ms, tagCB("MinimalH4GER40"));
    } else if (g_inst=="UK100") {
        g_idd_uk100.on_tick(bid, ask, now_ms, tagCB("IddUK100"));
    } else if (g_inst=="EURUSD") {
        g_amr_eurusd.on_tick(bid, ask, now_ms);
        g_eurusd_turtle_h4.on_tick(bid, ask, now_ms, tagCB("EurusdTurtleH4"));
    } else if (g_inst=="GBPUSD") {
        g_amr_gbpusd.on_tick(bid, ask, now_ms);
        g_gbpusd_turtle_h4.on_tick(bid, ask, now_ms, tagCB("GbpusdTurtleH4"));
    } else if (g_inst=="EURGBP") {
        g_fx_xrev_eurgbp.on_tick(bid, ask, now_ms, tagCB("FxXrevEURGBP"));
    }
}

int main(int argc, char** argv) {
    const char* csv = (argc >= 2) ? argv[1] : "/Users/jo/Tick/xau_6mo_ds10.csv";
    const char* out = (argc >= 3) ? argv[2] : "/tmp/xau_book_trades.csv";
    if (argc >= 4) g_inst = argv[3];

    MemMappedFile mf;
    if (!mf.open(csv)) { fprintf(stderr, "[ERROR] cannot open %s\n", csv); return 1; }
    fprintf(stderr, "[LOAD] parsing %s ...\n", csv);
    auto t0 = std::chrono::steady_clock::now();
    std::vector<TickRow> ticks = parse_csv(mf);
    double ps = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
    if (ticks.empty()) { fprintf(stderr, "[ERROR] no ticks parsed\n"); return 1; }
    fprintf(stderr, "[LOAD] %zu ticks in %.1fs  (%s .. %s)\n",
            ticks.size(), ps,
            "", "");

    if (g_inst == "XAUUSD") {
        fprintf(stderr, "[INIT] configuring + warm-seeding 24 XAU engines ...\n");
        init_all();
    } else {
        fprintf(stderr, "[INIT] configuring non-XAU engines for %s ...\n", g_inst.c_str());
        init_nonxau();
    }

    // Warmup window: skip a fixed lead so own-bar engines (orb/panic/breakbounce
    // etc.) that warm from the LIVE stream are hot before we record. The CSV is
    // ~6 months; we drop the first ~5 days of trades. CSV-seeded engines emit
    // from tick 1 (they're pre-warm); the cutoff only filters cold-start noise.
    const int64_t t_first = ticks.front().ts_ms / 1000;
    store::warmup_sec = t_first + 5*86400;   // first 5 days = warmup buffer

    // Per-tick OHLC accumulators (match tick_gold.hpp exactly).
    AggBar c15{}, c30{}, ch1{}, ch4{}; bool o15=false,o30=false,oh1=false,oh4=false;

    const int64_t N = (int64_t)ticks.size();
    int64_t last_pct = -1;
    for (int64_t i = 0; i < N; ++i) {
        const TickRow& r = ticks[(size_t)i];
        const int64_t now_ms = r.ts_ms;
        const double bid = r.bid, ask = r.ask;
        const double mid = (bid + ask) * 0.5;
        omega::bt::set_sim_time(now_ms);

        if (g_inst != "XAUUSD") { dispatch_nonxau(bid, ask, now_ms); continue; }

        // ---- external BtBarEngine indicator builders ----
        bool h1_closed  = bt_h1.on_tick(mid, now_ms);
        bool h4_closed  = bt_h4.on_tick(mid, now_ms);
        bool m30_closed = bt_m30.on_tick(mid, now_ms);
        bool m15_closed = bt_m15.on_tick(mid, now_ms);
        const double h1_atr  = bt_h1.indicators_ready()  ? bt_h1.atr14()  : 0.0;
        const double h4_atr  = bt_h4.indicators_ready()  ? bt_h4.atr14()  : 0.0;

        // =====================================================================
        // OWN-BAR ENGINES (tick-driven; aggregate their own bars internally).
        // Match tick_gold.hpp / on_tick.hpp dispatch order.
        // =====================================================================
        g_adhull_xau.on_tick(bid, ask, now_ms);
        g_supertrend_gold.on_tick(bid, ask, now_ms);
        g_gold_orb_retrace.on_tick(bid, ask, now_ms);
        g_gold_panic_bounce.on_tick(bid, ask, now_ms);
        g_xau_breakbounce.on_tick(bid, ask, now_ms);  // emits via on_trade_record; USE_L2_PROTECT=false so set_l2 is moot
        g_xau_sess_nypm.feed_tick(bid, ask, now_ms, tagCB("XauSessNYpm"));
        g_xau_sess_overnight.feed_tick(bid, ask, now_ms, tagCB("XauSessOvernight"));
        g_gold_seasonal.on_tick(bid, ask, now_ms, tagCB("GoldSeasonal"));
        g_gold_oversold.on_tick(bid, ask, now_ms, tagCB("GoldOversold"));

        // =====================================================================
        // EXTERNAL-BAR ENGINES -- per-tick management every tick + bar dispatch.
        // =====================================================================
        // Tick management (must run every tick, exactly like tick_gold.hpp):
        g_gold_volbrk_m30.on_tick(bid, ask, now_ms, tagCB("GoldVolBreakoutM30"));
        g_xau_straddle_m30.on_tick(bid, ask, now_ms, tagCB("XauStraddleM30"));
        g_xau_straddle_m15.on_tick(bid, ask, now_ms, tagCB("XauStraddleM15"));
        g_xau_threebar_30m.on_tick(bid, ask, now_ms, tagCB("XauThreeBar30m"));
        g_xau_tf_1h.on_tick(bid, ask, now_ms, tagCB("XauTrendFollow1h"));
        g_xau_tf_m15.on_tick(bid, ask, now_ms, tagCB("XauTrendFollowM15"));
        g_xau_tf_2h.on_tick(bid, ask, now_ms, tagCB("XauTrendFollow2h"));
        g_xau_tf_4h.on_tick(bid, ask, now_ms, tagCB("XauTrendFollow4h"));
        g_xau_tf_d1.on_tick(bid, ask, now_ms, tagCB("XauTrendFollowD1"));
        g_xau_turtle_d1.on_tick(bid, ask, now_ms, tagCB("XauTurtleD1"));
        g_xau_doji_rej_d1.on_tick(bid, ask, now_ms, tagCB("XauDojiRejD1"));
        g_xau_outside_bar_d1.on_tick(bid, ask, now_ms, tagCB("XauOutsideBarD1"));
        g_donchian.on_tick(bid, ask, now_ms, tagCB("Donchian"));
        g_ema_pullback.on_tick(bid, ask, now_ms, tagCB("EmaPullback"));
        g_tsmom_v2.on_tick(bid, ask, now_ms, tagCB("TsmomV2"));

        // ---- M15 accumulate + close ----
        const int64_t b15 = (now_ms / 900000LL) * 900000LL;
        if (!o15) { c15 = {b15/60000LL, mid, mid, mid, mid}; o15=true; }
        else if (b15 != c15.start_min*60000LL) {
            // close prior M15 -> XauStraddleM15 + XauTrendFollow M15 (Donchian40)
            g_xau_straddle_m15.on_m30_bar(c15.high, c15.low, c15.close, bid, ask, now_ms, tagCB("XauStraddleM15"));
            {
                omega::XauTfBar1h bar15{};
                bar15.bar_start_ms = c15.start_min*60000LL;
                bar15.open=c15.open; bar15.high=c15.high; bar15.low=c15.low; bar15.close=c15.close;
                const double m15_atr = bt_m15.indicators_ready() ? bt_m15.atr14() : 0.0;
                g_xau_tf_m15.on_h1_bar(bar15, bid, ask, /*atr14_external=*/0.0, now_ms, tagCB("XauTrendFollowM15"));
                (void)m15_atr;  // live passes 0.0 -> engine uses own internal ATR
            }
            c15 = {b15/60000LL, mid, mid, mid, mid};
        } else { if(mid>c15.high)c15.high=mid; if(mid<c15.low)c15.low=mid; c15.close=mid; }

        // ---- M30 accumulate + close ----
        const int64_t b30 = (now_ms / 1800000LL) * 1800000LL;
        if (!o30) { c30 = {b30/60000LL, mid, mid, mid, mid}; o30=true; }
        else if (b30 != c30.start_min*60000LL) {
            const int64_t bar30_ms = c30.start_min*60000LL;
            // XauThreeBar30m (on_30m_bar; live passes atr14_external=0.0)
            {
                omega::XauThreeBar30mBar bar30m{};
                bar30m.bar_start_ms = bar30_ms;
                bar30m.open=c30.open; bar30m.high=c30.high; bar30m.low=c30.low; bar30m.close=c30.close;
                g_xau_threebar_30m.on_30m_bar(bar30m, bid, ask, /*atr14_external=*/0.0, now_ms, tagCB("XauThreeBar30m"));
            }
            // XauStraddleM30
            g_xau_straddle_m30.on_m30_bar(c30.high, c30.low, c30.close, bid, ask, now_ms, tagCB("XauStraddleM30"));
            // GoldVolBreakoutM30
            g_gold_volbrk_m30.on_m30_bar(c30.high, c30.low, c30.close, bid, ask, now_ms, tagCB("GoldVolBreakoutM30"));
            c30 = {b30/60000LL, mid, mid, mid, mid};
        } else { if(mid>c30.high)c30.high=mid; if(mid<c30.low)c30.low=mid; c30.close=mid; }

        // ---- H1 accumulate + close ----
        const int64_t bh1 = (now_ms / 3600000LL) * 3600000LL;
        if (!oh1) { ch1 = {bh1/60000LL, mid, mid, mid, mid}; oh1=true; }
        else if (bh1 != ch1.start_min*60000LL) {
            const int64_t bar_h1_ms = ch1.start_min*60000LL;
            // GoldVolBreakoutM30 H1 trend gate (must run on H1 close before M30 entry)
            g_gold_volbrk_m30.on_h1_close(ch1.close);
            // XauTrendFollow1h
            {
                omega::XauTfBar1h tf{}; tf.bar_start_ms=bar_h1_ms;
                tf.open=ch1.open; tf.high=ch1.high; tf.low=ch1.low; tf.close=ch1.close;
                g_xau_tf_1h.on_h1_bar(tf, bid, ask, h1_atr, now_ms, tagCB("XauTrendFollow1h"));
            }
            // TsmomPortfolioV2
            {
                omega::cell::Bar b{}; b.bar_start_ms=bar_h1_ms;
                b.open=ch1.open; b.high=ch1.high; b.low=ch1.low; b.close=ch1.close;
                g_tsmom_v2.on_h1_bar(b, bid, ask, h1_atr, now_ms, tagCB("TsmomV2"));
            }
            // DonchianPortfolio
            {
                omega::DonchianBar b{}; b.bar_start_ms=bar_h1_ms;
                b.open=ch1.open; b.high=ch1.high; b.low=ch1.low; b.close=ch1.close;
                g_donchian.on_h1_bar(b, bid, ask, h1_atr, now_ms, tagCB("Donchian"));
            }
            // XauTrendFollow2h
            {
                omega::XauTf2hBar tf{}; tf.bar_start_ms=bar_h1_ms;
                tf.open=ch1.open; tf.high=ch1.high; tf.low=ch1.low; tf.close=ch1.close;
                g_xau_tf_2h.on_h1_bar(tf, bid, ask, now_ms, tagCB("XauTrendFollow2h"));
            }
            // EmaPullbackPortfolio
            {
                omega::EpbBar b{}; b.bar_start_ms=bar_h1_ms;
                b.open=ch1.open; b.high=ch1.high; b.low=ch1.low; b.close=ch1.close;
                g_ema_pullback.on_h1_bar(b, bid, ask, h1_atr, now_ms, tagCB("EmaPullback"));
            }
            ch1 = {bh1/60000LL, mid, mid, mid, mid};
        } else { if(mid>ch1.high)ch1.high=mid; if(mid<ch1.low)ch1.low=mid; ch1.close=mid; }

        // ---- H4 accumulate + close ----
        const int64_t bh4 = (now_ms / 14400000LL) * 14400000LL;
        if (!oh4) { ch4 = {bh4/60000LL, mid, mid, mid, mid}; oh4=true; }
        else if (bh4 != ch4.start_min*60000LL) {
            const int64_t bar_h4_ms = ch4.start_min*60000LL;
            // XauTrendFollow4h (needs external H4 atr14)
            {
                omega::XauTfBar tf{}; tf.bar_start_ms=bar_h4_ms;
                tf.open=ch4.open; tf.high=ch4.high; tf.low=ch4.low; tf.close=ch4.close;
                g_xau_tf_4h.on_h4_bar(tf, bid, ask, h4_atr, now_ms, tagCB("XauTrendFollow4h"));
            }
            // XauTrendFollowD1 (synthesises D1 internally)
            {
                omega::XauTfD1Bar d{}; d.bar_start_ms=bar_h4_ms;
                d.open=ch4.open; d.high=ch4.high; d.low=ch4.low; d.close=ch4.close;
                g_xau_tf_d1.on_h4_bar(d, bid, ask, now_ms, tagCB("XauTrendFollowD1"));
            }
            // XauTurtleD1 / XauDojiRejD1 / XauOutsideBarD1 (h,l,c form)
            g_xau_turtle_d1.on_h4_bar(ch4.high, ch4.low, ch4.close, bid, ask, now_ms, tagCB("XauTurtleD1"));
            g_xau_doji_rej_d1.on_h4_bar(ch4.high, ch4.low, ch4.close, bid, ask, now_ms, tagCB("XauDojiRejD1"));
            g_xau_outside_bar_d1.on_h4_bar(ch4.high, ch4.low, ch4.close, bid, ask, now_ms, tagCB("XauOutsideBarD1"));
            ch4 = {bh4/60000LL, mid, mid, mid, mid};
        } else { if(mid>ch4.high)ch4.high=mid; if(mid<ch4.low)ch4.low=mid; ch4.close=mid; }

        // progress
        const int64_t pct = (i*100)/N;
        if (pct != last_pct && pct % 10 == 0) {
            fprintf(stderr, "\r[RUN] %lld%%  trades=%zu ", (long long)pct, store::recs.size());
            fflush(stderr);
            last_pct = pct;
        }
    }
    fprintf(stderr, "\r[RUN] 100%%  trades=%zu          \n", store::recs.size());

    // ---- Write trades CSV ----
    FILE* f = fopen(out, "w");
    if (!f) { fprintf(stderr, "[ERROR] cannot write %s\n", out); return 1; }
    fprintf(f, "entryTs,symbol,side,engine,entryPrice,exitPrice,"
               "pnl,mfe,mae,hold_sec,exitReason,spreadAtEntry,atrAtEntry,latencyMs,regime\n");
    for (const auto& t : store::recs)
        fprintf(f, "%lld,%s,%s,%s,%.5f,%.5f,%.4f,%.4f,%.4f,%lld,%s,%.4f,%.4f,%.2f,%s\n",
                (long long)t.entryTs, t.symbol.c_str(), t.side.c_str(), t.engine.c_str(),
                t.entryPrice, t.exitPrice, t.pnl, t.mfe, t.mae,
                (long long)(t.exitTs - t.entryTs), t.exitReason.c_str(),
                t.spreadAtEntry, t.atr_at_entry, t.latencyMs, t.regime.c_str());
    fclose(f);

    // ---- Per-engine summary table ----
    std::vector<const Stat*> v;
    for (auto& kv : store::stats) v.push_back(&kv.second);
    std::sort(v.begin(), v.end(), [](const Stat*a, const Stat*b){ return a->pnl > b->pnl; });
    printf("\n==================== PER-ENGINE (gross, pre-cost) ====================\n");
    printf("  %-26s %8s  %8s  %12s\n", "engine", "trades", "wins", "gross_pnl");
    printf("  ------------------------------------------------------------------\n");
    int64_t tot_t = 0;
    for (const auto* s : v) {
        printf("  %-26s %8lld  %8lld  %+12.2f\n",
               s->name.c_str(), (long long)s->n, (long long)s->wins, s->pnl);
        tot_t += s->n;
    }
    printf("  ------------------------------------------------------------------\n");
    printf("  %-26s %8lld\n", "TOTAL", (long long)tot_t);
    printf("\n[OUTPUT] %zu trades -> %s\n", store::recs.size(), out);
    return 0;
}

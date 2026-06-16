// squeeze_xregime_nas.cpp — cross-regime backtest of the REAL SqueezeSlingshotEngine
// production path (on_tick → internal bar aggregation → lookahead-free core) over
// multi-year NAS100 intraday tick (2022 bear + 2023-26 bull).
//
// One data pass feeds every tick to N trait variants at once (type-erased Runner).
// Trades are collected via the engine's own on_close_cb (the production hook). Cost is
// applied here in reporting (the engine emits RAW points; the ledger/harness owns cost).
// Reports per-year + BEAR'22/BULL + chronological walk-forward H1/H2, at 1x/2x/3x cost.
//
// Data format (duka/histdata): "YYYYMMDD HHMMSSmmm,bid,ask[,vol]"  (BID first).
//
// build: clang++ -O2 -std=c++17 -Iinclude -o /tmp/sqzx backtest/squeeze_xregime_nas.cpp
// run:   /tmp/sqzx /Users/jo/Tick/xregime/NAS100_full_ds10.csv [cost_rt_pts=2.0]
#include "SqueezeSlingshotEngine.hpp"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>

using omega::SqueezeSlingshotEngine;
using omega::SqzTraits_NAS100;
using omega::TradeRecord;

// ---- harness-local sweep traits (derive from the production NAS100 traits) ----
namespace sw {
using B = omega::SqzBaseParams;
struct H1_prod      : SqzTraits_NAS100 {};                                            // tier1, tgt4, both sides
struct H1_tier2_t6  : SqzTraits_NAS100 { static constexpr int MIN_TIER=2; static constexpr double ATR_TARGET_MULT=6.0; };
struct H1_strict_C  : SqzTraits_NAS100 { static constexpr int MIN_TIER=2; static constexpr bool REQUIRE_MOMO_BELOW_ZERO=true; static constexpr double ATR_TARGET_MULT=0.0; };
struct H1_longonly  : SqzTraits_NAS100 { static constexpr bool LONG_ONLY=true; };
struct H1_rollover  : SqzTraits_NAS100 { static constexpr double ATR_TARGET_MULT=0.0; };   // no fixed TP: ride the slingshot
struct H1_tier3     : SqzTraits_NAS100 { static constexpr int MIN_TIER=3; static constexpr double ATR_TARGET_MULT=0.0; };
struct H1_wide      : SqzTraits_NAS100 { static constexpr double ATR_STOP_MULT=2.5; static constexpr double ATR_TARGET_MULT=5.0; };
struct M30_prod     : SqzTraits_NAS100 { static constexpr std::int64_t BAR_INTERVAL_MS=1800000LL; };
// higher-timeframe — squeeze classically lives on H4/D1
struct H4_prod      : SqzTraits_NAS100 { static constexpr std::int64_t BAR_INTERVAL_MS=14400000LL; };
struct H4_rollover  : SqzTraits_NAS100 { static constexpr std::int64_t BAR_INTERVAL_MS=14400000LL; static constexpr double ATR_TARGET_MULT=0.0; };
struct H4_longonly  : SqzTraits_NAS100 { static constexpr std::int64_t BAR_INTERVAL_MS=14400000LL; static constexpr bool LONG_ONLY=true; };
struct D1_prod      : SqzTraits_NAS100 { static constexpr std::int64_t BAR_INTERVAL_MS=86400000LL; };
struct D1_rollover  : SqzTraits_NAS100 { static constexpr std::int64_t BAR_INTERVAL_MS=86400000LL; static constexpr double ATR_TARGET_MULT=0.0; };
}

// ---- type-erased runner so one tick loop can drive many template instances ----
struct Runner {
    std::string name;
    std::vector<TradeRecord> trades;
    virtual void on_tick(double bid, double ask, std::int64_t ts_ms) = 0;
    virtual ~Runner() = default;
};
template<class T>
struct Run : Runner {
    SqueezeSlingshotEngine<T> eng;
    explicit Run(const char* nm) {
        name = nm;
        eng.shadow_mode = true;
        eng.on_close_cb = [this](const TradeRecord& tr){ trades.push_back(tr); };
    }
    void on_tick(double bid, double ask, std::int64_t ts_ms) override { eng.on_tick(bid, ask, ts_ms); }
};

static int year_of(std::int64_t ts_sec) {
    time_t tt = (time_t)ts_sec; struct tm* g = gmtime(&tt); return g ? g->tm_year + 1900 : 0;
}

struct Stat { int n=0, w=0; double gw=0, gl=0, net=0; };
static void acc(Stat& s, double net_pts) {
    s.n++; s.net += net_pts;
    if (net_pts > 0) { s.w++; s.gw += net_pts; } else { s.gl -= net_pts; }
}
static void line(const char* tag, const Stat& s) {
    if (s.n == 0) { printf("  %-9s n=0\n", tag); return; }
    double pf = s.gl > 1e-9 ? s.gw / s.gl : 99.0;
    printf("  %-9s n=%4d WR=%3.0f%% PF=%5.2f net=%+10.1f avg=%+7.2f\n",
           tag, s.n, 100.0 * s.w / s.n, pf, s.net, s.net / s.n);
}

static void report(const Runner& r, double cost_rt, double mult) {
    const double cost = cost_rt * mult;
    // chronological split: sort by entryTs, halve
    std::vector<const TradeRecord*> ord;
    for (auto& t : r.trades) ord.push_back(&t);
    std::sort(ord.begin(), ord.end(), [](auto a, auto b){ return a->entryTs < b->entryTs; });
    const size_t half = ord.size() / 2;

    Stat all, bear, bull, h1, h2;
    std::vector<std::pair<int,Stat>> years; // ordered
    auto yslot = [&](int y)->Stat& {
        for (auto& kv : years) if (kv.first == y) return kv.second;
        years.push_back({y, Stat{}}); return years.back().second;
    };
    for (size_t i = 0; i < ord.size(); ++i) {
        const TradeRecord& t = *ord[i];
        double raw = (t.side == "LONG") ? (t.exitPrice - t.entryPrice) : (t.entryPrice - t.exitPrice);
        double net = raw - cost;
        int y = year_of(t.entryTs);
        acc(all, net); acc(yslot(y), net);
        if (y == 2022 || y == 2020) acc(bear, net); else acc(bull, net);
        if (i < half) acc(h1, net); else acc(h2, net);
    }
    std::sort(years.begin(), years.end(), [](auto&a, auto&b){ return a.first < b.first; });
    printf("\n== %-12s  cost=%.1f pts/rt (%.0fx) ==\n", r.name.c_str(), cost, mult);
    for (auto& kv : years) { char tg[8]; snprintf(tg, 8, "%d", kv.first); line(tg, kv.second); }
    printf("  --\n");
    line("BEAR'22", bear); line("BULL", bull);
    line("WF-H1", h1); line("WF-H2", h2);
    line("ALL", all);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <tickfile> [cost_rt_pts=2.0]\n", argv[0]); return 1; }
    const double COST = argc > 2 ? atof(argv[2]) : 2.0;
    std::ifstream f(argv[1]);
    if (!f) { fprintf(stderr, "open fail: %s\n", argv[1]); return 1; }

    std::vector<std::unique_ptr<Runner>> runners;
    runners.emplace_back(new Run<sw::H1_prod>("H1_prod"));
    runners.emplace_back(new Run<sw::H1_tier2_t6>("H1_tier2_t6"));
    runners.emplace_back(new Run<sw::H1_strict_C>("H1_strictC"));
    runners.emplace_back(new Run<sw::H1_longonly>("H1_longonly"));
    runners.emplace_back(new Run<sw::H1_rollover>("H1_rollover"));
    runners.emplace_back(new Run<sw::H1_tier3>("H1_tier3"));
    runners.emplace_back(new Run<sw::H1_wide>("H1_wide"));
    runners.emplace_back(new Run<sw::M30_prod>("M30_prod"));
    runners.emplace_back(new Run<sw::H4_prod>("H4_prod"));
    runners.emplace_back(new Run<sw::H4_rollover>("H4_rollover"));
    runners.emplace_back(new Run<sw::H4_longonly>("H4_longonly"));
    runners.emplace_back(new Run<sw::D1_prod>("D1_prod"));
    runners.emplace_back(new Run<sw::D1_rollover>("D1_rollover"));

    std::string ln; std::int64_t n = 0;
    while (std::getline(f, ln)) {
        if (ln.size() < 16 || ln[8] != ' ') continue;  // expect "YYYYMMDD HHMMSS..."
        const char* s = ln.c_str();
        int Y=(s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+(s[3]-'0');
        int Mo=(s[4]-'0')*10+(s[5]-'0'), D=(s[6]-'0')*10+(s[7]-'0');
        if (Y < 1990 || Mo < 1 || Mo > 12 || D < 1 || D > 31) continue;  // header/garbage
        int hh=(s[9]-'0')*10+(s[10]-'0'), mm=(s[11]-'0')*10+(s[12]-'0'), ss=(s[13]-'0')*10+(s[14]-'0');
        int y=Y-(Mo<=2); int era=(y>=0?y:y-399)/400; unsigned yoe=(unsigned)(y-era*400);
        unsigned doy=(153*(Mo+(Mo>2?-3:9))+2)/5+D-1; unsigned doe=yoe*365+yoe/4-yoe/100+doy;
        long days=(long)era*146097+(long)doe-719468;
        std::int64_t ts_ms=((std::int64_t)days*86400L+hh*3600L+mm*60L+ss)*1000L;
        size_t c1=ln.find(',',15); if (c1==std::string::npos) continue;
        size_t c2=ln.find(',',c1+1);
        double bid=strtod(s+c1+1,nullptr);
        double ask=(c2==std::string::npos)?bid:strtod(s+c2+1,nullptr);
        if (bid<=0||ask<=0) continue;
        for (auto& r : runners) r->on_tick(bid, ask, ts_ms);
        ++n;
    }
    fprintf(stderr, "# %lld ticks processed\n", (long long)n);

    for (auto& r : runners) {
        printf("\n################ %s  (%zu trades) ################\n", r->name.c_str(), r->trades.size());
        report(*r, COST, 1.0);
        report(*r, COST, 2.0);
        report(*r, COST, 3.0);
    }
    return 0;
}

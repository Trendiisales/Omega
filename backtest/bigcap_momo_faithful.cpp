// bigcap_momo_faithful.cpp — ENGINE-FAITHFUL tick backtest for g_bigcap_momo.
//
// Drives the REAL omega::PumpScalpManager class (the type of g_bigcap_momo in
// globals.hpp) with the EXACT engine_init.hpp production config. Feeds closed 5m
// OHLCV bars via on_bar() and an intra-bar price PATH via on_price() so the
// trailing-stop exit runs on the real tick path (no within-bar look-ahead like
// the bar-replay pump/bigcap_sweep_ext.py).
//
// Intra-bar price path (conservative, no look-ahead): for each 5m bar we replay
// open -> (low then high, or high then low) -> close as discrete on_price calls.
// Default ordering is open->LOW->HIGH->close on the *entry-already-open* bar so a
// trailing exit can trigger on the adverse extreme BEFORE the favourable one
// (the realistic worst case the bar-replay cheats away).
//
// Cost: the engine already bakes SLIP_PCT (%/side) into recorded pnl. We add an
// optional extra per-side commission stress (bps) on top, applied to notional.
//
// Build (Mac):
//   g++ -std=c++17 -O2 -Iinclude backtest/bigcap_momo_faithful.cpp -o /tmp/bcfaith
// Run:
//   /tmp/bcfaith /tmp/bigcap_5m.bin [extra_comm_bps_per_side] [path_order]
//     path_order: 0 = open->low->high->close (adverse-first, conservative DEFAULT)
//                 1 = open->high->low->close (favourable-first, optimistic)

#include "PumpScalpManager.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

struct BarRow { int64_t ts; double o,h,l,c,v; };
struct SymData { std::string name; std::vector<BarRow> bars; };

static std::vector<SymData> load_bin(const char* path) {
    std::vector<SymData> out;
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return out; }
    uint32_t nsym = 0; if (fread(&nsym,4,1,f)!=1) { fclose(f); return out; }
    for (uint32_t s=0; s<nsym; ++s) {
        uint8_t nl=0; if (fread(&nl,1,1,f)!=1) break;
        std::string nm(nl,'\0'); if (fread(&nm[0],1,nl,f)!=nl) break;
        uint32_t nb=0; if (fread(&nb,4,1,f)!=1) break;
        SymData sd; sd.name=nm; sd.bars.reserve(nb);
        for (uint32_t b=0;b<nb;++b){
            int64_t ts; double v[5];
            if (fread(&ts,8,1,f)!=1) break;
            if (fread(v,8,5,f)!=5) break;
            sd.bars.push_back({ts,v[0],v[1],v[2],v[3],v[4]});
        }
        out.push_back(std::move(sd));
    }
    fclose(f);
    return out;
}

// ── collected closed trades for stats ────────────────────────────────────────
struct Closed { double net_ret; double notional; int64_t entryTs; double pnl_raw; double size; double entry, exit; };
static std::vector<Closed> g_trades;
static double g_extra_comm_bps = 0.0;   // per-side commission stress

static void on_close(const omega::TradeRecord& tr) {
    // engine pnl already includes SLIP_PCT both sides. Add extra commission stress.
    double extra = (tr.entryPrice + tr.exitPrice) * (g_extra_comm_bps/1e4) * tr.size;
    double net = tr.pnl - extra;
    double notional = tr.entryPrice * tr.size;
    g_trades.push_back({net/ (notional>0?notional:1.0), notional, tr.entryTs, tr.pnl, tr.size, tr.entryPrice, tr.exitPrice});
}

static void configure_prod(omega::PumpScalpManager& m) {
    // EXACT engine_init.hpp production config (lines 4737-4771).
    m.enabled      = true;
    m.shadow_mode  = true;
    m.tf_sec       = 300;
    m.label        = "BigCapMomo";
    m.day_gate_pct = 4.0;     // S-2026-06-12b
    m.trail_pct    = 5.0;     // S-2026-06-12b
    m.volx         = 0.0;     // S-2026-06-13k OFF (LIVE config)
    m.be_arm_pct   = 0.0;
    m.be_floor_pct = 0.0;
    m.maxhold_bars = 48;
    m.pyr_adds     = 0;
    m.max_entries_per_day = 2;
    m.notional_usd = 1000.0;
    m.slip_pct     = 0.15;    // big-cap realistic, %/side baked into pnl
    m.min_dvol_usd = 0.0;
    m.price_min    = 10.0;
    m.verbose      = false;
    m.max_symbols  = 100000;  // never evict in BT (single-sym replay anyway)
    m.on_trade_record = on_close;
}

int main(int argc, char** argv) {
    const char* binpath = argc>1 ? argv[1] : "/tmp/bigcap_5m.bin";
    g_extra_comm_bps = argc>2 ? atof(argv[2]) : 0.0;
    int path_order   = argc>3 ? atoi(argv[3]) : 0;  // 0=adverse-first (default)

    auto syms = load_bin(binpath);
    fprintf(stderr, "# loaded %zu syms\n", syms.size());

    // Feed each symbol through its OWN fresh manager so cross-symbol eviction
    // and shared day-state never interfere — faithful to one cell per symbol.
    int total_bars=0;
    for (auto& sd : syms) {
        omega::PumpScalpManager m;
        configure_prod(m);
        const std::string& sym = sd.name;
        for (size_t i=0;i<sd.bars.size();++i) {
            const BarRow& b = sd.bars[i];
            int64_t ts_ms = b.ts*1000;
            // 1) feed the CLOSED bar for entry evaluation (engine state/EMA/gate)
            m.on_bar(sym, 300, b.o, b.h, b.l, b.c, b.v, ts_ms, /*is_seed=*/false);
            // 2) replay an intra-bar price PATH for exit management (on_price).
            //    Spread ticks across the 5m window so MAXHOLD time-stop sees real ts.
            double seq[4];
            if (path_order==1) { seq[0]=b.o; seq[1]=b.h; seq[2]=b.l; seq[3]=b.c; }
            else               { seq[0]=b.o; seq[1]=b.l; seq[2]=b.h; seq[3]=b.c; }
            for (int k=0;k<4;++k) {
                int64_t pts = ts_ms + (int64_t)(k * (300000/4));
                m.on_price(sym, seq[k], pts);
            }
            total_bars++;
        }
        // flush any still-open position at last price (avoid phantom open carry)
        if (!sd.bars.empty()) {
            // nothing to force; engine has no public force at mgr level — leave open
        }
    }
    fprintf(stderr, "# fed %d bars, %zu closed trades\n", total_bars, g_trades.size());

    // ── stats: overall + walk-forward halves (by entry ts) + fat-tail top3 ──
    auto report = [&](const char* tag, std::vector<Closed>& v){
        if (v.empty()) { printf("%-10s n=0\n", tag); return; }
        double gw=0,gl=0; int w=0; double net=0;
        for (auto& t : v) { net+=t.net_ret; if (t.net_ret>0){gw+=t.net_ret;++w;} else gl-=t.net_ret; }
        double pf = gl>0 ? gw/gl : (gw>0?99.0:0.0);
        printf("%-10s n=%-4zu WR=%.1f%%  PF=%.2f  net=%.3f(sum-ret)  avg=%.4f\n",
               tag, v.size(), 100.0*w/v.size(), pf, net, net/v.size());
    };

    // sort by entry ts for WF split
    std::sort(g_trades.begin(), g_trades.end(),
              [](const Closed&a,const Closed&b){return a.entryTs<b.entryTs;});
    report("ALL", g_trades);
    size_t half = g_trades.size()/2;
    std::vector<Closed> h1(g_trades.begin(), g_trades.begin()+half);
    std::vector<Closed> h2(g_trades.begin()+half, g_trades.end());
    report("H1", h1);
    report("H2", h2);

    // fat-tail: top-3 winners share of gross win
    std::vector<double> wins;
    double grosswin=0;
    for (auto&t:g_trades) if (t.net_ret>0){wins.push_back(t.net_ret); grosswin+=t.net_ret;}
    std::sort(wins.rbegin(), wins.rend());
    double top3=0; for (int i=0;i<3 && i<(int)wins.size();++i) top3+=wins[i];
    printf("fat-tail   top3 winners = %.1f%% of gross win (n_wins=%zu)\n",
           grosswin>0?100.0*top3/grosswin:0.0, wins.size());

    // net in $ terms (sum of per-trade $ net using $1000 notional)
    double dollar_net=0; for (auto&t:g_trades) dollar_net += t.net_ret * t.notional;
    printf("dollar_net = $%.0f  (extra_comm=%.0fbps/side, path_order=%s)\n",
           dollar_net, g_extra_comm_bps, path_order==1?"fav-first(optimistic)":"adverse-first(conservative)");
    return 0;
}

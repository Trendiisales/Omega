// =============================================================================
// xau_tf_m15_revalidate.cpp -- ENGINE-FAITHFUL tick backtest for g_xau_tf_m15.
//
// Drives the REAL omega::XauTrendFollow1hEngine class with the PRODUCTION M15
// config from include/engine_init.hpp:1540+ :
//     cell_enable_mask = 0x02 (Donchian40 cell ONLY)
//     lot = 0.01, max_spread = 1.0, use_vol_target = false, pyramid_max_adds = 0
//     LOSS_CUT_PCT = 0.4
// Fed M15 bars (the engine treats each on_h1_bar() call as a closed bar; in
// production tick_gold.hpp feeds it M15 bars), exactly mirroring deployment.
//
// Faithfulness extras vs the bar-replay sweep that "validated" it
// (gold_cost_unlock_sweep.cpp):
//   - Real WaveTrend momentum gate fed (gold_wt(), production default ON).
//   - Real gold_regime() bear-block fed via ticks (long-only skip in bear).
//   - LOSS_CUT_PCT=0.4 active (bar-replay sweep had no per-tick loss-cut).
//   - Cost-honest: entry at ask, exits at bid; spread stress 0.30/0.60/1.00.
//   - Intrabar tick path o->(l,h dir-ordered)->c so SL/TP/LOSS_CUT fire faithfully.
//
// Build:
//   clang++ -O3 -std=c++17 -I include backtest/xau_tf_m15_revalidate.cpp \
//           -o backtest/xau_tf_m15_revalidate
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

#include "XauTrendFollow1hEngine.hpp"
#include "GoldWaveTrend.hpp"
#include "RegimeState.hpp"
#include "PortfolioGuard.hpp"

struct Bar { int64_t ts_sec; double o, h, l, c; };

static std::vector<Bar> load(const std::string& path) {
    std::vector<Bar> bars; std::ifstream f(path);
    std::string line; std::getline(f, line);
    while (std::getline(f, line)) {
        Bar b; const char* p = line.c_str(); char* e;
        b.ts_sec = std::strtoll(p, &e, 10); if (*e != ',') continue; p = e + 1;
        b.o = std::strtod(p, &e); if (*e != ',') continue; p = e + 1;
        b.h = std::strtod(p, &e); if (*e != ',') continue; p = e + 1;
        b.l = std::strtod(p, &e); if (*e != ',') continue; p = e + 1;
        b.c = std::strtod(p, &e);
        bars.push_back(b);
    }
    return bars;
}

struct Stats {
    int64_t n=0; double net=0, gp=0, gl=0; int wins=0;
    std::vector<double> wins_v;
};
static double pf(const Stats& s){ return s.gl>0 ? s.gp/s.gl : (s.gp>0?999.0:0.0); }

// Drive the real engine over [i0,i1). half = half-spread (per side).
// wt_feed/regime_feed toggle the production gates.
static Stats drive(const std::vector<Bar>& bars, size_t i0, size_t i1, double half,
                   double comm_per_lot, bool gates,
                   std::vector<double>* pnls_out, std::vector<int64_t>* ts_out) {
    // Fresh gate state each run (singletons can't be reassigned -- reset fields).
    {
        auto& w = omega::gold_wt();
        w.esa_=w.dd_=w.wt1_=0.0; w.ema_fast_=w.ema_slow_=0.0;
        w.wt1_hist_={{0,0,0,0}}; w.wt1_n_=0; w.prev_wt1_=w.prev_wt2_=0.0;
        w.init_=false; w.bars_=0; w.mom_up_={}; w.mom_dn_={}; w.ring_head_=0;
        w.gate_enabled = gates;   // production: ON
        auto& g = omega::gold_regime();
        g.emaS_hist_.clear(); g.emaS_=g.emaF_=0.0; g.have_ema_=false; g.bars_=0;
        g.last_close_=0.0; g.bear_=g.bull_=false; g.acc_bar_=-1; g.a_c_=0.0; g.a_n_=0; g.last_mid_=0.0;
    }

    omega::XauTrendFollow1hEngine eng;
    eng.shadow_mode      = true;
    eng.enabled          = true;
    eng.LOSS_CUT_PCT     = 0.4;
    eng.cell_enable_mask = 0x02;     // Donchian40 only
    eng.lot              = 0.01;
    eng.max_spread       = 1.0;
    eng.use_vol_target   = false;
    eng.pyramid_max_adds = 0;
    eng.init();

    Stats st;
    auto cb = [&](const omega::TradeRecord& tr) {
        // commission: comm_per_lot charged per round trip, scaled by size.
        double pnl = tr.pnl - comm_per_lot * tr.size;
        st.net += pnl; st.n++;
        if (pnl > 0) { st.gp += pnl; st.wins++; st.wins_v.push_back(pnl); }
        else st.gl += -pnl;
        if (pnls_out) { pnls_out->push_back(pnl); ts_out->push_back(tr.entryTs); }
    };

    for (size_t i = i0; i < i1 && i < bars.size(); ++i) {
        const auto& b = bars[i];
        const int64_t ts_ms = b.ts_sec * 1000LL;
        // Feed production gates.
        if (gates) {
            omega::gold_wt().on_m1_close(b.h, b.l, b.c);     // M15-proxy (native cadence)
            omega::gold_regime().on_tick(b.c - half, b.c + half, ts_ms);
        }
        // Bar close -> engine decision (entry at ask = close+half inside _fire_entry).
        omega::XauTfBar1h bar; bar.bar_start_ms = ts_ms;
        bar.open=b.o; bar.high=b.h; bar.low=b.l; bar.close=b.c;
        eng.on_h1_bar(bar, b.c - half, b.c + half, 0.0, ts_ms, cb);

        // Intrabar tick path over the NEXT bar for position management.
        if (i + 1 < bars.size()) {
            const auto& nb = bars[i + 1];
            const int64_t nts = nb.ts_sec * 1000LL;
            // path: open, then (low,high) ordered by bar direction, then close
            double seq[4];
            if (nb.c >= nb.o) { seq[0]=nb.o; seq[1]=nb.l; seq[2]=nb.h; seq[3]=nb.c; }
            else              { seq[0]=nb.o; seq[1]=nb.h; seq[2]=nb.l; seq[3]=nb.c; }
            for (double px : seq)
                eng.on_tick(px - half, px + half, nts, cb);
        }
    }
    return st;
}

static void print_stats(const char* lbl, const Stats& s) {
    double avg = s.n? s.net/s.n : 0;
    double wr  = s.n? 100.0*s.wins/s.n : 0;
    std::printf("  %-26s n=%-5lld WR=%4.1f%% PF=%5.2f net=%+10.2f avg=%+7.3f\n",
                lbl,(long long)s.n,wr,pf(s),s.net,avg);
}

int main(int argc, char** argv) {
    const char* path = argc>1?argv[1]:"/tmp/xau_m15.csv";
    auto bars = load(path);
    std::fprintf(stderr,"loaded %zu M15 bars from %s\n", bars.size(), path);
    if (bars.empty()) return 1;
    omega::pg::g_pg_cfg.max_concurrent_positions = 0;

    const size_t split = (bars.size()*70)/100;
    const size_t half_n = bars.size()/2;

    std::printf("\n================ XAU_TF_M15 FAITHFUL REVALIDATE ================\n");
    std::printf("Production config: Donchian40 mask=0x02, lot=0.01, LOSS_CUT=0.4, vol-target OFF, pyramid OFF\n");
    std::printf("Cost: entry@ask exit@bid, half-spread + commission stress. Gates: WT + bear-block ON.\n\n");

    // --- IBKR-cost baseline: half=0.185 (~0.37 RT pts spread), comm ~0.10/lot equiv ---
    // engine_init notes IBKR cost 0.37 RT pts. half=0.185 each side.
    struct Cfg { const char* name; double half; double comm; bool gates; };
    Cfg cfgs[] = {
        {"IBKR base (gate ON)",   0.185, 0.0,  true},
        {"IBKR base (gate OFF)",  0.185, 0.0,  false},
        {"spread 0.60 (half.30)", 0.30,  0.0,  true},
        {"spread 1.00 (half.50)", 0.50,  0.0,  true},
        {"IBKR + comm stress",    0.185, 0.05, true},
    };
    std::printf("--- FULL SAMPLE (cost / gate sweep) ---\n");
    for (auto&c:cfgs){ auto s=drive(bars,0,bars.size(),c.half,c.comm,c.gates,nullptr,nullptr); print_stats(c.name,s); }

    // --- Walk-forward halves at IBKR base, gate ON (production) ---
    std::printf("\n--- WALK-FORWARD (IBKR base, gate ON = production) ---\n");
    auto s_is  = drive(bars,0,split,0.185,0.0,true,nullptr,nullptr);
    auto s_oos = drive(bars,split,bars.size(),0.185,0.0,true,nullptr,nullptr);
    print_stats("IS  (first 70%)",s_is);
    print_stats("OOS (last 30%)", s_oos);
    auto s_h1 = drive(bars,0,half_n,0.185,0.0,true,nullptr,nullptr);
    auto s_h2 = drive(bars,half_n,bars.size(),0.185,0.0,true,nullptr,nullptr);
    print_stats("H1  (first 50%)",s_h1);
    print_stats("H2  (last 50%)", s_h2);

    // --- Fat-tail concentration (full sample, gate ON) ---
    std::vector<double> pnls; std::vector<int64_t> ts;
    auto sf = drive(bars,0,bars.size(),0.185,0.0,true,&pnls,&ts);
    std::vector<double> sorted=pnls; std::sort(sorted.rbegin(),sorted.rend());
    double top3=0; for(int i=0;i<3&&i<(int)sorted.size();++i) top3+=sorted[i];
    std::printf("\n--- FAT TAIL (full, gate ON) ---\n");
    std::printf("  net=%+.2f  top3 wins=%+.2f  top3 share of net=%.0f%%\n",
                sf.net, top3, sf.net!=0?100.0*top3/sf.net:0.0);
    std::printf("  top3 trade pnls: ");
    for(int i=0;i<3&&i<(int)sorted.size();++i) std::printf("%+.2f ",sorted[i]);
    std::printf("\n");
    return 0;
}

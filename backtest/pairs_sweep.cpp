// =============================================================================
// pairs_sweep.cpp -- EurGbpPairsEngine CRTP-ish sweep harness
//
// Streams interleaved M5 OHLC bars of EURUSD + GBPUSD by ts, feeds them to
// the real omega::EurGbpPairsEngine, sweeps over (z_window, z_in, z_out, hold).
//
// BUILD:  bash backtest/build_pairs_sweep.sh
// RUN:    ./pairs_sweep <eur_m5.csv> <gbp_m5.csv> <out.csv> [cost_per_leg=0.00010]
//
// =============================================================================

#include "OmegaTimeShim.hpp"
#include "../include/OmegaTradeLedger.hpp"
#include "../include/EurGbpPairsEngine.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>

struct Bar { int64_t ts; double o,h,l,c; };

static std::vector<Bar> load_m5(const char* path) {
    std::vector<Bar> v;
    std::ifstream f(path); std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        Bar b{};
        if (sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf",
                   (long long*)&b.ts, &b.o, &b.h, &b.l, &b.c) == 5)
            v.push_back(b);
    }
    return v;
}

struct TradeStats {
    int n=0; double pnl=0; int wins=0;
    double cum=0, peak=0, mdd=0;
    std::vector<double> rets;
    void add(double p) {
        ++n; pnl+=p; rets.push_back(p);
        if (p>0) ++wins;
        cum+=p;
        if (cum>peak) peak=cum;
        if (peak-cum>mdd) mdd=peak-cum;
    }
    double sharpe() const {
        if (rets.size()<2) return 0;
        double m=pnl/rets.size(), v=0;
        for (double r:rets) v+=(r-m)*(r-m);
        v/=(rets.size()-1);
        double sd=std::sqrt(v);
        return sd>0 ? (m/sd)*std::sqrt(252.0) : 0;
    }
};

FILE* g_csv = nullptr;
#define SCSV(...) fprintf(g_csv, __VA_ARGS__)

struct ParamSet {
    int z_window;
    double z_in, z_out;
    int hold;
};

static std::vector<ParamSet> param_grid() {
    std::vector<ParamSet> g;
    for (int w : {40, 60, 80, 120, 200})
    for (double zi : {1.0, 1.5, 2.0, 2.5, 3.0})
    for (double zo : {0.0, 0.3, 0.5, 0.8})
    for (int h : {12, 24, 48, 72, 120})
        g.push_back({w, zi, zo, h});
    return g;
}

static void run_one(const ParamSet& ps,
                    const std::vector<Bar>& eur_m5,
                    const std::vector<Bar>& gbp_m5,
                    double cost_per_leg,
                    TradeStats& st)
{
    omega::EurGbpPairsEngine eng;
    eng.shadow_mode = true; eng.enabled = true;
    eng.p.z_window = ps.z_window;
    eng.p.z_in     = ps.z_in;
    eng.p.z_out    = ps.z_out;
    eng.p.hold_timeout_h1 = ps.hold;
    eng.p.max_spread_eur = 1.0; // disable filter for backtest
    eng.p.max_spread_gbp = 1.0;
    eng.p.weekend_close_gate = false;

    // Cost = 2 * spread; we model it as a flat per-leg deduction at exit.
    auto on_close = [&st, cost_per_leg](const omega::TradeRecord& tr) {
        // cost_per_leg is in PRICE units (0.00010 = 1 pip). PnL per pip at 0.01 lot = $0.10.
        // So $cost = cost_per_leg * 100000 * size * 2 legs.
        const double cost_dollars = cost_per_leg * 100000.0 * tr.size * 2.0;
        st.add(tr.pnl - cost_dollars);
    };

    // Interleave EUR + GBP M5 bars by ts. We need to call on_tick_* in order.
    size_t i_e = 0, i_g = 0;
    const double eur_spread = 0.00010, gbp_spread = 0.00012;
    while (i_e < eur_m5.size() || i_g < gbp_m5.size()) {
        bool take_eur;
        if (i_e >= eur_m5.size()) take_eur = false;
        else if (i_g >= gbp_m5.size()) take_eur = true;
        else take_eur = (eur_m5[i_e].ts <= gbp_m5[i_g].ts);

        // S37 audit fix: replace bar-direction-dependent 4-point synthesis
        // (close>=open => L->H, else H->L) with a NEUTRAL ordering that does
        // NOT depend on bar direction. The old logic fed an adverse-then-
        // favorable path on directional bars, which biased the engine's
        // z-score trigger timing toward the more comfortable side. We now
        // always use open -> low -> high -> close. This is the conservative
        // ordering for a long-spread position (adverse first); for short-
        // spread it's the optimistic ordering. The pair is symmetric on z
        // (long-spread enters at high-z, short-spread at low-z), so any
        // residual bias is much smaller than the prior path-direction one.
        if (take_eur) {
            const auto& b = eur_m5[i_e++];
            int64_t ts_ms = b.ts * 1000LL;
            // 4 ticks per M5 (open, low, high, close) at +0/60/120/240 sec
            eng.on_tick_eur(b.o - eur_spread/2, b.o + eur_spread/2, ts_ms,         on_close);
            eng.on_tick_eur(b.l - eur_spread/2, b.l + eur_spread/2, ts_ms+60000,   on_close);
            eng.on_tick_eur(b.h - eur_spread/2, b.h + eur_spread/2, ts_ms+120000,  on_close);
            eng.on_tick_eur(b.c - eur_spread/2, b.c + eur_spread/2, ts_ms+240000,  on_close);
        } else {
            const auto& b = gbp_m5[i_g++];
            int64_t ts_ms = b.ts * 1000LL;
            eng.on_tick_gbp(b.o - gbp_spread/2, b.o + gbp_spread/2, ts_ms,         on_close);
            eng.on_tick_gbp(b.l - gbp_spread/2, b.l + gbp_spread/2, ts_ms+60000,   on_close);
            eng.on_tick_gbp(b.h - gbp_spread/2, b.h + gbp_spread/2, ts_ms+120000,  on_close);
            eng.on_tick_gbp(b.c - gbp_spread/2, b.c + gbp_spread/2, ts_ms+240000,  on_close);
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <eur_m5.csv> <gbp_m5.csv> <out.csv> [cost_per_leg=0.00010]\n",
                argv[0]); return 1;
    }
    auto eur = load_m5(argv[1]);
    auto gbp = load_m5(argv[2]);
    double cost = (argc>=5) ? atof(argv[4]) : 0.00010;
    fprintf(stderr, "[LOAD] eur=%zu gbp=%zu cost_per_leg=%.5f\n", eur.size(), gbp.size(), cost);

    g_csv = fopen(argv[3], "w");
    if (!g_csv) { fprintf(stderr, "cannot open %s\n", argv[3]); return 1; }

    // Silence engine spam
    fflush(stdout);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) { dup2(dn, fileno(stdout)); close(dn); }

    auto grid = param_grid();
    fprintf(stderr, "[SWEEP] %zu configs\n", grid.size());
    SCSV("rank,cfg,n,pnl_usd,wr_pct,sharpe,maxdd,expectancy\n");
    std::vector<std::pair<TradeStats,std::string>> results;
    size_t done = 0;
    for (const auto& ps : grid) {
        TradeStats st;
        run_one(ps, eur, gbp, cost, st);
        ++done;
        if (done % 10 == 0) fprintf(stderr, "  %zu/%zu done\n", done, grid.size());
        if (st.n < 10) continue;
        char buf[128];
        snprintf(buf, sizeof(buf), "w=%d_zi=%.1f_zo=%.1f_h=%d",
                 ps.z_window, ps.z_in, ps.z_out, ps.hold);
        results.push_back({st, buf});
    }
    std::sort(results.begin(), results.end(),
              [](auto&a,auto&b){return a.first.sharpe() > b.first.sharpe();});
    int rank = 0;
    for (auto& [st, cfg] : results) {
        double wr = st.n ? 100.0*st.wins/st.n : 0;
        double exp_ = st.n ? st.pnl/st.n : 0;
        SCSV("%d,%s,%d,%.2f,%.1f,%.3f,%.2f,%.4f\n",
             rank++, cfg.c_str(), st.n, st.pnl, wr, st.sharpe(), st.mdd, exp_);
    }
    fflush(g_csv); fclose(g_csv);
    fprintf(stderr, "[DONE] %zu valid configs -> %s\n", results.size(), argv[3]);
    return 0;
}

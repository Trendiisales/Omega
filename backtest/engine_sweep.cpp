// =============================================================================
// engine_sweep.cpp -- CRTP harness, M5-interleaved tick stream
//
// Drives REAL Omega engines through M5 OHLC bars. Sweeps params per-engine.
//
// BUILD:  bash backtest/build_engine_sweep.sh
// RUN:    ./engine_sweep <engine> <m5.csv> <out.csv>  [cost_pt]
//
//   engine = minh4 | minh4_ger
//
// =============================================================================

#include "OmegaTimeShim.hpp"
#include "../include/OmegaTradeLedger.hpp"
#include "../include/MinimalH4Breakout.hpp"
#include "../include/MinimalH4GER40Breakout.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>
#include <algorithm>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>

// =============================================================================
//  Bar + loader
// =============================================================================
struct Bar { int64_t ts; double o,h,l,c; };

static std::vector<Bar> load_m5(const char* path) {
    std::vector<Bar> v;
    std::ifstream f(path); std::string line;
    std::getline(f, line); // header
    while (std::getline(f, line)) {
        Bar b{};
        if (sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf",
                   (long long*)&b.ts, &b.o, &b.h, &b.l, &b.c) == 5) {
            v.push_back(b);
        }
    }
    return v;
}

// Build daily OHLC from M5 bars (UTC days)
struct DailyBar { int64_t ts_close_ms; double o,h,l,c; size_t end_m5_idx; };
static std::vector<DailyBar> aggregate_daily(const std::vector<Bar>& m5) {
    std::vector<DailyBar> out;
    int64_t cur_day = -1;
    DailyBar cur{};
    for (size_t i = 0; i < m5.size(); ++i) {
        int64_t day = m5[i].ts / 86400;
        if (day != cur_day) {
            if (cur_day != -1) {
                cur.ts_close_ms = (cur_day * 86400 + 86340) * 1000LL; // 23:59 close
                cur.end_m5_idx = i;
                out.push_back(cur);
            }
            cur_day = day;
            cur.o = m5[i].o; cur.h = m5[i].h; cur.l = m5[i].l; cur.c = m5[i].c;
        } else {
            if (m5[i].h > cur.h) cur.h = m5[i].h;
            if (m5[i].l < cur.l) cur.l = m5[i].l;
            cur.c = m5[i].c;
        }
    }
    if (cur_day != -1) {
        cur.ts_close_ms = (cur_day * 86400 + 86340) * 1000LL;
        cur.end_m5_idx = m5.size();
        out.push_back(cur);
    }
    return out;
}

// ATR14 over daily bars
static std::vector<double> atr14_daily(const std::vector<DailyBar>& d) {
    std::vector<double> out(d.size(), 0.0);
    double prev_c = 0; std::vector<double> trs;
    for (size_t i = 0; i < d.size(); ++i) {
        double tr = d[i].h - d[i].l;
        if (i > 0) {
            tr = std::max(tr, std::fabs(d[i].h - prev_c));
            tr = std::max(tr, std::fabs(d[i].l - prev_c));
        }
        trs.push_back(tr);
        if (trs.size() > 14) trs.erase(trs.begin());
        prev_c = d[i].c;
        if (trs.size() == 14) {
            double s = 0; for (double t : trs) s += t;
            out[i] = s / 14.0;
        }
    }
    return out;
}

// H4 aggregator: 4-hour buckets (0/4/8/12/16/20 UTC)
static std::vector<DailyBar> aggregate_h4(const std::vector<Bar>& m5) {
    std::vector<DailyBar> out;
    int64_t cur_b = -1; DailyBar cur{};
    for (size_t i = 0; i < m5.size(); ++i) {
        int64_t b = m5[i].ts / (4*3600);
        if (b != cur_b) {
            if (cur_b != -1) {
                cur.ts_close_ms = (cur_b * 4 * 3600 + 4*3600 - 1) * 1000LL;
                cur.end_m5_idx = i;
                out.push_back(cur);
            }
            cur_b = b;
            cur.o = m5[i].o; cur.h = m5[i].h; cur.l = m5[i].l; cur.c = m5[i].c;
        } else {
            if (m5[i].h > cur.h) cur.h = m5[i].h;
            if (m5[i].l < cur.l) cur.l = m5[i].l;
            cur.c = m5[i].c;
        }
    }
    if (cur_b != -1) {
        cur.ts_close_ms = (cur_b * 4 * 3600 + 4*3600 - 1) * 1000LL;
        cur.end_m5_idx = m5.size();
        out.push_back(cur);
    }
    return out;
}

static std::vector<double> atr14_bars(const std::vector<DailyBar>& d) {
    return atr14_daily(d);
}

// =============================================================================
//  Trade stats + CSV sink
// =============================================================================
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

// =============================================================================
//  CRTP base sweeper
// =============================================================================
template<typename Derived>
struct EngineSweeper {
    void run(const std::vector<Bar>& m5, double cost_pt) {
        auto grid = Derived::param_grid();
        fprintf(stderr, "# %s sweep: m5_bars=%zu combos=%zu cost_pt=%.3f\n",
                Derived::label(), m5.size(), grid.size(), cost_pt);
        SCSV("rank,cfg,n,pnl_usd,wr_pct,sharpe,maxdd,expectancy\n");
        std::vector<std::pair<TradeStats,std::string>> results;
        size_t done=0;
        for (const auto& p : grid) {
            TradeStats st;
            Derived::run_one(p, m5, cost_pt, st);
            ++done;
            if (done % 50 == 0)
                fprintf(stderr, "  %zu/%zu done\n", done, grid.size());
            if (st.n < 5) continue;
            results.push_back({st, Derived::cfg_label(p)});
        }
        std::sort(results.begin(), results.end(),
                  [](auto&a,auto&b){return a.first.sharpe() > b.first.sharpe();});
        int rank=0;
        for (auto& [st,cfg] : results) {
            double wr = st.n ? 100.0*st.wins/st.n : 0;
            double exp_ = st.n ? st.pnl/st.n : 0;
            SCSV("%d,%s,%d,%.2f,%.1f,%.3f,%.2f,%.4f\n",
                 rank++, cfg.c_str(), st.n, st.pnl, wr, st.sharpe(),
                 st.mdd, exp_);
        }
        fprintf(stderr, "[DBG] total combos=%zu valid_results=%zu\n", done, results.size());
    }
};

// =============================================================================
//  MinimalH4 sweep -- drives REAL omega::MinimalH4Breakout on H4 bars
// =============================================================================
struct MinH4Sweep : EngineSweeper<MinH4Sweep> {
    struct ParamSet {
        int donchian; double sl_mult, tp_mult; int timeout; int cooldown;
        bool long_only;
    };
    static const char* label() { return "MinimalH4Breakout"; }
    static std::string cfg_label(const ParamSet& p) {
        char buf[256];
        snprintf(buf,sizeof(buf),
                 "don=%d_sl=%.1f_tp=%.1f_to=%d_cd=%d_lo=%d",
                 p.donchian, p.sl_mult, p.tp_mult, p.timeout, p.cooldown,
                 p.long_only?1:0);
        return buf;
    }
    static std::vector<ParamSet> param_grid() {
        std::vector<ParamSet> g;
        for (int don : {6, 8, 10, 14, 20})
        for (double sl : {1.0, 1.5, 2.0, 2.5})
        for (double tp : {2.5, 4.0, 6.0, 8.0})
        for (int to : {12, 24, 48})
        for (int cd : {1, 2, 4})
        for (bool lo : {true, false})
            g.push_back({don, sl, tp, to, cd, lo});
        return g;
    }
    static void run_one(const ParamSet& p,
                        const std::vector<Bar>& m5,
                        double cost_pt,
                        TradeStats& st) {
        omega::MinimalH4Breakout eng;
        eng.shadow_mode = true;
        eng.enabled = true;
        eng.symbol = "XAUUSD";
        eng.p.donchian_bars       = p.donchian;
        eng.p.sl_mult             = p.sl_mult;
        eng.p.tp_mult             = p.tp_mult;
        eng.p.timeout_h4_bars     = p.timeout;
        eng.p.cooldown_h4_bars    = p.cooldown;
        eng.p.long_only           = p.long_only;
        eng.p.weekend_close_gate  = false;

        auto on_close = [&st, cost_pt](const omega::TradeRecord& tr) {
            st.add(tr.pnl - cost_pt);
        };

        auto h4 = aggregate_h4(m5);
        auto atr = atr14_bars(h4);
        const double spread = 0.30;
        size_t m5_idx = 0;
        for (size_t i = 0; i < h4.size(); ++i) {
            const auto& hb = h4[i];
            const double a = atr[i];
            // stream M5 between
            while (m5_idx < hb.end_m5_idx) {
                const auto& bar = m5[m5_idx];
                int64_t ts_ms = bar.ts * 1000LL;
                double bid_c = bar.c - spread/2, ask_c = bar.c + spread/2;
                eng.on_tick(bid_c, ask_c, ts_ms, on_close);
                ++m5_idx;
            }
            // fire H4 bar close (sig: high, low, close, bid, ask, atr, ts_ms, cb)
            if (a > 0)
                eng.on_h4_bar(hb.h, hb.l, hb.c,
                              hb.c - spread/2, hb.c + spread/2,
                              a, hb.ts_close_ms, on_close);
        }
    }
};

// =============================================================================
//  MinH4 GER40 sweep -- drives real omega::MinimalH4GER40Breakout
// =============================================================================
struct MinH4GERSweep : EngineSweeper<MinH4GERSweep> {
    struct ParamSet {
        int donchian; double sl_mult, tp_mult; int timeout; int cooldown;
        bool long_only;
    };
    static const char* label() { return "MinimalH4GER40Breakout"; }
    static std::string cfg_label(const ParamSet& p) {
        char buf[256];
        snprintf(buf,sizeof(buf),
                 "don=%d_sl=%.1f_tp=%.1f_to=%d_cd=%d_lo=%d",
                 p.donchian, p.sl_mult, p.tp_mult, p.timeout, p.cooldown,
                 p.long_only?1:0);
        return buf;
    }
    static std::vector<ParamSet> param_grid() {
        std::vector<ParamSet> g;
        for (int don : {6, 8, 10, 14, 20})
        for (double sl : {1.0, 1.5, 2.0, 2.5})
        for (double tp : {2.0, 3.0, 4.0, 6.0})
        for (int to : {24, 48})
        for (int cd : {2, 4})
        for (bool lo : {true, false})
            g.push_back({don, sl, tp, to, cd, lo});
        return g;
    }
    static void run_one(const ParamSet& p,
                        const std::vector<Bar>& m5,
                        double cost_pt,
                        TradeStats& st) {
        omega::MinimalH4GER40Breakout eng;
        eng.shadow_mode = true;
        eng.enabled = true;
        eng.symbol = "GER40";
        eng.p.donchian_bars       = p.donchian;
        eng.p.sl_mult             = p.sl_mult;
        eng.p.tp_mult             = p.tp_mult;
        eng.p.timeout_h4_bars     = p.timeout;
        eng.p.cooldown_h4_bars    = p.cooldown;
        eng.p.long_only           = p.long_only;
        eng.p.weekend_close_gate  = false;

        auto on_close = [&st, cost_pt](const omega::TradeRecord& tr) {
            st.add(tr.pnl - cost_pt);
        };

        const double spread = 2.0;
        for (const auto& bar : m5) {
            int64_t ts_ms = bar.ts * 1000LL;
            double bid_c = bar.c - spread/2, ask_c = bar.c + spread/2;
            eng.on_tick(bid_c, ask_c, ts_ms, on_close);
        }
    }
};

// =============================================================================
//  main
// =============================================================================
int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <engine: minh4|minh4_ger> <m5.csv> <out.csv> [cost_pt=0.30]\n",
                argv[0]);
        return 1;
    }
    const char* eng_name = argv[1];
    const char* m5_path  = argv[2];
    const char* out_path = argv[3];
    double cost_pt = (argc>=5) ? atof(argv[4]) : 0.30;

    auto m5 = load_m5(m5_path);
    if (m5.empty()) { fprintf(stderr, "no m5 bars loaded\n"); return 1; }
    fprintf(stderr, "[LOAD] %zu m5 bars\n", m5.size());

    g_csv = fopen(out_path, "w");
    if (!g_csv) { fprintf(stderr, "cannot open %s\n", out_path); return 1; }

    // Silence engine printf spam
    fflush(stdout);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull < 0) return 1;
    dup2(devnull, fileno(stdout));
    close(devnull);

    if (strcmp(eng_name, "minh4") == 0) {
        MinH4Sweep().run(m5, cost_pt);
    } else if (strcmp(eng_name, "minh4_ger") == 0) {
        MinH4GERSweep().run(m5, cost_pt);
    } else {
        fprintf(stderr, "unknown engine: %s\n", eng_name);
        return 1;
    }

    fflush(g_csv);
    fclose(g_csv);
    fprintf(stderr, "[DONE] -> %s\n", out_path);
    return 0;
}

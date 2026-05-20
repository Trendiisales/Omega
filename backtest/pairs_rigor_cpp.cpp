// =============================================================================
// pairs_rigor_cpp.cpp -- comprehensive rigor harness for EurGbpPairsEngine.
//
// Modes:
//   isoos      : 50/50 IS/OOS time split with same config -> Sharpe each half
//   wf         : N-fold rolling walk-forward (default 4 folds)
//   monthly    : per-month PnL distribution (UTC month buckets)
//   robust     : ±20% perturbation of (z_window, z_in, hold)
//   monte      : Monte Carlo permutation -- shuffle returns, p-value of Sharpe
//   cost       : cost stress matrix 0..4 pips/leg
//
// BUILD:  bash backtest/build_pairs_rigor.sh
// RUN:    ./pairs_rigor <mode> <eur_m5.csv> <gbp_m5.csv> <out.txt>
//         optional: --w N --zi F --zo F --h N --cost F --folds N --nperm N
// =============================================================================

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
#include <random>
#include <map>

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

struct Trade {
    int64_t ts;
    double pnl;
};

struct TradeStats {
    std::vector<Trade> trades;
    void add(int64_t ts, double pnl) { trades.push_back({ts, pnl}); }
    int n() const { return (int)trades.size(); }
    double pnl_sum() const { double s=0; for (auto& t:trades) s+=t.pnl; return s; }
    int wins() const { int w=0; for (auto& t:trades) if(t.pnl>0) ++w; return w; }
    double sharpe() const {
        if (trades.size()<2) return 0;
        double s=0; for(auto&t:trades) s+=t.pnl;
        double m=s/trades.size(), v=0;
        for(auto&t:trades) v+=(t.pnl-m)*(t.pnl-m);
        v/=(trades.size()-1);
        double sd=std::sqrt(v);
        return sd>0 ? (m/sd)*std::sqrt(252.0) : 0;
    }
    double mdd() const {
        double cum=0, pk=0, mdd=0;
        for(auto&t:trades) {
            cum+=t.pnl;
            if(cum>pk) pk=cum;
            if(pk-cum>mdd) mdd=pk-cum;
        }
        return mdd;
    }
};

struct Cfg {
    int z_window = 120;
    double z_in = 1.5, z_out = 0.5;
    int hold = 48;
    double cost_per_leg = 0.00010;
    int64_t bucket_ms = 3600000LL; // H1 default
};

// Run engine on bars within [ts_lo, ts_hi). Returns trades.
static TradeStats run_engine(const std::vector<Bar>& eur, const std::vector<Bar>& gbp,
                              const Cfg& cfg,
                              int64_t ts_lo = 0, int64_t ts_hi = INT64_MAX)
{
    TradeStats st;
    omega::EurGbpPairsEngine eng;
    eng.shadow_mode = true; eng.enabled = true;
    eng.p.z_window = cfg.z_window;
    eng.p.z_in = cfg.z_in; eng.p.z_out = cfg.z_out;
    eng.p.hold_timeout_h1 = cfg.hold;
    eng.p.bucket_ms = cfg.bucket_ms;
    eng.p.max_spread_eur = 1.0;
    eng.p.max_spread_gbp = 1.0;
    eng.p.weekend_close_gate = false;

    auto on_close = [&st, &cfg](const omega::TradeRecord& tr) {
        const double cost = cfg.cost_per_leg * 100000.0 * tr.size * 2.0;
        st.add(tr.exitTs, tr.pnl - cost);
    };

    size_t i_e = 0, i_g = 0;
    const double esp = 0.00010, gsp = 0.00012;
    auto stream_eur = [&](const Bar& b){
        if (b.ts < ts_lo || b.ts >= ts_hi) return;
        int64_t ms = b.ts * 1000LL;
        eng.on_tick_eur(b.o-esp/2, b.o+esp/2, ms, on_close);
        if (b.c >= b.o) {
            eng.on_tick_eur(b.l-esp/2, b.l+esp/2, ms+60000, on_close);
            eng.on_tick_eur(b.h-esp/2, b.h+esp/2, ms+120000, on_close);
        } else {
            eng.on_tick_eur(b.h-esp/2, b.h+esp/2, ms+60000, on_close);
            eng.on_tick_eur(b.l-esp/2, b.l+esp/2, ms+120000, on_close);
        }
        eng.on_tick_eur(b.c-esp/2, b.c+esp/2, ms+240000, on_close);
    };
    auto stream_gbp = [&](const Bar& b){
        if (b.ts < ts_lo || b.ts >= ts_hi) return;
        int64_t ms = b.ts * 1000LL;
        eng.on_tick_gbp(b.o-gsp/2, b.o+gsp/2, ms, on_close);
        if (b.c >= b.o) {
            eng.on_tick_gbp(b.l-gsp/2, b.l+gsp/2, ms+60000, on_close);
            eng.on_tick_gbp(b.h-gsp/2, b.h+gsp/2, ms+120000, on_close);
        } else {
            eng.on_tick_gbp(b.h-gsp/2, b.h+gsp/2, ms+60000, on_close);
            eng.on_tick_gbp(b.l-gsp/2, b.l+gsp/2, ms+120000, on_close);
        }
        eng.on_tick_gbp(b.c-gsp/2, b.c+gsp/2, ms+240000, on_close);
    };

    while (i_e < eur.size() || i_g < gbp.size()) {
        bool take_eur;
        if (i_e >= eur.size()) take_eur = false;
        else if (i_g >= gbp.size()) take_eur = true;
        else take_eur = (eur[i_e].ts <= gbp[i_g].ts);
        if (take_eur) stream_eur(eur[i_e++]);
        else          stream_gbp(gbp[i_g++]);
    }
    return st;
}

static void print_stats(FILE* out, const char* label, const TradeStats& st) {
    int n = st.n();
    double pnl = st.pnl_sum();
    int w = st.wins();
    double wr = n ? 100.0*w/n : 0;
    double sh = st.sharpe();
    double md = st.mdd();
    fprintf(out, "%s n=%-4d pnl=$%8.2f WR=%5.1f%% Sh=%6.3f MDD=$%6.2f\n",
            label, n, pnl, wr, sh, md);
}

// ---- mode handlers ----

static void mode_isoos(const std::vector<Bar>& eur, const std::vector<Bar>& gbp,
                       const Cfg& cfg, FILE* out)
{
    int64_t lo = INT64_MAX, hi = 0;
    for (auto& b : eur) { lo = std::min(lo, b.ts); hi = std::max(hi, b.ts); }
    for (auto& b : gbp) { lo = std::min(lo, b.ts); hi = std::max(hi, b.ts); }
    int64_t mid = lo + (hi - lo) / 2;
    fprintf(out, "== IS/OOS 50/50 split: ts [%lld..%lld..%lld] ==\n",
            (long long)lo, (long long)mid, (long long)hi);
    fprintf(out, "Config: w=%d zi=%.1f zo=%.1f h=%d cost=%.5f\n",
            cfg.z_window, cfg.z_in, cfg.z_out, cfg.hold, cfg.cost_per_leg);
    auto is_ = run_engine(eur, gbp, cfg, lo, mid);
    auto oos = run_engine(eur, gbp, cfg, mid, hi+1);
    print_stats(out, "IS ", is_);
    print_stats(out, "OOS", oos);
    auto full = run_engine(eur, gbp, cfg, lo, hi+1);
    print_stats(out, "FUL", full);
}

static void mode_wf(const std::vector<Bar>& eur, const std::vector<Bar>& gbp,
                    const Cfg& cfg, int folds, FILE* out)
{
    int64_t lo = INT64_MAX, hi = 0;
    for (auto& b : eur) { lo = std::min(lo, b.ts); hi = std::max(hi, b.ts); }
    for (auto& b : gbp) { lo = std::min(lo, b.ts); hi = std::max(hi, b.ts); }
    fprintf(out, "== Walk-forward %d folds ==\n", folds);
    fprintf(out, "Config: w=%d zi=%.1f zo=%.1f h=%d cost=%.5f\n",
            cfg.z_window, cfg.z_in, cfg.z_out, cfg.hold, cfg.cost_per_leg);
    int positive = 0;
    for (int f = 0; f < folds; ++f) {
        int64_t a = lo + (hi-lo)*f/folds;
        int64_t b = lo + (hi-lo)*(f+1)/folds;
        auto st = run_engine(eur, gbp, cfg, a, b);
        char tag[32]; snprintf(tag, sizeof(tag), "F%d", f+1);
        print_stats(out, tag, st);
        if (st.sharpe() > 0) ++positive;
    }
    fprintf(out, "Positive folds: %d/%d\n", positive, folds);
}

static void mode_monthly(const std::vector<Bar>& eur, const std::vector<Bar>& gbp,
                         const Cfg& cfg, FILE* out)
{
    auto full = run_engine(eur, gbp, cfg);
    std::map<int, std::vector<double>> by_month; // yyyymm -> pnls
    for (auto& t : full.trades) {
        time_t ts = t.ts;
        struct tm* tmp = gmtime(&ts);
        int yyyymm = (tmp->tm_year + 1900) * 100 + (tmp->tm_mon + 1);
        by_month[yyyymm].push_back(t.pnl);
    }
    fprintf(out, "== Monthly PnL distribution (cost=%.5f) ==\n", cfg.cost_per_leg);
    fprintf(out, "yyyymm  n    pnl       wr      mean      worst\n");
    int positive=0, total=0;
    double total_pnl = 0;
    for (auto& [ym, ps] : by_month) {
        double s=0, mn=1e18; int w=0;
        for (double p : ps) { s+=p; if (p>0) ++w; if (p<mn) mn=p; }
        double me = ps.size() ? s/ps.size() : 0;
        double wr = ps.size() ? 100.0*w/ps.size() : 0;
        fprintf(out, "%-7d %-4zu $%-8.2f %5.1f%% $%-8.4f $%-8.4f\n",
                ym, ps.size(), s, wr, me, mn);
        if (s > 0) ++positive; ++total; total_pnl += s;
    }
    fprintf(out, "Positive months: %d/%d (%.1f%%)  total=$%.2f\n",
            positive, total, total ? 100.0*positive/total : 0, total_pnl);
}

static void mode_robust(const std::vector<Bar>& eur, const std::vector<Bar>& gbp,
                        const Cfg& base, FILE* out)
{
    fprintf(out, "== Robustness ±20%% perturbation (base w=%d zi=%.1f zo=%.1f h=%d) ==\n",
            base.z_window, base.z_in, base.z_out, base.hold);
    struct Pert { const char* name; Cfg cfg; };
    std::vector<Pert> perts;
    perts.push_back({"baseline", base});
    Cfg c;
    c = base; c.z_window = (int)(base.z_window*0.8); perts.push_back({"w-20%", c});
    c = base; c.z_window = (int)(base.z_window*1.2); perts.push_back({"w+20%", c});
    c = base; c.z_in = base.z_in*0.8;                perts.push_back({"zi-20%", c});
    c = base; c.z_in = base.z_in*1.2;                perts.push_back({"zi+20%", c});
    c = base; c.z_out = base.z_out*0.5;              perts.push_back({"zo-50%", c});
    c = base; c.z_out = base.z_out*1.5;              perts.push_back({"zo+50%", c});
    c = base; c.hold  = (int)(base.hold*0.5);        perts.push_back({"h-50%", c});
    c = base; c.hold  = (int)(base.hold*1.5);        perts.push_back({"h+50%", c});
    for (auto& p : perts) {
        auto st = run_engine(eur, gbp, p.cfg);
        char tag[64]; snprintf(tag, sizeof(tag), "%-9s", p.name);
        print_stats(out, tag, st);
    }
}

static void mode_monte(const std::vector<Bar>& eur, const std::vector<Bar>& gbp,
                       const Cfg& cfg, int n_perm, FILE* out)
{
    auto full = run_engine(eur, gbp, cfg);
    double actual_sh = full.sharpe();
    fprintf(out, "== Monte Carlo permutation test (n_perm=%d) ==\n", n_perm);
    fprintf(out, "Actual trades: %d, Actual Sharpe: %.3f\n", full.n(), actual_sh);
    // Permutation: randomly flip sign of each trade pnl (under null: equal P(profit)) ->
    // measure how often shuffled Sharpe >= actual.
    std::mt19937 rng(42);
    int ge_count = 0;
    std::vector<double> base; base.reserve(full.trades.size());
    for (auto& t : full.trades) base.push_back(t.pnl);
    for (int p = 0; p < n_perm; ++p) {
        double s=0, v=0;
        std::vector<double> perm = base;
        // Random sign flip
        for (auto& x : perm) {
            if (rng() & 1) x = -x;
            s += x;
        }
        double m = s / perm.size();
        for (double x : perm) v += (x-m)*(x-m);
        v /= (perm.size()-1);
        double sd = std::sqrt(v);
        double sh = sd>0 ? (m/sd)*std::sqrt(252.0) : 0;
        if (sh >= actual_sh) ++ge_count;
    }
    double pv = (double)ge_count / n_perm;
    fprintf(out, "Permutations >= actual Sharpe: %d/%d (p=%.5f)\n", ge_count, n_perm, pv);
    fprintf(out, "Interpretation: %s\n",
            pv < 0.001 ? "HIGHLY SIGNIFICANT (p<0.001)"
            : pv < 0.01 ? "SIGNIFICANT (p<0.01)"
            : pv < 0.05 ? "marginally significant"
            : "NOT significant");
}

// ---- First-N trades cumulative Sharpe (stabilization) ----
static void mode_firstn(const std::vector<Bar>& eur, const std::vector<Bar>& gbp,
                         const Cfg& cfg, FILE* out)
{
    auto full = run_engine(eur, gbp, cfg);
    fprintf(out, "== First-N cumulative Sharpe (config w=%d zi=%.1f zo=%.1f h=%d cost=%.5f) ==\n",
            cfg.z_window, cfg.z_in, cfg.z_out, cfg.hold, cfg.cost_per_leg);
    fprintf(out, "n       pnl       wr      sharpe   mdd\n");
    std::vector<double> pnls; pnls.reserve(full.trades.size());
    for (size_t i = 0; i < full.trades.size(); ++i) {
        pnls.push_back(full.trades[i].pnl);
        int n = (int)pnls.size();
        if (n == 5 || n == 10 || n == 15 || n == 20 || n == 25 || n == 30 ||
            n == 40 || n == 50 || n == 75 || n == 100 || n == 150 || n == 200 ||
            n == 300 || n == (int)full.trades.size()) {
            double s = 0; for (double x : pnls) s += x;
            double m = s / n, v = 0;
            for (double x : pnls) v += (x-m)*(x-m);
            v /= (n>1 ? n-1 : 1);
            double sd = std::sqrt(v);
            double sh = sd>0 ? (m/sd)*std::sqrt(252.0) : 0;
            int w = 0; for (double x : pnls) if (x>0) ++w;
            double cum=0, pk=0, mdd=0;
            for (double x : pnls) { cum+=x; if(cum>pk) pk=cum; if(pk-cum>mdd) mdd=pk-cum; }
            fprintf(out, "%-7d $%-8.2f %5.1f%%  %6.3f  $%-6.2f\n",
                    n, s, 100.0*w/n, sh, mdd);
        }
    }
}

// ---- Concurrent H1+H4 portfolio ----
// Run TWO engine instances (different bucket_ms) on the same tick stream.
// Compute combined PnL stream + correlation between engine streams.
static void mode_portfolio(const std::vector<Bar>& eur, const std::vector<Bar>& gbp,
                           const Cfg& h1_cfg, FILE* out)
{
    Cfg c1 = h1_cfg; c1.bucket_ms = 3600000LL;
    Cfg c4 = h1_cfg;
    c4.bucket_ms = 14400000LL;
    c4.z_window = 20; c4.z_in = 2.0; c4.z_out = 0.5; c4.hold = 6;
    fprintf(out, "== H1+H4 concurrent portfolio (cost=%.5f) ==\n", h1_cfg.cost_per_leg);
    fprintf(out, "H1 cfg: w=%d zi=%.1f zo=%.1f h=%d\n", c1.z_window,c1.z_in,c1.z_out,c1.hold);
    fprintf(out, "H4 cfg: w=%d zi=%.1f zo=%.1f h=%d\n", c4.z_window,c4.z_in,c4.z_out,c4.hold);

    auto h1 = run_engine(eur, gbp, c1);
    auto h4 = run_engine(eur, gbp, c4);

    print_stats(out, "H1 ", h1);
    print_stats(out, "H4 ", h4);

    // Merge trade streams by ts. Bucket daily PnL.
    std::map<int64_t, double> h1_daily, h4_daily;
    for (auto& t : h1.trades) h1_daily[t.ts / 86400] += t.pnl;
    for (auto& t : h4.trades) h4_daily[t.ts / 86400] += t.pnl;
    // Union of days
    std::map<int64_t, std::pair<double,double>> day_pnl;
    for (auto& [d, p] : h1_daily) day_pnl[d].first  = p;
    for (auto& [d, p] : h4_daily) day_pnl[d].second = p;
    int days = 0; double s1=0, s2=0;
    double v1=0, v2=0, cov=0;
    std::vector<double> r1, r2, rc;
    for (auto& [d, pp] : day_pnl) {
        ++days;
        r1.push_back(pp.first);
        r2.push_back(pp.second);
        rc.push_back(pp.first + pp.second);
        s1+=pp.first; s2+=pp.second;
    }
    double m1=s1/days, m2=s2/days;
    for (size_t i=0;i<r1.size();++i) {
        v1 += (r1[i]-m1)*(r1[i]-m1);
        v2 += (r2[i]-m2)*(r2[i]-m2);
        cov+= (r1[i]-m1)*(r2[i]-m2);
    }
    v1/=(days-1); v2/=(days-1); cov/=(days-1);
    double sd1=std::sqrt(v1), sd2=std::sqrt(v2);
    double corr = (sd1>0 && sd2>0) ? cov/(sd1*sd2) : 0;

    // Combined daily stream stats
    double sc=0; for (double x:rc) sc+=x;
    double mc=sc/days, vc=0;
    for (double x:rc) vc+=(x-mc)*(x-mc);
    vc/=(days-1);
    double sdc=std::sqrt(vc);
    double sh_c = sdc>0 ? (mc/sdc)*std::sqrt(252) : 0;
    double cum=0, pk=0, mddc=0;
    for (double x:rc) { cum+=x; if(cum>pk) pk=cum; if(pk-cum>mddc) mddc=pk-cum; }

    fprintf(out, "\nDaily-bucket portfolio metrics (%d trading days):\n", days);
    fprintf(out, "  H1 daily mean=$%.3f sd=$%.3f Sh(daily)=%.3f\n", m1, sd1, sd1>0?m1/sd1*std::sqrt(252):0);
    fprintf(out, "  H4 daily mean=$%.3f sd=$%.3f Sh(daily)=%.3f\n", m2, sd2, sd2>0?m2/sd2*std::sqrt(252):0);
    fprintf(out, "  Combined daily mean=$%.3f sd=$%.3f Sh=%.3f MDD=$%.2f\n", mc, sdc, sh_c, mddc);
    fprintf(out, "  H1<->H4 daily PnL correlation: %.3f\n", corr);
    fprintf(out, "  Total combined PnL: $%.2f (H1 $%.2f + H4 $%.2f)\n", s1+s2, s1, s2);
}

// ---- Recent-window test (last N days) ----
static void mode_recent(const std::vector<Bar>& eur, const std::vector<Bar>& gbp,
                         const Cfg& cfg, FILE* out)
{
    int64_t hi = 0;
    for (auto& b : eur) hi = std::max(hi, b.ts);
    fprintf(out, "== Recent-window test (config w=%d zi=%.1f zo=%.1f h=%d cost=%.5f) ==\n",
            cfg.z_window, cfg.z_in, cfg.z_out, cfg.hold, cfg.cost_per_leg);
    fprintf(out, "Most recent ts: %lld\n", (long long)hi);
    for (int days : {30, 60, 90, 180, 365}) {
        int64_t lo = hi - (int64_t)days * 86400;
        auto st = run_engine(eur, gbp, cfg, lo, hi+1);
        char tag[32]; snprintf(tag, sizeof(tag), "last%dd ", days);
        print_stats(out, tag, st);
    }
}

static void mode_cost(const std::vector<Bar>& eur, const std::vector<Bar>& gbp,
                      const Cfg& base, FILE* out)
{
    fprintf(out, "== Cost stress matrix (w=%d zi=%.1f zo=%.1f h=%d) ==\n",
            base.z_window, base.z_in, base.z_out, base.hold);
    for (double c : {0.0, 0.00005, 0.00010, 0.00015, 0.00020, 0.00030, 0.00050}) {
        Cfg cf = base; cf.cost_per_leg = c;
        auto st = run_engine(eur, gbp, cf);
        char tag[64]; snprintf(tag, sizeof(tag), "cost=%5.1fpip", c*10000);
        print_stats(out, tag, st);
    }
}

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s <mode> <eur_m5.csv> <gbp_m5.csv> <out.txt>"
                " [--w N --zi F --zo F --h N --cost F --folds N --nperm N]\n"
                "modes: isoos wf monthly robust monte cost\n", argv[0]);
        return 1;
    }
    const char* mode = argv[1];
    auto eur = load_m5(argv[2]);
    auto gbp = load_m5(argv[3]);
    fprintf(stderr, "[LOAD] eur=%zu gbp=%zu m5 bars\n", eur.size(), gbp.size());

    Cfg cfg;
    int folds = 4, n_perm = 10000;
    for (int i = 5; i < argc; ++i) {
        if (i+1>=argc) break;
        if (strcmp(argv[i],"--w")==0) cfg.z_window = atoi(argv[++i]);
        else if (strcmp(argv[i],"--zi")==0) cfg.z_in = atof(argv[++i]);
        else if (strcmp(argv[i],"--zo")==0) cfg.z_out = atof(argv[++i]);
        else if (strcmp(argv[i],"--h")==0) cfg.hold = atoi(argv[++i]);
        else if (strcmp(argv[i],"--cost")==0) cfg.cost_per_leg = atof(argv[++i]);
        else if (strcmp(argv[i],"--folds")==0) folds = atoi(argv[++i]);
        else if (strcmp(argv[i],"--nperm")==0) n_perm = atoi(argv[++i]);
        else if (strcmp(argv[i],"--bucket")==0) cfg.bucket_ms = atoll(argv[++i]);
        else if (strcmp(argv[i],"--h4")==0) cfg.bucket_ms = 14400000LL;
        else if (strcmp(argv[i],"--d1")==0) cfg.bucket_ms = 86400000LL;
    }

    FILE* out = fopen(argv[4], "w");
    if (!out) { fprintf(stderr, "cannot open %s\n", argv[4]); return 1; }

    // Silence engine printf spam
    fflush(stdout);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) { dup2(dn, fileno(stdout)); close(dn); }

    if (strcmp(mode,"isoos")==0)       mode_isoos(eur, gbp, cfg, out);
    else if (strcmp(mode,"wf")==0)     mode_wf(eur, gbp, cfg, folds, out);
    else if (strcmp(mode,"monthly")==0) mode_monthly(eur, gbp, cfg, out);
    else if (strcmp(mode,"robust")==0) mode_robust(eur, gbp, cfg, out);
    else if (strcmp(mode,"monte")==0)  mode_monte(eur, gbp, cfg, n_perm, out);
    else if (strcmp(mode,"cost")==0)   mode_cost(eur, gbp, cfg, out);
    else if (strcmp(mode,"firstn")==0) mode_firstn(eur, gbp, cfg, out);
    else if (strcmp(mode,"recent")==0) mode_recent(eur, gbp, cfg, out);
    else if (strcmp(mode,"portfolio")==0) mode_portfolio(eur, gbp, cfg, out);
    else { fprintf(stderr, "unknown mode: %s\n", mode); fclose(out); return 1; }

    fflush(out); fclose(out);
    fprintf(stderr, "[DONE] -> %s\n", argv[4]);
    return 0;
}

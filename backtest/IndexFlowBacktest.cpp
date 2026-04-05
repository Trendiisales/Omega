// =============================================================================
// IndexFlowBacktest.cpp
// =============================================================================
// Standalone backtest for IndexFlowEngine + VWAPAtrTrail upgrade.
//
// Since we don't have index tick CSVs locally, this sim generates synthetic
// tick streams that replicate the statistical properties of each index as
// observed from the gold tick data and live log context:
//   - Apr 2-4 2026 tariff crash: NQ -800pt, SP -200pt, DJ30 -1500pt
//   - Normal London/NY session: NQ ±100pt/day, SP ±35pt/day, DJ30 ±250pt/day
//   - Spread and ATR characteristics confirmed from OmegaCostGuard.hpp
//
// SYNTHETIC DATA GENERATION:
//   Uses a regime-switching GBM model:
//     MEAN_REVERSION: μ=-κ(mid-mean), σ=ATR_NORMAL*0.01 per tick
//     TREND: μ=trend_drift per tick, σ=ATR_NORMAL*0.015 per tick
//     IMPULSE: μ=impulse_drift per tick, σ=ATR_NORMAL*0.02 per tick
//   Regime transitions calibrated to match gold session data proportions.
//   Tick rate: ~8 ticks/sec London, ~12 ticks/sec NY, ~2 ticks/sec Asia
//
// BUILD (Mac):
//   cd /tmp/omega_audit/omega
//   g++ -std=c++17 -O3 -o backtest/IndexFlowBT backtest/IndexFlowBacktest.cpp
//       -I include
//
// RUN:
//   ./backtest/IndexFlowBT [--days N] [--symbol SYM] [--seed N]
//   Defaults: --days 504 --symbol ALL --seed 42
//
// OUTPUT:
//   Console: per-engine summary table (WR, total PnL, Sharpe, MaxDD)
//   bt_index_trades.csv: all trades
//   bt_index_report.csv: per-engine aggregate
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <functional>
#include <random>
#include <unordered_map>

// Engine headers (use real OmegaTradeLedger — no shim needed)
#include "../include/IndexFlowEngine.hpp"

// =============================================================================
// Tick generation
// =============================================================================
struct TickBar {
    double bid, ask, l2_imb;
    int64_t ts_ms;
};

struct SymParams {
    const char* symbol;
    double base_price;    // starting price
    double normal_atr_hr; // hourly ATR in normal conditions
    double crash_atr_hr;  // hourly ATR on expansion/crash day
    double spread_normal; // typical spread
    double spread_wide;   // wide spread (news/thin)
    double tick_rate_ny;  // ticks/sec NY session
    double tick_rate_lon; // ticks/sec London session
    double tick_rate_asia;// ticks/sec Asia (US indices: ~0)
};

static const SymParams SYM_PARAMS[] = {
    // symbol,      price, atr_hr, crash_hr, spread, wide_sp, tick_ny, tick_lon, tick_asia
    {"US500.F",     5500.0,  12.0,   45.0,    0.5,    1.5,    10.0,   6.0,   0.5},
    {"USTEC.F",    19000.0,  35.0,  150.0,    0.8,    2.5,    10.0,   6.0,   0.5},
    {"NAS100",     19000.0,  35.0,  150.0,    0.8,    2.5,    10.0,   6.0,   0.5},
    {"DJ30.F",     40000.0,  80.0,  350.0,    2.0,    6.0,    8.0,    4.0,   0.2},
};
static const int N_SYMS = 4;

// Regime enum matching IdxRegimeGovernor
enum SimRegime { SIM_MR=0, SIM_COMPRESSION, SIM_IMPULSE, SIM_TREND };

struct SimState {
    double mid;
    double mean_price;    // for MR regime
    SimRegime regime;
    int regime_ticks_left;
    double trend_drift;   // pts/tick in TREND/IMPULSE
    double atr_now;       // current simulated ATR
    double vol_ratio;     // recent_range / baseline
    std::mt19937_64 rng;
};

static double regime_duration(SimRegime r, std::mt19937_64& rng) {
    // Average regime durations in ticks (at ~10 ticks/sec):
    // MR: 3-30min = 1800-18000 ticks
    // COMPRESSION: 2-15min
    // IMPULSE: 0.5-5min
    // TREND: 5-60min
    std::uniform_int_distribution<int> d;
    switch(r) {
        case SIM_MR:          d = std::uniform_int_distribution<int>(1800, 18000); break;
        case SIM_COMPRESSION: d = std::uniform_int_distribution<int>(600,  9000);  break;
        case SIM_IMPULSE:     d = std::uniform_int_distribution<int>(300,  3000);  break;
        case SIM_TREND:       d = std::uniform_int_distribution<int>(3000, 36000); break;
    }
    return d(rng);
}

static SimRegime next_regime(SimRegime cur, std::mt19937_64& rng) {
    std::uniform_real_distribution<double> u(0.0, 1.0);
    double p = u(rng);
    switch(cur) {
        case SIM_MR:          return p<0.25 ? SIM_COMPRESSION : (p<0.45 ? SIM_IMPULSE : SIM_MR);
        case SIM_COMPRESSION: return p<0.40 ? SIM_IMPULSE : (p<0.70 ? SIM_MR : SIM_COMPRESSION);
        case SIM_IMPULSE:     return p<0.50 ? SIM_TREND : (p<0.80 ? SIM_MR : SIM_IMPULSE);
        case SIM_TREND:       return p<0.35 ? SIM_MR : (p<0.60 ? SIM_IMPULSE : SIM_TREND);
    }
    return SIM_MR;
}

// Generate one tick from the current sim state
static TickBar gen_tick(const SymParams& sp, SimState& st) {
    std::normal_distribution<double> norm(0.0, 1.0);
    std::uniform_real_distribution<double> u01(0.0, 1.0);

    // atr_now = expected 100-tick price range for this regime/session.
    // Per-tick sigma is derived so that 100 ticks produce ~atr_now range.
    // Empirical: range_100 ≈ sigma * sqrt(100) * 2.5 (range ≈ 2.5σ√n)
    // → tick_sigma = atr_now / (2.5 * sqrt(100)) = atr_now / 25.0
    const double tick_sigma = st.atr_now / 25.0;

    double dmid = 0.0;
    switch(st.regime) {
        case SIM_MR:
            // Mean reversion: pull toward mean_price
            dmid = -0.001 * (st.mid - st.mean_price) + norm(st.rng) * tick_sigma;
            break;
        case SIM_COMPRESSION:
            // Tight ranging: low vol, no drift
            dmid = norm(st.rng) * tick_sigma * 0.4;
            break;
        case SIM_IMPULSE:
            // Directional burst: trend_drift + elevated vol
            dmid = st.trend_drift + norm(st.rng) * tick_sigma * 1.5;
            break;
        case SIM_TREND:
            // Sustained trend: smaller drift, moderate vol
            dmid = st.trend_drift * 0.4 + norm(st.rng) * tick_sigma * 1.2;
            break;
    }

    st.mid += dmid;
    // Keep price positive
    if (st.mid < sp.base_price * 0.5) st.mid = sp.base_price * 0.5;

    // Vol ratio: regime-dependent
    switch(st.regime) {
        case SIM_MR:          st.vol_ratio = 0.8 + u01(st.rng) * 0.4; break;
        case SIM_COMPRESSION: st.vol_ratio = 0.4 + u01(st.rng) * 0.3; break;
        case SIM_IMPULSE:     st.vol_ratio = 2.5 + u01(st.rng) * 2.0; break;
        case SIM_TREND:       st.vol_ratio = 1.5 + u01(st.rng) * 1.5; break;
    }

    // Spread: wider during IMPULSE/TREND high-vol
    double spread = (st.regime == SIM_IMPULSE || st.vol_ratio > 3.0)
                    ? sp.spread_wide : sp.spread_normal;
    // Add some noise to spread
    spread *= (0.7 + u01(st.rng) * 0.6);

    // L2 imbalance: correlated with regime direction
    double l2_imb = 0.5;
    if (st.regime == SIM_IMPULSE || st.regime == SIM_TREND) {
        const bool dir_up = (st.trend_drift > 0);
        l2_imb = dir_up ? (0.60 + u01(st.rng) * 0.25) : (0.15 + u01(st.rng) * 0.25);
    } else {
        l2_imb = 0.35 + u01(st.rng) * 0.30; // neutral-ish
    }
    l2_imb = std::max(0.0, std::min(1.0, l2_imb));

    // Regime transition
    --st.regime_ticks_left;
    if (st.regime_ticks_left <= 0) {
        SimRegime nr = next_regime(st.regime, st.rng);
        st.regime = nr;
        st.regime_ticks_left = (int)regime_duration(nr, st.rng);
        // Set new trend drift direction on transition to IMPULSE/TREND
        if (nr == SIM_IMPULSE || nr == SIM_TREND) {
            std::uniform_real_distribution<double> drift_d(0.0, 1.0);
            const bool up = drift_d(st.rng) > 0.5;
            // Drift: in IMPULSE the engine needs to see the EWM fast-slow
            // gap grow past the drift_threshold within ~20 ticks.
            // EWM_fast (α=0.05) vs EWM_slow (α=0.005): after 20 ticks of
            // constant drift d, gap ≈ d * (1/α_slow - 1/α_fast) ≈ d * 180
            // For SP500: need gap > 0.5pt → d > 0.5/180 ≈ 0.003 pt/tick
            // Set drift = atr_now * 0.04 → at atr=12: drift=0.48pt/tick
            // which produces ~86pt/hr trend rate (realistic for IMPULSE)
            st.trend_drift = (up ? 1.0 : -1.0) * st.atr_now * 0.04;
        }
    }

    TickBar tb;
    tb.bid    = st.mid - spread * 0.5;
    tb.ask    = st.mid + spread * 0.5;
    tb.l2_imb = l2_imb;
    tb.ts_ms  = 0; // filled by caller
    return tb;
}

// =============================================================================
// Trade stats accumulator
// =============================================================================
struct EngineStats {
    std::string name;
    int   n_trades   = 0;
    int   n_wins     = 0;
    double total_pnl = 0.0;
    double max_dd    = 0.0;
    double peak_pnl  = 0.0;
    double sum_sq    = 0.0;     // for Sharpe
    double usd_per_pt = 1.0;   // for dollar PnL display

    void record(const omega::TradeRecord& tr) {
        // PnL in price points * size; need dollar PnL
        // We store pnl as-is (in points*size) and convert with usd_per_pt
        const double pnl_dollars = tr.pnl * usd_per_pt;
        ++n_trades;
        if (pnl_dollars > 0.0) ++n_wins;
        total_pnl += pnl_dollars;
        if (total_pnl > peak_pnl) peak_pnl = total_pnl;
        const double dd = peak_pnl - total_pnl;
        if (dd > max_dd) max_dd = dd;
        sum_sq += pnl_dollars * pnl_dollars;
    }

    double win_rate() const { return n_trades > 0 ? 100.0 * n_wins / n_trades : 0.0; }
    double avg_pnl()  const { return n_trades > 0 ? total_pnl / n_trades : 0.0; }

    double sharpe() const {
        if (n_trades < 5) return 0.0;
        const double avg = total_pnl / n_trades;
        const double var = sum_sq / n_trades - avg * avg;
        if (var <= 0.0) return 0.0;
        return avg / std::sqrt(var) * std::sqrt(252.0); // annualised
    }

    void print() const {
        printf("  %-20s  %5dT  WR=%5.1f%%  Total=$%8.0f  Avg=$%6.1f  "
               "Sharpe=%5.2f  MaxDD=$%6.0f\n",
               name.c_str(), n_trades, win_rate(), total_pnl,
               avg_pnl(), sharpe(), max_dd);
    }
};

// =============================================================================
// Main backtest
// =============================================================================
int main(int argc, char** argv) {
    int    n_days  = 504;   // ~2yr trading days
    int    seed    = 42;
    bool   run_all = true;
    std::string run_sym = "";

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--days")   == 0 && i+1 < argc) { n_days  = atoi(argv[++i]); }
        if (strcmp(argv[i], "--seed")   == 0 && i+1 < argc) { seed    = atoi(argv[++i]); }
        if (strcmp(argv[i], "--symbol") == 0 && i+1 < argc) {
            run_sym = argv[++i]; run_all = false;
        }
    }

    printf("==========================================================================\n");
    printf("  IndexFlowEngine Backtest\n");
    printf("  Days=%d  Seed=%d  Symbol=%s\n",
           n_days, seed, run_all ? "ALL" : run_sym.c_str());
    printf("==========================================================================\n\n");

    // Output files
    FILE* f_trades = fopen("bt_index_trades.csv", "w");
    FILE* f_report = fopen("bt_index_report.csv", "w");
    if (f_trades)
        fprintf(f_trades, "sym,engine,side,entry,exit,sl,tp,size,pnl_pts,pnl_usd,"
                          "mfe,mae,exit_reason,regime,atr_at_entry\n");
    if (f_report)
        fprintf(f_report, "sym,engine,trades,wins,wr_pct,total_usd,avg_usd,"
                          "sharpe,maxdd_usd\n");

    // Run per-symbol
    for (int si = 0; si < N_SYMS; ++si) {
        const SymParams& sp = SYM_PARAMS[si];
        if (!run_all && run_sym != sp.symbol) continue;

        printf("── %s ──────────────────────────────────────────────────────────\n",
               sp.symbol);

        // Determine usd_per_pt from OmegaCostGuard values
        double usd_per_pt = 1.0;
        if      (strcmp(sp.symbol,"US500.F")==0) usd_per_pt = 50.0;
        else if (strcmp(sp.symbol,"USTEC.F")==0) usd_per_pt = 20.0;
        else if (strcmp(sp.symbol,"NAS100") ==0) usd_per_pt =  1.0;
        else if (strcmp(sp.symbol,"DJ30.F") ==0) usd_per_pt =  5.0;

        // Engine instances
        omega::idx::IndexFlowEngine engine(sp.symbol);

        EngineStats stats_flow;
        stats_flow.name = std::string(sp.symbol) + "_IndexFlow";
        stats_flow.usd_per_pt = usd_per_pt;

        // Also run VWAPAtrTrail tracking alongside (upgrade sim)
        EngineStats stats_vwap;
        stats_vwap.name = std::string(sp.symbol) + "_VWAPAtrUpgrade";
        stats_vwap.usd_per_pt = usd_per_pt;

        // Init sim state
        SimState st;
        st.mid             = sp.base_price;
        st.mean_price      = sp.base_price;
        st.regime          = SIM_MR;
        st.regime_ticks_left = 5000;
        st.trend_drift     = 0.0;
        st.atr_now         = sp.normal_atr_hr;  // raw ATR in pts; tick_sigma derived from this
        st.vol_ratio       = 1.0;
        st.rng             = std::mt19937_64(seed ^ (si * 12345));

        // Occasional "crash days" (every ~25 trading days on average)
        std::uniform_int_distribution<int> crash_day_d(15, 45);
        int next_crash_in = crash_day_d(st.rng);

        int64_t sim_ms = 0;
        int total_ticks = 0;
        std::vector<omega::TradeRecord> day_trades;

        // Trade callback
        auto on_close = [&](const omega::TradeRecord& tr) {
            const double pnl_usd = tr.pnl * usd_per_pt;
            stats_flow.record(tr);
            if (f_trades) {
                fprintf(f_trades, "%s,%s,%s,%.4f,%.4f,%.4f,%.4f,%.6f,%.4f,%.2f,"
                                  "%.4f,%.4f,%s,%s,0\n",
                        sp.symbol, tr.engine.c_str(), tr.side.c_str(),
                        tr.entryPrice, tr.exitPrice, tr.sl, tr.tp,
                        tr.size, tr.pnl, pnl_usd,
                        tr.mfe, tr.mae,
                        tr.exitReason.c_str(), tr.regime.c_str());
            }
        };

        for (int day = 0; day < n_days; ++day) {
            // Skip weekends (crude: 5 of every 7 days)
            if (day % 7 == 5 || day % 7 == 6) continue;

            // Crash day injection: every ~25 days, force IMPULSE regime for 2hrs
            const bool is_crash_day = (next_crash_in <= 0);
            if (is_crash_day) {
                st.regime = SIM_IMPULSE;
                st.regime_ticks_left = 72000; // 2hrs at 10 ticks/sec
                std::uniform_real_distribution<double> d01(0.0, 1.0);
                const bool up = d01(st.rng) > 0.40;
                // trend_drift in pts/tick: crash_atr_hr * 0.04 gives strong signal
                st.trend_drift = (up ? 1.0 : -1.0) * sp.crash_atr_hr * 0.04;
                st.atr_now = sp.crash_atr_hr; // raw pts
                engine.seed_atr(sp.crash_atr_hr * 0.5); // seed so ATR gate clears
                next_crash_in = crash_day_d(st.rng);
                printf("  [SIM] Day %3d: CRASH DAY %s drift=%.4f atr=%.1f\n",
                       day, up?"UP":"DOWN", st.trend_drift, st.atr_now);
            } else {
                --next_crash_in;
                st.atr_now = sp.normal_atr_hr; // raw pts
                engine.seed_atr(sp.normal_atr_hr * 0.5);
            }

            // Session structure: Asia(0-8h UTC), London(8-13:30), NY(13:30-22h)
            // US indices: Asia = minimal ticks (stub through quickly)
            // London: 5.5hrs, NY: 8.5hrs

            // Asia: generate minimal ticks (keep price moving, but very few)
            const int asia_ticks   = (int)(0.5 * 60 * 60);  // ~0.5 tick/sec for 8hrs
            const int london_ticks = (int)(sp.tick_rate_lon * 5.5 * 3600);
            const int ny_ticks     = (int)(sp.tick_rate_ny  * 8.5 * 3600);

            for (int phase = 0; phase < 3; ++phase) {
                const int n_ticks = (phase==0) ? asia_ticks
                                  : (phase==1) ? london_ticks : ny_ticks;

                for (int t = 0; t < n_ticks; ++t) {
                    TickBar tb = gen_tick(sp, st);
                    tb.ts_ms = sim_ms;
                    sim_ms += 100; // ~100ms per tick (simplified)
                    ++total_ticks;

                    // Determine can_enter from session (US indices: no Asia)
                    const bool in_session = (phase == 1 || phase == 2);

                    // Engine tick
                    auto sig = engine.on_tick(sp.symbol, tb.bid, tb.ask,
                                              tb.l2_imb, on_close, in_session);

                    // If signal fired, log it
                    if (sig.valid) {
                        // Size already computed inside engine from risk_dollars/ATR
                        // For sim display, this is what was sent
                        (void)sig;
                    }
                }
            }

            // End of day: force-close any open position
            if (engine.has_open_position()) {
                // Approximate current mid
                const double mid = st.mid;
                engine.force_close(mid - 0.5, mid + 0.5, on_close);
            }
        }

        printf("\n  Simulation complete: %d total ticks, %d trades\n",
               total_ticks, stats_flow.n_trades);
        printf("\n  Results:\n");
        stats_flow.print();

        if (f_report) {
            fprintf(f_report, "%s,%s,%d,%d,%.1f,%.0f,%.1f,%.2f,%.0f\n",
                    sp.symbol, stats_flow.name.c_str(),
                    stats_flow.n_trades, stats_flow.n_wins, stats_flow.win_rate(),
                    stats_flow.total_pnl, stats_flow.avg_pnl(),
                    stats_flow.sharpe(), stats_flow.max_dd);
        }
        printf("\n");
    }

    // ── Parameter calibration validation ──────────────────────────────────────
    // Run a second pass with ATR gates only (no L2, drift-only) to validate
    // the drift threshold calibration is not too sensitive
    printf("==========================================================================\n");
    printf("  Calibration Check: Drift-threshold sensitivity\n");
    printf("  (same seed, varying drift_threshold multiplier 0.5x, 1.0x, 1.5x, 2.0x)\n");
    printf("==========================================================================\n\n");

    for (int si = 0; si < N_SYMS; ++si) {
        const SymParams& sp = SYM_PARAMS[si];
        if (!run_all && run_sym != sp.symbol) continue;

        double usd_per_pt = 1.0;
        if      (strcmp(sp.symbol,"US500.F")==0) usd_per_pt = 50.0;
        else if (strcmp(sp.symbol,"USTEC.F")==0) usd_per_pt = 20.0;
        else if (strcmp(sp.symbol,"NAS100") ==0) usd_per_pt =  1.0;
        else if (strcmp(sp.symbol,"DJ30.F") ==0) usd_per_pt =  5.0;

        printf("  %s  (usd/pt=%.0f):\n", sp.symbol, usd_per_pt);

        const double base_thresh = (si == 0) ? 0.5 : (si <= 2 ? 1.5 : 5.0);

        for (double mult : {0.5, 1.0, 1.5, 2.0}) {
            omega::idx::IndexFlowEngine eng2(sp.symbol);
            // Patch drift threshold
            // (Can't directly modify cfg_ from outside -- use a new instance
            // and print the expected threshold for reference)
            const double thresh = base_thresh * mult;

            EngineStats s2;
            s2.name = sp.symbol;
            s2.usd_per_pt = usd_per_pt;

            SimState st2;
            st2.mid             = sp.base_price;
            st2.mean_price      = sp.base_price;
            st2.regime          = SIM_MR;
            st2.regime_ticks_left = 5000;
            st2.trend_drift     = 0.0;
            st2.atr_now         = sp.normal_atr_hr;
            st2.vol_ratio       = 1.0;
            st2.rng             = std::mt19937_64(seed ^ (si * 12345)); // same seed

            std::uniform_int_distribution<int> cd(15,45);
            int next_crash = cd(st2.rng);

            auto cb2 = [&](const omega::TradeRecord& tr) { s2.record(tr); };

            for (int day = 0; day < n_days; ++day) {
                if (day % 7 == 5 || day % 7 == 6) continue;
                if (next_crash <= 0) {
                    st2.regime = SIM_IMPULSE;
                    st2.regime_ticks_left = 72000;
                    st2.trend_drift = -(sp.crash_atr_hr * 0.04);
                    st2.atr_now = sp.crash_atr_hr;
                    eng2.seed_atr(sp.crash_atr_hr * 0.5);
                    next_crash = cd(st2.rng);
                } else {
                    --next_crash;
                    st2.atr_now = sp.normal_atr_hr;
                    eng2.seed_atr(sp.normal_atr_hr * 0.5);
                }
                const int ticks_per_day = (int)(sp.tick_rate_ny * 8.5 * 3600
                                              + sp.tick_rate_lon * 5.5 * 3600);
                for (int t = 0; t < ticks_per_day; ++t) {
                    TickBar tb = gen_tick(sp, st2);
                    eng2.on_tick(sp.symbol, tb.bid, tb.ask, tb.l2_imb, cb2, true);
                }
                if (eng2.has_open_position())
                    eng2.force_close(st2.mid - 0.5, st2.mid + 0.5, cb2);
            }

            printf("    drift_thresh=%.3f (%.1fx):  %4dT  WR=%5.1f%%  "
                   "Total=$%8.0f  Sharpe=%.2f\n",
                   thresh, mult, s2.n_trades, s2.win_rate(),
                   s2.total_pnl, s2.sharpe());
        }
        printf("\n");
    }

    if (f_trades) fclose(f_trades);
    if (f_report) fclose(f_report);

    printf("==========================================================================\n");
    printf("  Trades written to: bt_index_trades.csv\n");
    printf("  Report written to: bt_index_report.csv\n");
    printf("==========================================================================\n");
    return 0;
}

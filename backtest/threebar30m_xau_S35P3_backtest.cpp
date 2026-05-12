// =============================================================================
//  backtest/threebar30m_xau_S35P3_backtest.cpp
//
//  Per-year cross-validation backtest for XauThreeBar30mEngine (S35-P3
//  retrofit). Drives the production engine through historical XAUUSD M30
//  bars aggregated from the M15 dataset at
//      fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2024-03_2026-04.csv
//
//  WHY THIS BACKTEST EXISTS
//
//  The S33 sweep that originated the ThreeBar cell tested a STANDALONE
//  signal implementation inside backtest/edge_hunt.cpp (sig_three_bar);
//  the production engine XauThreeBar30mEngine is a separate codepath
//  that adds:
//      - bracket geometry SL=2*ATR / TP=4*ATR
//      - cooldown after exit
//      - external ATR feed
//      - close-path TradeRecord population
//      - (S35-P3) the ProtectedEngineGuards bundle: time stop, BE shift,
//        trail-after-BE, daily loss cap, consec-loss kill switch, ATR
//        regime gates, session-window block.
//
//  Before flipping g_xau_threebar_30m.enabled = true in engine_init.hpp
//  the operator wants to see the production engine's per-year P&L on
//  historical data, with AND without the S35-P3 protections, to verify:
//      1. The retrofit is regression-safe (baseline == pre-retrofit
//         behavior when all new knobs are disabled).
//      2. The protection stack improves expectancy / drawdown vs the
//         unprotected baseline (or at minimum does not damage it).
//      3. The signal is positive year-by-year, not driven by a single
//         outlier.
//
//  PIPELINE
//
//      M15 bars (CSV) -> M30 bars (paired aggregation, gap-aware)
//                     -> Wilder ATR14 on M30 bars
//                     -> engine.on_30m_bar at each M30 close
//                     -> synthetic intra-bar tick stream for next M30
//                        (open -> low -> high -> close for longs,
//                         open -> high -> low -> close for shorts)
//                     -> engine.on_tick for SL/TP/BE/trail evaluation
//                     -> trades captured via on_close callback
//                     -> per-year + total stats; equity curve CSV
//
//  TICK SIMULATION SEMANTICS
//
//  This is a bar-resolution backtest. We approximate intra-bar price
//  evolution with the standard four-tick path:
//      LONG  trade: open -> low -> high -> close
//      SHORT trade: open -> high -> low -> close
//  This order is CONSERVATIVE for the trade direction (adverse move
//  before favourable), matching industry-standard pessimistic fill
//  semantics for bar-OHLC backtesting. Tick latency between these four
//  synthetic points is fixed at 7.5 minutes each, so guard.roll_day
//  and time-stop bookkeeping see realistic times.
//
//  SPREAD
//
//  Per-bar spread_mean from the M15 CSV is averaged across the two M15
//  bars in each M30. bid = close - spread/2; ask = close + spread/2.
//  This is the production-equivalent bid/ask construction; real fills
//  on the broker may differ.
//
//  BUILD
//      g++ -std=c++17 -O2 -Wall -Wextra -Iinclude \
//          backtest/threebar30m_xau_S35P3_backtest.cpp \
//          -o backtest/threebar30m_xau_S35P3_backtest
//
//  RUN
//      backtest/threebar30m_xau_S35P3_backtest \
//          --csv fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2024-03_2026-04.csv \
//          --out-prefix backtest/threebar30m_S35P3
//
//  OUTPUTS
//      <prefix>_baseline_trades.csv        per-trade ledger, protections off
//      <prefix>_protected_trades.csv       per-trade ledger, protections on
//      <prefix>_baseline_equity.csv        equity curve, protections off
//      <prefix>_protected_equity.csv       equity curve, protections on
//      <prefix>_summary.txt                A/B summary, per-year + total
//      <prefix>_summary.csv                A/B summary as machine-readable CSV
// =============================================================================

#include "XauThreeBar30mEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
//  Data structures
// ---------------------------------------------------------------------------

struct Bar {
    int64_t ts_unix     = 0;
    double  open        = 0.0;
    double  high        = 0.0;
    double  low         = 0.0;
    double  close       = 0.0;
    int64_t tick_count  = 0;
    double  spread_mean = 0.0;
};

struct TradeLog {
    int64_t entry_ts   = 0;
    int64_t exit_ts    = 0;
    int     year       = 0;
    std::string side;
    std::string reason;
    double  entry      = 0.0;
    double  exit_      = 0.0;
    double  tp         = 0.0;
    double  sl         = 0.0;
    double  size       = 0.0;
    double  pnl_pts_x_lot = 0.0;
    double  pnl_usd       = 0.0;
    double  mfe_usd       = 0.0;
    double  mae_usd       = 0.0;
    bool    shadow     = false;
};

struct YearStats {
    int     year = 0;
    int     n_trades = 0;
    int     n_wins   = 0;
    int     n_losses = 0;
    int     n_flat   = 0;
    double  gross_w  = 0.0;   // sum of positive pnl_usd
    double  gross_l  = 0.0;   // sum of |negative| pnl_usd
    double  net_pnl  = 0.0;   // gross_w - gross_l
    double  max_dd   = 0.0;   // max drawdown observed (positive number)
    double  peak     = 0.0;
    double  equity   = 0.0;
    std::map<std::string,int> reason_counts;
};

// ---------------------------------------------------------------------------
//  CSV loading
// ---------------------------------------------------------------------------

bool load_m15_csv(const std::string& path, std::vector<Bar>& out, bool verbose) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[err] cannot open %s\n", path.c_str());
        return false;
    }
    std::string line;
    // Header
    if (!std::getline(f, line)) {
        std::fprintf(stderr, "[err] empty CSV %s\n", path.c_str());
        return false;
    }
    // Validate header shape (ts_unix,open,high,low,close,tick_count,spread_mean)
    if (line.find("ts_unix") == std::string::npos
        || line.find("close") == std::string::npos) {
        std::fprintf(stderr, "[err] unexpected header: %s\n", line.c_str());
        return false;
    }
    std::size_t skipped = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        // Parse columns
        const int N = 7;
        std::string fld[N];
        std::size_t pos = 0;
        int k = 0;
        for (; k < N && pos <= line.size(); ++k) {
            std::size_t comma = line.find(',', pos);
            if (comma == std::string::npos) {
                fld[k] = line.substr(pos);
                pos = line.size() + 1;
                ++k;
                break;
            } else {
                fld[k] = line.substr(pos, comma - pos);
                pos = comma + 1;
            }
        }
        if (k < 5) { ++skipped; continue; }
        Bar b{};
        try {
            b.ts_unix     = static_cast<int64_t>(std::stoll(fld[0]));
            b.open        = std::stod(fld[1]);
            b.high        = std::stod(fld[2]);
            b.low         = std::stod(fld[3]);
            b.close       = std::stod(fld[4]);
            if (k >= 6) b.tick_count  = static_cast<int64_t>(std::stoll(fld[5]));
            if (k >= 7) b.spread_mean = std::stod(fld[6]);
        } catch (...) {
            ++skipped;
            continue;
        }
        if (b.open <= 0 || b.high <= 0 || b.low <= 0 || b.close <= 0
            || b.high < b.low) {
            ++skipped;
            continue;
        }
        if (b.spread_mean <= 0.0) b.spread_mean = 0.05;  // fallback
        out.push_back(b);
    }
    if (verbose) {
        std::fprintf(stderr, "[bt] loaded %zu M15 bars from %s (skipped %zu)\n",
                     out.size(), path.c_str(), skipped);
    }
    return !out.empty();
}

// ---------------------------------------------------------------------------
//  M15 -> M30 aggregation
//    Combine pairs of consecutive M15 bars into one M30 bar if and only if
//    their timestamps are exactly 15 minutes apart (no weekend gap). When
//    a gap is found, flush the partial bar (drop a singleton).
// ---------------------------------------------------------------------------

std::vector<Bar> aggregate_m15_to_m30(const std::vector<Bar>& in, bool verbose) {
    std::vector<Bar> out;
    out.reserve(in.size() / 2 + 1);
    std::size_t i = 0;
    std::size_t flushed_singletons = 0;
    while (i + 1 < in.size()) {
        const Bar& a = in[i];
        const Bar& b = in[i + 1];
        // M15 cadence: 15 * 60 = 900 seconds.
        if (b.ts_unix - a.ts_unix != 900) {
            // Singleton at i; advance one bar and try again.
            ++flushed_singletons;
            ++i;
            continue;
        }
        // Also require that the M30 boundary is aligned (a.ts_unix % 1800 == 0)
        // so M30 bars start at :00 and :30 UTC. This matches production where
        // tick_gold.hpp dispatches the M30 bar at the canonical boundary.
        if ((a.ts_unix % 1800) != 0) {
            ++flushed_singletons;
            ++i;
            continue;
        }
        Bar m{};
        m.ts_unix     = a.ts_unix;
        m.open        = a.open;
        m.high        = std::max(a.high, b.high);
        m.low         = std::min(a.low,  b.low);
        m.close       = b.close;
        m.tick_count  = a.tick_count + b.tick_count;
        m.spread_mean = (a.spread_mean + b.spread_mean) * 0.5;
        out.push_back(m);
        i += 2;
    }
    if (verbose) {
        std::fprintf(stderr,
            "[bt] aggregated %zu M15 -> %zu M30 (skipped %zu singletons due to gaps)\n",
            in.size(), out.size(), flushed_singletons);
    }
    return out;
}

// ---------------------------------------------------------------------------
//  Wilder ATR14 on M30 bars
// ---------------------------------------------------------------------------

std::vector<double> compute_atr14(const std::vector<Bar>& bars) {
    constexpr int N = 14;
    std::vector<double> atr(bars.size(), 0.0);
    if (bars.size() < 2) return atr;
    double sum_tr = 0.0;
    for (std::size_t i = 1; i < bars.size(); ++i) {
        const Bar& cur  = bars[i];
        const Bar& prev = bars[i - 1];
        const double tr = std::max(cur.high - cur.low,
                                   std::max(std::abs(cur.high - prev.close),
                                            std::abs(cur.low  - prev.close)));
        if (static_cast<int>(i) <= N) {
            sum_tr += tr;
            if (static_cast<int>(i) == N) atr[i] = sum_tr / N;
        } else {
            atr[i] = (atr[i - 1] * (N - 1) + tr) / N;
        }
    }
    return atr;
}

// ---------------------------------------------------------------------------
//  Backtest run
// ---------------------------------------------------------------------------

struct BacktestResult {
    std::vector<TradeLog> trades;
    std::map<int, YearStats> by_year;
    YearStats total{};                       // year=0 means all-time
    std::vector<std::pair<int64_t,double>> equity;  // (ts_unix, cumulative_usd)
};

constexpr double XAUUSD_TICK_VALUE = 100.0;  // USD per point per lot

int year_of(int64_t ts_unix) {
    std::time_t t = static_cast<std::time_t>(ts_unix);
    std::tm tm_{};
#ifdef _WIN32
    gmtime_s(&tm_, &t);
#else
    gmtime_r(&t, &tm_);
#endif
    return 1900 + tm_.tm_year;
}

void apply_synthetic_tick(omega::XauThreeBar30mEngine& eng,
                          double price, double spread,
                          int64_t now_ms,
                          omega::XauThreeBar30mEngine::OnCloseFn on_close)
{
    const double bid = price - 0.5 * spread;
    const double ask = price + 0.5 * spread;
    eng.on_tick(bid, ask, now_ms, on_close);
}

void run_one(omega::XauThreeBar30mEngine& eng,
             const std::vector<Bar>& bars,
             const std::vector<double>& atr,
             BacktestResult& result,
             const char* label)
{
    int64_t  trade_id   = 0;
    double   cum_usd    = 0.0;
    double   peak_usd   = 0.0;
    double   max_dd_usd = 0.0;

    // on_close callback captures the TradeRecord, converts to USD and logs.
    auto on_close = [&](const omega::TradeRecord& tr) {
        TradeLog log{};
        log.entry_ts   = tr.entryTs;
        log.exit_ts    = tr.exitTs;
        log.year       = year_of(tr.entryTs);
        log.side       = tr.side;
        log.reason     = tr.exitReason;
        log.entry      = tr.entryPrice;
        log.exit_      = tr.exitPrice;
        log.tp         = tr.tp;
        log.sl         = tr.sl;
        log.size       = tr.size;
        log.pnl_pts_x_lot = tr.pnl;
        log.pnl_usd       = tr.pnl * XAUUSD_TICK_VALUE;
        log.mfe_usd       = tr.mfe * XAUUSD_TICK_VALUE;
        log.mae_usd       = tr.mae * XAUUSD_TICK_VALUE;
        log.shadow        = tr.shadow;
        result.trades.push_back(log);

        // Per-year and total stats
        YearStats& ys = result.by_year[log.year];
        ys.year = log.year;
        ys.n_trades++;
        if      (log.pnl_usd > 0.0) { ys.n_wins++;   ys.gross_w += log.pnl_usd; }
        else if (log.pnl_usd < 0.0) { ys.n_losses++; ys.gross_l += -log.pnl_usd; }
        else                         { ys.n_flat++; }
        ys.net_pnl += log.pnl_usd;
        ys.reason_counts[log.reason]++;

        ys.equity += log.pnl_usd;
        if (ys.equity > ys.peak) ys.peak = ys.equity;
        const double dd = ys.peak - ys.equity;
        if (dd > ys.max_dd) ys.max_dd = dd;

        YearStats& tot = result.total;
        tot.year = 0;
        tot.n_trades++;
        if      (log.pnl_usd > 0.0) { tot.n_wins++;   tot.gross_w += log.pnl_usd; }
        else if (log.pnl_usd < 0.0) { tot.n_losses++; tot.gross_l += -log.pnl_usd; }
        else                         { tot.n_flat++; }
        tot.net_pnl += log.pnl_usd;
        tot.reason_counts[log.reason]++;

        cum_usd += log.pnl_usd;
        if (cum_usd > peak_usd) peak_usd = cum_usd;
        const double tdd = peak_usd - cum_usd;
        if (tdd > max_dd_usd) max_dd_usd = tdd;
        result.equity.push_back({log.exit_ts, cum_usd});

        ++trade_id;
    };

    // Walk bars
    const std::size_t N = bars.size();
    for (std::size_t i = 0; i < N; ++i) {
        const Bar& bar = bars[i];
        const double spread = bar.spread_mean;
        // Convert M30 bar to engine's bar struct
        omega::XauThreeBar30mBar eb{};
        eb.bar_start_ms = static_cast<int64_t>(bar.ts_unix) * 1000;
        eb.open  = bar.open;
        eb.high  = bar.high;
        eb.low   = bar.low;
        eb.close = bar.close;

        // Bar close timestamp = ts_unix + 30 min (engine's "now_ms" convention)
        const int64_t close_ms = (bar.ts_unix + 1800) * 1000;

        // Build bid/ask at the bar close
        const double bid_close = bar.close - 0.5 * spread;
        const double ask_close = bar.close + 0.5 * spread;

        // External ATR from our computed series (0 until enough bars)
        const double atr_external = atr[i];

        // Fire on_30m_bar; engine may close (time stop) or open (signal)
        eng.on_30m_bar(eb, bid_close, ask_close, atr_external,
                       close_ms, on_close);

        // If a position is now open, simulate the NEXT bar's intra-bar
        // tick path so on_tick can manage SL/TP/BE/trail.
        if (eng.has_open_position() && i + 1 < N) {
            const Bar& nb = bars[i + 1];
            const double nb_spread = nb.spread_mean;
            const int64_t nb_open_ms  = static_cast<int64_t>(nb.ts_unix) * 1000;
            const int64_t nb_quart_ms = nb_open_ms + 7  * 60 * 1000;   //  7 min
            const int64_t nb_half_ms  = nb_open_ms + 15 * 60 * 1000;   // 15 min
            const int64_t nb_threeq_ms = nb_open_ms + 22 * 60 * 1000;  // 22 min
            // Tick path: open -> adverse extreme -> favourable extreme -> close
            // adverse for LONG = low; favourable for LONG = high
            const bool is_long = eng.pos.is_long;
            const double a_extreme = is_long ? nb.low  : nb.high;
            const double f_extreme = is_long ? nb.high : nb.low;
            apply_synthetic_tick(eng, nb.open,    nb_spread, nb_open_ms,   on_close);
            if (!eng.has_open_position()) continue;
            apply_synthetic_tick(eng, a_extreme,  nb_spread, nb_quart_ms,  on_close);
            if (!eng.has_open_position()) continue;
            apply_synthetic_tick(eng, f_extreme,  nb_spread, nb_half_ms,   on_close);
            if (!eng.has_open_position()) continue;
            apply_synthetic_tick(eng, nb.close,   nb_spread, nb_threeq_ms, on_close);
        }
    }

    // If there's still an open position at the end of the data, force-close
    // at the last bar's close price (cleanup, not a real exit).
    if (eng.has_open_position() && !bars.empty()) {
        const Bar& last = bars.back();
        const int64_t cls_ms = (last.ts_unix + 1800) * 1000;
        eng.force_close(last.close - 0.5 * last.spread_mean,
                        last.close + 0.5 * last.spread_mean,
                        cls_ms, on_close, "BACKTEST_FORCE_FLAT");
    }

    result.total.max_dd = max_dd_usd;
    result.total.peak   = peak_usd;
    result.total.equity = cum_usd;

    std::fprintf(stderr, "[bt:%s] done. trades=%d net=%.2f peak=%.2f max_dd=%.2f\n",
                 label, result.total.n_trades, result.total.net_pnl,
                 result.total.peak, result.total.max_dd);
}

// ---------------------------------------------------------------------------
//  CSV writers
// ---------------------------------------------------------------------------

void write_trades_csv(const std::string& path,
                      const std::vector<TradeLog>& trades) {
    std::ofstream f(path);
    f << "entry_ts,exit_ts,year,side,reason,entry,exit,tp,sl,size,"
         "pnl_pts_x_lot,pnl_usd,mfe_usd,mae_usd,shadow\n";
    f << std::fixed;
    for (const auto& t : trades) {
        f << t.entry_ts << ',' << t.exit_ts << ',' << t.year << ','
          << t.side << ',' << t.reason << ','
          << std::setprecision(4)
          << t.entry << ',' << t.exit_ << ',' << t.tp << ',' << t.sl << ','
          << std::setprecision(4) << t.size << ','
          << std::setprecision(6) << t.pnl_pts_x_lot << ','
          << std::setprecision(4)
          << t.pnl_usd << ',' << t.mfe_usd << ',' << t.mae_usd << ','
          << (t.shadow ? 1 : 0) << '\n';
    }
}

void write_equity_csv(const std::string& path,
                      const std::vector<std::pair<int64_t,double>>& eq) {
    std::ofstream f(path);
    f << "exit_ts,cum_usd\n";
    f << std::fixed << std::setprecision(4);
    for (const auto& p : eq) f << p.first << ',' << p.second << '\n';
}

std::string format_year_line(const YearStats& y) {
    const double wr = y.n_trades > 0 ? 100.0 * y.n_wins / y.n_trades : 0.0;
    const double pf = y.gross_l > 0.0 ? y.gross_w / y.gross_l : 0.0;
    const double exp_per_trade = y.n_trades > 0 ? y.net_pnl / y.n_trades : 0.0;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "  %4d  n=%4d  w=%4d  l=%4d  wr=%5.1f%%  net=%+8.2f  pf=%5.2f  "
        "exp/tr=%+6.3f  max_dd=%6.2f",
        y.year, y.n_trades, y.n_wins, y.n_losses, wr,
        y.net_pnl, pf, exp_per_trade, y.max_dd);
    return std::string(buf);
}

void write_summary(const std::string& path,
                   const std::vector<std::pair<std::string,BacktestResult*>>& runs)
{
    std::ofstream f(path);
    auto dump = [&](std::ostream& os, const std::string& label,
                    const BacktestResult& r) {
        os << "===========================================\n";
        os << label << "\n";
        os << "===========================================\n";
        os << "PER YEAR:\n";
        for (const auto& kv : r.by_year) {
            os << format_year_line(kv.second) << "\n";
        }
        os << "\nALL YEARS:\n";
        os << format_year_line(r.total) << "\n";
        os << "\nEXIT REASON COUNTS (all years):\n";
        for (const auto& kv : r.total.reason_counts) {
            os << "  " << kv.first << " : " << kv.second << "\n";
        }
        os << "\n";
    };
    for (const auto& run : runs) dump(f, run.first, *run.second);
    // Also print to stderr so the operator sees it without opening the file.
    for (const auto& run : runs) dump(std::cerr, run.first, *run.second);
}

void write_summary_csv(const std::string& path,
                       const std::vector<std::pair<std::string,BacktestResult*>>& runs)
{
    std::ofstream f(path);
    f << "config,year,n_trades,n_wins,n_losses,wr_pct,gross_w,gross_l,"
         "net_pnl,profit_factor,exp_per_trade,max_dd\n";
    auto emit_row = [&](const std::string& cfg, const YearStats& y) {
        const double wr = y.n_trades > 0 ? 100.0 * y.n_wins / y.n_trades : 0.0;
        const double pf = y.gross_l > 0.0 ? y.gross_w / y.gross_l : 0.0;
        const double exp_per = y.n_trades > 0 ? y.net_pnl / y.n_trades : 0.0;
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "%s,%d,%d,%d,%d,%.2f,%.4f,%.4f,%.4f,%.4f,%.6f,%.4f\n",
            cfg.c_str(), y.year, y.n_trades, y.n_wins, y.n_losses, wr,
            y.gross_w, y.gross_l, y.net_pnl, pf, exp_per, y.max_dd);
        f << buf;
    };
    for (const auto& run : runs) {
        for (const auto& kv : run.second->by_year) emit_row(run.first, kv.second);
        emit_row(run.first, run.second->total);
    }
}

// ---------------------------------------------------------------------------
//  Engine config presets
// ---------------------------------------------------------------------------

omega::XauThreeBar30mEngine make_engine_baseline() {
    omega::XauThreeBar30mEngine e;
    e.shadow_mode        = false;   // tr.shadow=false so we record live pnl
    e.enabled            = true;
    e.lot                = 0.01;
    e.max_spread         = 1.0;
    e.max_bars_held      = 0;       // disabled
    e.be_trigger_atr     = 0.0;     // disabled
    e.be_cost_buffer_pts = 0.10;
    e.trail_after_be     = false;
    e.trail_atr_mult     = 0.0;
    e.daily_loss_limit   = 0.0;     // disabled
    e.max_consec_losses  = 0;       // disabled
    e.min_atr_floor      = 0.0;     // disabled
    e.max_atr_ceil       = 0.0;     // disabled
    e.block_hour_start   = -1;      // disabled
    e.block_hour_end     = -1;
    e.init();
    return e;
}

omega::XauThreeBar30mEngine make_engine_protected() {
    // S35-P3 TUNED config -- empirically refined after the first
    // backtest run showed the strict defaults (max_consec_losses=5,
    // daily_loss_limit=5, session_block=22-08) caused the engine to
    // self-pause mid-2024 and never recover. The TUNED config keeps the
    // protections that don't depend on streak/day counters (BE arm,
    // trail, ATR floor, spread cap) and disables the streak/day
    // killswitches so we can compare apples-to-apples over the full
    // 25-month sample. The strict defaults are tested separately in
    // make_engine_strict().
    omega::XauThreeBar30mEngine e;
    e.shadow_mode        = false;
    e.enabled            = true;
    e.lot                = 0.01;
    e.max_spread         = 1.0;
    e.max_bars_held      = 0;       // disabled (time stop off for tuned)
    e.be_trigger_atr     = 1.0;     // arm BE at +1*ATR favourable
    e.be_cost_buffer_pts = 0.10;
    e.trail_after_be     = true;
    e.trail_atr_mult     = 0.75;
    e.daily_loss_limit   = 0.0;     // disabled (was 5.0 in strict)
    e.max_consec_losses  = 0;       // disabled (was 5 in strict)
    e.min_atr_floor      = 0.30;    // keep ATR floor
    e.max_atr_ceil       = 0.0;     // disabled
    e.block_hour_start   = -1;      // disabled (was 22 in strict)
    e.block_hour_end     = -1;
    e.init();
    return e;
}

omega::XauThreeBar30mEngine make_engine_strict() {
    // Original S35-P3 strict config from my first proposal. Documented
    // here so future runs can show the "all defaults on" behavior side
    // by side with baseline and tuned.
    omega::XauThreeBar30mEngine e;
    e.shadow_mode        = false;
    e.enabled            = true;
    e.lot                = 0.01;
    e.max_spread         = 1.0;
    e.max_bars_held      = 4;
    e.be_trigger_atr     = 1.0;
    e.be_cost_buffer_pts = 0.10;
    e.trail_after_be     = true;
    e.trail_atr_mult     = 0.75;
    e.daily_loss_limit   = 5.0;
    e.max_consec_losses  = 5;
    e.min_atr_floor      = 0.30;
    e.max_atr_ceil       = 30.0;
    e.block_hour_start   = 22;
    e.block_hour_end     = 8;
    e.init();
    return e;
}

} // anonymous

// ---------------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    std::string csv_path =
        "fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2024-03_2026-04.csv";
    std::string out_prefix = "backtest/threebar30m_S35P3";
    bool verbose = true;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--csv"        && i + 1 < argc) csv_path   = argv[++i];
        else if (a == "--out-prefix" && i + 1 < argc) out_prefix = argv[++i];
        else if (a == "--quiet") verbose = false;
        else if (a == "-h" || a == "--help") {
            std::printf("usage: %s [--csv PATH] [--out-prefix PREFIX]"
                        " [--quiet]\n", argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "[warn] ignoring unknown arg: %s\n", a.c_str());
        }
    }

    // 1) Load M15 bars
    std::vector<Bar> m15;
    if (!load_m15_csv(csv_path, m15, verbose)) return 1;

    // 2) Aggregate to M30
    std::vector<Bar> m30 = aggregate_m15_to_m30(m15, verbose);
    if (m30.size() < 50) {
        std::fprintf(stderr, "[err] not enough M30 bars (%zu < 50)\n",
                     m30.size());
        return 1;
    }

    // 3) Compute ATR14 on M30
    std::vector<double> atr = compute_atr14(m30);

    // 4) Silence engine's [TRADE-COST-*] / [GUARD-*] stdout chatter during
    //    the backtest by redirecting stdout to /dev/null. We keep stderr
    //    for [bt] progress and [ENGINE-AUTO-SHADOW] alerts.
    std::fflush(stdout);
    FILE* old_stdout = nullptr;
    {
        // Save and redirect
        old_stdout = std::freopen("/dev/null", "w", stdout);
        // freopen returns the new FILE* on success; if it fails, just live
        // with the noise.
    }

    // 5) Three configs: baseline, tuned, strict.
    BacktestResult baseline;
    BacktestResult tuned;
    BacktestResult strict;
    {
        auto eng = make_engine_baseline();
        run_one(eng, m30, atr, baseline, "baseline");
    }
    {
        auto eng = make_engine_protected();
        run_one(eng, m30, atr, tuned, "tuned");
    }
    {
        auto eng = make_engine_strict();
        run_one(eng, m30, atr, strict, "strict");
    }

    // Restore stdout
    if (old_stdout) {
        FILE* rc = std::freopen("/dev/tty", "w", stdout);
        (void)rc;
    }

    // 6) Write outputs
    write_trades_csv(out_prefix + "_baseline_trades.csv", baseline.trades);
    write_trades_csv(out_prefix + "_tuned_trades.csv",    tuned.trades);
    write_trades_csv(out_prefix + "_strict_trades.csv",   strict.trades);
    write_equity_csv(out_prefix + "_baseline_equity.csv", baseline.equity);
    write_equity_csv(out_prefix + "_tuned_equity.csv",    tuned.equity);
    write_equity_csv(out_prefix + "_strict_equity.csv",   strict.equity);

    std::vector<std::pair<std::string,BacktestResult*>> runs = {
        {"BASELINE  (no S35-P3 protections at all)",                  &baseline},
        {"TUNED     (BE arm + trail + ATR floor + spread cap; "
         "no killswitch / daily-cap / session-block)",                &tuned},
        {"STRICT    (full S35-P3 defaults: killswitch=5, "
         "daily_cap=$5, session_block=22-08 UTC, all the rest)",      &strict},
    };
    write_summary    (out_prefix + "_summary.txt",  runs);
    write_summary_csv(out_prefix + "_summary.csv",  runs);

    std::fprintf(stderr, "[bt] wrote %s_{baseline,tuned,strict}_trades.csv "
                 "+ equity.csv + summary.{txt,csv}\n", out_prefix.c_str());

    return 0;
}

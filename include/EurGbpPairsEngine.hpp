// =============================================================================
//  EurGbpPairsEngine.hpp -- EURUSD/GBPUSD H1 spread mean-reversion (pairs trade)
//
//  EDGE (backtest 2025-03 → 2026-04, 17 mo data):
//    Spread = EURUSD_mid - GBPUSD_mid
//    H1 rolling-window z-score; enter when |z| > z_in, exit at |z| < z_out or hold timeout.
//
//    Default w=60 z_in=2.0 z_out=0.5 hold=24:
//      IS:   n=217 Sharpe=3.59 PnL_per_pip_per_leg
//      OOS:  n=214 Sharpe=2.86  (50/50 time split)
//      WF 4-fold: Sh 2.22 .. 4.62 all positive
//      Cost stress (2pip/leg worst case): Sh=1.64 still profitable
//      Cost stress (1pip/leg realistic): Sh=3.20 PnL >>
//
//  ARCHITECTURE:
//    Pairs trading needs TWO simultaneous symbol legs. This engine receives
//    ticks from BOTH on_tick_eurusd() and on_tick_gbpusd() via separate
//    callbacks. It maintains per-symbol H1 OHLC accumulators, and only acts
//    on H1 close events where BOTH legs have a closed bar for the same bucket.
//
//    Position state is per-pair (one logical trade = 2 broker orders):
//      long-spread  = long EURUSD + short GBPUSD  (bet spread widens)
//      short-spread = short EURUSD + long GBPUSD  (bet spread tightens)
//
//    Exit triggers (both legs flat together):
//      - z-score crosses back through ±z_out
//      - hold timeout (default 24 H1 bars = 1 trading day)
//      - hard stop if z extends beyond ±3.5 (3-sigma blowout)
//
//  SHADOW MODE:
//    Default shadow_mode=true. NEVER set false without minimum 30 closed
//    shadow trades matching backtest expectation (~50 trades/quarter).
//
//  S37 Phase H STAGE-TRAIL NOT-APPLICABLE (2026-05-27b).
//  The Phase H trail audit considered this engine and rejected trail addition
//  on STRUCTURAL grounds (not empirical -- no test run).
//  Reasoning: trail addition assumes the engine seeks unbounded MFE on the
//  signal side and needs protection against unrealised-profit giveback.
//  Pairs mean-reversion EXPECTS the spread to revert to zero; the engine's
//  natural exit is z-score crossing back through ±z_out (the intended
//  profit-take). There is no "winner-giveback" because there is no fixed TP
//  to give back from -- exits are state-driven, not target-driven.
//  A trail would CONFLICT with the z-out exit: trail SL could fire before
//  the spread reverts, locking small profit at half the natural profit
//  trajectory. Engine STAYS as-is. Trail NOT implemented.
//
//  PERSISTENCE:
//    Stateless across restarts -- pairs engine re-warms from first H1 closes.
//    No save_state/load_state (rolling window self-rebuilds in ~60 H1 bars = 2.5 days).
// =============================================================================

#pragma once
//  ADVERSE-PROTECTION: mean-reversion -- in-flight cut is the z_stop=3.5sigma hard-stop blowout exit + state-driven z_out=0.5 profit-take + 24-H1-bar hold timeout (no LOSS_CUT_PCT/BE ratchet; trail REJECTED S37 Phase H 2026-05-27b on structural grounds -- a trail SL would fire before reversion); engine is DISABLED (g_eur_gbp_pairs CULL S-2026-06-17, faithful pairs_rigor_cpp Sh-2.04 both halves, all-9-neg) -- protection moot while off, verdict owed before re-enable. (backfill S-2026-06-24n)
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <string>
#include <tuple>
#include <vector>
#include <algorithm>
#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"
#include "SeedGuard.hpp"

namespace omega {

struct EurGbpPairsParams {
    int    z_window           = 60;     // rolling N bars for mean/sd
    double z_in               = 2.0;    // entry z-score threshold
    double z_out              = 0.5;    // exit z-score threshold
    double z_stop             = 3.5;    // hard-stop blowout
    int    hold_timeout_h1    = 24;     // exit after N timeframe bars regardless
    double risk_dollars       = 10.0;   // approx $ risk per leg
    double max_lot_per_leg    = 0.10;   // safety cap
    double eur_usd_per_pt     = 100000.0; // EURUSD: $10/pip at 1.0 lot -> PnL = move * lot * 100000
    double gbp_usd_per_pt     = 100000.0; // GBPUSD: same scale
    double max_spread_eur     = 0.00030; // skip entry if EUR spread > 3pip
    double max_spread_gbp     = 0.00040; // skip entry if GBP spread > 4pip
    bool   weekend_close_gate = true;
    int64_t bucket_ms         = 3600000LL; // 3600000=H1, 14400000=H4, 86400000=D1
};

inline EurGbpPairsParams make_eur_gbp_pairs_params() { return EurGbpPairsParams{}; }

struct EurGbpPairsSignal {
    bool        valid       = false;
    bool        long_spread = false; // true=long EUR + short GBP
    double      eur_entry   = 0.0;
    double      gbp_entry   = 0.0;
    double      eur_sl      = 0.0;
    double      gbp_sl      = 0.0;
    double      lot         = 0.0;
    double      z           = 0.0;
    const char* reason      = "";
};

struct EurGbpPairsEngine {

    bool                shadow_mode = true;
    bool                enabled     = true;
    EurGbpPairsParams   p;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct H1Accum {
        bool     active    = false;
        int64_t  bucket_ms = 0;
        double   open      = 0.0;
        double   high      = 0.0;
        double   low       = 0.0;
        double   close     = 0.0;
    };

    H1Accum eur_acc_;
    H1Accum gbp_acc_;

    double  eur_last_bid_ = 0.0;
    double  eur_last_ask_ = 0.0;
    double  gbp_last_bid_ = 0.0;
    double  gbp_last_ask_ = 0.0;
    int64_t last_ts_ms_   = 0;

    // Synced closed-bar history: only push entries where BOTH symbols closed same bucket
    std::deque<double> spread_hist_;  // EUR_close - GBP_close per common bucket
    int     bar_count_           = 0;

    // Open position state
    struct OpenPos {
        bool    active        = false;
        bool    long_spread   = false;
        double  eur_entry     = 0.0;
        double  gbp_entry     = 0.0;
        double  entry_spread  = 0.0;
        double  lot           = 0.0;
        int64_t entry_ts_ms   = 0;
        int     bars_held     = 0;
        double  z_at_entry    = 0.0;
        double  mfe = 0.0, mae = 0.0;   // spread-excursion terms
    } pos_;

    int m_trade_id_ = 0;

    bool has_open_position() const noexcept { return pos_.active; }

    // ── Tick handlers (one per leg) ─────────────────────────────────────────
    // Called from on_tick_eurusd / on_tick_gbpusd. Updates H1 accumulator for
    // that leg. When the H1 boundary crosses for THIS leg AND the other leg
    // has also closed the same bucket, signal evaluation runs.

    EurGbpPairsSignal on_tick_eur(double bid, double ask, int64_t now_ms,
                                  CloseCallback on_close) noexcept
    {
        eur_last_bid_ = bid; eur_last_ask_ = ask; last_ts_ms_ = now_ms;
        return _tick_internal(true, bid, ask, now_ms, on_close);
    }

    EurGbpPairsSignal on_tick_gbp(double bid, double ask, int64_t now_ms,
                                  CloseCallback on_close) noexcept
    {
        gbp_last_bid_ = bid; gbp_last_ask_ = ask; last_ts_ms_ = now_ms;
        return _tick_internal(false, bid, ask, now_ms, on_close);
    }

    EurGbpPairsSignal _tick_internal(bool is_eur_leg, double bid, double ask,
                                     int64_t now_ms, CloseCallback on_close) noexcept
    {
        EurGbpPairsSignal sig{};
        if (bid <= 0.0 || ask <= 0.0) return sig;
        const double mid = (bid + ask) * 0.5;
        H1Accum& acc = is_eur_leg ? eur_acc_ : gbp_acc_;
        const int64_t bucket = (now_ms / p.bucket_ms) * p.bucket_ms;

        // Manage open position on EVERY tick (for hard stops + timeouts)
        if (pos_.active && is_eur_leg) _manage_position(now_ms, on_close);

        if (!acc.active) {
            acc.active = true; acc.bucket_ms = bucket;
            acc.open = acc.high = acc.low = acc.close = mid;
            return sig;
        }

        if (bucket != acc.bucket_ms) {
            // H1 boundary crossed for THIS leg.
            const double eur_close = is_eur_leg ? acc.close : eur_acc_.close;
            const double gbp_close = is_eur_leg ? gbp_acc_.close : acc.close;
            const int64_t eur_bucket = is_eur_leg ? acc.bucket_ms : eur_acc_.bucket_ms;
            const int64_t gbp_bucket = is_eur_leg ? gbp_acc_.bucket_ms : acc.bucket_ms;

            // Only consume if BOTH legs have a closed bar for the same bucket.
            if (eur_acc_.active && gbp_acc_.active && eur_bucket == gbp_bucket) {
                const double spread = eur_close - gbp_close;
                spread_hist_.push_back(spread);
                while ((int)spread_hist_.size() > p.z_window + 1) spread_hist_.pop_front();
                ++bar_count_;
                sig = _check_signal(spread, now_ms, on_close);
            }

            // Reset this leg's accumulator with the current tick
            acc.bucket_ms = bucket;
            acc.open = acc.high = acc.low = acc.close = mid;
        } else {
            if (mid > acc.high) acc.high = mid;
            if (mid < acc.low)  acc.low  = mid;
            acc.close = mid;
        }
        return sig;
    }

    EurGbpPairsSignal _check_signal(double spread, int64_t now_ms,
                                     CloseCallback on_close) noexcept
    {
        EurGbpPairsSignal sig{};
        if ((int)spread_hist_.size() < p.z_window + 1) return sig;
        if (pos_.active) {
            ++pos_.bars_held;
            { double fav=(pos_.long_spread?(spread-pos_.entry_spread):(pos_.entry_spread-spread)); if(fav>pos_.mfe)pos_.mfe=fav; if(-fav>pos_.mae)pos_.mae=-fav; }  // spread excursion
            return sig;
        }
        if (!enabled) return sig;

        // Compute rolling mean/sd over the LAST z_window closes (excluding current)
        double sum = 0.0;
        const int base = (int)spread_hist_.size() - 1 - p.z_window;
        for (int i = 0; i < p.z_window; ++i) sum += spread_hist_[base + i];
        const double mean = sum / p.z_window;
        double var = 0.0;
        for (int i = 0; i < p.z_window; ++i) {
            const double d = spread_hist_[base + i] - mean;
            var += d * d;
        }
        var /= (p.z_window - 1);
        const double sd = std::sqrt(var);
        if (sd <= 0.0) return sig;
        const double z = (spread - mean) / sd;

        // Spread quality gate
        if ((eur_last_ask_ - eur_last_bid_) > p.max_spread_eur) return sig;
        if ((gbp_last_ask_ - gbp_last_bid_) > p.max_spread_gbp) return sig;

        if (z > p.z_in) {
            sig.long_spread = false;  // short spread (short EUR + long GBP)
        } else if (z < -p.z_in) {
            sig.long_spread = true;   // long spread (long EUR + short GBP)
        } else return sig;

        // Sizing: equal lot per leg; very small fixed risk
        const double lot = std::max(0.01, std::min(p.max_lot_per_leg, 0.01));
        const double eur_entry = sig.long_spread ? eur_last_ask_ : eur_last_bid_;
        const double gbp_entry = sig.long_spread ? gbp_last_bid_ : gbp_last_ask_;

        // cost gate per leg: expected reversion = (z_in - z_out)*sd in spread
        // units, split half per leg (the convergence is shared across legs)
        const double leg_move = 0.5 * (p.z_in - p.z_out) * sd;
        if (!ExecutionCostGuard::is_viable("EURUSD", eur_last_ask_ - eur_last_bid_, leg_move, lot, 1.5)) return sig;
        if (!ExecutionCostGuard::is_viable("GBPUSD", gbp_last_ask_ - gbp_last_bid_, leg_move, lot, 1.5)) return sig;

        pos_.active        = true;
        pos_.long_spread   = sig.long_spread;
        pos_.eur_entry     = eur_entry;
        pos_.gbp_entry     = gbp_entry;
        pos_.entry_spread  = spread;
        pos_.lot           = lot;
        pos_.entry_ts_ms   = now_ms;
        pos_.bars_held     = 0;
        pos_.z_at_entry    = z;
        ++m_trade_id_;

        sig.valid     = true;
        sig.eur_entry = eur_entry;
        sig.gbp_entry = gbp_entry;
        sig.lot       = lot;
        sig.z         = z;
        sig.reason    = "PAIRS_ZSCORE_FADE";

        printf("[PAIRS] ENTRY %s_SPREAD z=%.2f eur@%.5f gbp@%.5f lot=%.2f%s\n",
               sig.long_spread ? "LONG" : "SHORT", z, eur_entry, gbp_entry, lot,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);
        (void)on_close;
        return sig;
    }

    void _manage_position(int64_t now_ms, CloseCallback on_close) noexcept
    {
        if (!pos_.active || (int)spread_hist_.size() < p.z_window + 1) return;
        const double cur_spread = eur_acc_.close - gbp_acc_.close;
        const int base = (int)spread_hist_.size() - 1 - p.z_window;
        if (base < 0) return;
        double sum = 0.0;
        for (int i = 0; i < p.z_window; ++i) sum += spread_hist_[base + i];
        const double mean = sum / p.z_window;
        double var = 0.0;
        for (int i = 0; i < p.z_window; ++i) {
            const double d = spread_hist_[base + i] - mean;
            var += d * d;
        }
        var /= (p.z_window - 1);
        const double sd = std::sqrt(var);
        if (sd <= 0.0) return;
        const double z = (cur_spread - mean) / sd;

        bool exit_hit = false;
        const char* reason = "";
        if (pos_.long_spread) {
            if (z >= -p.z_out) { exit_hit = true; reason = "Z_OUT"; }
            else if (z <= -p.z_stop) { exit_hit = true; reason = "Z_STOP"; }
        } else {
            if (z <= p.z_out) { exit_hit = true; reason = "Z_OUT"; }
            else if (z >= p.z_stop) { exit_hit = true; reason = "Z_STOP"; }
        }
        if (!exit_hit && pos_.bars_held >= p.hold_timeout_h1) {
            exit_hit = true; reason = "TIMEOUT";
        }
        if (!exit_hit) return;

        const double eur_exit = pos_.long_spread ? eur_last_bid_ : eur_last_ask_;
        const double gbp_exit = pos_.long_spread ? gbp_last_ask_ : gbp_last_bid_;
        // PnL: long-spread = long EUR + short GBP
        const double eur_pnl = (pos_.long_spread ? +1.0 : -1.0)
            * (eur_exit - pos_.eur_entry) * pos_.lot * p.eur_usd_per_pt;
        const double gbp_pnl = (pos_.long_spread ? -1.0 : +1.0)
            * (gbp_exit - pos_.gbp_entry) * pos_.lot * p.gbp_usd_per_pt;
        const double pnl = eur_pnl + gbp_pnl;

        printf("[PAIRS] EXIT %s reason=%s z=%.2f eur_exit=%.5f gbp_exit=%.5f"
               " eur_pnl=%.2f gbp_pnl=%.2f total=%.2f bars=%d%s\n",
               pos_.long_spread ? "LONG_SPREAD" : "SHORT_SPREAD", reason,
               z, eur_exit, gbp_exit, eur_pnl, gbp_pnl, pnl, pos_.bars_held,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        TradeRecord tr{};
        tr.symbol     = "EURUSD+GBPUSD";
        tr.side       = pos_.long_spread ? "LONG" : "SHORT";
        tr.entryPrice = pos_.eur_entry;
        tr.exitPrice  = eur_exit;
        tr.pnl        = pnl;
        tr.size       = pos_.lot;
        tr.entryTs    = pos_.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.engine     = "EurGbpPairs";
        tr.exitReason = reason;
        tr.mfe = pos_.mfe; tr.mae = pos_.mae;
        if (on_close) on_close(tr);

        pos_ = OpenPos{};
    }

    void force_close(int64_t now_ms, CloseCallback cb) noexcept {
        if (!pos_.active) return;
        const double eur_exit = pos_.long_spread ? eur_last_bid_ : eur_last_ask_;
        const double gbp_exit = pos_.long_spread ? gbp_last_ask_ : gbp_last_bid_;
        const double eur_pnl = (pos_.long_spread ? +1.0 : -1.0)
            * (eur_exit - pos_.eur_entry) * pos_.lot * p.eur_usd_per_pt;
        const double gbp_pnl = (pos_.long_spread ? -1.0 : +1.0)
            * (gbp_exit - pos_.gbp_entry) * pos_.lot * p.gbp_usd_per_pt;
        TradeRecord tr{};
        tr.symbol = "EURUSD+GBPUSD";
        tr.side   = pos_.long_spread ? "LONG" : "SHORT";
        tr.entryPrice = pos_.eur_entry;
        tr.exitPrice  = eur_exit;
        tr.pnl = eur_pnl + gbp_pnl;
        tr.size = pos_.lot;
        tr.entryTs = pos_.entry_ts_ms / 1000;
        tr.exitTs  = now_ms / 1000;
        tr.engine = "EurGbpPairs";
        tr.exitReason = "FORCE_CLOSE";
        tr.mfe = pos_.mfe; tr.mae = pos_.mae;
        if (cb) cb(tr);
        pos_ = OpenPos{};
    }

    void cancel() noexcept { pos_ = OpenPos{}; }

    // Warm-seed from EUR + GBP H1 close CSVs.
    // CSV format: ts,o,h,l,c (ts in SECONDS).
    // Replays both legs alternately as synthetic ticks at H1-close timestamps
    // so internal H1 accumulators + spread_hist_ + z-window populate.
    // Engine momentarily disabled during seed to suppress entries.
    size_t seed_from_h1_csvs(const std::string& eur_path,
                              const std::string& gbp_path) noexcept {
        // Load each CSV into vectors of (ts_sec, close).
        auto load = [](const std::string& path,
                       std::vector<std::pair<int64_t,double>>& out) {
            std::ifstream f(path);
            if (!f.is_open()) return;
            std::string line; std::getline(f, line);  // header
            while (std::getline(f, line)) {
                long long ts_s_ll=0; double o=0,h=0,l=0,c=0;
                if (sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf",
                           &ts_s_ll, &o, &h, &l, &c) == 5) {
                    out.emplace_back(static_cast<int64_t>(ts_s_ll), c);
                }
            }
        };
        const std::string eur_actual = omega::resolve_seed_path(eur_path);
        const std::string gbp_actual = omega::resolve_seed_path(gbp_path);
        std::vector<std::pair<int64_t,double>> eur_h1, gbp_h1;
        load(eur_actual, eur_h1);
        load(gbp_actual, gbp_h1);
        if (eur_h1.empty() || gbp_h1.empty()) {
            printf("[SEED-FATAL] EurGbpPairs: empty seed (eur=%zu gbp=%zu) "
                   "from %s / %s\n",
                   eur_h1.size(), gbp_h1.size(),
                   eur_actual.c_str(), gbp_actual.c_str());
            fflush(stdout);
            omega::seed_die("EurGbpPairs",
                            eur_h1.empty() ? eur_actual : gbp_actual);
        }

        // Build a sorted timeline of (ts_ms, leg, close).
        // leg = 0 (eur) or 1 (gbp).
        std::vector<std::tuple<int64_t,int,double>> stream;
        stream.reserve(eur_h1.size() + gbp_h1.size());
        for (auto& [ts, c] : eur_h1) stream.emplace_back(ts*1000LL, 0, c);
        for (auto& [ts, c] : gbp_h1) stream.emplace_back(ts*1000LL, 1, c);
        std::sort(stream.begin(), stream.end());

        const bool was_enabled = enabled;
        enabled = false;
        auto null_cb = [](const omega::TradeRecord&){};

        // Synthetic spread: EURUSD ~1pip, GBPUSD ~1.2pip
        const double eur_sp = 0.00010, gbp_sp = 0.00012;
        size_t n = 0;
        for (auto& [ts_ms, leg, c] : stream) {
            if (leg == 0) {
                on_tick_eur(c - eur_sp/2, c + eur_sp/2, ts_ms, null_cb);
            } else {
                on_tick_gbp(c - gbp_sp/2, c + gbp_sp/2, ts_ms, null_cb);
            }
            ++n;
        }
        enabled = was_enabled;
        printf("[SEED] EurGbpPairs: %zu H1 ticks replayed (eur=%zu gbp=%zu)"
               " spread_hist_size=%d -- hot\n",
               n, eur_h1.size(), gbp_h1.size(), (int)spread_hist_.size());
        fflush(stdout);
        return n;
    }
};

} // namespace omega

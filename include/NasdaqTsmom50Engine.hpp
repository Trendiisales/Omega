// =============================================================================
//  NasdaqTsmom50Engine.hpp -- TSMom50 on NAS100 D1 (long-only, flip exit)
//
//  PROVENANCE (S-2026-07-03)
//
//  MIGRATED from the Mac-side ~/Crypto book ("IBKRCrypto -- Tradeable Book",
//  QNDX leg, strat tag "Trend (TSMom50)") per operator instruction: "MOVE THIS
//  NASDAQ ENTRY OFF CRYPTO AND ONTO OMEGA ... it must be activated and working".
//  The rule was reverse-confirmed against the live book to the cent: the GUI
//  flip level 26,479.47 == ^NDX close exactly 50 trading days back. Rule:
//
//      LONG while daily close > close[50 trading days ago]; flat otherwise.
//      (Signal on COMPLETED daily bar; act at next tick = next-open fill.)
//
//  Chassis: clone of NasTurtleD1Engine (proven index-D1 pattern: self-aggregates
//  D1 bars from the tick stream, warm-seeded, VIX risk-off gate, cost guard,
//  shadow ledger + GUI telemetry). Differences: TSMom50 signal instead of
//  Donchian break, FLIP exit instead of TP/SL/timeout, vol-target sizing
//  (matches the Mac book's "size 0.96x" display), 8% disaster stop.
//
//  BACKTEST (faithful daily ^NDX 1996-2026, 30y: dot-com bust + 2008 + 2022;
//  next-open fills, 5 bps/side; /tmp scratch tsmom50_bt.py, results recorded in
//  backtest/crypto_bear_bounce/FINDINGS.md addendum + PR #2):
//    LONG-only flip-only: n=189 PF 1.94 CAGR 5.4% (vt15) maxDD -26.7%,
//      both WF halves + (H1 1.63 / H2 2.09), cost-x3 PF 1.69,
//      lookback plateau broad (LB40 1.83 / LB60 2.24).
//    LONG+SHORT: PF 1.05 -- shorts DEAD (matches repo index-short priors).
//      Long-only is the shipped rule.
//
//  ADVERSE-PROTECTION: flip-exit + 8% DISASTER STOP (backtested verdict).
//    The Mac book ran "no hard stop by design"; the 30y sweep upgrades that:
//    hard stop  5%: PF 1.94->1.82 (too tight, scalps recoverable dips)
//    hard stop  8%: net UP ($371k->$379k), PF holds 1.94, worst -12.2%->-8.0%,
//                   maxDD -26.7%->-25.5%  => SHIPPED (p.stop_pct=0.08).
//    hard stop 12%: neutral (fires too late to matter).
//    BE floor arm2%: PF 1.94->1.71 net -38% => HARMFUL, do NOT add (same
//    verdict class as the crypto BearRecovery giveback finding).
//
//  COST GATE: ExecutionCostGuard::is_viable at entry with a conservative +5%
//  proxy target (flip exit has no TP; realized median winner is larger, so the
//  proxy under-states edge -- gate errs safe). NAS100 CFD costs are trivial vs
//  50-day holds (cost-x3 PF 1.69).
// =============================================================================

#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <string>
#include <algorithm>
#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"
#include "OpenPositionRegistry.hpp"
#include "SeedGuard.hpp"
#include "IndexRiskGate.hpp"

namespace omega {

struct NasdaqTsmom50Params {
    int    lookback_bars   = 50;     // TSMom lookback (trading days)
    double stop_pct        = 0.08;   // disaster stop below entry (backtested; see header)
    double vol_target      = 0.15;   // annualized vol target for sizing
    double vol_cap_mult    = 1.5;    // cap on the vol-target multiplier
    int    vol_window      = 20;     // realized-vol window (daily closes)
    double notional_target = 3000.0; // $ notional at 1.0x vol mult (Mac book: qty 1 QNDX ~ $2.9k)
    double dollars_per_pt  = 1.0;    // NAS100 CFD ~ $1/pt at 1.0 lot (shadow PnL scale)
    double max_spread      = 30.0;   // NAS100 wide vs gold/DAX
    bool   use_risk_gate   = true;   // S44 portfolio VIX risk-off: no NEW entries
};

inline NasdaqTsmom50Params make_nasdaq_tsmom50_params() { return NasdaqTsmom50Params{}; }

struct NasdaqTsmom50Signal {
    bool        valid  = false;
    double      entry  = 0.0;
    double      sl     = 0.0;
    double      lot    = 0.0;
    const char* reason = "";
};

struct NasdaqTsmom50Engine {
    bool   shadow_mode = true;
    bool   enabled     = true;
    NasdaqTsmom50Params p;
    std::string symbol = "NAS100";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct D1Accum {
        bool     active    = false;
        int64_t  bucket_ms = 0;
        double   open = 0, high = 0, low = 0, close = 0;
    } d1_acc_;

    std::deque<double> d1_closes_;   // completed daily closes (keep lookback+vol+2)
    int  bar_count_ = 0;

    struct OpenPos {
        bool active = false;
        double entry = 0, sl = 0, lot = 0, mfe = 0, mae = 0;
        int64_t entry_ts_ms = 0;
        int bars_held = 0;
    } pos_;

    bool has_open_position() const noexcept { return pos_.active; }

    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const {
        if (!pos_.active) return false;
        o.engine = eng; o.symbol = sym; o.side = "LONG";
        o.size = pos_.lot; o.entry = pos_.entry; o.sl = pos_.sl; o.tp = 0.0;
        o.entry_ts = pos_.entry_ts_ms / 1000;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        pos_.active = true;
        pos_.entry = ps.entry; pos_.sl = ps.sl; pos_.lot = ps.size;
        pos_.entry_ts_ms = ps.entry_ts * 1000;
        return true;
    }

    void force_close(double bid, double /*ask*/, CloseCallback on_close, const char* reason) noexcept {
        if (!pos_.active) return;
        _close(bid, reason, (int64_t)std::time(nullptr) * 1000, on_close);
    }

    NasdaqTsmom50Signal on_tick(double bid, double ask, int64_t now_ms,
                                CloseCallback on_close) noexcept {
        NasdaqTsmom50Signal sig{};
        if (bid <= 0.0 || ask <= 0.0) return sig;
        const double mid = (bid + ask) * 0.5;

        // in-flight: mfe/mae marks + the 8% disaster stop (intrabar, every tick)
        if (pos_.active) {
            const double move = mid - pos_.entry;
            if (move > pos_.mfe) pos_.mfe = move;
            if (move < pos_.mae) pos_.mae = move;
            if (bid <= pos_.sl) { _close(bid, "DISASTER_STOP", now_ms, on_close); return sig; }
        }

        const int64_t bucket = (now_ms / 86400000LL) * 86400000LL;
        if (!d1_acc_.active) {
            d1_acc_.active = true; d1_acc_.bucket_ms = bucket;
            d1_acc_.open = d1_acc_.high = d1_acc_.low = d1_acc_.close = mid;
            return sig;
        }
        if (bucket != d1_acc_.bucket_ms) {
            const double bar_close = d1_acc_.close;

            d1_closes_.push_back(bar_close);
            const int keep = p.lookback_bars + p.vol_window + 2;
            while ((int)d1_closes_.size() > keep) d1_closes_.pop_front();
            ++bar_count_;

            const int n = (int)d1_closes_.size();
            // TSMom50 on the COMPLETED bar: close vs close 50 trading days back.
            // (Confirmed vs the live Mac book: its flip level == close[t-50] exactly.)
            const bool mom_long = n >= p.lookback_bars + 1
                && bar_close > d1_closes_[n - 1 - p.lookback_bars];

            // FLIP exit: momentum sign turned on the completed bar; fill at the
            // current tick (= next-open semantics, faithful to the backtest).
            if (pos_.active && !mom_long) {
                ++pos_.bars_held;
                _close(bid, "TSMOM_FLIP", now_ms, on_close);
            } else if (pos_.active) {
                ++pos_.bars_held;
            }

            if (!pos_.active && enabled && mom_long
                && (ask - bid) <= p.max_spread
                && !(p.use_risk_gate && omega::index_risk_off()))   // S44 VIX risk-off gate
            {
                const double entry_px = ask;
                const double sl_px    = entry_px * (1.0 - p.stop_pct);

                // vol-target size (the Mac book's "0.96x" column): annualized
                // realized vol over vol_window daily closes -> multiplier.
                double mult = 1.0;
                if (n >= p.vol_window + 1) {
                    double sum = 0.0, sum2 = 0.0;
                    for (int i = n - p.vol_window; i < n; ++i) {
                        const double r = d1_closes_[i] / d1_closes_[i - 1] - 1.0;
                        sum += r; sum2 += r * r;
                    }
                    const double m = sum / p.vol_window;
                    const double var = std::max(0.0, sum2 / p.vol_window - m * m);
                    const double ann = std::sqrt(var * 252.0);
                    mult = std::min(p.vol_cap_mult, p.vol_target / std::max(ann, 1e-9));
                }
                double size = p.notional_target * mult / (entry_px * p.dollars_per_pt);
                size = std::floor(size / 0.01) * 0.01;
                size = std::max(0.01, size);

                // +5% proxy target for the entry-time cost gate (flip exit has no
                // TP; the 30y median winner is larger, so this under-states edge).
                if (ExecutionCostGuard::is_viable(symbol.c_str(), ask - bid,
                                                  entry_px * 0.05, size, 1.5)) {
                    pos_.active = true;
                    pos_.entry = entry_px; pos_.sl = sl_px; pos_.lot = size;
                    pos_.mfe = pos_.mae = 0;
                    pos_.entry_ts_ms = now_ms; pos_.bars_held = 0;

                    printf("[NAS_TSMOM50] ENTRY LONG @ %.2f sl=%.2f lot=%.3f volx=%.2f"
                           " flip_lvl=%.2f%s\n",
                           entry_px, sl_px, size, mult,
                           d1_closes_[n - 1 - p.lookback_bars],
                           shadow_mode ? " [SHADOW]" : "");
                    fflush(stdout);

                    sig.valid = true; sig.entry = entry_px; sig.sl = sl_px; sig.lot = size;
                    sig.reason = "NAS_TSMOM50_LONG";
                }
            }

            d1_acc_.bucket_ms = bucket;
            d1_acc_.open = d1_acc_.high = d1_acc_.low = d1_acc_.close = mid;
        } else {
            if (mid > d1_acc_.high) d1_acc_.high = mid;
            if (mid < d1_acc_.low)  d1_acc_.low  = mid;
            d1_acc_.close = mid;
        }
        return sig;
    }

    void cancel() noexcept { pos_ = OpenPos{}; }

    // Warm-seed historical D1 closes (bypasses tick aggregation) so the 50-day
    // momentum + 20-day vol windows are HOT on first live tick instead of
    // cold-warming ~10 weeks. CSV: ts_ms,open,high,low,close (header optional).
    size_t seed_from_d1_csv(const std::string& path) noexcept {
        const std::string actual = omega::resolve_seed_path(path);
        std::ifstream f(actual);
        if (!f.is_open()) {
            omega::seed_die("NasdaqTsmom50", actual);  // [[noreturn]]
        }
        std::string line; std::getline(f, line); // header
        size_t nrows = 0;
        while (std::getline(f, line)) {
            long long ts_ms_ll = 0; double o = 0, h = 0, l = 0, c = 0;
            if (sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf",
                       &ts_ms_ll, &o, &h, &l, &c) == 5) {
                d1_closes_.push_back(c);
                const int keep = p.lookback_bars + p.vol_window + 2;
                while ((int)d1_closes_.size() > keep) d1_closes_.pop_front();
                ++bar_count_;
                ++nrows;
            }
        }
        if (nrows < static_cast<size_t>(p.lookback_bars + 1)) {
            printf("[SEED-FATAL] NasdaqTsmom50: only %zu rows in %s (need >= %d)\n",
                   nrows, actual.c_str(), p.lookback_bars + 1);
            fflush(stdout);
            omega::seed_die("NasdaqTsmom50", actual);  // [[noreturn]]
        }
        const int n = (int)d1_closes_.size();
        const bool mom = n >= p.lookback_bars + 1
            && d1_closes_.back() > d1_closes_[n - 1 - p.lookback_bars];
        printf("[SEED] NasdaqTsmom50: %zu D1 bars -> hot (mom=%s flip_lvl=%.2f) [%s]\n",
               nrows, mom ? "LONG" : "flat",
               n >= p.lookback_bars + 1 ? d1_closes_[n - 1 - p.lookback_bars] : 0.0,
               actual.c_str());
        fflush(stdout);
        return nrows;
    }

    void _close(double exit_px, const char* reason, int64_t now_ms,
                CloseCallback on_close) noexcept {
        if (!pos_.active) return;
        const double pts_move = exit_px - pos_.entry;
        const double pnl_dollars = pts_move * pos_.lot * p.dollars_per_pt;

        printf("[NAS_TSMOM50] EXIT reason=%s entry=%.2f exit=%.2f pts=%.2f pnl=%.2f bars=%d%s\n",
               reason, pos_.entry, exit_px, pts_move, pnl_dollars, pos_.bars_held,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        TradeRecord tr{};
        tr.symbol = symbol; tr.side = "LONG";
        tr.entryPrice = pos_.entry; tr.exitPrice = exit_px;
        tr.tp = 0.0; tr.sl = pos_.sl; tr.size = pos_.lot;
        tr.pnl = pnl_dollars; tr.mfe = pos_.mfe; tr.mae = pos_.mae;
        tr.entryTs = pos_.entry_ts_ms / 1000; tr.exitTs = now_ms / 1000;
        tr.exitReason = reason; tr.engine = std::string("NasdaqTsmom50_") + symbol;
        if (on_close) on_close(tr);
        pos_ = OpenPos{};
    }
};

} // namespace omega

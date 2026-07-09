// =============================================================================
//  NasTurtleD1Engine.hpp -- 20-day Donchian break on NAS100 D1 (long-only)
//
//  PROVENANCE (2026-06-14)
//
//  Seykota/Donchian D1 archetype extended to NAS100. Clone of
//  Ger40TurtleH4Engine (proven index-turtle chassis: self-aggregates bars from
//  the tick stream, long-only, index VIX risk-off gate) retuned to D1 (86400s
//  bucket) + NAS100. Differences vs the GER40 template: D1 bucket, lookback=20
//  days, NAS100 symbol/cost-gate, the DAX-cash session filter removed (NAS D1
//  bars close at 00:00 UTC; no intraday session gate).
//
//  BACKTEST (independent Yahoo daily 2016-2026, robust config ema100/don20):
//    NAS100 long-only MAR 0.44, PF 2.10, Sharpe 0.58. One of only two trend
//    horses in the Omega universe (the other = XAU, [[XauTurtleD1Engine]]); FX
//    and EU indices were DEAD for breakout-trend. Reconfirms the FVG/Peachy NAS
//    edge from a 3rd data source. SHADOW until >=5 live shadow trades.
//
//  NOTE: cold-warm (NO mandatory seed CSV -> no seed_die crash risk). Needs
//  ~20 live D1 bars (~4 weeks) before the first possible signal. Exit uses the
//  GER40 chassis TP/SL (sl 1.5*ATR / tp 5.0*ATR); the Yahoo test favoured a
//  wide-trail no-TP exit -> revisit exit after first shadow trades.
// =============================================================================

#pragma once
//  ADVERSE-PROTECTION: trail-only by design (backtested). Turtle D1 breakout =
//    structural Donchian-exit stop + trend trail; index_turtle_d1_audit.cpp faithful
//    (SPX 1.69 / DJ30 1.40 EDGE, both WF halves + 2022 bear+). A cold loss-cut is NOT
//    added -- swing-protection sweep 2026-06-17 (omega-engine-swing-protection-sweep)
//    showed tightening LOWERS net on trend/trail engines. NasTurtleD1 instance
//    tombstoned (TOMBSTONES.tsv, bull-beta); SPX/DJ30 carry the real edge.
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
#include "TrendAccountingGuard.hpp"   // S-2026-06-26 selective STALL/REVERSAL supervisor (validated daily-index)
#include "OpenPositionRegistry.hpp"
#include "SeedGuard.hpp"
#include "IndexRiskGate.hpp"
#include "GoldTrendMimicLadder.hpp" // one-way mimic trigger + specific D1 feed

namespace omega {

struct NasTurtleD1Params {
    int    lookback_bars       = 20;
    int    hold_max_bars       = 20;
    double sl_atr_mult         = 1.5;
    double tp_atr_mult         = 5.0;
    // S-2026-06-29 RIDE EXIT (validated /tmp/turtle_exit_recon.py + ride_guard.py, faithful
    // daily 2016-26 next-open fill, 2bp RT, vs the fixed 5ATR-TP/20-timeout this replaces):
    //   exit when bar_close < prior n_out-bar Donchian low (structural turtle ride) instead
    //   of the 20-bar timeout that amputates the fat-tail winners.
    //   RIDE+GUARD overall PF — NDX 1.92->2.97, SPX 2.28->3.60, DJ30 1.94 (ride alone 2.34).
    //   The TrendAccountingGuard (already wired below) supplies the give-back/reversal cut
    //   that makes the ride safe; ride_exit + guard is the validated pair.
    //   BEAR caveat (n=4, 2022): RIDE leaves NDX bear mildly negative (guard trims -6.6->-4.4);
    //   regime_bear_block below sits the longs out in a sustained bear to plug the residual.
    bool   ride_exit           = false;  // opt-in per instance; live NAS set true in engine_init
    int    n_out               = 10;     // Donchian exit-channel lookback (structural ride exit)
    bool   regime_bear_block   = false;  // skip new longs when index_market_regime() == sustained bear
    // S-2026-06-19 giveback BE-ratchet (default OFF — KEEP OFF, documented-NEGATIVE).
    // Once mfe(pts) >= entry*BE_ARM_PCT/100, raise SL to entry+entry*BE_BUFFER_PCT/100.
    // FAITHFUL 10yr-daily audit (index_turtle_d1_audit.cpp argv9/10) on SPX/DJ30/NDX:
    // BE-arm does NOT improve any instrument — neutral-to-slightly-NEGATIVE net at
    // arm2/3/5, never beats baseline, and arm3/buf1 BREAKS NDX both-halves (H2 -36).
    // Root: the turtle already exits tight (1.5-ATR SL + 5-ATR TP + 20-bar timeout)
    // so BE-arm only trims winners that would have recovered — same as the fast 4h
    // (BE-arm helps ONLY loose/no-TP exits, e.g. XauTrendFollowD1 runner cells).
    // Fields kept (default 0) so the lever is re-testable without re-adding; do NOT
    // enable. Same disposition as use_ema100_filter (tested, negative, off).
    double BE_ARM_PCT          = 0.0;
    double BE_BUFFER_PCT       = 0.0;
    double risk_dollars        = 10.0;
    double lot                 = 0.01;
    double dollars_per_pt      = 1.0;   // NAS100 CFD ~ $1/pt at 1.0 lot (shadow PnL scale)
    int    atr_period          = 14;
    double max_spread          = 30.0;  // NAS100 wide vs gold/DAX
    bool   weekend_close_gate  = true;
    // Optional long-only trend filter: only break long when bar_close > EMA(ema100_period).
    // DEFAULT OFF — KEEP OFF. 2026-06-18 faithful 10yr-daily audit (index_turtle_d1_audit.cpp)
    // proved this filter HURTS cross-regime on ALL THREE instruments: it inflates the bull-window
    // PF (the source of the misleading "PF2.69" claim) but DESTROYS 2022-bear protection — in a
    // bear, price is below ema100 so the filter blocks the long breakouts that catch bear bounces
    // (SPX bear +92 -> -50, DJ30 +63 -> -8, NAS -8 -> -30). Documented negative result; do NOT enable.
    bool   use_ema100_filter   = false;
    int    ema100_period       = 100;
};

inline NasTurtleD1Params make_nas_turtle_d1_params() { return NasTurtleD1Params{}; }

struct NasTurtleD1Signal {
    bool        valid   = false;
    double      entry   = 0.0;
    double      sl      = 0.0;
    double      tp      = 0.0;
    double      lot     = 0.0;
    const char* reason  = "";
};

struct NasTurtleD1Engine {
    bool   shadow_mode = true;
    bool   enabled     = true;
    NasTurtleD1Params p;
    std::string symbol = "NAS100";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct D1Accum {
        bool     active     = false;
        int64_t  bucket_ms  = 0;
        double   open       = 0.0;
        double   high       = 0.0;
        double   low        = 0.0;
        double   close      = 0.0;
    } d1_acc_;

    std::deque<double> d1_highs_;
    std::deque<double> d1_lows_;
    std::deque<double> d1_closes_;

    double atr_=0.0;
    int    atr_seed_count_=0;
    double atr_seed_sum_=0.0;
    double prev_d1_close_=0.0;
    int    bar_count_=0;
    double ema100_=0.0;          // EMA of daily closes (trend filter)
    int    ema100_count_=0;      // bars accumulated into ema100_

    struct OpenPos {
        bool active=false;
        double entry=0, sl=0, tp=0, lot=0, mfe=0, mae=0;
        int64_t entry_ts_ms=0;
        int bars_held=0;
    } pos_;

    // S-2026-06-26 accounting supervisor: cut STALL (dead money) / REVERSAL (momentum turn) on the
    // SLOW index trend. Validated daily-index ONLY (PF 3.8->5.3 SPX, both-halves) -- NOT gold/crypto
    // (a wash/hurt there). enabled set per-instance in engine_init for the index turtles; default OFF.
    omega::TrendAccountingGuard accounting_guard_;

    int m_trade_id_=0;
    bool has_open_position() const noexcept { return pos_.active; }

    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const {
        if (!pos_.active) return false;
        o.engine = eng; o.symbol = sym; o.side = "LONG";
        o.size = pos_.lot; o.entry = pos_.entry; o.sl = pos_.sl; o.tp = pos_.tp;
        o.entry_ts = pos_.entry_ts_ms / 1000;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        pos_.active = true;
        pos_.entry = ps.entry; pos_.sl = ps.sl; pos_.tp = ps.tp; pos_.lot = ps.size;
        pos_.entry_ts_ms = ps.entry_ts * 1000;
        return true;
    }

    void force_close(double bid, double ask, CloseCallback on_close, const char* reason) noexcept {
        if (!pos_.active) return;
        _close(bid, reason, (int64_t)std::time(nullptr) * 1000, on_close);
    }

    NasTurtleD1Signal on_tick(double bid, double ask, int64_t now_ms,
                              CloseCallback on_close) noexcept {
        NasTurtleD1Signal sig{};
        if (bid <= 0.0 || ask <= 0.0) return sig;
        const double mid = (bid + ask) * 0.5;

        if (pos_.active) {
            const double move = mid - pos_.entry;
            if (move > pos_.mfe) pos_.mfe = move;
            if (move < pos_.mae) pos_.mae = move;
            // S-2026-06-19 giveback BE-ratchet (default OFF). Arm on mfe, raise SL to
            // entry+buffer so a reverse before TP exits ~breakeven not full round-trip.
            if (p.BE_ARM_PCT > 0.0 && pos_.entry > 0.0) {
                const double arm_pts = pos_.entry * p.BE_ARM_PCT / 100.0;
                if (pos_.mfe >= arm_pts) {
                    const double be_sl = pos_.entry + pos_.entry * p.BE_BUFFER_PCT / 100.0;
                    if (be_sl > pos_.sl) pos_.sl = be_sl;
                }
            }
            if (bid <= pos_.sl)      { _close(bid, "SL_HIT", now_ms, on_close); return sig; }
            else if (bid >= pos_.tp) { _close(bid, "TP_HIT", now_ms, on_close); return sig; }
        }

        const int64_t bucket = (now_ms / 86400000LL) * 86400000LL;
        if (!d1_acc_.active) {
            d1_acc_.active=true; d1_acc_.bucket_ms=bucket;
            d1_acc_.open=mid; d1_acc_.high=mid; d1_acc_.low=mid; d1_acc_.close=mid;
            return sig;
        }
        if (bucket != d1_acc_.bucket_ms) {
            const double bar_high = d1_acc_.high;
            const double bar_low  = d1_acc_.low;
            const double bar_close= d1_acc_.close;

            const double atr_pre = atr_;
            const int n_prior = (int)d1_highs_.size();
            double prior_high = 0.0;
            if (n_prior >= p.lookback_bars) {
                int start = n_prior - p.lookback_bars;
                prior_high = d1_highs_[start];
                for (int i = start+1; i < n_prior; ++i)
                    if (d1_highs_[i] > prior_high) prior_high = d1_highs_[i];
            }
            // S-2026-06-29 structural ride exit: prior n_out-bar Donchian LOW (excl today).
            double prior_low = 0.0;
            if (n_prior >= p.n_out) {
                int lstart = n_prior - p.n_out;
                prior_low = d1_lows_[lstart];
                for (int i = lstart+1; i < n_prior; ++i)
                    if (d1_lows_[i] < prior_low) prior_low = d1_lows_[i];
            }

            d1_highs_.push_back(bar_high);
            d1_lows_ .push_back(bar_low);
            d1_closes_.push_back(bar_close);
            const int keep = std::max(p.lookback_bars, p.atr_period) + 2;
            while ((int)d1_highs_.size() > keep) {
                d1_highs_.pop_front();
                d1_lows_ .pop_front();
                d1_closes_.pop_front();
            }

            _update_atr_on_bar_close(bar_high, bar_low, bar_close);
            ++bar_count_;

            // SPECIFIC FEED: drive this instance's mimic book (tag <symbol>Turtle) on the D1 bar
            // close -- the cadence the index-turtle mimic was backtested on. One-way; no-op if
            // the book is unregistered / has no open legs.
            omega::gold_trend_mimic().on_bar(symbol + "Turtle", bar_high, bar_low, bar_close, now_ms / 1000);

            const bool ema100_ok = !p.use_ema100_filter
                || (ema100_count_ >= p.ema100_period && bar_close > ema100_);
            if (!pos_.active && enabled
                && n_prior >= p.lookback_bars && atr_pre > 0.0
                && bar_close > prior_high
                && ema100_ok                   // optional trend filter (drops counter-trend breaks)
                && (ask - bid) <= p.max_spread
                && !(p.regime_bear_block && omega::index_market_regime().long_blocked()) // S-2026-06-29: sit out sustained bear (plugs NDX ride bear hole)
                && !omega::index_risk_off())   // S44 portfolio VIX risk-off: no new entry
            {
                const double entry_px = ask;
                const double atr_pct = atr_pre / bar_close;
                const double sl_px = entry_px * (1.0 - p.sl_atr_mult * atr_pct);
                const double tp_px = entry_px * (1.0 + p.tp_atr_mult * atr_pct);

                const double sl_pts = entry_px * p.sl_atr_mult * atr_pct;
                double size = p.risk_dollars / (sl_pts * p.dollars_per_pt);
                size = std::floor(size / 0.01) * 0.01;
                size = std::max(0.01, std::min(p.lot * 10, size));

                if (ExecutionCostGuard::is_viable("NAS100", ask - bid, tp_px - entry_px, size, 1.5)) {

                pos_.active=true;
                pos_.entry=entry_px; pos_.sl=sl_px; pos_.tp=tp_px;
                pos_.lot=size; pos_.mfe=pos_.mae=0;
                pos_.entry_ts_ms=now_ms; pos_.bars_held=0;
                // one-way mimic notify (fire-and-forget; long-only turtle -> dir +1)
                omega::gold_trend_mimic().on_trend_open(symbol + "Turtle", +1, entry_px, now_ms / 1000);
                ++m_trade_id_;

                printf("[NAS_TURTLE_D1] ENTRY LONG @ %.2f sl=%.2f tp=%.2f lot=%.3f"
                       " prior20d_high=%.2f atr=%.2f%s\n",
                       entry_px, sl_px, tp_px, size, prior_high, atr_pre,
                       shadow_mode ? " [SHADOW]" : "");
                fflush(stdout);

                sig.valid=true; sig.entry=entry_px; sig.sl=sl_px; sig.tp=tp_px; sig.lot=size;
                sig.reason = "NAS_TURTLE_D1_LONG";
                }
            }

            if (pos_.active) {
                ++pos_.bars_held;
                // S-2026-06-26 accounting supervisor (validated daily-index): cut a STALLED trade
                // (held long, never moved -> dead money) or a REVERSAL (9<21 EMA cross after profit).
                // Runs on daily-bar close. mfe is in pts -> /entry for the fraction the guard expects.
                if (accounting_guard_.enabled && pos_.active && (int)d1_closes_.size() >= 21) {
                    auto ema_of = [&](int span) {
                        double a = 2.0 / (span + 1.0), e = d1_closes_.front();
                        for (size_t i = 1; i < d1_closes_.size(); ++i) e = d1_closes_[i] * a + e * (1.0 - a);
                        return e;
                    };
                    const double mfe_frac = pos_.entry > 0.0 ? pos_.mfe / pos_.entry : 0.0;
                    const auto cut = accounting_guard_.decide(pos_.bars_held, mfe_frac, ema_of(9), ema_of(21));
                    if (cut != omega::TrendAccountingGuard::Cut::NONE) {
                        _close(bid, omega::TrendAccountingGuard::reason(cut), now_ms, on_close);
                        return sig;
                    }
                }
                // S-2026-06-29 exit: RIDE (structural Donchian-n_out low) vs the legacy
                // fixed 20-bar TIMEOUT. ride_exit lets winners run the fat tail (validated
                // PF lift NDX 1.92->2.97 with the guard above); a wide safety cap (3x hold)
                // still backstops a position that never trips the channel.
                if (pos_.active) {
                    if (p.ride_exit) {
                        if (n_prior >= p.n_out && bar_close < prior_low)
                            _close(bid, "DONCH_RIDE_EXIT", now_ms, on_close);
                        else if (pos_.bars_held >= p.hold_max_bars * 3)
                            _close(bid, "RIDE_SAFETY_CAP", now_ms, on_close);
                    } else if (pos_.bars_held >= p.hold_max_bars) {
                        _close(bid, "TIMEOUT", now_ms, on_close);
                    }
                }
            }

            d1_acc_.bucket_ms=bucket;
            d1_acc_.open=mid; d1_acc_.high=mid; d1_acc_.low=mid; d1_acc_.close=mid;
        } else {
            if (mid > d1_acc_.high) d1_acc_.high = mid;
            if (mid < d1_acc_.low)  d1_acc_.low  = mid;
            d1_acc_.close = mid;
        }
        return sig;
    }

    void check_weekend_close(double bid, double ask, int64_t now_ms,
                             CloseCallback on_close) noexcept {
        if (!pos_.active || !p.weekend_close_gate) return;
        const int64_t utc_sec = now_ms / 1000LL;
        const int dow = static_cast<int>((utc_sec / 86400LL + 3) % 7);
        const int hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
        if (dow != 4 || hour < 20) return;
        const double mid = (bid+ask)*0.5;
        if (mid > pos_.entry) _close(bid, "WEEKEND_CLOSE", now_ms, on_close);
    }

    void cancel() noexcept { pos_ = OpenPos{}; }

    // Warm-seed historical D1 bars directly into deques + ATR state (bypasses
    // tick aggregation). Called once at startup from engine_init.hpp so the
    // 20-bar Donchian + 14-bar ATR are HOT on first live tick instead of
    // cold-warming ~20 trading days. Mirrors Ger40TurtleH4Engine::seed_from_h4_csv.
    // CSV format: ts_ms,open,high,low,close
    size_t seed_from_d1_csv(const std::string& path) noexcept {
        const std::string actual = omega::resolve_seed_path(path);
        std::ifstream f(actual);
        if (!f.is_open()) {
            omega::seed_die("NasTurtleD1", actual);  // [[noreturn]]
        }
        std::string line; std::getline(f, line); // header
        size_t n = 0;
        while (std::getline(f, line)) {
            long long ts_ms_ll=0; double o=0,h=0,l=0,c=0;
            if (sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf",
                       &ts_ms_ll, &o, &h, &l, &c) == 5) {
                d1_highs_.push_back(h);
                d1_lows_.push_back(l);
                d1_closes_.push_back(c);
                const int keep = std::max(p.lookback_bars, p.atr_period) + 2;
                while ((int)d1_highs_.size() > keep) {
                    d1_highs_.pop_front();
                    d1_lows_ .pop_front();
                    d1_closes_.pop_front();
                }
                _update_atr_on_bar_close(h, l, c);
                ++bar_count_;
                ++n;
            }
        }
        if (n < static_cast<size_t>(p.lookback_bars)) {
            printf("[SEED-FATAL] NasTurtleD1: only %zu rows in %s (need >= %d)\n",
                   n, actual.c_str(), p.lookback_bars);
            fflush(stdout);
            omega::seed_die("NasTurtleD1", actual);  // [[noreturn]]
        }
        printf("[SEED] NasTurtleD1: %zu D1 bars -> hot (atr=%.2f bars=%d) [%s]\n",
               n, atr_, bar_count_, actual.c_str());
        fflush(stdout);
        return n;
    }

    void _update_atr_on_bar_close(double bar_h, double bar_l, double bar_c) noexcept {
        double tr = bar_h - bar_l;
        if (prev_d1_close_ > 0.0) {
            tr = std::max(tr, std::fabs(bar_h - prev_d1_close_));
            tr = std::max(tr, std::fabs(bar_l - prev_d1_close_));
        }
        prev_d1_close_ = bar_c;
        // EMA100 of daily closes (trend filter, SMA-seed then EMA)
        if (ema100_count_ < p.ema100_period) {
            ema100_ += bar_c; ++ema100_count_;
            if (ema100_count_ == p.ema100_period) ema100_ /= p.ema100_period;
        } else {
            const double a = 2.0 / (p.ema100_period + 1);
            ema100_ = a * bar_c + (1.0 - a) * ema100_;
        }
        if (atr_seed_count_ < p.atr_period) {
            atr_seed_sum_ += tr; ++atr_seed_count_;
            if (atr_seed_count_ == p.atr_period) atr_ = atr_seed_sum_ / p.atr_period;
        } else {
            atr_ = (atr_ * (p.atr_period - 1) + tr) / p.atr_period;
        }
    }

    void _close(double exit_px, const char* reason, int64_t now_ms,
                CloseCallback on_close) noexcept {
        if (!pos_.active) return;
        const double pts_move = exit_px - pos_.entry;
        const double pnl_dollars = pts_move * pos_.lot * p.dollars_per_pt;

        printf("[NAS_TURTLE_D1] EXIT reason=%s entry=%.2f exit=%.2f pts=%.2f pnl=%.2f bars=%d%s\n",
               reason, pos_.entry, exit_px, pts_move, pnl_dollars, pos_.bars_held,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        TradeRecord tr{};
        // 2026-06-24: tag per-symbol instead of the hardcoded "NAS100"/"NasTurtleD1".
        // All three instances (g_spx_turtle_d1=US500.F, g_dj30_turtle_d1=DJ30.F,
        // g_nas_turtle_d1=NAS100) share this class AND shared one literal tag -> the
        // AUDITED_CONFIGS-EDGE SpxTurtleD1/Dj30TurtleD1 were indistinguishable in the
        // ledger from the MARGINAL NasTurtleD1, so the live-book allowlist could not
        // select them. Use the configured `symbol` member.
        tr.symbol=symbol; tr.side="LONG";
        tr.entryPrice=pos_.entry; tr.exitPrice=exit_px;
        tr.tp=pos_.tp; tr.sl=pos_.sl; tr.size=pos_.lot;
        tr.pnl=pnl_dollars; tr.mfe=pos_.mfe; tr.mae=pos_.mae;
        tr.entryTs=pos_.entry_ts_ms/1000; tr.exitTs=now_ms/1000;
        tr.exitReason=reason; tr.engine=std::string("NasTurtleD1_")+symbol;
        if (on_close) on_close(tr);
        pos_ = OpenPos{};
    }
};

} // namespace omega

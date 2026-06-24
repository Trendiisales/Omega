#pragma once
//  ADVERSE-PROTECTION: fixed SL-TP OCO bracket -- the hard SL (entry -/+ stop_atr*ATR, set at fill, checked every tick in on_tick) IS the in-flight protection, plus WEEKEND_CLOSE + hold_max_bars TIMEOUT; gold M30 instance CULLED 2026-06-17 (engine_init.hpp: ledger_analytics n=8 NEG-expectancy net -$123 expR -0.51, bar-replay PF1.6-1.9 overstatement, enabled=false) and M15 sibling DEAD (CULL_LEDGER.tsv 2026-06-15) -- not live, protection moot, re-enable blocked (backfill S-2026-06-24n)
// =============================================================================
// XauStraddleM30Engine.hpp -- single-shot OCO breakout straddle on XAUUSD M30.
//
// Origin: the "Quantum Dark Gold" EA is a dual-pending Buy-Stop/Sell-Stop M5
// scalper with a martingale grid (~12 trades). Research (2026-06-02,
// backtest/straddle_breakout_sweep.cpp on the 2yr XAUUSD tick corpus) showed
// the straddle ENTRY is sound but its "death by a thousand cuts" is the M5
// timeframe (3845 near-cost trades the grid was masking). Lifting the IDENTICAL
// straddle to M30 + a 1R fixed TP removes the bleed entirely -- NO GRID needed:
//   M30 boxN15 stop3*ATR TP=1R symmetric: OOS PF 1.64-1.90, Sharpe 4-6, WR ~62%,
//   survives 3x cost (PF 1.50). Both legs profitable (long + short momentum).
//
// Mechanics (faithful to the validated harness):
//   - on_m30_bar: roll a box = prior boxN M30 high/low (exclude current), update
//     Wilder ATR14, and (if flat) ARM Buy-Stop = box_high, Sell-Stop = box_low.
//   - on_tick: while ARMED + flat, fill intrabar at whichever stop price trades
//     through first (OCO -- the other is cancelled). Single position only.
//     Manage SL (entry -/+ stop_atr*ATR) and fixed TP (1R = stop distance).
//   - NO grid, NO averaging-in, NO re-entry stacking. One position, hard SL/TP.
//
// Symmetric by design (BOTH legs validated net-positive at M30 -- the short leg
// is a momentum breakdown, NOT the dead mean-rev short). Shadow until the
// auto-demote gate (EngineGate) judges it on >=30 live trades.
// =============================================================================
#include <string>
#include <deque>
#include <functional>
#include "OpenPositionRegistry.hpp"   // S-2026-06-03: omega::PositionSnapshot for persist
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"

namespace omega {

struct XauStraddleM30Engine {
    bool   shadow_mode = true;
    bool   enabled     = false;

    // ---- params (validated optimum) ----
    int    box_n       = 15;     // box lookback in M30 bars
    int    atr_period  = 14;
    double stop_atr    = 3.0;    // SL distance = stop_atr * ATR
    double tp_r        = 1.0;    // TP = tp_r * SL distance (1R)
    double max_spread  = 1.0;    // pts
    double lot         = 0.01;
    // Partial scale-out (S-2026-06-03): bank partial_frac of the position at
    // +partial_r * 1R, run the rest to TP. Backtest (2yr gold M30): 30%@0.5R ->
    // PF 1.58->1.65, Sharpe 3.94->4.29, maxDD -33%, net -9% (risk-adjusted win;
    // unlike BE/trail which were net-negative). Default OFF (frac=0).
    double partial_frac = 0.0;   // 0 = off
    double partial_r    = 0.5;   // bank at this *R favorable
    double gap_atr_max = 3.0;    // reject a stop gapped > this*ATR from mid (stale-box / gap phantom guard).
                                 // A real breakout fills AT the box edge (|fill-mid|~=0); 3*ATR is already far
                                 // beyond any legit fill, but << a stale-seed-box gap (hundreds of pts). 8.0 was
                                 // too loose -- a 204pt stale gap == ~8*ATR(25) slipped through (2026-06-01).

    // S-2026-06-01: OBI sizing overlay (SHADOW measurement). When the smoothed
    // order-book imbalance agrees with the breakout side, size up; when it fights,
    // size down. Validated at the SIGNAL level (l2_obi_replay.cpp: OBI 54-56%
    // directional, large n) but NOT yet on straddle entries (too sparse to prove
    // offline) -- so this runs in shadow and the live ledger/gate measures whether
    // the tilt lifts shadow PnL before it ever sizes real capital. obi_dir is set
    // each tick from g_macro_ctx.gold_obi_dir by the tick_gold dispatch; 0 = stale
    // book or below threshold -> no tilt.
    bool   obi_tilt = false;     // overlay on/off (engine_init sets true for shadow)
    int    obi_dir  = 0;         // +1/-1/0, refreshed per tick from the OBI signal
    double obi_up   = 1.25;      // lot x when book agrees with the trade side
    double obi_dn   = 0.75;      // lot x when book disagrees

    // S-2026-06-02: self-aggregation for symbols WITHOUT an external bar feed
    // (the index straddle cells). When tf_min > 0, call on_tick_agg() per tick and
    // the engine builds its own tf_min-minute bars from the mid, drives the box
    // roll on bar close, then manages the position -- no tick_*.hpp bar aggregator
    // needed. Gold leaves tf_min=0 and keeps feeding on_m30_bar externally.
    int     tf_min = 0;          // 0 = external bars (gold); >0 = self-aggregate
    int64_t agg_bar_ms_ = -1;
    double  agg_h_ = 0.0, agg_l_ = 0.0, agg_c_ = 0.0;
    int    hold_max_bars = 48;   // safety timeout (24h on M30)

    std::string symbol  = "XAUUSD";
    std::string engine_name = "XauStraddleM30";  // override per instance (M15 sibling)
    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // ---- state ----
    std::deque<double> highs_, lows_;
    double atr_ = 0.0; int atr_seed_n_ = 0; double atr_seed_sum_ = 0.0;
    double prev_close_ = 0.0;
    int    bar_count_ = 0;

    bool   armed_ = false;
    double buy_stop_ = 0.0, sell_stop_ = 0.0;

    struct OpenPos {
        bool   active = false;
        int    side = 0;            // +1 long, -1 short
        double entry = 0.0, sl = 0.0, tp = 0.0, lot = 0.0, sl_dist = 0.0;
        double mfe = 0.0;
        int64_t entry_ts_ms = 0;
        int    bars_held = 0;
        bool   part_taken = false;   // partial scale-out already banked
    } pos_;
    int trade_id_ = 0;

    bool has_open_position() const noexcept { return pos_.active; }

    // S-2026-06-13u: public force-close so the AccountingGuard closer + the
    // book-wide cascades can flatten this position (wraps the private _close).
    void force_close(double bid, double ask, CloseCallback cb, const char* reason) noexcept {
        if (!pos_.active) return;
        const double px = pos_.side > 0 ? bid : ask;
        _close(px, reason, (int64_t)std::time(nullptr) * 1000, cb);
    }

    // S-2026-06-03: persistence batch 4 — resume the filled OCO position across
    // restart (the armed/pending re-arms next bar anyway). Covers all straddle
    // instances (gold + index) since they share this class.
    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const {
        if (!pos_.active) return false;
        o.engine = eng; o.symbol = sym; o.side = pos_.side > 0 ? "LONG" : "SHORT";
        o.size = pos_.lot; o.entry = pos_.entry; o.sl = pos_.sl; o.tp = pos_.tp;
        o.entry_ts = pos_.entry_ts_ms / 1000;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        pos_.active = true; pos_.side = (ps.side == "LONG") ? 1 : -1;
        pos_.entry = ps.entry; pos_.sl = ps.sl; pos_.tp = ps.tp; pos_.lot = ps.size;
        pos_.sl_dist = std::fabs(pos_.entry - pos_.sl); pos_.entry_ts_ms = ps.entry_ts * 1000;
        // part_taken is not in the snapshot. Treat a restored position as
        // already-banked: the partial is one-shot by design, and without this a
        // restart re-banked partial_frac of the REMAINDER at the same level
        // (observed 2026-06-12: M15 4201.33 long banked 0.0030 at 15:24, then
        // 0.0021 again at the 18:39 deploy-restart -- same exit px 4225.3672).
        // Conservative direction: restored runner runs straight to TP/SL.
        pos_.part_taken = true;
        return true;
    }

    void _update_atr(double h, double l, double c) noexcept {
        if (prev_close_ <= 0.0) { prev_close_ = c; return; }
        const double tr = std::max({h - l, std::fabs(h - prev_close_), std::fabs(l - prev_close_)});
        if (atr_seed_n_ < atr_period) {
            atr_seed_sum_ += tr; ++atr_seed_n_;
            if (atr_seed_n_ == atr_period) atr_ = atr_seed_sum_ / atr_period;
        } else {
            atr_ = (atr_ * (atr_period - 1) + tr) / atr_period;
        }
        prev_close_ = c;
    }

    void _close(double exit_px, const char* reason, int64_t now_ms, CloseCallback cb) noexcept {
        const double pnl = pos_.side * (exit_px - pos_.entry) * pos_.lot;
        omega::TradeRecord tr{};
        tr.symbol     = symbol;
        tr.side       = pos_.side > 0 ? "LONG" : "SHORT";
        tr.engine     = engine_name;
        tr.exitReason = reason;
        tr.entryPrice = pos_.entry;
        tr.exitPrice  = exit_px;
        tr.sl         = pos_.sl;
        tr.tp         = pos_.tp;
        tr.size       = pos_.lot;
        tr.pnl        = pnl;
        tr.entryTs    = pos_.entry_ts_ms / 1000LL;
        tr.exitTs     = now_ms / 1000LL;
        tr.mfe        = pos_.mfe;
        tr.atr_at_entry = pos_.sl_dist / std::max(stop_atr, 1e-9);
        tr.shadow     = shadow_mode;
        std::printf("[%s] CLOSE %s @ %.2f entry=%.2f pnl=%.2f %s%s\n",
                    engine_name.c_str(), tr.side.c_str(), exit_px, pos_.entry, pnl, reason,
                    shadow_mode ? " [SHADOW]" : "");
        std::fflush(stdout);
        if (cb) cb(tr);
        pos_ = OpenPos{};
    }

    // Partial scale-out: bank partial_frac of the lot at exit_px, keep the rest
    // open to run to TP/SL. Emits a PARTIAL_TP TradeRecord; does NOT reset pos_.
    void _partial_close(double exit_px, int64_t now_ms, CloseCallback cb) noexcept {
        const double closed_lot = pos_.lot * partial_frac;
        if (closed_lot <= 0.0) { pos_.part_taken = true; return; }
        const double pnl = pos_.side * (exit_px - pos_.entry) * closed_lot;
        omega::TradeRecord tr{};
        tr.symbol = symbol; tr.side = pos_.side > 0 ? "LONG" : "SHORT";
        tr.engine = engine_name; tr.exitReason = "PARTIAL_TP";
        tr.entryPrice = pos_.entry; tr.exitPrice = exit_px;
        tr.sl = pos_.sl; tr.tp = pos_.tp; tr.size = closed_lot; tr.pnl = pnl;
        tr.entryTs = pos_.entry_ts_ms / 1000LL; tr.exitTs = now_ms / 1000LL;
        tr.mfe = pos_.mfe; tr.atr_at_entry = pos_.sl_dist / std::max(stop_atr, 1e-9);
        tr.shadow = shadow_mode;
        std::printf("[%s] PARTIAL %s %.0f%% @ %.2f entry=%.2f pnl=%.2f%s\n",
                    engine_name.c_str(), tr.side.c_str(), partial_frac * 100.0,
                    exit_px, pos_.entry, pnl, shadow_mode ? " [SHADOW]" : "");
        std::fflush(stdout);
        if (cb) cb(tr);
        pos_.lot -= closed_lot;        // remainder runs to TP/SL
        pos_.part_taken = true;
    }

    // Weekend-flat (S-2026-06-13p): force-close any open position at/after
    // Friday 20:45 UTC and take no new fills until the weekend gap. Backtested
    // 2yr gold M15 prod config vs hold-across: net -2.5%, PF 1.60->1.57,
    // maxDD IMPROVES 316->289, OOS half agrees (1.79 vs 1.76) -- edge-neutral
    // gap insurance. dow formula: (days+4)%7 -> Sun=0..Fri=5 (epoch = Thu).
    bool weekend_flat = true;

    bool _weekend_window(int64_t now_ms) const noexcept {
        if (!weekend_flat) return false;
        const int dow = (int)((now_ms / 86400000LL + 4) % 7);   // Sun=0..Fri=5
        const int mod = (int)((now_ms / 60000LL) % 1440);
        return dow == 5 && mod >= 20 * 60 + 45;                  // Fri >= 20:45 UTC
    }

    // ---- tick: fill armed straddle (OCO) + manage open pos ----
    void on_tick(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (bid <= 0.0 || ask <= 0.0) return;

        if (_weekend_window(now_ms)) {
            if (pos_.active) _close((bid + ask) * 0.5, "WEEKEND_CLOSE", now_ms, cb);
            return;   // no new fills into the gap
        }

        if (pos_.active) {
            const double mid = (bid + ask) * 0.5;
            const double move = pos_.side * (mid - pos_.entry);
            if (move > pos_.mfe) pos_.mfe = move;
            // partial scale-out: bank partial_frac at +partial_r*1R, run the rest
            if (partial_frac > 0.0 && !pos_.part_taken && pos_.sl_dist > 0.0
                && move >= partial_r * pos_.sl_dist) {
                _partial_close(pos_.entry + pos_.side * partial_r * pos_.sl_dist, now_ms, cb);
            }
            if (pos_.side > 0) {
                if (bid <= pos_.sl)      _close(bid, "SL_HIT", now_ms, cb);
                else if (bid >= pos_.tp) _close(bid, "TP_HIT", now_ms, cb);
            } else {
                if (ask >= pos_.sl)      _close(ask, "SL_HIT", now_ms, cb);
                else if (ask <= pos_.tp) _close(ask, "TP_HIT", now_ms, cb);
            }
            return;
        }

        if (!enabled || !armed_ || atr_ <= 0.0) return;
        if ((ask - bid) > max_spread) return;

        // OCO intrabar fill: long if ask trades >= buy_stop, short if bid <= sell_stop.
        int side = 0; double fill = 0.0;
        if (ask >= buy_stop_ && buy_stop_ > 0.0)      { side = +1; fill = buy_stop_; }
        else if (bid <= sell_stop_ && sell_stop_ > 0.0) { side = -1; fill = sell_stop_; }
        if (side == 0) return;

        // gap guard: a stop already gapped far through the market (stale warm-seed
        // box, or a large overnight gap) must NOT fill at the phantom stop price --
        // that produces a fake 0-second TP (e.g. seed box ~4716 vs live gold ~4482).
        const double mid_fill = (bid + ask) * 0.5;
        if (std::fabs(fill - mid_fill) > gap_atr_max * atr_) { armed_ = false; return; }

        const double sl_dist = stop_atr * atr_;
        const double tp_dist = tp_r * sl_dist;
        if (sl_dist <= 0.0) return;
        // entry cost gate (same unified layer as the C1/bracket engines)
        if (!ExecutionCostGuard::is_viable(symbol.c_str(), (ask - bid), tp_dist, lot, 1.5))
            return;

        // OBI sizing tilt (overlay): scale lot by book agreement. Logged so the
        // ledger/gate can compare agree vs disagree vs neutral outcomes.
        double use_lot = lot; int obi_agree = 0;
        if (obi_tilt && obi_dir != 0) {
            if (obi_dir == side) { use_lot = lot * obi_up; obi_agree = +1; }
            else                 { use_lot = lot * obi_dn; obi_agree = -1; }
        }

        pos_.active = true; pos_.side = side; pos_.entry = fill;
        pos_.sl = fill - side * sl_dist;
        pos_.tp = fill + side * tp_dist;
        pos_.lot = use_lot; pos_.sl_dist = sl_dist; pos_.mfe = 0.0;
        pos_.entry_ts_ms = now_ms; pos_.bars_held = 0;
        armed_ = false;   // one shot until next bar re-arms
        ++trade_id_;
        std::printf("[%s] ENTRY %s @ %.2f sl=%.2f tp=%.2f atr=%.2f lot=%.4f obi=%d(%s)%s\n",
                    engine_name.c_str(), side > 0 ? "LONG" : "SHORT", fill, pos_.sl, pos_.tp,
                    atr_, use_lot, obi_dir,
                    obi_agree > 0 ? "AGREE" : obi_agree < 0 ? "DISAGREE" : "neutral",
                    shadow_mode ? " [SHADOW]" : "");
        std::fflush(stdout);
    }

    // ---- M30 bar close: roll box + ATR, re-arm straddle ----
    void on_m30_bar(double m30_high, double m30_low, double m30_close,
                    double bid, double ask, int64_t bar_close_ms,
                    CloseCallback cb) noexcept {
        highs_.push_back(m30_high);
        lows_ .push_back(m30_low);
        const int keep = box_n + 2;
        while ((int)highs_.size() > keep) { highs_.pop_front(); lows_.pop_front(); }
        _update_atr(m30_high, m30_low, m30_close);
        ++bar_count_;

        if (pos_.active) {
            ++pos_.bars_held;
            if (pos_.bars_held >= hold_max_bars)
                _close((bid + ask) * 0.5, "TIMEOUT", bar_close_ms, cb);
        }

        armed_ = false;
        if (!enabled || pos_.active || atr_ <= 0.0) return;
        if ((int)highs_.size() < box_n + 1) return;

        // box = prior box_n bars EXCLUDING the just-closed current bar
        const int sz = (int)highs_.size();
        double bh = highs_[sz - 1 - box_n], bl = lows_[sz - 1 - box_n];
        for (int i = sz - box_n; i < sz - 1; ++i) {
            if (highs_[i] > bh) bh = highs_[i];
            if (lows_[i]  < bl) bl = lows_[i];
        }
        buy_stop_ = bh; sell_stop_ = bl;
        // Guard #2 (ATR-INDEPENDENT): only arm when the box straddles the current
        // market. A box sitting entirely above/below price (stale warm-seed box, or
        // a large gap) has one stop already through the market -> it would fill
        // instantly at the phantom stop price. This catches the stale-box case
        // regardless of ATR (the on_tick gap guard is the second line). The only
        // legit case it skips is a bar that closed fully outside its own box (rare
        // runaway breakout) -- it simply re-arms next bar, negligible vs validated.
        const double mid_arm = (bid > 0.0 && ask > 0.0) ? (bid + ask) * 0.5 : m30_close;
        armed_ = (buy_stop_ > mid_arm && sell_stop_ < mid_arm);
    }

    // ---- self-aggregating tick (tf_min>0): build tf_min bars from mid, roll the
    //      box on bar close, then manage. For index cells with no external bar feed.
    void on_tick_agg(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (tf_min <= 0 || bid <= 0.0 || ask <= 0.0) { on_tick(bid, ask, now_ms, cb); return; }
        const double mid = (bid + ask) * 0.5;
        const int64_t tfms = (int64_t)tf_min * 60000LL;
        const int64_t b = (now_ms / tfms) * tfms;
        if (agg_bar_ms_ < 0) { agg_bar_ms_ = b; agg_h_ = agg_l_ = agg_c_ = mid; }
        else if (b != agg_bar_ms_) {
            on_m30_bar(agg_h_, agg_l_, agg_c_, bid, ask, agg_bar_ms_, cb);  // close prior bar
            agg_bar_ms_ = b; agg_h_ = agg_l_ = agg_c_ = mid;
        } else {
            if (mid > agg_h_) agg_h_ = mid;
            if (mid < agg_l_) agg_l_ = mid;
            agg_c_ = mid;
        }
        on_tick(bid, ask, now_ms, cb);   // fill armed straddle + manage open pos
    }

    // ---- warm-seed: replay M30 bars (bar_start_ms,open,high,low,close) ----
    int seed_from_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[%s] SEED FAIL '%s'\n", engine_name.c_str(), path.c_str()); return 0; }
        const bool was = enabled; enabled = false;   // no fills during seed
        std::string line; std::getline(f, line);      // header
        int fed = 0;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == 'b') continue;
            std::stringstream ss(line); std::string a,o,h,l,c;
            std::getline(ss,a,','); std::getline(ss,o,','); std::getline(ss,h,',');
            std::getline(ss,l,','); std::getline(ss,c,',');
            if (h.empty()||l.empty()||c.empty()) continue;
            double hi=std::strtod(h.c_str(),nullptr), lo=std::strtod(l.c_str(),nullptr), cl=std::strtod(c.c_str(),nullptr);
            int64_t ms=std::strtoll(a.c_str(),nullptr,10);
            if (hi<=0||lo<=0||cl<=0) continue;
            on_m30_bar(hi, lo, cl, cl, cl, ms, CloseCallback{});
            ++fed;
        }
        enabled = was;
        std::printf("[%s] SEED fed=%d atr=%.3f bars=%d box[%.2f,%.2f]\n",
                    engine_name.c_str(), fed, atr_, (int)highs_.size(), sell_stop_, buy_stop_);
        std::fflush(stdout);
        return fed;
    }
};

} // namespace omega

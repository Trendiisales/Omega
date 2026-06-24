#pragma once
//  ADVERSE-PROTECTION: in-flight protection = fixed SL/TP bracket (sl=impulse-candle low, tp=2.5R) + maxStop=1.0ATR risk-cap (rejects wide-stop setups) + FLAT_EOD time-stop @ flat_min(16:00 UTC); no LOSS_CUT_PCT/BE ratchet. Moot: enabled=false (struct default) and CULL-CONFIRMED dead -- full-span 2022-bear+2024-26 NAS every config PF<1.1 (documented winner PF0.93), live shadow ledger -$253/4 closes 0% WR (engine_init.hpp ~L3947, memory omega-peachy-onecandle-orb-deadend); re-enable blocked pending cross-regime walk-forward proof. (backfill S-2026-06-24n)
// =============================================================================
// PeachyOrbEngine.hpp -- "one-candle confirmation" ORB-retest, m5-native, self-
// contained (aggregates its own m5 bars from ticks; no external bar feed).
//
// Origin: backtest/peachy_orb_nas.cpp + peachy_sweep.sh (2026-06-05). Distilled
// from the Peachy/PG "one-candle theory" YouTube walkthrough on NQ. The LITERAL
// 3 rules (strong body + close beyond level + rising volume) backtested DEAD on
// NAS (WF half-flip). The real edge is her unstated DISCRETION: she only takes
// ~20-30pt-stop setups and skips the rest. Mechanizing that risk-selectivity
// (MAXSTOP risk-cap = the load-bearing lever) flips it to a robust edge.
//
// VALIDATED CONFIG C (NAS100, long-only):
//   OR = first 15 min of NY cash open (13:30-13:45 UTC); breakout 5m candle must
//   CLOSE above OR_high by >= CLOSE_BUF*ATR with body/range >= BODY_FRAC and a
//   small upper (push-side) wick; enter on the RETEST 0.3 into the breakout
//   candle; stop = impulse-candle low; REJECT the setup if that stop > MAXSTOP*
//   ATR (her risk-cap); only long when EMA(100) of the 5m close is rising; TP =
//   2.5R; one shot/day; flat 16:00 UTC.
//     BULL (16mo NAS): n=103 PF 2.19 (H1 2.06/H2 2.34), 9/9 param plateau,
//       3x-cost-robust; edge holds with trend filter OFF (not bull-riding).
//     BEAR (real 2022 NDX, -30%/33%DD): PF 2.25 net+313 maxDD only 91pt, 3x:1.97
//       -- long-only catches bear-bounces; the tight stop keeps losers small.
//   ==> bull AND bear robust. Volume filter does NOTHING on proxy data (real
//       CME volume untested) -- so NO volume gate here; the risk-cap is the edge.
//
// Self-simulating SHADOW cell (like OrbBreakoutEngine / ConnorsRSI2Engine):
// reports closed trades via on_trade_record -> handle_closed_trade -> ledger/
// gate; it NEVER touches the live order path. shadow_mode=true makes it sim-only.
// EngineGate judges it on >=30 live shadow trades.
// =============================================================================
#include <string>
#include <deque>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"
#include "ClusterGate.hpp"   // cross-engine same-direction cluster cap (S-2026-06-11)

namespace omega {

struct PeachyOrbEngine {
    bool   shadow_mode = true;
    bool   enabled     = false;

    // ---- params (validated config C) ----
    int    or_start_min  = 810;   // 13:30 UTC -- NY cash open (OR window start, minute-of-day)
    int    or_end_min    = 825;   // 13:45 UTC -- OR window end (exclusive) = first 15 min
    int    flat_min      = 960;   // 16:00 UTC -- force flat (~noon ET), no overnight
    int    atr_period    = 14;
    int    ema_len       = 100;   // 5m-close EMA trend filter (only long when rising)
    double body_frac     = 0.60;  // breakout candle body/range minimum
    double close_buf_atr = 0.30;  // close must clear OR level by this * ATR
    double push_wick     = 0.40;  // max upper (push-side) wick, fraction of range
    double retr          = 0.30;  // retrace fraction into breakout candle for the limit entry
    double big_atr       = 2.00;  // candle range > big_atr*ATR -> deep retrace
    double deep          = 0.75;  // deep retrace fraction for large impulse candles
    double max_stop_atr  = 1.00;  // HER RISK-CAP: reject setup if stop > this * ATR (the lever)
    double tp_r          = 2.50;  // TP = tp_r * risk (ignored when trail_atr>0)
    double trail_atr     = 0.0;   // runner trail: 0=fixed TP; >0 = no TP, drag SL by trail_atr*ATR off each 5m close (harness-faithful)
    double max_spread    = 6.0;   // pts -- skip arming if spread too wide
    double lot           = 1.0;
    bool   long_only     = true;  // shorts dead on NAS (counter-trend on up-drift)

    std::string symbol      = "NAS100";
    std::string engine_name = "PeachyOrb";
    using TradeRecordCallback = std::function<void(const omega::TradeRecord&)>;
    TradeRecordCallback on_trade_record;
    bool verbose = false;

    // ---- m5 aggregation state ----
    int64_t bar_start_ms_ = -1;
    double  bar_o_ = 0.0, bar_h_ = 0.0, bar_l_ = 0.0, bar_c_ = 0.0;

    // ---- ATR (SMA of TR over atr_period) + EMA(ema_len) of close ----
    std::deque<double> trq_;
    double atr_ = 0.0, prev_close_ = 0.0;
    double ema_ = 0.0, ema_prev_ = 0.0; bool ema_init_ = false;

    // ---- per-day opening range / state ----
    int64_t cur_day_ = -1;
    double  or_high_ = 0.0, or_low_ = 1e18;
    bool    or_done_ = false, traded_ = false;

    // ---- pending retest order (armed after a qualified breakout candle) ----
    bool   armed_ = false; double arm_entry_ = 0.0, arm_sl_ = 0.0;

    // ---- open position ----
    struct OpenPos {
        bool   active = false;
        int    side = 0;
        double entry = 0.0, sl = 0.0, tp = 0.0, lot = 0.0, sl_dist = 0.0;
        double mfe = 0.0;
        int64_t entry_ts_ms = 0;
    } pos_;
    int trade_id_ = 0;

    bool has_open_position() const noexcept { return pos_.active; }

    static int  minute_of_day(int64_t ms) noexcept { int64_t s = ms/1000LL; return (int)((s % 86400LL)/60LL); }
    static int64_t day_of(int64_t ms) noexcept { return (ms/1000LL)/86400LL; }

    void _close(double exit_px, const char* reason, int64_t now_ms) noexcept {
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
        tr.atr_at_entry = atr_;
        tr.shadow     = shadow_mode;
        std::printf("[PEACHY_%s] CLOSE %s @ %.2f entry=%.2f pnl=%.2f %s%s\n",
                    engine_name.c_str(), tr.side.c_str(), exit_px, pos_.entry, pnl, reason,
                    shadow_mode ? " [SHADOW]" : "");
        std::fflush(stdout);
        if (on_trade_record) on_trade_record(tr);
        pos_ = OpenPos{};
    }

    // ---- finalize one m5 bar: roll ATR + EMA + OR, then detect a breakout candle ----
    void _on_m5_bar(double o, double h, double l, double c, int64_t bar_start_ms) noexcept {
        const int64_t day = day_of(bar_start_ms);
        const int     mod = minute_of_day(bar_start_ms);
        if (day != cur_day_) {                   // new UTC day -> reset the range/state
            cur_day_ = day;
            or_high_ = 0.0; or_low_ = 1e18; or_done_ = false; traded_ = false; armed_ = false;
        }
        // ATR = SMA of TR over atr_period (faithful to the harness)
        if (prev_close_ > 0.0) {
            const double tr = std::max({h - l, std::fabs(h - prev_close_), std::fabs(l - prev_close_)});
            trq_.push_back(tr);
            while ((int)trq_.size() > atr_period) trq_.pop_front();
            double s = 0.0; for (double v : trq_) s += v;
            atr_ = s / (double)trq_.size();
        }
        prev_close_ = c;
        // EMA(ema_len) of close + slope
        ema_prev_ = ema_;
        if (!ema_init_) { ema_ = c; ema_init_ = true; }
        else { const double k = 2.0 / (ema_len + 1); ema_ += k * (c - ema_); }

        // runner trail (harness-faithful): on each closed 5m bar, drag SL by trail_atr*ATR
        // off the close, ratchet-only. No fixed TP when trailing (gated in the tick manager).
        if (pos_.active && trail_atr > 0.0 && atr_ > 0.0) {
            const double t = pos_.side > 0 ? (c - trail_atr * atr_) : (c + trail_atr * atr_);
            if (pos_.side > 0 && t > pos_.sl) pos_.sl = t;
            if (pos_.side < 0 && t < pos_.sl) pos_.sl = t;
        }

        // accumulate the opening range
        if (mod >= or_start_min && mod < or_end_min) { if (h > or_high_) or_high_ = h; if (l < or_low_) or_low_ = l; return; }
        if (mod >= or_end_min && !or_done_ && or_high_ > 0.0 && or_low_ < 1e18) or_done_ = true;

        // only hunt for a breakout candle inside the trade window, one shot/day, flat post-window
        if (!or_done_ || traded_ || armed_ || pos_.active) return;
        if (mod < or_end_min || mod >= flat_min) return;
        if (atr_ <= 0.0) return;

        const double rng = h - l; if (rng <= 0.0) return;
        if (std::fabs(c - o) / rng < body_frac) return;          // strong trend-setting body

        int side = 0;
        if (c > or_high_ + close_buf_atr * atr_ && c > o) {       // convincing close above OR high
            if ((h - c) <= push_wick * rng) side = +1;            // small upper (push-side) wick
        } else if (!long_only && c < or_low_ - close_buf_atr * atr_ && c < o) {
            if ((c - l) <= push_wick * rng) side = -1;
        }
        if (side == 0) return;

        // trend filter: only long when EMA rising (short when falling)
        const bool ema_up = ema_ > ema_prev_;
        if (side > 0 && !ema_up) return;
        if (side < 0 &&  ema_up) return;

        // arm the retest limit into the breakout candle; deeper retrace for large impulse
        const double frac = (rng > big_atr * atr_) ? deep : retr;
        const double entry = side > 0 ? (h - frac * rng) : (l + frac * rng);
        const double sl    = side > 0 ? l : h;
        const double sd    = side > 0 ? (entry - sl) : (sl - entry);
        if (sd <= 0.05 * atr_) return;                            // degenerate
        if (max_stop_atr > 0.0 && sd > max_stop_atr * atr_) return;  // HER RISK-CAP

        armed_ = true; arm_entry_ = entry; arm_sl_ = sl;
        // store side via sign of (entry-sl): for long entry>sl, short entry<sl
        if (verbose) {
            std::printf("[PEACHY_%s] ARM %s retest=%.2f sl=%.2f atr=%.2f or[%.2f,%.2f]\n",
                        engine_name.c_str(), side > 0 ? "LONG" : "SHORT", entry, sl, atr_, or_low_, or_high_);
            std::fflush(stdout);
        }
    }

    // ---- tick: aggregate m5, fill armed retest, manage open position ----
    void on_tick(double bid, double ask, int64_t now_ms) noexcept {
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid = (bid + ask) * 0.5;

        // rolling m5 bar from mid
        const int64_t b5 = (now_ms / 300000LL) * 300000LL;
        if (bar_start_ms_ < 0) { bar_start_ms_ = b5; bar_o_ = bar_h_ = bar_l_ = bar_c_ = mid; }
        else if (b5 != bar_start_ms_) {
            _on_m5_bar(bar_o_, bar_h_, bar_l_, bar_c_, bar_start_ms_);
            bar_start_ms_ = b5; bar_o_ = bar_h_ = bar_l_ = bar_c_ = mid;
        } else {
            if (mid > bar_h_) bar_h_ = mid; if (mid < bar_l_) bar_l_ = mid; bar_c_ = mid;
        }

        const int mod = minute_of_day(now_ms);

        // ---- manage open position intrabar ----
        if (pos_.active) {
            const double move = pos_.side * (mid - pos_.entry);
            if (move > pos_.mfe) pos_.mfe = move;
            if (pos_.side > 0) {
                if (bid <= pos_.sl)      { _close(pos_.sl, "SL_HIT", now_ms); return; }
                else if (trail_atr <= 0.0 && bid >= pos_.tp) { _close(pos_.tp, "TP_HIT", now_ms); return; }
            } else {
                if (ask >= pos_.sl)      { _close(pos_.sl, "SL_HIT", now_ms); return; }
                else if (trail_atr <= 0.0 && ask <= pos_.tp) { _close(pos_.tp, "TP_HIT", now_ms); return; }
            }
            if (mod >= flat_min) { _close(mid, "FLAT_EOD", now_ms); return; }
            return;
        }

        // ---- fill an armed retest order ----
        if (!enabled || !armed_ || traded_) return;
        if (mod >= flat_min) { armed_ = false; return; }
        const int side = arm_entry_ > arm_sl_ ? +1 : -1;
        const bool fill = side > 0 ? (bid <= arm_entry_) : (ask >= arm_entry_);
        if (!fill) return;
        if ((ask - bid) > max_spread) return;

        const double risk = side > 0 ? (arm_entry_ - arm_sl_) : (arm_sl_ - arm_entry_);
        if (risk <= 0.0) { armed_ = false; return; }
        // cost gate: TP distance = tp_r * risk (disarm on block, same as zero-risk path)
        if (!ExecutionCostGuard::is_viable(symbol.c_str(), ask - bid, tp_r * risk, lot, 1.5)) { armed_ = false; return; }
        if (!ClusterGate::allow_entry(symbol.c_str(), side > 0, engine_name.c_str())) { armed_ = false; return; }
        pos_.active = true; pos_.side = side; pos_.entry = arm_entry_;
        pos_.sl = arm_sl_; pos_.tp = arm_entry_ + side * tp_r * risk;
        pos_.lot = lot; pos_.sl_dist = risk; pos_.mfe = 0.0; pos_.entry_ts_ms = now_ms;
        armed_ = false; traded_ = true; ++trade_id_;
        std::printf("[PEACHY_%s] ENTRY %s @ %.2f sl=%.2f tp=%.2f atr=%.2f%s\n",
                    engine_name.c_str(), side > 0 ? "LONG" : "SHORT", pos_.entry, pos_.sl, pos_.tp,
                    atr_, shadow_mode ? " [SHADOW]" : "");
        std::fflush(stdout);
    }

    // ---- warm-seed: replay m5 bars (ts[ms|s],o,h,l,c[,v]) to warm ATR+EMA+today's OR ----
    int seed_from_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[PEACHY_%s] SEED FAIL '%s'\n", engine_name.c_str(), path.c_str()); return 0; }
        const bool was_enabled = enabled; enabled = false;   // no entries fire on historical bars
        std::string line; std::getline(f, line);             // header
        int fed = 0;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == 't' || line[0] == 'b') continue;
            std::stringstream ss(line); std::string a,o,h,l,c;
            std::getline(ss,a,','); std::getline(ss,o,','); std::getline(ss,h,',');
            std::getline(ss,l,','); std::getline(ss,c,',');
            if (o.empty()||h.empty()||l.empty()||c.empty()) continue;
            double op=std::strtod(o.c_str(),nullptr), hi=std::strtod(h.c_str(),nullptr),
                   lo=std::strtod(l.c_str(),nullptr), cl=std::strtod(c.c_str(),nullptr);
            int64_t ms=std::strtoll(a.c_str(),nullptr,10);
            if (ms < 100000000000LL) ms *= 1000LL;            // accept seconds
            if (hi<=0||lo<=0||cl<=0) continue;
            _on_m5_bar(op, hi, lo, cl, ms);
            ++fed;
        }
        enabled = was_enabled;
        std::printf("[PEACHY_%s] SEED fed=%d atr=%.3f ema=%.2f or_done=%d or[%.2f,%.2f]\n",
                    engine_name.c_str(), fed, atr_, ema_, (int)or_done_, or_low_, or_high_);
        std::fflush(stdout);
        return fed;
    }
};

} // namespace omega

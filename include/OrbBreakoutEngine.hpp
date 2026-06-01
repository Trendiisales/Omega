#pragma once
// =============================================================================
// OrbBreakoutEngine.hpp -- faithful Opening-Range-Breakout, m5-native, self-
// contained (aggregates its own m5 bars from ticks; no external bar feed).
//
// Origin: backtest/orb_multi_sweep.cpp (2026-06-02, 2yr m5, OOS + 3x-cost stress)
// swept multi-symbol ORB. Result mostly NEGATIVE -- FX dead, GER40/UK100/NAS
// dead or in-sample-only, gold ORB < the shipped rolling straddle. The ONE
// genuine OOS-robust survivor: ESTX50 LONG-ONLY.
//   ESTX50 long-only, OR = 07:00-08:00 UTC (first EU cash hour), enter on break
//   of OR_high + buf*ATR, hard SL = opposite OR edge (range, NOT ATR -- ATR
//   worse everywhere), TP = 2.0R, FLAT by 15:30 UTC (no overnight), one shot/day.
//   OOS folds @cost2.0: PF 1.27/1.30/1.30, Sharpe ~1.4, MDD ~200, win 46-48%.
//   Cost-sensitive (PF 1.35@cost1 -> 1.09@cost3) -- watch live ESTX50 cost.
//   Short leg DEAD on every symbol (counter-trend on up-drift) -> long_only.
//
// Mechanics (faithful to the validated harness):
//   - on_tick aggregates a rolling m5 bar from mid; on each m5 close it (a) rolls
//     the day's opening range over [or_start,or_end) minutes-of-day UTC, (b)
//     updates a 14-bar SMA-of-TR ATR (matches the harness, NOT Wilder).
//   - intrabar (tick) it arms a Buy-Stop at OR_high + buf*ATR; long-only fill when
//     ask trades through; SL = OR_low (range), TP = entry + 2R. One shot/day.
//   - FLAT at flat_min (close at mid). Single position, hard SL/TP, no re-entry.
//
// Self-simulating SHADOW cell (like XauStraddleM30Engine): reports closed trades
// through the CloseCallback only -- it does NOT touch the live order path. The
// EngineGate auto-demote table judges it on >=30 live shadow trades.
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

namespace omega {

struct OrbBreakoutEngine {
    bool   shadow_mode = true;
    bool   enabled     = false;

    // ---- params (validated ESTX50 optimum) ----
    int    or_start_min = 420;   // 07:00 UTC -- OR window start (minute-of-day)
    int    or_end_min   = 480;   // 08:00 UTC -- OR window end (exclusive)
    int    flat_min     = 930;   // 15:30 UTC -- force flat, no overnight
    int    atr_period   = 14;
    double buf_atr      = 0.05;  // breakout buffer = buf_atr * ATR
    double tp_r         = 2.0;   // TP = tp_r * risk (risk = entry - OR_low)
    double max_spread   = 3.0;   // pts
    double lot          = 0.01;
    bool   long_only    = true;  // short leg dead on every symbol

    std::string symbol      = "ESTX50";
    std::string engine_name = "OrbEstx50";
    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // ---- m5 aggregation state ----
    int64_t bar_start_ms_ = -1;
    double  bar_h_ = 0.0, bar_l_ = 0.0, bar_c_ = 0.0;

    // ---- ATR (SMA of TR over atr_period) ----
    std::deque<double> trq_;
    double atr_ = 0.0, prev_close_ = 0.0;

    // ---- per-day opening range ----
    int64_t cur_day_ = -1;
    double  or_high_ = 0.0, or_low_ = 1e18;
    bool    or_done_ = false, traded_ = false;

    // ---- open position ----
    struct OpenPos {
        bool   active = false;
        int    side = 0;            // +1 long, -1 short
        double entry = 0.0, sl = 0.0, tp = 0.0, lot = 0.0, sl_dist = 0.0;
        double mfe = 0.0;
        int64_t entry_ts_ms = 0;
    } pos_;
    int trade_id_ = 0;

    bool has_open_position() const noexcept { return pos_.active; }

    static int minute_of_day(int64_t ms) noexcept {
        int64_t s = ms / 1000LL; return (int)((s % 86400LL) / 60LL);
    }
    static int64_t day_of(int64_t ms) noexcept { return (ms / 1000LL) / 86400LL; }

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
        tr.atr_at_entry = atr_;
        tr.shadow     = shadow_mode;
        std::printf("[ORB_%s] CLOSE %s @ %.2f entry=%.2f pnl=%.2f %s%s\n",
                    engine_name.c_str(), tr.side.c_str(), exit_px, pos_.entry, pnl, reason,
                    shadow_mode ? " [SHADOW]" : "");
        std::fflush(stdout);
        if (cb) cb(tr);
        pos_ = OpenPos{};
    }

    // ---- finalize one m5 bar: roll OR + ATR ----
    void _on_m5_bar(double h, double l, double c, int64_t bar_start_ms) noexcept {
        const int64_t day = day_of(bar_start_ms);
        const int     mod = minute_of_day(bar_start_ms);
        if (day != cur_day_) {                  // new UTC day -> reset the range
            cur_day_ = day;
            or_high_ = 0.0; or_low_ = 1e18; or_done_ = false; traded_ = false;
        }
        // ATR = SMA of TR over atr_period (faithful to orb_multi_sweep.cpp)
        if (prev_close_ > 0.0) {
            const double tr = std::max({h - l, std::fabs(h - prev_close_), std::fabs(l - prev_close_)});
            trq_.push_back(tr);
            while ((int)trq_.size() > atr_period) trq_.pop_front();
            double s = 0.0; for (double v : trq_) s += v;
            atr_ = s / (double)trq_.size();
        }
        prev_close_ = c;

        if (mod >= or_start_min && mod < or_end_min) {   // accumulate opening range
            if (h > or_high_) or_high_ = h;
            if (l < or_low_)  or_low_  = l;
        }
        if (mod >= or_end_min && !or_done_ && or_high_ > 0.0 && or_low_ < 1e18)
            or_done_ = true;
    }

    // ---- tick: aggregate m5, manage open pos, arm + fill ORB ----
    void on_tick(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid = (bid + ask) * 0.5;

        // rolling m5 bar from mid
        const int64_t b5 = (now_ms / 300000LL) * 300000LL;
        if (bar_start_ms_ < 0) { bar_start_ms_ = b5; bar_h_ = bar_l_ = bar_c_ = mid; }
        else if (b5 != bar_start_ms_) {
            _on_m5_bar(bar_h_, bar_l_, bar_c_, bar_start_ms_);
            bar_start_ms_ = b5; bar_h_ = bar_l_ = bar_c_ = mid;
        } else {
            if (mid > bar_h_) bar_h_ = mid;
            if (mid < bar_l_) bar_l_ = mid;
            bar_c_ = mid;
        }

        const int mod = minute_of_day(now_ms);

        // ---- manage open position ----
        if (pos_.active) {
            const double move = pos_.side * (mid - pos_.entry);
            if (move > pos_.mfe) pos_.mfe = move;
            if (pos_.side > 0) {
                if (bid <= pos_.sl)      { _close(pos_.sl, "SL_HIT", now_ms, cb); return; }
                else if (bid >= pos_.tp) { _close(pos_.tp, "TP_HIT", now_ms, cb); return; }
            } else {
                if (ask >= pos_.sl)      { _close(pos_.sl, "SL_HIT", now_ms, cb); return; }
                else if (ask <= pos_.tp) { _close(pos_.tp, "TP_HIT", now_ms, cb); return; }
            }
            if (mod >= flat_min) { _close(mid, "FLAT_EOD", now_ms, cb); return; }
            return;
        }

        // ---- arm + fill (one shot/day, inside the trading window) ----
        if (!enabled || !or_done_ || traded_ || atr_ <= 0.0) return;
        if (mod < or_end_min || mod >= flat_min) return;
        if ((ask - bid) > max_spread) return;

        const double buf = buf_atr * atr_;
        int side = 0; double fill = 0.0, risk = 0.0;
        const double buy_stop  = or_high_ + buf;
        const double sell_stop = or_low_  - buf;
        if (ask >= buy_stop && buy_stop > 0.0) {        // long breakout
            side = +1; fill = buy_stop; risk = fill - or_low_;
        } else if (!long_only && bid <= sell_stop && sell_stop > 0.0) {  // short (off for ESTX50)
            side = -1; fill = sell_stop; risk = or_high_ - fill;
        }
        if (side == 0 || risk <= 0.0) return;

        const double tp_dist = tp_r * risk;
        if (!ExecutionCostGuard::is_viable(symbol.c_str(), (ask - bid), tp_dist, lot, 1.5))
            return;

        pos_.active = true; pos_.side = side; pos_.entry = fill;
        pos_.sl = fill - side * risk;
        pos_.tp = fill + side * tp_dist;
        pos_.lot = lot; pos_.sl_dist = risk; pos_.mfe = 0.0;
        pos_.entry_ts_ms = now_ms;
        traded_ = true;
        ++trade_id_;
        std::printf("[ORB_%s] ENTRY %s @ %.2f sl=%.2f tp=%.2f atr=%.2f or[%.2f,%.2f]%s\n",
                    engine_name.c_str(), side > 0 ? "LONG" : "SHORT", fill, pos_.sl, pos_.tp,
                    atr_, or_low_, or_high_, shadow_mode ? " [SHADOW]" : "");
        std::fflush(stdout);
    }

    // ---- warm-seed: replay m5 bars (ts[ms|s],o,h,l,c) to warm ATR + today's OR ----
    int seed_from_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[ORB_%s] SEED FAIL '%s'\n", engine_name.c_str(), path.c_str()); return 0; }
        std::string line; std::getline(f, line);   // header
        int fed = 0;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == 't' || line[0] == 'b') continue;
            std::stringstream ss(line); std::string a,o,h,l,c;
            std::getline(ss,a,','); std::getline(ss,o,','); std::getline(ss,h,',');
            std::getline(ss,l,','); std::getline(ss,c,',');
            if (h.empty()||l.empty()||c.empty()) continue;
            double hi=std::strtod(h.c_str(),nullptr), lo=std::strtod(l.c_str(),nullptr), cl=std::strtod(c.c_str(),nullptr);
            int64_t ms=std::strtoll(a.c_str(),nullptr,10);
            if (ms < 100000000000LL) ms *= 1000LL;   // accept seconds
            if (hi<=0||lo<=0||cl<=0) continue;
            _on_m5_bar(hi, lo, cl, ms);
            ++fed;
        }
        std::printf("[ORB_%s] SEED fed=%d atr=%.3f or_done=%d or[%.2f,%.2f]\n",
                    engine_name.c_str(), fed, atr_, (int)or_done_, or_low_, or_high_);
        std::fflush(stdout);
        return fed;
    }
};

} // namespace omega

// =============================================================================
//  TrendLineBreakEngine.hpp -- validated non-intersecting trend-line break
//
//  PROVENANCE (2026-06-09)
//  This is the C++ port of the backtest variant that actually showed an edge
//  (trendline_fan_v2.py): Tori's masterclass rule that a valid trend line is
//  one price has NOT poked through -- i.e. the convex hull of highs (resistance)
//  and lows (support). We trade the BREAK only, use the opposing hull line as
//  the SAFETY line (trailing stop), and exit when price closes through it.
//
//  Backtest (H4, ~2-3yr, R-multiples, spread friction), 2-touch variant:
//    GBPUSD PF 1.53 | USDJPY 1.37 | XAUUSD (Gold) 1.24 | DAX 1.17 ; others <1.
//  The earlier loose-line bounce/break engine was net-negative -- this replaces
//  it for the break edge. Modest edge: ship in SHADOW, prove live, then arm.
//
//  Interface matches Xau3BarMomGatedH4Engine.hpp so the shared warm-seed helper
//  omega::seed_h4_engine() and the H4 dispatch in tick_gold work unchanged:
//    on_h4_bar(h,l,c,bid,ask,bar_close_ms,cb)  -- state + break decision
//    on_tick(bid,ask,now_ms,cb)                -- intrabar safety-line stop
//    public bool enabled                       -- seed/off switch
//    emits omega::TradeRecord via CloseCallback ; ExecutionCostGuard-gated.
// =============================================================================
#pragma once
//  ADVERSE-PROTECTION: trail-only by design -- safety = opposing convex-hull line trailed on each H4 close (SAFETY_BREAK) + max_hold_bars=160 TIMEOUT; no LOSS_CUT/BE ratchet; shadow only (GBP enabled, JPY disabled 2026-06-11), header PFs are bar-replay -- no faithful backtest on record -- verdict owed before re-enable (backfill S-2026-06-24n)
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>
#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"
#include "SeedGuard.hpp"

namespace omega {

struct TrendLineBreakParams {
    int    window         = 120;   // H4 bars in view for the hull (~4 weeks)
    int    atr_period     = 14;
    int    min_touch      = 2;     // touches on the broken action line (gold: 2 > 3)
    double touch_tol_atr  = 0.40;  // high/low within this*ATR of the line = a touch
    double break_buf_atr  = 0.10;  // close must clear the line by this*ATR
    double body_atr       = 0.25;  // break candle body >= this*ATR
    double slope_atr_cap  = 0.50;  // |slope|/bar <= this*ATR  (~"under 45 deg")
    double min_risk_atr   = 0.50;  // entry->safety distance >= this*ATR (room)
    double max_risk_atr   = 3.00;  // entry->safety distance <= this*ATR (tight)
    int    max_hold_bars  = 160;
    int    cooldown_bars  = 1;
    double lot            = 0.01;
    double max_spread     = 1.0;
    double cost_ratio_min = 1.5;
};

struct TrendLineBreakSignal {
    bool        valid  = false;
    int         side   = 0;        // +1 long, -1 short
    double      entry  = 0.0;
    double      sl     = 0.0;
    double      lot    = 0.0;
    int         touches= 0;
    const char* reason = "";
};

struct TrendLineBreakEngine {
    bool shadow_mode = true;
    bool enabled     = false;
    TrendLineBreakParams p;
    std::string symbol = "XAUUSD";
    // Per-instance ledger/dashboard label. Running 2+ instances under one
    // hardcoded "TrendLineBreak" tag would collide all closed trades. Set this
    // per instance (e.g. "TrendLineBreakGBP") so the ledger + dashboard separate.
    std::string engine_name = "TrendLineBreak";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct Bar { long idx; double h, l, c; };
    struct Pt  { long x;   double y; };
    struct Edge {
        bool   ok = false;
        long   x0 = 0; double y0 = 0.0, slope = 0.0;
        double val(long x) const noexcept { return y0 + slope * (double)(x - x0); }
    };

    std::deque<Bar> win_;
    double atr_ = 0.0, atr_sum_ = 0.0; int atr_n_ = 0; double prevc_ = 0.0;
    double prev_bar_close_ = 0.0;
    long   bar_idx_ = 0;
    int    cooldown_ = 0, m_trade_id_ = 0;

    struct OpenPos {
        bool   active = false;
        int    side   = 0;
        double entry = 0, sl = 0, risk = 0, lot = 0, mfe = 0, atr_entry = 0;
        int64_t entry_ts_ms = 0;
        int    bars_held = 0;
    } pos_;

    bool has_open_position() const noexcept { return pos_.active; }

    // ---------------- ATR (Wilder) ----------------
    void _update_atr(double h, double l, double c) noexcept {
        if (prevc_ <= 0.0) { prevc_ = c; return; }
        const double tr = std::max({h - l, std::fabs(h - prevc_), std::fabs(l - prevc_)});
        if (atr_n_ < p.atr_period) { atr_sum_ += tr; if (++atr_n_ == p.atr_period) atr_ = atr_sum_ / p.atr_period; }
        else atr_ = (atr_ * (p.atr_period - 1) + tr) / p.atr_period;
        prevc_ = c;
    }

    // ---------------- convex hulls (non-intersecting lines) ----------------
    static double _cross(const Pt& o, const Pt& a, const Pt& b) noexcept {
        return (double)(a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (double)(b.x - o.x);
    }
    // upper hull: chain ABOVE all points (resistance)
    static Edge _upper_last_edge(const std::vector<Pt>& pts) noexcept {
        std::vector<Pt> h;
        for (const auto& q : pts) {
            while (h.size() >= 2 && _cross(h[h.size()-2], h[h.size()-1], q) >= 0.0) h.pop_back();
            h.push_back(q);
        }
        Edge e; if (h.size() < 2) return e;
        const Pt& a = h[h.size()-2]; const Pt& b = h[h.size()-1];
        if (b.x == a.x) return e;
        e.ok = true; e.x0 = a.x; e.y0 = a.y; e.slope = (b.y - a.y) / (double)(b.x - a.x);
        return e;
    }
    // lower hull: chain BELOW all points (support)
    static Edge _lower_last_edge(const std::vector<Pt>& pts) noexcept {
        std::vector<Pt> h;
        for (const auto& q : pts) {
            while (h.size() >= 2 && _cross(h[h.size()-2], h[h.size()-1], q) <= 0.0) h.pop_back();
            h.push_back(q);
        }
        Edge e; if (h.size() < 2) return e;
        const Pt& a = h[h.size()-2]; const Pt& b = h[h.size()-1];
        if (b.x == a.x) return e;
        e.ok = true; e.x0 = a.x; e.y0 = a.y; e.slope = (b.y - a.y) / (double)(b.x - a.x);
        return e;
    }
    // hulls over PRIOR bars only (exclude the just-closed current bar at back)
    Edge _resistance() const noexcept {
        std::vector<Pt> pts; pts.reserve(win_.size());
        for (size_t i = 0; i + 1 < win_.size(); ++i) pts.push_back(Pt{win_[i].idx, win_[i].h});
        return _upper_last_edge(pts);
    }
    Edge _support() const noexcept {
        std::vector<Pt> pts; pts.reserve(win_.size());
        for (size_t i = 0; i + 1 < win_.size(); ++i) pts.push_back(Pt{win_[i].idx, win_[i].l});
        return _lower_last_edge(pts);
    }
    int _touch_count_high(const Edge& e) const noexcept {
        if (!e.ok || atr_ <= 0.0) return 0; const double tol = p.touch_tol_atr * atr_; int n = 0;
        for (size_t i = 0; i + 1 < win_.size(); ++i)
            if (std::fabs(win_[i].h - e.val(win_[i].idx)) <= tol) ++n;
        return n;
    }
    int _touch_count_low(const Edge& e) const noexcept {
        if (!e.ok || atr_ <= 0.0) return 0; const double tol = p.touch_tol_atr * atr_; int n = 0;
        for (size_t i = 0; i + 1 < win_.size(); ++i)
            if (std::fabs(win_[i].l - e.val(win_[i].idx)) <= tol) ++n;
        return n;
    }

    // ---------------- close out ----------------
    void _close(double exit_px, const char* reason, int64_t now_ms, CloseCallback cb) noexcept {
        if (!pos_.active) return;
        const double pnl = pos_.side * (exit_px - pos_.entry) * pos_.lot;
        omega::TradeRecord tr{};
        tr.symbol = symbol; tr.side = pos_.side > 0 ? "LONG" : "SHORT";
        tr.engine = engine_name; tr.exitReason = reason;
        tr.entryPrice = pos_.entry; tr.exitPrice = exit_px;
        tr.sl = pos_.sl; tr.tp = 0.0; tr.size = pos_.lot; tr.pnl = pnl;
        tr.entryTs = pos_.entry_ts_ms / 1000LL; tr.exitTs = now_ms / 1000LL;
        tr.mfe = pos_.mfe; tr.atr_at_entry = pos_.atr_entry; tr.shadow = shadow_mode;
        if (cb) cb(tr);
        printf("[%s] CLOSE %s @ %.2f entry=%.2f pnl=%.2f reason=%s bars=%d%s\n",
               engine_name.c_str(), pos_.side > 0 ? "LONG" : "SHORT", exit_px, pos_.entry, pnl, reason,
               pos_.bars_held, shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);
        pos_ = OpenPos{}; cooldown_ = p.cooldown_bars;
    }

    // ---------------- intrabar safety-line stop ----------------
    void on_tick(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (!pos_.active || bid <= 0 || ask <= 0) return;
        const double mid = (bid + ask) * 0.5;
        const double mv = pos_.side * (mid - pos_.entry);
        if (mv > pos_.mfe) pos_.mfe = mv;
        if (pos_.side > 0) { if (bid <= pos_.sl) _close(bid, "SAFETY_BREAK", now_ms, cb); }
        else               { if (ask >= pos_.sl) _close(ask, "SAFETY_BREAK", now_ms, cb); }
    }
    void force_close(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (pos_.active) _close((bid + ask) * 0.5, "FORCE_CLOSE", now_ms, cb);
    }

    // ---------------- H4 bar ----------------
    TrendLineBreakSignal on_h4_bar(double h4_high, double h4_low, double h4_close,
                                   double bid, double ask, int64_t bar_close_ms,
                                   CloseCallback cb) noexcept {
        TrendLineBreakSignal sig{};
        ++bar_idx_;
        _update_atr(h4_high, h4_low, h4_close);
        win_.push_back(Bar{bar_idx_, h4_high, h4_low, h4_close});
        while ((int)win_.size() > p.window) win_.pop_front();
        const double body = std::fabs(h4_close - prev_bar_close_);
        prev_bar_close_ = h4_close;

        // manage: trail stop along the live opposing safety line, exit on close beyond it
        if (pos_.active) {
            ++pos_.bars_held;
            if (pos_.side > 0) {                          // safety = support
                Edge s = _support();
                if (s.ok) { double v = s.val(bar_idx_); if (v > pos_.sl) pos_.sl = v; }
                if (h4_close < pos_.sl) _close(h4_close, "SAFETY_BREAK", bar_close_ms, cb);
            } else {                                      // safety = resistance
                Edge r = _resistance();
                if (r.ok) { double v = r.val(bar_idx_); if (v < pos_.sl) pos_.sl = v; }
                if (h4_close > pos_.sl) _close(h4_close, "SAFETY_BREAK", bar_close_ms, cb);
            }
            if (pos_.active && pos_.bars_held >= p.max_hold_bars)
                _close((bid + ask) * 0.5, "TIMEOUT", bar_close_ms, cb);
        }

        if (cooldown_ > 0) --cooldown_;
        if (!enabled || pos_.active || cooldown_ > 0) return sig;
        if (atr_ <= 0.0 || (int)win_.size() < p.window) return sig;
        if ((ask - bid) > p.max_spread) return sig;
        if (body < p.body_atr * atr_) return sig;

        Edge res = _resistance(), sup = _support();
        if (!res.ok || !sup.ok) return sig;
        const double slope_cap = p.slope_atr_cap * atr_;

        // LONG: close breaks UP through a descending resistance; safety = support
        if (res.slope <= 0.0 && std::fabs(res.slope) <= slope_cap &&
            h4_close > res.val(bar_idx_) + p.break_buf_atr * atr_) {
            const int touches = _touch_count_high(res);
            const double sv = sup.val(bar_idx_);
            const double risk = h4_close - sv;
            if (touches >= p.min_touch &&
                risk >= p.min_risk_atr * atr_ && risk <= p.max_risk_atr * atr_ &&
                ExecutionCostGuard::is_viable(symbol.c_str(), ask - bid,
                                              p.max_risk_atr * atr_, p.lot, p.cost_ratio_min)) {
                _open(+1, ask, sv, risk, bar_close_ms);
                sig = _sig(+1, ask, sv, touches, "TLBREAK_LONG");
                _log_entry(+1, ask, sv, touches); return sig;
            }
        }
        // SHORT: close breaks DOWN through an ascending support; safety = resistance
        if (sup.slope >= 0.0 && std::fabs(sup.slope) <= slope_cap &&
            h4_close < sup.val(bar_idx_) - p.break_buf_atr * atr_) {
            const int touches = _touch_count_low(sup);
            const double rv = res.val(bar_idx_);
            const double risk = rv - h4_close;
            if (touches >= p.min_touch &&
                risk >= p.min_risk_atr * atr_ && risk <= p.max_risk_atr * atr_ &&
                ExecutionCostGuard::is_viable(symbol.c_str(), ask - bid,
                                              p.max_risk_atr * atr_, p.lot, p.cost_ratio_min)) {
                _open(-1, bid, rv, risk, bar_close_ms);
                sig = _sig(-1, bid, rv, touches, "TLBREAK_SHORT");
                _log_entry(-1, bid, rv, touches); return sig;
            }
        }
        return sig;
    }

    // ---------------- helpers ----------------
    void _open(int side, double entry, double safety_val, double risk, int64_t now_ms) noexcept {
        pos_ = OpenPos{};
        pos_.active = true; pos_.side = side; pos_.entry = entry;
        pos_.sl = safety_val; pos_.risk = risk; pos_.lot = p.lot;
        pos_.atr_entry = atr_; pos_.entry_ts_ms = now_ms; ++m_trade_id_;
    }
    TrendLineBreakSignal _sig(int side, double entry, double sl, int touches, const char* r) noexcept {
        TrendLineBreakSignal s; s.valid = true; s.side = side; s.entry = entry;
        s.sl = sl; s.lot = p.lot; s.touches = touches; s.reason = r; return s;
    }
    void _log_entry(int side, double entry, double sl, int touches) noexcept {
        printf("[%s] ENTRY %s @ %.2f safety=%.2f touches=%d atr=%.2f%s\n",
               engine_name.c_str(), side > 0 ? "LONG" : "SHORT", entry, sl, touches, atr_,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);
    }
};

} // namespace omega

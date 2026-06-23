#pragma once
// =============================================================================
//  IndexVwapReclaimEngine.hpp  (S-2026-06-23)
//  Intraday session-VWAP RECLAIM continuation, long-only, BULL-GATED.
//
//  ⛔ NOT DEPLOYED -- enabled=false, never wired. (S-2026-06-23)
//  ADVERSE-PROTECTION: ATR stop (2.5*ATR14) + ATR trail (3.0*ATR, arms at 1.0*ATR
//  MFE = BE-arm) + hard max-hold (180min) -- trail-based, backtested.
//  REAL-ENGINE VERDICT (backtest/index_vwap_reclaim_bt.cpp drives THIS class on
//  real NAS 1m->5m): does NOT reproduce the port. The 2026-06-23 dig PORT
//  (/tmp/idxhunt/bt.py vwap_pull) claimed NAS 2024H1 PF1.59 / 2025H2 PF1.30. The
//  faithful REAL engine (after fixing a session-VWAP-anchor bug that corrected
//  trade count 1.7->4-7 t/wk to MATCH the port) = breakeven: 2024H1 PF1.04,
//  2025H2 PF0.74, 2025bull PF0.86, 2022bear PF0.41. Right trades fire; the
//  profitability is a PORT ARTIFACT (port's 1m-intrabar adverse-first exits were
//  optimistic vs the real 5m engine). Per BACKTEST_TRUTH the edge is NOT
//  confirmed -> NOT deployed. Kept for the record + as a reconciliation target
//  (match the port's intrabar exit fidelity before any re-test). See
//  [[omega-futures-gold-unlock-index-dig]] -- index intraday has NO deployable edge.
//
//  EDGE (price-based session VWAP -- NO volume needed, solves the
//  vwap-reclaim-untestable tombstone): typical-price equal-weight VWAP reset each
//  UTC session. Signal on a CLOSED 5m bar: prev close < VWAP && cur close > VWAP
//  (reclaim), cur close > trend SMA(50), inside session 13:30-21:00 UTC, AND the
//  externally-supplied daily bull gate is true. Long-only. NAS100 only.
//  Shadow until a live-bull forward confirm.
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <deque>
#include <functional>
#include <string>
#include "OmegaTradeLedger.hpp"  // omega::TradeRecord

namespace omega {

struct IndexVwapReclaimEngine {
    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    // ---- config (validated plateau midpoint) ----
    bool   enabled        = false;
    bool   shadow_mode    = true;
    double lot            = 1.0;        // index lot (cap applied downstream)
    std::string symbol    = "NAS100";
    std::string engine_name = "IndexVwapReclaim";
    int    kTrendSma      = 50;         // 5m bars (40-60 plateau)
    int    kAtr           = 14;
    double kStopAtr       = 2.5;
    double kTrailAtr      = 3.0;
    double kBeArmAtr      = 1.0;        // MFE that arms break-even
    int    kMaxHoldBars   = 36;         // 180min / 5m
    int    sess_start_min = 13*60+30;   // 13:30 UTC
    int    sess_end_min   = 21*60;      // 21:00 UTC
    double max_spread     = 6.0;        // index pts

    // ---- session VWAP state (typical-price equal-weight) ----
    int64_t cur_day_      = -1;
    double  vwap_sum_     = 0.0;
    long    vwap_n_       = 0;
    double  vwap_         = 0.0;

    // ---- indicators ----
    std::deque<double> closes_;         // 5m closes for trend SMA
    double atr_          = 0.0;
    double prev_close_   = 0.0;
    bool   have_prev_    = false;

    // ---- position ----
    bool   active_       = false;
    double entry_=0, sl_=0, peak_=0, atr_at_entry_=0;
    int64_t entry_ts_=0;
    int    bars_held_=0;
    bool   be_armed_=false;

    bool warmup_ = false;

    void init() noexcept { closes_.clear(); active_=false; have_prev_=false; vwap_n_=0; cur_day_=-1; }

    static int min_of_day(int64_t ts_ms){ int64_t s=(ts_ms/1000)%86400; return (int)(s/60); }

    // Feed a CLOSED 5m bar. bull_gate = external daily macro bull (price>SMA200 etc).
    void on_5m_bar(double h, double l, double c, int64_t ts_ms, bool bull_gate,
                   OnCloseFn on_close) noexcept {
        if (!enabled) return;
        int m = min_of_day(ts_ms);
        bool in_sess_now = (m>=sess_start_min && m<sess_end_min);
        // SESSION-anchored VWAP: reset at session open (13:30 UTC), accumulate only
        // during the session (pre/post-session bars must NOT pollute the anchor).
        int64_t day = (ts_ms/1000)/86400;
        if (in_sess_now && (day != cur_day_)) { cur_day_=day; vwap_sum_=0; vwap_n_=0; vwap_=0; }
        if (in_sess_now) { double tp=(h+l+c)/3.0; vwap_sum_+=tp; ++vwap_n_; vwap_=vwap_sum_/vwap_n_; }

        // Wilder ATR over 5m (use bar range as TR proxy on close-only feed)
        double tr = h-l; if (prev_close_>0) tr = std::max(tr, std::max(std::fabs(h-prev_close_), std::fabs(l-prev_close_)));
        atr_ = (atr_<=0)? tr : (atr_*(kAtr-1)+tr)/kAtr;

        closes_.push_back(c); while ((int)closes_.size()>kTrendSma+2) closes_.pop_front();

        // ---- manage open position FIRST (this bar's path) ----
        if (active_) {
            ++bars_held_;
            if (c>peak_) peak_=c;
            double mfe = peak_-entry_;
            if (!be_armed_ && mfe>=kBeArmAtr*atr_at_entry_) { be_armed_=true; if (sl_<entry_) sl_=entry_; }
            double trail = peak_ - kTrailAtr*atr_at_entry_;
            if (be_armed_ && trail>sl_) sl_=trail;
            bool exit=false; const char* why=""; double exitpx=c;
            if (l<=sl_) { exit=true; why=be_armed_?"TRAIL/BE":"SL"; exitpx=std::min(c,sl_); }
            else if (bars_held_>=kMaxHoldBars) { exit=true; why="MAXHOLD"; exitpx=c; }
            if (exit) { _close(exitpx, ts_ms, why, on_close); }
        }

        // ---- entry (only when flat) ----
        if (!active_ && !warmup_) {
            bool in_sess = in_sess_now;
            bool have_trend = (int)closes_.size()>=kTrendSma;
            double sma=0; if (have_trend){ for (int i=(int)closes_.size()-kTrendSma;i<(int)closes_.size();++i) sma+=closes_[i]; sma/=kTrendSma; }
            bool reclaim = have_prev_ && prev_close_<vwap_ && c>vwap_;
            if (bull_gate && in_sess && have_trend && reclaim && c>sma && vwap_n_>3) {
                active_=true; entry_=c; peak_=c; atr_at_entry_=(atr_>0?atr_:tr);
                sl_=entry_ - kStopAtr*atr_at_entry_; entry_ts_=ts_ms; bars_held_=0; be_armed_=false;
            }
        }
        prev_close_=c; have_prev_=true;
    }

    void _close(double px, int64_t ts_ms, const char* why, OnCloseFn on_close) noexcept {
        double pnl_pts = px-entry_;
        if (on_close) {
            omega::TradeRecord tr{};
            tr.symbol=symbol; tr.engine=engine_name; tr.side="LONG";
            tr.entryPrice=entry_; tr.exitPrice=px; tr.size=lot;
            tr.pnl=pnl_pts*lot; tr.exitReason=why;
            on_close(tr);
        }
        active_=false; be_armed_=false; bars_held_=0;
    }
};

} // namespace omega

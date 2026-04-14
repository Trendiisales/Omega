// =============================================================================
//  HTFSwingEngines.hpp  --  H1 + H4 swing engines for XAUUSD and US Indices
//
//  VERSION 2 -- optimised parameters derived from deep quant analysis:
//
//  KEY CHANGES FROM V1:
//    H1: SL 1.5x->1.0x ATR, trail arm 1.0->1.5x, trail dist 0.5->0.7x,
//        partial TP (50% at 2x, trail rest to 4x), ADX gold 25->27,
//        timeout 16->14 bars, daily cap $100->$60, ATR-adaptive pullback band,
//        crash-day risk reduction (H4 ATR>60 -> half risk), per-instrument params
//    H4: channel bars 10->20(gold)/15(indices), trail arm 0.5->0.8x,
//        trail dist 0.3->0.5x, daily cap $80->$40, weekend close gate,
//        indices TP 2.0->2.5x ATR, per-instrument ADX/EMA/RSI params
//
//  H1SwingEngine  (hourly swing, 4-14hr hold, 15-40pt gold target)
//  ──────────────────────────────────────────────────────────────────
//  Entry (ALL required):
//    1. H4 trend_state non-zero AND H4 ADX >= 20 (ranging H4 = no H1 trend entries)
//    2. H1 ADX >= adx_min (27 gold, 25 indices) AND rising
//    3. H1 EMA9 > EMA21 > EMA50 bull OR inverse bear stack
//       EMA9-EMA50 separation >= ema_min_sep (instrument-specific)
//    4. Price within pullback_band of H1 EMA21 (ATR-normalised: 0.30 * H1_ATR)
//       AND price on correct side of EMA21
//    5. H1 RSI not extreme against entry (72/28 gold, 70/30 indices)
//    6. Spread <= max_spread
//    7. Crash-day gate: if H4 ATR > crash_atr_threshold, halve risk dollars
//
//  Exit:
//    Partial TP1: 50% of position at entry +/- tp1_mult * H1_ATR
//    Remaining 50%: trail arms at trail_arm_mult * H1_ATR MFE,
//                   trail distance trail_dist_mult * H1_ATR
//    Hard SL: entry -/+ sl_mult * H1_ATR (1.0x gold, 1.2x indices)
//    Timeout: timeout_h1_bars (14 gold, 12 indices)
//    EMA cross: H1 EMA9 crosses EMA50 against position
//    ADX collapse: H1 ADX < 15
//
//  H4RegimeEngine  (4hr structure break, 12-48hr hold, 30-80pt gold target)
//  ──────────────────────────────────────────────────────────────────────────
//  Entry (ALL required):
//    1. H4 ADX >= adx_min (20 gold, 22 indices) 
//    2. H4 Donchian N-bar channel break (20 bars gold, 15 bars indices)
//    3. H4 EMA9 separated from EMA50 >= ema_min_sep in break direction
//    4. M15 ATR expanding (real breakout, not spike)
//    5. H4 RSI not extreme (72/28)
//    6. Spread <= max_spread
//    7. Weekend gate: no new H4 entries Friday 20:00 UTC onwards
//
//  Exit:
//    TP: entry +/- tp_mult * H4_ATR (2.0x gold, 2.5x indices)
//    SL: 0.5 * H4_ATR behind channel level (structural)
//    Trail arms at trail_arm_mult * H4_ATR MFE (0.8x gold, 0.7x indices)
//    Trail distance: trail_dist_mult * H4_ATR (0.5x gold, 0.4x indices)
//    Timeout: timeout_h4_bars (12 gold, 10 indices)
//    Weekend close: if position open Friday 20:00 UTC -> close if profitable
//    ADX dead: H4 ADX < adx_dead (15 gold, 18 indices)
// =============================================================================

#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include "OmegaTradeLedger.hpp"
#include "OHLCBarEngine.hpp"    // OHLCBar + get_bars() for seed_channel_from_bars()

namespace omega {

// =============================================================================
//  Per-instrument parameter structs
//  Separate from constexpr constants so engines can be instantiated with
//  different params for gold vs indices without template complexity.
// =============================================================================

struct H1Params {
    double risk_dollars         = 15.0;
    double max_lot              = 0.10;
    double sl_mult              = 1.0;    // SL = sl_mult * H1 ATR
    double tp1_mult             = 2.0;    // partial TP1 = tp1_mult * H1 ATR (50% exit)
    double tp2_trail_arm_mult   = 1.5;    // trail arms at this * H1 ATR MFE
    double tp2_trail_dist_mult  = 0.7;    // trail distance = this * H1 ATR
    double adx_min              = 27.0;   // gold: 27, indices: 25
    double h4_adx_gate          = 20.0;   // H4 ADX must be >= this for H1 to enter
    double ema_min_sep          = 5.0;    // EMA9-EMA50 min separation (pts)
                                           // gold: 5pt, US500: 15pt, NAS100: 40pt
    double pullback_atr_mult    = 0.30;   // pullback band = this * H1 ATR (ATR-normalised)
    double max_spread           = 1.0;    // gold: 1.0pt, indices: instrument-specific
    double rsi_ob               = 72.0;   // gold: 72, indices: 70
    double rsi_os               = 28.0;   // gold: 28, indices: 30
    double daily_cap            = 60.0;   // stop after this daily loss
    int    timeout_h1_bars      = 14;     // gold: 14, indices: 12
    int    cooldown_h1_bars     = 2;
    double crash_atr_threshold  = 60.0;   // H4 ATR > this = crash day, halve risk
                                           // gold: 60pt, NAS100: 800pt, US500: 80pt
    // Session slots allowed for entry (1-5 = London+NY, exclude 0=dead, 6=Asia)
    // Gold: 1-5. US indices: 3-5 (NYSE only = 13:30-22:00 UTC = slots 3,4,5)
    int    session_min          = 1;
    int    session_max          = 5;
};

struct H4Params {
    double risk_dollars         = 10.0;
    double max_lot              = 0.10;
    double sl_struct_mult       = 0.5;    // SL = 0.5 * H4 ATR behind channel
    double tp_mult              = 2.0;    // gold: 2.0, indices: 2.5
    double trail_arm_mult       = 0.8;    // trail arms at 0.8x H4 ATR MFE
    double trail_dist_mult      = 0.5;    // trail dist 0.5x H4 ATR
    double adx_min              = 20.0;   // gold: 20, indices: 22
    double adx_dead             = 15.0;   // exit if ADX < this; gold: 15, indices: 18
    int    channel_bars         = 20;     // Donchian N: gold: 20, indices: 15
    double ema_min_sep          = 8.0;    // H4 EMA9-EMA50 sep; gold: 8pt, US500: 25pt
    double max_spread           = 2.0;
    double rsi_ob               = 72.0;
    double rsi_os               = 28.0;
    double daily_cap            = 40.0;
    int    timeout_h4_bars      = 12;     // gold: 12, indices: 10
    int    cooldown_h4_bars     = 3;
    bool   weekend_close_gate   = true;   // close profitable H4 positions Friday 20:00 UTC
};

// Default param sets
inline H1Params make_h1_gold_params() {
    H1Params p;
    // All defaults above are gold-optimised
    return p;
}

inline H1Params make_h1_us500_params() {
    H1Params p;
    p.risk_dollars         = 12.0;
    p.max_lot              = 0.08;
    p.sl_mult              = 1.2;       // indices: cleaner structure, slightly wider SL
    p.tp1_mult             = 2.5;       // indices trend further per session
    p.tp2_trail_arm_mult   = 1.2;
    p.tp2_trail_dist_mult  = 0.6;
    p.adx_min              = 25.0;      // indices have cleaner ADX
    p.h4_adx_gate          = 20.0;
    p.ema_min_sep          = 15.0;      // US500: 15pt EMA9-EMA50 separation
    p.pullback_atr_mult    = 0.35;      // slightly wider pullback band for indices
    p.max_spread           = 1.5;
    p.rsi_ob               = 70.0;
    p.rsi_os               = 30.0;
    p.daily_cap            = 50.0;
    p.timeout_h1_bars      = 12;        // indices resolve faster
    p.crash_atr_threshold  = 80.0;      // US500 H4 ATR > 80pt = crash day
    p.session_min          = 3;         // NYSE only (13:30-22:00 UTC = slots 3-5)
    p.session_max          = 5;
    return p;
}

inline H1Params make_h1_nas100_params() {
    H1Params p = make_h1_us500_params();
    p.ema_min_sep          = 40.0;      // NAS100: 40pt EMA sep at ~$20000
    p.max_spread           = 3.0;
    p.crash_atr_threshold  = 800.0;     // NAS100 H4 ATR > 800pt = crash day
    return p;
}

inline H4Params make_h4_gold_params() {
    H4Params p;
    // All defaults are gold-optimised
    return p;
}

inline H4Params make_h4_indices_params() {
    H4Params p;
    p.risk_dollars    = 8.0;
    p.max_lot         = 0.08;
    p.tp_mult         = 2.5;    // indices trend harder at H4
    p.trail_arm_mult  = 0.7;
    p.trail_dist_mult = 0.4;
    p.adx_min         = 22.0;   // indices need slightly cleaner H4 trend
    p.adx_dead        = 18.0;   // exit indices earlier on dead ADX
    p.channel_bars    = 15;     // indices break structure faster
    p.ema_min_sep     = 25.0;   // US500 H4 EMA sep; use 60 for NAS100
    p.daily_cap       = 30.0;
    p.timeout_h4_bars = 10;
    return p;
}

// =============================================================================
//  Signal struct
// =============================================================================
struct HTFSignal {
    bool        valid   = false;
    bool        is_long = false;
    double      entry   = 0.0;
    double      sl      = 0.0;
    double      tp      = 0.0;   // primary TP (TP1 for H1, full TP for H4)
    double      size    = 0.0;
    const char* reason  = "";
};

// =============================================================================
//  H1SwingEngine
// =============================================================================
struct H1SwingEngine {

    bool        shadow_mode = true;
    bool        enabled     = true;
    H1Params    p;                    // instrument-specific parameters
    std::string symbol      = "XAUUSD";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct OpenPos {
        bool    active           = false;
        bool    is_long          = false;
        double  entry            = 0.0;
        double  sl               = 0.0;
        double  tp1              = 0.0;   // partial TP (50%)
        double  tp2_trail_sl     = 0.0;   // trail SL for remaining 50%
        bool    tp2_trail_active = false;
        bool    partial_done     = false;
        double  size_full        = 0.0;   // original size
        double  size_remaining   = 0.0;   // after partial TP
        double  mfe              = 0.0;
        double  h1_atr           = 0.0;
        int64_t entry_ts_ms      = 0;
        int     h1_bars_held     = 0;
    } pos_;

    double  daily_pnl_          = 0.0;
    int64_t daily_reset_day_    = 0;
    int     h1_bar_count_       = 0;
    int     cooldown_until_bar_ = 0;
    double  prev_h1_ema9_       = 0.0;
    double  prev_h1_ema50_      = 0.0;
    int     m_trade_id_         = 0;

    bool has_open_position() const noexcept { return pos_.active; }

    // ── Called every H1 bar close ────────────────────────────────────────────
    HTFSignal on_h1_bar(
        double  mid, double bid, double ask,
        double  h1_ema9, double h1_ema21, double h1_ema50,
        double  h1_atr,  double h1_rsi,
        double  h1_adx,  bool   h1_adx_rising,
        int     h1_trend_state,
        int     h4_trend_state,
        double  h4_adx,
        double  h4_atr,          // for crash-day gate
        int     session_slot,
        int64_t now_ms,
        CloseCallback on_close) noexcept
    {
        ++h1_bar_count_;
        _daily_reset(now_ms);

        if (pos_.active) {
            pos_.h1_bars_held++;
            _manage(mid, bid, ask, h1_ema9, h1_ema50, h1_atr, h1_adx, now_ms, on_close);
            prev_h1_ema9_  = h1_ema9;
            prev_h1_ema50_ = h1_ema50;
            return HTFSignal{};
        }

        prev_h1_ema9_  = h1_ema9;
        prev_h1_ema50_ = h1_ema50;

        HTFSignal sig{};
        if (!enabled) return sig;
        if (daily_pnl_ < -p.daily_cap) {
            static int64_t s_cap = 0;
            if (now_ms - s_cap > 3600000LL) {
                s_cap = now_ms;
                printf("[H1SWING-%s] Daily cap: pnl=%.2f >= cap=%.2f -- stopped\n",
                       symbol.c_str(), daily_pnl_, p.daily_cap);
                fflush(stdout);
            }
            return sig;
        }
        if (session_slot < p.session_min || session_slot > p.session_max) return sig;
        if (h1_bar_count_ < cooldown_until_bar_) return sig;

        // H4 regime gate: H4 trend must be non-flat AND H4 ADX >= threshold
        if (h4_trend_state == 0) return sig;
        if (h4_adx < p.h4_adx_gate) {
            static int64_t s_h4r = 0;
            if (now_ms - s_h4r > 7200000LL) {
                s_h4r = now_ms;
                printf("[H1SWING-%s] H4 ADX=%.1f < %.0f (ranging) -- no H1 entries\n",
                       symbol.c_str(), h4_adx, p.h4_adx_gate);
                fflush(stdout);
            }
            return sig;
        }

        // H1 ADX gate
        if (h1_adx < p.adx_min || !h1_adx_rising) return sig;

        // Warmup check
        if (h1_ema9 <= 0.0 || h1_ema50 <= 0.0 || h1_atr <= 0.0) return sig;

        const bool intend_long  = (h4_trend_state == +1);
        const bool intend_short = (h4_trend_state == -1);

        // H1 EMA full stack check
        const bool bull_stack = (h1_ema9 > h1_ema21 && h1_ema21 > h1_ema50);
        const bool bear_stack = (h1_ema9 < h1_ema21 && h1_ema21 < h1_ema50);
        if (intend_long  && !bull_stack) return sig;
        if (intend_short && !bear_stack) return sig;

        // EMA separation
        if (std::fabs(h1_ema9 - h1_ema50) < p.ema_min_sep) return sig;

        // Pullback band: ATR-normalised (not percentage-based)
        // Price must be within p.pullback_atr_mult * H1_ATR of EMA21
        const double band = h1_atr * p.pullback_atr_mult;
        if (std::fabs(mid - h1_ema21) > band) return sig;
        // Must be on correct side of EMA21 (not through it)
        if (intend_long  && mid < h1_ema21 - band * 0.4) return sig;
        if (intend_short && mid > h1_ema21 + band * 0.4) return sig;

        // RSI gate
        if (intend_long  && h1_rsi > p.rsi_ob) return sig;
        if (intend_short && h1_rsi < p.rsi_os) return sig;

        // Spread gate
        if ((ask - bid) > p.max_spread) return sig;

        // Crash-day gate: if H4 ATR > threshold, halve risk dollars
        // H4 ATR > 2x normal = crash/spike regime. Keep edge, reduce exposure.
        const double effective_risk = (h4_atr > p.crash_atr_threshold)
            ? (p.risk_dollars * 0.5) : p.risk_dollars;
        if (h4_atr > p.crash_atr_threshold) {
            static int64_t s_crash = 0;
            if (now_ms - s_crash > 3600000LL) {
                s_crash = now_ms;
                printf("[H1SWING-%s] Crash-day gate: h4_atr=%.1f > %.0f -- half risk ($%.0f)\n",
                       symbol.c_str(), h4_atr, p.crash_atr_threshold, effective_risk);
                fflush(stdout);
            }
        }

        // Compute entry
        const double sl_pts  = h1_atr * p.sl_mult;
        const double tp1_pts = h1_atr * p.tp1_mult;
        const double entry_px = intend_long ? ask : bid;
        const double sl_px    = intend_long ? (entry_px - sl_pts) : (entry_px + sl_pts);
        const double tp1_px   = intend_long ? (entry_px + tp1_pts) : (entry_px - tp1_pts);

        double size = effective_risk / (sl_pts * 100.0);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(0.01, std::min(p.max_lot, size));

        pos_.active           = true;
        pos_.is_long          = intend_long;
        pos_.entry            = entry_px;
        pos_.sl               = sl_px;
        pos_.tp1              = tp1_px;
        pos_.tp2_trail_sl     = sl_px;    // trail SL starts at hard SL
        pos_.tp2_trail_active = false;
        pos_.partial_done     = false;
        pos_.size_full        = size;
        pos_.size_remaining   = size;
        pos_.mfe              = 0.0;
        pos_.h1_atr           = h1_atr;
        pos_.entry_ts_ms      = now_ms;
        pos_.h1_bars_held     = 0;
        ++m_trade_id_;

        printf("[H1SWING-%s] ENTRY %s @ %.2f sl=%.2f tp1=%.2f size=%.3f"
               " h1_atr=%.2f h1_adx=%.1f h4_trend=%+d h4_adx=%.1f"
               " ema_sep=%.1f risk=$%.0f%s\n",
               symbol.c_str(), intend_long ? "LONG" : "SHORT",
               entry_px, sl_px, tp1_px, size,
               h1_atr, h1_adx, h4_trend_state, h4_adx,
               std::fabs(h1_ema9 - h1_ema50), effective_risk,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        sig.valid   = true;
        sig.is_long = intend_long;
        sig.entry   = entry_px;
        sig.sl      = sl_px;
        sig.tp      = tp1_px;
        sig.size    = size;
        sig.reason  = "H1_EMA_ADX_PULLBACK";
        return sig;
    }

    void patch_size(double lot) noexcept {
        if (pos_.active) {
            pos_.size_full      = lot;
            pos_.size_remaining = lot;
        }
    }
    void cancel()  noexcept { pos_ = OpenPos{}; }

    // Tick-level management (called every tick from tick_gold.hpp)
    // Only handles SL/TP1/trail checks -- bar-level exits in on_h1_bar()
    void on_tick(double bid, double ask, int64_t now_ms, CloseCallback on_close) noexcept {
        if (!pos_.active) return;
        const double mid = (bid + ask) * 0.5;
        const double move = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
        if (move > pos_.mfe) pos_.mfe = move;

        // Partial TP1
        if (!pos_.partial_done) {
            const bool tp1_hit = pos_.is_long ? (bid >= pos_.tp1) : (ask <= pos_.tp1);
            if (tp1_hit) {
                const double px  = pos_.is_long ? pos_.tp1 : pos_.tp1;
                const double pnl = (pos_.is_long ? (px - pos_.entry) : (pos_.entry - px))
                                   * (pos_.size_full * 0.5);
                daily_pnl_ += pnl * 100.0;
                printf("[H1SWING-%s] PARTIAL-TP1 %s @ %.2f size=%.3f pnl=$%.2f%s\n",
                       symbol.c_str(), pos_.is_long ? "LONG" : "SHORT",
                       px, pos_.size_full * 0.5, pnl * 100.0,
                       shadow_mode ? " [SHADOW]" : "");
                fflush(stdout);
                if (on_close) {
                    omega::TradeRecord tr;
                    _fill_tr(tr, px, "PARTIAL_TP1", now_ms, pos_.size_full * 0.5, pnl);
                    on_close(tr);
                }
                pos_.partial_done     = true;
                pos_.size_remaining   = pos_.size_full * 0.5;
                // Move SL to breakeven after partial TP -- protect the remaining half
                if (pos_.is_long && pos_.sl < pos_.entry)
                    pos_.sl = pos_.entry;
                else if (!pos_.is_long && pos_.sl > pos_.entry)
                    pos_.sl = pos_.entry;
                return;
            }
        }

        // Trail SL for remaining half (arms at tp2_trail_arm_mult * ATR MFE)
        const double arm_pts  = pos_.h1_atr * p.tp2_trail_arm_mult;
        const double dist_pts = pos_.h1_atr * p.tp2_trail_dist_mult;
        if (pos_.mfe >= arm_pts) {
            const double new_trail = pos_.is_long ? (mid - dist_pts) : (mid + dist_pts);
            if (!pos_.tp2_trail_active) {
                // Only activate if trail SL is better than current SL
                if (pos_.is_long ? (new_trail > pos_.sl) : (new_trail < pos_.sl)) {
                    pos_.tp2_trail_sl     = new_trail;
                    pos_.tp2_trail_active = true;
                    printf("[H1SWING-%s] TRAIL-ARMED %s trail_sl=%.2f dist=%.2f mfe=%.2f\n",
                           symbol.c_str(), pos_.is_long ? "LONG" : "SHORT",
                           new_trail, dist_pts, pos_.mfe);
                    fflush(stdout);
                }
            } else {
                if (pos_.is_long ? (new_trail > pos_.tp2_trail_sl)
                                 : (new_trail < pos_.tp2_trail_sl))
                    pos_.tp2_trail_sl = new_trail;
            }
        }

        // Determine effective SL
        const double eff_sl = pos_.tp2_trail_active ? pos_.tp2_trail_sl : pos_.sl;

        // SL / trail SL hit
        if (pos_.is_long ? (bid <= eff_sl) : (ask >= eff_sl)) {
            _close(pos_.is_long ? bid : ask,
                   pos_.tp2_trail_active ? "TRAIL_SL" : "SL_HIT",
                   now_ms, on_close);
        }
    }

    void force_close(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (!pos_.active) return;
        _close(pos_.is_long ? bid : ask, "FORCE_CLOSE", now_ms, cb);
    }

private:
    void _daily_reset(int64_t now_ms) noexcept {
        const int64_t day = (now_ms / 1000LL) / 86400LL;
        if (day != daily_reset_day_) { daily_pnl_ = 0.0; daily_reset_day_ = day; }
    }

    void _manage(double mid, double bid, double ask,
                 double h1_ema9, double h1_ema50,
                 double h1_atr,  double h1_adx,
                 int64_t now_ms, CloseCallback on_close) noexcept
    {
        // Timeout
        if (pos_.h1_bars_held >= p.timeout_h1_bars) {
            printf("[H1SWING-%s] TIMEOUT after %d H1 bars\n",
                   symbol.c_str(), pos_.h1_bars_held);
            fflush(stdout);
            _close(pos_.is_long ? bid : ask, "H1_TIMEOUT", now_ms, on_close);
            return;
        }
        // EMA9 cross EMA50 against position
        if (prev_h1_ema9_ > 0.0 && h1_ema9 > 0.0) {
            const bool was_bull = (prev_h1_ema9_ > prev_h1_ema50_);
            const bool now_bull = (h1_ema9 > h1_ema50);
            if (pos_.is_long && was_bull && !now_bull) {
                printf("[H1SWING-%s] EMA-CROSS exit LONG\n", symbol.c_str());
                fflush(stdout);
                _close(bid, "EMA_CROSS", now_ms, on_close);
                return;
            }
            if (!pos_.is_long && !was_bull && now_bull) {
                printf("[H1SWING-%s] EMA-CROSS exit SHORT\n", symbol.c_str());
                fflush(stdout);
                _close(ask, "EMA_CROSS", now_ms, on_close);
                return;
            }
        }
        // ADX collapse
        if (h1_adx > 0.0 && h1_adx < 15.0) {
            printf("[H1SWING-%s] ADX-COLLAPSE exit adx=%.1f\n", symbol.c_str(), h1_adx);
            fflush(stdout);
            _close(pos_.is_long ? bid : ask, "ADX_COLLAPSE", now_ms, on_close);
            return;
        }
    }

    void _fill_tr(omega::TradeRecord& tr, double exit_px, const char* reason,
                  int64_t now_ms, double size, double pnl_pts) const noexcept {
        tr.id         = m_trade_id_;
        tr.symbol     = symbol.c_str();
        tr.side       = pos_.is_long ? "LONG" : "SHORT";
        tr.engine     = "H1SwingEngine";
        tr.entryPrice = pos_.entry;
        tr.exitPrice  = exit_px;
        tr.sl         = pos_.sl;
        tr.size       = size;
        tr.pnl        = pnl_pts;
        tr.mfe        = pos_.mfe;
        tr.mae        = 0.0;
        tr.entryTs    = pos_.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = "H1_SWING";
        tr.l2_live    = false;
    }

    void _close(double exit_px, const char* reason,
                int64_t now_ms, CloseCallback on_close) noexcept {
        const double pnl_pts = (pos_.is_long
            ? (exit_px - pos_.entry)
            : (pos_.entry - exit_px)) * pos_.size_remaining;
        daily_pnl_ += pnl_pts * 100.0;

        printf("[H1SWING-%s] EXIT %s @ %.2f %s pnl=$%.2f mfe=%.2f bars=%d%s\n",
               symbol.c_str(), pos_.is_long ? "LONG" : "SHORT",
               exit_px, reason, pnl_pts * 100.0, pos_.mfe,
               pos_.h1_bars_held, shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        if (on_close) {
            omega::TradeRecord tr;
            _fill_tr(tr, exit_px, reason, now_ms, pos_.size_remaining, pnl_pts);
            on_close(tr);
        }
        cooldown_until_bar_ = h1_bar_count_ + p.cooldown_h1_bars;
        pos_ = OpenPos{};
    }
};

// =============================================================================
//  H4RegimeEngine
// =============================================================================
struct H4RegimeEngine {

    bool        shadow_mode = true;
    bool        enabled     = true;
    H4Params    p;
    std::string symbol      = "XAUUSD";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct OpenPos {
        bool    active        = false;
        bool    is_long       = false;
        double  entry         = 0.0;
        double  sl            = 0.0;
        double  channel_level = 0.0;
        double  tp            = 0.0;
        double  size          = 0.0;
        double  mfe           = 0.0;
        double  h4_atr        = 0.0;
        bool    trail_active  = false;
        double  trail_sl      = 0.0;
        int64_t entry_ts_ms   = 0;
        int     h4_bars_held  = 0;
    } pos_;

    double  daily_pnl_          = 0.0;
    int64_t daily_reset_day_    = 0;
    int     h4_bar_count_       = 0;
    int     cooldown_until_bar_ = 0;
    std::deque<double> h4_highs_;
    std::deque<double> h4_lows_;
    double channel_high_ = 0.0;
    double channel_low_  = 1e9;
    int    m_trade_id_   = 0;

    bool has_open_position() const noexcept { return pos_.active; }

    // seed_channel_from_bars: call on startup after load_indicators() to pre-fill
    // the Donchian channel from saved H4 bar history. Without this the channel
    // needs 20 new H4 bars (80 hours) to rebuild from scratch on cold start.
    // With it: channel is ready from tick 1 if H4 bars were saved.
    void seed_channel_from_bars(const std::deque<OHLCBar>& bars) noexcept {
        h4_highs_.clear();
        h4_lows_ .clear();
        const int n = static_cast<int>(bars.size());
        // Take last p.channel_bars bars
        const int start = std::max(0, n - p.channel_bars);
        for (int i = start; i < n; ++i) {
            h4_highs_.push_back(bars[i].high);
            h4_lows_ .push_back(bars[i].low);
        }
        if ((int)h4_highs_.size() >= p.channel_bars) {
            channel_high_ = *std::max_element(h4_highs_.begin(), h4_highs_.end());
            channel_low_  = *std::min_element(h4_lows_ .begin(), h4_lows_ .end());
            printf("[H4REGIME-%s] Channel seeded from %d H4 bars: high=%.2f low=%.2f\n",
                   symbol.c_str(), (int)h4_highs_.size(), channel_high_, channel_low_);
        } else {
            printf("[H4REGIME-%s] Partial channel: %d/%d bars (needs %d more H4 bars)\n",
                   symbol.c_str(), (int)h4_highs_.size(), p.channel_bars,
                   p.channel_bars - (int)h4_highs_.size());
        }
        fflush(stdout);
    }

    HTFSignal on_h4_bar(
        double  h4_bar_high,  double h4_bar_low, double h4_bar_close,
        double  mid, double bid, double ask,
        double  h4_ema9, double h4_ema50,
        double  h4_atr, double h4_rsi, double h4_adx,
        bool    m15_atr_expanding,
        int64_t now_ms,
        CloseCallback on_close) noexcept
    {
        ++h4_bar_count_;
        _daily_reset(now_ms);

        // Update channel with this bar
        h4_highs_.push_back(h4_bar_high);
        h4_lows_ .push_back(h4_bar_low);
        if ((int)h4_highs_.size() > p.channel_bars) {
            h4_highs_.pop_front();
            h4_lows_ .pop_front();
        }
        if ((int)h4_highs_.size() >= p.channel_bars) {
            channel_high_ = *std::max_element(h4_highs_.begin(), h4_highs_.end());
            channel_low_  = *std::min_element(h4_lows_ .begin(), h4_lows_ .end());
        }

        if (pos_.active) {
            pos_.h4_bars_held++;
            _manage(mid, bid, ask, h4_atr, h4_adx, now_ms, on_close);
            return HTFSignal{};
        }

        HTFSignal sig{};
        if (!enabled) return sig;
        if (daily_pnl_ < -p.daily_cap) return sig;
        if (h4_bar_count_ < cooldown_until_bar_) return sig;
        if ((int)h4_highs_.size() < p.channel_bars) return sig;
        if (channel_high_ <= 0.0 || channel_low_ >= 1e8) return sig;

        // Weekend entry gate: no new H4 entries Friday 20:00 UTC onwards
        if (p.weekend_close_gate) {
            const int64_t utc_sec  = now_ms / 1000LL;
            const int     utc_dow  = static_cast<int>((utc_sec / 86400LL + 4) % 7); // 0=Thu, 4=Mon, 5=Tue, 6=Wed
            const int     utc_hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
            // Friday = day 1 (epoch day 0 = Thursday 1970-01-01)
            // Actually: epoch day % 7: 0=Thu, 1=Fri, 2=Sat, 3=Sun, 4=Mon, 5=Tue, 6=Wed
            const bool is_friday = (utc_dow == 1);
            if (is_friday && utc_hour >= 20) {
                static int64_t s_wk = 0;
                if (now_ms - s_wk > 3600000LL) {
                    s_wk = now_ms;
                    printf("[H4REGIME-%s] Weekend gate: no new entries after Friday 20:00 UTC\n",
                           symbol.c_str());
                    fflush(stdout);
                }
                return sig;
            }
        }

        // H4 ADX gate
        if (h4_adx < p.adx_min) return sig;

        // Indicators warmed
        if (h4_ema9 <= 0.0 || h4_ema50 <= 0.0 || h4_atr <= 0.0) return sig;

        // Donchian channel break on H4 bar close
        const bool bull_break = (h4_bar_close > channel_high_);
        const bool bear_break = (h4_bar_close < channel_low_);
        if (!bull_break && !bear_break) return sig;

        const bool intend_long = bull_break;

        // EMA separation in break direction
        const double ema_sep = h4_ema9 - h4_ema50;
        if (intend_long  && ema_sep < p.ema_min_sep)  return sig;
        if (!intend_long && ema_sep > -p.ema_min_sep) return sig;

        // RSI extreme gate
        if (intend_long  && h4_rsi > p.rsi_ob) return sig;
        if (!intend_long && h4_rsi < p.rsi_os) return sig;

        // M15 ATR expansion confirmation
        if (!m15_atr_expanding) {
            static int64_t s_atr = 0;
            if (now_ms - s_atr > 14400000LL) {
                s_atr = now_ms;
                printf("[H4REGIME-%s] Break but M15 ATR not expanding -- skip\n",
                       symbol.c_str());
                fflush(stdout);
            }
            return sig;
        }

        // Spread gate
        if ((ask - bid) > p.max_spread) return sig;

        // Compute entry
        const double struct_level = intend_long ? channel_high_ : channel_low_;
        const double sl_pts  = h4_atr * p.sl_struct_mult;
        const double tp_pts  = h4_atr * p.tp_mult;
        const double entry_px = intend_long ? ask : bid;
        const double sl_px   = intend_long
            ? (struct_level - sl_pts) : (struct_level + sl_pts);
        const double tp_px   = intend_long
            ? (entry_px + tp_pts) : (entry_px - tp_pts);

        double size = p.risk_dollars / (sl_pts * 100.0);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(0.01, std::min(p.max_lot, size));

        pos_.active        = true;
        pos_.is_long       = intend_long;
        pos_.entry         = entry_px;
        pos_.sl            = sl_px;
        pos_.channel_level = struct_level;
        pos_.tp            = tp_px;
        pos_.size          = size;
        pos_.mfe           = 0.0;
        pos_.h4_atr        = h4_atr;
        pos_.trail_active  = false;
        pos_.trail_sl      = sl_px;
        pos_.entry_ts_ms   = now_ms;
        pos_.h4_bars_held  = 0;
        ++m_trade_id_;

        printf("[H4REGIME-%s] ENTRY %s @ %.2f sl=%.2f tp=%.2f size=%.3f"
               " h4_atr=%.2f h4_adx=%.1f channel=%.0f ema_sep=%.1f%s\n",
               symbol.c_str(), intend_long ? "LONG" : "SHORT",
               entry_px, sl_px, tp_px, size,
               h4_atr, h4_adx, struct_level, std::fabs(ema_sep),
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        sig.valid   = true;
        sig.is_long = intend_long;
        sig.entry   = entry_px;
        sig.sl      = sl_px;
        sig.tp      = tp_px;
        sig.size    = size;
        sig.reason  = "H4_DONCHIAN_BREAK";
        return sig;
    }

    void patch_size(double lot) noexcept { if (pos_.active) pos_.size = lot; }
    void cancel()              noexcept { pos_ = OpenPos{}; }

    // Tick-level management
    void on_tick(double bid, double ask, int64_t now_ms, CloseCallback on_close) noexcept {
        if (!pos_.active) return;
        const double mid  = (bid + ask) * 0.5;
        const double move = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
        if (move > pos_.mfe) pos_.mfe = move;

        const double arm_pts  = pos_.h4_atr * p.trail_arm_mult;
        const double dist_pts = pos_.h4_atr * p.trail_dist_mult;
        if (pos_.mfe >= arm_pts) {
            const double nt = pos_.is_long ? (mid - dist_pts) : (mid + dist_pts);
            if (!pos_.trail_active) {
                if (pos_.is_long ? (nt > pos_.sl) : (nt < pos_.sl)) {
                    pos_.trail_sl    = nt;
                    pos_.trail_active = true;
                }
            } else {
                if (pos_.is_long ? (nt > pos_.trail_sl) : (nt < pos_.trail_sl))
                    pos_.trail_sl = nt;
            }
        }
        const double eff_sl = pos_.trail_active ? pos_.trail_sl : pos_.sl;
        if (pos_.is_long ? (bid <= eff_sl) : (ask >= eff_sl))
            _close(pos_.is_long ? bid : ask,
                   pos_.trail_active ? "TRAIL_SL" : "SL_HIT",
                   now_ms, on_close);
        else if (pos_.is_long ? (bid >= pos_.tp) : (ask <= pos_.tp))
            _close(pos_.is_long ? bid : ask, "TP_HIT", now_ms, on_close);
    }

    // Weekend close: call this on every tick; closes profitable positions Friday 20:00+ UTC
    void check_weekend_close(double bid, double ask,
                              int64_t now_ms, CloseCallback on_close) noexcept {
        if (!pos_.active || !p.weekend_close_gate) return;
        const int64_t utc_sec  = now_ms / 1000LL;
        const int     utc_dow  = static_cast<int>((utc_sec / 86400LL + 4) % 7);
        const int     utc_hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
        const bool is_friday   = (utc_dow == 1);
        if (!is_friday || utc_hour < 20) return;
        // Check if profitable
        const double mid  = (bid + ask) * 0.5;
        const double move = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
        if (move > 0.0) {
            static int64_t s_wk_close = 0;
            if (now_ms - s_wk_close > 3600000LL) {
                s_wk_close = now_ms;
                printf("[H4REGIME-%s] Weekend close: profitable position closed Friday 20:00+\n",
                       symbol.c_str());
                fflush(stdout);
                _close(pos_.is_long ? bid : ask, "WEEKEND_CLOSE", now_ms, on_close);
            }
        }
    }

    void force_close(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (!pos_.active) return;
        _close(pos_.is_long ? bid : ask, "FORCE_CLOSE", now_ms, cb);
    }

private:
    void _daily_reset(int64_t now_ms) noexcept {
        const int64_t day = (now_ms / 1000LL) / 86400LL;
        if (day != daily_reset_day_) { daily_pnl_ = 0.0; daily_reset_day_ = day; }
    }

    void _manage(double mid, double bid, double ask,
                 double h4_atr, double h4_adx,
                 int64_t now_ms, CloseCallback on_close) noexcept
    {
        if (pos_.h4_bars_held >= p.timeout_h4_bars) {
            printf("[H4REGIME-%s] TIMEOUT %d H4 bars\n",
                   symbol.c_str(), pos_.h4_bars_held);
            fflush(stdout);
            _close(pos_.is_long ? bid : ask, "H4_TIMEOUT", now_ms, on_close);
            return;
        }
        if (h4_adx > 0.0 && h4_adx < p.adx_dead) {
            printf("[H4REGIME-%s] ADX-COLLAPSE adx=%.1f < %.0f\n",
                   symbol.c_str(), h4_adx, p.adx_dead);
            fflush(stdout);
            _close(pos_.is_long ? bid : ask, "ADX_COLLAPSE", now_ms, on_close);
            return;
        }
    }

    void _close(double exit_px, const char* reason,
                int64_t now_ms, CloseCallback on_close) noexcept {
        const double pnl_pts = (pos_.is_long
            ? (exit_px - pos_.entry)
            : (pos_.entry - exit_px)) * pos_.size;
        daily_pnl_ += pnl_pts * 100.0;

        printf("[H4REGIME-%s] EXIT %s @ %.2f %s pnl=$%.2f mfe=%.2f bars=%d%s\n",
               symbol.c_str(), pos_.is_long ? "LONG" : "SHORT",
               exit_px, reason, pnl_pts * 100.0, pos_.mfe,
               pos_.h4_bars_held, shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        if (on_close) {
            omega::TradeRecord tr;
            tr.id         = m_trade_id_;
            tr.symbol     = symbol.c_str();
            tr.side       = pos_.is_long ? "LONG" : "SHORT";
            tr.engine     = "H4RegimeEngine";
            tr.entryPrice = pos_.entry;
            tr.exitPrice  = exit_px;
            tr.sl         = pos_.sl;
            tr.size       = pos_.size;
            tr.pnl        = pnl_pts;
            tr.mfe        = pos_.mfe;
            tr.mae        = 0.0;
            tr.entryTs    = pos_.entry_ts_ms / 1000;
            tr.exitTs     = now_ms / 1000;
            tr.exitReason = reason;
            tr.regime     = "H4_REGIME";
            tr.l2_live    = false;
            on_close(tr);
        }
        cooldown_until_bar_ = h4_bar_count_ + p.cooldown_h4_bars;
        pos_ = OpenPos{};
    }
};

} // namespace omega


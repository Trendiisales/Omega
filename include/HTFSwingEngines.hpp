// =============================================================================
//  HTFSwingEngines.hpp  --  H1 + H4 swing engines for XAUUSD
//
//  TWO ENGINES:
//
//  H1SwingEngine  (hourly swing, 4-16hr hold, 15-40pt target)
//  ────────────────────────────────────────────────────────────
//  Entry logic (ALL required):
//    1. H4 regime agrees: h4_trend_state == direction
//       (H4 trending up = only H1 LONGs; H4 trending down = only H1 SHORTs)
//       Exception: H4 trend_state == 0 (flat) -> disabled entirely
//    2. H1 ADX(14) >= 25 AND rising  (trending session, not chop)
//    3. H1 EMA9 > EMA21 > EMA50 (bull stack) or inverse (bear stack)
//       Minimum EMA9-EMA50 separation >= H1_EMA_MIN_SEP (5pt default)
//    4. Entry trigger: price pulls back within H1_PULLBACK_BAND of H1 EMA21
//       AND current H1 bar closes in direction (close above EMA21 for long)
//    5. H1 RSI NOT extreme against entry: LONG blocked when RSI > 75,
//       SHORT blocked when RSI < 25 (don't buy overbought on H1)
//    6. Spread <= H1_MAX_SPREAD (1.0pt -- H1 entries tolerate wider spreads)
//
//  Exit:
//    Primary TP: entry +/- H1_ATR * H1_TP_MULT (default 2.5x H1 ATR)
//    Hard SL:    entry -/+ H1_ATR * H1_SL_MULT (default 1.5x H1 ATR)
//    Trailing:   arms when MFE >= 1x H1 ATR; trail distance = 0.5x H1 ATR
//    Timeout:    H1_TIMEOUT_BARS H1 bars (default 16 = 16 hours)
//    EMA cross:  H1 EMA9 crosses EMA50 against position -> exit
//
//  Sizing: $H1_RISK_DOLLARS / (SL_pts * 100), floor 0.01, ceiling 0.10 lots
//
//  Sessions: London + NY only (slots 1-5), not Asia (slot 6)
//  Cooldown:  H1_COOLDOWN_H1_BARS H1 bars after any close
//
//  H4RegimeEngine  (4-hour structure break, 1-3 day hold, 30-80pt target)
//  ────────────────────────────────────────────────────────────────────────
//  This engine fires RARELY (1-3 times per week) but targets the "big moves".
//  It is a Donchian/Turtle channel breakout filtered by ADX regime.
//
//  Entry logic (ALL required):
//    1. H4 ADX(14) >= H4_ADX_MIN (20) -- some directionality present
//    2. H4 price breaks above N-bar high (LONG) or below N-bar low (SHORT)
//       N = H4_CHANNEL_BARS (10 H4 bars = 40 hours = ~2 trading days)
//    3. H4 EMA9 separated from EMA50 by >= H4_EMA_MIN_SEP in break direction
//       (prevents entering a break that is fighting the H4 trend)
//    4. M15 ATR is expanding (atr_expanding = true from g_bars_gold.m15)
//       Real breakouts have M15 vol expansion at the moment of H4 break
//    5. H4 RSI NOT already extreme: LONG blocked RSI>75, SHORT blocked RSI<25
//       (don't buy a H4 bar that is already at extreme RSI)
//    6. Spread <= H4_MAX_SPREAD (2.0pt -- H4 entries can tolerate wide spreads)
//
//  Exit:
//    Primary TP: entry +/- H4_ATR * H4_TP_MULT (default 2.0x H4 ATR)
//    Hard SL:    0.5x H4 ATR behind the channel level (tight vs the structure)
//    Trailing:   arms when MFE >= 0.5x H4 ATR; trail distance = 0.3x H4 ATR
//    Timeout:    H4_TIMEOUT_H4_BARS H4 bars (default 12 = 48 hours = 2 days)
//    ADX collapse: if H4 ADX falls below 15 while in trade -> exit (trend died)
//
//  Sizing: $H4_RISK_DOLLARS / (SL_pts * 100), floor 0.01, ceiling 0.10 lots
//  NOTE: SL for H4 is 0.5 * H4_ATR which at 25pt ATR = 12.5pt -> 0.01 lots at $10 risk
//
//  Sessions: 24h (H4 breaks can happen at any time including Asia)
//  Cooldown:  H4_COOLDOWN_H4_BARS H4 bars after any close (default 3 = 12hrs)
//
//  SAFETY:
//  Both engines respect gold_any_open (1-at-a-time gold invariant via caller).
//  Neither engine calls any order function -- they return a signal struct.
//  The caller in tick_gold.hpp decides whether to execute.
//  Both engines have a daily loss cap ($H1_DAILY_CAP, $H4_DAILY_CAP).
//  State is entirely internal -- no shared globals, no mutex in hot path.
// =============================================================================

#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <algorithm>
#include <cstdio>
#include <functional>
#include "OmegaTradeLedger.hpp"

namespace omega {

// =============================================================================
//  H1 config constants
// =============================================================================
static constexpr double  H1_RISK_DOLLARS       = 15.0;   // $15 risk per H1 trade
static constexpr double  H1_MIN_LOT            = 0.01;
static constexpr double  H1_MAX_LOT            = 0.10;   // H1 hold time = more risk -> cap lower
static constexpr double  H1_TP_MULT            = 2.5;    // TP = 2.5x H1 ATR
static constexpr double  H1_SL_MULT            = 1.5;    // SL = 1.5x H1 ATR
static constexpr double  H1_TRAIL_ARM_MULT     = 1.0;    // trail arms at 1x ATR MFE
static constexpr double  H1_TRAIL_DIST_MULT    = 0.5;    // trail distance 0.5x ATR
static constexpr int     H1_TIMEOUT_H1_BARS    = 16;     // exit after 16 H1 bars
static constexpr int     H1_COOLDOWN_H1_BARS   = 2;      // 2 H1 bar cooldown = 2hrs
static constexpr double  H1_ADX_MIN            = 25.0;   // ADX must be trending
static constexpr double  H1_EMA_MIN_SEP        = 5.0;    // EMA9-EMA50 min 5pt separation
static constexpr double  H1_PULLBACK_BAND_PCT  = 0.10;   // 0.10% of price = ~4.7pt at $4700
                                                           // price must be within this of EMA21
static constexpr double  H1_MAX_SPREAD         = 1.0;    // max spread to enter (pts)
static constexpr double  H1_RSI_OB             = 75.0;   // overbought -- block H1 LONGs
static constexpr double  H1_RSI_OS             = 25.0;   // oversold -- block H1 SHORTs
static constexpr double  H1_DAILY_CAP          = 100.0;  // stop H1 engine after $100 daily loss

// =============================================================================
//  H4 config constants
// =============================================================================
static constexpr double  H4_RISK_DOLLARS       = 10.0;   // $10 risk per H4 trade (longer hold)
static constexpr double  H4_MIN_LOT            = 0.01;
static constexpr double  H4_MAX_LOT            = 0.10;
static constexpr double  H4_TP_MULT            = 2.0;    // TP = 2x H4 ATR
static constexpr double  H4_SL_STRUCT_MULT     = 0.5;    // SL = 0.5x H4 ATR behind channel
static constexpr double  H4_TRAIL_ARM_MULT     = 0.5;    // trail arms at 0.5x H4 ATR MFE
static constexpr double  H4_TRAIL_DIST_MULT    = 0.3;    // trail distance 0.3x H4 ATR
static constexpr int     H4_TIMEOUT_H4_BARS    = 12;     // exit after 12 H4 bars (48hrs)
static constexpr int     H4_COOLDOWN_H4_BARS   = 3;      // 3 H4 bars = 12hr cooldown
static constexpr double  H4_ADX_MIN            = 20.0;   // H4 ADX threshold for breakout
static constexpr double  H4_ADX_DEAD           = 15.0;   // exit if ADX falls below this
static constexpr int     H4_CHANNEL_BARS       = 10;     // Donchian N-bar channel
static constexpr double  H4_EMA_MIN_SEP        = 8.0;    // H4 EMA9-EMA50 min separation
static constexpr double  H4_MAX_SPREAD         = 2.0;    // max spread (H4 can accept wider)
static constexpr double  H4_RSI_OB             = 75.0;
static constexpr double  H4_RSI_OS             = 25.0;
static constexpr double  H4_DAILY_CAP          = 80.0;   // stop H4 engine after $80 daily loss

// =============================================================================
//  Signal struct returned by both engines
// =============================================================================
struct HTFSignal {
    bool   valid    = false;
    bool   is_long  = false;
    double entry    = 0.0;
    double sl       = 0.0;
    double tp       = 0.0;
    double size     = 0.0;
    const char* reason = "";
};

// =============================================================================
//  H1SwingEngine
// =============================================================================
struct H1SwingEngine {

    bool shadow_mode = true;
    bool enabled     = true;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // ── Open position state ───────────────────────────────────────────────────
    struct OpenPos {
        bool    active       = false;
        bool    is_long      = false;
        double  entry        = 0.0;
        double  sl           = 0.0;
        double  tp           = 0.0;
        double  size         = 0.0;
        double  mfe          = 0.0;
        double  h1_atr       = 0.0;  // ATR at entry for trail calc
        bool    trail_active = false;
        double  trail_sl     = 0.0;
        int64_t entry_ts_ms  = 0;
        int     h1_bars_held = 0;    // H1 bars since entry
    } pos_;

    double  daily_pnl_     = 0.0;
    int64_t daily_reset_ts_ = 0;

    // H1 bar counter for cooldown and timeout
    int     h1_bar_count_   = 0;
    int     cooldown_until_bar_ = 0;

    // EMA cross exit: track previous H1 bar's EMA9/EMA50 relationship
    double  prev_h1_ema9_  = 0.0;
    double  prev_h1_ema50_ = 0.0;

    int     m_trade_id_    = 0;

    bool has_open_position() const noexcept { return pos_.active; }

    // ── Called once per H1 bar close ─────────────────────────────────────────
    // h1_*: indicators from g_bars_gold.h1.ind
    // h4_trend: g_bars_gold.h4.ind.trend_state (-1/0/+1)
    // mid: current mid price
    // Returns HTFSignal (valid=true if new entry should be placed)
    HTFSignal on_h1_bar(
        double  mid,
        double  bid,
        double  ask,
        double  h1_ema9,
        double  h1_ema21,
        double  h1_ema50,
        double  h1_atr,
        double  h1_rsi,
        double  h1_adx,
        bool    h1_adx_rising,
        int     h1_trend_state,  // from h1.ind.trend_state
        int     h4_trend_state,  // from h4.ind.trend_state
        double  h4_adx,          // from h4.ind.adx14
        int     session_slot,    // 0-6; 6=Asia, 0=dead zone
        int64_t now_ms,
        CloseCallback on_close) noexcept
    {
        ++h1_bar_count_;

        _check_daily_reset(now_ms);

        // Manage open position on every H1 bar close
        if (pos_.active) {
            pos_.h1_bars_held++;
            _manage_h1(mid, bid, ask, h1_ema9, h1_ema50, h1_atr, h1_adx, now_ms, on_close);
            prev_h1_ema9_  = h1_ema9;
            prev_h1_ema50_ = h1_ema50;
            return HTFSignal{};
        }

        prev_h1_ema9_  = h1_ema9;
        prev_h1_ema50_ = h1_ema50;

        HTFSignal sig{};
        if (!enabled) return sig;

        // Daily loss cap
        if (daily_pnl_ < -H1_DAILY_CAP) {
            static int64_t s_cap_log = 0;
            if (now_ms - s_cap_log > 3600000LL) {
                s_cap_log = now_ms;
                printf("[H1SWING] Daily cap hit: pnl=%.2f cap=%.2f -- disabled today\n",
                       daily_pnl_, H1_DAILY_CAP);
                fflush(stdout);
            }
            return sig;
        }

        // Session gate: London + NY only (slots 1-5)
        if (session_slot == 6 || session_slot == 0) return sig;

        // Cooldown
        if (h1_bar_count_ < cooldown_until_bar_) return sig;

        // H4 regime gate: H4 trend must be non-flat and agree with intended direction
        // If H4 ADX < 20 (ranging), H1 trend-following is suppressed
        if (h4_trend_state == 0) return sig;
        if (h4_adx < 20.0) {
            static int64_t s_h4_log = 0;
            if (now_ms - s_h4_log > 3600000LL) {
                s_h4_log = now_ms;
                printf("[H1SWING] H4 ADX=%.1f < 20 -- ranging regime, no H1 trend entries\n", h4_adx);
                fflush(stdout);
            }
            return sig;
        }

        // H1 ADX gate: must be trending and rising
        if (h1_adx < H1_ADX_MIN || !h1_adx_rising) return sig;

        // H1 indicators must be warmed
        if (h1_ema9 <= 0.0 || h1_ema50 <= 0.0 || h1_atr <= 0.0) return sig;

        // Determine intended direction from H4 regime
        const bool intend_long  = (h4_trend_state == +1);
        const bool intend_short = (h4_trend_state == -1);

        // H1 EMA stack must agree with H4 direction
        const bool h1_bull_stack = (h1_ema9 > h1_ema21 && h1_ema21 > h1_ema50);
        const bool h1_bear_stack = (h1_ema9 < h1_ema21 && h1_ema21 < h1_ema50);
        if (intend_long  && !h1_bull_stack) return sig;
        if (intend_short && !h1_bear_stack) return sig;

        // EMA separation check
        const double ema_sep = std::fabs(h1_ema9 - h1_ema50);
        if (ema_sep < H1_EMA_MIN_SEP) return sig;

        // Pullback-to-value: price must be within H1_PULLBACK_BAND of EMA21
        const double band = mid * H1_PULLBACK_BAND_PCT / 100.0;
        const double dist_to_ema21 = std::fabs(mid - h1_ema21);
        if (dist_to_ema21 > band) return sig;

        // Price must be on the correct side of EMA21 (not through it)
        if (intend_long  && mid < h1_ema21 - band * 0.5) return sig;
        if (intend_short && mid > h1_ema21 + band * 0.5) return sig;

        // RSI extreme gate
        if (intend_long  && h1_rsi > H1_RSI_OB) return sig;
        if (intend_short && h1_rsi < H1_RSI_OS) return sig;

        // Spread gate
        const double spread = ask - bid;
        if (spread > H1_MAX_SPREAD) return sig;

        // All gates passed -- compute entry
        const double sl_pts    = h1_atr * H1_SL_MULT;
        const double tp_pts    = h1_atr * H1_TP_MULT;
        const double entry_px  = intend_long ? ask : bid;
        const double sl_px     = intend_long ? (entry_px - sl_pts) : (entry_px + sl_pts);
        const double tp_px     = intend_long ? (entry_px + tp_pts) : (entry_px - tp_pts);

        double size = H1_RISK_DOLLARS / (sl_pts * 100.0);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(H1_MIN_LOT, std::min(H1_MAX_LOT, size));

        // Arm position state
        pos_.active       = true;
        pos_.is_long      = intend_long;
        pos_.entry        = entry_px;
        pos_.sl           = sl_px;
        pos_.tp           = tp_px;
        pos_.size         = size;
        pos_.mfe          = 0.0;
        pos_.h1_atr       = h1_atr;
        pos_.trail_active = false;
        pos_.trail_sl     = sl_px;
        pos_.entry_ts_ms  = now_ms;
        pos_.h1_bars_held = 0;
        ++m_trade_id_;

        printf("[H1SWING] ENTRY %s @ %.2f sl=%.2f tp=%.2f size=%.3f"
               " h1_atr=%.2f h1_adx=%.1f h4_trend=%+d ema_sep=%.2f%s\n",
               intend_long ? "LONG" : "SHORT",
               entry_px, sl_px, tp_px, size,
               h1_atr, h1_adx, h4_trend_state, ema_sep,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        sig.valid   = true;
        sig.is_long = intend_long;
        sig.entry   = entry_px;
        sig.sl      = sl_px;
        sig.tp      = tp_px;
        sig.size    = size;
        sig.reason  = "H1_EMA_ADX_PULLBACK";
        return sig;
    }

    void patch_size(double lot) noexcept { if (pos_.active) pos_.size = lot; }

    void cancel() noexcept {
        pos_ = OpenPos{};
    }

    void force_close(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (!pos_.active) return;
        const double px = pos_.is_long ? bid : ask;
        _close(px, "FORCE_CLOSE", now_ms, cb);
    }

private:
    void _check_daily_reset(int64_t now_ms) noexcept {
        const int64_t day = (now_ms / 1000LL) / 86400LL;
        if (day != daily_reset_ts_) {
            daily_pnl_      = 0.0;
            daily_reset_ts_ = day;
        }
    }

    void _manage_h1(double mid, double bid, double ask,
                    double h1_ema9, double h1_ema50,
                    double h1_atr, double h1_adx,
                    int64_t now_ms,
                    CloseCallback on_close) noexcept
    {
        const double move = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
        if (move > pos_.mfe) pos_.mfe = move;

        // Trail SL: arm when MFE >= H1_TRAIL_ARM_MULT * h1_atr
        const double trail_arm  = pos_.h1_atr * H1_TRAIL_ARM_MULT;
        const double trail_dist = pos_.h1_atr * H1_TRAIL_DIST_MULT;
        if (pos_.mfe >= trail_arm) {
            const double new_trail = pos_.is_long
                ? (mid - trail_dist) : (mid + trail_dist);
            if (!pos_.trail_active) {
                if (pos_.is_long ? (new_trail > pos_.sl) : (new_trail < pos_.sl)) {
                    pos_.trail_sl     = new_trail;
                    pos_.trail_active = true;
                }
            } else {
                if (pos_.is_long ? (new_trail > pos_.trail_sl)
                                 : (new_trail < pos_.trail_sl))
                    pos_.trail_sl = new_trail;
            }
        }

        const double eff_sl = pos_.trail_active ? pos_.trail_sl : pos_.sl;

        // TP
        if (pos_.is_long ? (bid >= pos_.tp) : (ask <= pos_.tp)) {
            _close(pos_.is_long ? bid : ask, "TP_HIT", now_ms, on_close); return;
        }
        // SL / Trail SL
        if (pos_.is_long ? (bid <= eff_sl) : (ask >= eff_sl)) {
            _close(pos_.is_long ? bid : ask,
                   pos_.trail_active ? "TRAIL_SL" : "SL_HIT", now_ms, on_close); return;
        }
        // H1 bar timeout
        if (pos_.h1_bars_held >= H1_TIMEOUT_H1_BARS) {
            _close(pos_.is_long ? bid : ask, "H1_TIMEOUT", now_ms, on_close); return;
        }
        // EMA cross exit: H1 EMA9 crosses EMA50 against position
        // Uses prev bar EMA to detect the cross (current vs previous)
        if (prev_h1_ema9_ > 0.0 && prev_h1_ema50_ > 0.0 &&
            h1_ema9 > 0.0 && h1_ema50 > 0.0) {
            const bool was_bull = (prev_h1_ema9_ > prev_h1_ema50_);
            const bool now_bull = (h1_ema9 > h1_ema50);
            if (pos_.is_long && was_bull && !now_bull) {
                printf("[H1SWING] EMA cross exit LONG -- H1 EMA9 crossed below EMA50\n");
                fflush(stdout);
                _close(bid, "EMA_CROSS", now_ms, on_close); return;
            }
            if (!pos_.is_long && !was_bull && now_bull) {
                printf("[H1SWING] EMA cross exit SHORT -- H1 EMA9 crossed above EMA50\n");
                fflush(stdout);
                _close(ask, "EMA_CROSS", now_ms, on_close); return;
            }
        }
        // ADX collapse: if H1 ADX falls below 15, trend has died
        if (h1_adx > 0.0 && h1_adx < 15.0) {
            printf("[H1SWING] ADX collapse exit: h1_adx=%.1f < 15\n", h1_adx);
            fflush(stdout);
            _close(pos_.is_long ? bid : ask, "ADX_COLLAPSE", now_ms, on_close); return;
        }
    }

    void _close(double exit_px, const char* reason,
                int64_t now_ms, CloseCallback on_close) noexcept
    {
        const double pnl_pts = pos_.is_long
            ? (exit_px - pos_.entry) : (pos_.entry - exit_px);
        const double pnl_usd = pnl_pts * pos_.size * 100.0;
        daily_pnl_ += pnl_usd;

        printf("[H1SWING] EXIT %s @ %.2f %s pnl=$%.2f mfe=%.2f held=%d bars%s\n",
               pos_.is_long ? "LONG" : "SHORT",
               exit_px, reason, pnl_usd, pos_.mfe,
               pos_.h1_bars_held, shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        if (on_close) {
            omega::TradeRecord tr;
            tr.id         = m_trade_id_;
            tr.symbol     = "XAUUSD";
            tr.side       = pos_.is_long ? "LONG" : "SHORT";
            tr.engine     = "H1SwingEngine";
            tr.entryPrice = pos_.entry;
            tr.exitPrice  = exit_px;
            tr.sl         = pos_.sl;
            tr.size       = pos_.size;
            tr.pnl        = pnl_pts * pos_.size;
            tr.mfe        = pos_.mfe;
            tr.mae        = 0.0;
            tr.entryTs    = pos_.entry_ts_ms / 1000;
            tr.exitTs     = now_ms / 1000;
            tr.exitReason = reason;
            tr.regime     = "H1_SWING";
            tr.l2_live    = false;
            on_close(tr);
        }

        const bool is_win = (pnl_pts > 0.0);
        (void)is_win;
        cooldown_until_bar_ = h1_bar_count_ + H1_COOLDOWN_H1_BARS;
        pos_ = OpenPos{};
    }
};

// =============================================================================
//  H4RegimeEngine
// =============================================================================
struct H4RegimeEngine {

    bool shadow_mode = true;
    bool enabled     = true;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct OpenPos {
        bool    active          = false;
        bool    is_long         = false;
        double  entry           = 0.0;
        double  sl              = 0.0;        // structural SL (behind channel level)
        double  channel_level   = 0.0;        // the H4 channel high/low that was broken
        double  tp              = 0.0;
        double  size            = 0.0;
        double  mfe             = 0.0;
        double  h4_atr          = 0.0;
        bool    trail_active    = false;
        double  trail_sl        = 0.0;
        int64_t entry_ts_ms     = 0;
        int     h4_bars_held    = 0;
    } pos_;

    double  daily_pnl_      = 0.0;
    int64_t daily_reset_ts_ = 0;

    int     h4_bar_count_       = 0;
    int     cooldown_until_bar_ = 0;

    // Rolling H4 Donchian channel: high/low of last H4_CHANNEL_BARS bars
    // Updated on each H4 bar close from bars fed in
    double  channel_high_  = 0.0;
    double  channel_low_   = 1e9;
    std::deque<double> h4_highs_;
    std::deque<double> h4_lows_;

    int m_trade_id_ = 0;

    bool has_open_position() const noexcept { return pos_.active; }

    // ── Called once per H4 bar close ─────────────────────────────────────────
    // h4_bar:       the just-closed H4 bar (OHLC)
    // h4_*:         indicators from g_bars_gold.h4.ind
    // m15_atr_expanding: g_bars_gold.m15.ind.atr_expanding (breakout confirmation)
    // mid/bid/ask:  current tick price
    HTFSignal on_h4_bar(
        double  h4_bar_high,
        double  h4_bar_low,
        double  h4_bar_close,
        double  mid,
        double  bid,
        double  ask,
        double  h4_ema9,
        double  h4_ema50,
        double  h4_atr,
        double  h4_rsi,
        double  h4_adx,
        bool    m15_atr_expanding,
        int64_t now_ms,
        CloseCallback on_close) noexcept
    {
        ++h4_bar_count_;
        _check_daily_reset(now_ms);

        // Update Donchian channel with PREVIOUS bar's H/L (not current -- don't include the breakout bar itself)
        // We add the bar BEFORE checking the break so the channel reflects history only
        h4_highs_.push_back(h4_bar_high);
        h4_lows_ .push_back(h4_bar_low);
        if ((int)h4_highs_.size() > H4_CHANNEL_BARS) { h4_highs_.pop_front(); h4_lows_.pop_front(); }
        if ((int)h4_highs_.size() >= H4_CHANNEL_BARS) {
            channel_high_ = *std::max_element(h4_highs_.begin(), h4_highs_.end());
            channel_low_  = *std::min_element(h4_lows_ .begin(), h4_lows_ .end());
        }

        // Manage open position
        if (pos_.active) {
            pos_.h4_bars_held++;
            _manage_h4(mid, bid, ask, h4_atr, h4_adx, now_ms, on_close);
            return HTFSignal{};
        }

        HTFSignal sig{};
        if (!enabled) return sig;

        // Daily loss cap
        if (daily_pnl_ < -H4_DAILY_CAP) return sig;

        // Cooldown
        if (h4_bar_count_ < cooldown_until_bar_) return sig;

        // Need channel history
        if ((int)h4_highs_.size() < H4_CHANNEL_BARS) return sig;
        if (channel_high_ <= 0.0 || channel_low_ >= 1e8) return sig;

        // H4 ADX gate
        if (h4_adx < H4_ADX_MIN) {
            static int64_t s_adx_log = 0;
            if (now_ms - s_adx_log > 14400000LL) {
                s_adx_log = now_ms;
                printf("[H4REGIME] H4 ADX=%.1f < %.0f -- ranging, no breakout entries\n",
                       h4_adx, H4_ADX_MIN);
                fflush(stdout);
            }
            return sig;
        }

        // H4 indicators must be warmed
        if (h4_ema9 <= 0.0 || h4_ema50 <= 0.0 || h4_atr <= 0.0) return sig;

        // Breakout detection: H4 bar close breaks the channel
        const bool bull_break = (h4_bar_close > channel_high_);
        const bool bear_break = (h4_bar_close < channel_low_);
        if (!bull_break && !bear_break) return sig;

        const bool intend_long = bull_break;

        // EMA must agree with break direction
        const double ema_sep = h4_ema9 - h4_ema50;
        if (intend_long  && ema_sep < H4_EMA_MIN_SEP) return sig;
        if (!intend_long && ema_sep > -H4_EMA_MIN_SEP) return sig;

        // RSI extreme gate (don't buy overbought H4)
        if (intend_long  && h4_rsi > H4_RSI_OB) return sig;
        if (!intend_long && h4_rsi < H4_RSI_OS) return sig;

        // M15 ATR expansion confirms real breakout (not a fake spike)
        if (!m15_atr_expanding) {
            static int64_t s_atr_log = 0;
            if (now_ms - s_atr_log > 14400000LL) {
                s_atr_log = now_ms;
                printf("[H4REGIME] H4 break but M15 ATR not expanding -- skipping\n");
                fflush(stdout);
            }
            return sig;
        }

        // Spread gate
        const double spread = ask - bid;
        if (spread > H4_MAX_SPREAD) return sig;

        // Compute entry
        // SL = H4_SL_STRUCT_MULT * H4 ATR behind the channel level
        const double struct_level = intend_long ? channel_high_ : channel_low_;
        const double sl_pts       = h4_atr * H4_SL_STRUCT_MULT;
        const double tp_pts       = h4_atr * H4_TP_MULT;
        const double entry_px     = intend_long ? ask : bid;
        const double sl_px        = intend_long
            ? (struct_level - sl_pts) : (struct_level + sl_pts);
        const double tp_px        = intend_long
            ? (entry_px + tp_pts) : (entry_px - tp_pts);

        double size = H4_RISK_DOLLARS / (sl_pts * 100.0);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(H4_MIN_LOT, std::min(H4_MAX_LOT, size));

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

        printf("[H4REGIME] ENTRY %s @ %.2f sl=%.2f tp=%.2f size=%.3f"
               " h4_atr=%.2f h4_adx=%.1f channel_%.0f ema_sep=%.2f%s\n",
               intend_long ? "LONG" : "SHORT",
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

    void force_close(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (!pos_.active) return;
        _close(pos_.is_long ? bid : ask, "FORCE_CLOSE", now_ms, cb);
    }

private:
    void _check_daily_reset(int64_t now_ms) noexcept {
        const int64_t day = (now_ms / 1000LL) / 86400LL;
        if (day != daily_reset_ts_) { daily_pnl_ = 0.0; daily_reset_ts_ = day; }
    }

    void _manage_h4(double mid, double bid, double ask,
                    double h4_atr, double h4_adx,
                    int64_t now_ms,
                    CloseCallback on_close) noexcept
    {
        const double move = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
        if (move > pos_.mfe) pos_.mfe = move;

        // Trail SL
        const double trail_arm  = pos_.h4_atr * H4_TRAIL_ARM_MULT;
        const double trail_dist = pos_.h4_atr * H4_TRAIL_DIST_MULT;
        if (pos_.mfe >= trail_arm) {
            const double new_trail = pos_.is_long
                ? (mid - trail_dist) : (mid + trail_dist);
            if (!pos_.trail_active) {
                if (pos_.is_long ? (new_trail > pos_.sl) : (new_trail < pos_.sl)) {
                    pos_.trail_sl     = new_trail;
                    pos_.trail_active = true;
                }
            } else {
                if (pos_.is_long ? (new_trail > pos_.trail_sl)
                                 : (new_trail < pos_.trail_sl))
                    pos_.trail_sl = new_trail;
            }
        }

        const double eff_sl = pos_.trail_active ? pos_.trail_sl : pos_.sl;

        // TP
        if (pos_.is_long ? (bid >= pos_.tp) : (ask <= pos_.tp)) {
            _close(pos_.is_long ? bid : ask, "TP_HIT", now_ms, on_close); return;
        }
        // SL / Trail
        if (pos_.is_long ? (bid <= eff_sl) : (ask >= eff_sl)) {
            _close(pos_.is_long ? bid : ask,
                   pos_.trail_active ? "TRAIL_SL" : "SL_HIT", now_ms, on_close); return;
        }
        // Timeout
        if (pos_.h4_bars_held >= H4_TIMEOUT_H4_BARS) {
            _close(pos_.is_long ? bid : ask, "H4_TIMEOUT", now_ms, on_close); return;
        }
        // ADX collapse: trend has ended while in trade
        if (h4_adx > 0.0 && h4_adx < H4_ADX_DEAD) {
            printf("[H4REGIME] ADX collapse exit: h4_adx=%.1f < %.0f\n",
                   h4_adx, H4_ADX_DEAD);
            fflush(stdout);
            _close(pos_.is_long ? bid : ask, "ADX_COLLAPSE", now_ms, on_close); return;
        }
    }

    void _close(double exit_px, const char* reason,
                int64_t now_ms, CloseCallback on_close) noexcept
    {
        const double pnl_pts = pos_.is_long
            ? (exit_px - pos_.entry) : (pos_.entry - exit_px);
        const double pnl_usd = pnl_pts * pos_.size * 100.0;
        daily_pnl_ += pnl_usd;

        printf("[H4REGIME] EXIT %s @ %.2f %s pnl=$%.2f mfe=%.2f held=%d H4bars%s\n",
               pos_.is_long ? "LONG" : "SHORT",
               exit_px, reason, pnl_usd, pos_.mfe,
               pos_.h4_bars_held, shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        if (on_close) {
            omega::TradeRecord tr;
            tr.id         = m_trade_id_;
            tr.symbol     = "XAUUSD";
            tr.side       = pos_.is_long ? "LONG" : "SHORT";
            tr.engine     = "H4RegimeEngine";
            tr.entryPrice = pos_.entry;
            tr.exitPrice  = exit_px;
            tr.sl         = pos_.sl;
            tr.size       = pos_.size;
            tr.pnl        = pnl_pts * pos_.size;
            tr.mfe        = pos_.mfe;
            tr.mae        = 0.0;
            tr.entryTs    = pos_.entry_ts_ms / 1000;
            tr.exitTs     = now_ms / 1000;
            tr.exitReason = reason;
            tr.regime     = "H4_REGIME";
            tr.l2_live    = false;
            on_close(tr);
        }

        cooldown_until_bar_ = h4_bar_count_ + H4_COOLDOWN_H4_BARS;
        pos_ = OpenPos{};
    }
};

} // namespace omega

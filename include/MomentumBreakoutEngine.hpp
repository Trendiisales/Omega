// =============================================================================
//  MomentumBreakoutEngine.hpp
//
//  Pure price/EMA/ATR momentum breakout. Zero DOM dependency.
//
//  Entry:
//    1. EMA20 > EMA50 = uptrend  (EMA20 < EMA50 = downtrend)
//    2. Price breaks prev candle high (long) or low (short)
//       by ATR * ATR_BREAK_MULT
//    3. Spread <= MAX_SPREAD
//
//  Exit (first of):
//    1. TP hit:        entry +/- ATR * ATR_TP_MULT
//    2. SL hit:        entry -/+ ATR * ATR_SL_MULT
//    3. EMA cross:     EMA20 crosses EMA50 against position
//    4. Timeout:       held > TIMEOUT_BARS M1 bars with no TP
//
//  No DOM. No RSI. No L2. No regime gate.
//  Session filter: London + NY only (07:00-22:00 UTC).
// =============================================================================

#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <deque>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <iomanip>
#include "OmegaTradeLedger.hpp"

namespace omega {

// -----------------------------------------------------------------------------
//  Config
// -----------------------------------------------------------------------------
static constexpr int     MBE_EMA_FAST        = 20;
static constexpr int     MBE_EMA_SLOW        = 50;
static constexpr int     MBE_ATR_PERIOD      = 14;
static constexpr double  MBE_ATR_BREAK_MULT  = 1.5;   // breakout = ATR * 1.5
static constexpr double  MBE_ATR_SL_MULT     = 1.2;   // SL = ATR * 1.2
static constexpr double  MBE_ATR_TP_MULT     = 2.5;   // TP = ATR * 2.5
static constexpr double  MBE_MAX_SPREAD      = 0.40;  // max spread to enter
static constexpr double  MBE_EMA_MIN_SEP     = 0.10;  // min EMA separation (pts) to confirm trend
static constexpr int     MBE_TIMEOUT_BARS    = 20;    // exit after 20 M1 bars if TP not hit
static constexpr int64_t MBE_COOLDOWN_MS     = 10000; // 10s cooldown after exit
static constexpr double  MBE_RISK_DOLLARS    = 30.0;
static constexpr double  MBE_MIN_LOT         = 0.01;
static constexpr double  MBE_MAX_LOT         = 0.20;  // capped 0.50->0.20: matches all gold engine hard ceiling
static constexpr double  MBE_TICK_VALUE      = 100.0; // XAUUSD $100/pt/lot

// Session filter: only trade 07:00-22:00 UTC (London + NY)
static constexpr int     MBE_SESSION_START_H = 7;
static constexpr int     MBE_SESSION_END_H   = 22;

// -----------------------------------------------------------------------------
struct MomentumBreakoutEngine {

    bool   shadow_mode  = true;
    double risk_dollars = MBE_RISK_DOLLARS;

    enum class Phase { IDLE, LIVE, COOLDOWN } phase = Phase::IDLE;

    struct OpenPos {
        bool    active      = false;
        bool    is_long     = false;
        double  entry       = 0.0;
        double  sl          = 0.0;
        double  tp          = 0.0;
        double  size        = 0.01;
        double  atr_entry   = 0.0;
        int64_t entry_ts_ms = 0;
        int     bars_held   = 0;   // M1 bars since entry
        double  mfe         = 0.0;
    } pos;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    bool has_open_position() const noexcept { return phase == Phase::LIVE; }

    void force_close(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (!has_open_position()) return;
        close_pos(pos.is_long ? bid : ask, "FORCE_CLOSE", now_ms, cb);
    }

    // -------------------------------------------------------------------------
    // Called every M1 bar close from tick_gold.hpp
    // bar_open/high/low/close: last completed M1 bar
    // prev_high/prev_low:      previous M1 bar high/low
    // ema_fast/ema_slow:       pre-computed EMAs from OHLCBarEngine
    // atr:                     pre-computed ATR14 from OHLCBarEngine
    // bid/ask:                 current quote
    // now_ms:                  epoch ms
    // session_hour_utc:        current UTC hour (0-23)
    // -------------------------------------------------------------------------
    void on_bar(double bar_open, double bar_high, double bar_low, double bar_close,
                double prev_high, double prev_low,
                double ema_fast, double ema_slow, double atr,
                double bid, double ask,
                int64_t now_ms,
                int session_hour_utc,
                CloseCallback on_close) noexcept
    {
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // ── Advance bar counter for open position ────────────────────────────
        if (phase == Phase::LIVE && pos.active) {
            pos.bars_held++;
            const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
            if (move > pos.mfe) pos.mfe = move;
        }

        // ── Cooldown ─────────────────────────────────────────────────────────
        if (phase == Phase::COOLDOWN) {
            if (now_ms - m_cooldown_start_ms >= MBE_COOLDOWN_MS)
                phase = Phase::IDLE;
            else
                return;
        }

        // ── Manage open position ─────────────────────────────────────────────
        if (phase == Phase::LIVE) {
            manage(bid, ask, mid, ema_fast, ema_slow, now_ms, on_close);
            return;
        }

        // ── IDLE: check for entry ────────────────────────────────────────────
        if (atr <= 0.0 || ema_fast <= 0.0 || ema_slow <= 0.0) return;
        if (spread > MBE_MAX_SPREAD) return;

        // Session filter: London + NY only
        if (session_hour_utc < MBE_SESSION_START_H ||
            session_hour_utc >= MBE_SESSION_END_H) return;

        // Gate 1: EMA trend with minimum separation
        const double ema_sep = ema_fast - ema_slow;
        const bool uptrend   = (ema_sep >  MBE_EMA_MIN_SEP);
        const bool downtrend = (ema_sep < -MBE_EMA_MIN_SEP);
        if (!uptrend && !downtrend) return;

        // Gate 2: price breakout past prev candle high/low by ATR * mult
        const double breakout = atr * MBE_ATR_BREAK_MULT;
        const bool long_break  = uptrend   && (bar_close > prev_high + breakout);
        const bool short_break = downtrend && (bar_close < prev_low  - breakout);
        if (!long_break && !short_break) return;

        // All gates passed — enter
        enter(long_break, bid, ask, atr, now_ms);
    }

    // -------------------------------------------------------------------------
    // on_tick: called every tick while position is LIVE for SL/TP monitoring
    // (bar close exits are handled in on_bar, tick-level SL/TP here)
    // -------------------------------------------------------------------------
    void on_tick(double bid, double ask, int64_t now_ms, CloseCallback on_close) noexcept {
        if (phase != Phase::LIVE || !pos.active) return;

        const double mid  = (bid + ask) * 0.5;
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;

        // SL
        if (pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl)) {
            close_pos(pos.is_long ? bid : ask, "SL_HIT", now_ms, on_close);
            return;
        }

        // TP
        if (pos.is_long ? (ask >= pos.tp) : (bid <= pos.tp)) {
            close_pos(pos.is_long ? ask : bid, "TP_HIT", now_ms, on_close);
            return;
        }
    }

private:
    int64_t m_cooldown_start_ms = 0;
    int     m_trade_id          = 0;

    // -------------------------------------------------------------------------
    void manage(double bid, double ask, double mid,
                double ema_fast, double ema_slow,
                int64_t now_ms,
                CloseCallback on_close) noexcept
    {
        if (!pos.active) return;

        // SL check
        if (pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl)) {
            close_pos(pos.is_long ? bid : ask, "SL_HIT", now_ms, on_close);
            return;
        }

        // TP check
        if (pos.is_long ? (ask >= pos.tp) : (bid <= pos.tp)) {
            close_pos(pos.is_long ? ask : bid, "TP_HIT", now_ms, on_close);
            return;
        }

        // EMA cross exit: fast crosses slow against our direction
        const bool ema_cross_against =
            (pos.is_long  && ema_fast < ema_slow - MBE_EMA_MIN_SEP) ||
            (!pos.is_long && ema_fast > ema_slow + MBE_EMA_MIN_SEP);
        if (ema_cross_against) {
            const double px = pos.is_long ? bid : ask;
            std::cout << "[MBE] EMA-CROSS-EXIT " << (pos.is_long?"LONG":"SHORT")
                      << " ema_fast=" << std::fixed << std::setprecision(2) << ema_fast
                      << " ema_slow=" << ema_slow
                      << " @ " << px << "\n";
            std::cout.flush();
            close_pos(px, "EMA_CROSS", now_ms, on_close);
            return;
        }

        // Timeout exit: held > TIMEOUT_BARS without hitting TP
        if (pos.bars_held >= MBE_TIMEOUT_BARS) {
            const double px = pos.is_long ? bid : ask;
            std::cout << "[MBE] TIMEOUT-EXIT " << (pos.is_long?"LONG":"SHORT")
                      << " bars_held=" << pos.bars_held
                      << " mfe=" << std::fixed << std::setprecision(3) << pos.mfe
                      << " @ " << std::setprecision(2) << px << "\n";
            std::cout.flush();
            close_pos(px, "TIMEOUT", now_ms, on_close);
            return;
        }
    }

    // -------------------------------------------------------------------------
    void enter(bool is_long, double bid, double ask,
               double atr, int64_t now_ms) noexcept
    {
        const double entry_px = is_long ? ask : bid;
        const double sl_pts   = atr * MBE_ATR_SL_MULT;
        const double tp_pts   = atr * MBE_ATR_TP_MULT;
        const double sl_px    = is_long ? (entry_px - sl_pts) : (entry_px + sl_pts);
        const double tp_px    = is_long ? (entry_px + tp_pts) : (entry_px - tp_pts);

        double size = risk_dollars / (sl_pts * MBE_TICK_VALUE);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(MBE_MIN_LOT, std::min(MBE_MAX_LOT, size));

        pos.active      = true;
        pos.is_long     = is_long;
        pos.entry       = entry_px;
        pos.sl          = sl_px;
        pos.tp          = tp_px;
        pos.size        = size;
        pos.atr_entry   = atr;
        pos.entry_ts_ms = now_ms;
        pos.bars_held   = 0;
        pos.mfe         = 0.0;
        ++m_trade_id;
        phase = Phase::LIVE;

        std::cout << "[MBE] ENTRY " << (is_long?"LONG":"SHORT")
                  << " @ " << std::fixed << std::setprecision(2) << entry_px
                  << " sl=" << sl_px << " tp=" << tp_px
                  << " sl_pts=" << std::setprecision(3) << sl_pts
                  << " tp_pts=" << tp_pts
                  << " size=" << size
                  << " atr=" << std::setprecision(2) << atr
                  << (shadow_mode?" [SHADOW]":"") << "\n";
        std::cout.flush();
    }

    // -------------------------------------------------------------------------
    void close_pos(double exit_px, const char* reason,
                   int64_t now_ms, CloseCallback on_close) noexcept
    {
        omega::TradeRecord tr;
        tr.id         = m_trade_id;
        tr.symbol     = "XAUUSD";
        tr.side       = pos.is_long ? "LONG" : "SHORT";
        tr.entryPrice = pos.entry;
        tr.exitPrice  = exit_px;
        tr.sl         = pos.sl;
        tr.tp         = pos.tp;
        tr.size       = pos.size;
        tr.pnl        = (pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px))
                        * pos.size;
        tr.mfe        = pos.mfe * pos.size;
        tr.mae        = 0.0;
        tr.entryTs    = pos.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.engine     = "MomentumBreakoutEngine";
        tr.regime     = "MOMENTUM_BREAKOUT";
        tr.l2_live    = false;
        tr.shadow     = shadow_mode;

        std::cout << "[MBE] EXIT " << (pos.is_long?"LONG":"SHORT")
                  << " @ " << std::fixed << std::setprecision(2) << exit_px
                  << " reason=" << reason
                  << " pnl_raw=" << std::setprecision(4) << tr.pnl
                  << " pnl_usd=" << std::setprecision(2) << (tr.pnl * MBE_TICK_VALUE)
                  << " mfe=" << std::setprecision(3) << pos.mfe
                  << " bars=" << pos.bars_held
                  << (shadow_mode?" [SHADOW]":"") << "\n";
        std::cout.flush();

        pos = OpenPos{};
        phase = Phase::COOLDOWN;
        m_cooldown_start_ms = now_ms;

        if (on_close) on_close(tr);
    }
};

} // namespace omega

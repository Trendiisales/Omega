#pragma once
// ==============================================================================
// BracketEngine — Breakout bracket with confirmation filter for Gold and Silver
//
// State machine:
//   IDLE       → not enough data / session closed / range too small
//   ARMED      → bracket levels set, waiting for initial touch
//   CONFIRMING → price touched bracket, waiting for CONFIRM_MOVE continuation
//   PENDING    → confirmed breakout, order sent, awaiting broker fill
//   LIVE       → fill confirmed, managing open position (TP/SL + min hold)
//   COOLDOWN   → position closed, waiting before re-arming
//
// Key fixes:
//   ✅ CONFIRMING phase kills fake/touch breakouts
//   ✅ MIN_RANGE filter — no arming in dead tape
//   ✅ MIN_HOLD_MS — prevents 2-15s noise SL hits
//   ✅ PENDING phase: pos.active=false until confirm_fill() called
//   ✅ Self-inclusion bug fixed: structure uses window[0..N-2]
//   ✅ has_open_position() covers PENDING+LIVE — blocks all other engines
//   ✅ on_close() / on_reject() safe state transitions
//   ✅ configure() for clean parameterisation
//
// Used by: GOLD.F, XAGUSD
// ==============================================================================

#include <deque>
#include <string>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <functional>
#include "OmegaTradeLedger.hpp"

namespace omega {

enum class BracketPhase : uint8_t {
    IDLE       = 0,
    ARMED      = 1,
    CONFIRMING = 2,
    PENDING    = 3,
    LIVE       = 4,
    COOLDOWN   = 5,
};

struct BracketSignal {
    bool        valid   = false;
    bool        is_long = true;
    double      entry   = 0.0;
    double      tp      = 0.0;
    double      sl      = 0.0;
    const char* reason  = "";
};

class BracketEngine {
public:
    const char* symbol = "???";

    // ── Config ────────────────────────────────────────────────────────────────
    // All set via configure() — do not assign fields directly after that call.
    int    STRUCTURE_LOOKBACK    = 25;
    double BUFFER                = 0.3;
    double RR                    = 0.9;
    int    COOLDOWN_MS           = 90000;
    double MIN_RANGE             = 0.0;
    double CONFIRM_MOVE          = 0.0;
    int    CONFIRM_TIMEOUT_MS    = 5000;
    int    MIN_HOLD_MS           = 15000;
    double VWAP_MIN_DIST         = 0.0;   // min distance from VWAP to allow entry
    int    MIN_STRUCTURE_MS      = 0;     // min ms range must hold before arming
    int    FAILURE_WINDOW_MS     = 5000;  // ms after fill to check for breakout failure

    // ATR volatility scaling — when ATR_PERIOD > 0, CONFIRM_MOVE and MIN_RANGE
    // are scaled by current ATR instead of using static values.
    // ATR_CONFIRM_K * atr replaces CONFIRM_MOVE (0 = use static)
    // ATR_RANGE_K   * atr replaces MIN_RANGE    (0 = use static)
    int    ATR_PERIOD            = 0;     // 0 = disabled; recommended 20
    double ATR_CONFIRM_K         = 0.0;  // CONFIRM_MOVE = ATR * k; gold ~0.15, silver ~0.17
    double ATR_RANGE_K           = 0.0;  // MIN_RANGE    = ATR * k; gold ~1.5,  silver ~1.4

    // Legacy fields kept for telemetry reads — not used for logic.
    double ENTRY_SIZE            = 0.01;
    double SL_PCT                = 0.0;

    // ── Observable state ──────────────────────────────────────────────────────
    BracketPhase phase        = BracketPhase::IDLE;
    double       bracket_high = 0.0;
    double       bracket_low  = 0.0;
    int          signal_count = 0;
    double       atr          = 0.0;  // current ATR (readable by telemetry/logs)

    struct OpenPos {
        bool    active          = false;
        bool    is_long         = true;
        bool    sl_locked_to_be = false;
        double  entry           = 0.0;
        double  tp              = 0.0;
        double  sl              = 0.0;
        double  breakout_level  = 0.0;  // bracket_high (long) or bracket_low (short) — for failure check
        double  size            = 0.01;
        double  mfe             = 0.0;
        double  mae             = 0.0;
        int64_t entry_ts        = 0;
        double  spread_at_entry = 0.0;
        char    regime[32]      = {};
    } pos;

    BracketSignal pending_sig;

    using CloseCallback = std::function<void(const TradeRecord&)>;

    // ── configure() — call once at startup before first tick ──────────────────
    void configure(double buffer,
                   int    lookback,
                   double rr,
                   int    cooldown_ms,
                   double min_range,
                   double confirm_move,
                   int    confirm_timeout_ms,
                   int    min_hold_ms,
                   double vwap_min_dist     = 0.0,
                   int    min_structure_ms  = 0,
                   int    failure_window_ms = 5000,
                   int    atr_period        = 0,
                   double atr_confirm_k     = 0.0,
                   double atr_range_k       = 0.0)
    {
        BUFFER              = buffer;
        STRUCTURE_LOOKBACK  = lookback;
        RR                  = rr;
        COOLDOWN_MS         = cooldown_ms;
        MIN_RANGE           = min_range;
        CONFIRM_MOVE        = confirm_move;
        CONFIRM_TIMEOUT_MS  = confirm_timeout_ms;
        MIN_HOLD_MS         = min_hold_ms;
        VWAP_MIN_DIST       = vwap_min_dist;
        MIN_STRUCTURE_MS    = min_structure_ms;
        FAILURE_WINDOW_MS   = failure_window_ms;
        ATR_PERIOD          = atr_period;
        ATR_CONFIRM_K       = atr_confirm_k;
        ATR_RANGE_K         = atr_range_k;
    }

    // ── has_open_position(): blocks other engines ─────────────────────────────
    bool has_open_position() const noexcept {
        return phase == BracketPhase::PENDING || phase == BracketPhase::LIVE;
    }

    // ── on_tick(): call every tick — does not return signal (use get_signal()) ─
    void on_tick(double bid, double ask, long long ts, bool can_enter,
                 const char* macro_regime, CloseCallback on_close,
                 double vwap = 0.0) noexcept
    {
        if (bid <= 0.0 || ask <= 0.0) return;

        m_last_bid = bid;
        m_last_ask = ask;
        m_last_ts  = ts;

        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // ── COOLDOWN ──────────────────────────────────────────────────────────
        if (phase == BracketPhase::COOLDOWN) {
            if (ts - m_cooldown_start >= static_cast<long long>(COOLDOWN_MS))
                phase = BracketPhase::IDLE;
            else
                return;
        }

        // ── LIVE: manage position ─────────────────────────────────────────────
        if (phase == BracketPhase::LIVE) {
            if (!pos.active) return;

            const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
            if ( move > pos.mfe) pos.mfe =  move;
            if (-move > pos.mae) pos.mae = -move;

            // Breakout failure detection within FAILURE_WINDOW_MS:
            // If price returns INSIDE the bracket (back through the breakout level)
            // immediately after fill, it's a liquidity sweep — exit early.
            // Uses breakout_level (bracket_high/low), NOT pos.sl (full SL distance).
            if (FAILURE_WINDOW_MS > 0 &&
                ts - pos.entry_ts < static_cast<long long>(FAILURE_WINDOW_MS)) {
                if ( pos.is_long && bid < pos.breakout_level) {
                    closePos(bid, "BREAKOUT_FAIL", macro_regime, on_close); return;
                }
                if (!pos.is_long && ask > pos.breakout_level) {
                    closePos(ask, "BREAKOUT_FAIL", macro_regime, on_close); return;
                }
            }

            // MIN_HOLD_MS: don't check SL/TP until position has been open long enough
            if (ts - pos.entry_ts < static_cast<long long>(MIN_HOLD_MS)) return;

            // Partial lock-in: move SL to breakeven at 60% TP progress
            // Converts "good move that reversed" trades into breakeven/small wins
            {
                const double tp_dist  = std::fabs(pos.tp - pos.entry);
                const double progress = tp_dist > 0.0
                    ? (pos.is_long ? (mid - pos.entry) : (pos.entry - mid)) / tp_dist
                    : 0.0;
                if (progress >= 0.60 && !pos.sl_locked_to_be) {
                    const double be = pos.entry;
                    if ( pos.is_long && be > pos.sl) { pos.sl = be; pos.sl_locked_to_be = true; }
                    if (!pos.is_long && be < pos.sl) { pos.sl = be; pos.sl_locked_to_be = true; }
                    if (pos.sl_locked_to_be) {
                        std::cout << "[BRACKET-" << symbol << "] SL LOCKED TO BE"
                                  << " progress=" << std::fixed << std::setprecision(2) << progress
                                  << " sl=" << pos.sl << "\n";
                        std::cout.flush();
                    }
                }
            }

            if ( pos.is_long && bid >= pos.tp) { closePos(pos.tp, "TP_HIT",  macro_regime, on_close); return; }
            if (!pos.is_long && ask <= pos.tp) { closePos(pos.tp, "TP_HIT",  macro_regime, on_close); return; }
            if ( pos.is_long && bid <= pos.sl) { closePos(pos.sl, pos.sl_locked_to_be ? "BE_HIT" : "SL_HIT", macro_regime, on_close); return; }
            if (!pos.is_long && ask >= pos.sl) { closePos(pos.sl, pos.sl_locked_to_be ? "BE_HIT" : "SL_HIT", macro_regime, on_close); return; }
            return;
        }

        // ── PENDING: in flight, waiting for confirm_fill() ────────────────────
        if (phase == BracketPhase::PENDING) return;

        // ── Session gate ──────────────────────────────────────────────────────
        if (!can_enter) {
            phase        = BracketPhase::IDLE;
            bracket_high = 0.0;
            bracket_low  = 0.0;
            return;
        }

        // Maintain structure window
        m_window.push_back(mid);
        if (static_cast<int>(m_window.size()) > STRUCTURE_LOOKBACK * 2)
            m_window.pop_front();

        if (static_cast<int>(m_window.size()) < STRUCTURE_LOOKBACK) return;

        // ── Compute structure — exclude current tick ──────────────────────────
        const int wsize = static_cast<int>(m_window.size());
        const auto wbegin = m_window.begin() + (wsize - STRUCTURE_LOOKBACK);
        const auto wend   = m_window.end() - 1;
        const double struct_hi = *std::max_element(wbegin, wend);
        const double struct_lo = *std::min_element(wbegin, wend);
        const double range     = struct_hi - struct_lo;

        // ── ATR update — rolling mean of per-tick bid-ask ranges ──────────────
        // Spread is a reasonable per-tick volatility proxy available on every tick.
        // True ATR (high-low per bar) would need bar aggregation; tick spread ATR
        // is a consistent, low-latency substitute that scales with volatility.
        if (ATR_PERIOD > 0) {
            m_atr_window.push_back(spread);
            if (static_cast<int>(m_atr_window.size()) > ATR_PERIOD * 3)
                m_atr_window.pop_front();
            if (static_cast<int>(m_atr_window.size()) >= ATR_PERIOD) {
                double sum = 0.0;
                const int n = static_cast<int>(m_atr_window.size());
                for (int i = n - ATR_PERIOD; i < n; ++i) sum += m_atr_window[i];
                atr = sum / ATR_PERIOD;
            }
        }

        // Effective params — ATR-scaled when ATR is ready, static otherwise
        const double eff_min_range    = (ATR_RANGE_K   > 0.0 && atr > 0.0)
                                        ? atr * ATR_RANGE_K   : MIN_RANGE;
        const double eff_confirm_move = (ATR_CONFIRM_K > 0.0 && atr > 0.0)
                                        ? atr * ATR_CONFIRM_K : CONFIRM_MOVE;

        // MIN_RANGE filter — don't arm in dead tape
        if (range < eff_min_range) {
            phase        = BracketPhase::IDLE;
            bracket_high = 0.0;
            bracket_low  = 0.0;
            return;
        }

        // VWAP distance filter — don't enter when price is near VWAP (sideways/chop)
        if (VWAP_MIN_DIST > 0.0 && vwap > 0.0 && std::fabs(mid - vwap) < VWAP_MIN_DIST) {
            phase        = BracketPhase::IDLE;
            bracket_high = 0.0;
            bracket_low  = 0.0;
            return;
        }

        const double buf = spread * 0.5;
        bracket_high = struct_hi + buf;
        bracket_low  = struct_lo - buf;

        // ── IDLE → ARMED ──────────────────────────────────────────────────────
        if (phase == BracketPhase::IDLE) {
            phase       = BracketPhase::ARMED;
            m_armed_ts  = ts;  // record when structure was established
            return;
        }

        // ── ARMED: watch for initial touch ────────────────────────────────────
        if (phase == BracketPhase::ARMED) {
            // Structure duration filter — range must hold for MIN_STRUCTURE_MS
            // before we allow a trigger. Removes drifting/noise ranges.
            if (MIN_STRUCTURE_MS > 0 && ts - m_armed_ts < static_cast<long long>(MIN_STRUCTURE_MS))
                return;
            if (ask >= bracket_high) {
                m_confirm_side       = 1;
                m_confirm_start_ts   = ts;
                m_eff_confirm_move   = eff_confirm_move;  // snapshot for next tick
                phase                = BracketPhase::CONFIRMING;
                std::cout << "[BRACKET-" << symbol << "] CONFIRMING LONG"
                          << " need ask>=" << bracket_high + eff_confirm_move
                          << (atr > 0.0 ? " (ATR-scaled)" : "") << "\n";
                std::cout.flush();
            } else if (bid <= bracket_low) {
                m_confirm_side       = -1;
                m_confirm_start_ts   = ts;
                m_eff_confirm_move   = eff_confirm_move;
                phase                = BracketPhase::CONFIRMING;
                std::cout << "[BRACKET-" << symbol << "] CONFIRMING SHORT"
                          << " need bid<=" << bracket_low - eff_confirm_move
                          << (atr > 0.0 ? " (ATR-scaled)" : "") << "\n";
                std::cout.flush();
            }
            return;
        }

        // ── CONFIRMING: wait for CONFIRM_MOVE continuation ────────────────────
        if (phase == BracketPhase::CONFIRMING) {
            if (m_confirm_side == 1) {
                if (ask >= bracket_high + m_eff_confirm_move) {
                    trigger(1, spread, macro_regime);
                    return;
                }
            } else {
                if (bid <= bracket_low - m_eff_confirm_move) {
                    trigger(-1, spread, macro_regime);
                    return;
                }
            }
            // Timeout — price touched but didn't follow through
            if (ts - m_confirm_start_ts > static_cast<long long>(CONFIRM_TIMEOUT_MS)) {
                std::cout << "[BRACKET-" << symbol << "] CONFIRM TIMEOUT — back to ARMED\n";
                std::cout.flush();
                m_confirm_side = 0;
                phase          = BracketPhase::ARMED;
            }
        }
    }

    // ── get_signal(): consume pending signal after on_tick() ──────────────────
    // Returns the signal and clears it. Call after on_tick() each tick.
    BracketSignal get_signal() noexcept {
        BracketSignal out = pending_sig;
        pending_sig       = BracketSignal{};
        return out;
    }

    // ── confirm_fill(): called by execution report handler on FILL ─────────────
    void confirm_fill(double actual_price, double actual_size) noexcept {
        if (phase != BracketPhase::PENDING) return;
        pos.active   = true;
        pos.entry    = actual_price;
        pos.size     = actual_size;
        pos.entry_ts = m_last_ts;
        pos.tp       = pending_sig.tp;
        pos.sl       = pending_sig.sl;
        pos.is_long  = pending_sig.is_long;
        phase        = BracketPhase::LIVE;
        std::cout << "[BRACKET-" << symbol << "] FILL CONFIRMED"
                  << " price=" << actual_price
                  << " size="  << actual_size
                  << " sl="    << pos.sl
                  << " tp="    << pos.tp << "\n";
        std::cout.flush();
    }

    // ── on_reject(): called by execution report handler on REJECT ──────────────
    void on_reject() noexcept {
        std::cout << "[BRACKET-" << symbol << "] ORDER REJECTED — resetting\n";
        std::cout.flush();
        reset();
    }

    // ── forceClose(): called at disconnect / EOD ───────────────────────────────
    void forceClose(double bid, double ask, const char* reason,
                    double /*latency_ms*/, const char* macro_regime,
                    CloseCallback on_close) noexcept
    {
        if (phase == BracketPhase::LIVE && pos.active)
            closePos((bid + ask) * 0.5, reason, macro_regime, on_close);
        else if (phase == BracketPhase::PENDING)
            reset();
    }

private:
    std::deque<double> m_window;
    std::deque<double> m_atr_window;   // rolling per-tick ranges for ATR
    int64_t  m_last_ts            = 0;
    double   m_last_bid           = 0.0;
    double   m_last_ask           = 0.0;
    int64_t  m_cooldown_start     = 0;
    int      m_confirm_side       = 0;
    int64_t  m_confirm_start_ts   = 0;
    double   m_eff_confirm_move   = 0.0;  // ATR-scaled confirm move, snapshotted at touch
    int64_t  m_armed_ts           = 0;
    int      m_trade_id           = 0;

    void trigger(int side, double spread, const char* macro_regime) noexcept
    {
        const double entry   = (side > 0 ? m_last_ask : m_last_bid);
        const double stop    = (side > 0 ? bracket_low  : bracket_high);
        const double dist    = std::fabs(entry - stop);
        const double tp      = entry + side * dist * RR;

        std::cout << "[BRACKET-" << symbol << "] TRIGGERED "
                  << (side > 0 ? "LONG" : "SHORT")
                  << " entry=" << entry
                  << " sl="    << stop
                  << " tp="    << tp
                  << " range=" << (bracket_high - bracket_low) << "\n";
        std::cout.flush();

        pos            = OpenPos{};
        pos.active     = false;
        pos.is_long    = (side > 0);
        pos.entry      = entry;
        pos.sl         = stop;
        pos.tp         = tp;
        pos.breakout_level = (side > 0) ? bracket_high : bracket_low;  // level price must stay past
        pos.size       = ENTRY_SIZE;
        pos.spread_at_entry = spread;
        if (macro_regime) strncpy_s(pos.regime, macro_regime, 31);

        pending_sig.valid   = true;
        pending_sig.is_long = (side > 0);
        pending_sig.entry   = entry;
        pending_sig.sl      = stop;
        pending_sig.tp      = tp;
        pending_sig.reason  = (side > 0) ? "BRACKET_LONG" : "BRACKET_SHORT";

        bracket_high = 0.0;
        bracket_low  = 0.0;
        m_confirm_side = 0;
        phase        = BracketPhase::PENDING;

        ++signal_count;
        ++m_trade_id;
    }

    void closePos(double exit_px, const char* reason,
                  const char* macro_regime,
                  CloseCallback& on_close) noexcept
    {
        if (!pos.active) return;

        TradeRecord tr;
        tr.id            = m_trade_id;
        tr.symbol        = symbol;
        tr.side          = pos.is_long ? "LONG" : "SHORT";
        tr.entryPrice    = pos.entry;
        tr.exitPrice     = exit_px;
        tr.tp            = pos.tp;
        tr.sl            = pos.sl;
        tr.size          = pos.size;
        tr.pnl           = (pos.is_long ? (exit_px - pos.entry)
                                        : (pos.entry - exit_px)) * pos.size;
        tr.mfe           = pos.mfe;
        tr.mae           = pos.mae;
        tr.entryTs       = pos.entry_ts;
        tr.exitTs        = m_last_ts;
        tr.exitReason    = reason;
        tr.spreadAtEntry = pos.spread_at_entry;
        tr.latencyMs     = 0.0;
        tr.engine        = std::string(symbol ? symbol : "???") + "_BRACKET";
        tr.regime        = (macro_regime && *macro_regime) ? macro_regime : pos.regime;

        pos              = OpenPos{};
        pending_sig      = BracketSignal{};
        m_cooldown_start = m_last_ts;
        phase            = BracketPhase::COOLDOWN;

        if (on_close) on_close(tr);
    }

    void reset() noexcept {
        pos                = OpenPos{};
        pending_sig        = BracketSignal{};
        bracket_high       = 0.0;
        bracket_low        = 0.0;
        m_confirm_side     = 0;
        m_eff_confirm_move = 0.0;
        m_armed_ts         = 0;
        phase              = BracketPhase::IDLE;
        // Note: m_atr_window intentionally NOT cleared on reset —
        // ATR history is continuous and should survive position resets
    }
};

} // namespace omega

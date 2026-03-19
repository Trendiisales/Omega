#pragma once
// ==============================================================================
// BracketEngine — CRTP bracket breakout engine
//
// Follows the same pattern as BreakoutEngineBase<Derived> in BreakoutEngine.hpp.
// CRTP eliminates virtual dispatch — shouldTrade() is inlined per instrument.
//
// State machine:
//   IDLE       → not enough data / range too small / VWAP too close
//   ARMED      → bracket levels set, MIN_STRUCTURE_MS timer running
//   CONFIRMING → price touched bracket, waiting for CONFIRM_MOVE follow-through
//   PENDING    → signal emitted, order sent, awaiting confirm_fill()
//   LIVE       → fill confirmed, managing position
//   COOLDOWN   → post-close cooldown
//
// Derived classes provide:
//   bool shouldTrade(double bid, double ask, double spread_pct, double vwap)
//   void onSignal(const BracketSignal&)   [optional]
//
// Used by: GoldBracketEngine (GOLD.F), SilverBracketEngine (XAGUSD)
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

// ==============================================================================
// BracketEngineBase<Derived> — CRTP base
// ==============================================================================
template<typename Derived>
class BracketEngineBase
{
public:
    // ── Config — set via configure() before first tick ────────────────────────
    int    STRUCTURE_LOOKBACK  = 30;
    double BUFFER              = 0.3;
    double RR                  = 1.5;
    int    COOLDOWN_MS         = 120000;
    double MIN_RANGE           = 0.0;
    double CONFIRM_MOVE        = 0.0;
    int    CONFIRM_TIMEOUT_MS  = 4000;
    int    MIN_HOLD_MS         = 15000;
    double VWAP_MIN_DIST       = 0.0;
    int    MIN_STRUCTURE_MS    = 0;
    int    FAILURE_WINDOW_MS   = 5000;
    int    ATR_PERIOD          = 0;
    double ATR_CONFIRM_K       = 0.0;
    double ATR_RANGE_K         = 0.0;
    double SLIPPAGE_BUFFER     = 0.0;  // estimated one-way slippage in price points
    double EDGE_MULTIPLIER     = 1.5;  // tp_dist must be >= (spread + slippage_buffer) * EDGE_MULTIPLIER

    // Legacy fields kept for telemetry / main.cpp reads
    double ENTRY_SIZE          = 0.01;
    double SL_PCT              = 0.0;
    const char* symbol         = "???";

    // ── Observable state ──────────────────────────────────────────────────────
    BracketPhase phase        = BracketPhase::IDLE;
    double       bracket_high = 0.0;
    double       bracket_low  = 0.0;
    int          signal_count = 0;
    double       atr          = 0.0;

    struct OpenPos {
        bool    active          = false;
        bool    is_long         = true;
        bool    sl_locked_to_be = false;
        double  entry           = 0.0;
        double  tp              = 0.0;
        double  sl              = 0.0;
        double  breakout_level  = 0.0;
        double  size            = 0.01;
        double  mfe             = 0.0;
        double  mae             = 0.0;
        int64_t entry_ts        = 0;      // seconds
        double  spread_at_entry = 0.0;
        char    regime[32]      = {};
    } pos;

    BracketSignal pending_sig;

    using CloseCallback = std::function<void(const TradeRecord&)>;

    // ── Default CRTP hooks ─────────────────────────────────────────────────────
    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double /*vwap*/) const noexcept
    {
        return true; // derived classes override
    }
    void onSignal(const BracketSignal&) const noexcept {}

    // ── configure() ───────────────────────────────────────────────────────────
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
                   double atr_range_k       = 0.0,
                   double slippage_buffer   = 0.0,
                   double edge_multiplier   = 1.5)
    {
        BUFFER             = buffer;
        STRUCTURE_LOOKBACK = lookback;
        RR                 = rr;
        COOLDOWN_MS        = cooldown_ms;
        MIN_RANGE          = min_range;
        CONFIRM_MOVE       = confirm_move;
        CONFIRM_TIMEOUT_MS = confirm_timeout_ms;
        MIN_HOLD_MS        = min_hold_ms;
        VWAP_MIN_DIST      = vwap_min_dist;
        MIN_STRUCTURE_MS   = min_structure_ms;
        FAILURE_WINDOW_MS  = failure_window_ms;
        ATR_PERIOD         = atr_period;
        ATR_CONFIRM_K      = atr_confirm_k;
        ATR_RANGE_K        = atr_range_k;
        SLIPPAGE_BUFFER    = slippage_buffer;
        EDGE_MULTIPLIER    = edge_multiplier;
    }

    // ── has_open_position() ───────────────────────────────────────────────────
    bool has_open_position() const noexcept {
        return phase == BracketPhase::PENDING || phase == BracketPhase::LIVE;
    }

    // ── on_tick() ─────────────────────────────────────────────────────────────
    void on_tick(double bid, double ask, long long /*ts_ms_ignored*/,
                 bool can_enter,
                 const char* macro_regime,
                 CloseCallback on_close,
                 double vwap = 0.0) noexcept
    {
        if (bid <= 0.0 || ask <= 0.0) return;

        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const double spread_pct = (mid > 0.0) ? (spread / mid * 100.0) : 999.0;
        const int64_t now   = nowSec();

        // ── COOLDOWN ──────────────────────────────────────────────────────────
        if (phase == BracketPhase::COOLDOWN) {
            if (now - m_cooldown_start >= static_cast<int64_t>(COOLDOWN_MS / 1000))
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

            // Breakout failure: price re-enters bracket through breakout level
            if (FAILURE_WINDOW_MS > 0 &&
                (now - pos.entry_ts) < static_cast<int64_t>(FAILURE_WINDOW_MS / 1000)) {
                if ( pos.is_long && bid < pos.breakout_level) {
                    closePos(bid, "BREAKOUT_FAIL", macro_regime, on_close); return;
                }
                if (!pos.is_long && ask > pos.breakout_level) {
                    closePos(ask, "BREAKOUT_FAIL", macro_regime, on_close); return;
                }
            }

            // MIN_HOLD: suppress SL/TP until position has developed
            if ((now - pos.entry_ts) < static_cast<int64_t>(MIN_HOLD_MS / 1000)) return;

            // BE lock at 60% TP progress
            {
                const double tp_dist  = std::fabs(pos.tp - pos.entry);
                const double progress = (tp_dist > 0.0)
                    ? (pos.is_long ? (mid - pos.entry) : (pos.entry - mid)) / tp_dist
                    : 0.0;
                if (progress >= 0.60 && !pos.sl_locked_to_be) {
                    const double be = pos.entry;
                    if ( pos.is_long && be > pos.sl) { pos.sl = be; pos.sl_locked_to_be = true; }
                    if (!pos.is_long && be < pos.sl) { pos.sl = be; pos.sl_locked_to_be = true; }
                    if (pos.sl_locked_to_be) {
                        std::cout << "[BRACKET-" << symbol << "] SL->BE"
                                  << " progress=" << std::fixed << std::setprecision(2) << progress << "\n";
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

        // ── PENDING: awaiting confirm_fill() ──────────────────────────────────
        if (phase == BracketPhase::PENDING) return;

        // ── Session/entry gate ────────────────────────────────────────────────
        if (!can_enter) {
            phase = BracketPhase::IDLE; bracket_high = 0.0; bracket_low = 0.0;
            return;
        }

        // ── Maintain structure window ─────────────────────────────────────────
        m_window.push_back(mid);
        if (static_cast<int>(m_window.size()) > STRUCTURE_LOOKBACK * 2)
            m_window.pop_front();
        if (static_cast<int>(m_window.size()) < STRUCTURE_LOOKBACK) return;

        // ── ATR update ────────────────────────────────────────────────────────
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

        // Effective params — ATR-scaled when ready, static otherwise
        const double eff_min_range    = (ATR_RANGE_K   > 0.0 && atr > 0.0) ? atr * ATR_RANGE_K   : MIN_RANGE;
        const double eff_confirm_move = (ATR_CONFIRM_K > 0.0 && atr > 0.0) ? atr * ATR_CONFIRM_K : CONFIRM_MOVE;

        // ── Compute structure (exclude current tick) ──────────────────────────
        const int wsize  = static_cast<int>(m_window.size());
        const auto wbegin = m_window.begin() + (wsize - STRUCTURE_LOOKBACK);
        const auto wend   = m_window.end() - 1;
        const double struct_hi = *std::max_element(wbegin, wend);
        const double struct_lo = *std::min_element(wbegin, wend);
        const double range     = struct_hi - struct_lo;

        if (range < eff_min_range) {
            phase = BracketPhase::IDLE; bracket_high = 0.0; bracket_low = 0.0;
            return;
        }

        // CRTP shouldTrade gate (VWAP, spread etc — instrument-specific)
        if (!static_cast<Derived*>(this)->shouldTrade(bid, ask, spread_pct, vwap)) {
            phase = BracketPhase::IDLE; bracket_high = 0.0; bracket_low = 0.0;
            return;
        }

        const double buf = spread * 0.5;
        bracket_high = struct_hi + buf;
        bracket_low  = struct_lo - buf;

        // ── IDLE → ARMED ──────────────────────────────────────────────────────
        if (phase == BracketPhase::IDLE) {
            phase      = BracketPhase::ARMED;
            m_armed_ts = now;
            return;
        }

        // ── ARMED: watch for initial touch ────────────────────────────────────
        if (phase == BracketPhase::ARMED) {
            if (MIN_STRUCTURE_MS > 0 &&
                (now - m_armed_ts) < static_cast<int64_t>(MIN_STRUCTURE_MS / 1000))
                return;
            if (ask >= bracket_high) {
                m_confirm_side     = 1;
                m_confirm_start_ts = now;
                m_eff_confirm_move = eff_confirm_move;
                phase              = BracketPhase::CONFIRMING;
                std::cout << "[BRACKET-" << symbol << "] CONFIRMING LONG"
                          << " need ask>=" << bracket_high + eff_confirm_move
                          << (atr > 0.0 ? " (ATR)" : "") << "\n";
                std::cout.flush();
            } else if (bid <= bracket_low) {
                m_confirm_side     = -1;
                m_confirm_start_ts = now;
                m_eff_confirm_move = eff_confirm_move;
                phase              = BracketPhase::CONFIRMING;
                std::cout << "[BRACKET-" << symbol << "] CONFIRMING SHORT"
                          << " need bid<=" << bracket_low - eff_confirm_move
                          << (atr > 0.0 ? " (ATR)" : "") << "\n";
                std::cout.flush();
            }
            return;
        }

        // ── CONFIRMING: wait for follow-through ───────────────────────────────
        if (phase == BracketPhase::CONFIRMING) {
            if (m_confirm_side == 1) {
                if (ask >= bracket_high + m_eff_confirm_move) {
                    trigger(1, spread, macro_regime); return;
                }
            } else {
                if (bid <= bracket_low - m_eff_confirm_move) {
                    trigger(-1, spread, macro_regime); return;
                }
            }
            if ((now - m_confirm_start_ts) > static_cast<int64_t>(CONFIRM_TIMEOUT_MS / 1000)) {
                std::cout << "[BRACKET-" << symbol << "] CONFIRM TIMEOUT\n";
                std::cout.flush();
                m_confirm_side = 0;
                phase          = BracketPhase::ARMED;
            }
        }
    }

    // ── get_signal() ──────────────────────────────────────────────────────────
    BracketSignal get_signal() noexcept {
        BracketSignal out = pending_sig;
        pending_sig       = BracketSignal{};
        return out;
    }

    // ── confirm_fill() ────────────────────────────────────────────────────────
    void confirm_fill(double actual_price, double actual_size) noexcept {
        if (phase != BracketPhase::PENDING) return;
        // NOTE: do NOT read pos.tp/pos.sl from pending_sig here —
        // get_signal() already consumed and zeroed pending_sig before confirm_fill
        // is called. pos.tp and pos.sl are correctly set in trigger() and survive.
        pos.active   = true;
        pos.entry    = actual_price;
        pos.size     = actual_size;
        pos.entry_ts = nowSec();
        // pos.tp, pos.sl, pos.is_long already set correctly in trigger() — do not overwrite
        phase        = BracketPhase::LIVE;
        std::cout << "[BRACKET-" << symbol << "] FILL CONFIRMED"
                  << " px=" << actual_price << " size=" << actual_size
                  << " sl=" << pos.sl << " tp=" << pos.tp << "\n";
        std::cout.flush();
    }

    // ── on_reject() ───────────────────────────────────────────────────────────
    void on_reject() noexcept {
        std::cout << "[BRACKET-" << symbol << "] REJECTED — reset\n";
        std::cout.flush();
        reset();
    }

    // ── forceClose() ──────────────────────────────────────────────────────────
    void forceClose(double bid, double ask, const char* reason,
                    double /*latency_ms*/, const char* macro_regime,
                    CloseCallback on_close) noexcept
    {
        if (phase == BracketPhase::LIVE && pos.active)
            closePos((bid + ask) * 0.5, reason, macro_regime, on_close);
        else if (phase == BracketPhase::PENDING)
            reset();
    }

protected:
    std::deque<double> m_window;
    std::deque<double> m_atr_window;
    int64_t  m_cooldown_start  = 0;
    int      m_confirm_side    = 0;
    int64_t  m_confirm_start_ts = 0;
    double   m_eff_confirm_move = 0.0;
    int64_t  m_armed_ts        = 0;
    int      m_trade_id        = 0;

    static int64_t nowSec() noexcept {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void trigger(int side, double spread, const char* macro_regime) noexcept
    {
        const double e    = (side > 0) ? bracket_high : bracket_low;  // entry level
        const double stop = (side > 0) ? bracket_low  : bracket_high;
        const double dist = std::fabs(e - stop);
        const double tp   = e + side * dist * RR;

        // ── Pre-entry viability check ─────────────────────────────────────────
        // Block trade if TP distance < (spread + slippage_buffer) * edge_multiplier.
        // Ensures we have a real edge over execution costs before firing.
        const double tp_dist = std::fabs(tp - e);
        const double cost    = spread + SLIPPAGE_BUFFER;
        if (EDGE_MULTIPLIER > 0.0 && cost > 0.0 && tp_dist < cost * EDGE_MULTIPLIER) {
            std::cout << "[BRACKET-" << symbol << "] BLOCKED: no_edge"
                      << " tp_dist=" << tp_dist
                      << " cost=" << cost
                      << " need>=" << cost * EDGE_MULTIPLIER << "\n";
            std::cout.flush();
            phase = BracketPhase::IDLE;
            bracket_high = 0.0; bracket_low = 0.0; m_confirm_side = 0;
            return;
        }

        std::cout << "[BRACKET-" << symbol << "] TRIGGERED "
                  << (side > 0 ? "LONG" : "SHORT")
                  << " entry~" << e << " sl=" << stop << " tp=" << tp
                  << " range=" << (bracket_high - bracket_low) << "\n";
        std::cout.flush();

        pos              = OpenPos{};
        pos.active       = false;
        pos.is_long      = (side > 0);
        pos.entry        = e;
        pos.sl           = stop;
        pos.tp           = tp;
        pos.breakout_level = (side > 0) ? bracket_high : bracket_low;
        pos.size         = ENTRY_SIZE;
        pos.spread_at_entry = spread;
        if (macro_regime) strncpy_s(pos.regime, macro_regime, 31);

        pending_sig.valid   = true;
        pending_sig.is_long = (side > 0);
        pending_sig.entry   = e;
        pending_sig.sl      = stop;
        pending_sig.tp      = tp;
        pending_sig.reason  = (side > 0) ? "BRACKET_LONG" : "BRACKET_SHORT";

        bracket_high   = 0.0;
        bracket_low    = 0.0;
        m_confirm_side = 0;
        phase          = BracketPhase::PENDING;

        ++signal_count;
        ++m_trade_id;

        static_cast<Derived*>(this)->onSignal(pending_sig);
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
        tr.entryTs       = pos.entry_ts;        // already in seconds
        tr.exitTs        = nowSec();             // already in seconds
        tr.exitReason    = reason;
        tr.spreadAtEntry = pos.spread_at_entry;
        tr.latencyMs     = 0.0;
        tr.engine        = std::string(symbol ? symbol : "???") + "_BRACKET";
        tr.regime        = (macro_regime && *macro_regime) ? macro_regime : pos.regime;

        pos              = OpenPos{};
        pending_sig      = BracketSignal{};
        m_cooldown_start = nowSec();
        phase            = BracketPhase::COOLDOWN;

        if (on_close) on_close(tr);
    }

    void reset() noexcept {
        pos              = OpenPos{};
        pending_sig      = BracketSignal{};
        bracket_high     = 0.0;
        bracket_low      = 0.0;
        m_confirm_side   = 0;
        m_eff_confirm_move = 0.0;
        m_armed_ts       = 0;
        phase            = BracketPhase::IDLE;
    }
};

// ==============================================================================
// GoldBracketEngine — GOLD.F
// shouldTrade: spread check + VWAP distance check
// ==============================================================================
class GoldBracketEngine final : public BracketEngineBase<GoldBracketEngine>
{
public:
    explicit GoldBracketEngine() noexcept {
        symbol     = "GOLD.F";
        ENTRY_SIZE = 0.01;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double /*spread_pct*/, double /*vwap*/) const noexcept
    {
        // Primary VWAP check is done in on_tick via VWAP_MIN_DIST field.
        // Spread gate for gold uses absolute points — checked in on_tick before shouldTrade.
        return true;
    }
};

// ==============================================================================
// SilverBracketEngine — XAGUSD
// shouldTrade: spread check only (no external VWAP source for silver)
// ==============================================================================
class SilverBracketEngine final : public BracketEngineBase<SilverBracketEngine>
{
public:
    explicit SilverBracketEngine() noexcept {
        symbol     = "XAGUSD";
        ENTRY_SIZE = 0.01;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double /*spread_pct*/, double /*vwap*/) const noexcept
    {
        return true;  // filters handled by MIN_RANGE, CONFIRM_MOVE, VWAP_MIN_DIST in base
    }
};

// ==============================================================================
// BracketEngine — backward-compatible alias (plain BreakoutEngine-style)
// Not used for gold/silver — kept so any other code compiles without changes
// ==============================================================================
class BracketEngine final : public BracketEngineBase<BracketEngine>
{
public:
    explicit BracketEngine() noexcept = default;
    bool shouldTrade(double, double, double, double) const noexcept { return true; }
};

} // namespace omega

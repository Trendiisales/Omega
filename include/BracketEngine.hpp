#pragma once
// ==============================================================================
// BracketEngine — Classic breakout bracket for Gold and Silver
//
// State machine (FIXED):
//   IDLE      → not enough data / session closed
//   ARMED     → bracket levels set, waiting for price to cross
//   PENDING   → signal fired, order sent, awaiting broker fill confirmation
//   LIVE      → fill confirmed, managing open position (TP/SL/trail/timeout)
//   COOLDOWN  → position closed, waiting before re-arming
//
// Key fixes vs previous version:
//   ✅ PENDING phase: engine does NOT mark pos.active until confirm_fill() called
//   ✅ Self-inclusion bug fixed: structure computed from window[0..N-2] only
//   ✅ has_open_position() covers both PENDING and LIVE — blocks other engines
//   ✅ confirm_fill(price, size) sets real broker fill — no phantom positions
//   ✅ on_reject() resets cleanly without ghost state
//   ✅ Correct bid/ask execution pricing (long=ask, short=bid)
//   ✅ SL anchored to bracket opposite side (structure-based, not % guess)
//
// Used by: GOLD.F (parallel with GoldEngineStack), XAGUSD
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

// ── Bracket phase ──────────────────────────────────────────────────────────────
enum class BracketPhase : uint8_t {
    IDLE     = 0,   // not enough data / session closed
    ARMED    = 1,   // bracket levels set, waiting for price to cross
    PENDING  = 2,   // signal emitted, order sent — awaiting confirm_fill()
    LIVE     = 3,   // fill confirmed, managing open position
    COOLDOWN = 4,   // post-close cooldown before re-arming
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
    // ── Config — set via apply_*_config before first tick ─────────────────────
    const char* symbol            = "???";

    int    STRUCTURE_LOOKBACK     = 40;
    double TP_PCT                 = 0.25;
    double SL_PCT                 = 0.12;
    double MIN_RANGE_PCT          = 0.04;
    double MAX_SPREAD_PCT         = 0.06;
    int    MIN_GAP_SEC            = 90;
    int    COOLDOWN_AFTER_SL_SEC  = 120;
    int    MAX_HOLD_SEC           = 900;
    double ENTRY_SIZE             = 0.01;
    bool   AGGRESSIVE_SHADOW      = false;

    // ── Observable state ──────────────────────────────────────────────────────
    BracketPhase phase        = BracketPhase::IDLE;
    double       bracket_high = 0.0;
    double       bracket_low  = 0.0;
    int          signal_count = 0;

    struct OpenPos {
        bool    active   = false;
        bool    is_long  = true;
        double  entry    = 0.0;
        double  tp       = 0.0;
        double  sl       = 0.0;
        double  size     = 0.01;
        double  mfe      = 0.0;
        double  mae      = 0.0;
        int64_t entry_ts = 0;
        double  spread_at_entry = 0.0;
        char    regime[32] = {};
    } pos;

    BracketSignal pending_sig;

    using CloseCallback = std::function<void(const TradeRecord&)>;

    // ── has_open_position(): blocks other engines on same symbol ──────────────
    bool has_open_position() const noexcept {
        return phase == BracketPhase::PENDING || phase == BracketPhase::LIVE;
    }

    // ── confirm_fill(): called by execution report on FILL ────────────────────
    void confirm_fill(double actual_price, double actual_size) noexcept {
        if (phase != BracketPhase::PENDING) return;
        pos.active   = true;
        pos.entry    = actual_price;
        pos.size     = actual_size;
        pos.entry_ts = nowSec();
        phase        = BracketPhase::LIVE;
        std::cout << "[BRACKET-" << symbol << "] FILL CONFIRMED"
                  << " price=" << actual_price
                  << " size="  << actual_size << "\n";
        std::cout.flush();
    }

    // ── on_reject(): called by execution report on REJECT ─────────────────────
    void on_reject() noexcept {
        std::cout << "[BRACKET-" << symbol << "] ORDER REJECTED — resetting\n";
        std::cout.flush();
        reset();
    }

    // ── update(): call on every tick ─────────────────────────────────────────
    [[nodiscard]] BracketSignal update(
        double bid, double ask,
        double latency_ms,
        const char* macro_regime,
        CloseCallback on_close,
        bool can_enter = true) noexcept
    {
        if (bid <= 0.0 || ask <= 0.0) return {};

        m_last_bid = bid;
        m_last_ask = ask;

        const double mid        = (bid + ask) * 0.5;
        const double spread     = ask - bid;
        const double spread_pct = (mid > 0.0) ? (spread / mid * 100.0) : 999.0;

        // ── COOLDOWN ──────────────────────────────────────────────────────────
        if (phase == BracketPhase::COOLDOWN) {
            if ((nowSec() - m_cooldown_start) >= static_cast<int64_t>(m_cooldown_sec))
                phase = BracketPhase::IDLE;
            else
                return {};
        }

        // Always maintain window
        m_window.push_back(mid);
        if (static_cast<int>(m_window.size()) > STRUCTURE_LOOKBACK * 2)
            m_window.pop_front();

        // ── LIVE: manage open position ─────────────────────────────────────────
        if (phase == BracketPhase::LIVE) {
            if (!pos.active) return {};

            const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
            if ( move > pos.mfe) pos.mfe =  move;
            if (-move > pos.mae) pos.mae = -move;

            if ( pos.is_long && bid >= pos.tp) { closePos(pos.tp, "TP_HIT",  latency_ms, macro_regime, on_close); return {}; }
            if (!pos.is_long && ask <= pos.tp) { closePos(pos.tp, "TP_HIT",  latency_ms, macro_regime, on_close); return {}; }
            if ( pos.is_long && bid <= pos.sl) { closePos(pos.sl, "SL_HIT",  latency_ms, macro_regime, on_close); return {}; }
            if (!pos.is_long && ask >= pos.sl) { closePos(pos.sl, "SL_HIT",  latency_ms, macro_regime, on_close); return {}; }

            // Trailing stop
            {
                const double move_pct = pos.is_long
                    ? (mid - pos.entry) / pos.entry * 100.0
                    : (pos.entry - mid) / pos.entry * 100.0;
                const double trail1_arm  = SL_PCT * 1.00;
                const double trail2_arm  = SL_PCT * 2.00;
                const double trail1_dist = SL_PCT * 0.40;
                const double trail2_dist = SL_PCT * 0.25;
                const double lock_arm    = SL_PCT * 0.50;
                const double lock_gain   = SL_PCT * 0.10;

                if (move_pct >= trail2_arm) {
                    const double t = pos.is_long
                        ? mid * (1.0 - trail2_dist / 100.0)
                        : mid * (1.0 + trail2_dist / 100.0);
                    if ( pos.is_long && t > pos.sl) pos.sl = t;
                    if (!pos.is_long && t < pos.sl) pos.sl = t;
                } else if (move_pct >= trail1_arm) {
                    const double t = pos.is_long
                        ? mid * (1.0 - trail1_dist / 100.0)
                        : mid * (1.0 + trail1_dist / 100.0);
                    if ( pos.is_long && t > pos.sl) pos.sl = t;
                    if (!pos.is_long && t < pos.sl) pos.sl = t;
                } else if (move_pct >= lock_arm) {
                    const double be = pos.is_long
                        ? pos.entry * (1.0 + lock_gain / 100.0)
                        : pos.entry * (1.0 - lock_gain / 100.0);
                    if ( pos.is_long && be > pos.sl) pos.sl = be;
                    if (!pos.is_long && be < pos.sl) pos.sl = be;
                }
            }

            // Timeout
            if (nowSec() - pos.entry_ts >= static_cast<int64_t>(MAX_HOLD_SEC)) {
                double exit_px = mid;
                const bool sl_breached = pos.is_long ? (mid < pos.sl) : (mid > pos.sl);
                if (sl_breached) exit_px = pos.sl;
                closePos(exit_px, "TIMEOUT", latency_ms, macro_regime, on_close);
                return {};
            }
            return {};
        }

        // ── PENDING: order in flight, waiting for confirm_fill() ──────────────
        if (phase == BracketPhase::PENDING) return {};

        // ── Session gate (only for IDLE/ARMED) ────────────────────────────────
        if (!can_enter) {
            phase        = BracketPhase::IDLE;
            bracket_high = 0.0;
            bracket_low  = 0.0;
            return {};
        }

        if (static_cast<int>(m_window.size()) < STRUCTURE_LOOKBACK) return {};

        // ── FIXED: compute structure excluding current tick ────────────────────
        const int wsize = static_cast<int>(m_window.size());
        const auto begin = m_window.begin() + (wsize - STRUCTURE_LOOKBACK);
        const auto end_excl = m_window.end() - 1;  // exclude current tick
        const double struct_hi = *std::max_element(begin, end_excl);
        const double struct_lo = *std::min_element(begin, end_excl);
        const double range_pct = (mid > 0.0) ? ((struct_hi - struct_lo) / mid * 100.0) : 0.0;

        const double min_range = AGGRESSIVE_SHADOW ? (MIN_RANGE_PCT * 0.7) : MIN_RANGE_PCT;
        if (range_pct < min_range) {
            phase        = BracketPhase::IDLE;
            bracket_high = 0.0;
            bracket_low  = 0.0;
            return {};
        }

        const double buf = spread * 0.5;
        bracket_high = struct_hi + buf;
        bracket_low  = struct_lo - buf;
        phase        = BracketPhase::ARMED;

        if ((nowSec() - m_last_signal_ts) < static_cast<int64_t>(m_cooldown_sec))
            return {};

        if (spread_pct > MAX_SPREAD_PCT) {
            std::cout << "[BRACKET-" << symbol << "] spread too wide: "
                      << spread_pct << "% > " << MAX_SPREAD_PCT << "%\n";
            return {};
        }

        // ── Trigger ───────────────────────────────────────────────────────────
        const bool long_trigger  = (ask >= bracket_high);
        const bool short_trigger = (bid <= bracket_low);
        if (!long_trigger && !short_trigger) return {};

        const bool is_long = long_trigger && (!short_trigger ||
            (ask - bracket_high) >= (bracket_low - bid));

        const double entry = is_long ? ask : bid;
        const double tp    = is_long ? entry * (1.0 + TP_PCT / 100.0)
                                     : entry * (1.0 - TP_PCT / 100.0);
        const double sl    = is_long ? (struct_lo - buf)
                                     : (struct_hi + buf);

        std::cout << "[BRACKET-" << symbol << "] TRIGGERED "
                  << (is_long ? "LONG" : "SHORT")
                  << " entry=" << entry
                  << " tp=" << tp << " sl=" << sl
                  << " hi=" << bracket_high << " lo=" << bracket_low
                  << " range=" << range_pct << "%\n";
        std::cout.flush();

        // ── Set PENDING (pos.active = false until confirm_fill) ───────────────
        pos              = OpenPos{};
        pos.active       = false;
        pos.is_long      = is_long;
        pos.entry        = entry;
        pos.tp           = tp;
        pos.sl           = sl;
        pos.size         = ENTRY_SIZE;
        pos.spread_at_entry = spread;
        strncpy_s(pos.regime, macro_regime ? macro_regime : "", 31);

        phase        = BracketPhase::PENDING;
        bracket_high = 0.0;
        bracket_low  = 0.0;

        m_last_signal_ts = nowSec();
        m_cooldown_sec   = MIN_GAP_SEC;
        ++signal_count;
        ++m_trade_id;

        pending_sig.valid   = true;
        pending_sig.is_long = is_long;
        pending_sig.entry   = entry;
        pending_sig.tp      = tp;
        pending_sig.sl      = sl;
        pending_sig.reason  = is_long ? "BRACKET_LONG" : "BRACKET_SHORT";

        return pending_sig;
    }

    void forceClose(double bid, double ask, const char* reason,
                    double latency_ms, const char* macro_regime,
                    CloseCallback on_close) noexcept
    {
        if (phase == BracketPhase::LIVE && pos.active)
            closePos((bid + ask) * 0.5, reason, latency_ms, macro_regime, on_close);
        else if (phase == BracketPhase::PENDING)
            reset();
    }

private:
    std::deque<double> m_window;
    int64_t m_last_signal_ts = 0;
    int64_t m_cooldown_sec   = 0;
    int64_t m_cooldown_start = 0;
    int     m_trade_id       = 0;
    double  m_last_bid       = 0.0;
    double  m_last_ask       = 0.0;

    static int64_t nowSec() noexcept {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void closePos(double exit_px, const char* reason,
                  double latency_ms, const char* macro_regime,
                  CloseCallback& on_close) noexcept
    {
        if (!pos.active) return;

        m_cooldown_sec   = (std::strcmp(reason, "SL_HIT") == 0)
                               ? COOLDOWN_AFTER_SL_SEC
                               : MIN_GAP_SEC;
        m_cooldown_start = nowSec();

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
        tr.exitTs        = nowSec();
        tr.exitReason    = reason;
        tr.spreadAtEntry = pos.spread_at_entry;
        tr.latencyMs     = latency_ms;
        tr.engine        = std::string(symbol ? symbol : "???") + "_BRACKET";
        tr.regime        = (macro_regime && *macro_regime) ? macro_regime : pos.regime;

        pos         = OpenPos{};
        pending_sig = BracketSignal{};
        phase       = BracketPhase::COOLDOWN;

        if (on_close) on_close(tr);
    }

    void reset() noexcept {
        pos          = OpenPos{};
        pending_sig  = BracketSignal{};
        bracket_high = 0.0;
        bracket_low  = 0.0;
        phase        = BracketPhase::IDLE;
    }
};

} // namespace omega

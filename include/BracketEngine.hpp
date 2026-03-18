#pragma once
// ==============================================================================
// BracketEngine — Classic breakout bracket for Gold and Silver
//
// Logic (exactly as specified):
//   1. Identify a recent high/low structure over a lookback window
//   2. Place a virtual buy stop above the high
//   3. Place a virtual sell stop below the low
//   4. Attach SL and TP to each side immediately
//   5. Wait for price to cross either stop level
//   6. Once one side triggers → enter, cancel the other side
//
// Key differences from BreakoutEngine (compression/vol approach):
//   - Structure is price-based (hi/lo of N ticks), not volatility ratio
//   - Both sides are "armed" simultaneously — no FLAT→COMPRESSION→WATCH FSM
//   - No momentum gate, no vol gate — price doing the work
//   - Min range filter: bracket only arms if hi-lo spread is meaningful
//   - Cooldown after a fill: prevents immediate re-arming after stop hit
//   - Session-aware: caller gates this via can_enter
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

// ── Bracket state ──────────────────────────────────────────────────────────────
enum class BracketPhase : uint8_t {
    IDLE    = 0,   // not enough data yet
    ARMED   = 1,   // both stops set, waiting for trigger
    IN_TRADE = 2,  // one side triggered, position live
};

struct BracketSignal {
    bool   valid   = false;
    bool   is_long = true;
    double entry   = 0.0;
    double tp      = 0.0;
    double sl      = 0.0;
    const char* reason = "";
};

class BracketEngine {
public:
    // ── Config — set via apply_*_config before first tick ─────────────────────
    const char* symbol            = "???";

    int    STRUCTURE_LOOKBACK     = 40;    // ticks to define the recent high/low
    double TP_PCT                 = 0.25;  // TP as % of entry price
    double SL_PCT                 = 0.12;  // SL as % of entry price (inside the range)
    double MIN_RANGE_PCT          = 0.04;  // bracket only arms if hi-lo >= this % of mid
                                           // prevents arming in flat/dead tape
    double MAX_SPREAD_PCT         = 0.06;  // max spread at trigger (% of mid)
    int    MIN_GAP_SEC            = 90;    // min seconds between any two entries
    int    COOLDOWN_AFTER_SL_SEC  = 120;   // extra cooldown after an SL hit
    int    MAX_HOLD_SEC           = 900;   // position timeout
    double ENTRY_SIZE             = 0.01;  // lot size
    bool   AGGRESSIVE_SHADOW      = false;

    // ── Observable state (read by telemetry) ──────────────────────────────────
    BracketPhase phase         = BracketPhase::IDLE;
    double       bracket_high  = 0.0;  // armed buy stop level
    double       bracket_low   = 0.0;  // armed sell stop level
    int          signal_count  = 0;

    // Open position — mirrors BreakoutEngine OpenPos layout
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

    using CloseCallback = std::function<void(const TradeRecord&)>;

    // ── update() — call on every tick ─────────────────────────────────────────
    [[nodiscard]] BracketSignal update(
        double bid, double ask,
        double latency_ms,
        const char* macro_regime,
        CloseCallback on_close,
        bool can_enter = true) noexcept
    {
        if (bid <= 0.0 || ask <= 0.0) return {};

        const double mid        = (bid + ask) * 0.5;
        const double spread     = ask - bid;
        const double spread_pct = (mid > 0.0) ? (spread / mid * 100.0) : 999.0;

        // Always maintain the structure window
        m_window.push_back(mid);
        if (static_cast<int>(m_window.size()) > STRUCTURE_LOOKBACK * 2)
            m_window.pop_front();

        // ── Manage open position ───────────────────────────────────────────────
        if (pos.active) {
            const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
            if (move  > pos.mfe) pos.mfe =  move;
            if (-move > pos.mae) pos.mae = -move;

            // TP/SL — use aggressive fill side
            if ( pos.is_long && bid >= pos.tp) { closePos(pos.tp, "TP_HIT",  latency_ms, macro_regime, on_close); return {}; }
            if (!pos.is_long && ask <= pos.tp) { closePos(pos.tp, "TP_HIT",  latency_ms, macro_regime, on_close); return {}; }
            if ( pos.is_long && bid <= pos.sl) { closePos(pos.sl, "SL_HIT",  latency_ms, macro_regime, on_close); return {}; }
            if (!pos.is_long && ask >= pos.sl) { closePos(pos.sl, "SL_HIT",  latency_ms, macro_regime, on_close); return {}; }

            // Trailing stop — same SL-relative scaling as BreakoutEngine
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

        // ── Not in trade — manage bracket ─────────────────────────────────────
        if (!can_enter) {
            phase = BracketPhase::IDLE;
            return {};
        }

        // Need enough ticks to define structure
        if (static_cast<int>(m_window.size()) < STRUCTURE_LOOKBACK) return {};

        // Recompute bracket from the most recent STRUCTURE_LOOKBACK ticks every tick.
        // This keeps the structure current — stale structure after a big move
        // would leave stops far from current price and miss the real breakout.
        const auto begin = m_window.end() - STRUCTURE_LOOKBACK;
        const auto end   = m_window.end();
        const double struct_hi = *std::max_element(begin, end);
        const double struct_lo = *std::min_element(begin, end);
        const double range_pct = (mid > 0.0) ? ((struct_hi - struct_lo) / mid * 100.0) : 0.0;

        // Only arm if range is meaningful — avoids bracketing dead tape
        const double min_range = AGGRESSIVE_SHADOW ? (MIN_RANGE_PCT * 0.7) : MIN_RANGE_PCT;
        if (range_pct < min_range) {
            phase         = BracketPhase::IDLE;
            bracket_high  = 0.0;
            bracket_low   = 0.0;
            return {};
        }

        // Set armed levels: buy stop just above hi, sell stop just below lo
        // Use a small buffer (0.5x spread) so we don't trigger on the range extreme itself
        const double buf     = spread * 0.5;
        bracket_high = struct_hi + buf;  // buy stop trigger
        bracket_low  = struct_lo - buf;  // sell stop trigger
        phase        = BracketPhase::ARMED;

        // Check cooldown from last trade
        const int64_t now = nowSec();
        if (now - m_last_signal_ts < static_cast<int64_t>(m_cooldown_sec)) {
            // Still cooling down — bracket is visible but won't fire
            return {};
        }

        // Spread check
        if (spread_pct > MAX_SPREAD_PCT) {
            std::cout << "[BRACKET-" << symbol << "] spread too wide: "
                      << spread_pct << "% > " << MAX_SPREAD_PCT << "%\n";
            return {};
        }

        // ── Trigger check: has price crossed either stop? ─────────────────────
        // Buy stop: ask crosses above bracket_high (buy at ask — we're paying up)
        // Sell stop: bid crosses below bracket_low (sell at bid — we're selling down)
        const bool long_trigger  = (ask >= bracket_high);
        const bool short_trigger = (bid <= bracket_low);

        if (!long_trigger && !short_trigger) return {};  // bracket armed but not triggered

        // Prefer the stronger trigger if both fire simultaneously (shouldn't happen
        // in normal markets but can on gap opens)
        const bool is_long = long_trigger && (!short_trigger ||
            (ask - bracket_high) >= (bracket_low - bid));

        // Entry price = the stop level itself (fill at the breakout point, not mid)
        const double entry = is_long ? bracket_high : bracket_low;
        const double tp    = is_long ? entry * (1.0 + TP_PCT / 100.0)
                                     : entry * (1.0 - TP_PCT / 100.0);
        // SL sits inside the range — below struct_hi for longs, above struct_lo for shorts
        // This gives the trade room to develop without a fixed % SL being too tight
        const double sl    = is_long ? (struct_lo - buf)   // below the recent low
                                     : (struct_hi + buf);  // above the recent high

        std::cout << "[BRACKET-" << symbol << "] TRIGGERED "
                  << (is_long ? "LONG" : "SHORT")
                  << " entry=" << entry
                  << " tp=" << tp << " sl=" << sl
                  << " hi=" << bracket_high << " lo=" << bracket_low
                  << " range=" << range_pct << "%\n";
        std::cout.flush();

        // ── Enter position — opposite side automatically cancelled ────────────
        pos.active          = true;
        pos.is_long         = is_long;
        pos.entry           = entry;
        pos.tp              = tp;
        pos.sl              = sl;
        pos.size            = ENTRY_SIZE;
        pos.mfe             = 0.0;
        pos.mae             = 0.0;
        pos.entry_ts        = now;
        pos.spread_at_entry = spread;
        strncpy_s(pos.regime, macro_regime ? macro_regime : "", 31);

        // Reset bracket — opposite side is now implicitly cancelled
        phase        = BracketPhase::IN_TRADE;
        bracket_high = 0.0;
        bracket_low  = 0.0;

        m_last_signal_ts = now;
        m_cooldown_sec   = MIN_GAP_SEC;  // reset to standard cooldown
        ++signal_count;
        ++m_trade_id;

        BracketSignal sig;
        sig.valid   = true;
        sig.is_long = is_long;
        sig.entry   = entry;
        sig.tp      = tp;
        sig.sl      = sl;
        sig.reason  = is_long ? "BRACKET_LONG" : "BRACKET_SHORT";
        return sig;
    }

    void forceClose(double bid, double ask, const char* reason,
                    double latency_ms, const char* macro_regime,
                    CloseCallback on_close) noexcept
    {
        if (!pos.active) return;
        closePos((bid + ask) * 0.5, reason, latency_ms, macro_regime, on_close);
    }

private:
    std::deque<double> m_window;
    int64_t m_last_signal_ts = 0;
    int64_t m_cooldown_sec   = 0;
    int     m_trade_id       = 0;

    static int64_t nowSec() noexcept {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void closePos(double exit_px, const char* reason,
                  double latency_ms, const char* macro_regime,
                  CloseCallback& on_close) noexcept
    {
        if (!pos.active) return;

        // Extra cooldown after SL — avoid re-entering the same broken structure
        if (std::strcmp(reason, "SL_HIT") == 0)
            m_cooldown_sec = COOLDOWN_AFTER_SL_SEC;
        else
            m_cooldown_sec = MIN_GAP_SEC;

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

        pos.active = false;
        pos        = OpenPos{};
        phase      = BracketPhase::IDLE;

        if (on_close) on_close(tr);
    }
};

} // namespace omega

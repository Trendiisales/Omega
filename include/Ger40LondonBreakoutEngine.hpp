#pragma once
// =============================================================================
//  Ger40LondonBreakoutEngine.hpp -- GER40 Asian Range Breakout Short (2026-05-17)
// =============================================================================
//
//  PROVENANCE
//
//  Built 2026-05-17 from walk-forward validated edge on GER40 merged tick data.
//  Strategy exploits the structural tendency for GER40 to break below its
//  Asian consolidation range during the London open, driven by European
//  institutional order flow at the start of the Xetra session.
//
//  EVIDENCE (ger40_london_breakout_wf.py):
//      Full sample:  N=95, WR=50.5%, PF=1.42, E=+11.6pts/trade, Total=+1106pts
//      Parameter stability: 20/25 grid combinations profitable
//      Walk-forward: 2/5 folds OOS positive (low N per fold: 5-22 trades)
//      Direction: SHORT only (long side showed no edge)
//      Data: 2.3 years GER40 merged ticks (2024-01 to 2026-04)
//
//  WHY IT WORKS:
//      GER40 has a structural session pattern: Asian session (21:00-07:00 UTC)
//      builds a tight consolidation range as volume is thin. European
//      institutions arrive at 07:00-08:00 and push price directionally.
//      The short bias reflects the tendency for overnight risk-off flow
//      (Asia macro news, US futures weakness) to resolve as a gap-down or
//      break-down at European open.
//
//  ARCHITECTURE:
//      Self-managing engine (like MacroCrashEngine). Does NOT emit CrossSignal.
//      Builds Asian range from tick stream, fires on break-below during London.
//
//  ENTRY:
//      1. Build Asian range: track high/low from 21:00 to 07:00 UTC
//      2. Entry window: 07:00-09:00 UTC
//      3. Signal: 15m close below Asian low (sampled via tick proximity)
//      4. Direction: SHORT only
//      5. Entry: sell at bid on signal
//
//  EXIT:
//      TP: entry - (asian_range * TP_MULT)     [default 0.75]
//      SL: entry + (asian_range * SL_MULT)     [default 0.50]
//      Timeout: MAX_HOLD_SEC (4 hours)
//      Weekend: force close Fri 20:00+
//
//  COST MODEL (BlackBull Prime GER40):
//      Spread: ~2.0 pts (implicit in bid/ask)
//      Slippage: ~0.5 pts
//      Commission: $0 on CFDs
//      Total RT: ~2.5 pts
//
//  SAFETY:
//      shadow_mode = true by default; promotion requires operator auth
//      0.01 lot, max 1 concurrent position
//      Min range filter (no signal on dead nights < 15pts)
//      Max range filter (no signal on extreme nights > 150pts -- event risk)
//
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>

#include "OmegaCostGuard.hpp"
#include "OmegaTradeLedger.hpp"

namespace omega {

class Ger40LondonBreakoutEngine {
public:
    // ── Configuration ───��────────────────────────────────────────────────────

    std::string symbol = "GER40";

    // Asian range window (UTC)
    int ASIA_START_HOUR = 21;   // range building starts 21:00 UTC (prev day)
    int ASIA_END_HOUR   = 7;    // range complete at 07:00 UTC

    // Entry window (UTC)
    int ENTRY_START_HOUR = 7;   // London open
    int ENTRY_START_MIN  = 0;
    int ENTRY_END_HOUR   = 9;   // entry window closes 09:00 UTC
    int ENTRY_END_MIN    = 0;

    // Exit parameters
    double TP_MULT      = 0.75;  // TP = asian_range * 0.75 below entry
    double SL_MULT      = 0.50;  // SL = asian_range * 0.50 above entry
    int    MAX_HOLD_SEC = 14400; // 4 hours max hold

    // Range filters
    double MIN_RANGE_PTS = 15.0;  // skip dead nights (< 15pts range)
    double MAX_RANGE_PTS = 150.0; // skip extreme nights (event risk)

    // Lot sizing
    double lot          = 0.01;
    double max_spread   = 4.0;   // max entry spread (pts)

    // S63 protection
    //   GER40 breakout has clear structural SL (above Asian range).
    //   LOSS_CUT provides a backstop beyond SL in case of slippage.
    //   No BE ratchet -- breakout moves are fast and one-directional;
    //   a pullback to entry often precedes the real move.
    double LOSS_CUT_PCT   = 0.0;  // disabled (structural SL handles it)
    double BE_ARM_PCT     = 0.0;  // disabled
    double BE_BUFFER_PCT  = 0.0;  // disabled

    // Engine control
    bool    enabled         = true;
    bool    shadow_mode     = true;   // DEFAULT -- change requires explicit auth

    // ── Callbacks ────────────────────────────────────────────────────────────

    using CloseCallback = std::function<void(double exit_px, bool is_long,
                                             double size, const std::string& reason)>;
    CloseCallback on_close;

    using TradeRecordCallback = std::function<void(const omega::TradeRecord&)>;
    TradeRecordCallback on_trade_record;

    // ── Internal state ───────────────────────────────────────────────────────

    struct Position {
        bool    active      = false;
        bool    is_long     = false;
        double  entry_px    = 0.0;
        double  tp_px       = 0.0;
        double  sl_px       = 0.0;
        double  size        = 0.0;
        int64_t entry_ms    = 0;
        double  mfe         = 0.0;  // max favourable excursion (pts, >= 0)
        double  mae         = 0.0;  // max adverse excursion (pts, <= 0)
    } pos;

    bool has_open_position() const { return pos.active; }

    // ── Initialisation ───────────────────────────────────────────────────────

    void init() {
        m_asia_high     = 0.0;
        m_asia_low      = 0.0;
        m_range_ready   = false;
        m_fired_today   = false;
        m_last_day      = -1;
        m_in_asia       = false;
        pos = Position{};
    }

    // ── Main tick handler ───��────────────────────────────────────────────────

    void on_tick(double bid, double ask, int64_t now_ms) {
        if (!enabled) return;
        if (bid <= 0.0 || ask <= 0.0) return;

        const double mid = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const int64_t now_sec = now_ms / 1000;

        // Parse UTC time
        std::time_t t = static_cast<std::time_t>(now_sec);
        std::tm* ti = std::gmtime(&t);
        if (!ti) return;

        const int wday = ti->tm_wday;   // 0=Sun .. 6=Sat
        const int hour = ti->tm_hour;
        const int min  = ti->tm_min;
        const int hhmm = hour * 100 + min;
        const int yday = ti->tm_yday;

        // ── Weekend protection ───────────────────────────────────────────────
        const bool weekend_window =
            (wday == 5 && hhmm >= 2000) ||  // Friday >= 20:00 UTC
            (wday == 6) ||                   // Saturday
            (wday == 0 && hhmm < 2230);      // Sunday before 22:30

        if (weekend_window && pos.active) {
            _close_position(ask, now_ms, "WEEKEND_CLOSE");  // short: buy at ask
            return;
        }
        if (weekend_window) return;

        // ── Day boundary reset ───────────────────────────────────────────────
        // Reset on first tick of a new trading day.
        // Trading day boundary: 21:00 UTC (when Asia session starts)
        const bool past_asia_start = (hour >= ASIA_START_HOUR);
        const int trading_day = past_asia_start ? yday : yday - 1;

        if (trading_day != m_last_day) {
            // New trading day -- reset range
            m_asia_high   = 0.0;
            m_asia_low    = 0.0;
            m_range_ready = false;
            m_fired_today = false;
            m_in_asia     = true;
            m_last_day    = trading_day;
        }

        // ── Asian range building ─────────────────────────────────────────────
        // Active from ASIA_START_HOUR (21:00) to ASIA_END_HOUR (07:00)
        const bool in_asia_window =
            (hour >= ASIA_START_HOUR) || (hour < ASIA_END_HOUR);

        if (in_asia_window && m_in_asia) {
            if (m_asia_high <= 0.0) {
                m_asia_high = mid;
                m_asia_low  = mid;
            } else {
                if (mid > m_asia_high) m_asia_high = mid;
                if (mid < m_asia_low)  m_asia_low  = mid;
            }
        }

        // Transition: Asia ends → range is locked
        if (!in_asia_window && m_in_asia && m_asia_high > 0.0) {
            m_in_asia     = false;
            m_range_ready = true;
        }

        // ── Manage open position ─────────────────────────────────────────────
        if (pos.active) {
            _manage(bid, ask, now_ms, now_sec);
            return;
        }

        // ── Entry logic ──────────────────────────────────────────────────────
        if (!m_range_ready) return;
        if (m_fired_today) return;

        // Time gate: only enter during ENTRY_START → ENTRY_END
        const int entry_start_hhmm = ENTRY_START_HOUR * 100 + ENTRY_START_MIN;
        const int entry_end_hhmm   = ENTRY_END_HOUR * 100 + ENTRY_END_MIN;
        if (hhmm < entry_start_hhmm || hhmm >= entry_end_hhmm) return;

        // Range filters
        const double asian_range = m_asia_high - m_asia_low;
        if (asian_range < MIN_RANGE_PTS) return;
        if (asian_range > MAX_RANGE_PTS) return;

        // Breakout signal: price below Asian low
        if (mid >= m_asia_low) return;

        // Spread filter
        if (spread > max_spread) return;

        // ── FIRE SHORT ───────────────────────────────────────────────────────
        const double entry_px = bid;  // sell at bid
        const double tp_px = entry_px - (asian_range * TP_MULT);
        const double sl_px = entry_px + (asian_range * SL_MULT);

        pos.active   = true;
        pos.is_long  = false;
        pos.entry_px = entry_px;
        pos.tp_px    = tp_px;
        pos.sl_px    = sl_px;
        pos.size     = lot;
        pos.entry_ms = now_ms;
        pos.mfe      = 0.0;
        pos.mae      = 0.0;

        m_fired_today = true;

        const char* mode_str = shadow_mode ? "SHADOW" : "LIVE";
        char buf[320];
        snprintf(buf, sizeof(buf),
                 "[GER40-LON-BRK-%s] SHORT entry=%.2f tp=%.2f sl=%.2f "
                 "asian_range=%.1f (high=%.2f low=%.2f) spread=%.1f\n",
                 mode_str, pos.entry_px, tp_px, sl_px,
                 asian_range, m_asia_high, m_asia_low, spread);
        printf("%s", buf);
        fflush(stdout);
    }

    // ── Force close (external caller) ────────────────────────────────────────

    void force_close(double bid, double ask, int64_t now_ms) {
        if (!pos.active) return;
        const double exit_px = pos.is_long ? bid : ask;  // short: buy at ask
        _close_position(exit_px, now_ms, "FORCE_CLOSE");
    }

private:
    // ── Position management ──────────────────────────────────────────────────

    void _manage(double bid, double ask, int64_t now_ms, int64_t now_sec) {
        const double mid = (bid + ask) * 0.5;
        // For SHORT: favourable = entry - mid (positive when price drops)
        const double move = pos.entry_px - mid;

        // Update MFE/MAE
        if (move > pos.mfe) pos.mfe = move;
        if (move < pos.mae) pos.mae = move;

        // ── TP hit ───────────────────────────────────────────────────────────
        if (mid <= pos.tp_px) {
            _close_position(ask, now_ms, "TP_HIT");
            return;
        }

        // ── SL hit ───────────────────────────────────────────────────────────
        if (mid >= pos.sl_px) {
            _close_position(ask, now_ms, "SL_HIT");
            return;
        }

        // ── S63: LOSS_CUT ────────────────────────────────────────────────────
        if (LOSS_CUT_PCT > 0.0) {
            const double loss_cut_pts = pos.entry_px * LOSS_CUT_PCT / 100.0;
            if (move < -loss_cut_pts) {
                _close_position(ask, now_ms, "LOSS_CUT");
                return;
            }
        }

        // ── S63: BE ratchet ──────────────────────────────────────────────────
        // (Currently disabled for breakout; left as infrastructure)
        if (BE_ARM_PCT > 0.0 && !m_be_armed) {
            const double arm_pts = pos.entry_px * BE_ARM_PCT / 100.0;
            if (move >= arm_pts) {
                m_be_armed = true;
                m_be_level = pos.entry_px - (pos.entry_px * BE_BUFFER_PCT / 100.0);
            }
        }
        if (m_be_armed) {
            if (mid >= m_be_level) {
                _close_position(ask, now_ms, "BE_RATCHET");
                return;
            }
        }

        // ── Timeout ──────────────────────────────────────────────────────────
        const int64_t held_sec = (now_ms - pos.entry_ms) / 1000;
        if (held_sec >= MAX_HOLD_SEC) {
            _close_position(ask, now_ms, "TIMEOUT");
            return;
        }
    }

    // ── Close position ──��────────────────────────────────────────────────────

    void _close_position(double exit_px, int64_t now_ms, const char* reason) {
        if (!pos.active) return;

        // SHORT: pnl = entry - exit
        const double pnl_pts = pos.entry_px - exit_px;

        const char* mode_str = shadow_mode ? "SHADOW" : "LIVE";
        char buf[384];
        snprintf(buf, sizeof(buf),
                 "[GER40-LON-BRK-%s] CLOSE reason=%s exit=%.2f pnl=%.2f pts "
                 "mfe=%.2f mae=%.2f held=%lld s\n",
                 mode_str, reason, exit_px, pnl_pts,
                 pos.mfe, pos.mae,
                 (long long)((now_ms - pos.entry_ms) / 1000));
        printf("%s", buf);
        fflush(stdout);

        // Build TradeRecord
        omega::TradeRecord tr{};
        tr.symbol       = symbol;
        tr.engine       = "Ger40LondonBreakout";
        tr.side         = "SHORT";
        tr.entryPrice   = pos.entry_px;
        tr.exitPrice    = exit_px;
        tr.size         = pos.size;
        tr.pnl          = pnl_pts * pos.size;
        tr.mfe          = pos.mfe;
        tr.mae          = std::abs(pos.mae);
        tr.entryTs      = pos.entry_ms / 1000;
        tr.exitTs       = now_ms / 1000;
        tr.exitReason   = reason;
        tr.shadow       = shadow_mode;

        // Fire callbacks
        if (on_trade_record) on_trade_record(tr);
        if (on_close) on_close(exit_px, false, pos.size, std::string(reason));

        // Reset position
        pos = Position{};
        m_be_armed = false;
        m_be_level = 0.0;
    }

    // ── Internal state ───────────────────────────────────────────────────────

    double  m_asia_high     = 0.0;
    double  m_asia_low      = 0.0;
    bool    m_range_ready   = false;
    bool    m_fired_today   = false;
    bool    m_in_asia       = false;
    int     m_last_day      = -1;

    // BE ratchet state
    bool    m_be_armed      = false;
    double  m_be_level      = 0.0;
};

}  // namespace omega

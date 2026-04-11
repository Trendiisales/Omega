// =============================================================================
//  DomPersistEngine.hpp
//  Lightweight DOM persistence engine for XAUUSD
//
//  Signal: L2 imbalance must exceed threshold from neutral (0.5) for at least
//  p consecutive ticks before entry fires. No drift, no momentum, no VWAP.
//  This is the pure DOM persistence signal identified in backtest on 2 days of
//  real cTrader depth data (l2_ticks_2026-04-09.csv, l2_ticks_2026-04-10.csv).
//
//  Backtest winner params:
//    i=0.05 p=5 dc=0.50  -> T=5  WR=60% PnL=$476  MaxDD=$31   RR=10.74 Exp=95.33
//    i=0.05 p=5 dc=0.20  -> T=7  WR=57% PnL=$467  MaxDD=$61   RR=6.36  Exp=66.80
//    i=0.03 p=10 dc=0.20 -> T=8  WR=50% PnL=$354  MaxDD=$128  RR=3.75  Exp=44.30
//
//  Using: i=0.05, p=5 (best expectancy, best RR, acceptable sample size)
//
//  Entry: market order. SL: ATR * SL_MULT (same as GFE). Trail: MFE-proportional.
//  Sessions: London + NY (slots 1-4 only). Shadow mode ON until edge confirmed.
//
//  Integration:
//    In globals.hpp:  static DomPersistEngine g_dom_persist;
//    In tick_gold.hpp: call g_dom_persist.on_tick() in entry block
// =============================================================================

#pragma once
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include "OmegaTradeLedger.hpp"

// ── Config ───────────────────────────────────────────────────────────────────

// Imbalance deviation threshold from neutral 0.5
// Long signal:  l2_imb > 0.5 + DPE_IMB_THRESHOLD
// Short signal: l2_imb < 0.5 - DPE_IMB_THRESHOLD
static constexpr double DPE_IMB_THRESHOLD    = 0.05;   // backtest winner: i=0.05

// Number of consecutive ticks the threshold must be sustained
static constexpr int    DPE_PERSIST_TICKS    = 5;      // backtest winner: p=5

// SL sizing: ATR * this multiplier (mirrors GFE_ATR_SL_MULT)
static constexpr double DPE_SL_MULT          = 1.0;

// ATR floor when cTrader L2 is live (mirrors GFE_ATR_MIN)
static constexpr double DPE_ATR_MIN          = 2.0;

// ATR floor when L2 is not confirmed live (mirrors GFE_ATR_SL_FLOOR_NO_L2)
static constexpr double DPE_ATR_FLOOR_NO_L2  = 5.0;

// Cooldown after any exit
static constexpr int64_t DPE_COOLDOWN_MS     = 30000; // 30s

// Minimum hold before SL management kicks in
static constexpr int64_t DPE_MIN_HOLD_MS     = 3000;  // 3s

// Max hold: if no step banked and still in position, time out
static constexpr int64_t DPE_MAX_HOLD_MS     = 600000; // 10 min

// Risk per trade (overridable from main.cpp)
static constexpr double DPE_RISK_DOLLARS     = 30.0;

// Minimum ticks received before entries allowed (cold start protection)
static constexpr int    DPE_MIN_ENTRY_TICKS  = 50;

// ATR warmup ticks needed
static constexpr int    DPE_ATR_WARMUP_TICKS = 100;

// Trail: distance = min(0.5*ATR, 0.3*MFE) -- MFE-proportional, same as GFE normal trail
// This produced the high RR in backtest: AvgW=169 vs AvgL=16 means trail lets winners run

// Maximum spread allowed at entry
static constexpr double DPE_MAX_SPREAD       = 2.5;

// ── Engine ───────────────────────────────────────────────────────────────────

struct DomPersistEngine {

    // Public config
    double risk_dollars = DPE_RISK_DOLLARS;

    // Shadow mode: when true, logs signal but does NOT call on_close (no broker order)
    // Set false from main.cpp once shadow logs confirm correct behaviour
    bool shadow_mode = true;

    enum class Phase { IDLE, BUILDING, LIVE, COOLDOWN } phase = Phase::IDLE;

    struct OpenPos {
        bool    active        = false;
        bool    is_long       = false;
        double  entry         = 0.0;
        double  sl            = 0.0;
        double  size          = 0.01;
        double  mfe           = 0.0;
        double  atr_at_entry  = 0.0;
        bool    be_locked     = false;
        bool    partial_closed = false;
        int64_t entry_ts      = 0;  // epoch seconds
    } pos;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // ── Main tick function ────────────────────────────────────────────────
    // bid, ask        : current quotes
    // l2_imb          : L2 imbalance 0..1 from cTrader depth
    // l2_live         : true when cTrader depth events are actively flowing
    // now_ms          : epoch ms
    // session_slot    : 0=dead 1=London 2=London_core 3=overlap 4=NY 5=NY_late 6=Asia
    // on_close        : callback fired on position close (or shadow log if shadow_mode)
    void on_tick(double bid, double ask,
                 double l2_imb, bool l2_live,
                 int64_t now_ms,
                 int session_slot,
                 CloseCallback on_close) noexcept
    {
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // Always update ATR and tick counter
        update_atr(mid, spread);

        // Cooldown
        if (phase == Phase::COOLDOWN) {
            if (now_ms - m_cooldown_start >= DPE_COOLDOWN_MS) {
                phase = Phase::IDLE;
            } else {
                return;
            }
        }

        // Manage open position
        if (phase == Phase::LIVE) {
            manage_position(bid, ask, mid, now_ms, on_close);
            return;
        }

        // Cold start guard
        if (m_ticks_received < DPE_MIN_ENTRY_TICKS) return;

        // ATR warmup guard
        if (m_atr_warmup < DPE_ATR_WARMUP_TICKS) return;

        // Session gate: London + NY only (slots 1-4)
        // Not enough data to trust Asia (slot 6) — 2 days of backtest is insufficient
        const bool session_ok = (session_slot >= 1 && session_slot <= 4);
        if (!session_ok) {
            if (phase == Phase::BUILDING) phase = Phase::IDLE;
            return;
        }

        // Spread gate
        if (spread > DPE_MAX_SPREAD) {
            if (phase == Phase::BUILDING) phase = Phase::IDLE;
            return;
        }

        // L2 must be live — DOM persistence requires real depth events
        if (!l2_live) {
            static int64_t s_l2_log = 0;
            if (now_ms - s_l2_log > 30000) {
                s_l2_log = now_ms;
                printf("[DPE] L2 not live -- no entries\n");
                fflush(stdout);
            }
            if (phase == Phase::BUILDING) phase = Phase::IDLE;
            return;
        }

        // Update persistence window
        update_persistence(l2_imb);

        // Signal evaluation
        const bool long_sig  = (m_long_count  >= DPE_PERSIST_TICKS);
        const bool short_sig = (m_short_count >= DPE_PERSIST_TICKS);

        if (!long_sig && !short_sig) {
            phase = Phase::IDLE;
            return;
        }

        phase = Phase::BUILDING;

        // Both can't be true simultaneously (window is exclusive per direction)
        // but handle gracefully: prefer whichever has higher count
        const bool is_long = long_sig && (!short_sig || m_long_count >= m_short_count);

        // Diagnostic log every 5s while building
        {
            static int64_t s_build_log = 0;
            if (now_ms - s_build_log > 5000) {
                s_build_log = now_ms;
                printf("[DPE-BUILDING] %s l2_imb=%.4f long_cnt=%d short_cnt=%d atr=%.2f slot=%d\n",
                       is_long ? "LONG" : "SHORT",
                       l2_imb, m_long_count, m_short_count, m_atr, session_slot);
                fflush(stdout);
            }
        }

        // Enter
        enter(is_long, bid, ask, spread, l2_live, now_ms, on_close);
    }

    bool has_open_position() const noexcept { return phase == Phase::LIVE; }
    double current_atr()    const noexcept { return m_atr; }

    // Seed ATR from bar data (same pattern as GFE)
    void seed_bar_atr(double bar_atr) noexcept {
        if (bar_atr <= 0.0) return;
        if (m_atr <= 0.0) {
            m_atr = bar_atr;
        } else {
            m_atr = 0.95 * m_atr + 0.05 * bar_atr;
        }
        if (m_atr_ewm <= 0.0) m_atr_ewm = m_atr;
        else m_atr_ewm = 0.95 * m_atr_ewm + 0.05 * bar_atr;
    }

    // Seed mid price on restart
    void seed(double mid, double vix_level = 0.0) noexcept {
        if (mid <= 0.0 || m_atr_warmup >= DPE_ATR_WARMUP_TICKS) return;
        double seed_range;
        if      (vix_level <= 0.0)  seed_range =  8.0;
        else if (vix_level <  15.0) seed_range =  5.0;
        else if (vix_level <  20.0) seed_range =  8.0;
        else if (vix_level <  25.0) seed_range = 12.0;
        else                        seed_range = 18.0;
        m_atr           = seed_range;
        m_atr_ewm       = seed_range;
        m_atr_warmup    = DPE_ATR_WARMUP_TICKS;
        m_last_mid      = mid;
        m_price_window.clear();
        for (int i = 0; i < 100; ++i) m_price_window.push_back(mid);
        printf("[DPE-SEED] mid=%.2f vix=%.1f seed_atr=%.1f\n", mid, vix_level, seed_range);
        fflush(stdout);
    }

    void force_close(double bid, double ask, int64_t now_ms, CloseCallback on_close) noexcept {
        if (!has_open_position()) return;
        const double exit_px = pos.is_long ? bid : ask;
        close_position(exit_px, "FORCE_CLOSE", now_ms, on_close);
    }

private:

    // ATR state
    double             m_atr         = 0.0;
    double             m_atr_ewm     = 0.0;
    double             m_last_mid    = 0.0;
    int                m_atr_warmup  = 0;
    int                m_ticks_received = 0;
    std::deque<double> m_price_window;

    // Persistence window: track consecutive ticks above/below threshold
    // We use a simple rolling approach: count how many of the last DPE_PERSIST_TICKS
    // ticks were in each direction. Unlike GFE's full ring buffer, we use simpler
    // consecutive-only counting since the backtest used strict consecutive ticks.
    int m_long_streak  = 0;  // consecutive ticks where l2_imb > 0.5 + threshold
    int m_short_streak = 0;  // consecutive ticks where l2_imb < 0.5 - threshold
    int m_long_count   = 0;  // capped at DPE_PERSIST_TICKS
    int m_short_count  = 0;  // capped at DPE_PERSIST_TICKS

    int64_t m_cooldown_start   = 0;
    double  m_spread_at_entry  = 0.0;
    int     m_trade_id         = 0;

    void update_atr(double mid, double spread) noexcept {
        m_price_window.push_back(mid);
        if ((int)m_price_window.size() > 100) m_price_window.pop_front();

        if (m_last_mid > 0.0 && (int)m_price_window.size() >= 100) {
            const double hi = *std::max_element(m_price_window.begin(), m_price_window.end());
            const double lo = *std::min_element(m_price_window.begin(), m_price_window.end());
            const double range = std::max(hi - lo, spread);
            if (m_atr_ewm <= 0.0) m_atr_ewm = range;
            else m_atr_ewm = 0.05 * range + 0.95 * m_atr_ewm;
        }
        m_last_mid = mid;
        ++m_atr_warmup;
        if (m_atr_warmup >= DPE_ATR_WARMUP_TICKS)
            m_atr = std::max(DPE_ATR_MIN, m_atr_ewm);
        ++m_ticks_received;
    }

    void update_persistence(double l2_imb) noexcept {
        const double lo_thresh = 0.5 - DPE_IMB_THRESHOLD;  // 0.45
        const double hi_thresh = 0.5 + DPE_IMB_THRESHOLD;  // 0.55

        if (l2_imb > hi_thresh) {
            ++m_long_streak;
            m_short_streak = 0;
        } else if (l2_imb < lo_thresh) {
            ++m_short_streak;
            m_long_streak = 0;
        } else {
            // Neutral tick: reset both streaks
            m_long_streak  = 0;
            m_short_streak = 0;
        }

        m_long_count  = std::min(m_long_streak,  DPE_PERSIST_TICKS);
        m_short_count = std::min(m_short_streak, DPE_PERSIST_TICKS);
    }

    void enter(bool is_long, double bid, double ask, double spread,
               bool l2_live, int64_t now_ms, CloseCallback on_close) noexcept
    {
        if (m_atr <= 0.0) return;

        const double atr_floor = l2_live ? DPE_ATR_MIN : DPE_ATR_FLOOR_NO_L2;
        const double atr       = std::max(atr_floor, m_atr);
        const double sl_pts    = std::max(atr * DPE_SL_MULT, spread * 5.0);
        if (sl_pts <= 0.0) return;

        // Sizing: risk_dollars / (sl_pts * $100/pt for gold)
        static constexpr double TICK_MULT  = 100.0;
        static constexpr double LOT_STEP   = 0.001;
        static constexpr double MIN_LOT    = 0.01;
        static constexpr double MAX_LOT    = 0.50;
        double size = risk_dollars / (sl_pts * TICK_MULT);
        size = std::floor(size / LOT_STEP) * LOT_STEP;
        size = std::max(MIN_LOT, std::min(MAX_LOT, size));

        const double entry_px = is_long ? ask : bid;
        const double sl_px    = is_long ? (entry_px - sl_pts) : (entry_px + sl_pts);

        pos.active       = true;
        pos.is_long      = is_long;
        pos.entry        = entry_px;
        pos.sl           = sl_px;
        pos.size         = size;
        pos.mfe          = 0.0;
        pos.atr_at_entry = atr;
        pos.be_locked    = false;
        pos.partial_closed = false;
        pos.entry_ts     = now_ms / 1000;
        m_spread_at_entry = spread;
        phase            = Phase::LIVE;
        ++m_trade_id;

        // Reset persistence so we don't re-enter immediately
        m_long_streak = m_short_streak = 0;
        m_long_count  = m_short_count  = 0;

        const char* mode = shadow_mode ? " [SHADOW]" : "";
        printf("[DPE] ENTRY %s @ %.2f sl=%.2f sl_pts=%.2f size=%.3f atr=%.2f spread=%.3f%s\n",
               is_long ? "LONG" : "SHORT",
               entry_px, sl_px, sl_pts, size, atr, spread, mode);
        fflush(stdout);

        if (shadow_mode) {
            // Shadow: do not fire on_close callback (no broker order)
            // The entry log above is the observation artifact
            return;
        }
        // Live: notify main.cpp so it can send the broker order
        // on_close is called at EXIT, not entry. main.cpp checks has_open_position()
        // after on_tick() to detect new entries (same pattern as GFE).
        (void)on_close;  // used at close time, not here
    }

    void manage_position(double bid, double ask, double mid,
                         int64_t now_ms, CloseCallback on_close) noexcept
    {
        if (!pos.active) return;

        const double move   = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;

        const double atr    = pos.atr_at_entry;
        const int64_t held_ms = now_ms - (pos.entry_ts * 1000LL);

        // Immediate reversal guard: price never went our way in 30s
        {
            const int64_t held_s = held_ms / 1000;
            const double  adverse = pos.is_long ? (pos.entry - mid) : (mid - pos.entry);
            const double  imm_thresh = std::max(1.5, atr * 0.20);
            if (held_s <= 30 && adverse > imm_thresh && pos.mfe < 0.30) {
                const double exit_px = pos.is_long ? bid : ask;
                close_position(exit_px, "IMM_REVERSAL", now_ms, on_close);
                return;
            }
        }

        // Time stop: 3 min with no progress
        if (!pos.partial_closed && held_ms > 180000) {
            const double adverse = pos.is_long ? (pos.entry - mid) : (mid - pos.entry);
            if (adverse > std::max(2.0, atr * 0.5) && pos.mfe < std::max(2.0, atr * 0.5)) {
                const double exit_px = pos.is_long ? bid : ask;
                close_position(exit_px, "TIME_STOP", now_ms, on_close);
                return;
            }
        }

        // Max hold
        if (held_ms >= DPE_MAX_HOLD_MS && !pos.partial_closed) {
            const double exit_px = pos.is_long ? bid : ask;
            close_position(exit_px, "MAX_HOLD_TIMEOUT", now_ms, on_close);
            return;
        }

        // Step 1: bank 33% at $35 open PnL, lock BE
        if (!pos.partial_closed) {
            const double open_pnl = move * pos.size * 100.0;
            const double step1_trigger = std::max(35.0, 0.75 * atr * pos.size * 100.0);
            if (open_pnl >= step1_trigger) {
                // Bank partial
                const double close_qty = std::floor(pos.size * 0.33 / 0.001) * 0.001;
                if (close_qty >= 0.01) {
                    const double exit_px = pos.is_long ? bid : ask;
                    const double pnl = (pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px))
                                       * close_qty;

                    omega::TradeRecord ptr;
                    ptr.id         = m_trade_id;
                    ptr.symbol     = "XAUUSD";
                    ptr.side       = pos.is_long ? "LONG" : "SHORT";
                    ptr.entryPrice = pos.entry;
                    ptr.exitPrice  = exit_px;
                    ptr.sl         = pos.sl;
                    ptr.size       = close_qty;
                    ptr.pnl        = pnl;
                    ptr.mfe        = pos.mfe * close_qty;
                    ptr.mae        = 0.0;
                    ptr.entryTs    = pos.entry_ts;
                    ptr.exitTs     = now_ms / 1000;
                    ptr.exitReason = "PARTIAL_1R";
                    ptr.engine     = "DomPersistEngine";
                    ptr.regime     = "DOM_PERSIST";
                    ptr.spreadAtEntry = m_spread_at_entry;

                    // Lock SL to entry (BE)
                    if (pos.is_long  && pos.entry > pos.sl) pos.sl = pos.entry;
                    if (!pos.is_long && pos.entry < pos.sl) pos.sl = pos.entry;

                    pos.size         -= close_qty;
                    pos.partial_closed = true;
                    pos.be_locked    = true;

                    printf("[DPE] PARTIAL_1R %s @ %.2f qty=%.3f pnl_usd=%.0f remaining=%.3f%s\n",
                           pos.is_long ? "LONG" : "SHORT",
                           exit_px, close_qty, pnl * 100.0, pos.size,
                           shadow_mode ? " [SHADOW]" : "");
                    fflush(stdout);

                    if (!shadow_mode && on_close) on_close(ptr);
                }
            }
        }

        // Trail: MFE-proportional (same as GFE normal trail)
        if (pos.be_locked && pos.mfe > 0.0) {
            const double trail_dist = std::min(atr * 0.5, pos.mfe * 0.30);
            const double trail_sl   = pos.is_long
                ? (pos.entry + pos.mfe - trail_dist)
                : (pos.entry - pos.mfe + trail_dist);
            if ((pos.is_long  && trail_sl > pos.sl) ||
                (!pos.is_long && trail_sl < pos.sl)) {
                pos.sl = trail_sl;
            }
        }

        // SL check
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (!sl_hit) return;

        const double exit_px = pos.is_long ? bid : ask;
        const char*  reason  = pos.be_locked ? "TRAIL_HIT" : "SL_HIT";
        close_position(exit_px, reason, now_ms, on_close);
    }

    void close_position(double exit_px, const char* reason,
                        int64_t now_ms, CloseCallback on_close) noexcept
    {
        omega::TradeRecord tr;
        tr.id          = m_trade_id;
        tr.symbol      = "XAUUSD";
        tr.side        = pos.is_long ? "LONG" : "SHORT";
        tr.entryPrice  = pos.entry;
        tr.exitPrice   = exit_px;
        tr.sl          = pos.sl;
        tr.size        = pos.size;
        tr.pnl         = (pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px)) * pos.size;
        tr.mfe         = pos.mfe * pos.size;
        tr.mae         = 0.0;
        tr.entryTs     = pos.entry_ts;
        tr.exitTs      = now_ms / 1000;
        tr.exitReason  = reason;
        tr.engine      = "DomPersistEngine";
        tr.regime      = "DOM_PERSIST";
        tr.spreadAtEntry = m_spread_at_entry;

        const double held_s = static_cast<double>(now_ms / 1000 - pos.entry_ts);
        const char* mode = shadow_mode ? " [SHADOW]" : "";
        printf("[DPE] EXIT %s @ %.2f reason=%s pnl_usd=%.0f mfe=%.2f held=%.0fs%s\n",
               pos.is_long ? "LONG" : "SHORT",
               exit_px, reason, tr.pnl * 100.0, pos.mfe, held_s, mode);
        fflush(stdout);

        pos              = OpenPos{};
        phase            = Phase::COOLDOWN;
        m_cooldown_start = now_ms;

        if (!shadow_mode && on_close) on_close(tr);
        else if (shadow_mode) {
            // Shadow: fire on_close so handle_closed_trade logs the result
            // but main.cpp must NOT send a broker order (checked via shadow_mode flag)
            if (on_close) on_close(tr);
        }
    }
};

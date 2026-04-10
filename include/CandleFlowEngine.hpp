// =============================================================================
//  CandleFlowEngine.hpp
//
//  Strategy (updated — sweep-optimised against real L2 data 2026-04-10):
//    ENTRY:  expansion candle (quality gate) + RSI trend direction
//    EXIT:   L2 imbalance reversal (sustained N ticks) OR stagnation
//
//  Entry conditions (ALL required):
//    1. Expansion candle: body >= 60% of range, close breaks prev high/low
//    2. Bar range >= 2.5 * cost (spread + slippage + commission)
//    3. RSI slope EMA direction agrees with candle (period=30, ema=10, thresh=6.0)
//       RSI replaces old DOM entry confirmation -- DOM is EXIT only
//
//  Exit conditions:
//    Primary: L2 imbalance flips against position for >= 2 consecutive ticks
//             imbalance threshold = 0.08 (calibrated to cTrader level-count data)
//             AND minimum hold time of 20s elapsed (prevents noise exits)
//    Safety:  move < cost * 1.0 within 60s stagnation window
//    Hard SL: 1x ATR behind entry
//
//  Sweep result (20,736 configs, real cTrader L2 data 2026-04-10):
//    RSI_P=30  EMA=10  THRESH=6.0  IMB=0.08  IMB_TICKS=2
//    BODY=0.60  COST_MULT=2.5  STAG=60s
//    -> 14 trades/day, 71.4% WR, R:R=1.90, exp=$8.13/trade, maxDD=$19.68
//
//  2026-04-11 fixes:
//    - CFE_IMB_EXIT_TICKS restored to 2 (was changed to 1 -- too hair-trigger)
//    - CFE_IMB_MIN_HOLD_MS = 20000: IMB_EXIT cannot fire before 20s in trade
//      Root cause of 8s/37s exits: single-tick imbalance noise fired immediately
//      after entry. 20s minimum hold eliminates sub-10s exits entirely.
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
#include "OmegaFIX.hpp"          // L2Book, L2Level

namespace omega {

// -----------------------------------------------------------------------------
//  Config -- sweep-optimised values
// -----------------------------------------------------------------------------
static constexpr double  CFE_BODY_RATIO_MIN    = 0.60;
static constexpr double  CFE_COST_SLIPPAGE     = 0.10;
static constexpr double  CFE_COMMISSION_PTS    = 0.10;
static constexpr double  CFE_COST_MULT         = 2.5;
static constexpr int64_t CFE_STAGNATION_MS     = 60000;
static constexpr double  CFE_STAGNATION_MULT   = 1.0;   // exit if mfe < cost*1.0
static constexpr double  CFE_RISK_DOLLARS      = 30.0;
static constexpr double  CFE_MIN_LOT           = 0.01;
static constexpr double  CFE_MAX_LOT           = 0.50;

// RSI trend (entry direction signal)
static constexpr int     CFE_RSI_PERIOD        = 30;    // tick RSI lookback
static constexpr int     CFE_RSI_EMA_N         = 10;    // slope EMA smoothing
static constexpr double  CFE_RSI_THRESH        = 6.0;   // min slope EMA to enter

// L2 imbalance exit (cTrader depth level-count signal)
// imbalance = dom.l2_imb converted to -1..+1: (l2_imb - 0.5) * 2
// Exit long when imb < -CFE_IMB_EXIT_THRESH for >= CFE_IMB_EXIT_TICKS ticks
static constexpr double  CFE_IMB_EXIT_THRESH   = 0.08;  // calibrated to cTrader level-count data (0.4545/0.5455 = imb +/-0.091)
static constexpr int     CFE_IMB_EXIT_TICKS    = 2;     // restored to 2: single tick is noise, need 2 consecutive
static constexpr int64_t CFE_IMB_MIN_HOLD_MS   = 20000; // IMB_EXIT blocked for first 20s after entry
                                                          // prevents immediate noise exits (8s/37s trades confirmed as noise)

// -----------------------------------------------------------------------------
struct CandleFlowEngine {

    // -------------------------------------------------------------------------
    // Public config
    double risk_dollars = CFE_RISK_DOLLARS;
    bool   shadow_mode  = true;

    // -------------------------------------------------------------------------
    enum class Phase { IDLE, LIVE, COOLDOWN } phase = Phase::IDLE;

    struct OpenPos {
        bool    active       = false;
        bool    is_long      = false;
        double  entry        = 0.0;
        double  sl           = 0.0;
        double  size         = 0.01;
        double  cost_pts     = 0.0;
        int64_t entry_ts_ms  = 0;
        double  mfe          = 0.0;
    } pos;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // -------------------------------------------------------------------------
    // DOM snapshot -- passed in every tick from the L2Book
    // l2_imb: level-count imbalance from imbalance_level(), range 0..1
    //         (bid_count / (bid_count + ask_count))
    //         0.5 = balanced, >0.5 = bid pressure, <0.5 = ask pressure
    struct DOMSnap {
        int    bid_count    = 0;
        int    ask_count    = 0;
        double bid_vol      = 0.0;
        double ask_vol      = 0.0;
        double l2_imb       = 0.5;   // level-count imbalance 0..1
        bool   vacuum_ask   = false;
        bool   vacuum_bid   = false;
        bool   wall_above   = false;
        bool   wall_below   = false;
        int    prev_bid_count = 0;
        int    prev_ask_count = 0;
        double prev_bid_vol   = 0.0;
        double prev_ask_vol   = 0.0;
    };

    // -------------------------------------------------------------------------
    // Bar snapshot -- one completed M1 candle
    struct BarSnap {
        double open      = 0.0;
        double high      = 0.0;
        double low       = 0.0;
        double close     = 0.0;
        double prev_high = 0.0;
        double prev_low  = 0.0;
        bool   valid     = false;
    };

    // -------------------------------------------------------------------------
    // Main tick function
    // Called on every XAUUSD tick. RSI is computed internally from mid price.
    // bar: last closed M1 bar
    // dom: current DOM snapshot (build_dom() from L2Book)
    // now_ms: epoch ms
    // atr_pts: current ATR for SL sizing
    // -------------------------------------------------------------------------
    void on_tick(double bid, double ask,
                 const BarSnap& bar,
                 const DOMSnap& dom,
                 int64_t now_ms,
                 double  atr_pts,
                 CloseCallback on_close) noexcept
    {
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // ── RSI update (unconditional, every tick) ───────────────────────────
        rsi_update(mid);

        // ── DOM history ──────────────────────────────────────────────────────
        m_dom_prev = m_dom_cur;
        m_dom_cur  = dom;

        // ── Cooldown ─────────────────────────────────────────────────────────
        if (phase == Phase::COOLDOWN) {
            if (now_ms - m_cooldown_start_ms >= m_cooldown_ms)
                phase = Phase::IDLE;
            else
                return;
        }

        // ── Manage open position ─────────────────────────────────────────────
        if (phase == Phase::LIVE) {
            manage(bid, ask, mid, dom, now_ms, on_close);
            return;
        }

        // ── IDLE: check for entry ────────────────────────────────────────────
        if (!bar.valid) return;
        if (!m_rsi_warmed) return;

        // Gate 1: RSI trend direction
        const int rsi_dir = rsi_direction();
        if (rsi_dir == 0) return;

        // Gate 2: expansion candle agreeing with RSI direction
        const bool bullish = candle_is_bullish(bar);
        const bool bearish = candle_is_bearish(bar);
        if (rsi_dir == +1 && !bullish) return;
        if (rsi_dir == -1 && !bearish) return;

        // Gate 3: cost coverage
        const double cost_pts = spread + CFE_COST_SLIPPAGE * 2.0 + CFE_COMMISSION_PTS * 2.0;
        const double bar_range = bar.high - bar.low;
        if (bar_range < CFE_COST_MULT * cost_pts) {
            static int64_t s_log = 0;
            if (now_ms - s_log > 10000) {
                s_log = now_ms;
                std::cout << "[CFE-COST-BLOCK] range=" << std::fixed << std::setprecision(3)
                          << bar_range << " < min=" << CFE_COST_MULT * cost_pts
                          << " spread=" << spread << "\n";
                std::cout.flush();
            }
            return;
        }

        // All gates passed
        enter(rsi_dir == +1, bid, ask, spread, cost_pts, atr_pts, now_ms);
    }

    bool has_open_position() const noexcept { return phase == Phase::LIVE; }

    void force_close(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (!has_open_position()) return;
        close_pos(pos.is_long ? bid : ask, "FORCE_CLOSE", now_ms, cb);
    }

    // -------------------------------------------------------------------------
    // Build DOMSnap from L2Book + previous book
    // Uses imbalance_level() -- level-count based, correct for cTrader feed
    // where size_raw=0 is sent on depth quotes (level count is the real signal)
    // -------------------------------------------------------------------------
    static DOMSnap build_dom(const L2Book& book,
                              const L2Book& prev_book,
                              double /*mid_price*/) noexcept
    {
        DOMSnap d;
        d.bid_count = book.bid_count;
        d.ask_count = book.ask_count;
        d.bid_vol   = static_cast<double>(book.bid_count);
        d.ask_vol   = static_cast<double>(book.ask_count);

        d.prev_bid_count = prev_book.bid_count;
        d.prev_ask_count = prev_book.ask_count;
        d.prev_bid_vol   = static_cast<double>(prev_book.bid_count);
        d.prev_ask_vol   = static_cast<double>(prev_book.ask_count);

        // Level-count imbalance: primary signal for cTrader L2 feed
        d.l2_imb = book.imbalance_level();

        d.vacuum_ask = book.liquidity_vacuum_ask();
        d.vacuum_bid = book.liquidity_vacuum_bid();
        d.wall_above = (book.ask_count >= 4 && book.bid_count <= 2);
        d.wall_below = (book.bid_count >= 4 && book.ask_count <= 2);
        return d;
    }

private:
    // -------------------------------------------------------------------------
    // State
    int64_t m_cooldown_start_ms = 0;
    int64_t m_cooldown_ms       = 15000;
    int     m_trade_id          = 0;
    DOMSnap m_dom_cur;
    DOMSnap m_dom_prev;

    // RSI state
    std::deque<double> m_rsi_gains;
    std::deque<double> m_rsi_losses;
    double m_rsi_prev_mid  = 0.0;
    double m_rsi_cur       = 50.0;
    double m_rsi_prev      = 50.0;
    double m_rsi_trend     = 0.0;   // EMA of RSI slope
    bool   m_rsi_warmed    = false;
    double m_rsi_ema_alpha = 2.0 / (CFE_RSI_EMA_N + 1);

    // L2 imbalance exit state
    int m_imb_against_ticks = 0;   // consecutive ticks imb is against position
    int m_prev_depth_bid    = 0;
    int m_prev_depth_ask    = 0;

    // -------------------------------------------------------------------------
    // RSI slope EMA -- updated every tick unconditionally
    void rsi_update(double mid) noexcept {
        if (m_rsi_prev_mid == 0.0) { m_rsi_prev_mid = mid; return; }
        const double chg = mid - m_rsi_prev_mid;
        m_rsi_prev_mid   = mid;
        m_rsi_gains.push_back(chg > 0 ? chg : 0.0);
        m_rsi_losses.push_back(chg < 0 ? -chg : 0.0);
        if ((int)m_rsi_gains.size() > CFE_RSI_PERIOD) {
            m_rsi_gains.pop_front();
            m_rsi_losses.pop_front();
        }
        if ((int)m_rsi_gains.size() < CFE_RSI_PERIOD) return;
        double ag = 0.0, al = 0.0;
        for (auto x : m_rsi_gains)  ag += x;
        for (auto x : m_rsi_losses) al += x;
        ag /= CFE_RSI_PERIOD; al /= CFE_RSI_PERIOD;
        m_rsi_prev = m_rsi_cur;
        m_rsi_cur  = (al == 0.0) ? 100.0 : 100.0 - 100.0 / (1.0 + ag / al);
        const double slope = m_rsi_cur - m_rsi_prev;
        if (!m_rsi_warmed) { m_rsi_trend = slope; m_rsi_warmed = true; }
        else m_rsi_trend = slope * m_rsi_ema_alpha + m_rsi_trend * (1.0 - m_rsi_ema_alpha);
    }

    // Returns +1 (up trend), -1 (down trend), 0 (no signal)
    int rsi_direction() const noexcept {
        if (!m_rsi_warmed) return 0;
        if (m_rsi_trend >  CFE_RSI_THRESH) return +1;
        if (m_rsi_trend < -CFE_RSI_THRESH) return -1;
        return 0;
    }

    // -------------------------------------------------------------------------
    // Candle direction
    static bool candle_is_bullish(const BarSnap& b) noexcept {
        if (b.close <= b.open) return false;
        const double range = b.high - b.low;
        if (range <= 0.0) return false;
        if ((b.close - b.open) / range < CFE_BODY_RATIO_MIN) return false;
        if (b.close <= b.prev_high) return false;
        return true;
    }

    static bool candle_is_bearish(const BarSnap& b) noexcept {
        if (b.close >= b.open) return false;
        const double range = b.high - b.low;
        if (range <= 0.0) return false;
        if ((b.open - b.close) / range < CFE_BODY_RATIO_MIN) return false;
        if (b.close >= b.prev_low) return false;
        return true;
    }

    // -------------------------------------------------------------------------
    // L2 imbalance exit
    // imb = l2_imb converted to -1..+1 centered: (l2_imb - 0.5) * 2
    // Exit long when imb < -thresh for >= N ticks AND hold >= CFE_IMB_MIN_HOLD_MS
    // Exit short when imb > +thresh for >= N ticks AND hold >= CFE_IMB_MIN_HOLD_MS
    // Also exits on imb against for >= 1 tick AND depth drop AND hold >= min
    bool check_imb_exit(bool is_long, const DOMSnap& dom, int64_t hold_ms) noexcept {
        // Minimum hold guard: IMB_EXIT is blocked for the first CFE_IMB_MIN_HOLD_MS
        // milliseconds after entry. This eliminates the sub-10s noise exits caused by
        // a single imbalance blip immediately after entry (8s/37s losses on 2026-04-11).
        if (hold_ms < CFE_IMB_MIN_HOLD_MS) {
            // Still track counter ticks for continuity -- just don't act on them yet
            const double imb_track = (dom.l2_imb - 0.5) * 2.0;
            const bool against_track = is_long
                ? (imb_track < -CFE_IMB_EXIT_THRESH)
                : (imb_track >  CFE_IMB_EXIT_THRESH);
            if (against_track) m_imb_against_ticks++;
            else               m_imb_against_ticks = 0;
            m_prev_depth_bid = dom.bid_count;
            m_prev_depth_ask = dom.ask_count;
            return false;
        }

        // Use level-count imbalance from cTrader depth feed directly
        const double imb = (dom.l2_imb - 0.5) * 2.0;  // -1..+1
        const bool against = is_long
            ? (imb < -CFE_IMB_EXIT_THRESH)
            : (imb >  CFE_IMB_EXIT_THRESH);

        if (against) m_imb_against_ticks++;
        else         m_imb_against_ticks = 0;

        const bool depth_drop = is_long
            ? (dom.bid_count < m_prev_depth_bid)
            : (dom.ask_count < m_prev_depth_ask);

        m_prev_depth_bid = dom.bid_count;
        m_prev_depth_ask = dom.ask_count;

        return (m_imb_against_ticks >= CFE_IMB_EXIT_TICKS)
            || (m_imb_against_ticks >= 1 && depth_drop);
    }

    // -------------------------------------------------------------------------
    void enter(bool is_long, double bid, double ask,
               double spread, double cost_pts,
               double atr_pts, int64_t now_ms) noexcept
    {
        const double entry_px = is_long ? ask : bid;
        const double sl_pts   = (atr_pts > 0.0) ? atr_pts : spread * 5.0;
        const double sl_px    = is_long ? (entry_px - sl_pts) : (entry_px + sl_pts);
        double size = risk_dollars / (sl_pts * 100.0);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(CFE_MIN_LOT, std::min(CFE_MAX_LOT, size));

        pos.active      = true;
        pos.is_long     = is_long;
        pos.entry       = entry_px;
        pos.sl          = sl_px;
        pos.size        = size;
        pos.cost_pts    = cost_pts;
        pos.entry_ts_ms = now_ms;
        pos.mfe         = 0.0;
        ++m_trade_id;
        phase = Phase::LIVE;

        // Reset imbalance exit state
        m_imb_against_ticks = 0;
        m_prev_depth_bid    = m_dom_cur.bid_count;
        m_prev_depth_ask    = m_dom_cur.ask_count;

        std::cout << "[CFE] ENTRY " << (is_long?"LONG":"SHORT")
                  << " @ " << std::fixed << std::setprecision(2) << entry_px
                  << " sl=" << sl_px << " sl_pts=" << sl_pts
                  << " size=" << std::setprecision(3) << size
                  << " cost=" << cost_pts
                  << " rsi_trend=" << std::setprecision(2) << m_rsi_trend
                  << " atr=" << atr_pts
                  << " spread=" << spread
                  << (shadow_mode?" [SHADOW]":"") << "\n";
        std::cout.flush();
    }

    // -------------------------------------------------------------------------
    void manage(double bid, double ask, double mid,
                const DOMSnap& dom,
                int64_t now_ms,
                CloseCallback on_close) noexcept
    {
        if (!pos.active) return;

        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;

        const int64_t hold_ms = now_ms - pos.entry_ts_ms;

        // Hard SL
        if (pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl)) {
            close_pos(pos.is_long ? bid : ask, "SL_HIT", now_ms, on_close);
            return;
        }

        // L2 imbalance exit -- requires CFE_IMB_MIN_HOLD_MS elapsed
        if (check_imb_exit(pos.is_long, dom, hold_ms)) {
            const double px = pos.is_long ? bid : ask;
            std::cout << "[CFE] IMB-EXIT " << (pos.is_long?"LONG":"SHORT")
                      << " imb_ticks=" << m_imb_against_ticks
                      << " l2_imb=" << std::fixed << std::setprecision(4) << dom.l2_imb
                      << " hold_ms=" << hold_ms
                      << " @ " << std::setprecision(2) << px
                      << " mfe=" << std::setprecision(3) << pos.mfe << "\n";
            std::cout.flush();
            close_pos(px, "IMB_EXIT", now_ms, on_close);
            return;
        }

        // Stagnation safety exit
        if (hold_ms >= CFE_STAGNATION_MS) {
            if (pos.mfe < pos.cost_pts * CFE_STAGNATION_MULT) {
                const double px = pos.is_long ? bid : ask;
                std::cout << "[CFE] STAGNATION-EXIT " << (pos.is_long?"LONG":"SHORT")
                          << " held=" << hold_ms
                          << "ms mfe=" << std::fixed << std::setprecision(3) << pos.mfe
                          << " < need=" << pos.cost_pts * CFE_STAGNATION_MULT << "\n";
                std::cout.flush();
                close_pos(px, "STAGNATION", now_ms, on_close);
                return;
            }
        }
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
        tr.size       = pos.size;
        tr.pnl        = (pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px))
                        * pos.size;
        tr.mfe        = pos.mfe * pos.size;
        tr.mae        = 0.0;
        tr.entryTs    = pos.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.engine     = "CandleFlowEngine";
        tr.regime     = "CANDLE_FLOW";
        tr.l2_live    = true;

        std::cout << "[CFE] EXIT " << (pos.is_long?"LONG":"SHORT")
                  << " @ " << std::fixed << std::setprecision(2) << exit_px
                  << " reason=" << reason
                  << " pnl_raw=" << std::setprecision(4) << tr.pnl
                  << " pnl_usd=" << std::setprecision(2) << (tr.pnl * 100.0)
                  << " mfe=" << std::setprecision(3) << pos.mfe
                  << " held=" << std::setprecision(0)
                  << static_cast<double>((now_ms - pos.entry_ts_ms) / 1000) << "s"
                  << (shadow_mode?" [SHADOW]":"") << "\n";
        std::cout.flush();

        pos = OpenPos{};
        phase = Phase::COOLDOWN;
        m_cooldown_start_ms = now_ms;
        m_cooldown_ms = (strcmp(reason,"IMB_EXIT")   == 0) ? 5000
                      : (strcmp(reason,"STAGNATION") == 0) ? 10000
                      :                                       15000;
        m_imb_against_ticks = 0;

        if (on_close) on_close(tr);
    }
};

} // namespace omega

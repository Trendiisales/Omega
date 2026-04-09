// =============================================================================
//  CandleFlowEngine.hpp
//
//  Strategy:
//    ENTRY:  candle direction + cost coverage + DOM confirmation (2-of-4)
//    EXIT:   DOM reversal (2-of-4 reversal signals) OR stagnation safety exit
//
//  Entry conditions (ALL three required):
//    1. Expansion candle: body >= 60% of range, close breaks prev high/low
//    2. Expected move >= 2x cost (spread + slippage + commission)
//    3. DOM confirms direction: 2-of-4 signals agree
//
//  DOM entry signals (long):
//    - bid_count > ask_count
//    - ask levels shrinking (consumption)
//    - liquidity vacuum on ask side (offers pulled)
//    - bid sizes increasing near price
//
//  DOM exit signals (long) -- exit on 2-of-4:
//    - bid_count decreasing AND ask_count increasing sharply
//    - large sell wall appears above price
//    - bids shrinking quickly + asks stacking
//    - support bids disappear
//
//  Safety exit: move < cost * 1.5 within STAGNATION_MS
// =============================================================================

#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <deque>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include "OmegaTradeLedger.hpp"
#include "OmegaFIX.hpp"          // L2Book, L2Level

namespace omega {

// -----------------------------------------------------------------------------
//  Config
// -----------------------------------------------------------------------------
static constexpr double CFE_BODY_RATIO_MIN    = 0.60;   // candle body >= 60% of range
static constexpr double CFE_COST_SLIPPAGE     = 0.10;   // pts slippage per side (conservative)
static constexpr double CFE_COMMISSION_PTS    = 0.10;   // pts commission per side
static constexpr double CFE_COST_MULT         = 2.0;    // expected_move >= 2 * cost
static constexpr double CFE_WALL_THRESHOLD    = 40.0;   // level size >= this = wall (level-count units)
static constexpr int    CFE_DOM_CONFIRM_MIN   = 2;      // need 2-of-4 DOM signals
static constexpr int    CFE_DOM_EXIT_MIN      = 2;      // need 2-of-4 DOM reversal signals
static constexpr int64_t CFE_STAGNATION_MS   = 8000;   // 8s stagnation window
static constexpr double CFE_STAGNATION_MULT   = 1.5;    // exit if move < cost * 1.5 within window
static constexpr double CFE_RISK_DOLLARS      = 30.0;   // default risk per trade
static constexpr double CFE_MIN_LOT           = 0.01;
static constexpr double CFE_MAX_LOT           = 0.50;

// -----------------------------------------------------------------------------
struct CandleFlowEngine {

    // -------------------------------------------------------------------------
    // Public config
    double risk_dollars = CFE_RISK_DOLLARS;
    bool   shadow_mode  = true;   // true = log only, no real orders

    // -------------------------------------------------------------------------
    // Observable state
    enum class Phase { IDLE, LIVE, COOLDOWN } phase = Phase::IDLE;

    struct OpenPos {
        bool    active        = false;
        bool    is_long       = false;
        double  entry         = 0.0;
        double  sl            = 0.0;   // hard SL = entry +/- ATR (safety only)
        double  size          = 0.01;
        double  cost_pts      = 0.0;   // total cost at entry (for stagnation check)
        int64_t entry_ts_ms   = 0;
        double  mfe           = 0.0;
        int     prev_bid_count = 0;    // DOM state at entry tick for delta tracking
        int     prev_ask_count = 0;
    } pos;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // -------------------------------------------------------------------------
    // DOM snapshot -- passed in every tick from the L2Book
    // Populated by caller from g_l2_books["XAUUSD"] under lock, or from
    // AtomicL2 fields on the hot path.
    struct DOMSnap {
        int    bid_count    = 0;   // number of active bid levels (0-5)
        int    ask_count    = 0;   // number of active ask levels (0-5)
        double bid_vol      = 0.0; // total bid size (may be level count if sizes=0)
        double ask_vol      = 0.0; // total ask size
        bool   vacuum_ask   = false; // top-3 ask is very thin
        bool   vacuum_bid   = false; // top-3 bid is very thin
        bool   wall_above   = false; // large sell wall above price
        bool   wall_below   = false; // large buy wall below price
        // Previous tick DOM (filled by engine internally)
        int    prev_bid_count = 0;
        int    prev_ask_count = 0;
        double prev_bid_vol   = 0.0;
        double prev_ask_vol   = 0.0;
    };

    // -------------------------------------------------------------------------
    // Bar snapshot -- one completed M1 candle
    struct BarSnap {
        double open  = 0.0;
        double high  = 0.0;
        double low   = 0.0;
        double close = 0.0;
        double prev_high = 0.0;  // previous candle high
        double prev_low  = 0.0;  // previous candle low
        bool   valid     = false;
    };

    // -------------------------------------------------------------------------
    // Main tick function
    // bid, ask      : current quotes
    // bar           : last closed M1 bar + previous bar boundaries
    // dom           : current DOM snapshot (built from L2Book)
    // now_ms        : epoch ms
    // atr_pts       : current ATR for SL sizing
    // on_close      : callback on position close
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

        // Update DOM history
        m_dom_prev = m_dom_cur;
        m_dom_cur  = dom;

        // Cooldown
        if (phase == Phase::COOLDOWN) {
            if (now_ms - m_cooldown_start_ms >= m_cooldown_ms) {
                phase = Phase::IDLE;
            } else {
                return;
            }
        }

        // Manage open position
        if (phase == Phase::LIVE) {
            manage(bid, ask, mid, dom, now_ms, atr_pts, on_close);
            return;
        }

        // ── IDLE: check for entry ──────────────────────────────────────────
        if (!bar.valid) return;

        // 1. Candle direction
        const bool bullish = candle_is_bullish(bar);
        const bool bearish = candle_is_bearish(bar);
        if (!bullish && !bearish) return;

        // 2. Cost coverage
        const double cost_pts = spread + CFE_COST_SLIPPAGE * 2.0 + CFE_COMMISSION_PTS * 2.0;
        // Expected move: use the actual bar range -- this is what just happened.
        // ATR is a session average and passes even on dead tape (ATR=2pt, move=0.1pt).
        // The bar range is the real move that triggered this candle signal.
        const double bar_range   = bar.high - bar.low;
        const double expected    = bar_range;  // actual candle range, not session ATR
        if (expected < CFE_COST_MULT * cost_pts) {
            static int64_t s_cost_log = 0;
            if (now_ms - s_cost_log > 10000) {
                s_cost_log = now_ms;
                std::cout << "[CFE-COST-BLOCK] expected=" << expected << " < " << CFE_COST_MULT << "x cost=" << cost_pts << " -- skipping\n"; std::cout.flush();
            }
            return;
        }

        // 3. DOM confirmation
        const int dom_score = dom_entry_score(bullish, dom);
        if (dom_score < CFE_DOM_CONFIRM_MIN) {
            static int64_t s_dom_log = 0;
            if (now_ms - s_dom_log > 8000) {
                s_dom_log = now_ms;
                std::cout << "[CFE-DOM-BLOCK] " << (bullish?"LONG":"SHORT") << " dom_score=" << dom_score << " < " << CFE_DOM_CONFIRM_MIN << " -- no DOM confirmation\n"; std::cout.flush();
            }
            return;
        }

        // All gates passed -- enter
        enter(bullish, bid, ask, spread, cost_pts, atr_pts, now_ms);
    }

    bool has_open_position() const noexcept { return phase == Phase::LIVE; }

    void force_close(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (!has_open_position()) return;
        const double px = pos.is_long ? bid : ask;
        close_pos(px, "FORCE_CLOSE", now_ms, cb);
    }

    // -------------------------------------------------------------------------
    // Build a DOMSnap from an L2Book + previous snapshot
    // Call this in tick_gold.hpp before passing to on_tick()
    // -------------------------------------------------------------------------
    static DOMSnap build_dom(const L2Book& book,
                              const L2Book& prev_book,
                              double mid_price) noexcept
    {
        DOMSnap d;
        d.bid_count = book.bid_count;
        d.ask_count = book.ask_count;

        // Sizes: BlackBull sends size_raw=0 so we use level count as proxy
        // bid_vol = bid_count, ask_vol = ask_count (uniform unit per level)
        d.bid_vol = static_cast<double>(book.bid_count);
        d.ask_vol = static_cast<double>(book.ask_count);

        d.prev_bid_count = prev_book.bid_count;
        d.prev_ask_count = prev_book.ask_count;
        d.prev_bid_vol   = static_cast<double>(prev_book.bid_count);
        d.prev_ask_vol   = static_cast<double>(prev_book.ask_count);

        d.vacuum_ask = book.liquidity_vacuum_ask();
        d.vacuum_bid = book.liquidity_vacuum_bid();

        // Wall detection: top ask level has >= CFE_WALL_THRESHOLD units above price
        // (using level count units: CFE_WALL_THRESHOLD relative to 5-level max)
        // Wall above = ask_count == 5 (all 5 ask slots full = maximum resistance)
        // Wall below = bid_count == 5 (all 5 bid slots full = maximum support)
        // Threshold: wall fires when count is at least 4 out of 5 on one side
        // and the other side has <= 2 -- strong imbalance = wall
        d.wall_above = (book.ask_count >= 4 && book.bid_count <= 2);
        d.wall_below = (book.bid_count >= 4 && book.ask_count <= 2);

        (void)mid_price;
        return d;
    }

private:
    // -------------------------------------------------------------------------
    // State
    int64_t m_cooldown_start_ms = 0;
    int64_t m_cooldown_ms       = 15000;  // 15s default cooldown
    int     m_trade_id          = 0;
    DOMSnap m_dom_cur;
    DOMSnap m_dom_prev;

    // -------------------------------------------------------------------------
    // Candle direction checks
    static bool candle_is_bullish(const BarSnap& b) noexcept {
        if (b.close <= b.open) return false;              // must close up
        const double range = b.high - b.low;
        if (range <= 0.0) return false;
        const double body = b.close - b.open;
        if (body / range < CFE_BODY_RATIO_MIN) return false; // body >= 60% range
        if (b.close <= b.prev_high) return false;         // close must break prev high
        return true;
    }

    static bool candle_is_bearish(const BarSnap& b) noexcept {
        if (b.close >= b.open) return false;              // must close down
        const double range = b.high - b.low;
        if (range <= 0.0) return false;
        const double body = b.open - b.close;
        if (body / range < CFE_BODY_RATIO_MIN) return false; // body >= 60% range
        if (b.close >= b.prev_low) return false;          // close must break prev low
        return true;
    }

    // -------------------------------------------------------------------------
    // DOM entry score: count how many of 4 signals confirm direction
    // Returns 0-4. Need >= CFE_DOM_CONFIRM_MIN to enter.
    static int dom_entry_score(bool is_long, const DOMSnap& d) noexcept {
        int score = 0;

        if (is_long) {
            // Signal 1: bid volume > ask volume
            if (d.bid_vol > d.ask_vol) ++score;
            // Signal 2: ask levels shrinking (being consumed)
            if (d.ask_count < d.prev_ask_count && d.prev_ask_count > 0) ++score;
            // Signal 3: ask vacuum (offers pulled / thin)
            if (d.vacuum_ask) ++score;
            // Signal 4: bid levels increasing near price (stacking)
            if (d.bid_count > d.prev_bid_count) ++score;
        } else {
            // Signal 1: ask volume > bid volume
            if (d.ask_vol > d.bid_vol) ++score;
            // Signal 2: bid levels shrinking (being consumed)
            if (d.bid_count < d.prev_bid_count && d.prev_bid_count > 0) ++score;
            // Signal 3: bid vacuum (bids pulled / thin)
            if (d.vacuum_bid) ++score;
            // Signal 4: ask levels increasing above price (stacking)
            if (d.ask_count > d.prev_ask_count) ++score;
        }

        return score;
    }

    // -------------------------------------------------------------------------
    // DOM exit score: count how many of 4 reversal signals fire
    // Returns 0-4. Exit when >= CFE_DOM_EXIT_MIN.
    int dom_exit_score(bool is_long, const DOMSnap& cur, const DOMSnap& prev) const noexcept {
        int score = 0;

        if (is_long) {
            // Exit Rule 1: bid vol decreasing AND ask vol increasing sharply
            const bool bid_drop  = (cur.bid_vol  < prev.bid_vol  * 0.7);  // >30% drop
            const bool ask_surge = (cur.ask_vol  > prev.ask_vol  * 1.3);  // >30% surge
            if (bid_drop || ask_surge) ++score;

            // Exit Rule 2: large sell wall appears above price
            if (cur.wall_above && !prev.wall_above) ++score;

            // Exit Rule 3: bids shrinking + asks stacking (consumption flip)
            const bool bids_shrink = (cur.bid_count < prev.bid_count);
            const bool asks_stack  = (cur.ask_count > prev.ask_count);
            if (bids_shrink && asks_stack) ++score;

            // Exit Rule 4: support bids disappear (bid vacuum)
            if (cur.vacuum_bid && !prev.vacuum_bid) ++score;

        } else {
            // Exit Rule 1: ask vol decreasing AND bid vol increasing sharply
            const bool ask_drop  = (cur.ask_vol  < prev.ask_vol  * 0.7);
            const bool bid_surge = (cur.bid_vol  > prev.bid_vol  * 1.3);
            if (ask_drop || bid_surge) ++score;

            // Exit Rule 2: large buy wall appears below price
            if (cur.wall_below && !prev.wall_below) ++score;

            // Exit Rule 3: asks shrinking + bids stacking
            const bool asks_shrink = (cur.ask_count < prev.ask_count);
            const bool bids_stack  = (cur.bid_count > prev.bid_count);
            if (asks_shrink && bids_stack) ++score;

            // Exit Rule 4: offers above price disappear (ask vacuum)
            if (cur.vacuum_ask && !prev.vacuum_ask) ++score;
        }

        return score;
    }

    // -------------------------------------------------------------------------
    void enter(bool is_long, double bid, double ask,
               double spread, double cost_pts,
               double atr_pts, int64_t now_ms) noexcept
    {
        const double entry_px = is_long ? ask : bid;

        // SL: 1x ATR behind entry (hard safety only -- primary exit is DOM)
        const double sl_pts   = (atr_pts > 0.0) ? atr_pts : spread * 5.0;
        const double sl_px    = is_long ? (entry_px - sl_pts) : (entry_px + sl_pts);

        // Sizing: risk / (sl_pts * tick_value)
        const double tick_val = 100.0; // XAUUSD: $100/pt/lot
        double size = risk_dollars / (sl_pts * tick_val);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(CFE_MIN_LOT, std::min(CFE_MAX_LOT, size));

        pos.active       = true;
        pos.is_long      = is_long;
        pos.entry        = entry_px;
        pos.sl           = sl_px;
        pos.size         = size;
        pos.cost_pts     = cost_pts;
        pos.entry_ts_ms  = now_ms;
        pos.mfe          = 0.0;
        pos.prev_bid_count = m_dom_cur.bid_count;
        pos.prev_ask_count = m_dom_cur.ask_count;
        ++m_trade_id;
        phase = Phase::LIVE;

        std::cout << "[CFE] ENTRY " << (is_long?"LONG":"SHORT") << " @ " << std::fixed << std::setprecision(2) << entry_px << " sl=" << sl_px << " sl_pts=" << sl_pts << " size=" << std::setprecision(3) << size << " cost=" << cost_pts << " atr=" << std::setprecision(2) << atr_pts << " spread=" << spread << (shadow_mode?" [SHADOW]":"") << "\n"; std::cout.flush();
    }

    // -------------------------------------------------------------------------
    void manage(double bid, double ask, double mid,
                const DOMSnap& dom,
                int64_t now_ms, double /*atr_pts*/,
                CloseCallback on_close) noexcept
    {
        if (!pos.active) return;

        // Track MFE
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;

        // ── Hard SL (safety backstop -- DOM exits should fire first) ────────
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (sl_hit) {
            const double px = pos.is_long ? bid : ask;
            close_pos(px, "SL_HIT", now_ms, on_close);
            return;
        }

        // ── DOM reversal exit ────────────────────────────────────────────────
        // Compare current DOM to previous DOM snapshot
        const int exit_score = dom_exit_score(pos.is_long, dom, m_dom_prev);
        if (exit_score >= CFE_DOM_EXIT_MIN) {
            const double px = pos.is_long ? bid : ask;
                std::cout << "[CFE] DOM-EXIT " << (pos.is_long?"LONG":"SHORT") << " exit_score=" << exit_score << " >= " << CFE_DOM_EXIT_MIN << " @ " << std::fixed << std::setprecision(2) << px << " mfe=" << std::setprecision(3) << pos.mfe << "\n"; std::cout.flush();
            fflush(stdout);
            close_pos(px, "DOM_REVERSAL", now_ms, on_close);
            return;
        }

        // ── Stagnation safety exit ───────────────────────────────────────────
        // If price hasn't moved enough to cover cost * 1.5 within STAGNATION_MS
        const int64_t held_ms = now_ms - pos.entry_ts_ms;
        if (held_ms >= CFE_STAGNATION_MS) {
            const double min_move = pos.cost_pts * CFE_STAGNATION_MULT;
            if (pos.mfe < min_move) {
                const double px = pos.is_long ? bid : ask;
                std::cout << "[CFE] STAGNATION-EXIT " << (pos.is_long?"LONG":"SHORT") << " held=" << held_ms << "ms mfe=" << std::fixed << std::setprecision(3) << pos.mfe << " < need=" << min_move << "\n"; std::cout.flush();
                fflush(stdout);
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
        tr.id          = m_trade_id;
        tr.symbol      = "XAUUSD";
        tr.side        = pos.is_long ? "LONG" : "SHORT";
        tr.entryPrice  = pos.entry;
        tr.exitPrice   = exit_px;
        tr.sl          = pos.sl;
        tr.size        = pos.size;
        tr.pnl         = (pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px))
                         * pos.size;
        tr.mfe         = pos.mfe * pos.size;
        tr.mae         = 0.0;
        tr.entryTs     = pos.entry_ts_ms / 1000;
        tr.exitTs      = now_ms / 1000;
        tr.exitReason  = reason;
        tr.engine      = "CandleFlowEngine";
        tr.regime      = "CANDLE_FLOW";
        tr.l2_live     = true;

        std::cout << "[CFE] EXIT " << (pos.is_long?"LONG":"SHORT") << " @ " << std::fixed << std::setprecision(2) << exit_px << " reason=" << reason << " pnl_raw=" << std::setprecision(4) << tr.pnl << " pnl_usd=" << std::setprecision(2) << (tr.pnl*100.0) << " mfe=" << std::setprecision(3) << pos.mfe << " held=" << std::setprecision(0) << static_cast<double>(now_ms/1000-pos.entry_ts_ms/1000) << "s" << (shadow_mode?" [SHADOW]":"") << "\n"; std::cout.flush();

        pos             = OpenPos{};
        phase           = Phase::COOLDOWN;
        m_cooldown_start_ms = now_ms;
        // Short cooldown after DOM exit -- DOM already flipped so re-entry
        // in the reverse direction may be valid quickly.
        m_cooldown_ms = (strcmp(reason, "DOM_REVERSAL") == 0) ? 5000
                      : (strcmp(reason, "STAGNATION")   == 0) ? 10000
                      :                                          15000;

        if (on_close) on_close(tr);
    }
};

} // namespace omega

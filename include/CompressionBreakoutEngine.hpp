// =============================================================================
// CompressionBreakoutEngine.hpp -- Compression coil -> momentum break engine
//
// STRATEGY:
//   Waits for price to compress (ATR contracting, tight Bollinger bands),
//   then detects the directional break (price closes outside range + CVD confirms
//   + EWM drift aligns), enters on first pullback or bar close post-break.
//
//   PHASE 1 -- COMPRESSION DETECTION (per M1 bar):
//     - Track N-bar rolling range (high - low over CBE_COMP_BARS bars)
//     - Compression when range < CBE_COMP_RANGE_MULT * ATR14
//     - Require CBE_COMP_MIN_BARS consecutive compression bars to arm
//
//   PHASE 2 -- BREAK DETECTION (per tick):
//     - Price closes outside compression range by >= CBE_BREAK_FRAC * ATR
//     - EWM drift direction matches break
//     - CVD direction matches break (no divergence)
//     - RSI not exhausted (< CBE_RSI_BLOCK_OB for LONGs, > CBE_RSI_BLOCK_OS for SHORTs)
//     - Session gate: London (07-12 UTC) + NY (13-20 UTC) only
//     - London/NY LONG block: 07-20 UTC LONGs blocked based on sweep result
//       (last session summary: London/NY LONG 0% WR -- re-enabled when out-of-sample proves)
//
//   PHASE 3 -- ENTRY:
//     - Enter at market on break confirmation tick
//     - SL: below compression range low (LONG) / above range high (SHORT)
//       = structural SL, not ATR mult
//     - TP: SL distance * CBE_TP_RR (R:R multiplier)
//     - If SL distance > CBE_MAX_SL_ATR_MULT * ATR: skip (too wide)
//
//   PHASE 4 -- MANAGEMENT:
//     - Breakeven when MFE >= 50% of TP distance
//     - Trail arms when MFE >= CBE_TRAIL_ARM_FRAC * TP distance
//     - Trail distance = CBE_TRAIL_DIST_FRAC * ATR
//     - Timeout: CBE_TIMEOUT_MS (exit if not closed)
//     - If price re-enters compression range: thesis failed, exit
//
// WIRING (tick_gold.hpp, after CFE block):
//   // CBE position management -- always runs when open
//   if (g_cbe.has_open_position()) {
//       g_cbe.on_tick(bid, ask, now_ms_g,
//           g_gold_stack.ewm_drift(),
//           g_macro_ctx.gold_cvd_bear_div,
//           g_macro_ctx.gold_cvd_bull_div,
//           g_macro_ctx.session_slot,
//           [&](const omega::TradeRecord& tr){ handle_closed_trade(tr); });
//   }
//   // CBE bar update -- called at M1 bar close (in bar builder block)
//   g_cbe.on_bar(lb.open, lb.high, lb.low, lb.close,
//       g_bars_gold.m1.ind.atr14.load(std::memory_order_relaxed),
//       g_bars_gold.m1.ind.rsi14.load(std::memory_order_relaxed),
//       now_ms_g);
//   // CBE entry gate
//   if (!g_cbe.has_open_position() && gold_can_enter && !cbe_blocked) {
//       g_cbe.on_tick(bid, ask, now_ms_g,
//           g_gold_stack.ewm_drift(),
//           g_macro_ctx.gold_cvd_bear_div,
//           g_macro_ctx.gold_cvd_bull_div,
//           g_macro_ctx.session_slot,
//           [&](const omega::TradeRecord& tr){ handle_closed_trade(tr); });
//   }
// =============================================================================

#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <cstring>
#include "OmegaTradeLedger.hpp"

namespace omega {

// =============================================================================
// Config
// =============================================================================
static constexpr int     CBE_COMP_BARS         = 3;     // sweep-confirmed: 6-day 1.5M tick     // bars in compression window
static constexpr double  CBE_COMP_RANGE_MULT   = 1.50;  // sweep-confirmed
static constexpr int     CBE_COMP_MIN_BARS      = 3;    // consecutive compression bars required
static constexpr double  CBE_BREAK_FRAC        = 0.30;  // sweep-confirmed
static constexpr double  CBE_TP_RR             = 1.5;   // sweep-confirmed: rr=1.5 best on 6 days
static constexpr double  CBE_MAX_SL_ATR_MULT   = 4.0;  // wide: tick-ATR ~1.5pt, bar ranges 2-8pt
static constexpr double  CBE_TRAIL_ARM_FRAC    = 0.50;  // trail arms at 50% of TP
static constexpr double  CBE_TRAIL_DIST_FRAC   = 0.40;  // trail distance = 0.40 * ATR
static constexpr double  CBE_BE_FRAC           = 0.40;  // BE at 40% of TP distance
static constexpr int64_t CBE_TIMEOUT_MS        = 180000; // 3 min -- sweep: 180s best (58.1% WR vs 53.5% at 300s)
static constexpr int64_t CBE_COOLDOWN_MS       = 30000;  // 30s after any exit
static constexpr double  CBE_RSI_BLOCK_OB      = 72.0;  // block LONG above this RSI
static constexpr double  CBE_RSI_BLOCK_OS      = 22.0;  // sweep-confirmed
static constexpr double  CBE_COMMISSION_RT     = 0.20;  // round-trip cost (pts)
static constexpr double  CBE_RISK_DOLLARS      = 30.0;
static constexpr double  CBE_MIN_LOT           = 0.01;
static constexpr double  CBE_MAX_LOT           = 0.10;
static constexpr int64_t CBE_STARTUP_LOCKOUT_MS = 90000; // 90s post-restart warmup
// Session gate: slots 3-5 = NY session (13:00-20:00 UTC)
//               slots 1-2 = London (07:00-12:59 UTC)
// Currently blocking London/NY LONGs (0% WR in last session sweep).
// Set to false once out-of-sample validation confirms LONGs.
static constexpr bool    CBE_BLOCK_LONDON_NY_LONG = true; // confirmed: SHORT-only, LONGs 0 edge

// =============================================================================
struct CompressionBreakoutEngine {

    bool shadow_mode = true;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // ── Open position state ──────────────────────────────────────────────────
    struct OpenPos {
        bool    active         = false;
        bool    is_long        = false;
        double  entry          = 0.0;
        double  sl             = 0.0;
        double  tp             = 0.0;
        double  size           = 0.0;
        double  mfe            = 0.0;
        double  comp_range_hi  = 0.0;  // compression range high at entry
        double  comp_range_lo  = 0.0;  // compression range low at entry
        double  atr_at_entry   = 0.0;
        bool    be_done        = false;
        bool    trail_armed    = false;
        double  trail_sl       = 0.0;
        int64_t entry_ts_ms    = 0;
        int     trade_id       = 0;
    } pos;

    bool has_open_position() const noexcept { return pos.active; }

    // ── Bar update -- called at each M1 bar close ────────────────────────────
    void on_bar(double open, double high, double low, double close,
                double atr14, double rsi14, int64_t now_ms) noexcept
    {
        _atr_cur = (atr14 > 0.5) ? atr14 : _atr_cur;
        _rsi_cur = rsi14;

        // Push bar into rolling window
        _bar_hi.push_back(high);
        _bar_lo.push_back(low);
        _bar_cl.push_back(close);
        if ((int)_bar_hi.size() > CBE_COMP_BARS) { _bar_hi.pop_front(); _bar_lo.pop_front(); _bar_cl.pop_front(); }

        _bars_total++;

        // Compute rolling range over window
        if ((int)_bar_hi.size() < 2) return;

        double whi = *std::max_element(_bar_hi.begin(), _bar_hi.end());
        double wlo = *std::min_element(_bar_lo.begin(), _bar_lo.end());
        double wrange = whi - wlo;

        const double comp_thresh = _atr_cur * CBE_COMP_RANGE_MULT;
        const bool in_comp = (_atr_cur > 0.0) && (wrange < comp_thresh);

        if (in_comp) {
            _consec_comp++;
            if (_consec_comp >= CBE_COMP_MIN_BARS) {
                // Arm: record the compression range
                _armed        = true;
                _comp_hi      = whi;
                _comp_lo      = wlo;
                _comp_armed_ms = now_ms;
            }
        } else {
            _consec_comp = 0;
            // Disarm only if we had NO break yet -- if break was detected, keep it
            if (!_break_detected) {
                _armed = false;
            }
        }

        // Log compression state every 5 bars
        if (_bars_total % 5 == 0) {
            std::cout << "[CBE-STATE]"
                      << " armed=" << _armed
                      << " consec_comp=" << _consec_comp
                      << " range=" << std::fixed << std::setprecision(2) << wrange
                      << " comp_thresh=" << comp_thresh
                      << " atr=" << _atr_cur
                      << " rsi=" << std::setprecision(1) << _rsi_cur
                      << " break=" << _break_detected
                      << " shadow=" << shadow_mode
                      << "\n";
            std::cout.flush();
        }

        (void)open; (void)close;
    }

    // ── Per-tick handler ─────────────────────────────────────────────────────
    void on_tick(double bid, double ask, int64_t now_ms,
                 double ewm_drift,
                 bool cvd_bear_div,
                 bool cvd_bull_div,
                 int session_slot,
                 CloseCallback on_close) noexcept
    {
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        _daily_reset(now_ms);

        // Startup lockout
        if (_startup_ms == 0) _startup_ms = now_ms;
        if (now_ms - _startup_ms < CBE_STARTUP_LOCKOUT_MS) return;

        // Position management -- always
        if (pos.active) {
            _manage(bid, ask, mid, now_ms, ewm_drift, on_close);
            return;
        }

        // ── Entry guards ─────────────────────────────────────────────────────
        if (!_armed) return;
        if (_atr_cur <= 0.0) return;
        if (now_ms - _last_exit_ms < CBE_COOLDOWN_MS) return;
        if (spread > _atr_cur * 0.30) return;   // spread anomaly block

        // Daily loss limit
        if (_daily_pnl <= -150.0) return;

        // Session gate: slots 1-5 (London+NY), not Asia (6), not dead zone (0)
        if (session_slot < 1 || session_slot > 5) return;

        // ── Break detection ──────────────────────────────────────────────────
        const double break_margin = _atr_cur * CBE_BREAK_FRAC;
        const bool long_break  = (bid >= _comp_hi + break_margin);
        const bool short_break = (ask <= _comp_lo - break_margin);

        if (!long_break && !short_break) return;

        const bool is_long = long_break;

        // EWM drift gate: must align with break direction
        if (is_long  && ewm_drift <= 0.0) return;
        if (!is_long && ewm_drift >= 0.0) return;

        // CVD divergence block
        if (is_long  && cvd_bear_div) {
            static int64_t s_cvd_log = 0;
            if (now_ms - s_cvd_log > 10000) {
                s_cvd_log = now_ms;
                std::cout << "[CBE-CVD-BLOCK] LONG break blocked: CVD bearish div\n";
                std::cout.flush();
            }
            return;
        }
        if (!is_long && cvd_bull_div) {
            static int64_t s_cvd_log2 = 0;
            if (now_ms - s_cvd_log2 > 10000) {
                s_cvd_log2 = now_ms;
                std::cout << "[CBE-CVD-BLOCK] SHORT break blocked: CVD bullish div\n";
                std::cout.flush();
            }
            return;
        }

        // RSI exhaustion block
        if (is_long  && _rsi_cur > CBE_RSI_BLOCK_OB) {
            static int64_t s_rsi_log = 0;
            if (now_ms - s_rsi_log > 10000) {
                s_rsi_log = now_ms;
                std::cout << "[CBE-RSI-BLOCK] LONG blocked RSI=" << std::fixed << std::setprecision(1) << _rsi_cur << " > " << CBE_RSI_BLOCK_OB << "\n";
                std::cout.flush();
            }
            return;
        }
        if (!is_long && _rsi_cur < CBE_RSI_BLOCK_OS) {
            static int64_t s_rsi_log2 = 0;
            if (now_ms - s_rsi_log2 > 10000) {
                s_rsi_log2 = now_ms;
                std::cout << "[CBE-RSI-BLOCK] SHORT blocked RSI=" << std::fixed << std::setprecision(1) << _rsi_cur << " < " << CBE_RSI_BLOCK_OS << "\n";
                std::cout.flush();
            }
            return;
        }

        // London/NY LONG block (sweep result: 0% WR for LONGs in this window)
        // slots 1-5 = 07:00-20:00 UTC = London + NY
        if (CBE_BLOCK_LONDON_NY_LONG && is_long && session_slot >= 1 && session_slot <= 5) {
            static int64_t s_lon_log = 0;
            if (now_ms - s_lon_log > 30000) {
                s_lon_log = now_ms;
                std::cout << "[CBE-LON-BLOCK] LONG blocked: London/NY LONG gate active (0% WR in sweep)\n";
                std::cout.flush();
            }
            return;
        }

        // ── SL geometry ─────────────────────────────────────────────────────
        // Structural SL: below comp range (LONG) or above comp range (SHORT)
        // Add 0.5pt buffer so normal volatility doesn't immediately nip SL
        const double sl_buffer = 0.50;
        const double sl_dist = is_long
            ? (ask - (_comp_lo - sl_buffer))    // ask - SL_price
            : ((_comp_hi + sl_buffer) - bid);   // SL_price - bid

        // SL too wide check
        if (sl_dist > _atr_cur * CBE_MAX_SL_ATR_MULT) {
            static int64_t s_sl_log = 0;
            if (now_ms - s_sl_log > 10000) {
                s_sl_log = now_ms;
                std::cout << "[CBE-SL-BLOCK] SL dist=" << std::fixed << std::setprecision(2) << sl_dist
                          << " > " << _atr_cur * CBE_MAX_SL_ATR_MULT << " (atr*" << CBE_MAX_SL_ATR_MULT << ") -- skip\n";
                std::cout.flush();
            }
            return;
        }
        if (sl_dist <= 0.0) return;

        // Cost coverage: TP must exceed round-trip cost
        const double tp_pts = sl_dist * CBE_TP_RR;
        const double cost   = spread + CBE_COMMISSION_RT;
        if (tp_pts <= cost) {
            static int64_t s_cost_log = 0;
            if (now_ms - s_cost_log > 10000) {
                s_cost_log = now_ms;
                std::cout << "[CBE-COST-BLOCK] tp=" << tp_pts << " <= cost=" << cost << "\n";
                std::cout.flush();
            }
            return;
        }

        // ── Enter ────────────────────────────────────────────────────────────
        _enter(is_long, bid, ask, spread, sl_dist, tp_pts, now_ms, on_close);
    }

    void force_close(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (!pos.active) return;
        _close(pos.is_long ? bid : ask, "FORCE_CLOSE", now_ms, cb);
    }

private:
    // ── Bar state ────────────────────────────────────────────────────────────
    std::deque<double> _bar_hi, _bar_lo, _bar_cl;
    int    _consec_comp    = 0;
    bool   _armed          = false;
    bool   _break_detected = false;
    double _comp_hi        = 0.0;
    double _comp_lo        = 0.0;
    int64_t _comp_armed_ms = 0;
    double _atr_cur        = 0.0;
    double _rsi_cur        = 50.0;
    int64_t _bars_total    = 0;

    // ── Session state ────────────────────────────────────────────────────────
    double  _daily_pnl     = 0.0;
    int64_t _daily_day     = 0;
    int64_t _last_exit_ms  = 0;
    int     _consec_losses = 0;
    int     _total_trades  = 0;
    int     _total_wins    = 0;
    int     _trade_id      = 0;
    int64_t _startup_ms    = 0;

    // ── Entry ─────────────────────────────────────────────────────────────────
    void _enter(bool is_long, double bid, double ask, double spread,
                double sl_dist, double tp_pts,
                int64_t now_ms, CloseCallback on_close) noexcept
    {
        const double entry_px = is_long ? ask : bid;
        const double sl_px    = is_long ? (entry_px - sl_dist) : (entry_px + sl_dist);
        const double tp_px    = is_long ? (entry_px + tp_pts)  : (entry_px - tp_pts);

        const double sl_safe = std::max(0.10, sl_dist);
        double size = CBE_RISK_DOLLARS / (sl_safe * 100.0);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(CBE_MIN_LOT, std::min(CBE_MAX_LOT, size));

        ++_trade_id;

        std::cout << "[CBE] " << (is_long ? "LONG" : "SHORT")
                  << " entry=" << std::fixed << std::setprecision(2) << entry_px
                  << " sl=" << sl_px << " tp=" << tp_px
                  << " size=" << std::setprecision(3) << size
                  << " sl_dist=" << std::setprecision(2) << sl_dist
                  << " tp_pts=" << tp_pts
                  << " spread=" << spread
                  << " atr=" << _atr_cur
                  << " rsi=" << std::setprecision(1) << _rsi_cur
                  << " comp_hi=" << std::setprecision(2) << _comp_hi
                  << " comp_lo=" << _comp_lo
                  << (shadow_mode ? " [SHADOW]" : "")
                  << "\n";
        std::cout.flush();

        pos.active        = true;
        pos.is_long       = is_long;
        pos.entry         = entry_px;
        pos.sl            = sl_px;
        pos.tp            = tp_px;
        pos.size          = size;
        pos.mfe           = 0.0;
        pos.comp_range_hi = _comp_hi;
        pos.comp_range_lo = _comp_lo;
        pos.atr_at_entry  = _atr_cur;
        pos.be_done       = false;
        pos.trail_armed   = false;
        pos.trail_sl      = sl_px;
        pos.entry_ts_ms   = now_ms;
        pos.trade_id      = _trade_id;

        // Reset armed state after entry -- require new compression to re-arm
        _armed          = false;
        _break_detected = true;

        (void)on_close;
    }

    // ── Manage ────────────────────────────────────────────────────────────────
    void _manage(double bid, double ask, double mid,
                 int64_t now_ms, double /*ewm_drift*/,
                 CloseCallback on_close) noexcept
    {
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;

        const double eff_price = pos.is_long ? bid : ask;
        const double tp_dist   = std::fabs(pos.tp - pos.entry);

        // Breakeven
        if (!pos.be_done && tp_dist > 0.0 && pos.mfe >= tp_dist * CBE_BE_FRAC) {
            pos.sl      = pos.entry;
            pos.trail_sl = pos.entry;
            pos.be_done  = true;
            std::cout << "[CBE-BE] " << (pos.is_long ? "LONG" : "SHORT")
                      << " entry=" << std::fixed << std::setprecision(2) << pos.entry
                      << " sl->BE mfe=" << std::setprecision(3) << pos.mfe << "\n";
            std::cout.flush();
        }

        // Trail
        if (!pos.trail_armed && tp_dist > 0.0 && pos.mfe >= tp_dist * CBE_TRAIL_ARM_FRAC) {
            pos.trail_armed = true;
            std::cout << "[CBE-TRAIL-ARM] " << (pos.is_long ? "LONG" : "SHORT")
                      << " mfe=" << std::fixed << std::setprecision(3) << pos.mfe << "\n";
            std::cout.flush();
        }
        if (pos.trail_armed) {
            const double trail_dist = pos.atr_at_entry * CBE_TRAIL_DIST_FRAC;
            const double new_trail  = pos.is_long ? (mid - trail_dist) : (mid + trail_dist);
            if (pos.is_long  && new_trail > pos.trail_sl) pos.trail_sl = new_trail;
            if (!pos.is_long && new_trail < pos.trail_sl) pos.trail_sl = new_trail;
            if (pos.is_long  && pos.trail_sl > pos.sl) pos.sl = pos.trail_sl;
            if (!pos.is_long && pos.trail_sl < pos.sl) pos.sl = pos.trail_sl;
        }

        // Re-entry into compression range: thesis failed, exit
        const bool reenter_comp = pos.is_long
            ? (bid < pos.comp_range_hi - 0.10)
            : (ask > pos.comp_range_lo + 0.10);
        if (reenter_comp && pos.mfe < 0.0 && (now_ms - pos.entry_ts_ms) > 5000) {
            _close(eff_price, "REENTER_COMP", now_ms, on_close);
            return;
        }

        // TP
        if (pos.is_long ? (bid >= pos.tp) : (ask <= pos.tp)) {
            _close(eff_price, "TP_HIT", now_ms, on_close);
            return;
        }

        // SL / trail SL
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (sl_hit) {
            const char* reason = pos.trail_armed ? "TRAIL_HIT"
                               : pos.be_done     ? "BE_HIT"
                               :                   "SL_HIT";
            _close(eff_price, reason, now_ms, on_close);
            return;
        }

        // Timeout
        if (now_ms - pos.entry_ts_ms > CBE_TIMEOUT_MS) {
            _close(eff_price, "TIMEOUT", now_ms, on_close);
            return;
        }
    }

    // ── Close ─────────────────────────────────────────────────────────────────
    void _close(double exit_px, const char* reason,
                int64_t now_ms, CloseCallback on_close) noexcept
    {
        const double pnl_pts = pos.is_long
            ? (exit_px - pos.entry)
            : (pos.entry - exit_px);
        const double pnl_usd = pnl_pts * pos.size * 100.0;

        _daily_pnl    += pnl_usd;
        _last_exit_ms  = now_ms;
        ++_total_trades;
        const bool win = (pnl_usd > 0);
        if (win) { ++_total_wins; _consec_losses = 0; }
        else     { ++_consec_losses; }

        std::cout << "[CBE] EXIT " << (pos.is_long ? "LONG" : "SHORT")
                  << " @ " << std::fixed << std::setprecision(2) << exit_px
                  << " " << reason
                  << " pnl=$" << std::setprecision(2) << pnl_usd
                  << " mfe=" << std::setprecision(2) << pos.mfe
                  << " held=" << (now_ms - pos.entry_ts_ms) / 1000LL << "s"
                  << " daily=$" << _daily_pnl
                  << " W/T=" << _total_wins << "/" << _total_trades
                  << (shadow_mode ? " [SHADOW]" : "")
                  << "\n";
        std::cout.flush();

        if (on_close) {
            omega::TradeRecord tr;
            tr.id         = pos.trade_id;
            tr.symbol     = "XAUUSD";
            tr.side       = pos.is_long ? "LONG" : "SHORT";
            tr.engine     = "CompBreakout";
            tr.entryPrice = pos.entry;
            tr.exitPrice  = exit_px;
            tr.sl         = pos.sl;
            tr.size       = pos.size;
            tr.pnl        = pnl_pts * pos.size;
            tr.mfe        = pos.mfe;
            tr.mae        = 0.0;
            tr.entryTs    = pos.entry_ts_ms / 1000;
            tr.exitTs     = now_ms / 1000;
            tr.exitReason = reason;
            tr.regime     = "CBE";
            tr.l2_live    = true;
            tr.shadow     = shadow_mode;
            on_close(tr);
        }

        // After close: disarm break state
        _break_detected = false;
        pos = OpenPos{};
    }

    // ── Daily reset ───────────────────────────────────────────────────────────
    void _daily_reset(int64_t now_ms) noexcept {
        const int64_t day = (now_ms / 1000LL) / 86400LL;
        if (day != _daily_day) {
            if (_daily_day > 0)
                std::cout << "[CBE] Daily reset. PnL=$" << std::fixed << std::setprecision(2) << _daily_pnl
                          << " W/T=" << _total_wins << "/" << _total_trades
                          << " WR=" << (int)(_total_trades > 0 ? 100.0 * _total_wins / _total_trades : 0.0) << "%\n";
            _daily_pnl = 0.0;
            _daily_day = day;
        }
    }
};

} // namespace omega

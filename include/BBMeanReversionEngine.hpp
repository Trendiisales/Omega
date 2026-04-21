// =============================================================================
// BBMeanReversionEngine.hpp -- Bollinger Band Mean Reversion for XAUUSD
//
// STRATEGY (sweep + diagnostic confirmed, 1.86M tick backtest 2026-04-14..16):
//   M1 bar closes outside Bollinger Band (25-period, 2.0 SD).
//   RSI confirms overextension. Price snaps back to BB midline.
//   Both LONG and SHORT -- but LONG restricted to London session.
//
// BEST CONFIG (bb_tuned_sweep.cpp, WR-ranked, T>=10):
//   sess=L+NY  wed=Y  llong=Y  ov=0.20  srsi=75  lrsi=27  hk=Agg
//   T=22  WR=68.2%  PnL=$594  Avg=$27.03  MaxDD=$46.80
//
// CONFIRMED PARAMETERS:
//   BB period=25, BB std=2.0
//   Session: London (07-12 UTC) + NY (13-19 UTC) only
//   Kill Wednesday (6.2% WR in diagnostic, structural dead day)
//   LONG only in London (07-12 UTC): LONG in Asia/NY loses $-234 total
//   Max overshoot=0.20: barely-outside entries win; extreme overshoot = trend
//   Short RSI minimum=75: RSI>80 bucket best (32.7% WR); 75-80 acceptable
//   Long RSI maximum=27: RSI<20 bucket negative ($-74); 20-27 is the sweet spot
//   Hour kill aggressive: H02, H03, H13, H15, H22 all 0-12% WR -- killed
//   SL=1.0x bar range, TMO=300s, BE at 40% MFE, cooldown=30s
//
// ENTRY:
//   SHORT: M1 bar closes ABOVE upper BB, bar is bearish (close < open not
//          required -- we just need the RSI gate), RSI >= 75, overshoot <= 0.20
//   LONG:  M1 bar closes BELOW lower BB, RSI <= 27, overshoot <= 0.20,
//          London session only (H07-H12 UTC)
//
// SL:  bar_high + bar_range * 0.5 (SHORT) / bar_low - bar_range * 0.5 (LONG)
// TP:  BB midline (SMA25) -- mean reversion target
// BE:  SL moves to entry when MFE >= 40% of TP distance
// TMO: 300 seconds
//
// FILTER CHAIN (in order):
//   1. Startup warmup 120s
//   2. Daily DD limit $200
//   3. Wednesday kill
//   4. Session gate (London + NY only)
//   5. Hour kill (H02, H03, H13, H15, H22)
//   6. LONG London-only gate (kills LONG outside H07-H12)
//   7. BB width >= 0.5 (band exists)
//   8. Spread <= ATR * 0.25
//   9. Bar range sanity (0.05 to ATR*3)
//  10. Bar closes outside band
//  11. RSI threshold (SHORT >= 75, LONG <= 27)
//  12. Overshoot cap 0.20 (avoids trend-continuation false setups)
//  13. Min RR 0.5 (TP must be at least half of SL distance)
//  14. Cost coverage (spread + commission)
//  15. Cooldown 30s since last exit
//
// WIRING (tick_gold.hpp):
//   // Declare at global scope:
//   #include "BBMeanReversionEngine.hpp"
//   omega::BBMeanReversionEngine g_bb_mr;
//   // (set g_bb_mr.shadow_mode = true; at startup)
//
//   // In M1 bar-close block (alongside g_ema_cross.on_bar):
//   g_bb_mr.on_bar(s_cur1.close,
//       g_bars_gold.m1.ind.atr14.load(std::memory_order_relaxed),
//       now_ms_g);
//
//   // In tick dispatch (after existing engines):
//   if (g_bb_mr.has_open_position()) {
//       g_bb_mr.on_tick(bid, ask, now_ms_g,
//           [&](const omega::TradeRecord& tr){ handle_closed_trade(tr); });
//   }
//   if (!g_bb_mr.has_open_position()
//       && gold_can_enter
//       && !g_ema_cross.has_open_position()
//       && !g_candle_flow.has_open_position()
//       && !g_cbe.has_open_position()
//       && !g_gold_flow.has_open_position()) {
//       g_bb_mr.on_tick(bid, ask, now_ms_g,
//           [&](const omega::TradeRecord& tr) {
//               handle_closed_trade(tr);
//               if (!g_bb_mr.shadow_mode)
//                   send_live_order("XAUUSD", !tr.side.empty() && tr.side=="SHORT",
//                       tr.size, tr.exitPrice);
//           });
//       if (g_bb_mr.has_open_position()) {
//           g_telemetry.UpdateLastEntryTs();
//           write_trade_open_log("XAUUSD", "BBMeanRev",
//               g_bb_mr.pos.is_long ? "LONG" : "SHORT",
//               g_bb_mr.pos.entry, g_bb_mr.pos.tp, g_bb_mr.pos.sl,
//               g_bb_mr.pos.size, ask - bid, "BB_OUTSIDE", "BB25_2SD");
//       }
//   }
// =============================================================================

#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>      // FIX BBMR-MUTEX 2026-04-21: serialize close paths (force_close vs on_tick SL/TP/TIMEOUT)
#include <cstring>    // FIX BBMR-SL-KILL 2026-04-21: strcmp for consec-SL tracking
#include "OmegaTradeLedger.hpp"

namespace omega {

// =============================================================================
// Config -- bb_tuned_sweep.cpp confirmed 2026-04-17
//
// Baseline (no filters):  T=151  WR=29.8%  PnL=$292   Avg=$1.94  MaxDD=$261
// Tuned (best WR config): T=22   WR=68.2%  PnL=$594   Avg=$27.03 MaxDD=$47
// Improvement: 2x PnL, 82% MaxDD reduction, WR 29.8% -> 68.2%
//
// Active filters confirmed by diagnostic + tuned sweep:
//   session L+NY   : London (H07-H12) + NY (H13-H19) only
//   kill_wed       : Wednesday 6.2% WR -- structural dead day
//   long_london    : LONG only H07-H12; LONG in Asia/NY loses $-234
//   max_overshoot  : 0.20 -- barely-outside entries win; extreme = trend trap
//   short_rsi_min  : 75 -- concentrates on RSI>75 bucket (best WR)
//   long_rsi_max   : 27 -- RSI<20 bucket negative; 20-27 is the sweet spot
//   hour_kill_agg  : H02,H03,H13,H15,H22 all 0-12% WR in diagnostic
// =============================================================================

static constexpr int     BBMR_BB_PERIOD       = 25;
static constexpr double  BBMR_BB_STD          = 2.0;
static constexpr double  BBMR_SL_MULT         = 1.0;   // SL = bar_range * 1.0 * 0.5
static constexpr int64_t BBMR_TIMEOUT_MS      = 300000; // 5 minutes
static constexpr int64_t BBMR_COOLDOWN_MS     = 30000;  // 30s between entries
static constexpr int64_t BBMR_STARTUP_MS      = 120000; // 2 min warmup
static constexpr double  BBMR_SHORT_RSI_MIN   = 75.0;   // short: RSI must be >= this
static constexpr double  BBMR_LONG_RSI_MAX    = 27.0;   // long:  RSI must be <= this
static constexpr double  BBMR_MAX_OVERSHOOT   = 0.20;   // cap on band overshoot
static constexpr double  BBMR_MIN_BB_WIDTH    = 0.50;   // band must exist
static constexpr double  BBMR_MIN_RR          = 0.50;   // TP dist >= SL dist * 0.5
static constexpr double  BBMR_RISK_DOLLARS    = 30.0;
static constexpr double  BBMR_MIN_LOT         = 0.01;
static constexpr double  BBMR_MAX_LOT         = 0.01;  // FIX 2026-04-21 uniformity: all engines capped at 0.01 until SHADOW validated
static constexpr double  BBMR_BE_FRAC         = 0.40;   // BE at 40% MFE
static constexpr double  BBMR_DAILY_DD_LIMIT  = 200.0;
static constexpr double  BBMR_COMMISSION      = 0.20;   // per side

// Aggressive hour kill: H02, H03, H13, H15, H22
// Diagnostic WR: H02=11%, H03=12.5%, H13=10%, H15=0%, H22=16.7%
static inline bool bbmr_hour_allowed(int h) noexcept {
    if (h == 2)  return false;
    if (h == 3)  return false;
    if (h == 13) return false;
    if (h == 15) return false;
    if (h == 22) return false;
    return true;
}

// Session gate: London (07-12) + NY (13-19)
// Note: H13 is killed by hour_kill above, so effective NY = H14,H16,H17,H18,H19
static inline bool bbmr_session_allowed(int h) noexcept {
    return (h >= 7 && h < 20);
}

// London session check: H07-H12 inclusive
static inline bool bbmr_in_london(int h) noexcept {
    return (h >= 7 && h < 13);
}

static inline int bbmr_utc_hour(int64_t ms) noexcept {
    return (int)(((ms / 1000LL) % 86400LL) / 3600LL);
}

static inline int bbmr_dow(int64_t ms) noexcept {
    return (int)((ms / 1000LL / 86400LL + 4) % 7); // 0=Sun, 3=Wed
}

// =============================================================================
// BBMeanReversionEngine
// =============================================================================
class BBMeanReversionEngine {
public:
    bool shadow_mode = true;

    // ── Open position (public for logging access) ─────────────────────────────
    struct OpenPos {
        bool     active   = false;
        bool     is_long  = false;
        bool     be_done  = false;
        double   entry    = 0.0;
        double   sl       = 0.0;
        double   tp       = 0.0;
        double   size     = 0.0;
        double   mfe      = 0.0;
        int64_t  ets      = 0;
        int      trade_id = 0;
        double   entry_rsi= 0.0;
        double   bb_mid   = 0.0;
        double   overshoot= 0.0;
        double   spread_at_entry = 0.0;  // bid-ask spread captured at entry (ledger forensics)
    } pos;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // ── on_bar: called at M1 bar close ────────────────────────────────────────
    // Must be called with bar close price and ATR before on_tick
    void on_bar(double bar_close, double bar_atr, int64_t now_ms) noexcept {
        // RSI bar-level update
        if (_rsi_prev > 0.0) {
            double chg = bar_close - _rsi_prev;
            _rsi_gains.push_back(chg > 0 ? chg : 0.0);
            _rsi_losses.push_back(chg < 0 ? -chg : 0.0);
            if ((int)_rsi_gains.size() > 14) { _rsi_gains.pop_front(); _rsi_losses.pop_front(); }
            if ((int)_rsi_gains.size() == 14) {
                double ag = 0, al = 0;
                for (auto x : _rsi_gains)  ag += x;
                for (auto x : _rsi_losses) al += x;
                ag /= 14.0; al /= 14.0;
                _rsi = (al == 0.0) ? 100.0 : 100.0 - 100.0 / (1.0 + ag / al);
            }
        }
        _rsi_prev = bar_close;

        // ATR
        if (bar_atr > 0.5 && bar_atr < 50.0) _atr = bar_atr;

        // BB update on bar close
        _bb_closes.push_back(bar_close);
        if ((int)_bb_closes.size() > BBMR_BB_PERIOD) _bb_closes.pop_front();
        if ((int)_bb_closes.size() == BBMR_BB_PERIOD) {
            double sum = 0.0;
            for (auto x : _bb_closes) sum += x;
            _bb_mid = sum / BBMR_BB_PERIOD;
            double var = 0.0;
            for (auto x : _bb_closes) var += (x - _bb_mid) * (x - _bb_mid);
            double sd = std::sqrt(var / BBMR_BB_PERIOD);
            _bb_upper = _bb_mid + BBMR_BB_STD * sd;
            _bb_lower = _bb_mid - BBMR_BB_STD * sd;
            _bb_width = _bb_upper - _bb_lower;
            _bb_ready = true;
        }

        // Store last closed bar for entry checks
        _last_bar_close = bar_close;
        _last_bar_high  = bar_close; // updated tick-by-tick below
        _last_bar_low   = bar_close;
        _bar_open       = bar_close; // approximate -- we use tick high/low
        _new_bar_ms     = now_ms;
        _bar_has_entry_candidate = _bb_ready && _atr > 0.0;

        (void)now_ms;
    }

    // ── on_tick: called every tick ────────────────────────────────────────────
    // Handles both position management and entry detection
    void on_tick(double bid, double ask, int64_t now_ms,
                 CloseCallback on_close = nullptr) noexcept
    {
        if (_startup_ms == 0) _startup_ms = now_ms;
        if (now_ms - _startup_ms < BBMR_STARTUP_MS) return;

        _daily_reset(now_ms);
        if (_daily_pnl <= -BBMR_DAILY_DD_LIMIT) return;

        double mid    = (bid + ask) * 0.5;
        double spread = ask - bid;

        // Track intra-bar high/low for bar range at entry
        if (mid > _intrabar_high) _intrabar_high = mid;
        if (mid < _intrabar_low || _intrabar_low == 0.0) _intrabar_low = mid;

        // ── Position management ───────────────────────────────────────────────
        if (pos.active) {
            double mv = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
            if (mv > pos.mfe) pos.mfe = mv;

            // BE: move SL to entry when MFE >= 40% of TP distance
            double td = std::fabs(pos.tp - pos.entry);
            if (!pos.be_done && td > 0.0 && pos.mfe >= td * BBMR_BE_FRAC) {
                pos.sl = pos.entry;
                pos.be_done = true;
            }

            double ep = pos.is_long ? bid : ask;

            // TP
            if (pos.is_long ? (bid >= pos.tp) : (ask <= pos.tp)) {
                _close(ep, "TP", now_ms, on_close); return;
            }
            // SL / BE
            if (pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl)) {
                _close(ep, pos.be_done ? "BE" : "SL", now_ms, on_close); return;
            }
            // Timeout
            if (now_ms - pos.ets > BBMR_TIMEOUT_MS) {
                _close(ep, "TIMEOUT", now_ms, on_close); return;
            }
            return;
        }

        // ── Entry checks ──────────────────────────────────────────────────────
        // FIX BBMR-SL-KILL 2026-04-21: consecutive-SL kill switch (mirror of ECE
        // and CBE-10 pattern). After 3 consecutive SL exits, block entries for
        // 30 minutes. Death-spiral circuit breaker: 3 SLs at the same BB-reversion
        // level means the regime has changed (trend-day) and re-entering keeps
        // paying spread+slippage on doomed trades.
        if (_consec_sl >= 3 && now_ms < _sl_kill_until) {
            static int64_t s_bbmr_kill_log = 0;
            if (now_ms - s_bbmr_kill_log > 60000) {
                s_bbmr_kill_log = now_ms;
                std::cout << "[BBMR-SL-KILL] " << _consec_sl
                          << " consec SLs, blocking entries for "
                          << ((_sl_kill_until - now_ms) / 1000LL) << "s more\n";
                std::cout.flush();
            }
            return;
        }
        if (_consec_sl >= 3 && now_ms >= _sl_kill_until) {
            std::cout << "[BBMR-SL-KILL] Reset after 30min, "
                      << _consec_sl << " SLs cleared\n";
            std::cout.flush();
            _consec_sl = 0;
        }

        if (!_bar_has_entry_candidate) return;
        if (_bb_width < BBMR_MIN_BB_WIDTH) return;
        if (_atr <= 0.0) return;
        if (spread > _atr * 0.25) return;
        if (now_ms / 1000LL - _last_exit_s < BBMR_COOLDOWN_MS / 1000LL) return;

        int h   = bbmr_utc_hour(now_ms);
        int dow = bbmr_dow(now_ms);

        // Wednesday kill (diagnostic: 6.2% WR, -$212 on 16 trades)
        if (dow == 3) return;

        // Session gate (London + NY only)
        if (!bbmr_session_allowed(h)) return;

        // Aggressive hour kill
        if (!bbmr_hour_allowed(h)) return;

        // Bar range sanity (use BB-period ATR)
        double bar_range = _intrabar_high - _intrabar_low;
        if (bar_range < 0.05) return;
        if (bar_range > _atr * 3.0) return;

        // ── SHORT: bar close above upper BB ───────────────────────────────────
        if (_last_bar_close > _bb_upper) {
            // RSI gate
            if (_rsi < BBMR_SHORT_RSI_MIN) goto check_long;

            // Overshoot cap (avoids trend-continuation traps)
            {
                double half = _bb_width / 2.0;
                double ov   = (half > 0.0) ? (_last_bar_close - _bb_upper) / half : 0.0;
                if (ov > BBMR_MAX_OVERSHOOT) goto check_long;

                // SL above bar high
                double sl_px   = _intrabar_high + bar_range * BBMR_SL_MULT * 0.5;
                double sl_dist = std::fabs(bid - sl_px);
                if (sl_dist <= 0.0) goto check_long;

                // TP at BB midline
                double tp_px   = _bb_mid;
                double tp_dist = bid - tp_px;
                if (tp_dist < sl_dist * BBMR_MIN_RR) goto check_long;

                double cost = spread + BBMR_COMMISSION;
                if (tp_dist <= cost) goto check_long;

                _enter(false, bid, ask, sl_px, tp_px, sl_dist, ov, now_ms, on_close);
                return;
            }
        }

        check_long:
        // ── LONG: bar close below lower BB ────────────────────────────────────
        if (_last_bar_close < _bb_lower) {
            // LONG restricted to London session only
            // Diagnostic: LONG in London = $+199, LONG in Asia = $-140, LONG in NY = $-94
            if (!bbmr_in_london(h)) return;

            // RSI gate
            if (_rsi > BBMR_LONG_RSI_MAX) return;

            // Overshoot cap
            double half = _bb_width / 2.0;
            double ov   = (half > 0.0) ? (_bb_lower - _last_bar_close) / half : 0.0;
            if (ov > BBMR_MAX_OVERSHOOT) return;

            // SL below bar low
            double sl_px   = _intrabar_low - bar_range * BBMR_SL_MULT * 0.5;
            double sl_dist = std::fabs(ask - sl_px);
            if (sl_dist <= 0.0) return;

            // TP at BB midline
            double tp_px   = _bb_mid;
            double tp_dist = tp_px - ask;
            if (tp_dist < sl_dist * BBMR_MIN_RR) return;

            double cost = spread + BBMR_COMMISSION;
            if (tp_dist <= cost) return;

            _enter(true, bid, ask, sl_px, tp_px, sl_dist, ov, now_ms, on_close);
        }
    }

    void force_close(double bid, double ask, int64_t now_ms,
                     CloseCallback cb) noexcept {
        // FIX BBMR-MUTEX 2026-04-21: serialize against concurrent _close path from
        // on_tick (TP / SL / BE / TIMEOUT). tick_gold.hpp may invoke force_close
        // (session-end / daily-kill path) while a tick-driven on_tick is mid-flight
        // on the same engine -- without this lock both paths could observe
        // pos.active==true and both invoke the close body, emitting a duplicate
        // TradeRecord with the same trade_id. Mirror of CandleFlow RACE-FIX and
        // CBE-8 pattern.
        std::lock_guard<std::mutex> lk(_close_mtx);
        if (!pos.active) return;
        _close_locked(pos.is_long ? bid : ask, "FORCE_CLOSE", now_ms, cb);
    }

    bool has_open_position() const noexcept { return pos.active; }

    // Intra-bar high/low reset -- call at M1 bar close (same time as on_bar)
    void reset_intrabar() noexcept {
        _intrabar_high = 0.0;
        _intrabar_low  = 0.0;
    }

private:
    // FIX BBMR-MUTEX 2026-04-21: race guard for _close / force_close.
    // mutable so const-qualified methods could also acquire if needed.
    mutable std::mutex _close_mtx;

    // FIX BBMR-SL-KILL 2026-04-21: consecutive-SL kill state.
    int     _consec_sl     = 0;    // consecutive SL counter
    int64_t _sl_kill_until = 0;    // block entries until this ms (set on 3rd SL)

    // ── BB state ──────────────────────────────────────────────────────────────
    std::deque<double> _bb_closes;
    double  _bb_mid    = 0.0;
    double  _bb_upper  = 0.0;
    double  _bb_lower  = 0.0;
    double  _bb_width  = 0.0;
    bool    _bb_ready  = false;

    // ── RSI (bar-level) ───────────────────────────────────────────────────────
    std::deque<double> _rsi_gains;
    std::deque<double> _rsi_losses;
    double  _rsi_prev  = 0.0;
    double  _rsi       = 50.0;

    // ── ATR / bar state ───────────────────────────────────────────────────────
    double  _atr             = 0.0;
    double  _last_bar_close  = 0.0;
    double  _last_bar_high   = 0.0;
    double  _last_bar_low    = 0.0;
    double  _bar_open        = 0.0;
    double  _intrabar_high   = 0.0;
    double  _intrabar_low    = 0.0;
    int64_t _new_bar_ms      = 0;
    bool    _bar_has_entry_candidate = false;

    // ── Session state ─────────────────────────────────────────────────────────
    double  _daily_pnl   = 0.0;
    int64_t _daily_day   = 0;
    int64_t _last_exit_s = 0;
    int64_t _startup_ms  = 0;
    int     _total_trades= 0;
    int     _total_wins  = 0;
    int     _trade_id    = 0;

    // ── Entry ─────────────────────────────────────────────────────────────────
    void _enter(bool is_long, double bid, double ask,
                double sl_px, double tp_px, double sl_dist, double overshoot,
                int64_t now_ms, CloseCallback on_close) noexcept
    {
        double entry_px = is_long ? ask : bid;

        double sl_safe = std::max(0.10, sl_dist);
        double size = BBMR_RISK_DOLLARS / (sl_safe * 100.0);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(BBMR_MIN_LOT, std::min(BBMR_MAX_LOT, size));

        ++_trade_id;

        std::cout << "[BBMR] " << (is_long ? "LONG" : "SHORT")
                  << " entry=" << std::fixed << std::setprecision(2) << entry_px
                  << " sl=" << sl_px
                  << " tp=" << tp_px
                  << " size=" << std::setprecision(3) << size
                  << " rsi=" << std::setprecision(1) << _rsi
                  << " bb_mid=" << std::setprecision(2) << _bb_mid
                  << " ov=" << std::setprecision(3) << overshoot
                  << " atr=" << std::setprecision(2) << _atr
                  << " spread=" << (ask - bid)
                  << (shadow_mode ? " [SHADOW]" : "") << "\n";
        std::cout.flush();

        pos.active    = true;
        pos.is_long   = is_long;
        pos.entry     = entry_px;
        pos.sl        = sl_px;
        pos.tp        = tp_px;
        pos.size      = size;
        pos.mfe       = 0.0;
        pos.be_done   = false;
        pos.ets       = now_ms;
        pos.trade_id  = _trade_id;
        pos.entry_rsi = _rsi;
        pos.bb_mid    = _bb_mid;
        pos.overshoot = overshoot;
        pos.spread_at_entry = ask - bid;

        (void)on_close;
    }

    // ── Close ─────────────────────────────────────────────────────────────────
    // FIX BBMR-MUTEX 2026-04-21: public _close acquires mutex and double-checks
    // pos.active. All internal call sites (TP, SL/BE, TIMEOUT) now go through
    // here. force_close uses _close_locked directly because it already holds
    // the lock.
    void _close(double exit_px, const char* reason,
                int64_t now_ms, CloseCallback on_close) noexcept
    {
        std::lock_guard<std::mutex> lk(_close_mtx);
        if (!pos.active) return;
        _close_locked(exit_px, reason, now_ms, on_close);
    }

    // Internal close path -- MUST be called with _close_mtx held AND pos.active==true.
    // Body is unchanged from pre-2026-04-21 _close, plus FIX BBMR-SL-KILL 2026-04-21
    // consec-SL tracking.
    void _close_locked(double exit_px, const char* reason,
                       int64_t now_ms, CloseCallback on_close) noexcept
    {
        double pnl_pts = pos.is_long
            ? (exit_px - pos.entry) : (pos.entry - exit_px);
        double pnl_usd = pnl_pts * pos.size * 100.0;

        _daily_pnl    += pnl_usd;
        _last_exit_s   = now_ms / 1000LL;
        ++_total_trades;
        if (pnl_usd > 0.0) ++_total_wins;

        // FIX BBMR-SL-KILL 2026-04-21: consecutive-SL tracker. Increment on true SL
        // exits only (not BE / TP / TIMEOUT / FORCE_CLOSE). On 3rd consecutive SL,
        // arm the 30-min entry block. Any non-SL exit resets the counter.
        if (std::strcmp(reason, "SL") == 0) {
            if (++_consec_sl >= 3) {
                _sl_kill_until = now_ms + 1800000LL;   // 30 minutes
                std::cout << "[BBMR-SL-KILL] " << _consec_sl
                          << " consec SLs -- blocking entries for 30min\n";
                std::cout.flush();
            }
        } else {
            _consec_sl = 0;
        }

        std::cout << "[BBMR] EXIT " << (pos.is_long ? "LONG" : "SHORT")
                  << " @ " << std::fixed << std::setprecision(2) << exit_px
                  << " " << reason
                  << " pnl=$" << std::setprecision(2) << pnl_usd
                  << " mfe=" << std::setprecision(2) << pos.mfe
                  << " held=" << (now_ms - pos.ets) / 1000LL << "s"
                  << " daily=$" << _daily_pnl
                  << " W/T=" << _total_wins << "/" << _total_trades
                  << " WR=" << (int)(_total_trades > 0
                      ? 100.0 * _total_wins / _total_trades : 0.0) << "%"
                  << (shadow_mode ? " [SHADOW]" : "") << "\n";
        std::cout.flush();

        if (on_close) {
            omega::TradeRecord tr;
            tr.id         = pos.trade_id;
            tr.symbol     = "XAUUSD";
            tr.side       = pos.is_long ? "LONG" : "SHORT";
            tr.engine     = "BBMeanRev";
            tr.entryPrice = pos.entry;
            tr.exitPrice  = exit_px;
            tr.sl         = pos.sl;
            tr.size       = pos.size;
            tr.pnl        = pnl_pts * pos.size;
            tr.mfe        = pos.mfe;
            tr.mae        = 0.0;
            tr.entryTs    = pos.ets / 1000LL;
            tr.exitTs     = now_ms / 1000LL;
            tr.exitReason = reason;
            tr.regime     = "BB_MEAN_REV";
            tr.l2_live    = true;
            tr.shadow     = shadow_mode;
            tr.spreadAtEntry = pos.spread_at_entry;
            on_close(tr);
        }

        pos = OpenPos{};
    }

    // ── Daily reset ───────────────────────────────────────────────────────────
    void _daily_reset(int64_t now_ms) noexcept {
        const int64_t day = (now_ms / 1000LL) / 86400LL;
        if (day != _daily_day) {
            if (_daily_day > 0)
                std::cout << "[BBMR] Daily reset PnL=$"
                          << std::fixed << std::setprecision(2) << _daily_pnl
                          << " W/T=" << _total_wins << "/" << _total_trades
                          << " WR=" << (int)(_total_trades > 0
                              ? 100.0 * _total_wins / _total_trades : 0.0) << "%\n";
            _daily_pnl = 0.0;
            _daily_day = day;
        }
    }
};

} // namespace omega

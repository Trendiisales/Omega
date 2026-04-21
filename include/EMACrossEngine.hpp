// =============================================================================
// EMACrossEngine.hpp -- EMA crossover scalper for XAUUSD
//
// STRATEGY (sweep-confirmed, 6-day / 1.5M tick backtest 2026-04-09..16):
//   Fast EMA(9) crosses Slow EMA(15) on M1 bar close
//   RSI(tick) confirms direction: cross must be in RSI 40-50 zone (not OB/OS)
//   Both LONG and SHORT -- 16 trades/day, $402/6days = $67/day
//
// BEST CONFIG:
//   fast=9, slow=15, rsi_lo=40, rsi_hi=50, sl_mult=1.5, tp_rr=1.0, cross_exit=true
//   WR=46.5%, AvgTrade=$4.06, MaxDD=$91.58
//
// ENTRY: on bar close after EMA cross, RSI in window
// SL:    ATR * 1.5 behind entry
// TP:    SL * 1.0 (1:1 RR -- gold mean-reverts, 2R rarely fills)
// EXIT:  TP hit, SL hit, EMA crosses back (cross_exit), or 3min timeout
// BE:    SL moves to (entry + cost_buffer) when MFE >= 50% of TP distance.
//        Buffer = spread + commission + safety. This covers the spread-crossing
//        loss (entry at ask, exit at bid) so BE exits lock a small profit
//        instead of deterministically losing spread+commission.
//        Audit 04-20..04-21: pre-fix BE exits were 100% losers (-$6.40 avg).
//
// NOTE: rr=1.0 is unusual but sweep-proven. Gold at M1 mean-reverts
// within the SL distance consistently. Higher RR (1.5, 2.0) cuts PnL
// because the full move rarely develops before reversal.
//
// WIRING (tick_gold.hpp):
//   // EMA cross engine -- bar update at M1 close
//   g_ema_cross.on_bar(s_cur1.close,
//       g_bars_gold.m1.ind.atr14.load(std::memory_order_relaxed),
//       now_ms_g);  // called in M1 bar close block
//
//   // EMA cross entry + management (after CBE block)
//   if (g_ema_cross.has_open_position()) {
//       g_ema_cross.on_tick(bid, ask, now_ms_g,
//           [&](const omega::TradeRecord& tr){ handle_closed_trade(tr); });
//   }
//   if (!g_ema_cross.has_open_position()
//       && gold_can_enter
//       && !g_candle_flow.has_open_position()
//       && !g_cbe.has_open_position()
//       && !g_gold_flow.has_open_position()
//       && !g_gold_stack.has_open_position()
//       && !g_bracket_gold.has_open_position()
//       && !g_trend_pb_gold.has_open_position()
//       && !g_hybrid_gold.has_open_position()
//       && !in_ny_close_noise) {
//       g_ema_cross.on_tick(bid, ask, now_ms_g,
//           [&](const omega::TradeRecord& tr) {
//               handle_closed_trade(tr);
//               if (!g_ema_cross.shadow_mode)
//                   send_live_order("XAUUSD", tr.side=="SHORT", tr.size, tr.exitPrice);
//           });
//       if (g_ema_cross.has_open_position()) {
//           g_telemetry.UpdateLastEntryTs();
//           write_trade_open_log("XAUUSD", "EMACross",
//               g_ema_cross.pos.is_long ? "LONG" : "SHORT",
//               g_ema_cross.pos.entry, g_ema_cross.pos.tp, g_ema_cross.pos.sl,
//               g_ema_cross.pos.size, ask - bid, "EMA_CROSS", "EMA9_15");
//       }
//   }
// =============================================================================

#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <deque>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <mutex>      // FIX ECE-MUTEX 2026-04-21: serialize close paths (force_close vs on_tick SL/TP/TIMEOUT)
#include "OmegaTradeLedger.hpp"

namespace omega {

// =============================================================================
// Config -- sweep-confirmed 2026-04-16
// =============================================================================
// =============================================================================
// TUNED PARAMS -- ema_tuned_sweep.cpp confirmed 2026-04-16
//   Baseline (9/15 sweep):   T=275 WR=41.1% PnL=$53   Avg=$0.19  MaxDD=$342
//   Tuned:                   T=91  WR=58.2% PnL=$731  Avg=$8.03  MaxDD=$56
//   Improvement: 13.7x PnL, 84% MaxDD reduction, Sharpe proxy 0.16 -> 13.1
//
// Active filters confirmed by diagnostic + tuned sweep:
//   max_lag_s=60    : only enter within 60s of cross (lag>60 loses $397)
//   rsi_exclude_bad : block RSI 30-35, 35-40, 50-55, 65-70 (all bleed)
//   gap_block=0.30  : block EMA spread >0.30 (stale late entries lose $260)
//   hour_kill       : block UTC 05,12,14,16,20 (all strongly negative hours)
//   cross_exit=OFF  : confirmed harmful ($-256 on 16 trades)
//   rr=1.0          : 1:1 confirmed again (gold M1 mean-reverts)
//   long_only=OFF   : both directions better than LONG-only
//   atr_block=OFF   : ATR 2-3 block not needed after other filters applied
// =============================================================================
static constexpr int     ECE_FAST_PERIOD    = 9;
static constexpr int     ECE_SLOW_PERIOD    = 15;
static constexpr double  ECE_RSI_LO         = 40.0;  // LONG: RSI must be > this
static constexpr double  ECE_RSI_HI         = 50.0;  // SHORT: RSI must be < this
static constexpr double  ECE_SL_MULT        = 1.5;   // SL = ATR * 1.5
static constexpr double  ECE_TP_RR          = 1.0;   // TP = SL (1:1 confirmed)
// ECE_CROSS_EXIT: permanently removed -- confirmed harmful ($-256 on 16 trades, -$16/trade avg)
static constexpr int64_t ECE_MAX_LAG_MS     = 60000; // 60s: lag>60s loses $397
static constexpr int64_t ECE_TIMEOUT_MS     = 180000;
static constexpr int64_t ECE_COOLDOWN_MS    = 20000;
static constexpr double  ECE_GAP_BLOCK      = 0.30;  // block EMA spread > 0.30
static constexpr double  ECE_RISK_DOLLARS   = 30.0;
static constexpr double  ECE_MIN_LOT        = 0.01;
static constexpr double  ECE_MAX_LOT        = 0.01;   // FIX 2026-04-21 uniformity: all engines capped at 0.01 until SHADOW validated
static constexpr int64_t ECE_STARTUP_MS     = 120000;
// BE-stop cost coverage -- BE SL must sit above entry by enough to cover
// spread-crossing loss + commission, else every BE exit deterministically
// loses spread+commission. Audit 04-20..04-21: 3/3 BE exits lost money
// (100% lose rate, avg -$6.40). Mechanism: entry at ask-side, exit at
// bid-side, so SL=entry triggers when bid touches entry == -spread at exit.
// Fix: BE SL = entry +/- (spread_entry + commission + safety).
// Values chosen for XAUUSD 0.1 lot: commission ~0.06 pts, typical spread
// 0.15-0.35 pts, safety 0.05 pts. Uses spread captured at entry time.
static constexpr double  ECE_BE_COMMISSION_PTS = 0.06;  // ~$0.60 per 0.1 lot round-trip
static constexpr double  ECE_BE_SAFETY_PTS     = 0.05;  // small guaranteed profit cushion
static constexpr double  ECE_BE_MIN_BUFFER_PTS = 0.20;  // floor: cover typical spread+comm
static constexpr double  ECE_BE_MAX_BUFFER_PTS = 0.80;  // ceiling: don't defeat BE purpose

// Bad RSI buckets confirmed by diagnostic (all bleed):
// 30-35: -$56, 35-40: -$128, 50-55: -$113, 65-70: -$111
static inline bool ece_rsi_allowed(double rsi, bool is_long) noexcept {
    if (rsi >= 30.0 && rsi < 35.0) return false;
    if (rsi >= 35.0 && rsi < 40.0) return false;
    if (rsi >= 50.0 && rsi < 55.0) return false;
    if (rsi >= 65.0 && rsi < 70.0) return false;
    if (is_long  && (rsi < ECE_RSI_LO || rsi > 75.0)) return false;
    if (!is_long && (rsi > ECE_RSI_HI || rsi < 25.0)) return false;
    return true;
}

// Bad UTC hours (diagnostic + live 2026-04-17):
// 05:-$54 12:-$112 13:10%WR 14:-$80 16:-$105 17-19:NYlate chop 20:-$45
static inline bool ece_hour_allowed(int64_t ms) noexcept {
    int h = (int)(((ms/1000LL)%86400LL)/3600LL);
    return !(h==5 || h==12 || h==13 || h==14 || h==16 || h==17 || h==18 || h==19 || h==20);
}

// =============================================================================
struct EMACrossEngine {

    bool shadow_mode = true;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct OpenPos {
        bool   active    = false;
        bool   is_long   = false;
        bool   be_done   = false;
        double entry     = 0.0;
        double sl        = 0.0;
        double tp        = 0.0;
        double size      = 0.0;
        double mfe       = 0.0;
        double atr       = 0.0;
        double spread_at_entry = 0.0;  // captured at entry, used for BE buffer
        int64_t ets      = 0;
        int    trade_id  = 0;
    } pos;

    bool has_open_position() const noexcept { return pos.active; }

    // ── Bar update -- call at every M1 close ─────────────────────────────────
    // bar_close: M1 bar close price
    // bar_atr:   ATR14 from OHLCBarEngine (use g_bars_gold.m1.ind.atr14)
    // bar_rsi:   RSI14 from OHLCBarEngine (use g_bars_gold.m1.ind.rsi14)
    // now_ms:    current timestamp
    void on_bar(double bar_close, double bar_atr, double bar_rsi, int64_t now_ms) noexcept {
        // Update ATR and RSI from bar data
        if (bar_atr > 0.5 && bar_atr < 50.0) _atr = bar_atr;
        _rsi = bar_rsi;

        // Update EMAs
        const double prev_fast = _ema_fast;
        const double prev_slow = _ema_slow;
        _update_ema(_ema_fast, _ema_fast_alpha, bar_close, _fast_warmed, _fast_count, ECE_FAST_PERIOD);
        _update_ema(_ema_slow, _ema_slow_alpha, bar_close, _slow_warmed, _slow_count, ECE_SLOW_PERIOD);

        if (!_fast_warmed || !_slow_warmed) { (void)now_ms; return; }

        // Cross detection
        bool long_cross  = (_ema_fast > _ema_slow) && (prev_fast <= prev_slow);
        bool short_cross = (_ema_fast < _ema_slow) && (prev_fast >= prev_slow);

        if (long_cross)  { _cross_dir = 1;  _cross_ms = now_ms; }
        if (short_cross) { _cross_dir = -1; _cross_ms = now_ms; }

        // Heartbeat every 10 bars
        ++_bars_total;
        if (_bars_total % 30 == 0) {
            std::cout << "[ECE-ALIVE]"
                      << " fast=" << std::fixed << std::setprecision(2) << _ema_fast
                      << " slow=" << _ema_slow
                      << " rsi=" << std::setprecision(1) << _rsi
                      << " atr=" << std::setprecision(2) << _atr
                      << " cross=" << _cross_dir
                      << " shadow=" << shadow_mode << "\n";
            std::cout.flush();
        }
    }

    // ── Per-tick handler ─────────────────────────────────────────────────────
    void on_tick(double bid, double ask, int64_t now_ms,
                 CloseCallback on_close) noexcept
    {
        if (_startup_ms == 0) _startup_ms = now_ms;
        if (now_ms - _startup_ms < ECE_STARTUP_MS) return;

        _daily_reset(now_ms);
        if (_daily_pnl <= -200.0) return;

        double mid   = (bid + ask) * 0.5;
        double spread = ask - bid;

        // Position management
        if (pos.active) {
            double mv = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
            if (mv > pos.mfe) pos.mfe = mv;

            double td = std::fabs(pos.tp - pos.entry);
            if (!pos.be_done && td > 0 && pos.mfe >= td * 0.50) {
                // Fix: BE SL must cover spread-crossing + commission, else
                // every BE exit loses ~spread deterministically.
                // Use MAX(spread_at_entry, current_spread) so widening after
                // entry doesn't defeat us. Clamp to sane range AND ensure
                // buffer < current MFE - spread, or BE would trigger on arm.
                double live_sp = spread;
                double use_sp  = std::max(pos.spread_at_entry, live_sp);
                double buffer  = use_sp + ECE_BE_COMMISSION_PTS + ECE_BE_SAFETY_PTS;
                if (buffer < ECE_BE_MIN_BUFFER_PTS) buffer = ECE_BE_MIN_BUFFER_PTS;
                if (buffer > ECE_BE_MAX_BUFFER_PTS) buffer = ECE_BE_MAX_BUFFER_PTS;

                // Safety: BE SL must sit below current fillable price.
                // For LONG: current bid = mid - spread/2 = pos.entry + pos.mfe - spread/2.
                //   Required: entry + buffer < bid  -->  buffer < mfe - spread/2
                // For SHORT: current ask = mid + spread/2 = pos.entry - pos.mfe + spread/2 (on pos side).
                //   Required (flipped sign): entry - buffer > ask  -->  buffer < mfe - spread/2
                // If buffer cannot satisfy this, defer BE arming (return without setting be_done).
                double max_safe_buffer = pos.mfe - live_sp * 0.5 - ECE_BE_SAFETY_PTS;
                if (buffer <= max_safe_buffer && max_safe_buffer > 0.0) {
                    pos.sl = pos.is_long ? (pos.entry + buffer)
                                         : (pos.entry - buffer);
                    pos.be_done = true;
                    std::cout << "[ECE-BE] " << (pos.is_long ? "LONG" : "SHORT")
                              << " mfe=" << std::fixed << std::setprecision(2) << pos.mfe
                              << " sp_entry=" << std::setprecision(3) << pos.spread_at_entry
                              << " sp_live=" << live_sp
                              << " buf=" << buffer
                              << " sl=" << std::setprecision(2) << pos.sl << "\n";
                    std::cout.flush();
                }
                // else: defer BE -- MFE not yet large enough to cover buffer safely.
            }

            double ep = pos.is_long ? bid : ask;

            // TP
            if (pos.is_long ? (bid >= pos.tp) : (ask <= pos.tp)) {
                _close(ep, "TP", now_ms, on_close); return;
            }
            // SL
            if (pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl)) {
                _close(ep, pos.be_done ? "BE" : "SL", now_ms, on_close); return;
            }
            // Timeout
            if (now_ms - pos.ets > ECE_TIMEOUT_MS) {
                _close(ep, "TIMEOUT", now_ms, on_close); return;
            }
            // Cross-exit: PERMANENTLY DISABLED (diagnostic: -$256 on 16 trades)
            // Removed to avoid MSVC C4127 constant expression warning
            return;
        }

        // Entry guards
        if (!_fast_warmed || !_slow_warmed) return;
        if (_cross_dir == 0) return;
        if (_atr <= 0.0) return;
        if (now_ms - _last_exit_ms < ECE_COOLDOWN_MS) return;
        // Consecutive SL kill: 3 SLs in a row = chop, block for 30min
        if (now_ms < _sl_kill_until) {
            static int64_t s_kl = 0;
            if (now_ms - s_kl > 120000) {
                s_kl = now_ms;
                char km[128];
                snprintf(km, sizeof(km), "[ECE-SL-KILL] blocked %d SLs %llds remain\n",
                    _consec_sl, (long long)((_sl_kill_until - now_ms) / 1000LL));
                std::cout << km; std::cout.flush();
            }
            return;
        }
        if (spread > _atr * 0.30) return;
        // Hour kill: UTC 05,12,14,16,20 all bleed (diagnostic confirmed)
        if (!ece_hour_allowed(now_ms)) return;
        // EMA gap block: spread >0.30 = stale late entry, loses $260
        if (std::fabs(_ema_fast - _ema_slow) > ECE_GAP_BLOCK) return;
        // Cross must be fresh (within 60s -- lag>60s loses $397 in diagnostic)
        if (now_ms - _cross_ms > ECE_MAX_LAG_MS) { _cross_dir = 0; return; }

        bool isl = (_cross_dir == 1);

        // RSI filter: confirm entry is not into exhausted move
        // LONG: RSI must be between rsi_lo and 75 (confirmed momentum, not overbought)
        // SHORT: RSI must be between 25 and rsi_hi (confirmed momentum, not oversold)
        // RSI gate: exclude confirmed bad buckets (30-35, 35-40, 50-55, 65-70)
        if (!ece_rsi_allowed(_rsi, isl)) return;

        // Cost coverage
        double sl_dist = _atr * ECE_SL_MULT;
        double tp_dist = sl_dist * ECE_TP_RR;
        double cost    = spread + 0.20;
        if (tp_dist <= cost) return;

        _enter(isl, bid, ask, spread, sl_dist, tp_dist, now_ms, on_close);
    }

    void force_close(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        // FIX ECE-MUTEX 2026-04-21: serialize against concurrent _close path from
        // on_tick (TP / SL / BE / TIMEOUT). tick_gold.hpp may invoke force_close
        // (session-end / daily-kill path) while a tick-driven on_tick is mid-flight
        // on the same engine -- without this lock both paths could observe
        // pos.active==true and both invoke the close body, emitting a duplicate
        // TradeRecord with the same trade_id. Mirror of CandleFlow RACE-FIX and
        // CBE-8 pattern. Re-check pos.active under lock so the second entrant
        // bails silently.
        std::lock_guard<std::mutex> lk(_close_mtx);
        if (!pos.active) return;
        _close_locked(pos.is_long ? bid : ask, "FORCE_CLOSE", now_ms, cb);
    }

private:
    // ── EMA state ────────────────────────────────────────────────────────────
    double _ema_fast       = 0.0;
    double _ema_slow       = 0.0;
    double _ema_fast_alpha = 2.0 / (ECE_FAST_PERIOD + 1.0);
    double _ema_slow_alpha = 2.0 / (ECE_SLOW_PERIOD + 1.0);
    bool   _fast_warmed    = false;
    bool   _slow_warmed    = false;
    int    _fast_count     = 0;
    int    _slow_count     = 0;

    // ── Indicator state ───────────────────────────────────────────────────────
    double  _atr           = 0.0;
    double  _rsi           = 50.0;
    int     _cross_dir     = 0;    // +1=bullish cross, -1=bearish cross, 0=none
    int64_t _cross_ms      = 0;
    int64_t _bars_total    = 0;

    // ── Session state ────────────────────────────────────────────────────────
    double  _daily_pnl     = 0.0;
    int64_t _daily_day     = 0;
    int64_t _last_exit_ms  = 0;
    int64_t _startup_ms    = 0;
    int     _total_trades  = 0;
    int     _total_wins    = 0;
    int     _trade_id      = 0;
    int     _consec_sl     = 0;    // consecutive SL counter
    int64_t _sl_kill_until = 0;    // block entries until this ms

    // FIX ECE-MUTEX 2026-04-21: race guard for _close / force_close.
    // mutable so const-qualified methods could also acquire if needed.
    mutable std::mutex _close_mtx;

    // ── EMA update ────────────────────────────────────────────────────────────
    static void _update_ema(double& ema, double alpha, double price,
                             bool& warmed, int& count, int period) noexcept {
        if (!warmed) {
            ema = (ema * count + price) / (count + 1);
            ++count;
            if (count >= period) warmed = true;
        } else {
            ema = price * alpha + ema * (1.0 - alpha);
        }
    }

    // ── Entry ─────────────────────────────────────────────────────────────────
    void _enter(bool is_long, double bid, double ask, double spread,
                double sl_dist, double tp_dist,
                int64_t now_ms, CloseCallback on_close) noexcept
    {
        double entry_px = is_long ? ask : bid;
        double sl_px    = is_long ? (entry_px - sl_dist) : (entry_px + sl_dist);
        double tp_px    = is_long ? (entry_px + tp_dist) : (entry_px - tp_dist);

        double sl_safe = std::max(0.10, sl_dist);
        double size = ECE_RISK_DOLLARS / (sl_safe * 100.0);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(ECE_MIN_LOT, std::min(ECE_MAX_LOT, size));

        ++_trade_id;

        std::cout << "[ECE] " << (is_long ? "LONG" : "SHORT")
                  << " entry=" << std::fixed << std::setprecision(2) << entry_px
                  << " sl=" << sl_px << " tp=" << tp_px
                  << " size=" << std::setprecision(3) << size
                  << " atr=" << std::setprecision(2) << _atr
                  << " rsi=" << std::setprecision(1) << _rsi
                  << " fast=" << std::setprecision(2) << _ema_fast
                  << " slow=" << _ema_slow
                  << " spread=" << spread
                  << (shadow_mode ? " [SHADOW]" : "") << "\n";
        std::cout.flush();

        pos.active   = true;
        pos.is_long  = is_long;
        pos.entry    = entry_px;
        pos.sl       = sl_px;
        pos.tp       = tp_px;
        pos.size     = size;
        pos.mfe      = 0.0;
        pos.be_done  = false;
        pos.atr      = _atr;
        pos.spread_at_entry = spread;
        pos.ets      = now_ms;
        pos.trade_id = _trade_id;

        _cross_dir = 0;  // consume the cross signal

        (void)on_close;
    }

    // ── Manage ────────────────────────────────────────────────────────────────
    // (handled inline in on_tick above)

    // ── Close ─────────────────────────────────────────────────────────────────
    // FIX ECE-MUTEX 2026-04-21: public _close acquires mutex and double-checks
    // pos.active. All internal call sites (TP, SL, BE, TIMEOUT) now go through
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
    // Body is unchanged from pre-2026-04-21 _close.
    void _close_locked(double exit_px, const char* reason,
                       int64_t now_ms, CloseCallback on_close) noexcept
    {
        double pnl_pts = pos.is_long
            ? (exit_px - pos.entry) : (pos.entry - exit_px);
        double pnl_usd = pnl_pts * pos.size * 100.0;

        _daily_pnl   += pnl_usd;
        _last_exit_ms = now_ms;
        ++_total_trades;

        const bool win = (pnl_usd > 0);
        if (win) ++_total_wins;

        // Consecutive SL tracker
        if (std::string(reason) == "SL") {
            if (++_consec_sl >= 3) {
                _sl_kill_until = now_ms + 1800000LL;
                char km[128];
                snprintf(km, sizeof(km), "[ECE-SL-KILL] %d consec SLs -- 30min block\n", _consec_sl);
                std::cout << km; std::cout.flush();
            }
        } else {
            _consec_sl = 0;
        }

        std::cout << "[ECE] EXIT " << (pos.is_long ? "LONG" : "SHORT")
                  << " @ " << std::fixed << std::setprecision(2) << exit_px
                  << " " << reason
                  << " pnl=$" << std::setprecision(2) << pnl_usd
                  << " mfe=" << std::setprecision(2) << pos.mfe
                  << " held=" << (now_ms - pos.ets) / 1000LL << "s"
                  << " daily=$" << _daily_pnl
                  << " W/T=" << _total_wins << "/" << _total_trades
                  << (shadow_mode ? " [SHADOW]" : "") << "\n";
        std::cout.flush();

        if (on_close) {
            omega::TradeRecord tr;
            tr.id         = pos.trade_id;
            tr.symbol     = "XAUUSD";
            tr.side       = pos.is_long ? "LONG" : "SHORT";
            tr.engine     = "EMACross";
            tr.entryPrice = pos.entry;
            tr.exitPrice  = exit_px;
            tr.sl         = pos.sl;
            tr.size       = pos.size;
            tr.pnl        = pnl_pts * pos.size;
            tr.mfe        = pos.mfe;
            tr.mae        = 0.0;
            tr.entryTs    = pos.ets / 1000;
            tr.exitTs     = now_ms / 1000;
            tr.exitReason = reason;
            tr.regime     = "EMA_CROSS";
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
                std::cout << "[ECE] Daily reset PnL=$"
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

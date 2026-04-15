#pragma once
// =============================================================================
// TickScalpEngine.hpp -- Tick-pattern scalper for XAUUSD
//
// COMPLETELY INDEPENDENT of all other Omega engines.
// Does NOT check gold_any_open. Does NOT block other engines from entering.
// Runs its own position, its own risk, its own state.
// The only shared reads are: bid/ask/now_ms, g_l2_gold atomics,
// g_bars_gold.m1.ind (tick_rate, atr14), ewm_drift, session_slot.
// Zero mutex, zero shared write state.
//
// THREE ENTRY PATTERNS:
//
//   P1 -- TICK MOMENTUM BURST
//     N consecutive ticks all moving same direction (bid rising or falling)
//     Net move over N ticks >= MIN_BURST_MOVE
//     Fires in burst direction. TP = net_move * 2.0. SL = net_move * 1.0.
//
//   P2 -- DOM ABSORPTION
//     L2 micro_edge sustained above/below threshold for P2_HOLD_TICKS ticks
//     (micro_edge is GoldMicrostructureAnalyzer score: >0.65 bid pressure,
//      <0.35 ask pressure -- same signal used by GoldFlow persistence path)
//     Price not fighting imbalance direction
//     TP: P2_TP_PT  SL: P2_SL_PT (fixed tight)
//
//   P3 -- VELOCITY SPIKE
//     tick_rate > P3_SPIKE_MULT * baseline AND net directional move > threshold
//     Fires in spike direction.
//     TP: P3_TP_PT  SL: P3_SL_PT
//
// COST COVERAGE GATE (all patterns):
//   Expected TP must exceed round-trip cost (spread + commission) before firing.
//
// RISK:
//   Max 1 position at a time (own position, independent of other engines)
//   Shadow mode ON by default -- logs [TSE] signals, no broker orders
//   Max lot: TSE_MAX_LOT (0.01 default)
//   Daily loss limit: TSE_DAILY_LOSS_LIMIT
//   Cooldown: TSE_COOLDOWN_MS after any exit
//   Session gate: slots 1-5 only (London + NY, no Asia, no dead zone)
//   L2 must be fresh (< 5s stale) to fire P2/P3
//   Max 3 consecutive losses then 5-min pause
//
// WIRING (tick_gold.hpp, at end of on_tick_gold before closing brace):
//   g_tick_scalp.on_tick(bid, ask, now_ms_g,
//       g_l2_gold.micro_edge.load(std::memory_order_relaxed),
//       g_l2_gold.fresh(now_ms_g),
//       g_bars_gold.m1.ind.tick_rate.load(std::memory_order_relaxed),
//       g_bars_gold.m1.ind.atr14.load(std::memory_order_relaxed),
//       ewm_drift,
//       gold_session_slot,
//       [](const omega::TradeRecord& tr){ handle_closed_trade(tr); });
// =============================================================================

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <iomanip>
#include "OmegaTradeLedger.hpp"

namespace omega {

// =============================================================================
// Config -- all constexpr, no runtime config needed for shadow period
// =============================================================================
static constexpr double   TSE_MAX_LOT           = 0.01;   // hard cap -- tiny
static constexpr double   TSE_RISK_DOLLARS       = 10.0;   // risk per trade
static constexpr double   TSE_DAILY_LOSS_LIMIT   = 50.0;   // stop for day
static constexpr int      TSE_MAX_CONSEC_LOSSES  = 3;      // then 5-min pause
static constexpr int64_t  TSE_COOLDOWN_MS        = 30000;  // 30s after any exit
static constexpr int64_t  TSE_PAUSE_MS           = 300000; // 5-min consec-loss pause
static constexpr double   TSE_MAX_SPREAD         = 0.50;   // max spread to enter
static constexpr double   TSE_COMMISSION_RT      = 0.20;   // round-trip commission estimate (pts)
static constexpr double   TSE_ATR_MIN            = 1.5;    // min ATR for entries (not dead tape)
static constexpr double   TSE_ATR_MAX            = 8.0;    // max ATR (not crash)

// P1: Tick Momentum Burst
static constexpr int      TSE_P1_TICKS           = 8;      // consecutive same-dir ticks
static constexpr double   TSE_P1_MIN_MOVE        = 0.50;   // min net move (pts) over P1_TICKS
static constexpr double   TSE_P1_TP_MULT         = 2.5;    // TP = net_move * mult
static constexpr double   TSE_P1_SL_MULT         = 1.5;    // SL = net_move * mult

// P2: DOM Absorption
static constexpr double   TSE_P2_EDGE_LONG       = 0.65;   // micro_edge > this = bid pressure
static constexpr double   TSE_P2_EDGE_SHORT      = 0.35;   // micro_edge < this = ask pressure
static constexpr int      TSE_P2_HOLD_TICKS      = 5;      // consecutive DOM ticks above threshold
static constexpr double   TSE_P2_TP              = 0.30;
static constexpr double   TSE_P2_SL              = 0.20;

// P3: Velocity Spike
static constexpr double   TSE_P3_SPIKE_MULT      = 3.0;    // tick_rate vs baseline
static constexpr int      TSE_P3_WINDOW          = 8;      // ticks for direction check
static constexpr double   TSE_P3_MIN_MOVE        = 0.15;   // min directional move over window
static constexpr double   TSE_P3_TP              = 1.00;
static constexpr double   TSE_P3_SL              = 0.40;

// BE and trail (profit locking)
// P1/P2: move SL to breakeven when MFE >= TSE_BE_FRAC * TP
// P3:    trail arms at TSE_P3_TRAIL_ARM pts MFE, trails TSE_P3_TRAIL_DIST pts
static constexpr double   TSE_BE_FRAC            = 0.50;  // BE at 50% of TP distance
static constexpr double   TSE_P3_TRAIL_ARM       = 0.50;  // arm trail at 0.5pt MFE
static constexpr double   TSE_P3_TRAIL_DIST      = 0.30;  // trail 0.3pt behind MFE

// RSI slope tracker
static constexpr int      TSE_RSI_SLOPE_N        = 8;    // ticks for slope EMA
static constexpr double   TSE_RSI_SLOPE_ALPHA    = 2.0 / (TSE_RSI_SLOPE_N + 1.0);
static constexpr double   TSE_RSI_SLOPE_THRESH   = 0.10; // min |slope EMA| to confirm trend direction

// Tick history size
static constexpr int      TSE_HIST_SIZE          = 60;
static constexpr int      TSE_DOM_HIST_SIZE       = 20;

// =============================================================================
struct TickScalpEngine {

    bool shadow_mode = true;   // flip to false after shadow validation

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // ── Open position state ──────────────────────────────────────────────────
    struct OpenPos {
        bool    active       = false;
        bool    is_long      = false;
        double  entry        = 0.0;
        double  sl           = 0.0;
        double  tp           = 0.0;
        double  size         = 0.0;
        double  mfe          = 0.0;
        bool    be_done      = false;  // true after SL moved to entry
        bool    trail_armed  = false;  // P3 only: trail engaged
        double  trail_sl     = 0.0;    // P3 only: current trail SL
        int64_t entry_ts_ms  = 0;
        int     trade_id     = 0;
        char    pattern[16]  = {};
    } pos_;

    bool has_open_position() const noexcept { return pos_.active; }

    // ── Main tick function ───────────────────────────────────────────────────
    void on_tick(double bid, double ask,
                 int64_t now_ms,
                 double micro_edge,      // g_l2_gold.micro_edge (0..1)
                 bool   l2_fresh,        // g_l2_gold.fresh(now_ms)
                 double tick_rate,       // g_bars_gold.m1.ind.tick_rate
                 double atr,             // g_bars_gold.m1.ind.atr14
                 double ewm_drift,       // g_gold_stack.ewm_drift()
                 double bar_rsi,         // g_bars_gold.m1.ind.rsi14 (0..100)
                 int    session_slot,    // g_macro_ctx.session_slot
                 CloseCallback on_close) noexcept
    {
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // Daily reset
        _daily_reset(now_ms);

        // Update tick history (always, unconditional)
        _bid_hist.push_back(bid);
        if ((int)_bid_hist.size() > TSE_HIST_SIZE) _bid_hist.pop_front();
        _tick_times.push_back(now_ms);
        if ((int)_tick_times.size() > TSE_HIST_SIZE) _tick_times.pop_front();

        // DOM history (always)
        _dom_hist.push_back(micro_edge);
        if ((int)_dom_hist.size() > TSE_DOM_HIST_SIZE) _dom_hist.pop_front();

        // Velocity baseline (EWM over 30s windows)
        // Initialise window start on first tick to prevent immediate bogus window close.
        if (_vel_window_start_ms == 0) _vel_window_start_ms = now_ms;
        _ticks_this_window++;
        if (now_ms - _vel_window_start_ms >= 30000LL) {
            _vel_baseline = _vel_baseline * 0.7 + _ticks_this_window * 0.3;
            _ticks_this_window  = 0;
            _vel_window_start_ms = now_ms;
        }

        // Manage open position
        if (pos_.active) {
            _manage(bid, ask, mid, now_ms, on_close);
            return;
        }

        // RSI slope tracker: EMA of bar_rsi changes
        // Positive slope = RSI trending up (bullish). Negative = trending down (bearish).
        if (bar_rsi > 1.0) {
            if (!_rsi_warmed) {
                _rsi_prev      = bar_rsi;
                _rsi_slope_ema = 0.0;
                _rsi_warmed    = true;
            } else if (bar_rsi != _rsi_prev) {
                const double slope = bar_rsi - _rsi_prev;
                _rsi_slope_ema = slope * TSE_RSI_SLOPE_ALPHA + _rsi_slope_ema * (1.0 - TSE_RSI_SLOPE_ALPHA);
                _rsi_prev = bar_rsi;
            }
        }

        // ── Entry guards ─────────────────────────────────────────────────────
        // Heartbeat: log TSE state every 60s so it's visible in logs
        {
            static int64_t s_hb = 0;
            if (now_ms - s_hb > 60000LL) {
                s_hb = now_ms;
                std::cout << "[TSE-ALIVE] slot=" << session_slot
                          << " atr=" << std::fixed << std::setprecision(2) << atr
                          << " vel_base=" << std::setprecision(1) << _vel_baseline
                          << " rsi=" << std::setprecision(1) << bar_rsi
                          << " rsi_slope=" << std::setprecision(3) << _rsi_slope_ema
                          << " spread=" << std::setprecision(2) << spread
                          << " daily=$" << _daily_pnl
                          << " shadow=" << (shadow_mode ? 1 : 0)
                          << " hist=" << (int)_bid_hist.size() << "\n";
                std::cout.flush();
            }
        }
        // Session: slots 1-5 only (London + NY). Slot 0 = dead, slot 6 = Asia.
        if (session_slot < 1 || session_slot > 5) return;

        // Daily loss limit
        if (_daily_pnl <= -TSE_DAILY_LOSS_LIMIT) {
            static int64_t s_dl = 0;
            if (now_ms - s_dl > 60000) {
                s_dl = now_ms;
                std::cout << "[TSE] Daily loss limit $" << (int)TSE_DAILY_LOSS_LIMIT << " hit -- stopped\n";
                std::cout.flush();
            }
            return;
        }

        // Consecutive loss pause
        if (now_ms < _pause_until_ms) return;

        // Cooldown
        if (now_ms - _last_exit_ms < TSE_COOLDOWN_MS) return;

        // Spread
        if (spread > TSE_MAX_SPREAD) return;

        // ATR quality gate -- not dead tape, not crash
        if (atr < TSE_ATR_MIN || atr > TSE_ATR_MAX) {
            static int64_t s_atr_log = 0;
            if (now_ms - s_atr_log > 60000LL) {
                s_atr_log = now_ms;
                std::cout << "[TSE-ATR-BLOCK] atr=" << std::fixed << std::setprecision(2) << atr
                          << " outside [" << std::setprecision(1) << TSE_ATR_MIN
                          << "," << TSE_ATR_MAX << "]\n";
                std::cout.flush();
            }
            return;
        }

        // Velocity baseline guard: require at least one 30s window before P1/P3
        // Prevents firing on the first 8 ticks after restart with no market context.
        const bool vel_ready = (_vel_baseline >= 10.0);

        // Try patterns
        if (vel_ready) _try_p1(bid, ask, spread, atr, bar_rsi, now_ms, on_close);
        if (pos_.active) return;
        if (l2_fresh) _try_p2(bid, ask, spread, micro_edge, atr, now_ms, on_close);
        if (pos_.active) return;
        if (l2_fresh && vel_ready) _try_p3(bid, ask, spread, tick_rate, atr, now_ms, on_close);
    }

    void force_close(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (!pos_.active) return;
        _close(pos_.is_long ? bid : ask, "FORCE_CLOSE", now_ms, cb);
    }

private:
    // ── State ────────────────────────────────────────────────────────────────
    std::deque<double>  _bid_hist;
    std::deque<int64_t> _tick_times;
    std::deque<double>  _dom_hist;

    double  _vel_baseline        = 0.0;
    int     _ticks_this_window   = 0;
    int64_t _vel_window_start_ms = 0;

    // RSI slope tracker
    double  _rsi_prev      = 0.0;
    double  _rsi_slope_ema = 0.0;
    bool    _rsi_warmed    = false;

    double  _daily_pnl     = 0.0;
    int64_t _daily_day     = 0;
    int64_t _last_exit_ms  = 0;
    int64_t _pause_until_ms= 0;
    int     _consec_losses = 0;
    int     _total_trades  = 0;
    int     _total_wins    = 0;
    int     _trade_id      = 0;

    // ── P1: Tick Momentum Burst ───────────────────────────────────────────────
    void _try_p1(double bid, double ask, double spread, double atr,
                 double bar_rsi, int64_t now_ms, CloseCallback on_close) noexcept
    {
        if ((int)_bid_hist.size() < TSE_P1_TICKS + 1) return;

        int n = (int)_bid_hist.size();
        int up = 0, dn = 0;
        for (int i = n - TSE_P1_TICKS; i < n; ++i) {
            double d = _bid_hist[i] - _bid_hist[i-1];
            if (d > 0) up++;
            else if (d < 0) dn++;
        }

        bool burst_up = (up >= TSE_P1_TICKS - 1 && dn == 0);
        bool burst_dn = (dn >= TSE_P1_TICKS - 1 && up == 0);
        if (!burst_up && !burst_dn) return;

        // RSI slope direction gate: burst must agree with RSI slope trend.
        // Uses EMA of RSI bar-to-bar changes -- positive = RSI trending up (bullish),
        // negative = RSI trending down (bearish).
        // A P1 LONG burst is only valid when RSI slope is rising.
        // A P1 SHORT burst is only valid when RSI slope is falling.
        if (_rsi_warmed) {
            if (burst_up && _rsi_slope_ema < -TSE_RSI_SLOPE_THRESH) {
                static int64_t s_rsi_log = 0;
                if (now_ms - s_rsi_log > 10000) {
                    s_rsi_log = now_ms;
                    std::cout << "[TSE-RSI-BLOCK] P1 LONG blocked: burst_up but rsi_slope="
                              << std::fixed << std::setprecision(3) << _rsi_slope_ema
                              << " (RSI falling)\n";
                    std::cout.flush();
                }
                return;
            }
            if (burst_dn && _rsi_slope_ema > TSE_RSI_SLOPE_THRESH) {
                static int64_t s_rsi_log2 = 0;
                if (now_ms - s_rsi_log2 > 10000) {
                    s_rsi_log2 = now_ms;
                    std::cout << "[TSE-RSI-BLOCK] P1 SHORT blocked: burst_dn but rsi_slope="
                              << std::fixed << std::setprecision(3) << _rsi_slope_ema
                              << " (RSI rising)\n";
                    std::cout.flush();
                }
                return;
            }
        }

        double net = std::fabs(_bid_hist[n-1] - _bid_hist[n-1-TSE_P1_TICKS]);
        if (net < TSE_P1_MIN_MOVE) return;

        double tp_pts = net * TSE_P1_TP_MULT;
        double sl_pts = net * TSE_P1_SL_MULT;

        _enter("P1-BURST", burst_up, bid, ask, spread, tp_pts, sl_pts, atr, now_ms, on_close);
    }

    // ── P2: DOM Absorption ────────────────────────────────────────────────────
    void _try_p2(double bid, double ask, double spread, double micro_edge, double atr,
                 int64_t now_ms, CloseCallback on_close) noexcept
    {
        if ((int)_dom_hist.size() < TSE_P2_HOLD_TICKS) return;

        int n = (int)_dom_hist.size();
        int long_cnt = 0, short_cnt = 0;
        for (int i = n - TSE_P2_HOLD_TICKS; i < n; ++i) {
            if (_dom_hist[i] > TSE_P2_EDGE_LONG)  long_cnt++;
            if (_dom_hist[i] < TSE_P2_EDGE_SHORT) short_cnt++;
        }

        bool dom_long  = (long_cnt  >= TSE_P2_HOLD_TICKS);
        bool dom_short = (short_cnt >= TSE_P2_HOLD_TICKS);
        if (!dom_long && !dom_short) return;

        // Price not fighting the DOM
        if ((int)_bid_hist.size() >= 3) {
            int nb = (int)_bid_hist.size();
            double recent = _bid_hist[nb-1] - _bid_hist[nb-3];
            if (dom_long  && recent < -0.05) return;
            if (dom_short && recent >  0.05) return;
        }

        _enter("P2-DOM", dom_long, bid, ask, spread, TSE_P2_TP, TSE_P2_SL, atr, now_ms, on_close);
    }

    // ── P3: Velocity Spike ────────────────────────────────────────────────────
    void _try_p3(double bid, double ask, double spread, double tick_rate, double atr,
                 int64_t now_ms, CloseCallback on_close) noexcept
    {
        if (_vel_baseline < 2.0) return;
        if (tick_rate < _vel_baseline * TSE_P3_SPIKE_MULT) return;

        if ((int)_bid_hist.size() < TSE_P3_WINDOW + 1) return;
        int n = (int)_bid_hist.size();
        double net = _bid_hist[n-1] - _bid_hist[n-1-TSE_P3_WINDOW];
        if (std::fabs(net) < TSE_P3_MIN_MOVE) return;

        _enter("P3-VEL", net > 0, bid, ask, spread, TSE_P3_TP, TSE_P3_SL, atr, now_ms, on_close);
    }

    // ── Enter ─────────────────────────────────────────────────────────────────
    void _enter(const char* pattern, bool is_long,
                double bid, double ask, double spread,
                double tp_pts, double sl_pts, double atr,
                int64_t now_ms, CloseCallback on_close) noexcept
    {
        // Cost coverage: TP must exceed round-trip cost
        const double cost = spread + TSE_COMMISSION_RT;
        if (tp_pts <= cost) {
            static int64_t s_cost = 0;
            if (now_ms - s_cost > 10000) {
                s_cost = now_ms;
                std::cout << "[TSE-NOCOST] " << pattern
                          << " tp=" << std::fixed << std::setprecision(2) << tp_pts
                          << " <= cost=" << cost << " -- skip\n";
                std::cout.flush();
            }
            return;
        }

        const double entry_px = is_long ? ask : bid;
        const double tp_px    = is_long ? entry_px + tp_pts : entry_px - tp_pts;
        const double sl_px    = is_long ? entry_px - sl_pts : entry_px + sl_pts;

        // Size: risk / sl_pts, capped at TSE_MAX_LOT
        const double sl_safe = std::max(0.10, sl_pts);
        double size = TSE_RISK_DOLLARS / (sl_safe * 100.0);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(0.01, std::min(TSE_MAX_LOT, size));

        ++_trade_id;

        std::cout << "[TSE] " << pattern
                  << " " << (is_long ? "LONG" : "SHORT")
                  << " @ " << std::fixed << std::setprecision(2) << entry_px
                  << " sl=" << sl_px << " tp=" << tp_px
                  << " size=" << std::setprecision(3) << size
                  << " spread=" << std::setprecision(2) << spread
                  << " atr=" << atr
                  << (shadow_mode ? " [SHADOW]" : "") << "\n";
        std::cout.flush();

        pos_.active      = true;
        pos_.is_long     = is_long;
        pos_.entry       = entry_px;
        pos_.sl          = sl_px;
        pos_.tp          = tp_px;
        pos_.size        = size;
        pos_.mfe         = 0.0;
        pos_.be_done     = false;
        pos_.trail_armed = false;
        pos_.trail_sl    = sl_px;
        pos_.entry_ts_ms = now_ms;
        pos_.trade_id    = _trade_id;
        std::strncpy(pos_.pattern, pattern, sizeof(pos_.pattern) - 1);
        pos_.pattern[sizeof(pos_.pattern) - 1] = '\0';
    }

    // ── Manage ────────────────────────────────────────────────────────────────
    // P1/P2: fixed TP/SL + BE move at 50% of TP distance.
    // P3:    fixed TP/SL + BE at 50% TP + trail arms at 0.5pt MFE (0.3pt dist).
    void _manage(double bid, double ask, double mid,
                 int64_t now_ms, CloseCallback on_close) noexcept
    {
        const double move = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
        if (move > pos_.mfe) pos_.mfe = move;

        const double eff_price = pos_.is_long ? bid : ask;
        const double tp_dist   = std::fabs(pos_.tp - pos_.entry);

        // ── BE move: P1/P2/P3 all get this ───────────────────────────────────
        // Once MFE >= 50% of TP distance, move SL to entry (risk-free).
        if (!pos_.be_done && tp_dist > 0.0 && pos_.mfe >= tp_dist * TSE_BE_FRAC) {
            pos_.sl      = pos_.entry;  // SL to breakeven
            pos_.trail_sl = pos_.entry; // sync trail SL
            pos_.be_done = true;
            std::cout << "[TSE-BE] " << pos_.pattern
                      << " " << (pos_.is_long ? "LONG" : "SHORT")
                      << " entry=" << std::fixed << std::setprecision(2) << pos_.entry
                      << " sl->BE mfe=" << std::setprecision(3) << pos_.mfe << "\n";
            std::cout.flush();
        }

        // ── P3-only trail: arms at TSE_P3_TRAIL_ARM pts MFE ─────────────────
        if (std::strncmp(pos_.pattern, "P3", 2) == 0) {
            if (!pos_.trail_armed && pos_.mfe >= TSE_P3_TRAIL_ARM) {
                pos_.trail_armed = true;
                std::cout << "[TSE-TRAIL-ARM] P3 " << (pos_.is_long ? "LONG" : "SHORT")
                          << " mfe=" << std::fixed << std::setprecision(3) << pos_.mfe << "\n";
                std::cout.flush();
            }
            if (pos_.trail_armed) {
                const double new_trail = pos_.is_long
                    ? (mid - TSE_P3_TRAIL_DIST)
                    : (mid + TSE_P3_TRAIL_DIST);
                // Ratchet: only move trail in profit direction, never past entry
                if (pos_.is_long  && new_trail > pos_.trail_sl) pos_.trail_sl = new_trail;
                if (!pos_.is_long && new_trail < pos_.trail_sl) pos_.trail_sl = new_trail;
                // Trail SL overrides hard SL once it is better (further in profit)
                if (pos_.is_long  && pos_.trail_sl > pos_.sl) pos_.sl = pos_.trail_sl;
                if (!pos_.is_long && pos_.trail_sl < pos_.sl) pos_.sl = pos_.trail_sl;
            }
        }

        // ── TP hit ────────────────────────────────────────────────────────────
        if (pos_.is_long ? (bid >= pos_.tp) : (ask <= pos_.tp)) {
            _close(eff_price, "TP_HIT", now_ms, on_close);
            return;
        }
        // ── SL / trail SL hit ─────────────────────────────────────────────────
        const bool sl_hit = pos_.is_long ? (bid <= pos_.sl) : (ask >= pos_.sl);
        if (sl_hit) {
            const char* reason = pos_.trail_armed ? "TRAIL_SL"
                               : pos_.be_done     ? "BE_HIT"
                               :                    "SL_HIT";
            _close(eff_price, reason, now_ms, on_close);
            return;
        }
        // ── Timeout: 2 minutes ────────────────────────────────────────────────
        if (now_ms - pos_.entry_ts_ms > 120000LL) {
            _close(eff_price, "TIMEOUT", now_ms, on_close);
            return;
        }
    }

    // ── Close ─────────────────────────────────────────────────────────────────
    void _close(double exit_px, const char* reason,
                int64_t now_ms, CloseCallback on_close) noexcept
    {
        const double pnl_pts = (pos_.is_long
            ? (exit_px - pos_.entry)
            : (pos_.entry - exit_px));
        const double pnl_usd = pnl_pts * pos_.size * 100.0;

        _daily_pnl   += pnl_usd;
        _last_exit_ms = now_ms;
        ++_total_trades;

        const bool win = (pnl_usd > 0);
        if (win) { ++_total_wins; _consec_losses = 0; }
        else {
            ++_consec_losses;
            if (_consec_losses >= TSE_MAX_CONSEC_LOSSES) {
                _pause_until_ms = now_ms + TSE_PAUSE_MS;
                std::cout << "[TSE] " << _consec_losses << " consecutive losses -- pausing 5 min\n";
                std::cout.flush();
                _consec_losses = 0;
            }
        }

        std::cout << "[TSE] EXIT " << pos_.pattern
                  << " " << (pos_.is_long ? "LONG" : "SHORT")
                  << " @ " << std::fixed << std::setprecision(2) << exit_px
                  << " " << reason
                  << " pnl=$" << std::setprecision(2) << pnl_usd
                  << " mfe=" << std::setprecision(2) << pos_.mfe
                  << " held=" << (long long)(now_ms - pos_.entry_ts_ms) / 1000LL << "s"
                  << " daily=$" << _daily_pnl
                  << " W/T=" << _total_wins << "/" << _total_trades
                  << (shadow_mode ? " [SHADOW]" : "") << "\n";
        std::cout.flush();

        if (on_close) {
            omega::TradeRecord tr;
            tr.id         = pos_.trade_id;
            tr.symbol     = "XAUUSD";
            tr.side       = pos_.is_long ? "LONG" : "SHORT";
            tr.engine     = "TickScalpEngine";
            tr.entryPrice = pos_.entry;
            tr.exitPrice  = exit_px;
            tr.sl         = pos_.sl;
            tr.size       = pos_.size;
            tr.pnl        = pnl_pts * pos_.size;
            tr.mfe        = pos_.mfe;
            tr.mae        = 0.0;
            tr.entryTs    = pos_.entry_ts_ms / 1000;
            tr.exitTs     = now_ms / 1000;
            tr.exitReason = reason;
            tr.regime     = "TICK_SCALP";
            tr.l2_live    = true;
            on_close(tr);
        }

        pos_ = OpenPos{};
    }

    // ── Daily reset ───────────────────────────────────────────────────────────
    void _daily_reset(int64_t now_ms) noexcept {
        const int64_t day = (now_ms / 1000LL) / 86400LL;
        if (day != _daily_day) {
            if (_daily_day > 0)
                std::cout << "[TSE] Daily reset. PnL=$" << std::fixed << std::setprecision(2) << _daily_pnl
                          << " W/T=" << _total_wins << "/" << _total_trades
                          << " WR=" << (int)(_total_trades > 0 ? 100.0 * _total_wins / _total_trades : 0.0) << "%\n";
            _daily_pnl = 0.0;
            _daily_day = day;
        }
    }
};

} // namespace omega



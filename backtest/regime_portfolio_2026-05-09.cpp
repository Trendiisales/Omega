// =============================================================================
// regime_portfolio_2026-05-09.cpp
// =============================================================================
// Tier 3 portfolio replay: regime-routed CHOP + TREND engines on XAUUSD.
//
// Tests whether running both engines in parallel, each gated by a real-time
// regime classifier, produces better aggregate P&L than either engine alone.
//
// COMPONENTS (single self-contained TU, no external deps):
//   1. Chop engine     -- exact port of GoldMicroScalperEngine rk12 (the
//                         current production geometry post-S23 revert):
//                         z-score fade entry, BE-arm + tight trail, TP=0.79
//                         SL=3.00 MAX_HOLD=60s.
//   2. Trend engine    -- Donchian-style breakout (NEW). Enters in the
//                         direction of a 60-tick high/low breakout that
//                         exceeds the boundary by BREAKOUT_BUFFER. Holds
//                         longer (MAX_HOLD=180s), wider TP=2.00 SL=1.50,
//                         BE_TRIGGER=1.00. The literal opposite of the
//                         chop engine's "fade the stretch" thesis.
//   3. Regime detector -- Kaufman Efficiency Ratio over 200 ticks:
//                           ER = |last - first| / sum(|tick - prev|)
//                         ER ~ 0 = perfect chop (price wandering).
//                         ER ~ 1 = perfect trend (price walking).
//                         Threshold 0.25 separates the two regimes.
//
// PORTFOLIO RULES:
//   - Chop engine accepts NEW entries only when regime detector says chop.
//   - Trend engine accepts NEW entries only when regime detector says trend.
//   - Position MANAGEMENT runs unconditionally for both engines (don't
//     abandon an open position just because regime flipped).
//   - Both engines maintain independent state (cooldown, position, etc.).
//   - Combined P&L = sum of both engines' realised P&L per tape.
//
// OUTPUT (per tape):
//   A. CHOP-ONLY    -- rk12 with regime gate disabled (= current production)
//   B. TREND-ONLY   -- breakout with regime gate disabled (pure trend strat)
//   C. PORTFOLIO    -- both engines, regime-gated entries
//
// BUILD:
//   clang++ -std=c++17 -O3 -DNDEBUG \
//       backtest/regime_portfolio_2026-05-09.cpp \
//       -o backtest/regime_portfolio_2026-05-09
//
// USAGE:
//   ./backtest/regime_portfolio_2026-05-09 [--no-session] <tape.csv>
//
// AUTHORISATION TRAIL: produced for user request 2026-05-09 in chat
// ("no do tier 3, make a backup of what we have no and give me tier 3 and
// then we test it").
// =============================================================================

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <deque>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

// -----------------------------------------------------------------------------
// Common types
// -----------------------------------------------------------------------------
struct Tick {
    int64_t ts_ms;
    double  bid;
    double  ask;
};

struct Stats {
    int    trades         = 0;
    int    wins           = 0;
    double gross_pnl_pts  = 0.0;     // pt*lot units
    double mfe_sum_pts    = 0.0;
    double mae_sum_pts    = 0.0;
    int    tp_hits        = 0;
    int    be_hits        = 0;
    int    trail_hits     = 0;
    int    sl_hits        = 0;
    int    rev_exits      = 0;
    int    max_hold_exits = 0;
    double max_dd_pts     = 0.0;
    double avg_hold_s     = 0.0;
};

// -----------------------------------------------------------------------------
// Geometry
// -----------------------------------------------------------------------------
struct ChopGeom {
    double entry_z          = 0.75;
    double tp_dist          = 0.79;
    double sl_dist          = 3.00;
    double be_trigger       = 0.50;
    double be_offset        = 0.30;
    double trail_dist       = 0.50;
    double reversal_delta   = 0.30;
    int    entry_lookback   = 20;
    int    reversal_lb      = 5;
    double max_spread       = 0.5;
    int    max_hold_sec     = 60;
    int    cooldown_s       = 5;
    int    min_entry_ticks  = 30;
    double min_sd           = 0.05;
    double live_lot         = 0.01;
};

struct TrendGeom {
    double tp_dist          = 2.00;
    double sl_dist          = 1.50;
    double be_trigger       = 1.00;
    double be_offset        = 0.30;
    double trail_dist       = 0.50;
    double breakout_buffer  = 0.20;     // pt above 60-tick high (or below low)
    int    breakout_window  = 60;       // ticks
    double max_spread       = 1.00;     // looser; trend days have wider spread
    int    max_hold_sec     = 180;
    int    cooldown_s       = 10;
    int    min_entry_ticks  = 60;
    double live_lot         = 0.01;
};

struct PortfolioConfig {
    int    sess_start       = 6;        // UTC
    int    sess_end         = 22;
    bool   session_enabled  = true;
    int    er_window        = 200;
    double er_threshold     = 0.25;
};

// -----------------------------------------------------------------------------
// Regime classifier (Kaufman Efficiency Ratio)
//   ER = |price_now - price_n_ago| / sum(|price_i - price_{i-1}|)
//   ER -> 0 = pure chop;  ER -> 1 = pure trend
// -----------------------------------------------------------------------------
struct Regime {
    int    window;
    double threshold;
    std::deque<double> prices;

    double current_er  = 0.0;
    bool   is_trend    = false;
    int    chop_ticks  = 0;
    int    trend_ticks = 0;
    int    warm_ticks  = 0;

    Regime() : window(200), threshold(0.25) {}
    Regime(int w, double t) : window(w), threshold(t) {}

    void on_tick(double mid) {
        prices.push_back(mid);
        if ((int)prices.size() > window) prices.pop_front();
        if ((int)prices.size() < window) {
            warm_ticks++;
            return;
        }
        const double net = std::fabs(prices.back() - prices.front());
        double gross = 0.0;
        for (size_t i = 1; i < prices.size(); ++i) {
            gross += std::fabs(prices[i] - prices[i - 1]);
        }
        current_er = (gross > 1e-9) ? (net / gross) : 0.0;
        const bool prev = is_trend;
        is_trend = (current_er >= threshold);
        (void)prev;
        if (is_trend) trend_ticks++; else chop_ticks++;
    }
};

// -----------------------------------------------------------------------------
// Tape parser (same format as L2 capture: ts_ms,mid,bid,ask,...)
// -----------------------------------------------------------------------------
static bool parse_tick(const std::string& line, Tick& t) {
    int field = 0;
    size_t pos = 0;
    int64_t ts_ms = 0;
    double bid = 0.0, ask = 0.0;
    while (pos <= line.size()) {
        size_t nxt = line.find(',', pos);
        if (nxt == std::string::npos) nxt = line.size();
        const char* s = line.c_str() + pos;
        switch (field) {
            case 0: ts_ms = std::strtoll(s, nullptr, 10); break;
            case 2: bid   = std::strtod (s, nullptr);     break;
            case 3: ask   = std::strtod (s, nullptr);     break;
            default: break;
        }
        ++field;
        if (nxt == line.size()) break;
        pos = nxt + 1;
    }
    if (ts_ms <= 0 || bid <= 0.0 || ask <= 0.0 || ask <= bid) return false;
    t.ts_ms = ts_ms;
    t.bid   = bid;
    t.ask   = ask;
    return true;
}

// -----------------------------------------------------------------------------
// Generic position state shared by both engines
// -----------------------------------------------------------------------------
struct Pos {
    bool    active     = false;
    bool    is_long    = false;
    double  entry      = 0.0;
    double  sl         = 0.0;
    double  tp         = 0.0;
    double  mfe        = 0.0;
    double  mae        = 0.0;
    int64_t entry_ts   = 0;
    bool    be_locked  = false;
};

// -----------------------------------------------------------------------------
// Helper: close a position, accumulate stats. Lot multiplier baked into pnl.
// -----------------------------------------------------------------------------
static void close_pos(Pos& pos, double exit_px, const char* reason,
                      int64_t now_s, double live_lot, Stats& out,
                      double& cum, double& peak, int& cooldown_dir,
                      int64_t& cooldown_start, double& total_hold_s)
{
    const double pnl = (pos.is_long ? (exit_px - pos.entry)
                                    : (pos.entry - exit_px)) * live_lot;
    out.gross_pnl_pts += pnl;
    out.mfe_sum_pts   += pos.mfe * live_lot;
    out.mae_sum_pts   += pos.mae * live_lot;
    out.trades++;
    if (pnl > 0) out.wins++;
    if      (!std::strcmp(reason, "TP_HIT"))         out.tp_hits++;
    else if (!std::strcmp(reason, "BE_HIT"))         out.be_hits++;
    else if (!std::strcmp(reason, "TRAIL_HIT"))      out.trail_hits++;
    else if (!std::strcmp(reason, "SL_HIT"))         out.sl_hits++;
    else if (!std::strcmp(reason, "REVERSAL_EXIT"))  out.rev_exits++;
    else if (!std::strcmp(reason, "MAX_HOLD_EXIT"))  out.max_hold_exits++;

    cum += pnl;
    if (cum > peak) peak = cum;
    const double dd = peak - cum;
    if (dd > out.max_dd_pts) out.max_dd_pts = dd;
    total_hold_s += (double)(now_s - pos.entry_ts);

    cooldown_start = now_s;
    cooldown_dir   = pos.is_long ? +1 : -1;
    pos.active = false;
}

// -----------------------------------------------------------------------------
// Session-window check
// -----------------------------------------------------------------------------
static bool in_session(int64_t now_s, int sess_start, int sess_end,
                       bool session_enabled)
{
    if (!session_enabled) return true;
    const std::time_t t = (std::time_t)now_s;
    std::tm utc{};
    gmtime_r(&t, &utc);
    const int  h = utc.tm_hour;
    return (sess_end > sess_start)
        ? (h >= sess_start && h <  sess_end)
        : (h >= sess_start || h <  sess_end);
}

// -----------------------------------------------------------------------------
// CHOP ENGINE simulation (port of GoldMicroScalperEngine rk12)
//   regime_filter == "chop"  -> only fire entries when ER < threshold
//   regime_filter == "all"   -> ignore regime, fire whenever z-signal trips
// -----------------------------------------------------------------------------
static void simulate_chop(const std::vector<Tick>& ticks,
                          const ChopGeom& g,
                          const PortfolioConfig& pc,
                          const char* regime_filter,
                          Regime& regime,
                          Stats& out)
{
    enum class Phase { IDLE, COOLDOWN, LIVE };
    Phase   phase = Phase::IDLE;
    int64_t cooldown_start = 0;
    int     cooldown_dir   = 0;

    Pos pos;
    std::deque<double> window;
    std::deque<double> micro;

    int     ticks_received = 0;
    double  cum            = 0.0;
    double  peak           = 0.0;
    double  total_hold_s   = 0.0;

    const bool gate_on_chop = !std::strcmp(regime_filter, "chop");

    for (const auto& tk : ticks) {
        const double  mid    = (tk.bid + tk.ask) * 0.5;
        const double  spread = tk.ask - tk.bid;
        const int64_t now_s  = tk.ts_ms / 1000;

        // Regime detector advances on every tick (independent of engine)
        regime.on_tick(mid);

        ++ticks_received;
        window.push_back(mid);
        if ((int)window.size() > g.entry_lookback * 4) window.pop_front();
        micro.push_back(mid);
        if ((int)micro.size() > g.reversal_lb * 4) micro.pop_front();

        if (phase == Phase::COOLDOWN) {
            if (now_s - cooldown_start >= g.cooldown_s) {
                phase = Phase::IDLE;
                cooldown_dir = 0;
            }
        }

        // -- Manage existing position --------------------------------------
        if (phase == Phase::LIVE && pos.active) {
            const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
            if (move > pos.mfe) pos.mfe = move;
            if (move < pos.mae) pos.mae = move;

            // BE-arm
            if (!pos.be_locked && pos.mfe >= g.be_trigger) {
                const double off = (pos.mfe >= g.be_offset) ? g.be_offset : 0.0;
                const double be_target = pos.is_long
                    ? (pos.entry + off)
                    : (pos.entry - off);
                if ( pos.is_long && be_target > pos.sl) pos.sl = be_target;
                if (!pos.is_long && be_target < pos.sl) pos.sl = be_target;
                pos.be_locked = true;
            }

            // Trail
            if (pos.be_locked) {
                const double trail_sl = pos.is_long
                    ? (pos.entry + pos.mfe - g.trail_dist)
                    : (pos.entry - pos.mfe + g.trail_dist);
                if ( pos.is_long && trail_sl > pos.sl) pos.sl = trail_sl;
                if (!pos.is_long && trail_sl < pos.sl) pos.sl = trail_sl;
            }

            // Reversal exit (post-BE)
            if (pos.be_locked) {
                const int n = (int)micro.size();
                if (n >= g.reversal_lb) {
                    const double last  = micro[n - 1];
                    const double prior = micro[n - g.reversal_lb];
                    const double delta = last - prior;
                    bool rev = false;
                    if ( pos.is_long && delta <= -g.reversal_delta) rev = true;
                    if (!pos.is_long && delta >=  g.reversal_delta) rev = true;
                    if (rev) {
                        const double exit_px = pos.is_long ? tk.bid : tk.ask;
                        close_pos(pos, exit_px, "REVERSAL_EXIT", now_s,
                                  g.live_lot, out, cum, peak,
                                  cooldown_dir, cooldown_start, total_hold_s);
                        phase = Phase::COOLDOWN;
                        continue;
                    }
                }
            }

            // TP_HIT
            const bool tp_hit = pos.is_long ? (tk.ask >= pos.tp) : (tk.bid <= pos.tp);
            if (tp_hit) {
                close_pos(pos, pos.tp, "TP_HIT", now_s,
                          g.live_lot, out, cum, peak,
                          cooldown_dir, cooldown_start, total_hold_s);
                phase = Phase::COOLDOWN;
                continue;
            }

            // SL_HIT
            const bool sl_hit = pos.is_long ? (tk.bid <= pos.sl) : (tk.ask >= pos.sl);
            if (sl_hit) {
                const double exit_px       = pos.is_long ? tk.bid : tk.ask;
                const bool   sl_at_be      = std::fabs(pos.sl - pos.entry) <= 0.05;
                const bool   trail_in_prof = pos.is_long
                    ? (pos.sl > pos.entry + 0.05)
                    : (pos.sl < pos.entry - 0.05);
                const char*  reason = sl_at_be      ? "BE_HIT"
                                    : trail_in_prof ? "TRAIL_HIT"
                                                    : "SL_HIT";
                close_pos(pos, exit_px, reason, now_s,
                          g.live_lot, out, cum, peak,
                          cooldown_dir, cooldown_start, total_hold_s);
                phase = Phase::COOLDOWN;
                continue;
            }

            // MAX_HOLD timeout
            if ((now_s - pos.entry_ts) >= g.max_hold_sec) {
                close_pos(pos, mid, "MAX_HOLD_EXIT", now_s,
                          g.live_lot, out, cum, peak,
                          cooldown_dir, cooldown_start, total_hold_s);
                phase = Phase::COOLDOWN;
                continue;
            }

            continue;
        }

        // -- New entry --------------------------------------------------------
        if (ticks_received < g.min_entry_ticks) continue;
        if ((int)window.size() < g.entry_lookback) continue;
        if (spread > g.max_spread) continue;
        if (!in_session(now_s, pc.sess_start, pc.sess_end, pc.session_enabled)) continue;

        // Regime gate (only when in chop-only mode)
        if (gate_on_chop && regime.is_trend) continue;

        // z-score
        const int  n  = (int)window.size();
        const int  st = n - g.entry_lookback;
        double sum = 0.0;
        for (int i = st; i < n; ++i) sum += window[i];
        const double mean = sum / (double)g.entry_lookback;
        double var = 0.0;
        for (int i = st; i < n; ++i) {
            const double d = window[i] - mean;
            var += d * d;
        }
        const double sd = std::sqrt(var / (double)g.entry_lookback);
        if (sd < g.min_sd) continue;
        const double z = (mid - mean) / sd;

        const bool block_long  = (phase == Phase::COOLDOWN && cooldown_dir == +1);
        const bool block_short = (phase == Phase::COOLDOWN && cooldown_dir == -1);
        const bool z_long      = (z <= -g.entry_z);
        const bool z_short     = (z >=  g.entry_z);
        const bool fire_long   = z_long  && !block_long;
        const bool fire_short  = z_short && !block_short;
        if (!fire_long && !fire_short) continue;
        if ( fire_long &&  fire_short) continue;

        const bool   is_long = fire_long;
        const double fill_px = is_long ? tk.ask : tk.bid;
        pos.active     = true;
        pos.is_long    = is_long;
        pos.entry      = fill_px;
        pos.sl         = is_long ? (fill_px - g.sl_dist) : (fill_px + g.sl_dist);
        pos.tp         = is_long ? (fill_px + g.tp_dist) : (fill_px - g.tp_dist);
        pos.mfe        = 0.0;
        pos.mae        = 0.0;
        pos.be_locked  = false;
        pos.entry_ts   = now_s;
        phase          = Phase::LIVE;
    }

    out.avg_hold_s = (out.trades > 0) ? (total_hold_s / (double)out.trades) : 0.0;
}

// -----------------------------------------------------------------------------
// TREND ENGINE simulation (Donchian-style breakout)
//
// ENTRY:
//   maintain rolling 60-tick max(bid) and min(ask)
//   LONG  on ask if ask > max_high + breakout_buffer
//   SHORT on bid if bid < min_low  - breakout_buffer
//
// MANAGEMENT:
//   Same BE-arm + trail + reversal-exit pattern as chop engine, but with
//   wider TP/SL and longer MAX_HOLD. No L2 confirmation; pure price action.
// -----------------------------------------------------------------------------
static void simulate_trend(const std::vector<Tick>& ticks,
                           const TrendGeom& g,
                           const PortfolioConfig& pc,
                           const char* regime_filter,
                           Regime& regime,
                           Stats& out)
{
    enum class Phase { IDLE, COOLDOWN, LIVE };
    Phase   phase = Phase::IDLE;
    int64_t cooldown_start = 0;
    int     cooldown_dir   = 0;

    Pos pos;

    std::deque<double> highs;   // bid history
    std::deque<double> lows;    // ask history

    int    ticks_received = 0;
    double cum            = 0.0;
    double peak           = 0.0;
    double total_hold_s   = 0.0;

    const bool gate_on_trend = !std::strcmp(regime_filter, "trend");

    for (const auto& tk : ticks) {
        const double  mid    = (tk.bid + tk.ask) * 0.5;
        const double  spread = tk.ask - tk.bid;
        const int64_t now_s  = tk.ts_ms / 1000;

        // We do NOT advance regime here -- chop simulator already did.
        // (When this is called standalone, the regime may not be advanced;
        //  that's fine because gate_on_trend=false in standalone modes.)
        (void)mid;

        ++ticks_received;
        highs.push_back(tk.bid);
        lows.push_back(tk.ask);
        if ((int)highs.size() > g.breakout_window) highs.pop_front();
        if ((int)lows.size()  > g.breakout_window) lows.pop_front();

        if (phase == Phase::COOLDOWN) {
            if (now_s - cooldown_start >= g.cooldown_s) {
                phase = Phase::IDLE;
                cooldown_dir = 0;
            }
        }

        // -- Manage existing position --------------------------------------
        if (phase == Phase::LIVE && pos.active) {
            const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
            if (move > pos.mfe) pos.mfe = move;
            if (move < pos.mae) pos.mae = move;

            // BE-arm
            if (!pos.be_locked && pos.mfe >= g.be_trigger) {
                const double off = (pos.mfe >= g.be_offset) ? g.be_offset : 0.0;
                const double be_target = pos.is_long
                    ? (pos.entry + off)
                    : (pos.entry - off);
                if ( pos.is_long && be_target > pos.sl) pos.sl = be_target;
                if (!pos.is_long && be_target < pos.sl) pos.sl = be_target;
                pos.be_locked = true;
            }

            // Trail
            if (pos.be_locked) {
                const double trail_sl = pos.is_long
                    ? (pos.entry + pos.mfe - g.trail_dist)
                    : (pos.entry - pos.mfe + g.trail_dist);
                if ( pos.is_long && trail_sl > pos.sl) pos.sl = trail_sl;
                if (!pos.is_long && trail_sl < pos.sl) pos.sl = trail_sl;
            }

            // TP_HIT
            const bool tp_hit = pos.is_long ? (tk.ask >= pos.tp) : (tk.bid <= pos.tp);
            if (tp_hit) {
                close_pos(pos, pos.tp, "TP_HIT", now_s,
                          g.live_lot, out, cum, peak,
                          cooldown_dir, cooldown_start, total_hold_s);
                phase = Phase::COOLDOWN;
                continue;
            }

            // SL_HIT
            const bool sl_hit = pos.is_long ? (tk.bid <= pos.sl) : (tk.ask >= pos.sl);
            if (sl_hit) {
                const double exit_px       = pos.is_long ? tk.bid : tk.ask;
                const bool   sl_at_be      = std::fabs(pos.sl - pos.entry) <= 0.05;
                const bool   trail_in_prof = pos.is_long
                    ? (pos.sl > pos.entry + 0.05)
                    : (pos.sl < pos.entry - 0.05);
                const char*  reason = sl_at_be      ? "BE_HIT"
                                    : trail_in_prof ? "TRAIL_HIT"
                                                    : "SL_HIT";
                close_pos(pos, exit_px, reason, now_s,
                          g.live_lot, out, cum, peak,
                          cooldown_dir, cooldown_start, total_hold_s);
                phase = Phase::COOLDOWN;
                continue;
            }

            // MAX_HOLD timeout
            if ((now_s - pos.entry_ts) >= g.max_hold_sec) {
                close_pos(pos, mid, "MAX_HOLD_EXIT", now_s,
                          g.live_lot, out, cum, peak,
                          cooldown_dir, cooldown_start, total_hold_s);
                phase = Phase::COOLDOWN;
                continue;
            }
            continue;
        }

        // -- New entry: Donchian breakout -----------------------------------
        if (ticks_received < g.min_entry_ticks) continue;
        if ((int)highs.size() < g.breakout_window) continue;
        if (spread > g.max_spread) continue;
        if (!in_session(now_s, pc.sess_start, pc.sess_end, pc.session_enabled)) continue;

        // Regime gate (only when in trend-only mode)
        if (gate_on_trend && !regime.is_trend) continue;

        const double max_high = *std::max_element(highs.begin(), highs.end());
        const double min_low  = *std::min_element(lows.begin(),  lows.end());

        const bool block_long  = (phase == Phase::COOLDOWN && cooldown_dir == +1);
        const bool block_short = (phase == Phase::COOLDOWN && cooldown_dir == -1);

        const bool break_up   = tk.ask >= max_high + g.breakout_buffer;
        const bool break_down = tk.bid <= min_low  - g.breakout_buffer;
        const bool fire_long  = break_up   && !block_long;
        const bool fire_short = break_down && !block_short;
        if (!fire_long && !fire_short) continue;
        if ( fire_long &&  fire_short) continue;

        const bool   is_long = fire_long;
        const double fill_px = is_long ? tk.ask : tk.bid;
        pos.active     = true;
        pos.is_long    = is_long;
        pos.entry      = fill_px;
        pos.sl         = is_long ? (fill_px - g.sl_dist) : (fill_px + g.sl_dist);
        pos.tp         = is_long ? (fill_px + g.tp_dist) : (fill_px - g.tp_dist);
        pos.mfe        = 0.0;
        pos.mae        = 0.0;
        pos.be_locked  = false;
        pos.entry_ts   = now_s;
        phase          = Phase::LIVE;
    }

    out.avg_hold_s = (out.trades > 0) ? (total_hold_s / (double)out.trades) : 0.0;
}

// -----------------------------------------------------------------------------
// PORTFOLIO simulation: chop + trend, both gated by regime.
// Run sequentially: first chop (which advances the regime detector), then
// trend (using the same regime detector state per tick by re-iterating with
// the regime tracker we just primed). For correctness we instead run them
// truly in lockstep below in a single loop -- duplicates some logic but
// keeps regime visibility per-tick aligned for both engines.
// -----------------------------------------------------------------------------
static void simulate_portfolio(const std::vector<Tick>& ticks,
                               const ChopGeom& cg,
                               const TrendGeom& tg,
                               const PortfolioConfig& pc,
                               Regime& regime,
                               Stats& chop_out,
                               Stats& trend_out)
{
    // ---- Chop engine state ----
    enum class CPhase { IDLE, COOLDOWN, LIVE };
    CPhase  c_phase  = CPhase::IDLE;
    int64_t c_cd_st  = 0;
    int     c_cd_dir = 0;
    Pos     c_pos;
    std::deque<double> c_window;
    std::deque<double> c_micro;
    double  c_cum    = 0.0;
    double  c_peak   = 0.0;
    double  c_hold_s = 0.0;

    // ---- Trend engine state ----
    enum class TPhase { IDLE, COOLDOWN, LIVE };
    TPhase  t_phase  = TPhase::IDLE;
    int64_t t_cd_st  = 0;
    int     t_cd_dir = 0;
    Pos     t_pos;
    std::deque<double> t_highs;
    std::deque<double> t_lows;
    double  t_cum    = 0.0;
    double  t_peak   = 0.0;
    double  t_hold_s = 0.0;

    int ticks_received = 0;

    for (const auto& tk : ticks) {
        const double  mid    = (tk.bid + tk.ask) * 0.5;
        const double  spread = tk.ask - tk.bid;
        const int64_t now_s  = tk.ts_ms / 1000;

        // Advance regime
        regime.on_tick(mid);

        ++ticks_received;

        // ---------- CHOP engine ----------
        c_window.push_back(mid);
        if ((int)c_window.size() > cg.entry_lookback * 4) c_window.pop_front();
        c_micro.push_back(mid);
        if ((int)c_micro.size() > cg.reversal_lb * 4) c_micro.pop_front();

        if (c_phase == CPhase::COOLDOWN) {
            if (now_s - c_cd_st >= cg.cooldown_s) {
                c_phase  = CPhase::IDLE;
                c_cd_dir = 0;
            }
        }

        bool c_handled_this_tick = false;
        if (c_phase == CPhase::LIVE && c_pos.active) {
            const double move = c_pos.is_long ? (mid - c_pos.entry) : (c_pos.entry - mid);
            if (move > c_pos.mfe) c_pos.mfe = move;
            if (move < c_pos.mae) c_pos.mae = move;

            if (!c_pos.be_locked && c_pos.mfe >= cg.be_trigger) {
                const double off = (c_pos.mfe >= cg.be_offset) ? cg.be_offset : 0.0;
                const double bt = c_pos.is_long
                    ? (c_pos.entry + off)
                    : (c_pos.entry - off);
                if ( c_pos.is_long && bt > c_pos.sl) c_pos.sl = bt;
                if (!c_pos.is_long && bt < c_pos.sl) c_pos.sl = bt;
                c_pos.be_locked = true;
            }
            if (c_pos.be_locked) {
                const double trail_sl = c_pos.is_long
                    ? (c_pos.entry + c_pos.mfe - cg.trail_dist)
                    : (c_pos.entry - c_pos.mfe + cg.trail_dist);
                if ( c_pos.is_long && trail_sl > c_pos.sl) c_pos.sl = trail_sl;
                if (!c_pos.is_long && trail_sl < c_pos.sl) c_pos.sl = trail_sl;
            }
            if (c_pos.be_locked) {
                const int n = (int)c_micro.size();
                if (n >= cg.reversal_lb) {
                    const double last  = c_micro[n - 1];
                    const double prior = c_micro[n - cg.reversal_lb];
                    const double delta = last - prior;
                    bool rev = false;
                    if ( c_pos.is_long && delta <= -cg.reversal_delta) rev = true;
                    if (!c_pos.is_long && delta >=  cg.reversal_delta) rev = true;
                    if (rev) {
                        const double exit_px = c_pos.is_long ? tk.bid : tk.ask;
                        close_pos(c_pos, exit_px, "REVERSAL_EXIT", now_s,
                                  cg.live_lot, chop_out, c_cum, c_peak,
                                  c_cd_dir, c_cd_st, c_hold_s);
                        c_phase = CPhase::COOLDOWN;
                        c_handled_this_tick = true;
                    }
                }
            }
            if (!c_handled_this_tick) {
                const bool tp_hit = c_pos.is_long ? (tk.ask >= c_pos.tp) : (tk.bid <= c_pos.tp);
                if (tp_hit) {
                    close_pos(c_pos, c_pos.tp, "TP_HIT", now_s,
                              cg.live_lot, chop_out, c_cum, c_peak,
                              c_cd_dir, c_cd_st, c_hold_s);
                    c_phase = CPhase::COOLDOWN;
                    c_handled_this_tick = true;
                }
            }
            if (!c_handled_this_tick) {
                const bool sl_hit = c_pos.is_long ? (tk.bid <= c_pos.sl) : (tk.ask >= c_pos.sl);
                if (sl_hit) {
                    const double exit_px       = c_pos.is_long ? tk.bid : tk.ask;
                    const bool   sl_at_be      = std::fabs(c_pos.sl - c_pos.entry) <= 0.05;
                    const bool   trail_in_prof = c_pos.is_long
                        ? (c_pos.sl > c_pos.entry + 0.05)
                        : (c_pos.sl < c_pos.entry - 0.05);
                    const char*  reason = sl_at_be      ? "BE_HIT"
                                        : trail_in_prof ? "TRAIL_HIT"
                                                        : "SL_HIT";
                    close_pos(c_pos, exit_px, reason, now_s,
                              cg.live_lot, chop_out, c_cum, c_peak,
                              c_cd_dir, c_cd_st, c_hold_s);
                    c_phase = CPhase::COOLDOWN;
                    c_handled_this_tick = true;
                }
            }
            if (!c_handled_this_tick) {
                if ((now_s - c_pos.entry_ts) >= cg.max_hold_sec) {
                    close_pos(c_pos, mid, "MAX_HOLD_EXIT", now_s,
                              cg.live_lot, chop_out, c_cum, c_peak,
                              c_cd_dir, c_cd_st, c_hold_s);
                    c_phase = CPhase::COOLDOWN;
                    c_handled_this_tick = true;
                }
            }
        }

        // Chop new-entry path (gated to chop regime)
        if (!c_handled_this_tick && c_phase != CPhase::LIVE) {
            bool ok = true;
            if (ticks_received < cg.min_entry_ticks) ok = false;
            if ((int)c_window.size() < cg.entry_lookback) ok = false;
            if (spread > cg.max_spread) ok = false;
            if (!in_session(now_s, pc.sess_start, pc.sess_end, pc.session_enabled)) ok = false;
            if (regime.is_trend) ok = false;  // PORTFOLIO GATE: chop only in chop regime

            if (ok) {
                const int  n  = (int)c_window.size();
                const int  st = n - cg.entry_lookback;
                double sum = 0.0;
                for (int i = st; i < n; ++i) sum += c_window[i];
                const double mean = sum / (double)cg.entry_lookback;
                double var = 0.0;
                for (int i = st; i < n; ++i) {
                    const double d = c_window[i] - mean;
                    var += d * d;
                }
                const double sd = std::sqrt(var / (double)cg.entry_lookback);
                if (sd >= cg.min_sd) {
                    const double z = (mid - mean) / sd;
                    const bool block_long  = (c_phase == CPhase::COOLDOWN && c_cd_dir == +1);
                    const bool block_short = (c_phase == CPhase::COOLDOWN && c_cd_dir == -1);
                    const bool z_long      = (z <= -cg.entry_z);
                    const bool z_short     = (z >=  cg.entry_z);
                    const bool fire_long   = z_long  && !block_long;
                    const bool fire_short  = z_short && !block_short;
                    if ((fire_long || fire_short) && !(fire_long && fire_short)) {
                        const bool   is_long = fire_long;
                        const double fill_px = is_long ? tk.ask : tk.bid;
                        c_pos.active    = true;
                        c_pos.is_long   = is_long;
                        c_pos.entry     = fill_px;
                        c_pos.sl        = is_long ? (fill_px - cg.sl_dist) : (fill_px + cg.sl_dist);
                        c_pos.tp        = is_long ? (fill_px + cg.tp_dist) : (fill_px - cg.tp_dist);
                        c_pos.mfe       = 0.0;
                        c_pos.mae       = 0.0;
                        c_pos.be_locked = false;
                        c_pos.entry_ts  = now_s;
                        c_phase         = CPhase::LIVE;
                    }
                }
            }
        }

        // ---------- TREND engine ----------
        t_highs.push_back(tk.bid);
        t_lows.push_back(tk.ask);
        if ((int)t_highs.size() > tg.breakout_window) t_highs.pop_front();
        if ((int)t_lows.size()  > tg.breakout_window) t_lows.pop_front();

        if (t_phase == TPhase::COOLDOWN) {
            if (now_s - t_cd_st >= tg.cooldown_s) {
                t_phase  = TPhase::IDLE;
                t_cd_dir = 0;
            }
        }

        bool t_handled_this_tick = false;
        if (t_phase == TPhase::LIVE && t_pos.active) {
            const double move = t_pos.is_long ? (mid - t_pos.entry) : (t_pos.entry - mid);
            if (move > t_pos.mfe) t_pos.mfe = move;
            if (move < t_pos.mae) t_pos.mae = move;

            if (!t_pos.be_locked && t_pos.mfe >= tg.be_trigger) {
                const double off = (t_pos.mfe >= tg.be_offset) ? tg.be_offset : 0.0;
                const double bt = t_pos.is_long
                    ? (t_pos.entry + off)
                    : (t_pos.entry - off);
                if ( t_pos.is_long && bt > t_pos.sl) t_pos.sl = bt;
                if (!t_pos.is_long && bt < t_pos.sl) t_pos.sl = bt;
                t_pos.be_locked = true;
            }
            if (t_pos.be_locked) {
                const double trail_sl = t_pos.is_long
                    ? (t_pos.entry + t_pos.mfe - tg.trail_dist)
                    : (t_pos.entry - t_pos.mfe + tg.trail_dist);
                if ( t_pos.is_long && trail_sl > t_pos.sl) t_pos.sl = trail_sl;
                if (!t_pos.is_long && trail_sl < t_pos.sl) t_pos.sl = trail_sl;
            }

            const bool tp_hit = t_pos.is_long ? (tk.ask >= t_pos.tp) : (tk.bid <= t_pos.tp);
            if (tp_hit) {
                close_pos(t_pos, t_pos.tp, "TP_HIT", now_s,
                          tg.live_lot, trend_out, t_cum, t_peak,
                          t_cd_dir, t_cd_st, t_hold_s);
                t_phase = TPhase::COOLDOWN;
                t_handled_this_tick = true;
            }
            if (!t_handled_this_tick) {
                const bool sl_hit = t_pos.is_long ? (tk.bid <= t_pos.sl) : (tk.ask >= t_pos.sl);
                if (sl_hit) {
                    const double exit_px       = t_pos.is_long ? tk.bid : tk.ask;
                    const bool   sl_at_be      = std::fabs(t_pos.sl - t_pos.entry) <= 0.05;
                    const bool   trail_in_prof = t_pos.is_long
                        ? (t_pos.sl > t_pos.entry + 0.05)
                        : (t_pos.sl < t_pos.entry - 0.05);
                    const char*  reason = sl_at_be      ? "BE_HIT"
                                        : trail_in_prof ? "TRAIL_HIT"
                                                        : "SL_HIT";
                    close_pos(t_pos, exit_px, reason, now_s,
                              tg.live_lot, trend_out, t_cum, t_peak,
                              t_cd_dir, t_cd_st, t_hold_s);
                    t_phase = TPhase::COOLDOWN;
                    t_handled_this_tick = true;
                }
            }
            if (!t_handled_this_tick) {
                if ((now_s - t_pos.entry_ts) >= tg.max_hold_sec) {
                    close_pos(t_pos, mid, "MAX_HOLD_EXIT", now_s,
                              tg.live_lot, trend_out, t_cum, t_peak,
                              t_cd_dir, t_cd_st, t_hold_s);
                    t_phase = TPhase::COOLDOWN;
                    t_handled_this_tick = true;
                }
            }
        }

        // Trend new-entry (gated to trend regime)
        if (!t_handled_this_tick && t_phase != TPhase::LIVE) {
            bool ok = true;
            if (ticks_received < tg.min_entry_ticks) ok = false;
            if ((int)t_highs.size() < tg.breakout_window) ok = false;
            if (spread > tg.max_spread) ok = false;
            if (!in_session(now_s, pc.sess_start, pc.sess_end, pc.session_enabled)) ok = false;
            if (!regime.is_trend) ok = false;  // PORTFOLIO GATE: trend only in trend regime

            if (ok) {
                const double max_high = *std::max_element(t_highs.begin(), t_highs.end());
                const double min_low  = *std::min_element(t_lows.begin(),  t_lows.end());
                const bool block_long  = (t_phase == TPhase::COOLDOWN && t_cd_dir == +1);
                const bool block_short = (t_phase == TPhase::COOLDOWN && t_cd_dir == -1);
                const bool break_up   = tk.ask >= max_high + tg.breakout_buffer;
                const bool break_down = tk.bid <= min_low  - tg.breakout_buffer;
                const bool fire_long  = break_up   && !block_long;
                const bool fire_short = break_down && !block_short;
                if ((fire_long || fire_short) && !(fire_long && fire_short)) {
                    const bool   is_long = fire_long;
                    const double fill_px = is_long ? tk.ask : tk.bid;
                    t_pos.active    = true;
                    t_pos.is_long   = is_long;
                    t_pos.entry     = fill_px;
                    t_pos.sl        = is_long ? (fill_px - tg.sl_dist) : (fill_px + tg.sl_dist);
                    t_pos.tp        = is_long ? (fill_px + tg.tp_dist) : (fill_px - tg.tp_dist);
                    t_pos.mfe       = 0.0;
                    t_pos.mae       = 0.0;
                    t_pos.be_locked = false;
                    t_pos.entry_ts  = now_s;
                    t_phase         = TPhase::LIVE;
                }
            }
        }
    }

    chop_out.avg_hold_s  = (chop_out.trades  > 0) ? (c_hold_s / (double)chop_out.trades)  : 0.0;
    trend_out.avg_hold_s = (trend_out.trades > 0) ? (t_hold_s / (double)trend_out.trades) : 0.0;
}

// -----------------------------------------------------------------------------
// Reporting
// -----------------------------------------------------------------------------
static void report_section(const char* name, const Stats& s, double live_lot) {
    const double raw_pts          = (live_lot > 0.0)
        ? (s.gross_pnl_pts / live_lot) : s.gross_pnl_pts;
    const double avg_pt           = (s.trades > 0) ? (raw_pts / s.trades) : 0.0;
    const double wr               = (s.trades > 0) ? (100.0 * s.wins / s.trades) : 0.0;
    const double gross_usd_001    = raw_pts * 100.0 * 0.01;
    const double gross_usd_010    = raw_pts * 100.0 * 0.10;
    const double gross_usd_030    = raw_pts * 100.0 * 0.30;

    std::printf("\n=================================================================\n");
    std::printf("%s\n", name);
    std::printf("-----------------------------------------------------------------\n");
    std::printf("  trades            : %d\n", s.trades);
    std::printf("  wins              : %d  (%.2f%% WR)\n", s.wins, wr);
    std::printf("  avg pt/trade      : %+.4f pt\n", avg_pt);
    std::printf("  avg hold          : %.1f s\n", s.avg_hold_s);
    std::printf("  exit reasons      : TP=%d  BE=%d  TRAIL=%d  SL=%d  REV=%d  MAX=%d\n",
                s.tp_hits, s.be_hits, s.trail_hits, s.sl_hits,
                s.rev_exits, s.max_hold_exits);
    std::printf("  raw pt total      : %+.2f pt\n", raw_pts);
    std::printf("  GROSS USD @ 0.01  : %+.2f\n", gross_usd_001);
    std::printf("  GROSS USD @ 0.10  : %+.2f\n", gross_usd_010);
    std::printf("  GROSS USD @ 0.30  : %+.2f\n", gross_usd_030);
}

int main(int argc, char** argv) {
    bool        no_session = false;
    const char* csv_path   = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--no-session"))  no_session = true;
        else if (!std::strcmp(argv[i], "--help") ||
                 !std::strcmp(argv[i], "-h")) {
            std::fprintf(stderr,
                "usage: %s [--no-session] <l2_ticks_XAUUSD_*.csv>\n", argv[0]);
            return 0;
        }
        else if (argv[i][0] == '-') {
            std::fprintf(stderr, "[err] unknown flag: %s\n", argv[i]);
            return 2;
        }
        else csv_path = argv[i];
    }
    if (!csv_path) {
        std::fprintf(stderr,
            "usage: %s [--no-session] <l2_ticks_XAUUSD_*.csv>\n", argv[0]);
        return 2;
    }

    std::ifstream f(csv_path);
    if (!f) {
        std::fprintf(stderr, "[err] cannot open: %s\n", csv_path);
        return 1;
    }

    std::vector<Tick> ticks;
    ticks.reserve(2'000'000);
    std::string line;
    std::getline(f, line);  // header
    int parsed = 0, skipped = 0;
    while (std::getline(f, line)) {
        Tick t;
        if (parse_tick(line, t)) { ticks.push_back(t); ++parsed; }
        else                     { ++skipped; }
    }
    if (ticks.empty()) {
        std::fprintf(stderr, "[err] no parseable ticks\n");
        return 1;
    }

    const int64_t first_ms = ticks.front().ts_ms;
    const int64_t last_ms  = ticks.back().ts_ms;
    const double  span_h   = (last_ms - first_ms) / 1000.0 / 3600.0;

    std::time_t fst = (std::time_t)(first_ms / 1000);
    std::time_t lst = (std::time_t)(last_ms  / 1000);
    char fst_buf[64], lst_buf[64];
    std::tm fut{}, lut{};
    gmtime_r(&fst, &fut);
    gmtime_r(&lst, &lut);
    std::strftime(fst_buf, sizeof(fst_buf), "%Y-%m-%d %H:%M:%S UTC", &fut);
    std::strftime(lst_buf, sizeof(lst_buf), "%Y-%m-%d %H:%M:%S UTC", &lut);

    std::fprintf(stderr,
        "[ok] loaded %d ticks (skipped %d)\n"
        "     span    = %.2f hours\n"
        "     first   = %s\n"
        "     last    = %s\n"
        "     session-mode: %s\n",
        parsed, skipped, span_h, fst_buf, lst_buf,
        no_session ? "DISABLED (--no-session)" : "06-22 UTC ENABLED");

    PortfolioConfig pc;
    pc.session_enabled = !no_session;

    ChopGeom  cg;
    TrendGeom tg;

    // ---- (A) CHOP-ONLY (regime gate disabled) ----
    {
        Regime regime(pc.er_window, pc.er_threshold);
        Stats s;
        simulate_chop(ticks, cg, pc, "all", regime, s);
        report_section("(A) CHOP-ONLY  (rk12, no regime gate -- = current production)",
                       s, cg.live_lot);
        std::printf("    Regime tape stats: trend ticks=%d  chop ticks=%d  "
                    "(of %d total, last ER=%.3f)\n",
                    regime.trend_ticks, regime.chop_ticks,
                    regime.trend_ticks + regime.chop_ticks, regime.current_er);
    }

    // ---- (B) TREND-ONLY (regime gate disabled) ----
    {
        Regime regime(pc.er_window, pc.er_threshold);
        Stats s;
        simulate_trend(ticks, tg, pc, "all", regime, s);
        report_section("(B) TREND-ONLY  (Donchian breakout, no regime gate)",
                       s, tg.live_lot);
    }

    // ---- (C) PORTFOLIO (chop + trend, both regime-gated) ----
    Stats chop_s, trend_s;
    {
        Regime regime(pc.er_window, pc.er_threshold);
        simulate_portfolio(ticks, cg, tg, pc, regime, chop_s, trend_s);
        report_section("(C) PORTFOLIO -- CHOP component  (gated to chop regime)",
                       chop_s, cg.live_lot);
        report_section("(C) PORTFOLIO -- TREND component (gated to trend regime)",
                       trend_s, tg.live_lot);
        std::printf("    Regime tape stats: trend ticks=%d  chop ticks=%d  "
                    "(threshold ER=%.3f, last ER=%.3f)\n",
                    regime.trend_ticks, regime.chop_ticks,
                    pc.er_threshold, regime.current_er);
    }

    // ---- Combined portfolio totals ----
    {
        const double chop_raw = (cg.live_lot > 0.0)
            ? (chop_s.gross_pnl_pts / cg.live_lot) : chop_s.gross_pnl_pts;
        const double trend_raw = (tg.live_lot > 0.0)
            ? (trend_s.gross_pnl_pts / tg.live_lot) : trend_s.gross_pnl_pts;
        const double total_raw = chop_raw + trend_raw;
        const int    total_trades = chop_s.trades + trend_s.trades;
        const int    total_wins   = chop_s.wins   + trend_s.wins;
        const double total_wr = (total_trades > 0)
            ? (100.0 * total_wins / total_trades) : 0.0;
        std::printf("\n=================================================================\n");
        std::printf("PORTFOLIO COMBINED TOTAL\n");
        std::printf("-----------------------------------------------------------------\n");
        std::printf("  trades         : %d  (chop=%d + trend=%d)\n",
                    total_trades, chop_s.trades, trend_s.trades);
        std::printf("  wins           : %d  (%.2f%% WR aggregate)\n",
                    total_wins, total_wr);
        std::printf("  raw pt total   : %+.2f  (chop=%+.2f + trend=%+.2f)\n",
                    total_raw, chop_raw, trend_raw);
        std::printf("  GROSS USD @ 0.01: %+.2f\n", total_raw * 100.0 * 0.01);
        std::printf("  GROSS USD @ 0.10: %+.2f\n", total_raw * 100.0 * 0.10);
        std::printf("  GROSS USD @ 0.30: %+.2f\n", total_raw * 100.0 * 0.30);
    }

    std::printf("\nDone.\n");
    return 0;
}

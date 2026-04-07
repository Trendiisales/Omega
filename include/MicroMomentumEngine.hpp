#pragma once
// =============================================================================
// MicroMomentumEngine.hpp  --  Fast 4-8pt momentum capture on XAUUSD
// =============================================================================
//
// DESIGN RATIONALE:
//   XAUUSD makes constant 4-8pt moves visible on any chart. These are missed
//   because existing engines require too many confirmations:
//     - GoldFlow: 70% of 30 ticks directional + momentum + exhaustion + L2
//     - RSIReversal: RSI must reach extreme (42/58) -- neutral moves missed
//     - Bracket: needs compression first
//
//   This engine catches a move WHILE IT IS HAPPENING using two signals:
//
//   1. RSI SLOPE -- the rate of RSI change, not the level
//      RSI rising at 0.5+ units/tick = bullish momentum building NOW
//      RSI falling at 0.5+ units/tick = bearish momentum building NOW
//      Works from RSI=50, not waiting for extreme
//
//   2. PRICE DISPLACEMENT -- price has moved N pts from recent anchor
//      Anchor = EWM of price over last 8 ticks (very fast, α=0.3)
//      If current price > anchor + ENTRY_DISP_PTS → confirmed move up
//      If current price < anchor - ENTRY_DISP_PTS → confirmed move down
//
//   BOTH must agree before entry. This prevents entries on:
//     - RSI noise without price moving (false slope)
//     - Price moving without RSI confirming (spread/manipulation spike)
//
//   EXIT:
//     - Fixed TP: ENTRY_DISP_PTS * TP_MULT (default 1.5x = 3pt on 2pt signal)
//     - RSI slope reversal: momentum dies, take what we have
//     - SL: 0.5x ATR (tight -- if wrong, out fast)
//     - Max hold: 5 minutes
//
//   COST COVERAGE:
//     At 0.10 lots, 3pt TP = $30 gross. Commission $0.60, spread ~$0.22 = $0.82.
//     Net ~$29. Even at 0.01 lots: $3 gross, $0.15 cost, $2.85 net.
//     Min profitable move: 0.82pt at 0.10 lots. ENTRY_DISP_PTS=2.0 comfortably covers.
//
// PARAMETERS (tuned for Asia 50 ticks/min, London 200 ticks/min):
//   ENTRY_DISP_PTS   = 1.5   -- price must move 1.5pt from anchor
//   RSI_SLOPE_MIN    = 0.4   -- RSI must change 0.4 units/tick (EWM smoothed)
//   RSI_SLOPE_WINDOW = 5     -- slope over last 5 RSI values
//   TP_PTS           = 3.0   -- fixed take profit 3pt
//   SL_ATR_MULT      = 0.4   -- SL = 0.4x tick ATR
//   MAX_SPREAD       = 2.0   -- don't enter wide spread
//   COOLDOWN_S       = 45    -- 45s between trades
//   MAX_HOLD_S       = 300   -- 5 min hard exit
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <functional>
#include <string>
#include <deque>
#include "OmegaTradeLedger.hpp"

namespace omega {

class MicroMomentumEngine {
public:
    // ── Parameters ────────────────────────────────────────────────────────────
    double ENTRY_DISP_PTS    = 1.5;  // price displacement from anchor to trigger
    double RSI_SLOPE_MIN     = 0.4;  // min RSI change/tick (EWM slope)
    int    RSI_SLOPE_WINDOW  = 5;    // ticks for RSI slope measurement
    double TP_PTS            = 3.0;  // fixed take profit in price points
    double SL_ATR_MULT       = 0.4;  // SL = 0.4x tick ATR
    double MAX_SPREAD_PTS    = 2.0;
    int    COOLDOWN_S        = 45;
    int    MAX_HOLD_S        = 300;
    int    MIN_HOLD_S        = 5;
    int    WARMUP_TICKS      = 20;   // ticks needed before signals valid
    bool   enabled           = true;
    bool   shadow_mode       = true;

    // ── State ─────────────────────────────────────────────────────────────────
    struct Position {
        bool    active    = false;
        bool    is_long   = false;
        double  entry     = 0.0;
        double  tp        = 0.0;
        double  sl        = 0.0;
        double  size      = 0.01;
        double  mfe       = 0.0;
        int64_t entry_ts  = 0;
    } pos;

    bool has_open_position() const noexcept { return pos.active; }

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // ── Main tick ─────────────────────────────────────────────────────────────
    void on_tick(double bid, double ask,
                 int session_slot, int64_t now_ms,
                 CloseCallback on_close) noexcept
    {
        if (!enabled) return;
        if (bid <= 0.0 || ask <= 0.0) return;

        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const int64_t now_s = now_ms / 1000;

        // Update indicators every tick
        _update(mid, spread);

        // Always manage open position
        if (pos.active) {
            _manage(bid, ask, mid, now_s, on_close);
            return;
        }

        // ── Gates ─────────────────────────────────────────────────────────────
        if (now_s < m_cooldown_until)          return;
        if (m_tick_count < WARMUP_TICKS)       return;
        if (spread > MAX_SPREAD_PTS)           return;
        if (session_slot == 0)                 return;  // dead zone
        if (_in_dead_zone())                   return;  // 05-07 UTC

        // ── Signal: RSI slope + price displacement must agree ─────────────────
        const double rsi_slope = m_rsi_slope;       // EWM of RSI change/tick
        const double disp      = mid - m_anchor;    // displacement from fast anchor

        const bool long_signal  = (rsi_slope >  RSI_SLOPE_MIN)
                                && (disp      >  ENTRY_DISP_PTS);
        const bool short_signal = (rsi_slope < -RSI_SLOPE_MIN)
                                && (disp      < -ENTRY_DISP_PTS);

        if (!long_signal && !short_signal) return;

        // ── Entry ─────────────────────────────────────────────────────────────
        const bool is_long  = long_signal;
        const double entry  = is_long ? ask : bid;
        const double sl_pts = std::max(m_tick_atr * SL_ATR_MULT, spread * 1.5);

        pos.active   = true;
        pos.is_long  = is_long;
        pos.entry    = entry;
        pos.tp       = is_long ? (entry + TP_PTS) : (entry - TP_PTS);
        pos.sl       = is_long ? (entry - sl_pts)  : (entry + sl_pts);
        pos.size     = 0.01;
        pos.mfe      = 0.0;
        pos.entry_ts = now_s;

        const char* pfx = shadow_mode ? "[MICROMOM-SHADOW]" : "[MICROMOM]";
        printf("%s %s entry=%.2f tp=%.2f sl=%.2f "
               "rsi_slope=%.3f disp=%.2f atr=%.2f spread=%.2f slot=%d\n",
               pfx, is_long ? "LONG" : "SHORT",
               entry, pos.tp, pos.sl,
               rsi_slope, disp, m_tick_atr, spread, session_slot);
        fflush(stdout);
    }

    void patch_size(double lot) noexcept { if (pos.active) pos.size = lot; }

    double rsi_slope()  const noexcept { return m_rsi_slope; }
    double anchor_disp(double mid) const noexcept { return mid - m_anchor; }

private:
    // ── Tick indicators ───────────────────────────────────────────────────────
    double  m_tick_rsi      = 50.0;
    double  m_rsi_prev      = 50.0;
    double  m_rsi_avg_gain  = 0.0;
    double  m_rsi_avg_loss  = 0.0;
    bool    m_rsi_seeded    = false;
    std::deque<double> m_rsi_seed_g;
    std::deque<double> m_rsi_seed_l;
    double  m_rsi_last_mid  = 0.0;
    static constexpr int RSI_PERIOD = 14;

    // RSI slope: EWM of per-tick RSI change (α=0.4 = reacts in ~2 ticks)
    double  m_rsi_slope     = 0.0;
    static constexpr double SLOPE_ALPHA = 0.4;

    // Price anchor: fast EWM of mid (α=0.25 = ~3-tick memory)
    double  m_anchor        = 0.0;
    bool    m_anchor_init   = false;
    static constexpr double ANCHOR_ALPHA = 0.25;

    // Tick ATR: EWM of |price change|
    double  m_tick_atr      = 0.0;
    double  m_atr_last_mid  = 0.0;
    bool    m_atr_init      = false;
    static constexpr double ATR_ALPHA = 2.0 / 15.0;

    int     m_tick_count    = 0;
    int64_t m_cooldown_until = 0;
    int     m_trade_id       = 0;

    void _update(double mid, double spread) noexcept {
        ++m_tick_count;

        // Price anchor (fast EWM)
        if (!m_anchor_init) { m_anchor = mid; m_anchor_init = true; }
        else m_anchor = ANCHOR_ALPHA * mid + (1.0 - ANCHOR_ALPHA) * m_anchor;

        // Tick ATR
        if (!m_atr_init) { m_tick_atr = std::max(spread, 0.1); m_atr_init = true; }
        else {
            const double chg = (m_atr_last_mid > 0.0)
                ? std::max(std::fabs(mid - m_atr_last_mid), spread)
                : spread;
            m_tick_atr = ATR_ALPHA * chg + (1.0 - ATR_ALPHA) * m_tick_atr;
        }
        m_atr_last_mid = mid;

        // Tick RSI (Wilder, period 14)
        if (m_rsi_last_mid <= 0.0) { m_rsi_last_mid = mid; return; }
        const double chg  = mid - m_rsi_last_mid;
        m_rsi_last_mid = mid;
        const double gain = chg > 0.0 ? chg : 0.0;
        const double loss = chg < 0.0 ? -chg : 0.0;

        if (!m_rsi_seeded) {
            m_rsi_seed_g.push_back(gain);
            m_rsi_seed_l.push_back(loss);
            if ((int)m_rsi_seed_g.size() >= RSI_PERIOD) {
                double sg = 0.0, sl = 0.0;
                for (int i = 0; i < RSI_PERIOD; ++i) { sg += m_rsi_seed_g[i]; sl += m_rsi_seed_l[i]; }
                m_rsi_avg_gain = sg / RSI_PERIOD;
                m_rsi_avg_loss = sl / RSI_PERIOD;
                m_rsi_seeded = true;
                m_rsi_seed_g.clear();
                m_rsi_seed_l.clear();
            }
        } else {
            m_rsi_avg_gain = (m_rsi_avg_gain * (RSI_PERIOD - 1) + gain) / RSI_PERIOD;
            m_rsi_avg_loss = (m_rsi_avg_loss * (RSI_PERIOD - 1) + loss) / RSI_PERIOD;
        }

        if (!m_rsi_seeded) return;

        m_rsi_prev = m_tick_rsi;
        m_tick_rsi = (m_rsi_avg_loss < 1e-10) ? 100.0
                   : 100.0 - 100.0 / (1.0 + m_rsi_avg_gain / m_rsi_avg_loss);

        // RSI slope: EWM of per-tick change
        const double raw_slope = m_tick_rsi - m_rsi_prev;
        m_rsi_slope = SLOPE_ALPHA * raw_slope + (1.0 - SLOPE_ALPHA) * m_rsi_slope;
    }

    static bool _in_dead_zone() noexcept {
        const auto t = std::time(nullptr);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        return (ti.tm_hour >= 5 && ti.tm_hour < 7);
    }

    void _manage(double bid, double ask, double mid,
                 int64_t now_s, CloseCallback on_close) noexcept
    {
        if (!pos.active) return;
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;

        if ((now_s - pos.entry_ts) < MIN_HOLD_S) return;

        // Max hold
        if ((now_s - pos.entry_ts) >= MAX_HOLD_S) {
            _close(pos.is_long ? bid : ask, "MAX_HOLD", now_s, on_close);
            return;
        }

        // TP hit
        const bool tp_hit = pos.is_long ? (ask >= pos.tp) : (bid <= pos.tp);
        if (tp_hit) {
            _close(pos.is_long ? pos.tp : pos.tp, "TP_HIT", now_s, on_close);
            return;
        }

        // RSI slope reversal -- momentum died, exit with what we have
        // Only exit if in profit (move > 0.5pt) to avoid exiting on noise
        if (move > 0.5) {
            const bool slope_died = pos.is_long  ? (m_rsi_slope < 0.0)
                                                 : (m_rsi_slope > 0.0);
            if (slope_died) {
                _close(pos.is_long ? bid : ask, "SLOPE_EXIT", now_s, on_close);
                return;
            }
        }

        // SL
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (sl_hit) {
            _close(pos.is_long ? bid : ask, "SL_HIT", now_s, on_close);
        }
    }

    void _close(double exit_px, const char* reason,
                int64_t now_s, CloseCallback on_close) noexcept
    {
        const double pnl_pts = pos.is_long
            ? (exit_px - pos.entry) : (pos.entry - exit_px);
        const double pnl = pnl_pts * pos.size * 100.0;

        printf("[MICROMOM] EXIT %s @ %.2f reason=%s pnl_pts=%.2f pnl=$%.2f mfe=%.2f\n",
               pos.is_long ? "LONG" : "SHORT",
               exit_px, reason, pnl_pts, pnl, pos.mfe);
        fflush(stdout);

        omega::TradeRecord tr;
        tr.id          = ++m_trade_id;
        tr.symbol      = "XAUUSD";
        tr.side        = pos.is_long ? "LONG" : "SHORT";
        tr.engine      = "MicroMomentum";
        tr.regime      = "MOMENTUM";
        tr.entryPrice  = pos.entry;
        tr.exitPrice   = exit_px;
        tr.sl          = pos.sl;
        tr.size        = pos.size;
        tr.pnl         = pnl_pts * pos.size;
        tr.mfe         = pos.mfe * pos.size;
        tr.mae         = 0.0;
        tr.entryTs     = pos.entry_ts;
        tr.exitTs      = now_s;
        tr.exitReason  = reason;
        tr.spreadAtEntry = 0.0;

        pos = Position{};
        m_cooldown_until = now_s + COOLDOWN_S;

        if (on_close) on_close(tr);
    }
};

} // namespace omega

#pragma once
// PullbackContEngine.hpp -- Pullback continuation after large directional move
//
// SIGNAL (from 2yr tick backtest, 111M ticks):
//   After a 20pt move in 5 minutes, wait for a 4pt (20%) retracement,
//   enter on continuation. Session gates h07, h17, h23 UTC.
//
// KEY BACKTEST RESULTS (cost=0.30pt, shadow mode until validated):
//   h23 LB=300s MOVE=20pt PB=20% HOLD=300s: N=794  WR=85.6% avg_net=+14.72
//   h23 LB=300s MOVE=20pt PB=20% HOLD=600s: N=794  WR=61.8% avg_net=+18.76
//   h17 LB=300s MOVE=20pt PB=20% HOLD=300s: N=849  WR=80.7% avg_net=+10.36
//   h07 LB=300s MOVE=20pt PB=20% HOLD=300s: N=803  WR=79.8% avg_net=+5.78

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <deque>
#include <string>
#include <functional>

namespace omega {

struct PullbackContEngine {

    // ---- Config (tunable) ----
    bool     enabled       = true;
    bool     shadow_mode   = true;    // SHADOW until validated in live logs
    double   MOVE_MIN      = 20.0;    // pts: min 5min move to qualify
    double   PB_FRAC       = 0.20;    // pullback fraction (20% = 4pts on 20pt move)
    int      LOOKBACK_S    = 300;     // lookback window seconds
    int      HOLD_S        = 600;     // max hold seconds
    double   SL_PTS        = 6.0;     // stop loss points
    double   TRAIL_PTS     = 4.0;     // trail arms at 4pts profit, trails 4pts behind
    double   BASE_RISK_USD = 80.0;    // risk per trade USD
    int64_t  COOLDOWN_MS   = 120000;  // 2min cooldown after any exit

    // ---- Callbacks (same pattern as MacroCrashEngine) ----
    std::function<void(double,bool,double,const std::string&)> on_close;
    std::function<void(const omega::TradeRecord&)>             on_trade_record;

    // ---- Position state ----
    struct Pos {
        bool     active      = false;
        bool     is_long     = false;
        double   entry       = 0.0;
        double   size        = 0.0;
        double   sl          = 0.0;
        double   trail_best  = 0.0;
        int64_t  entry_ms    = 0;
        int64_t  expire_ms   = 0;
        double   move_pts    = 0.0;
        double   mfe         = 0.0;
    } pos;

    bool has_open_position() const { return pos.active; }

    // ---- on_tick ----
    void on_tick(double bid, double ask, int64_t now_ms, bool gold_can_enter)
    {
        if (!enabled) return;

        const double mid = (bid + ask) * 0.5;

        // UTC hour gate: h07, h17, h23 only
        const int hour = (int)((now_ms / 1000LL) % 86400LL / 3600LL);
        const bool session_ok = (hour == 7 || hour == 17 || hour == 23);

        // Maintain lookback buffer
        m_buf.push_back({now_ms, mid});
        while (!m_buf.empty() &&
               now_ms - m_buf.front().ts_ms > (int64_t)LOOKBACK_S * 1000LL + 5000LL)
            m_buf.pop_front();

        // Manage open position first (no gate -- must manage regardless of session)
        if (pos.active) {
            _manage_position(bid, ask, mid, now_ms);
            return;
        }

        if (now_ms < m_cooldown_until) return;
        if (!session_ok) { m_state = IDLE; return; }
        if (m_buf.size() < 10) return;

        const double mid_back = m_buf.front().mid;
        const double move     = mid - mid_back;

        if (m_state == IDLE) {
            if (std::fabs(move) >= MOVE_MIN) {
                m_state      = WATCHING;
                m_long_dir   = (move > 0.0);
                m_signal_mid = mid;
                m_move_pts   = std::fabs(move);
                m_signal_ms  = now_ms;
                const double pb_pts = m_move_pts * PB_FRAC;
                m_wanted_mid = m_long_dir ? (m_signal_mid - pb_pts)
                                          : (m_signal_mid + pb_pts);
                printf("[PCE] WATCH %s move=%.1fpt @ %.2f -> pb_target=%.2f h%02d\n",
                       m_long_dir ? "LONG" : "SHORT", m_move_pts,
                       m_signal_mid, m_wanted_mid, hour);
                fflush(stdout);
            }
            return;
        }

        // WATCHING state
        // Timeout
        if (now_ms - m_signal_ms > (int64_t)HOLD_S * 1000LL) {
            m_state = IDLE;
            return;
        }
        // Counter-reversal: if price goes MORE than 50% against original move, abort
        const double vs_signal = m_long_dir ? (mid - m_signal_mid)
                                            : (m_signal_mid - mid);
        if (vs_signal < -(m_move_pts * 0.50)) {
            m_state = IDLE;
            return;
        }
        // Check pullback hit
        const bool pb_hit = m_long_dir ? (mid <= m_wanted_mid)
                                       : (mid >= m_wanted_mid);
        if (pb_hit && gold_can_enter) {
            _enter(bid, ask, now_ms, hour);
        }
    }

private:

    struct Snap { int64_t ts_ms; double mid; };
    std::deque<Snap> m_buf;

    enum State { IDLE, WATCHING } m_state = IDLE;
    bool    m_long_dir   = false;
    double  m_signal_mid = 0.0;
    double  m_move_pts   = 0.0;
    double  m_wanted_mid = 0.0;
    int64_t m_signal_ms  = 0;
    int64_t m_cooldown_until = 0;

    static uint64_t s_trade_id;

    void _enter(double bid, double ask, int64_t now_ms, int hour)
    {
        const double entry_px = m_long_dir ? ask : bid;
        const double sl_px    = m_long_dir ? (entry_px - SL_PTS) : (entry_px + SL_PTS);
        const double size     = std::max(0.01,
                                std::min(0.01, BASE_RISK_USD / (SL_PTS * 100.0)));  // FIX 2026-04-22 uniformity: capped to 0.01

        pos.active     = true;
        pos.is_long    = m_long_dir;
        pos.entry      = entry_px;
        pos.size       = size;
        pos.sl         = sl_px;
        pos.trail_best = entry_px;
        pos.entry_ms   = now_ms;
        pos.expire_ms  = now_ms + (int64_t)HOLD_S * 1000LL;
        pos.move_pts   = m_move_pts;
        pos.mfe        = 0.0;

        m_state = IDLE;

        printf("[PCE] ENTRY %s size=%.2f @ %.2f sl=%.2f move_was=%.1fpt h%02d %s\n",
               pos.is_long ? "LONG" : "SHORT",
               pos.size, pos.entry, pos.sl, pos.move_pts, hour,
               shadow_mode ? "[SHADOW]" : "[LIVE]");
        fflush(stdout);
    }

    void _manage_position(double bid, double ask, double mid, int64_t now_ms)
    {
        // Update MFE
        const double pnl_pts = pos.is_long ? (bid - pos.entry) : (pos.entry - ask);
        if (pnl_pts > pos.mfe) pos.mfe = pnl_pts;

        // Update trail
        if (pos.is_long  && bid > pos.trail_best) pos.trail_best = bid;
        if (!pos.is_long && ask < pos.trail_best) pos.trail_best = ask;

        const bool  trail_armed = (pnl_pts >= TRAIL_PTS);
        const double trail_sl   = pos.is_long
            ? pos.trail_best - TRAIL_PTS
            : pos.trail_best + TRAIL_PTS;

        double eff_sl = pos.sl;
        if (trail_armed) {
            eff_sl = pos.is_long
                ? std::max(pos.sl, trail_sl)
                : std::min(pos.sl, trail_sl);
        }

        const double chk = pos.is_long ? bid : ask;
        bool exit = false;
        std::string reason;

        if ( pos.is_long && chk <= eff_sl) { exit=true; reason = trail_armed ? "TRAIL_SL" : "SL_HIT"; }
        if (!pos.is_long && chk >= eff_sl) { exit=true; reason = trail_armed ? "TRAIL_SL" : "SL_HIT"; }
        if (now_ms >= pos.expire_ms)       { exit=true; reason = "TIME_EXIT"; }

        if (!exit) return;

        const double exit_px  = pos.is_long ? bid : ask;
        const double ppts     = pos.is_long ? (exit_px - pos.entry)
                                            : (pos.entry - exit_px);
        const double gross    = ppts * pos.size * 100.0;

        printf("[PCE] EXIT %s @ %.2f pnl_pts=%.2f gross=%.2f reason=%s %s\n",
               pos.is_long ? "LONG" : "SHORT",
               exit_px, ppts, gross, reason.c_str(),
               shadow_mode ? "[SHADOW]" : "[LIVE]");
        fflush(stdout);

        if (!shadow_mode && on_close)
            on_close(exit_px, pos.is_long, pos.size, reason);

        if (on_trade_record) {
            omega::TradeRecord tr;
            tr.symbol        = "XAUUSD";
            tr.side          = pos.is_long ? "LONG" : "SHORT";
            tr.engine        = "PullbackCont";
            tr.regime        = "PULLBACK";
            tr.entryPrice    = pos.entry;
            tr.exitPrice     = exit_px;
            tr.sl            = pos.sl;
            tr.size          = pos.size;
            tr.pnl           = ppts * pos.size;   // pts*lots; handle_closed_trade applies tick_mult
            tr.net_pnl       = tr.pnl;
            tr.mfe           = pos.mfe * pos.size;
            tr.mae           = 0.0;
            tr.entryTs       = pos.entry_ms / 1000LL;
            tr.exitTs        = now_ms / 1000LL;
            tr.exitReason    = reason;
            tr.spreadAtEntry = 0.0;
            tr.id            = static_cast<int>(++s_trade_id);
            tr.shadow        = shadow_mode;
            on_trade_record(tr);
        }

        m_cooldown_until = now_ms + COOLDOWN_MS;
        pos = Pos{};
    }
};

inline uint64_t PullbackContEngine::s_trade_id = 0;

} // namespace omega

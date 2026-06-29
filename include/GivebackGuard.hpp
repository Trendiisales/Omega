#pragma once
// GivebackGuard.hpp — INDEPENDENT profit-giveback clipper over ALL engines (S-2026-06-29).
//
// WHY (operator, 2026-06-29): a trade ran +$222 and gave back $142 with NOTHING catching it.
// The trade engines ride wide BY DESIGN (banking inside the engine kills the trend edge — proven).
// The protection is supposed to be an INDEPENDENT engine that clips the giveback by CLOSING the
// position, without touching the trade engine. That independent clipper was built only as a Mac
// SHADOW accountant (giveback_saver.py / stall_accountant.py) with ZERO close code -> it recorded
// "locked" and the real trade rode on. This is the REAL twin of AccountingGuard: same registry,
// same close path, but it triggers on PROFIT GIVEBACK / STALL instead of runaway loss.
//
// HOW: iterates g_open_positions (every engine, every symbol). Tracks each position's PEAK
// favorable excursion (uses PositionSnapshot.mfe; falls back to tracking max unrealized itself so
// it works even if an engine doesn't populate mfe). When an ARMED position (peak >= gate_usd) gives
// back >= trail of its peak (REVERSAL) or makes no new peak for stall_sec (STALL), it closes the
// position via g_open_positions.close_matching() — the SAME proven path AccountingGuard uses, which
// works in shadow (closes the sim position) and live (registered closer flattens). Independent of
// the trade engine -> keeps the edge, clips the giveback. Edge-safe: only fires after a real peak.
//
// Wire: once per 250ms in on_tick, call g_giveback_guard.check(now_s) alongside the catastrophe net.
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include "OpenPositionRegistry.hpp"
#include "RegimeState.hpp"

namespace omega {

class GivebackGuard {
public:
    bool    enabled     = true;
    double  gate_usd    = 40.0;     // ARM only once peak favorable >= +$ this (no clipping tiny trades)
    double  trail       = 0.40;     // REVERSAL: close when given back >= this fraction of peak
    double  stall_sec   = 86400.0;  // STALL: armed position makes no new peak for this long (24h) -> close
    int64_t log_every_s = 30;

    // Returns # of positions clipped this pass. now_s = epoch seconds.
    int check(int64_t now_s) {
        if (!enabled) return 0;
        int hit = 0;
        for (const auto& p : g_open_positions.snapshot_all()) {
            if (p.size <= 0.0) continue;
            const std::string key = p.symbol + "|" + p.engine;
            const double unr = p.unrealized_pnl;            // current unrealized USD
            // peak = best of (registry mfe, our own running max of unrealized) -> robust if mfe unpopulated
            double& pk  = peak_[key];
            int64_t& pts = peak_ts_[key];
            const double cand = std::max(p.mfe, unr);
            if (cand > pk) { pk = cand; pts = now_s; }       // new peak -> reset stall clock
            if (pk < gate_usd) continue;                     // not armed: never reached the profit gate

            const bool reversal = unr <= pk * (1.0 - trail);
            const bool stall    = (now_s - (pts ? pts : now_s)) >= (int64_t)stall_sec;
            if (!(reversal || stall)) continue;

            int64_t& last = last_log_[key];
            if ((now_s - last) < log_every_s) continue;      // throttle repeat attempts on the same key
            last = now_s;
            const char* why = reversal ? "REVERSAL" : "STALL";
            const bool closed = g_open_positions.close_matching(
                                    p, reversal ? "GIVEBACK_REVERSAL" : "GIVEBACK_STALL");
            printf("[GIVEBACK] %s %s/%s %s peak=$%.0f now=$%.0f gaveback=$%.0f -> %s\n",
                   why, p.symbol.c_str(), p.engine.c_str(), p.side.c_str(),
                   pk, unr, pk - unr, closed ? "CLIPPED (closed)" : "NO-CLOSER (engine has no registered closer)");
            fflush(stdout);
            if (closed) { ++hit; peak_.erase(key); peak_ts_.erase(key); last_log_.erase(key); }
        }
        return hit;
    }

    // ---- CONFIRMED-REVERSAL safe-stop (the ONE sanctioned real-engine close) ----------------------
    // EDGE-SAFE BY WHEN IT FIRES, not by independence: it closes a position ONLY when the trend it
    // rode is CONFIRMED DEAD -> the edge is already gone, so closing forfeits nothing. 2-of-2:
    //   (1) trade WAS armed (peak >= gate_usd) and has REVERSED THROUGH to a loss (unr <= reversal_loss_usd)
    //   (2) RegimeState confirms SUSTAINED against the position (long_blocked for longs / short_blocked
    //       for shorts) -- a slow high-conviction brain that a pullback does NOT trip.
    // A normal pullback trips NEITHER. DEFAULT OFF + per-engine opt-in: arm an engine ONLY after a
    // backtest shows net >= ride-wide for it. Backtest (tools reversal_stop_bt) showed the turtle
    // engines already exit-on-turn -> reversal-stop is redundant/slightly-negative -> armed NOWHERE
    // by default. reversal_armed_engines stays empty until a ride-forever engine backtests positive.
    bool   reversal_enabled    = false;            // master switch
    double reversal_loss_usd   = -10.0;            // "reversed through to a loss" threshold
    std::unordered_set<std::string> reversal_armed_engines;   // per-engine opt-in (backtest-approved only)

    int reversal_stop_check(int64_t now_s) {
        if (!reversal_enabled || reversal_armed_engines.empty()) return 0;
        int hit = 0;
        for (const auto& p : g_open_positions.snapshot_all()) {
            if (p.size <= 0.0) continue;
            if (reversal_armed_engines.find(p.engine) == reversal_armed_engines.end()) continue;  // only armed engines
            const double pk = std::max(p.mfe, p.unrealized_pnl);
            const bool was_armed = pk >= gate_usd;                    // trade WAS winning
            const bool reversed  = p.unrealized_pnl <= reversal_loss_usd;  // now reversed through to a loss
            if (!(was_armed && reversed)) continue;
            const bool is_long = (p.side == "LONG");
            omega::RegimeState& rs =
                (p.symbol.find("XAU") != std::string::npos || p.symbol.find("GOLD") != std::string::npos)
                ? omega::gold_regime() : omega::index_market_regime();
            if (!rs.warm()) continue;                                 // regime brain not ready -> no action
            const bool regime_against = is_long ? rs.long_blocked() : rs.short_blocked();
            if (!regime_against) continue;                            // 2-of-2 not met -> ride on
            const std::string key = p.symbol + "|" + p.engine;
            int64_t& last = last_log_[key];
            if ((now_s - last) < log_every_s) continue;
            last = now_s;
            const bool closed = g_open_positions.close_matching(p, "REVERSAL_STOP");
            printf("[REVERSAL-STOP] %s/%s %s CONFIRMED-REVERSAL (peak=$%.0f unr=$%.0f + regime-against) -> %s\n",
                   p.symbol.c_str(), p.engine.c_str(), p.side.c_str(), pk, p.unrealized_pnl,
                   closed ? "CLOSED" : "NO-CLOSER");
            fflush(stdout);
            if (closed) { ++hit; peak_.erase(key); peak_ts_.erase(key); }
        }
        return hit;
    }

private:
    std::unordered_map<std::string,double>  peak_;
    std::unordered_map<std::string,int64_t> peak_ts_, last_log_;
};

} // namespace omega

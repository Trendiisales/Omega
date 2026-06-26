#pragma once
//  ADVERSE-PROTECTION: 2.5% SAFETY_SL_PCT black-swan tail cut + deterministic EOD/rollover time-stop (one round-trip/day) -- no LOSS_CUT/BE/trail; the time-stop IS the in-flight protection for this daily-hold drift capture; no faithful backtest on record -- verdict owed before re-enable (backfill S-2026-06-24n)
// IndexIntradayDriftEngine.hpp -- BUY at session open / SELL at session close.
//
// Built 2026-05-28 (S37-Z) after a basket viability audit on 5 indices over
// the available H1 corpus (SPXUSD 586d, NSXUSD 521d, GER40 258d, USA30 148d,
// UK100 256d). The classic Beumer/Cliff 2018 "overnight drift" finding has
// REVERSED in the 2024-2026 regime: indices now earn nearly all their
// realised return INTRADAY and bleed overnight. See
// `backtest/index_intraday_drift_v2.py` for the measurement.
//
// VIABILITY RESULTS  (intraday NET-of-cost, walk-forward split)
// -------------------------------------------------------------
//                    Sharpe  WF1     WF2      verdict
//   USA30   n=148   +1.31   +1.04   +1.53    VIABLE  (best Sharpe)
//   UK100   n=256   +1.12   +1.01   +1.46    VIABLE  (strongest walk-fwd)
//   SPXUSD  n=586   +0.77   +1.28   +0.48    VIABLE  (both halves positive)
//   NSXUSD  n=521   +0.86   -0.14   +1.69    MARGINAL (skip until WF1 fixed)
//   GER40   n=257   +1.13   +1.92   -0.20    MARGINAL (skip until WF2 fixed)
//
// STRATEGY
// --------
// At the FIRST tick of each UTC trading day (no open pos, weekday only):
//    - Enter LONG @ ask, size = ENTRY_SIZE.
//    - Safety SL = entry * (1 - SAFETY_SL_PCT / 100). Black-swan protection.
//      Normal intraday std is ~1%, so 2.5% SL fires only on tail events.
//
// At the LAST tick of the same UTC day (a tick whose UTC-hour >=
// EXIT_HOUR_UTC), exit at bid. Final-tick exit guarantees one round-trip
// per day regardless of session calendar variation.
//
// NO microstructure. NO L2. NO indicator state. Pure time-of-day driven --
// hence no warm-seed buffer to fill. [SEED] line emitted at boot for
// CLAUDE.md hash-check compliance even though there is no state to seed.
//
// COST GATE
// ---------
// Skipped at entry: the strategy is a daily-cadence drift capture and the
// realistic round-trip cost is already baked into the viability gate
// upstream (1.5-3 pts per round-trip on the audited symbols). Per-trade
// OmegaCostGuard::is_viable() does not fit a no-TP daily-hold strategy --
// it expects TP_dist + spread to compute coverage. Document this exemption
// explicitly so the standing ungated-engine audit treats this engine the
// same as LatencyEdgeEngines / RSIExtremeTurnEngine (other accepted
// exemptions).
//
// LOG NAMESPACE: [IDD-<SYM>] for entry/exit lifecycle. tr.engine =
// "IndexIntradayDrift". tr.regime = "INTRADAY_DRIFT_LONG".
// =============================================================================

#include <cstdint>
#include <ctime>
#include <cstdio>
#include <string>
#include <iostream>
#include <functional>

#include "OmegaTradeLedger.hpp"
#include "IndexRiskGate.hpp"      // S44 portfolio VIX risk-off gate (entry-only)

namespace omega {

class IndexIntradayDriftEngine {
public:
    // -- public config (per-instance override in engine_init.hpp) ------------
    std::string symbol;             // e.g. "US500.F", "DJ30.F", "UK100"
    // S-2026-06-26s fleet-sweep KILL (workflow wn6lralw2, verify held=True):
    // bull PF1.04 / bear PF0.75 = no edge (bull-beta). Default OFF so no future
    // instance silently defaults to on. (Currently un-instantiated: tick path is
    // a no-op stub + no global g_idd_* + position-source helper void-cast.)
    bool        enabled         = false;
    bool        shadow_mode     = true;
    double      ENTRY_SIZE      = 0.01;
    int         ENTRY_HOUR_UTC  = 0;    // open the position in the first H1 of UTC day
    int         EXIT_HOUR_UTC   = 23;   // close the position at/after this UTC hour
    double      SAFETY_SL_PCT   = 2.5;  // 2.5% safety stop -- black-swan only

    // -- callback signature --------------------------------------------------
    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // -- public position view (read-only externally) -------------------------
    struct Position {
        bool    active   = false;
        bool    is_long  = true;
        double  entry    = 0.0;
        double  sl       = 0.0;
        double  size     = 0.0;
        double  mfe      = 0.0;
        double  mae      = 0.0;
        int64_t entry_ts = 0;
        int     entry_day_id = -1;     // UTC days-since-epoch
    } pos;

    bool has_open_position() const noexcept { return pos.active; }

    // -- boot announcement (called once from engine_init.hpp) ----------------
    // No buffer to warm but we still emit a [SEED] line for the hash-check
    // boot audit per CLAUDE.md (Engine Warm-Seed Mandate).
    void boot_announce() const noexcept {
        std::cout << "[SEED] " << symbol << " IndexIntradayDriftEngine"
                  << " enabled=" << (enabled ? 1 : 0)
                  << " shadow=" << (shadow_mode ? 1 : 0)
                  << " entry_hr=" << ENTRY_HOUR_UTC
                  << " exit_hr=" << EXIT_HOUR_UTC
                  << " safety_sl_pct=" << SAFETY_SL_PCT
                  << " (no warmup buffer required)\n";
        std::cout.flush();
    }

    // -- main per-tick handler ----------------------------------------------
    void on_tick(double bid, double ask, int64_t ts_ms,
                 CloseCallback on_close) noexcept
    {
        if (!enabled) return;
        if (bid <= 0.0 || ask <= 0.0) return;
        const int64_t ts_s    = ts_ms / 1000;
        const int     day_id  = static_cast<int>(ts_s / 86400);
        std::tm tm{};
        const std::time_t t_s = static_cast<std::time_t>(ts_s);
#if defined(_WIN32)
        gmtime_s(&tm, &t_s);
#else
        gmtime_r(&t_s, &tm);
#endif
        const int hour_utc = tm.tm_hour;
        const int wday     = tm.tm_wday;   // 0=Sun..6=Sat

        // ---------------------------------------------------------------
        // 1. If pos open, track MFE/MAE and check exits.
        // ---------------------------------------------------------------
        if (pos.active) {
            const double mid = (bid + ask) * 0.5;
            const double pl_pts = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
            if (pl_pts > pos.mfe) pos.mfe = pl_pts;
            if (pl_pts < pos.mae) pos.mae = pl_pts;

            // 1a. Safety SL -- black-swan tail cut.
            if (pos.is_long) {
                if (bid <= pos.sl) { close_pos(bid, ts_s, "SAFETY_SL", on_close); return; }
            } else {
                if (ask >= pos.sl) { close_pos(ask, ts_s, "SAFETY_SL", on_close); return; }
            }

            // 1b. EOD exit -- pos was opened earlier in same UTC day, now
            // hour_utc >= EXIT_HOUR_UTC. Exits at current bid (LONG) / ask
            // (SHORT, currently unused but symmetric).
            if (day_id == pos.entry_day_id && hour_utc >= EXIT_HOUR_UTC) {
                const double exit_px = pos.is_long ? bid : ask;
                close_pos(exit_px, ts_s, "EOD_CLOSE", on_close);
                return;
            }

            // 1c. Defensive day-rollover exit -- new UTC day with pos still
            // open from yesterday. Should not happen given 1b, but guards
            // against weekend gaps (Sun 22:00 reopen after Fri 21:00 close)
            // and missed EXIT_HOUR_UTC due to feed dropout.
            if (day_id != pos.entry_day_id) {
                const double exit_px = pos.is_long ? bid : ask;
                close_pos(exit_px, ts_s, "ROLLOVER_EXIT", on_close);
                return;
            }
        }

        // ---------------------------------------------------------------
        // 2. Entry check -- only if no open pos, weekday, and inside entry hour.
        //    Once per UTC day (entry_day_id guard).
        // ---------------------------------------------------------------
        if (!pos.active && wday >= 1 && wday <= 5
            && hour_utc == ENTRY_HOUR_UTC && day_id != m_last_entry_day_id
            && !omega::index_risk_off())   // S44 portfolio VIX risk-off: no new entry
        {
            const double entry_px = ask;
            pos.active       = true;
            pos.is_long      = true;
            pos.entry        = entry_px;
            pos.size         = ENTRY_SIZE;
            pos.sl           = entry_px * (1.0 - SAFETY_SL_PCT / 100.0);
            pos.mfe          = 0.0;
            pos.mae          = 0.0;
            pos.entry_ts     = ts_s;
            pos.entry_day_id = day_id;
            m_last_entry_day_id = day_id;

            char _msg[256];
            std::snprintf(_msg, sizeof(_msg),
                "[IDD-%s] ENTRY LONG @ %.4f sl=%.4f size=%.4f day=%d shadow=%d\n",
                symbol.c_str(), pos.entry, pos.sl, pos.size, day_id, shadow_mode ? 1 : 0);
            std::cout << _msg;
            std::cout.flush();
        }
    }

private:
    int  m_last_entry_day_id = -1;
    int  m_trade_id          = 0;

    void close_pos(double exit_px, int64_t now_s, const char* reason,
                   CloseCallback on_close) noexcept
    {
        const double pnl = (pos.is_long ? (exit_px - pos.entry)
                                        : (pos.entry - exit_px)) * pos.size;
        char _msg[320];
        std::snprintf(_msg, sizeof(_msg),
            "[IDD-%s] EXIT %s @ %.4f reason=%s pnl_raw=%.4f mfe=%.4f mae=%.4f hold_s=%lld\n",
            symbol.c_str(), pos.is_long ? "LONG" : "SHORT", exit_px, reason,
            pnl, pos.mfe, pos.mae, (long long)(now_s - pos.entry_ts));
        std::cout << _msg;
        std::cout.flush();

        omega::TradeRecord tr;
        tr.id            = ++m_trade_id;
        tr.symbol        = symbol;
        tr.side          = pos.is_long ? "LONG" : "SHORT";
        tr.engine        = "IndexIntradayDrift";
        tr.regime        = "INTRADAY_DRIFT_LONG";
        tr.entryPrice    = pos.entry;
        tr.exitPrice     = exit_px;
        tr.tp            = 0.0;     // no TP (EOD-driven)
        tr.sl            = pos.sl;
        tr.size          = pos.size;
        tr.pnl           = pnl;      // raw pts*lots -- handle_closed_trade applies tick_mult
        tr.net_pnl       = pnl;
        tr.mfe           = pos.mfe * pos.size;
        tr.mae           = pos.mae * pos.size;
        tr.entryTs       = pos.entry_ts;
        tr.exitTs        = now_s;
        tr.exitReason    = reason;
        tr.spreadAtEntry = 0.0;      // not measured here; ledger fills via apply_realistic_costs
        tr.shadow        = shadow_mode;

        pos = Position{};

        if (on_close) on_close(tr);
    }
};

} // namespace omega

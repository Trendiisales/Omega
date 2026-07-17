#pragma once
// =============================================================================
// XauTrendRiderEngine.hpp -- "bank-and-reload" companion that rides a host
// XauTrendFollow engine's open positions WITHOUT touching the host.
//
// MECHANISM (validated 2026-06-19, backtest/xau_tf_rider_overlay.py):
//   For each ACTIVE host cell position it opens a rider leg in the same
//   direction. When the leg's favourable move reaches N * host_atr_at_entry it
//   BANKS that leg (+N ATR) and immediately RE-OPENS a new leg at the current
//   price -- repeating while the host cell stays open. When the host cell
//   closes, the final open leg closes at the current price (it inherits the
//   host's own giveback, never worse). "Mirror" mode: NO independent leg stop
//   (the tight-stop variant was net-NEGATIVE on 4h in backtest -- do not add).
//
// EVIDENCE (engine-faithful overlay on the REAL host trades, XAU corpus):
//   D1  bull engine +$3838 -> +rider +$6489 (N2) ; bear +$859 -> +$1699. both-halves+.
//   4h  bull engine +$5766 -> +rider +$7896 (N2) ; bear +$447 -> +$701.  both-halves+.
//   ONLY wire D1 + 4h (2h marginal/N3-only, 1h rider HURTS -- do not wire).
//
// ADVERSE-PROTECTION: trail-only-equivalent by design -- the rider has no cold
//   loss-cut and needs none: each banked leg locks +N ATR; the single open leg
//   carries at most the host's own per-cell risk (which the host already gates).
//   Backtested: a tight per-leg stop LOWERS net (-$2842 on 4h). Verdict =
//   "bank-and-ride, no leg stop -- backtested, a cold cut lowers net".
//
// COST GATE (S-2026-07-17u, pre-live audit hole H2): each host episode's leg-arm
//   is gated by OmegaCostGuard::is_viable (expected gross = the bank target
//   N*host_atr; spread at arm time). A blocked episode sets `vetoed` for that
//   cell until the host closes (one check per episode -- no per-tick log spam;
//   reloads inherit the arm-time verdict, same atr basis). Previously this was
//   the ONLY live engine with zero cost-gate references (ungated-audit blind
//   until the S-17u ENTRY_RE widening exposed it).
//
// PnL convention: tr.pnl = points * lot (RAW). The ledger multiplies by
//   tick_value_multiplier(XAUUSD)=100 -> USD. Do NOT pre-multiply here
//   (avoids the double-100x gotcha, see omega-pnl-double-multiply-gotcha).
// =============================================================================
#include <array>
#include <string>
#include <cstdint>
#include <vector>
#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"
#include "OpenPositionRegistry.hpp"   // PositionSnapshot (S-17u persist wire)

namespace omega {

struct XauRiderLeg {
    bool   active   = false;
    bool   is_long  = false;
    bool   vetoed   = false;   // S-17u cost-gate veto: blocked at arm, sits out the episode
    double open_px  = 0.0;
    double atr      = 0.0;
    int    banked   = 0;
    int64_t open_ts = 0;
};

struct XauTrendRiderEngine {
    bool        enabled     = false;
    bool        shadow_mode = true;
    double      N           = 2.5;     // bank threshold in host ATR units
    double      lot         = 0.01;
    int         max_legs    = 6;       // per host-cell-open episode (bound exposure)
    std::string tag         = "XauTrendRider";

    static constexpr int MAXCELLS = 8;
    int                  ncells   = 0;
    std::array<XauRiderLeg, MAXCELLS> leg{};

    void init(int host_ncells) noexcept {
        ncells = host_ncells > MAXCELLS ? MAXCELLS : host_ncells;
        for (auto& l : leg) l = XauRiderLeg{};
    }

    template<class CB>
    void emit(CB&& cb, XauRiderLeg& L, double exit_px, const char* reason, int64_t now_ms) noexcept {
        const double pts = L.is_long ? (exit_px - L.open_px) : (L.open_px - exit_px);
        omega::TradeRecord tr;
        tr.symbol     = "XAUUSD";
        tr.engine     = tag;
        tr.side       = L.is_long ? "LONG" : "SHORT";
        tr.entryPrice = L.open_px;
        tr.exitPrice  = exit_px;
        tr.size       = lot;
        tr.pnl        = pts * lot;            // RAW points*lot; ledger applies x100
        tr.entryTs    = L.open_ts / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.shadow     = shadow_mode;
        tr.regime     = "TREND_RIDER";
        cb(tr);
    }

    // ── restart persistence (S-2026-07-17u, wired via wire_multicell) ────────
    // Before the S-17u register_source the rider was display-invisible AND its
    // legs died on every restart: on_host would re-arm a FRESH leg at the current
    // price with no ledger close for the pre-restart leg — the vanish-no-close
    // class the persistence audit exists for. One snapshot per active leg, tag
    // "<base>#<ci>". Field mapping (PositionSnapshot has no spare doubles):
    //   sl := atr (bank threshold basis), tp := banked (reload count),
    //   entry_ts = open_ts/1000 (epoch SECONDS per snapshot convention;
    //   restore re-anchors open_ts = entry_ts*1000).
    bool persist_save_all(const char* base, const char* sym,
                          std::vector<PositionSnapshot>& out) const {
        for (int ci = 0; ci < ncells; ++ci) {
            const XauRiderLeg& L = leg[ci];
            if (!L.active) continue;
            PositionSnapshot ps;
            ps.engine   = std::string(base) + "#" + std::to_string(ci);
            ps.symbol   = sym;
            ps.side     = L.is_long ? "LONG" : "SHORT";
            ps.size     = lot;
            ps.entry    = L.open_px;
            ps.sl       = L.atr;                       // atr basis (see mapping note)
            ps.tp       = (double)L.banked;            // reload count
            ps.entry_ts = L.open_ts / 1000;            // ms -> epoch SECONDS
            out.push_back(ps);
        }
        return !out.empty();
    }
    bool persist_restore(const PositionSnapshot& ps) {
        const auto h = ps.engine.rfind('#');
        if (h == std::string::npos) return false;
        const int ci = std::atoi(ps.engine.c_str() + h + 1);
        if (ci < 0 || ci >= ncells) return false;
        XauRiderLeg& L = leg[ci];
        if (L.active) return true;                     // already holding -- don't double
        L.active  = true;
        L.is_long = (ps.side == "LONG");
        L.open_px = ps.entry;
        L.atr     = ps.sl;
        L.banked  = (int)ps.tp;
        L.open_ts = ps.entry_ts * 1000;                // re-anchor hold clock (SECONDS -> ms)
        L.vetoed  = false;
        return true;
    }

    // Poll the host's per-cell position array each tick (call AFTER host on_tick).
    // HostPos must expose: .active (bool), .is_long (bool), .atr_at_entry (double).
    template<class HostPosArr, class CB>
    void on_host(const HostPosArr& pos, double bid, double ask, int64_t now_ms, CB&& cb) noexcept {
        if (!enabled) return;
        for (int ci = 0; ci < ncells; ++ci) {
            const auto& hp = pos[ci];
            XauRiderLeg& L = leg[ci];

            if (hp.active && !L.active) {
                if (L.vetoed) continue;            // cost-gated out of this host episode
                // S-17u cost gate: one check per episode at arm time -- the leg's
                // expected gross is its bank target N*host_atr.
                if (hp.atr_at_entry <= 0.0 ||
                    !ExecutionCostGuard::is_viable("XAUUSD", ask - bid, N * hp.atr_at_entry, lot)) {
                    L.vetoed = true;
                    continue;
                }
                // host cell just opened -> arm leg 1 in the host's direction
                L.active  = true;
                L.is_long = hp.is_long;
                L.atr     = hp.atr_at_entry;
                L.open_px = hp.is_long ? ask : bid;
                L.open_ts = now_ms;
                L.banked  = 0;
            }
            else if (L.active && hp.active) {
                if (L.atr <= 0.0) continue;
                const double bank = N * L.atr;
                const double fav  = L.is_long ? (bid - L.open_px) : (L.open_px - ask);
                if (fav >= bank && L.banked < max_legs) {
                    const double bank_px = L.is_long ? bid : ask;
                    emit(cb, L, bank_px, "RIDER_BANK", now_ms);
                    ++L.banked;
                    // reload at current price (same direction) while host still open
                    L.open_px = L.is_long ? ask : bid;
                    L.open_ts = now_ms;
                }
            }
            else if (L.active && !hp.active) {
                // host cell closed -> close the final open leg at current price
                const double xp = L.is_long ? bid : ask;
                emit(cb, L, xp, "RIDER_HOST_EXIT", now_ms);
                L = XauRiderLeg{};
            }
            else if (!hp.active && L.vetoed) {
                L.vetoed = false;                  // episode over -> clear the veto
            }
        }
    }
};

} // namespace omega

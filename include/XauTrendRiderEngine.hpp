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
// PnL convention: tr.pnl = points * lot (RAW). The ledger multiplies by
//   tick_value_multiplier(XAUUSD)=100 -> USD. Do NOT pre-multiply here
//   (avoids the double-100x gotcha, see omega-pnl-double-multiply-gotcha).
// =============================================================================
#include <array>
#include <string>
#include <cstdint>
#include "OmegaTradeLedger.hpp"

namespace omega {

struct XauRiderLeg {
    bool   active   = false;
    bool   is_long  = false;
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

    // Poll the host's per-cell position array each tick (call AFTER host on_tick).
    // HostPos must expose: .active (bool), .is_long (bool), .atr_at_entry (double).
    template<class HostPosArr, class CB>
    void on_host(const HostPosArr& pos, double bid, double ask, int64_t now_ms, CB&& cb) noexcept {
        if (!enabled) return;
        for (int ci = 0; ci < ncells; ++ci) {
            const auto& hp = pos[ci];
            XauRiderLeg& L = leg[ci];

            if (hp.active && !L.active) {
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
        }
    }
};

} // namespace omega

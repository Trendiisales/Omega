#pragma once
//  ADVERSE-PROTECTION: 3xATR14 HARD STOP, intrabar ADVERSE-FIRST (stop checked
//  BEFORE the channel exit within the same bar; gap-through fills at
//  min(open,stop)) -- backtested IN-harness: the validated cell ran WITH exactly
//  this stop (gdd_mgc_volband_breakout.py Don40 all-band LONG sl3.0, certified
//  MGC 30m 2024-06..2026-06, 0.31pt RT): PF1.83 n204 +2006pt maxDD -188pt
//  worst-trade -80pt both-halves+ (+753/+1254) ex-best +1741 2x-cost PF1.79
//  over-random +1329pt. sl neighbors 2.5/3.5 -> PF1.93/1.76 = plateau, not a
//  spike. Entry-side bear protection: gold_regime().long_blocked() is MANDATORY
//  (2022-bear shadow on XAU-30m proxy at MGC cost: naked PF0.38; gated, the
//  2022 exposure is sat out -- same gate MgcFastDon carries). AUTO-RETIREMENT
//  latch: banked net (BT cost basis 0.31pt RT) <= retire_net_pts blocks new
//  entries (wired ~2x the cell's worst BT drawdown episode). Evidence:
//  outputs/GOLD_DEEP_DIVE_2026-07-08.md Study 7 (commit 4bca1036 harnesses).
//  S-2026-07-11 PHASE 1b RE-CELL: wired 55/27 (omega_main), the TRUE second
//  horizon -- 40/20 duplicated the live MgcFastDon channel. Faithful cell
//  (same chassis, certified MGC 30m 2024-26, 0.31 RT): n158 +1504.6pt PF1.78
//  both-halves+ maxDD -278.9 2x-cost PF1.74; real-engine parity EXACT; latch
//  -560 (=2x new worst DD). Same 3xATR stop (plateau sl2.5-3.5 PF>=1.65).
// =============================================================================
//  MgcSlowDonchian30mEngine.hpp  (S-2026-07-08c)
//  SLOW sibling of MgcFastDonchian30m on the same MGC (COMEX micro gold) 30m
//  feed: Donchian Nin40 close-confirmed breakout LONG -> NEXT-BAR-OPEN entry,
//  3xATR14 hard stop (adverse-first) OR Nout20 opposite-channel close exit.
//  2-3x slower cell than the live MgcFastDon Nin40/Nout20-with-HVN variant's
//  20/10 origin family, deeper rides, half the trade count.
//
//  DEDUP (per the deep-dive overlap disclosure): books correlate with
//  MgcFastDonchian30m -> a new entry is SKIPPED while the fast sibling holds a
//  position (peer_holds_pos callback, wired in omega_main.hpp). Positions
//  already open ride to their own exit.
//
//  Faithful-cell fidelity notes (harness == this engine):
//    * entry channel  = max HIGH of the prior Nin bars (excl. signal bar)
//    * exit  channel  = min LOW  of the prior Nout bars (excl. current bar)
//    * entry fill     = next bar OPEN; stop = entry - sl_atr_mult*ATR14(signal bar)
//    * stop fill      = min(bar open, stop)  [gap honesty]
//    * vol-band gate  = NONE (all-band >= mid-band in the sweep; do not bolt on)
//  Live-only additions the harness had no equivalent for: gold_regime()
//  long-block (bear insurance), ExecutionCostGuard, dedup, retirement latch.
// =============================================================================
#include "OmegaTradeLedger.hpp"     // omega::TradeRecord
#include "OmegaCostGuard.hpp"       // ::ExecutionCostGuard (GLOBAL scope)
#include "RegimeState.hpp"          // omega::gold_regime().long_blocked()
#include "OpenPositionRegistry.hpp" // omega::PositionSnapshot (persist)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <deque>
#include <fstream>
#include <functional>
#include <string>

namespace omega {

struct MgcSlowDonchian30mEngine {
    // ---- config ----
    bool   enabled        = false;
    bool   shadow_mode    = true;    // paper until the live shadow ledger proves it
    double lot            = 1.0;     // 1 MGC micro = 10oz = $10/pt (smallest unit)
    int    Nin            = 40;      // breakout channel (prior bars)
    int    Nout           = 20;      // exit channel (prior bars)
    double sl_atr_mult    = 3.0;     // hard stop distance in ATR14 (validated 3.0)
    double cost_rt_pts    = 0.31;    // MGC futures RT (retirement-latch accounting)
    double retire_net_pts = -400.0;  // auto-retirement latch (2x worst BT DD -188pt)
    double mgc_spread_pts = 0.10;    // 1 exchange tick (cost-guard input)

    // DEDUP vs MgcFastDonchian30m: skip NEW entries while the peer holds.
    std::function<bool()> peer_holds_pos;

    // Boot-replay / seed guard (same S102 pattern as the MGC TF instances):
    // while true, indicators + position management run, NEW entries are blocked.
    bool   warmup_active_ = false;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    // ---- state ----
    struct Bar { double h, l, c; };
    std::deque<Bar> bars_;                    // prior bars (exclude current)
    double  atr14_ = 0.0; double prev_close_ = 0.0; bool atr_init_ = false;
    int     atr_bars_ = 0;                    // bars folded into ATR (warm >= 15)
    int64_t last_bar_ts_ = 0;                 // ts dedup (seed/live overlap + boot replay)

    bool    pend_entry_ = false; double pend_stop_dist_ = 0.0;
    bool    pos_active_ = false; double entry_ = 0.0, stop_ = 0.0;
    int64_t entry_ts_ = 0; double mfe_ = 0.0, mae_ = 0.0;

    double  banked_net_pts_ = 0.0;            // per-1.0-lot points net of 0.31 RT
    bool    retired_ = false, retired_logged_ = false;

    bool has_open_position() const noexcept { return pos_active_; }

    // ---- restart persistence (wire_cross archetype) ----
    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const noexcept {
        if (!pos_active_) return false;
        o.engine = eng; o.symbol = sym; o.side = "LONG"; o.size = lot;
        o.entry = entry_; o.sl = stop_; o.tp = 0.0; o.entry_ts = entry_ts_;
        o.mfe = mfe_; o.mae = mae_;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) noexcept {
        if (pos_active_) return false;        // adopt won't double an open slot
        pos_active_ = true; entry_ = ps.entry; stop_ = ps.sl;
        entry_ts_ = ps.entry_ts; mfe_ = ps.mfe; mae_ = ps.mae;
        pend_entry_ = false;
        return true;
    }
    // generic-closer form used by PositionPersistence acct_try_close
    void force_close(double bid, double /*ask*/, int64_t now_sec, OnCloseFn cb) noexcept {
        if (!pos_active_) return;
        _close(bid > 0.0 ? bid : entry_, now_sec > 0 ? now_sec : entry_ts_, "FORCE_CLOSE", cb);
    }

    // Warm-seed from a ts(sec),o,h,l,c[,v] CSV (data/mgc_30m_hist.csv, regenerated
    // at deploy). Entries blocked (warmup_active_), ts-dedup makes the subsequent
    // live-CSV boot replay skip every overlapping row.
    int seed_from_30m_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) {
            std::printf("[SEED-FATAL] MgcSlowDonchian30m: cannot open '%s'\n", path.c_str());
            std::fflush(stdout); return 0;
        }
        const bool was = warmup_active_; warmup_active_ = true;
        int fed = 0; std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] < '0' || line[0] > '9') continue;
            char* p1; long long ts = std::strtoll(line.c_str(), &p1, 10); if (!p1 || *p1 != ',') continue;
            char* p2; double o = std::strtod(p1 + 1, &p2); if (!p2 || *p2 != ',') continue;
            char* p3; double h = std::strtod(p2 + 1, &p3); if (!p3 || *p3 != ',') continue;
            char* p4; double l = std::strtod(p3 + 1, &p4); if (!p4 || *p4 != ',') continue;
            double c = std::strtod(p4 + 1, nullptr);
            if (!std::isfinite(o) || !std::isfinite(h) || !std::isfinite(l) || !std::isfinite(c)) continue;
            if (ts > 4000000000LL) ts /= 1000;             // ms -> s safety
            on_30m_bar(o, h, l, c, (int64_t)ts, OnCloseFn{});
            ++fed;
        }
        warmup_active_ = was;
        std::printf("[SEED] MgcSlowDonchian30m %d bars replayed atr=%.3f chan=%zu last_ts=%lld -- hot\n",
                    fed, atr14_, bars_.size(), (long long)last_bar_ts_);
        std::fflush(stdout);
        return fed;
    }

    double _chan_high(int n) const noexcept {
        if ((int)bars_.size() < n) return 1e18;            // not warm -> no breakout
        double hh = -1e18;
        for (int k = (int)bars_.size() - n; k < (int)bars_.size(); ++k) hh = std::fmax(hh, bars_[k].h);
        return hh;
    }
    double _chan_low(int n) const noexcept {
        if ((int)bars_.size() < n) return -1e18;
        double ll = 1e18;
        for (int k = (int)bars_.size() - n; k < (int)bars_.size(); ++k) ll = std::fmin(ll, bars_[k].l);
        return ll;
    }

    void _close(double exit_px, int64_t ts_sec, const char* why, OnCloseFn cb) noexcept {
        omega::TradeRecord tr;
        tr.symbol     = "MGC";
        tr.engine     = "MgcSlowDonchian30m";
        tr.side       = "LONG";
        tr.entryPrice = entry_;
        tr.exitPrice  = exit_px;
        tr.size       = lot;
        tr.pnl        = (exit_px - entry_) * lot;   // points*lot; mult applied downstream (same basis as MgcFastDon)
        tr.entryTs    = entry_ts_;
        tr.exitTs     = ts_sec;
        tr.shadow     = shadow_mode;
        tr.mfe        = mfe_; tr.mae = mae_;
        tr.exitReason = why ? why : "";
        pos_active_ = false;
        // retirement-latch accounting at the BT cost basis (per-1.0-lot points)
        banked_net_pts_ += (exit_px - entry_) - cost_rt_pts;
        const bool was_retired = retired_;
        retired_ = (banked_net_pts_ <= retire_net_pts);
        if (retired_ && !was_retired && !retired_logged_) {
            retired_logged_ = true;
            std::printf("[MgcSlowDon][RETIRED] banked net %.1fpt <= %.1fpt -- new entries latched OFF "
                        "(auto-retirement, 2x worst BT DD; S-2026-07-08c)\n",
                        banked_net_pts_, retire_net_pts);
            std::fflush(stdout);
        }
        if (!retired_ && was_retired) retired_logged_ = false;   // recovered above the latch
        if (cb) cb(tr);
    }

    // Call on each CLOSED 30m MGC bar. Harness-faithful order:
    //   (0) pending entry from the prior signal bar fills at THIS bar's open
    //   (1) manage open position on THIS bar: 3xATR stop ADVERSE-FIRST, then
    //       Nout close-exit (channels exclude the current bar)
    //   (2) entry signal on THIS bar's close -> pend for next bar's open
    //   (3) push bar into the channel window
    void on_30m_bar(double o, double h, double l, double c, int64_t ts_sec, OnCloseFn cb) {
        if (ts_sec <= last_bar_ts_) return;                // seed/live overlap + boot dedup
        last_bar_ts_ = ts_sec;

        // ATR14 (Wilder, includes current bar -- harness atr_series form)
        if (atr_init_) {
            const double tr = std::fmax(h - l, std::fmax(std::fabs(h - prev_close_), std::fabs(l - prev_close_)));
            atr14_ += (tr - atr14_) / 14.0;
        } else { atr14_ = h - l; atr_init_ = true; }
        prev_close_ = c; ++atr_bars_;

        // (0) fill pending entry at this bar's open
        if (pend_entry_) {
            pend_entry_ = false;
            if (enabled && !pos_active_) {
                pos_active_ = true; entry_ = o; stop_ = o - pend_stop_dist_;
                entry_ts_ = ts_sec; mfe_ = 0.0; mae_ = 0.0;
                std::printf("[MgcSlowDon] ENTRY LONG px=%.2f stop=%.2f (3xATR=%.2f)%s\n",
                            entry_, stop_, pend_stop_dist_, shadow_mode ? " [SHADOW]" : "");
                std::fflush(stdout);
            }
        }

        // (1) manage -- ADVERSE-FIRST: hard stop before the channel exit
        if (pos_active_) {
            { double fav = h - entry_; if (fav > mfe_) mfe_ = fav;
              double adv = entry_ - l; if (adv > mae_) mae_ = adv; }
            if (l <= stop_)                                _close(std::fmin(o, stop_), ts_sec, "SL_3ATR", cb);
            else if (c < _chan_low(Nout))                  _close(c, ts_sec, "CHAN_EXIT", cb);
        }

        // (2) entry signal (close-confirmed break of the prior Nin-bar high)
        if (!pos_active_ && !pend_entry_ && enabled && !warmup_active_ &&
            atr_bars_ >= 15 && atr14_ > 0.0 && c > _chan_high(Nin)) {
            bool skip = false;
            // MANDATORY bear gate -- naked 2022-bear PF0.38; fail-open until regime warm.
            if (omega::gold_regime().long_blocked()) skip = true;
            // DEDUP vs MgcFastDonchian30m (books correlate; deep-dive overlap rule).
            if (!skip && peer_holds_pos && peer_holds_pos()) skip = true;
            // AUTO-RETIREMENT latch.
            if (!skip && retired_) skip = true;
            // ExecutionCostGuard (GLOBAL scope). S-2026-07-11 GOLD PHASE 1: the
            // explicit MGC row ($10/pt/contract, $2.08 RT comm, 1-tick slip)
            // replaces the old XAUUSD spot proxy, which mis-scaled BOTH cost and
            // gross by ~10x (ratio near-neutral, dollars wrong). Honest MGC
            // block threshold: 3xATR*10 >= 1.5*(spread*10+0.10*10+2.08).
            if (!skip && !::ExecutionCostGuard::is_viable("MGC", mgc_spread_pts,
                                                          sl_atr_mult * atr14_, lot, 1.5)) skip = true;
            if (!skip) { pend_entry_ = true; pend_stop_dist_ = sl_atr_mult * atr14_; }
        }

        // (3) channel window
        bars_.push_back({h, l, c});
        while ((int)bars_.size() > 128) bars_.pop_front();
    }
};

} // namespace omega

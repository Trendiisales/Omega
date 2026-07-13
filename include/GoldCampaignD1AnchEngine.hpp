#pragma once
//  ADVERSE-PROTECTION: STRUCTURAL ANCHOR STOP ON EVERY TRADE FROM ENTRY + 250bp-ARMED
//  24h-ROLLING TRAIL (backtested, the validated exit set -- gold_pullback_core_bt.cpp
//  frozen D1-ANCH cell, 5 eras 2013/2015/2022/2023-24/2024-26, ~5.7yr certified M1/tick):
//  stop BELOW THE IMPULSE ANCHOR (med 213-251bp => RT cost 3-8% of the risk unit, the
//  passing-crypto-parent geometry), trail = rolling-24h extreme -/+15bp armed at +250bp
//  MFE, MAX_HOLD 10d. POOLED n=60: PF2.14@8bp / 2.02@14bp, win 63%, worst trade -352bp,
//  trade-level maxDD 861bp@8bp, matched-random z=+3.22, halves 2.06/2.23, plateau 13/13.
//  AUTO-RETIREMENT latch: banked net <= -1725bp (2x the 861bp worst pooled DD) blocks
//  new entries. Evidence: backtest/GOLD_STRUCTURAL_CAMPAIGN_FINDINGS_2026-07-13.md Part 2
//  (commit dd2c1c79) + additivity/May-Jun-2026 OOS run S-2026-07-13z.
// =============================================================================
//  GoldCampaignD1AnchEngine.hpp  (S-2026-07-13z)
//  Gold Structural Campaign CORE, family 1 (first-pullback continuation) at the
//  D1-ANCHOR scale -- the ONE cell that passed Stage A after tick/H4 scales
//  CORE-FAILED on cost-vs-risk-unit geometry. Symmetric L/S, no regime gate
//  (direction adapts by construction: 2013/15/22 short-heavy, bulls long-heavy).
//
//  MECHANIC (frozen cell -- do NOT tune live; every knob is the validated value):
//    impulse : 200-500bp move from a rolling 72h extreme (anchor), 6-72h elapsed,
//              path-efficiency >= 0.35 (1h-stride), activity >= 1.3x same-30min-slot
//              trailing-20-day baseline, spread <= 1.5x slot median
//    pullback: 20-45% retrace of the impulse on lower down-leg activity
//              (<= 0.85x impulse rate), no trough beyond the anchor, trough not
//              > 30bp through event-VWAP, <= 24h from impulse peak
//    entry   : break of the 2h local extreme + 1h up-tick-imbalance >= 0.55 +
//              VWAP reclaim + measured-move remaining >= 150bp; fill at ask/bid
//    stop    : impulse ANCHOR -/+ 15bp (STOPMODE=1; med stop 213-251bp)
//    trail   : rolling-24h extreme -/+15bp, armed at +250bp MFE; MAX_HOLD 10d
//    hold    : ~5-9 days typical; ~9 trades/yr BY DESIGN (sparse structural book)
//
//  INPUT: gold ticks (tick_gold.hpp) -> internal UTC-M1 aggregation (mid OHLC +
//  mean spread per minute) == the certified M1 research files byte-for-byte
//  semantics. The detector runs on CLOSED M1 bars only; seed/parity feed bars
//  straight into on_m1_bar() so live == seed == harness by construction
//  (parity: backtest/gold_campaign_parity.cpp reproduces the frozen-cell dump).
//
//  ADDITIVITY (S-2026-07-13z, wire precondition): vs the live gold book
//  (XauTF1h/4h/D1+GVB+ThreeBar faithful dumps, 2024-26): daily MTM PnL corr
//  -0.066 (weekly -0.113, conditional-on-active-days -0.133); incumbents'
//  10 worst days: this book negative on only 1/10; vol-normalized 50/50
//  combine ret/DD 2.44 -> 6.11. Time-overlap is high (84% same-dir union --
//  incumbents are near-always-in-market) but the PnL stream is uncorrelated.
//
//  SAFETY
//    - shadow_mode=true, 0.01 lot (worst trade -352bp, maxDD 861bp -- size later)
//    - single position; one CORE per structural event; next anchor must postdate
//      the last exit (BT-equivalent overlap control)
//    - ExecutionCostGuard::is_viable() on every entry (GLOBAL scope)
//    - warm-seed: seed_m1_from_csv() (>= 5 trading days needed, 30+ bundled);
//      windows re-warm from live ticks in 6-72h after a stale seed
//    - persistence: persist_save/persist_restore (trail re-arms from mfe;
//      rolling-24h trail queue rebuilds from live bars, stop is monotonic)
// =============================================================================
#include "OmegaTradeLedger.hpp"     // omega::TradeRecord
#include "OmegaCostGuard.hpp"       // ::ExecutionCostGuard (GLOBAL scope)
#include "OpenPositionRegistry.hpp" // omega::PositionSnapshot (persist)
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <string>

namespace omega {

class GoldCampaignD1AnchEngine {
public:
    // ---- config ----
    bool        enabled     = false;
    bool        shadow_mode = true;
    double      lot         = 0.01;
    double      max_spread  = 5.00;   // gold $ catastrophic-quote sanity cap ONLY -- the
                                      // validated spread filter is the 1.5x-slot-median gate
                                      // inside the detector (an absolute cap here would add an
                                      // unvalidated gate and break harness parity on wide eras)
    std::string engine_tag  = "GoldCampaignD1Anch";
    double      retire_net_bp = -1725.0;  // 2x worst pooled trade-level DD (861bp @pad8)

    // ---- frozen D1-ANCH cell (validated 2026-07-13x; DO NOT tune live) ----
    static constexpr double IMP_LO = 200.0, IMP_HI = 500.0;
    static constexpr int    DUR_LO = 21600, DUR_HI = 259200;
    static constexpr double EFF = 0.35, ACT = 1.3, SPR_MAX = 1.5;
    static constexpr double RET_LO = 0.20, RET_HI = 0.45, PB_ACT = 0.85;
    static constexpr int    PB_TIMEOUT = 86400;
    static constexpr double UPR = 0.55, REM = 150.0, ARM = 250.0;
    static constexpr int    TRAILSEC = 86400, BREAKSEC = 7200;
    static constexpr double STOPBUF = 15.0, VWTOL = 30.0;
    static constexpr int    MAXHOLD = 864000;
    static constexpr int    UPQ_WIN = 3600, EFF_STRIDE = 3600;
    static constexpr int    WARMUP_DAYS = 5;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    struct Pos {
        bool    active = false;
        int     side   = 0;          // +1 long, -1 short
        double  entry_px = 0.0, stop_px = 0.0, hwm = 0.0;
        bool    armed = false;
        int64_t entry_ts = 0;        // sec
        double  mfe_bp = 0.0, mae_bp = 0.0;
    };
    Pos pos{};

    bool any_open() const noexcept { return pos.active; }

    void init() noexcept {
        bars_.clear(); trailq_.clear();
        for (auto& b : base_) b = SlotBase{};
        reset_day_acc(); cur_day_ = -1; days_seen_ = 0;
        ev_ = Ev{}; last_exit_ts_ = 0; last_anchor_used_ = 0;
        pos = {}; acc_min_ = -1;
        banked_net_bp_ = 0.0; retired_ = false;
    }

    // ---- ticks -> internal UTC-M1 bars (mid OHLC + mean spread), then detector ----
    void on_tick(double bid, double ask, int64_t now_ms, OnCloseFn cb) noexcept {
        if (!enabled || bid <= 0.0 || ask < bid) return;
        const double mid = 0.5 * (bid + ask), spr = ask - bid;
        const int64_t mn = now_ms / 60000LL;
        if (acc_min_ < 0) { acc_min_ = mn; acc_o_ = acc_h_ = acc_l_ = acc_c_ = mid; acc_spr_ = spr; acc_n_ = 1; return; }
        if (mn != acc_min_) {
            // minute closed: run the detector on it with REAL current bid/ask fills
            on_m1_bar(acc_min_ * 60, acc_o_, acc_h_, acc_l_, acc_c_, acc_spr_ / acc_n_, bid, ask, cb);
            acc_min_ = mn; acc_o_ = acc_h_ = acc_l_ = acc_c_ = mid; acc_spr_ = spr; acc_n_ = 1;
            return;
        }
        acc_c_ = mid; if (mid > acc_h_) acc_h_ = mid; if (mid < acc_l_) acc_l_ = mid;
        acc_spr_ += spr; ++acc_n_;
        // intrabar excursion bookkeeping only (management is bar-based == harness)
        if (pos.active) {
            const double fav = pos.side > 0 ? (mid - pos.entry_px) : (pos.entry_px - mid);
            const double f_bp = fav / pos.entry_px * 1e4;
            if (f_bp > pos.mfe_bp) pos.mfe_bp = f_bp;
            if (-f_bp > pos.mae_bp) pos.mae_bp = -f_bp;
        }
    }

    // One CLOSED M1 bar (ts_sec = minute start). bid/ask = executable quotes for
    // any fill this bar triggers (live: real; seed/parity: c -/+ spr/2 == harness).
    void on_m1_bar(int64_t ts_sec, double o, double h, double l, double c, double spr,
                   double bid, double ask, OnCloseFn cb) noexcept {
        if (c <= 0.0 || spr < 0.0 || h < l) return;
        Bar s{};
        s.ts = ts_sec; s.spr = spr;
        s.bidc = c - 0.5 * spr; s.askc = c + 0.5 * spr;
        s.lo = l; s.hi = h;
        s.nup = c > o ? 1 : 0; s.ndn = c < o ? 1 : 0;

        // ---- baselines: day rollover + slot accumulators (harness lines) ----
        const int32_t dayn = (int32_t)(s.ts / 86400), slot = (int32_t)((s.ts % 86400) / 1800);
        if (dayn != cur_day_) {
            if (cur_day_ >= 0) {
                for (int k = 0; k < 48; ++k)
                    if (day_secs_[k] > 0)
                        base_[k].push(day_act_[k] / day_secs_[k], day_spr_[k] / std::max(day_sprn_[k], 1.0));
                ++days_seen_;
            }
            reset_day_acc(); cur_day_ = dayn;
        }
        day_act_[slot] += actv(s); day_secs_[slot] += 1.0;
        day_spr_[slot] += s.spr;   day_sprn_[slot] += 1.0;

        // ---- rolling windows ----
        bars_.push_back(s);
        while (!bars_.empty() && bars_.front().ts < s.ts - DUR_HI) bars_.pop_front();

        // ---- manage open position first (bar-based, mirrors run_exit) ----
        if (pos.active) { manage_bar(s, bid, ask, cb); return; }

        if (days_seen_ < WARMUP_DAYS) return;
        if (base_[slot].med_rate <= 0.0) return;
        const double mid = 0.5 * (s.bidc + s.askc);
        const double sprnow = s.spr;

        // ---- detector state machine (family 1, frozen cell) ----
        if (ev_.state == 0) {
            // scan window for impulse anchor (both directions)
            double wmin = 1e18, wmax = -1e18; int64_t tmin = 0, tmax = 0; size_t imin = 0, imax = 0;
            for (size_t k = 0; k < bars_.size(); ++k) {
                const Bar& w = bars_[k];
                if (w.lo < wmin) { wmin = w.lo; tmin = w.ts; imin = k; }
                if (w.hi > wmax) { wmax = w.hi; tmax = w.ts; imax = k; }
            }
            for (int dir = 0; dir < 2; ++dir) {
                const double a = dir == 0 ? wmin : wmax;
                const int64_t ta = dir == 0 ? tmin : tmax;
                const size_t ia = dir == 0 ? imin : imax;
                if (ta == last_anchor_used_) continue;     // one CORE per structural event
                if (ta < last_exit_ts_) continue;          // new event must postdate last exit
                const int64_t el = s.ts - ta;
                if (el < DUR_LO || el > DUR_HI) continue;
                const double move_bp = dir == 0 ? (mid - a) / a * 1e4 : (a - mid) / a * 1e4;
                if (move_bp < IMP_LO || move_bp > IMP_HI) continue;
                // path efficiency (EFF_STRIDE sampling) + activity over [ta, now]
                double path = 0.0, nt = 0.0, ns = 0.0, vws = 0.0, vwn = 0.0;
                double pm = -1.0; int64_t pts = -4000000000LL;
                for (size_t k = ia; k < bars_.size(); ++k) {
                    const Bar& w = bars_[k];
                    const double wc = 0.5 * (w.bidc + w.askc);
                    if (w.ts >= pts + EFF_STRIDE) { if (pm > 0.0) path += std::fabs(wc - pm); pm = wc; pts = w.ts; }
                    nt += actv(w); ns += 1.0; vws += wc; vwn += 1.0;
                }
                const double eff = path > 0.0 ? std::fabs(mid - a) / path : 0.0;
                const double rate = ns > 0.0 ? nt / ns : 0.0;
                const double ratio = rate / base_[slot].med_rate;
                if (eff < EFF) continue;
                if (ratio < ACT) continue;
                if (sprnow > SPR_MAX * std::max(base_[slot].med_spr, 1e-9)) continue;
                ev_ = Ev{}; ev_.state = 1; ev_.side = dir == 0 ? +1 : -1;
                ev_.L0 = a; ev_.P = dir == 0 ? s.hi : s.lo;
                ev_.t0 = ta; ev_.tpk = s.ts; ev_.imp_rate = rate; ev_.act_ratio = ratio;
                ev_.vwap_s = vws; ev_.vwap_n = vwn;
                last_anchor_used_ = ta;
                break;
            }
        }
        else if (ev_.state == 1) {  // impulse riding: extend peak, detect pullback start
            ev_.vwap_s += mid; ev_.vwap_n += 1.0;
            const double Sz = std::fabs(ev_.P - ev_.L0);
            if (ev_.side > 0) { if (s.hi > ev_.P) { ev_.P = s.hi; ev_.tpk = s.ts; } }
            else              { if (s.lo < ev_.P) { ev_.P = s.lo; ev_.tpk = s.ts; } }
            const double ret = ev_.side > 0 ? (ev_.P - s.lo) / std::max(Sz, 1e-9)
                                            : (s.hi - ev_.P) / std::max(Sz, 1e-9);
            if (ret >= RET_LO) {
                ev_.state = 2; ev_.T = ev_.side > 0 ? s.lo : s.hi;
                ev_.pb_act = actv(s); ev_.pb_secs = 1.0;
                ev_.pbdn_act = actv(s); ev_.pbdn_secs = 1.0;
            }
            if (s.ts - ev_.tpk > PB_TIMEOUT) ev_.state = 0;
            const double sz_bp = Sz / ev_.L0 * 1e4;
            if (sz_bp > IMP_HI * 1.6) ev_.state = 0;       // runaway, not our structure
        }
        else if (ev_.state == 2) {  // pullback: track trough, wait for break entry
            ev_.vwap_s += mid; ev_.vwap_n += 1.0;
            ev_.pb_act += actv(s); ev_.pb_secs += 1.0;
            const double Sz = std::fabs(ev_.P - ev_.L0);
            bool newT = false;
            if (ev_.side > 0 && s.lo < ev_.T) { ev_.T = s.lo; newT = true; }
            if (ev_.side < 0 && s.hi > ev_.T) { ev_.T = s.hi; newT = true; }
            if (newT) { ev_.pbdn_act = ev_.pb_act; ev_.pbdn_secs = ev_.pb_secs; }
            const double ret = std::fabs(ev_.P - ev_.T) / std::max(Sz, 1e-9);
            const double vwap = ev_.vwap_n > 0.0 ? ev_.vwap_s / ev_.vwap_n : mid;
            const bool structural_fail = ev_.side > 0 ? (ev_.T <= ev_.L0) : (ev_.T >= ev_.L0);
            const bool vwap_fail = ev_.side > 0 ? (ev_.T < vwap * (1.0 - VWTOL * 1e-4))
                                                : (ev_.T > vwap * (1.0 + VWTOL * 1e-4));
            if (ret > RET_HI || structural_fail || vwap_fail || s.ts - ev_.tpk > PB_TIMEOUT) { ev_.state = 0; return; }
            if (ret < RET_LO) return;
            // pullback on lower down-leg activity
            const double pbrate = ev_.pbdn_secs > 0.0 ? ev_.pbdn_act / ev_.pbdn_secs : 0.0;
            if (pbrate > PB_ACT * ev_.imp_rate) return;
            // entry trigger: 2h local-extreme break (excl. this bar) + imbalance + reclaim + remaining
            double brk_ext = ev_.side > 0 ? -1e18 : 1e18; bool have_brk = false;
            for (size_t k = 0; k + 1 < bars_.size(); ++k) {
                const Bar& w = bars_[k];
                if (w.ts < s.ts - BREAKSEC) continue;
                have_brk = true;
                if (ev_.side > 0) brk_ext = std::max(brk_ext, (double)w.hi);
                else              brk_ext = std::min(brk_ext, (double)w.lo);
            }
            const bool broke = have_brk && (ev_.side > 0 ? s.hi > brk_ext : s.lo < brk_ext);
            if (!broke) return;
            double nup = 0.0, ndn = 0.0;
            for (size_t k = 0; k < bars_.size(); ++k) {
                const Bar& w = bars_[k];
                if (w.ts < s.ts - UPQ_WIN) continue;
                nup += w.nup; ndn += w.ndn;
            }
            const double upr = (nup + ndn) > 0.0 ? (ev_.side > 0 ? nup / (nup + ndn) : ndn / (nup + ndn)) : 0.0;
            if (upr < UPR) return;
            const bool reclaim = ev_.side > 0 ? (mid >= vwap) : (mid <= vwap);
            if (!reclaim) return;
            const double entry_px = ev_.side > 0 ? ask : bid;
            if (entry_px <= 0.0) return;
            const double target = ev_.side > 0 ? ev_.T + Sz : ev_.T - Sz;   // measured-move
            const double rem_bp = ev_.side > 0 ? (target - entry_px) / entry_px * 1e4
                                               : (entry_px - target) / entry_px * 1e4;
            if (rem_bp < REM) return;
            const double stop_px = ev_.side > 0 ? ev_.L0 * (1.0 - STOPBUF * 1e-4)
                                                : ev_.L0 * (1.0 + STOPBUF * 1e-4);   // ANCHOR stop
            fire_entry(ev_.side, entry_px, stop_px, bid, ask, s.ts);
            ev_.state = 0;
        }
    }

    void force_close(double bid, double ask, int64_t now_sec, OnCloseFn cb) noexcept {
        if (!pos.active) return;
        close_pos(pos.side > 0 ? bid : ask, "FORCE_CLOSE", now_sec, cb);
    }

    // ---- restart persistence (wire_cross archetype) ----
    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const noexcept {
        if (!pos.active) return false;
        o.engine = eng; o.symbol = sym;
        o.side = pos.side > 0 ? "LONG" : "SHORT"; o.size = lot;
        o.entry = pos.entry_px; o.sl = pos.stop_px;
        o.tp = pos.hwm;                       // NOT a take-profit: high-water mark (trail arm state)
        o.entry_ts = pos.entry_ts; o.mfe = pos.mfe_bp; o.mae = pos.mae_bp;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) noexcept {
        if (pos.active || ps.entry <= 0.0) return false;
        pos.active = true;
        pos.side = (ps.side == "SHORT") ? -1 : +1;
        pos.entry_px = ps.entry; pos.stop_px = ps.sl;
        pos.hwm = (ps.tp > 0.0) ? ps.tp : ps.entry;
        pos.entry_ts = ps.entry_ts;
        pos.mfe_bp = ps.mfe; pos.mae_bp = ps.mae;
        pos.armed = pos.mfe_bp >= ARM;        // trail queue rebuilds from live bars (stop monotonic)
        trailq_.clear();
        return true;
    }

    // ---- warm-seed: replay an M1 csv (ts,o,h,l,c,spr; ts sec) through the same
    // bar path with entries suppressed. Baselines/windows warm; nothing books. ----
    size_t seed_m1_from_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) {
            std::printf("[SEED-FATAL] GoldCampaignD1Anch: cannot open '%s'\n", path.c_str());
            std::fflush(stdout); return 0;
        }
        const bool was = seeding_; seeding_ = true;
        std::string line; std::getline(f, line); size_t n = 0;
        while (std::getline(f, line)) {
            double ts = 0, o = 0, h = 0, l = 0, c = 0, sp = 0;
            if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c, &sp) != 6) continue;
            if (c <= 0.0) continue;
            on_m1_bar((int64_t)ts, o, h, l, c, sp, c - 0.5 * sp, c + 0.5 * sp, OnCloseFn{});
            ++n;
        }
        seeding_ = was;
        std::printf("[SEED] GoldCampaignD1Anch %zu M1 bars replayed days_seen=%d slots_warm=%d -- %s\n",
                    n, days_seen_, warm_slots(), days_seen_ >= WARMUP_DAYS ? "hot" : "COLD (needs more days)");
        std::fflush(stdout);
        return n;
    }

    int warm_slots() const noexcept {
        int w = 0; for (int k = 0; k < 48; ++k) if (base_[k].med_rate > 0.0) ++w; return w;
    }

private:
    struct Bar { int64_t ts = 0; double bidc = 0, askc = 0, lo = 0, hi = 0, spr = 0; int nup = 0, ndn = 0; };
    struct SlotBase {
        std::deque<double> rate, spr;
        double med_rate = 0.0, med_spr = 0.0;
        void push(double r, double s) {
            rate.push_back(r); spr.push_back(s);
            if (rate.size() > 20) { rate.pop_front(); spr.pop_front(); }
            auto med = [](std::deque<double> d) {
                std::sort(d.begin(), d.end()); return d.empty() ? 0.0 : d[d.size() / 2]; };
            med_rate = med(rate); med_spr = med(spr);
        }
    };
    struct Ev {
        int state = 0, side = 0;
        double L0 = 0, P = 0, T = 0;
        int64_t t0 = 0, tpk = 0;
        double imp_rate = 0, vwap_n = 0, vwap_s = 0;
        double pb_act = 0, pb_secs = 0, pbdn_act = 0, pbdn_secs = 0;
        double act_ratio = 0;
    };

    static double actv(const Bar& s) noexcept {  // M1-mode activity = bar range in bp
        return (s.hi - s.lo) / (0.5 * (s.hi + s.lo)) * 1e4;
    }

    void reset_day_acc() noexcept {
        for (int k = 0; k < 48; ++k) day_act_[k] = day_secs_[k] = day_spr_[k] = day_sprn_[k] = 0.0;
    }

    void fire_entry(int side, double entry_px, double stop_px, double bid, double ask, int64_t ts_sec) noexcept {
        if (seeding_) return;
        if (retired_) return;
        if (ask - bid > max_spread) return;
        const double stop_dist = std::fabs(entry_px - stop_px);
        if (!::ExecutionCostGuard::is_viable("XAUUSD", ask - bid, stop_dist, lot, 1.5)) return;
        pos.active = true; pos.side = side;
        pos.entry_px = entry_px; pos.stop_px = stop_px; pos.hwm = entry_px;
        pos.armed = false; pos.entry_ts = ts_sec; pos.mfe_bp = pos.mae_bp = 0.0;
        trailq_.clear();
        std::printf("[GoldCampaignD1Anch] ENTRY %s px=%.2f stop=%.2f (%.0fbp) anchor=%.2f%s\n",
                    side > 0 ? "LONG" : "SHORT", entry_px, stop_px,
                    stop_dist / entry_px * 1e4, ev_.L0, shadow_mode ? " [SHADOW]" : "");
        std::fflush(stdout);
    }

    // mirrors run_exit() bar step: excursions -> trail window -> arm/ratchet -> stop -> max-hold
    void manage_bar(const Bar& s, double bid, double ask, OnCloseFn cb) noexcept {
        const int side = pos.side;
        if (side > 0) {
            pos.mfe_bp = std::max(pos.mfe_bp, (s.hi - pos.entry_px) / pos.entry_px * 1e4);
            pos.mae_bp = std::max(pos.mae_bp, (pos.entry_px - s.lo) / pos.entry_px * 1e4);
        } else {
            pos.mfe_bp = std::max(pos.mfe_bp, (pos.entry_px - s.lo) / pos.entry_px * 1e4);
            pos.mae_bp = std::max(pos.mae_bp, (s.hi - pos.entry_px) / pos.entry_px * 1e4);
        }
        const double ext = side > 0 ? s.lo : s.hi;
        while (!trailq_.empty() && (side > 0 ? trailq_.back().second >= ext
                                             : trailq_.back().second <= ext)) trailq_.pop_back();
        trailq_.push_back({s.ts, ext});
        while (!trailq_.empty() && trailq_.front().first < s.ts - TRAILSEC) trailq_.pop_front();
        if (side > 0) {
            if (s.hi > pos.hwm) pos.hwm = s.hi;
            if (!pos.armed && (pos.hwm - pos.entry_px) / pos.entry_px * 1e4 >= ARM) pos.armed = true;
            if (pos.armed && !trailq_.empty()) {
                const double tl = trailq_.front().second * (1.0 - STOPBUF * 1e-4);
                if (tl > pos.stop_px) pos.stop_px = tl;
            }
            if (s.lo <= pos.stop_px) { close_pos(std::min(bid, pos.stop_px), "ANCHOR_STOP_OR_TRAIL", s.ts, cb); return; }
        } else {
            if (s.lo < pos.hwm || pos.hwm == pos.entry_px) pos.hwm = std::min(pos.hwm, s.lo);
            if (!pos.armed && (pos.entry_px - pos.hwm) / pos.entry_px * 1e4 >= ARM) pos.armed = true;
            if (pos.armed && !trailq_.empty()) {
                const double tl = trailq_.front().second * (1.0 + STOPBUF * 1e-4);
                if (tl < pos.stop_px) pos.stop_px = tl;
            }
            if (s.hi >= pos.stop_px) { close_pos(std::max(ask, pos.stop_px), "ANCHOR_STOP_OR_TRAIL", s.ts, cb); return; }
        }
        if (s.ts - pos.entry_ts > MAXHOLD) { close_pos(side > 0 ? bid : ask, "MAX_HOLD", s.ts, cb); return; }
    }

    void close_pos(double exit_px, const char* reason, int64_t now_sec, OnCloseFn cb) noexcept {
        if (!pos.active) return;
        const double pts_move = pos.side > 0 ? (exit_px - pos.entry_px) : (pos.entry_px - exit_px);
        const double net_bp = pts_move / pos.entry_px * 1e4;
        banked_net_bp_ += net_bp;
        if (!retired_ && banked_net_bp_ <= retire_net_bp) {
            retired_ = true;
            std::printf("[GoldCampaignD1Anch][RETIRED] banked net %.0fbp <= %.0fbp -- new entries "
                        "blocked (auto-retirement, 2x worst pooled DD; S-2026-07-13z)\n",
                        banked_net_bp_, retire_net_bp);
            std::fflush(stdout);
        }
        omega::TradeRecord tr;
        tr.symbol     = "XAUUSD";
        tr.engine     = engine_tag;
        tr.side       = pos.side > 0 ? "LONG" : "SHORT";
        tr.entryPrice = pos.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = 0.0;
        tr.sl         = pos.stop_px;
        tr.size       = lot;
        tr.entryTs    = pos.entry_ts;
        tr.exitTs     = now_sec;
        tr.exitReason = reason ? reason : "";
        tr.regime     = "CampaignCore_D1Anchor_family1";
        tr.shadow     = shadow_mode;
        tr.pnl        = pts_move * lot;   // RAW pts x lot; ledger applies tick_value_multiplier
        tr.mfe        = pos.mfe_bp * 1e-4 * pos.entry_px;   // pts, ledger convention
        tr.mae        = pos.mae_bp * 1e-4 * pos.entry_px;
        if (cb && !seeding_) cb(tr);
        pos = {};
        trailq_.clear();
        last_exit_ts_ = now_sec;
    }

    // ---- state ----
    std::deque<Bar> bars_;              // rolling DUR_HI (72h) window
    SlotBase base_[48];
    double day_act_[48] = {0}, day_secs_[48] = {0}, day_spr_[48] = {0}, day_sprn_[48] = {0};
    int32_t cur_day_ = -1; int days_seen_ = 0;
    Ev ev_{};
    int64_t last_exit_ts_ = 0, last_anchor_used_ = 0;
    std::deque<std::pair<int64_t, double>> trailq_;   // (ts, extreme) monotonic, rolling TRAILSEC
    int64_t acc_min_ = -1; double acc_o_ = 0, acc_h_ = 0, acc_l_ = 0, acc_c_ = 0, acc_spr_ = 0; int acc_n_ = 0;
    bool seeding_ = false;
    double banked_net_bp_ = 0.0; bool retired_ = false;
};

} // namespace omega

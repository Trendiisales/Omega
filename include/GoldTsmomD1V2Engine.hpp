#pragma once
//  ADVERSE-PROTECTION: VOL-TARGETED SIZING + MONTHLY REBALANCE + DIRECTION FLIP
//  = the protection, BY DESIGN (D1-close allocation book, no intrabar exposure
//  management -- there is no stop to place inside a monthly period without
//  changing the exposure profile the validation ran with). Backtested
//  (gdd_tsmom_cot.py chassis, GC=F daily 2015-2026, 0.31pt/turnover-unit):
//  exposure = comp{42,63,84}d x min(2, 15%/realized20d-vol) DE-LEVERS as vol
//  rises and FLIPS SHORT in sustained downtrends -- the one gold-TSMOM cell that
//  was NET SHORT PROFITABLY through the 2022 bear (+129pt validated cell /
//  +117pt wired rule). Worst episodes from the BT: maxDD -413pt (validated
//  21-trading-day cell, PF2.26 +2915pt n120) / -491pt trough 2021-12 (wired
//  calendar-month rule, PF2.09 +2689pt n121 both-halves+ 2x-cost PF2.08),
//  worst single period -233pt. AUTO-RETIREMENT latch: banked net <= -1000pt
//  (~2x the -491pt worst wired-rule DD episode) forces weight 0 at rebalance.
//  Evidence: outputs/GOLD_DEEP_DIVE_2026-07-08.md Study 4 (commit 4bca1036).
// =============================================================================
//  GoldTsmomD1V2Engine.hpp  (S-2026-07-08c)
//  D1 time-series-momentum composite on gold, BOTH directions, vol-targeted:
//    comp = mean over lb in {42,63,84} of sign(close > close[lb ago])   (+/-1 each)
//    lev  = min(2, 0.15 / (pstdev(last 21 daily rets) * sqrt(252)))
//    want = comp * lev          -- rebalanced on the FIRST trading day of each
//                                   calendar month (restart-proof "monthly
//                                   rebalance"; validated 21-trading-day cadence
//                                   PF2.26, calendar-month re-run PF2.09 -- both
//                                   on the same harness/data, see header top)
//  {42,63,84} ONLY -- the institutional-default 12m lookback is the WORST cell
//  (PF1.27, 2022 -540) and the classic {1,3,6,12m} composite is 2022-NEGATIVE;
//  do not "upgrade" this band without a fresh all-6-gates backtest.
//  DO NOT long-gate the shorts (no gold_regime() veto here): the short side
//  through 2022 IS the validated edge of this structure.
//
//  Per-period accounting mirrors the harness exactly: at each rebalance the
//  ENDING period books pnl_pts = w*(close - anchor) - |want - w| * 0.31/2.
//  An allocation book, not a trade engine: n(statistical unit) = rebalance
//  periods. D1-close grade; ~10 weight adjustments/yr. Drive from gold ticks
//  (internal UTC-D1 aggregation, CalendarTom pattern); weekend stub days are
//  dropped so live == seed == harness (trading-day closes only).
// =============================================================================
#include "OmegaTradeLedger.hpp"     // omega::TradeRecord
#include "OmegaCostGuard.hpp"       // ::ExecutionCostGuard (GLOBAL scope)
#include "OpenPositionRegistry.hpp" // omega::PositionSnapshot (persist)
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <deque>
#include <fstream>
#include <functional>
#include <string>

namespace omega {

class GoldTsmomD1V2Engine {
public:
    // ---- config ----
    bool   enabled        = false;
    bool   shadow_mode    = true;   // paper until the live shadow ledger proves it
    double lot            = 0.01;   // XAU spot lot per 1.0 weight-unit (0.01 lot = 1oz -> $1/pt)
    double target_vol     = 0.15;   // annualized vol target (validated 15%)
    double max_lev        = 2.0;    // leverage cap (validated)
    // S-2026-07-11 GOLD PHASE 1 (GOLD_BOOK_ROADMAP bug 3, CONFIRMED): the old
    // cost_rt_pts=0.31 was the MGC FUTURES basis on what runs as an XAU SPOT
    // book. Replaced with the honest spot round-trip in bp of price (6bp base
    // + 2bp slip), charged price-proportionally per turnover unit at rebalance.
    // Re-validated both ways (backtest/gold_tsmom_cost_basis_bt.py, GC_F daily
    // 2015-2026, wired calendar-month rule; 0.31pt basis reproduces the
    // documented parity n129 +2604.1pt PF1.98 exactly):
    //   spot 7bp PF1.96 +2566.6 | 8bp PF1.96 +2559.5 (H1 +261.1/H2 +2298.4,
    //   2022 +114.3, maxDD -497.7) | 10bp PF1.95 | 16bp 2x-stress PF1.92.
    // Turnover is ~10 adjustments/yr -> cost basis moves net only ~1.7% over
    // 11yr. SURVIVES SPOT AT TRUE COST -- no venue move required.
    double cost_rt_bp     = 8.0;    // honest XAU-spot RT cost, bp of rebalance price
    double retire_net_pts = -1000.0; // auto-retirement latch (2x worst wired-rule DD -491pt; 8bp-basis maxDD -497.7 keeps it valid)

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    bool has_open_position() const noexcept { return w_ != 0.0; }
    double weight() const noexcept { return w_; }

    // ---- tick -> UTC D1 aggregation (CalendarTom pattern) ----
    void on_tick(double bid, double ask, int64_t now_ms, OnCloseFn cb) noexcept {
        if (bid <= 0.0 || ask <= 0.0) return;
        last_bid_ = bid; last_ask_ = ask;
        // Fresh-boot adoption of a seed-computed weight: accrue only from NOW
        // (honest anchor -- no backfilled pre-boot pnl; a persisted snapshot
        // restore overwrites this with the real live anchor instead).
        if (w_ != 0.0 && period_start_ts_ == 0) {
            px_anchor_ = (bid + ask) * 0.5; period_start_ts_ = now_ms;
            std::printf("[GoldTsmomD1V2] seeded weight adopted live w=%+.2f anchor=%.2f (accrual from boot)\n",
                        w_, px_anchor_);
            std::fflush(stdout);
        }
        const double mid = (bid + ask) * 0.5;
        const int64_t day = (now_ms / 86400000LL) * 86400000LL;
        if (!acc_open_) { acc_open_ = true; acc_day_ = day; acc_h_ = acc_l_ = acc_c_ = mid; return; }
        if (day != acc_day_) {
            on_d1_bar(acc_h_, acc_l_, acc_c_, bid, ask, acc_day_, cb);
            acc_day_ = day; acc_h_ = acc_l_ = acc_c_ = mid;
        } else { if (mid > acc_h_) acc_h_ = mid; if (mid < acc_l_) acc_l_ = mid; acc_c_ = mid; }
    }

    // One CLOSED UTC day (day_ms = its 00:00 UTC). Weekend stubs dropped.
    void on_d1_bar(double h, double l, double c, double bid, double ask,
                   int64_t day_ms, OnCloseFn cb) noexcept {
        const int64_t z = day_ms / 86400000LL;
        if (!is_weekday_z(z) || c <= 0.0) return;
        update_atr(h, l, c);
        if (prev_c_ > 0.0) {
            rets_.push_back(c / prev_c_ - 1.0);
            while ((int)rets_.size() > 21) rets_.pop_front();
        }
        prev_c_ = c;
        closes_.push_back(c);
        while ((int)closes_.size() > 100) closes_.pop_front();

        // MFE/MAE for the held weight (points, direction-signed)
        if (w_ != 0.0 && px_anchor_ > 0.0) {
            const double fav = (w_ > 0.0) ? (h - px_anchor_) : (px_anchor_ - l);
            const double adv = (w_ > 0.0) ? (px_anchor_ - l) : (h - px_anchor_);
            if (fav > mfe_) mfe_ = fav; if (adv > mae_) mae_ = adv;
        }

        // Calendar-month flip: the day that just closed is the first trading
        // day of a new month -> rebalance at ITS close (harness-equivalent).
        int y, m, d; civil_from_days(z, y, m, d);
        const bool month_flip = (last_close_month_ != 0 && (m != last_close_month_ || y != last_close_year_));
        last_close_month_ = m; last_close_year_ = y;
        if (month_flip && enabled && (int)closes_.size() >= 85 && (int)rets_.size() >= 21)
            rebalance(c, bid, ask, day_ms, cb);
    }

    void force_close(double bid, double ask, int64_t now_sec, OnCloseFn cb) noexcept {
        if (w_ == 0.0) return;
        const double mid = (bid > 0.0 && ask > 0.0) ? (bid + ask) * 0.5 : px_anchor_;
        book_period(mid, 0.0, now_sec > 0 ? now_sec * 1000LL : period_start_ts_, "FORCE_CLOSE", cb);
        w_ = 0.0; px_anchor_ = 0.0; period_start_ts_ = 0; mfe_ = mae_ = 0.0;
    }

    // ---- restart persistence (wire_cross archetype). The carried state is the
    // signed weight + period anchor; the D1 history re-warms from the seed CSV.
    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const noexcept {
        if (w_ == 0.0) return false;
        o.engine = eng; o.symbol = sym; o.side = (w_ > 0.0) ? "LONG" : "SHORT";
        o.size = std::fabs(w_) * lot;            // real lots; weight = size/lot
        o.entry = px_anchor_; o.sl = 0.0; o.tp = 0.0;
        o.entry_ts = period_start_ts_ / 1000; o.mfe = mfe_; o.mae = mae_;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) noexcept {
        if (ps.size <= 0.0 || lot <= 0.0) return false;
        w_ = (ps.side == "SHORT" ? -1.0 : 1.0) * (ps.size / lot);
        px_anchor_ = ps.entry; period_start_ts_ = ps.entry_ts * 1000LL;
        mfe_ = ps.mfe; mae_ = ps.mae;
        return true;                             // overwrites any seed-derived weight
    }

    // Warm-seed from phase1/signal_discovery/warmup_XAUUSD_D1.csv
    // (bar_start_ms,open,high,low,close). Replays the full history INCLUDING
    // rebalances so the model weight is warm at boot; ledger cb is a nub and the
    // period anchor is reset to sentinel (adopted at first live tick, or
    // overwritten by a persisted snapshot) -- no pre-boot pnl can book.
    size_t seed_from_d1_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) {
            std::printf("[SEED-FATAL] GoldTsmomD1V2: cannot open '%s'\n", path.c_str());
            std::fflush(stdout); return 0;
        }
        const bool was_enabled = enabled; enabled = true;   // rebalances must run in seed
        const bool was_seed = seeding_; seeding_ = true;
        std::string line; std::getline(f, line); size_t n = 0;
        while (std::getline(f, line)) {
            double ts = 0, o = 0, hh = 0, ll = 0, cc = 0;
            if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &ts, &o, &hh, &ll, &cc) != 5) continue;
            if (cc <= 0.0) continue;
            int64_t day_ms = (ts > 1e11) ? (int64_t)ts : (int64_t)(ts * 1000.0);
            day_ms = (day_ms / 86400000LL) * 86400000LL;
            const double sp = cc * 0.00010;
            on_d1_bar(hh, ll, cc, cc - sp, cc + sp, day_ms, OnCloseFn{});
            ++n;
        }
        seeding_ = was_seed; enabled = was_enabled;
        px_anchor_ = 0.0; period_start_ts_ = 0; mfe_ = mae_ = 0.0;  // live anchor set at first tick / restore
        std::printf("[SEED] GoldTsmomD1V2 %zu D1 bars replayed closes=%zu rets=%zu w_seed=%+.2f -- hot\n",
                    n, closes_.size(), rets_.size(), w_);
        std::fflush(stdout);
        return n;
    }

private:
    // ---- portable calendar (no gmtime; Hinnant civil-from-days) ----
    static void civil_from_days(int64_t zz, int& y, int& m, int& d) noexcept {
        zz += 719468;
        int64_t era = (zz >= 0 ? zz : zz - 146096) / 146097;
        int64_t doe = zz - era * 146097;
        int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        int64_t yy  = yoe + era * 400;
        int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        int64_t mp  = (5 * doy + 2) / 153;
        d = (int)(doy - (153 * mp + 2) / 5 + 1);
        m = (int)(mp < 10 ? mp + 3 : mp - 9);
        y = (int)(yy + (m <= 2));
    }
    static bool is_weekday_z(int64_t z) noexcept {
        const int w = (int)(((z % 7) + 4 + 7) % 7);   // 0=Sun..6=Sat
        return w >= 1 && w <= 5;
    }

    void update_atr(double h, double l, double c) noexcept {
        if (atr_prev_c_ <= 0.0) { atr_prev_c_ = c; return; }
        const double tr = std::fmax(h - l, std::fmax(std::fabs(h - atr_prev_c_), std::fabs(l - atr_prev_c_)));
        atr_ = (atr_ <= 0.0) ? tr : (atr_ * 13.0 + tr) / 14.0;
        atr_prev_c_ = c;
    }

    // population stdev of rets_ (harness statistics.pstdev), annualized
    double realized_vol_ann() const noexcept {
        const int n = (int)rets_.size(); if (n < 2) return 0.0;
        double mu = 0.0; for (double r : rets_) mu += r; mu /= n;
        double v = 0.0; for (double r : rets_) v += (r - mu) * (r - mu); v /= n;
        return std::sqrt(v) * std::sqrt(252.0);
    }

    void rebalance(double c, double bid, double ask, int64_t day_ms, OnCloseFn cb) noexcept {
        // composite sign over {42,63,84}d (BOTH directions -- no long-gating)
        const int LBS[3] = { 42, 63, 84 };    // plain array (MSVC-conservative)
        const int nn = (int)closes_.size();
        double comp = 0.0;
        for (int k = 0; k < 3; ++k)
            comp += (closes_[nn - 1] > closes_[nn - 1 - LBS[k]]) ? 1.0 : -1.0;
        comp /= 3.0;
        double rv = realized_vol_ann();
        if (rv < 1e-9) rv = 1e-9;                 // harness: `pstdev*sqrt(252) or 1e-9`
        const double lev = std::fmin(max_lev, target_vol / rv);
        double want = comp * lev;

        // AUTO-RETIREMENT latch: forced flat at rebalance, no new weights.
        if (retired_) want = 0.0;
        // ExecutionCostGuard (GLOBAL scope) on the entry side of the rebalance
        // (opening a new weight from flat, or flipping direction).
        if (want != 0.0 && (w_ == 0.0 || (want > 0.0) != (w_ > 0.0)) && !seeding_ && atr_ > 0.0) {
            if (!::ExecutionCostGuard::is_viable("XAUUSD", ask - bid, atr_, lot, 1.5))
                want = 0.0;                       // close-only rebalance this month
        }

        if (px_anchor_ > 0.0 && (w_ != 0.0 || std::fabs(want - w_) > 1e-9)) {
            // harness-faithful: ENDING period pnl, turnover cost charged here.
            // S-2026-07-11: honest spot cost = bp of the rebalance price (was the
            // fixed 0.31pt MGC-basis constant on a spot book).
            book_period(c, std::fabs(want - w_) * (c * cost_rt_bp * 1e-4) * 0.5, day_ms, "REBALANCE", cb);
        }
        if (!seeding_ && std::fabs(want - w_) > 1e-9) {
            std::printf("[GoldTsmomD1V2] REBALANCE comp=%+.2f rv=%.3f lev=%.2f w %+.2f -> %+.2f px=%.2f%s\n",
                        comp, rv, lev, w_, want, c, shadow_mode ? " [SHADOW]" : "");
            std::fflush(stdout);
        }
        w_ = want; px_anchor_ = c; period_start_ts_ = day_ms; mfe_ = mae_ = 0.0;
    }

    void book_period(double px, double turnover_cost_pts, int64_t exit_ms,
                     const char* why, OnCloseFn cb) noexcept {
        const double pnl_pts = w_ * (px - px_anchor_) - turnover_cost_pts;
        banked_net_pts_ += pnl_pts;
        const bool was_retired = retired_;
        retired_ = (banked_net_pts_ <= retire_net_pts);
        if (retired_ && !was_retired) {
            std::printf("[GoldTsmomD1V2][RETIRED] banked net %.1fpt <= %.1fpt -- weight forced 0 at "
                        "rebalance (auto-retirement, 2x worst BT DD; S-2026-07-08c)\n",
                        banked_net_pts_, retire_net_pts);
            std::fflush(stdout);
        }
        if (seeding_) return;                     // seed replay: state only, no ledger
        omega::TradeRecord tr;
        tr.symbol     = "XAUUSD";
        tr.engine     = "GoldTsmomD1V2";
        tr.side       = (w_ > 0.0) ? "LONG" : (w_ < 0.0 ? "SHORT" : "FLAT");
        tr.entryPrice = px_anchor_;
        tr.exitPrice  = px;
        tr.size       = std::fabs(w_) * lot;
        // S-2026-07-11: emit RAW pts x lot per the ledger contract (trade_lifecycle
        // Step 1 multiplies by tick_value_multiplier("XAUUSD")=100). The old
        // pre-multiplied `pnl_pts * lot * 100.0` either tripped the central
        // PNL-DOUBLE-MULT backstop (P1 noise, mfe/mae zeroed) or -- when the
        // cost-dominated ratio fell outside the [0.7x,1.4x] net -- booked 100x
        // inflated USD.
        tr.pnl        = pnl_pts * lot;
        tr.entryTs    = period_start_ts_ / 1000;
        tr.exitTs     = exit_ms / 1000;
        tr.shadow     = shadow_mode;
        tr.mfe        = mfe_; tr.mae = mae_;
        tr.exitReason = why ? why : "";
        if (cb) cb(tr);
    }

    // ---- state ----
    std::deque<double> closes_;                   // trading-day closes (>=85 for lb84)
    std::deque<double> rets_;                     // last 21 daily returns
    double prev_c_ = 0.0;
    double atr_ = 0.0, atr_prev_c_ = 0.0;         // D1 ATR14 (cost-guard input only)
    int    last_close_month_ = 0, last_close_year_ = 0;
    bool   acc_open_ = false; int64_t acc_day_ = 0;
    double acc_h_ = 0.0, acc_l_ = 0.0, acc_c_ = 0.0, last_bid_ = 0.0, last_ask_ = 0.0;
    bool   seeding_ = false;

    double  w_ = 0.0;                             // signed weight (comp*lev), |w| <= 2
    double  px_anchor_ = 0.0;                     // period anchor close
    int64_t period_start_ts_ = 0;                 // ms; 0 = sentinel (adopt at first tick)
    double  mfe_ = 0.0, mae_ = 0.0;
    double  banked_net_pts_ = 0.0;
    bool    retired_ = false;
};

} // namespace omega

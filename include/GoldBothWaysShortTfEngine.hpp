#pragma once
//  ADVERSE-PROTECTION: per-mechanism BACKTESTED verdict (S-2026-07-14ax study,
//  backtest/gold_shorttf_bothways_bt.cpp on CERTIFIED MGC 30m splice 2024-06..
//  2026-07, 0.41pt RT + 2x stress, gap-honest adverse-first fills; findings
//  backtest/GOLD_SHORTTF_BOTHWAYS_2026H1_FINDINGS.md):
//    KELT m30 k1.25:  hard 2xATR(20) stop + 2.5xATR trail + 96-bar time stop.
//                     worst -$1,240 / maxDD $5,458 per 1 MGC (6mo). Tightening
//                     exits HURTS (fixed-R kills the long leg). NO loss-cut by
//                     design -- registry section-7 trap (spot LC kills MGC).
//    TF1H  ema-BW:    hard 2xATR(14) stop + 2.0xATR trail + reversal exit.
//                     worst -$1,822 / maxDD $7,430-7,858. Wider trails degrade
//                     net AND DD -- do not widen. NO loss-cut (same trap).
//    DON   h1 20/10:  hard 3xATR(14) adverse-first stop + opposite-channel
//                     close exit (MgcFastDonchian-family verdict). worst
//                     -$2,287 / maxDD $8,006.
//    DON  15m 60/35:  (S-2026-07-14 sub-30m study + operator BIG-GO sweep,
//                     backtest/gold_subh30_tf_bt.cpp DON15_STOP=1, certified
//                     spot-1m splice at MGC cost) hard 3.5xATR(14) adverse-
//                     first stop + opposite-channel close exit. worst -$945 /
//                     maxDD $3,794 per 1 MGC (6mo); whole plateau 144/144
//                     GATE-PASS -- stop lever 2.5-4.0 all pass, 3.5 interior peak.
//    DON  10m 30/35:  same sweep (DON10_SWEEP=1), operator accepts PF<1.3 at
//                     10m (actual PF 1.52). hard 3.0xATR(14) adverse-first
//                     stop + opposite-channel close exit. worst -$972 /
//                     maxDD $4,891 per 1 MGC (6mo); Nout>=31 slow-exit plateau
//                     GATE-PASS across Nin 20-50 x stop 2.5-3.5.
//  Plus AUTO-RETIREMENT latch per instance (retire_net_pts ~= -2x the config's
//  BT maxDD in points) -- banked net at BT cost <= latch blocks new entries.
//  Loss driver is regime-turn clusters (Feb/Apr-2026), not single-trade blowups.
// =============================================================================
//  GoldBothWaysShortTfEngine.hpp  (S-2026-07-14bc)
//  Symmetric LONG+SHORT short-timeframe gold engine on the MGC 30m feed --
//  the S-2026-07-14ax study's viable mechanisms, one instance per config:
//    KELT: M30 Keltner(EMA20, k*ATR20) close-through breakout both ways.
//    EMA : H1 dual-EMA trend-follow both ways (fast/slow alignment + close
//          beyond fast + 3-bar impulse >= imp_atr*ATR14), reversal exit.
//    DON : H1 Donchian Nin/Nout close-through both ways, 3xATR hard stop.
//  BOTH-WAYS BY DESIGN: no gold_regime long-block -- the short side IS the
//  bear-regime coverage (study window net -760pt tape, both legs positive;
//  the 6mo short edge is regime-specific per findings caveat #1).
//
//  Fidelity vs the harness (signals on native-bar close, managed from next
//  bar, adverse-first): stops are LEVEL checks -- live checks them on every
//  30m sub-bar (gap-honest at the row open), finer than the harness's native
//  bar, same trigger semantics (house convention: the MgcTF4h/2h port drive).
//  Trail update / reversal / channel / time-stop / entries: NATIVE close only,
//  exactly the harness order. H1 buckets aggregate 2x30m internally; the
//  :30-row close completes a bucket immediately (no next-bucket lag).
//
//  Judged via the live SHADOW ledger (primary record). Mimic: each instance
//  fires omega::gold_trend_mimic() one-way triggers (tag = mimic_tag) and
//  feeds its native bar -- the BE-mimic books are configured in engine_init.
// =============================================================================
#include "OmegaTradeLedger.hpp"     // omega::TradeRecord
#include "OmegaCostGuard.hpp"       // ::ExecutionCostGuard (GLOBAL scope)
#include "OpenPositionRegistry.hpp" // omega::PositionSnapshot (persist)
#include "GoldTrendMimicLadder.hpp" // omega::gold_trend_mimic() (one-way notify)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <deque>
#include <fstream>
#include <functional>
#include <string>

namespace omega {

struct GoldBothWaysShortTfEngine {
    enum class Mech { KELT, EMA, DON };

    // ---- config (set in omega_main.hpp before init/seed) ----
    bool        enabled     = false;
    bool        shadow_mode = true;     // paper until the live shadow ledger proves it
    double      lot         = 1.0;      // 1 MGC micro = 10oz = $10/pt (contracts)
    Mech        mech        = Mech::KELT;
    int         tf_secs     = 1800;     // native bar: 900 = M15, 1800 = M30, 3600 = H1
    int         row_secs    = 1800;     // feed row grain (S-2026-07-14: 600/900 for the
                                        // mgc_10m/15m_live.csv fine feeds; must divide tf_secs)
    // KELT params
    double      kelt_k      = 1.25;     // band width (k * ATR20 around EMA20)
    // EMA params
    int         ema_fast_n  = 10, ema_slow_n = 40;
    double      imp_atr     = 0.5;      // 3-bar impulse gate (ATRs)
    // DON params
    int         don_in      = 20, don_out = 10;
    // shared exits
    int         atr_n       = 20;       // 20 for KELT, 14 for EMA/DON (study)
    double      stop_atr    = 2.0;      // initial hard stop distance (ATRs)
    double      trail_atr   = 2.5;      // 0 = no trail (DON)
    int         time_stop_bars = 0;     // 0 = none (96 for KELT = 2 days M30)
    int         warm_bars   = 100;      // native bars before entries may fire
    double      cost_rt_pts = 0.41;     // certified MGC RT (latch accounting)
    double      retire_net_pts = -1100.0; // auto-retirement (-2x BT maxDD pts)
    double      mgc_spread_pts = 0.10;  // 1 exchange tick (cost-guard input)
    std::string engine_tag  = "GoldKeltM30";  // ledger engine name
    std::string mimic_tag;              // non-empty => gold_trend_mimic notify

    // Boot-replay / seed guard (S102 pattern): indicators + position management
    // run, NEW entries blocked while true.
    bool warmup_active_ = false;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    // ---- state ----
    struct NBar { double h, l, c; };
    std::deque<NBar> hist_;             // prior NATIVE bars (exclude current)
    std::deque<double> closes_;         // recent native closes (impulse lookback)
    double  ema_f_ = 0.0, ema_s_ = 0.0, ema_mid_ = 0.0;
    bool    ema_init_ = false;
    double  atr_ = 0.0, prev_close_ = 0.0; bool atr_init_ = false;
    int     nbars_ = 0;
    double  prev_band_up_ = 0.0, prev_band_dn_ = 0.0, prev_nclose_ = 0.0;
    bool    prev_band_ok_ = false;
    int64_t last_row_ts_ = 0;           // 30m-row dedup (seed/live overlap)
    // native bucket build (H1 from 2x30m; pass-through when tf_secs=1800)
    int64_t cur_bkt_ = 0; double bo_ = 0, bh_ = 0, bl_ = 0, bc_ = 0;

    // position
    bool    pos_active_ = false; int dir_ = 0;
    double  entry_ = 0.0, stop_ = 0.0, extreme_ = 0.0;
    int64_t entry_ts_ = 0; int bars_held_ = 0;
    double  mfe_ = 0.0, mae_ = 0.0;

    double  banked_net_pts_ = 0.0;
    bool    retired_ = false, retired_logged_ = false;

    bool has_open_position() const noexcept { return pos_active_; }

    // ---- restart persistence (wire_cross archetype; extreme_ rides in tp) ----
    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const noexcept {
        if (!pos_active_) return false;
        o.engine = eng; o.symbol = sym; o.side = dir_ > 0 ? "LONG" : "SHORT";
        o.size = lot; o.entry = entry_; o.sl = stop_; o.tp = extreme_;
        o.entry_ts = entry_ts_; o.mfe = mfe_; o.mae = mae_;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) noexcept {
        if (pos_active_) return false;
        pos_active_ = true; dir_ = (ps.side == "SHORT") ? -1 : +1;
        entry_ = ps.entry; stop_ = ps.sl;
        extreme_ = (ps.tp != 0.0) ? ps.tp : ps.entry;   // trail HWM (monotonic; stop persisted)
        entry_ts_ = ps.entry_ts; mfe_ = ps.mfe; mae_ = ps.mae;
        bars_held_ = 0;   // time-stop window restarts across a restart (documented)
        return true;
    }
    void force_close(double bid, double /*ask*/, int64_t now_sec, OnCloseFn cb) noexcept {
        if (!pos_active_) return;
        _close(bid > 0.0 ? bid : entry_, now_sec > 0 ? now_sec : entry_ts_, "FORCE_CLOSE", cb);
    }

    // Warm-seed from ts(sec),o,h,l,c[,v] CSV (data/mgc_30m_hist.csv --
    // or mgc_15m/10m_hist.csv for the fine-row instances; row grain must
    // match row_secs). Entries blocked; ts-dedup skips the live-CSV overlap.
    int seed_from_30m_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) {
            std::printf("[SEED-FATAL] %s: cannot open '%s'\n", engine_tag.c_str(), path.c_str());
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
            if (ts > 4000000000LL) ts /= 1000;
            on_30m_bar(o, h, l, c, (int64_t)ts, OnCloseFn{});
            ++fed;
        }
        warmup_active_ = was;
        std::printf("[SEED] %s %d x%dm replayed native_bars=%d atr=%.3f last_ts=%lld -- hot\n",
                    engine_tag.c_str(), fed, row_secs / 60, nbars_, atr_, (long long)last_row_ts_);
        std::fflush(stdout);
        return fed;
    }

    // Call on each CLOSED 30m MGC bar (poll_mgc_feed). Aggregates the native
    // bucket, runs level-stop checks per row, native logic per bucket close.
    void on_30m_bar(double o, double h, double l, double c, int64_t ts_sec, OnCloseFn cb) {
        if (ts_sec <= last_row_ts_) return;    // seed/live overlap + boot dedup
        last_row_ts_ = ts_sec;

        // (a) intrabar LEVEL stop on this 30m row (gap-honest, adverse-first;
        //     stop_ fixed since the last native close -- harness semantics).
        if (pos_active_) {
            const double fav = dir_ > 0 ? (h - entry_) : (entry_ - l);
            const double adv = dir_ > 0 ? (entry_ - l) : (h - entry_);
            if (fav > mfe_) mfe_ = fav;
            if (adv > mae_) mae_ = adv;
            if (dir_ > 0) {
                if      (o <= stop_) _close(o,     ts_sec, "SL_GAP",  cb);
                else if (l <= stop_) _close(stop_, ts_sec, "SL_ATR",  cb);
            } else {
                if      (o >= stop_) _close(o,     ts_sec, "SL_GAP",  cb);
                else if (h >= stop_) _close(stop_, ts_sec, "SL_ATR",  cb);
            }
        }

        // (b) native bucket build (pass-through at tf_secs=1800)
        const int64_t bkt = (ts_sec / tf_secs) * tf_secs;
        if (cur_bkt_ == 0 || bkt != cur_bkt_) {
            if (cur_bkt_ != 0) _native_close(cur_bkt_, cb);   // gap fallback (missed :30 row)
            cur_bkt_ = bkt; bo_ = o; bh_ = h; bl_ = l; bc_ = c;
        } else {
            if (h > bh_) bh_ = h;
            if (l < bl_) bl_ = l;
            bc_ = c;
        }
        // last sub-bar of the bucket closes it NOW (pass-through when
        // row_secs == tf_secs: every row; H1 from 30m rows: the :30 row)
        if (ts_sec - bkt >= tf_secs - row_secs) { _native_close(bkt, cb); cur_bkt_ = 0; }
    }

private:
    void _native_close(int64_t bkt_ts, OnCloseFn cb) {
        const double h = bh_, l = bl_, c = bc_;
        const int64_t ts = bkt_ts + tf_secs;   // bucket close time

        // indicators (Wilder ATR + incremental EMAs), study form
        if (atr_init_) {
            const double tr = std::fmax(h - l, std::fmax(std::fabs(h - prev_close_), std::fabs(l - prev_close_)));
            atr_ += (tr - atr_) / (double)atr_n;
        } else { atr_ = h - l; atr_init_ = true; }
        prev_close_ = c;
        if (!ema_init_) { ema_f_ = ema_s_ = ema_mid_ = c; ema_init_ = true; }
        else {
            ema_f_   += (c - ema_f_)   * (2.0 / (ema_fast_n + 1));
            ema_s_   += (c - ema_s_)   * (2.0 / (ema_slow_n + 1));
            ema_mid_ += (c - ema_mid_) * (2.0 / 21.0);   // Keltner EMA20 midline
        }
        ++nbars_;

        // manage open position at native close (trail -> reversal/channel -> time)
        if (pos_active_) {
            ++bars_held_;
            if (trail_atr > 0.0) {
                if (dir_ > 0) { if (h > extreme_) extreme_ = h;
                    stop_ = std::fmax(stop_, extreme_ - trail_atr * atr_); }
                else          { if (l < extreme_) extreme_ = l;
                    stop_ = std::fmin(stop_, extreme_ + trail_atr * atr_); }
            }
            if (pos_active_ && mech == Mech::EMA &&
                ((dir_ > 0 && ema_f_ < ema_s_) || (dir_ < 0 && ema_f_ > ema_s_)))
                _close(c, ts, "REVERSAL", cb);
            if (pos_active_ && mech == Mech::DON) {
                if      (dir_ > 0 && c < _chan_low(don_out))  _close(c, ts, "CHAN_EXIT", cb);
                else if (dir_ < 0 && c > _chan_high(don_out)) _close(c, ts, "CHAN_EXIT", cb);
            }
            if (pos_active_ && time_stop_bars > 0 && bars_held_ >= time_stop_bars)
                _close(c, ts, "TIME_STOP", cb);
        }

        // entry signal at native close (harness: same-bar re-entry allowed)
        if (!pos_active_ && enabled && !warmup_active_ && !retired_ &&
            nbars_ >= warm_bars && atr_ > 0.0) {
            int want = 0;
            if (mech == Mech::KELT) {
                const double up = ema_mid_ + kelt_k * atr_, dn = ema_mid_ - kelt_k * atr_;
                if (prev_band_ok_) {
                    if      (c > up && prev_nclose_ <= prev_band_up_) want = +1;
                    else if (c < dn && prev_nclose_ >= prev_band_dn_) want = -1;
                }
                prev_band_up_ = up; prev_band_dn_ = dn; prev_band_ok_ = true;
            } else if (mech == Mech::EMA) {
                if (closes_.size() >= 3) {
                    const double impulse = c - closes_[closes_.size() - 3];
                    if      (ema_f_ > ema_s_ && c > ema_f_ && impulse >=  imp_atr * atr_) want = +1;
                    else if (ema_f_ < ema_s_ && c < ema_f_ && impulse <= -imp_atr * atr_) want = -1;
                }
            } else { // DON
                const double hh = _chan_high(don_in), ll = _chan_low(don_in);
                if      (hh < 1e17 && c > hh) want = +1;
                else if (ll > -1e17 && c < ll) want = -1;
            }
            if (want != 0) {
                const double stop_dist = stop_atr * atr_;
                // cost gate: explicit MGC row ($10/pt/contract, $2.08 RT comm)
                if (::ExecutionCostGuard::is_viable("MGC", mgc_spread_pts, stop_dist, lot, 1.5)) {
                    pos_active_ = true; dir_ = want; entry_ = c;
                    stop_ = c - want * stop_dist; extreme_ = c;
                    entry_ts_ = ts; bars_held_ = 0; mfe_ = 0.0; mae_ = 0.0;
                    std::printf("[%s] ENTRY %s px=%.2f stop=%.2f atr=%.2f%s\n",
                                engine_tag.c_str(), want > 0 ? "LONG" : "SHORT",
                                entry_, stop_, atr_, shadow_mode ? " [SHADOW]" : "");
                    std::fflush(stdout);
                    if (!mimic_tag.empty())
                        omega::gold_trend_mimic().on_trend_open(mimic_tag, dir_, entry_, ts);
                }
            }
        } else if (mech == Mech::KELT) {
            // keep the band series continuous while blocked/positioned
            prev_band_up_ = ema_mid_ + kelt_k * atr_;
            prev_band_dn_ = ema_mid_ - kelt_k * atr_;
            prev_band_ok_ = true;
        }
        prev_nclose_ = c;

        // native-bar history (Donchian channels exclude current) + impulse closes
        hist_.push_back({h, l, c});
        while ((int)hist_.size() > 160) hist_.pop_front();
        closes_.push_back(c);
        while (closes_.size() > 8) closes_.pop_front();

        // mimic native-bar feed (one-way; no-op when tag unset/unregistered)
        if (!mimic_tag.empty())
            omega::gold_trend_mimic().on_bar(mimic_tag, h, l, c, ts);
    }

    double _chan_high(int n) const noexcept {
        if ((int)hist_.size() < n) return 1e18;
        double hh = -1e18;
        for (int k = (int)hist_.size() - n; k < (int)hist_.size(); ++k) hh = std::fmax(hh, hist_[k].h);
        return hh;
    }
    double _chan_low(int n) const noexcept {
        if ((int)hist_.size() < n) return -1e18;
        double ll = 1e18;
        for (int k = (int)hist_.size() - n; k < (int)hist_.size(); ++k) ll = std::fmin(ll, hist_[k].l);
        return ll;
    }

    void _close(double exit_px, int64_t ts_sec, const char* why, OnCloseFn cb) noexcept {
        omega::TradeRecord tr;
        tr.symbol     = "MGC";
        tr.engine     = engine_tag;
        tr.side       = dir_ > 0 ? "LONG" : "SHORT";
        tr.entryPrice = entry_;
        tr.exitPrice  = exit_px;
        tr.size       = lot;
        tr.pnl        = dir_ * (exit_px - entry_) * lot;   // pts*lot; mult downstream
        tr.entryTs    = entry_ts_;
        tr.exitTs     = ts_sec;
        tr.shadow     = shadow_mode;
        tr.mfe        = mfe_; tr.mae = mae_;
        tr.exitReason = why ? why : "";
        pos_active_ = false; dir_ = 0;
        banked_net_pts_ += (tr.pnl / (lot > 0.0 ? lot : 1.0)) - cost_rt_pts;
        const bool was_retired = retired_;
        retired_ = (banked_net_pts_ <= retire_net_pts);
        if (retired_ && !was_retired && !retired_logged_) {
            retired_logged_ = true;
            std::printf("[%s][RETIRED] banked net %.1fpt <= %.1fpt -- new entries latched OFF "
                        "(auto-retirement, -2x BT maxDD)\n",
                        engine_tag.c_str(), banked_net_pts_, retire_net_pts);
            std::fflush(stdout);
        }
        if (!retired_ && was_retired) retired_logged_ = false;
        if (cb) cb(tr);
    }
};

} // namespace omega

#pragma once
//  ADVERSE-PROTECTION: trail-only by design, backtested -- chandelier ATR-trail + structural selloff-low initial stop + MAX_HOLD_BARS(240) time-stop + macro-hostile gold_regime().long_blocked() ENTRY filter (added 2026-06-21); NO cold LOSS_CUT on purpose (2026-06-12 sweep: a velocity/cut gate HURTS gold -- depth, not speed, is the edge). S-2026-06-29 REACTIVATED to SHADOW: the 2026-06-17 cull basis was live ledger net -$205 / MAEp90 $5781 catastrophic falling-knife with "needs an entry filter not an exit" -- that entry filter (the macro long-block) now exists, and faithful BT at correct IBKR cost = bull PF~1.80 both-WF-halves+ / 2022-bear breakeven. Live-size BLOCKED pending a fresh shadow MAE distribution (cull was MAE, not PnL).
//  CADENCE (S-2026-07-05 intrabar-cadence): the chandelier/init stop is now CHECKED per-tick (_manage_intrabar in on_tick), not only at H1 close. Was H1-close-only -> a mid-hour reversal gave back up to an hour (bar-close-blind, same class as the crypto Mimic fix). Faithful BT (backtest/goldpanic_intrabar_vs_h1.cpp, exit-cadence toggle only, real params): intra-bar net +30-44% bull (m5/m30 agree) / +11% bear (less loss), PF up both regimes, maxDD equal-or-better; only cost a ~$13-wider bull worst-trade from gap fills. Chandelier LEVEL (hh - TRAIL_ATR*ATR) still uses the H1 ATR; only the CHECK is per-tick. TIME_STOP + entry logic stay on the H1 boundary. Verdict of the 5-engine cadence audit (outputs/CADENCE_AUDIT_2026-07-05.md): this was the ONLY suspect where intra-bar beat bar-close.
// =============================================================================
// GoldPanicBounceEngine.hpp -- "big reversal day" V-bounce catcher for XAUUSD
// =============================================================================
//
// 2026-06-12 SESSION DESIGN (Claude / Jo):
//   Long-only capitulation-bounce engine. Same family as the validated
//   CapitulationEngine (Lance Breitstein V-reversal, PF 1.82 on US equities)
//   and the Peachy long-only bounce edge -- but on gold, where the backtest
//   showed a ROBUST edge (the index version did NOT survive cross-instrument /
//   bear-regime checks and was NOT built).
//
//   ALWAYS-ON MONITOR: every H1 bar the engine recomputes the rolling
//   drawdown depth = (trailing DD_LOOKBACK-bar high - current low) / ATR.
//   This *is* the "watch the trading day, activate on drops" piece -- it is
//   intrinsic, runs on every bar with no external feed, and graceful by
//   construction (no signal until the buffer is warm + a real selloff prints).
//
//   TRIGGER (long): price DEEP in a selloff (depth >= DROP_K ATR) AND a TURN
//   prints -- a bullish H1 bar (close>open) that reclaims the prior bar's
//   high, after a red prior bar. Enter long at the turn close.
//
//   EXIT: aggressive chandelier ATR-trail (highest_high - TRAIL_ATR*ATR),
//   NO fixed TP -- ride the V. Initial stop = structural selloff low. This is
//   the "trade aggressively using trailing" the operator asked for, and the
//   no-TP wide-trail runner is the exit profile validated across the book
//   (FVG continuation, XauVolBreakout BE-off).
//
//   BACKTEST (panic_bounce_bt.cpp, H1, cost-incl 0.37pt RT):
//     XAU 24-26  drop8/lb250/tr4.5: PF 1.97 net +967pt n=113 WR28%
//       walk-forward BOTH halves +  (H1 PF1.82 / H2 PF2.01)
//       2022 bear OOS PF 1.08 (+24pt) -- survives a real bear tape
//     ROBUST RIDGE: drop{4,6,8}/lb250/tr4.5 all both-halves+ AND bear+.
//     Velocity/range-expansion gate HURTS gold -> depth, not speed, is the edge.
//   CAVEAT: low-win/fat-tail (WR ~28%, winners 5x losers). Shadow first.
//
// LOG NAMESPACE: [GPB]. tr.engine="GoldPanicBounce". tr.regime="PANIC_BOUNCE".
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <deque>
#include <functional>
#include <fstream>
#include <string>
#include "OmegaCostGuard.hpp"
#include "OmegaTradeLedger.hpp"
#include "OpenPositionRegistry.hpp"  // omega::PositionSnapshot (persist; S-2026-07-11)
#include "RegimeState.hpp"       // 2026-06-21: macro-hostile long-block (BearProtect coverage)

namespace omega {

class GoldPanicBounceEngine {
public:
    static constexpr int BAR_SECS = 3600;          // H1 decision bars

    // --- params (baked to the robust backtest cfg; tunable) ---
    int    ATR_N        = 24;     // ATR window (H1 bars ~ 1 day)
    int    DD_LOOKBACK  = 250;    // trailing-high window for drawdown monitor
    double DROP_K       = 8.0;    // selloff depth threshold (ATR) to arm
    double TRAIL_ATR    = 4.5;    // chandelier trail width (ATR) -- aggressive ride
    double SL_ATR       = 2.0;    // fallback initial-stop distance if struct invalid
    int    MAX_HOLD_BARS= 240;    // time stop (~10 trading days)
    int    COOLDOWN_BARS= 24;     // bars to wait after an exit

    // --- EMA200-slope regime gate (S-2026-06-30, sweepable, default OFF) ---
    // A capitulation dip-buyer fires BELOW a rising EMA, so volbrk's "price >
    // EMA200" gate would zero every entry. Instead gate on the EMA200 SLOPE:
    // permit dip-buys while the long trend is flat/rising (bull/chop), block
    // them when EMA200 is falling (confirmed bear) -> kills falling-knife bleed
    // without neutering the V-reversal edge. TREND_GATE=false reproduces the
    // pre-gate deploy exactly (no behaviour change until a sweep proves it).
    bool   TREND_GATE     = false;
    int    TREND_EMA_N    = 200;   // EMA period (H1 bars)
    int    TREND_SLOPE_LB = 200;   // bars back to measure the slope
    double TREND_SLOPE_MIN= 0.0;   // block if (ema_now-ema_lb)/ema_lb < this

    double COST_COVER_PTS = 0.40; // RT cost cover (IBKR gold ~0.37)
    static constexpr double USD_PER_PT_LOT = 100.0;
    static constexpr double RISK_DOLLARS   = 50.0;
    static constexpr double LOT_MIN = 0.01, LOT_MAX = 0.05;
    static constexpr double ATR_FLOOR = 3.0, ATR_CAP = 80.0, SPREAD_CAP = 0.80;

    bool shadow_mode = true;
    bool enabled     = true;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;
    CloseCallback on_close_cb;

    struct LivePos {
        bool    active = false;
        double  entry = 0.0, init_stop = 0.0, hh = 0.0, atr_at_entry = 0.0;
        double  size = 0.0, mae = 0.0;
        int64_t entry_ts = 0, entry_bar_seq = 0;
    } m_pos;

    bool has_open_position() const noexcept { return m_pos.active; }

    // ---- restart persistence (wire_cross archetype, S-2026-07-11 PHASE 1b) ----
    // Found by re-enabling the persistence audit (it sat behind an unreachable
    // exit in the canary): this engine displayed positions but never persisted
    // -- a restart orphaned the open V-bounce leg. Snapshot carries the
    // chandelier high-water mark hh in the (unused) tp field; atr_at_entry is
    // only the ATR fallback (atr_ rewarns from the H1 seed) and is
    // reconstructed from the structural stop distance.
    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const noexcept {
        if (!m_pos.active) return false;
        o.engine = eng; o.symbol = sym; o.side = "LONG"; o.size = m_pos.size;
        o.entry = m_pos.entry; o.sl = m_pos.init_stop;
        o.tp = m_pos.hh;                       // NOT a take-profit (engine has none)
        o.entry_ts = m_pos.entry_ts / 1000;    // engine keeps ms; snapshot is seconds
        o.mae = m_pos.mae;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) noexcept {
        if (m_pos.active) return false;        // adopt won't double an open slot
        m_pos = LivePos{};
        m_pos.active = true;
        m_pos.entry = ps.entry; m_pos.init_stop = ps.sl;
        m_pos.hh = (ps.tp > 0.0) ? ps.tp : ps.entry;
        m_pos.atr_at_entry = SL_ATR > 0.0 ? std::max(0.0, (ps.entry - ps.sl) / SL_ATR) : 0.0;
        m_pos.size = ps.size; m_pos.mae = ps.mae;
        m_pos.entry_ts = ps.entry_ts * 1000;   // snapshot seconds -> engine ms
        // time-stop clock: reconstruct elapsed H1 bars from wall time so the
        // 240-bar MAX_HOLD doesn't restart from zero on every reboot.
        const int64_t elapsed = ps.entry_ts > 0
            ? std::max<int64_t>((int64_t)0, ((int64_t)std::time(nullptr) - ps.entry_ts) / BAR_SECS) : 0;
        m_pos.entry_bar_seq = bar_seq_ > elapsed ? bar_seq_ - elapsed : 0;
        return true;
    }

    // ---- public seed (warm-restart mandate, pattern 2) ----
    // Replay an H1 CSV (ts,o,h,l,c) to fill the bar buffers without firing.
    void seed_from_h1_csv(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[GPB][SEED] MISS %s\n", path.c_str()); return; }
        bool was = enabled; enabled = false;     // no entries on historical bars
        std::string line; long n = 0;
        while (std::getline(f, line)) {
            if (line.empty() || (!std::isdigit((unsigned char)line[0]))) continue;
            char* p = (char*)line.c_str(); char* e;
            int64_t ts = std::strtoll(p,&e,10); if(*e!=',')continue; p=e+1;
            double o=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
            double h=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
            double l=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
            double c=std::strtod(p,&e);
            if(o>0&&h>0&&l>0&&c>0){ _push_bar(ts*1000,o,h,l,c); ++n; }
        }
        enabled = was;
        std::printf("[GPB][SEED] %ld H1 bars from %s (buf=%zu)\n", n, path.c_str(), c_.size());
    }

    // ---- tick entry (lightweight house convention; closes via on_close_cb) ----
    void on_tick(double bid, double ask, int64_t now_ms)
    {
        if (!enabled) return;
        const double mid = (bid + ask) * 0.5;
        const double spread = ask - bid;
        _accumulate(mid, now_ms);

        // S-2026-07-05 intra-bar exit: check the chandelier/init stop against the
        // LIVE tick price EVERY tick, not just at H1 close. Ratchets hh with the
        // live price and exits the instant the stop is touched intra-hour. Level
        // (chandelier = hh - TRAIL_ATR*ATR) uses the last-closed-bar H1 ATR; only
        // the CHECK is per-tick. TIME_STOP + entry decisions stay on the H1 bar
        // boundary below. Faithful-BT-backed (see header CADENCE note).
        if (m_pos.active) _manage_intrabar(mid);

        const int64_t bar = now_ms / 1000 / BAR_SECS;
        if (acc_.n > 0 && bar != acc_bar_) {
            _close_bar();                            // finalize the H1 bar
            _on_bar_close(spread, /*can_enter=*/true, nullptr);  // monitor + decide
            acc_bar_ = bar;
        }
    }

private:
    // bar buffers
    std::deque<double> o_, h_, l_, c_, atr_;
    std::deque<int64_t> ts_;
    struct Acc { double o=0,h=0,l=0,c=0; int64_t ts=0; int n=0; } acc_;
    int64_t acc_bar_ = -1;
    int64_t bar_seq_ = 0;
    int64_t last_exit_seq_ = -100000;

    void _accumulate(double mid, int64_t now_ms) {
        const int64_t bar = now_ms / 1000 / BAR_SECS;
        if (acc_.n == 0 || bar == acc_bar_into()) {
            if (acc_.n == 0) { acc_.o=acc_.h=acc_.l=acc_.c=mid; acc_.ts=now_ms; }
            else { if(mid>acc_.h)acc_.h=mid; if(mid<acc_.l)acc_.l=mid; acc_.c=mid; }
            acc_.n++;
        }
    }
    int64_t acc_bar_into() const { return acc_.ts/1000/BAR_SECS; }

    void _close_bar() {
        _push_bar(acc_.ts, acc_.o, acc_.h, acc_.l, acc_.c);
        acc_ = Acc{};
    }

    void _push_bar(int64_t ts, double o, double h, double l, double c) {
        // true range vs prior close
        double tr = h - l;
        if (!c_.empty()) tr = std::max(tr, std::max(std::fabs(h-c_.back()), std::fabs(l-c_.back())));
        // EMA200 (regime-slope gate input). Maintained always; only consulted
        // when TREND_GATE is on. Seeded by warm-restart replay like the bars.
        if (!ema_t_init_) { ema_t_ = c; ema_t_init_ = true; }
        else { const double a = 2.0 / (TREND_EMA_N + 1); ema_t_ = a * c + (1.0 - a) * ema_t_; }
        ema_hist_.push_back(ema_t_);
        while ((int)ema_hist_.size() > TREND_SLOPE_LB + 8) ema_hist_.pop_front();
        o_.push_back(o); h_.push_back(h); l_.push_back(l); c_.push_back(c); ts_.push_back(ts);
        // rolling-mean ATR
        tr_.push_back(tr); tr_sum_ += tr;
        if ((int)tr_.size() > ATR_N) { tr_sum_ -= tr_.front(); tr_.pop_front(); }
        atr_.push_back(tr_.empty()?0.0:tr_sum_/(double)tr_.size());
        const size_t cap = (size_t)DD_LOOKBACK + 8;
        while (o_.size() > cap) { o_.pop_front();h_.pop_front();l_.pop_front();c_.pop_front();ts_.pop_front();atr_.pop_front(); }
        ++bar_seq_;
    }
    std::deque<double> tr_; double tr_sum_ = 0.0;
    double ema_t_ = 0.0; bool ema_t_init_ = false;
    std::deque<double> ema_hist_;

    void _on_bar_close(double spread, bool can_enter, const CloseCallback* ext_close) {
        const int N = (int)c_.size();
        if (N < DD_LOOKBACK + 2) return;
        const double A = atr_.back();
        if (A < ATR_FLOOR || A > ATR_CAP) { if(m_pos.active) _manage(spread,ext_close); return; }

        if (m_pos.active) { _manage(spread, ext_close); return; }

        // cooldown
        if (bar_seq_ - last_exit_seq_ < COOLDOWN_BARS) return;
        if (!can_enter || spread > SPREAD_CAP) return;

        // 2026-06-21: macro-hostile de-risk. Long-only capitulation buyer -> flat
        // when real-yields-rip macro is hostile (MacroGoldGate). Fail-safe (false)
        // when the gate feed is missing/stale, so this can only ADD protection.
        if (omega::gold_regime().long_blocked()) return;

        // EMA200-slope regime gate (default OFF). Block the dip-buy when the
        // long trend is falling (confirmed bear) -- backtest-driven bear
        // protection that, unlike a price-position gate, still permits the
        // V-reversal in bull/chop. See param block above.
        if (TREND_GATE && (int)ema_hist_.size() > TREND_SLOPE_LB) {
            const double e_now = ema_t_;
            const double e_lb  = ema_hist_[ema_hist_.size() - 1 - TREND_SLOPE_LB];
            if (e_lb > 0.0 && (e_now - e_lb) / e_lb < TREND_SLOPE_MIN) return;
        }

        // ---- MONITOR: rolling drawdown depth in ATR ----
        double peakH = 0.0;
        for (int k = N-1-DD_LOOKBACK; k < N; ++k) if (h_[k] > peakH) peakH = h_[k];
        const double depth = (peakH - l_[N-1]) / A;
        if (depth < DROP_K) return;                         // not a big enough selloff

        // ---- TURN: bullish bar reclaims prior high, after a red bar ----
        const bool turn = c_[N-1] > o_[N-1] && h_[N-1] > h_[N-2] && c_[N-2] < o_[N-2];
        if (!turn) return;

        // ---- size from structural stop ----
        const double entry = c_[N-1];
        double swing_lo = std::min(l_[N-1], std::min(l_[N-2], l_[N-3]));
        double init_stop = swing_lo < entry ? swing_lo : (entry - SL_ATR * A);
        const double stop_dist = entry - init_stop;
        double size = (stop_dist > 1e-6) ? (RISK_DOLLARS / (stop_dist * USD_PER_PT_LOT)) : LOT_MIN;
        size = std::clamp(size, LOT_MIN, LOT_MAX);

        // ---- COST GATE (entry filter): first chandelier-trail leg must clear cost ----
        if (!ExecutionCostGuard::is_viable("XAUUSD", spread, TRAIL_ATR * A, size, 1.2)) return;

        // ---- ENTER LONG ----

        m_pos = LivePos{};
        m_pos.active = true; m_pos.entry = entry; m_pos.init_stop = init_stop;
        m_pos.hh = h_[N-1]; m_pos.atr_at_entry = A; m_pos.size = size;
        m_pos.entry_ts = ts_.back(); m_pos.entry_bar_seq = bar_seq_;

        std::printf("[GPB] %s LONG XAUUSD depth=%.1fATR entry=%.2f stop=%.2f trail=%.1fxATR sz=%.2f\n",
                    shadow_mode?"SHADOW":"LIVE", depth, entry, init_stop, TRAIL_ATR, size);
        std::fflush(stdout);
    }

    void _manage(double /*spread*/, const CloseCallback* ext_close) {
        const int N = (int)c_.size();
        const double A = atr_.back() > 0 ? atr_.back() : m_pos.atr_at_entry;
        const double curH = h_[N-1], curL = l_[N-1], curC = c_[N-1];
        if (curH > m_pos.hh) m_pos.hh = curH;
        const double adverse = m_pos.entry - curL; if (adverse > m_pos.mae) m_pos.mae = adverse;

        const double trail_stop = m_pos.hh - TRAIL_ATR * A;
        const double eff = std::max(m_pos.init_stop, trail_stop);

        double exit_px = 0.0; const char* why = nullptr;
        if (curL <= eff)                                     { exit_px = eff;  why = (eff>m_pos.init_stop?"TRAIL_HIT":"SL_HIT"); }
        else if (bar_seq_ - m_pos.entry_bar_seq >= MAX_HOLD_BARS) { exit_px = curC; why = "TIME_STOP"; }
        if (!why) return;
        _emit_close(exit_px, why, ext_close);
    }

    // S-2026-07-05 (intrabar-cadence): per-tick chandelier/init-stop check. Runs
    // from on_tick every tick while a position is open, so a mid-hour reversal
    // exits the instant the stop is touched instead of at the next H1 close.
    // ATR is the last completed-bar value (an H1 quantity, held constant intra-
    // bar -- matches the faithful BT in backtest/goldpanic_intrabar_vs_h1.cpp).
    // TIME_STOP stays on the bar boundary in _manage (a timeout, not a reversal).
    void _manage_intrabar(double px) {
        if (c_.empty()) return;
        const double A = atr_.back() > 0 ? atr_.back() : m_pos.atr_at_entry;
        if (A <= 0.0) return;
        if (px > m_pos.hh) m_pos.hh = px;
        const double adverse = m_pos.entry - px; if (adverse > m_pos.mae) m_pos.mae = adverse;
        const double trail_stop = m_pos.hh - TRAIL_ATR * A;
        const double eff = std::max(m_pos.init_stop, trail_stop);
        if (px <= eff)
            _emit_close(eff, (eff > m_pos.init_stop ? "TRAIL_HIT" : "SL_HIT"), nullptr);
    }

    // shared close-out: writes the TradeRecord + fires callbacks + clears pos.
    // Called by both the bar-close _manage (TIME_STOP / backstop) and the
    // per-tick _manage_intrabar (chandelier/init stop touched intra-hour).
    void _emit_close(double exit_px, const char* why, const CloseCallback* ext_close) {
        const double pnl_pts = (exit_px - m_pos.entry) - COST_COVER_PTS;
        omega::TradeRecord tr{};
        tr.engine = "GoldPanicBounce"; tr.regime = "PANIC_BOUNCE"; tr.symbol = "XAUUSD";
        tr.side = "LONG"; tr.entryPrice = m_pos.entry; tr.exitPrice = exit_px; tr.size = m_pos.size;
        tr.pnl = pnl_pts * m_pos.size;            // RAW pts*lot; ledger applies USD_PER_PT (gotcha memory)
        tr.mae = m_pos.mae; tr.mfe = (m_pos.hh > m_pos.entry ? m_pos.hh - m_pos.entry : 0.0); tr.atr_at_entry = m_pos.atr_at_entry; tr.shadow = shadow_mode;
        tr.exitReason = why; tr.entryTs = m_pos.entry_ts/1000; tr.exitTs = ts_.back()/1000;
        if (on_close_cb) on_close_cb(tr);
        if (ext_close && *ext_close) (*ext_close)(tr);

        std::printf("[GPB] EXIT %s XAUUSD %s entry=%.2f exit=%.2f pnl=%.2fpt hold=%lldbars\n",
                    shadow_mode?"SHADOW":"LIVE", why, m_pos.entry, exit_px, pnl_pts,
                    (long long)(bar_seq_ - m_pos.entry_bar_seq));
        std::fflush(stdout);
        m_pos = LivePos{}; last_exit_seq_ = bar_seq_;
    }
};

} // namespace omega

// CryptoBearRecoveryEngine.hpp -- long-only BTC/ETH bear-recovery bounce engine
// (S-2026-07-03, operator: "trade the bounces in the current downtrend, max
//  protection, max aggression, without impacting the edges if found" +
//  "no python in Omega -- ensure it is C++").
//
// THE REGIME LADDER (daily closes, completed UTC bars only) -- full study:
// backtest/crypto_bear_bounce/FINDINGS.md (Coinbase 1h 2017-2026, 3 full bears):
//   KNIFE     close < SMA200 and NOT (close > SMA50 and SMA50 rising)
//             -> FLAT. Seven long-only entry families backtested; ALL lose after
//                costs in the 2018/2022/2025-26 knife phases. Flat IS the edge.
//   RECOVERY  close < SMA200 AND close > SMA50 AND SMA50 > SMA50[5d ago]
//             -> THIS ENGINE: LONG on daily close crossing above EMA9 after
//                >= below_min closes below; ride to first daily close < EMA9.
//   BULL      close > SMA200 -> the Luke daily system owns this phase.
//
// ADVERSE-PROTECTION: backtested verdict (FINDINGS.md sect.4-5, 2026-07-03) --
//   hard stop = entry-day low - 0.5*ATR14(D), checked intrabar on every price;
//   BE-AND-RIDE floor: after MFE >= +2% (BE_ARM), floor = entry (BE_LOCK 0) --
//     net UP ($22.8k->$23.4k @1% risk), PF 5.16->8.85, worst -8.3%->-4.8%;
//   COLD CUT: no-op at every value 3-12% (EMA9-flip already bounds never-green
//     trades) -- OFF by design;
//   GIVEBACK clips 30/50/70%: net -62%..-90% -- PROVEN HARMFUL, must not be added.
//   Sizing: risk-based off the structural stop; 2% ship tier / 3% max-aggressive
//   (PF holds >= 4.2 up to 5%). ~2 signals/yr: aggression comes from SIZE, not
//   frequency. Walk-forward halves both +, both symbols +, PF 4.5 @3x costs.
//
// COST GATE NOTE: crypto executes via IBKR MBT/MET micro-futures (measured
// 2-8 bps ladder) / PAXOS spot -- OmegaCostGuard's CFD cost table is not
// applicable to this venue (same exemption class as PumpScalpEngine). Costs are
// baked into the faithful BT (6 bps/side + 10 bps stop slip, stress x2/x3) and
// into the live book's real fills; there is no TP to pre-check viability against
// (trend exit), so the entry-time cost gate is structurally n/a here.
//
// WIRING: crypto prices do NOT flow through Omega.exe's FIX tick path today.
// This engine is driven by (a) the faithful backtest
// (backtest/crypto_bear_bounce/faithful_bear_recovery_bt.cpp), and (b) the
// signal CLI (tools/crypto_bear_recovery.cpp) consumed by the ~/Crypto book /
// omega_crypto_bridge.py (paper 4002). Closed trades reach the Omega ledger via
// the existing CryptoLedgerInbound.hpp path (engine tag "IBKRCrypto:<sym>:BRCV").
//
// WARM-SEED (mandate): SMA200 needs 200 completed daily bars -> cold start is
// ~200 days. seed_from_daily_csv() replays a ts,o,h,l,c daily CSV (ts in sec or
// ms) straight into the indicator state with entries disabled; bundled warmups:
// phase1/signal_discovery/warmup_BTCUSD_D1.csv / warmup_ETHUSD_D1.csv.
// Boot logs MUST show one [SEED] line per instance.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace omega {

struct CbrClose {                 // emitted on every closed trade
    std::string symbol;
    double      entry = 0, exit = 0, qty = 0;
    long long   entry_ts_ms = 0, exit_ts_ms = 0;
    const char* reason = "";      // STOP | BE_FLOOR | EMA9_FLIP
    double      mfe = 0;          // peak favorable move (fraction)
};

struct CryptoBearRecoveryEngine {
    // ---- config (all values are the backtested champion; see header) ----
    bool        enabled     = true;
    std::string symbol      = "BTCUSD";
    int         below_min   = 3;      // consecutive closes below EMA9 required before the reclaim
    double      stop_atr    = 0.5;    // stop = entry-day low - stop_atr * ATR14(D)
    double      be_arm      = 0.02;   // BE-and-ride floor arms at +2% MFE
    double      be_lock     = 0.0;    // floor = entry * (1 + be_lock)
    double      risk_pct    = 0.02;   // 2% ship tier; 3% max-aggressive
    double      max_pos_pct = 0.5;    // notional cap as fraction of equity_ref
    double      equity_ref  = 100000.0;
    std::function<void(const CbrClose&)> on_close;   // trade sink (ledger/CSV)

    // ---- position ----
    struct Pos {
        bool   active = false;
        double entry = 0, qty = 0, stop = 0, floor = -1.0, mfe = 0;
        long long t_in_ms = 0;
    } pos_;

    // ---- ladder state of the LAST COMPLETED daily bar (for the CLI / GUI) ----
    struct State {
        const char* regime = "WARMUP";   // WARMUP | KNIFE | RECOVERY | BULL
        bool   enter_signal = false;     // reclaim cross fired on the completed bar
        bool   exit_flag    = false;     // completed close < EMA9
        double close = 0, ema9 = 0, sma50 = 0, sma200 = 0;
        double stop = 0, qty_usd = 0;    // populated when enter_signal
        int    below_prev = 0;
        long long bar_ts_ms = 0;
    };
    State state() const { return st_; }

    // =====================================================================
    // Seeding -- daily CSV ts,o,h,l,c (header line optional; ts in s or ms).
    // Entries stay disabled during seeding; only indicator state warms up.
    // =====================================================================
    bool seed_from_daily_csv(const char* path) {
        FILE* f = std::fopen(path, "rb");
        if (!f) { std::printf("[SEED] CryptoBearRecovery %s: MISSING %s\n", symbol.c_str(), path); return false; }
        char line[512]; int n = 0;
        while (std::fgets(line, sizeof line, f)) {
            double ts, o, h, l, c;
            if (std::sscanf(line, "%lf,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c) != 5) continue;
            long long ts_ms = ts > 1e12 ? (long long)ts : (long long)(ts * 1000.0);
            push_completed_daily(o, h, l, c, ts_ms, /*live=*/false);
            ++n;
        }
        std::fclose(f);
        std::printf("[SEED] CryptoBearRecovery %s: %d daily bars from %s (regime=%s)\n",
                    symbol.c_str(), n, path, st_.regime);
        std::fflush(stdout);
        return n >= 206;   // SMA200 + 5-day slope lag + reclaim context
    }

    // Seed one completed daily bar directly (ascending order; entries stay off).
    void seed_bar(double o, double h, double l, double c, long long ts_ms) {
        push_completed_daily(o, h, l, c, ts_ms, /*live=*/false);
    }

    // =====================================================================
    // Price feed. Call with every price observation (tick mid / bar o,h,l,c
    // in sequence). Aggregates UTC daily bars internally, manages the open
    // position intrabar, and fills pending entry/exit at the next price
    // (= faithful next-open semantics of the backtest).
    // =====================================================================
    void on_price(double px, long long ts_ms) {
        if (px <= 0) return;
        const long long day = ts_ms / 86400000LL;
        if (have_cur_ && day != cur_day_) {              // daily rollover
            push_completed_daily(cur_o_, cur_h_, cur_l_, cur_c_, cur_day_ * 86400000LL, /*live=*/true);
            have_cur_ = false;
        }
        if (!have_cur_) { cur_day_ = day; cur_o_ = cur_h_ = cur_l_ = px; have_cur_ = true; }
        cur_h_ = px > cur_h_ ? px : cur_h_;
        cur_l_ = px < cur_l_ ? px : cur_l_;
        cur_c_ = px;

        // ---- pending fills (set at rollover, filled at this price) ----
        if (pending_exit_ && pos_.active) { close_pos(px, ts_ms, "EMA9_FLIP"); pending_exit_ = false; }
        if (pending_entry_ && !pos_.active && enabled) {
            pending_entry_ = false;
            if (pend_stop_ < px) {
                double riskps = px - pend_stop_;
                double qty = equity_ref * risk_pct / riskps;
                double cap = equity_ref * max_pos_pct / px;
                pos_.active = true; pos_.entry = px; pos_.qty = qty < cap ? qty : cap;
                pos_.stop = pend_stop_; pos_.floor = -1.0; pos_.mfe = 0; pos_.t_in_ms = ts_ms;
            }
        } else if (pending_entry_) {
            pending_entry_ = false;
        }

        // ---- in-flight management (every price = intrabar marks) ----
        if (pos_.active) {
            const double fav = px / pos_.entry - 1.0;
            if (fav > pos_.mfe) pos_.mfe = fav;
            if (pos_.floor < 0 && be_arm > 0 && pos_.mfe >= be_arm)
                pos_.floor = pos_.entry * (1.0 + be_lock);            // BE-and-ride
            if (px <= pos_.stop)                    close_pos(pos_.stop, ts_ms, "STOP");
            else if (pos_.floor > 0 && px <= pos_.floor)
                                                    close_pos(pos_.floor, ts_ms, "BE_FLOOR");
        }
    }

private:
    // ---- daily indicator state ----
    std::deque<double> cl_, hi_, lo_;    // completed daily bars (capped)
    std::deque<double> sma50_hist_;      // last 6 SMA50 values (for the 5-day slope)
    double ema9_ = 0;  bool ema9_init_ = false;
    int    below9_prev_ = 0;             // consecutive closes below EMA9 up to PREVIOUS bar
    double prev_close_ = 0;
    State  st_;

    // ---- live daily aggregation ----
    bool have_cur_ = false; long long cur_day_ = 0;
    double cur_o_ = 0, cur_h_ = 0, cur_l_ = 0, cur_c_ = 0;

    // ---- pending next-open actions ----
    bool pending_entry_ = false, pending_exit_ = false;
    double pend_stop_ = 0;

    static double mean_last(const std::deque<double>& q, size_t w) {
        double s = 0; for (size_t i = q.size() - w; i < q.size(); ++i) s += q[i];
        return s / (double)w;
    }

    double atr14() const {
        const size_t n = cl_.size();
        if (n < 15) return 0;
        double s = 0;
        for (size_t i = n - 14; i < n; ++i) {
            double tr = hi_[i] - lo_[i];
            const double pc = cl_[i - 1];
            if (hi_[i] - pc > tr) tr = hi_[i] - pc;
            if (pc - lo_[i] > tr) tr = pc - lo_[i];
            s += tr;
        }
        return s / 14.0;
    }

    void close_pos(double px, long long ts_ms, const char* reason) {
        CbrClose t;
        t.symbol = symbol; t.entry = pos_.entry; t.exit = px; t.qty = pos_.qty;
        t.entry_ts_ms = pos_.t_in_ms; t.exit_ts_ms = ts_ms; t.reason = reason; t.mfe = pos_.mfe;
        pos_.active = false;
        if (on_close) on_close(t);
    }

    // one COMPLETED daily bar: update indicators, then (live only) evaluate
    // the ladder and schedule entry/exit for the next price.
    void push_completed_daily(double o, double h, double l, double c,
                              long long bar_ts_ms, bool live) {
        (void)o;
        const int below_before = below9_prev_;                 // up to bar N-1
        cl_.push_back(c); hi_.push_back(h); lo_.push_back(l);
        while (cl_.size() > 260) { cl_.pop_front(); hi_.pop_front(); lo_.pop_front(); }
        const double k = 2.0 / 10.0;                           // EMA9
        ema9_ = ema9_init_ ? c * k + ema9_ * (1.0 - k) : c;
        ema9_init_ = true;
        below9_prev_ = (c < ema9_) ? below_before + 1 : 0;     // now: up to bar N
        prev_close_ = c;

        st_ = State{};
        st_.bar_ts_ms = bar_ts_ms; st_.close = c; st_.ema9 = ema9_;
        st_.below_prev = below_before;
        if (cl_.size() < 205) { st_.regime = "WARMUP"; return; }

        const double sma200 = mean_last(cl_, 200);
        const double sma50  = mean_last(cl_, 50);
        sma50_hist_.push_back(sma50);
        while (sma50_hist_.size() > 6) sma50_hist_.pop_front();
        st_.sma50 = sma50; st_.sma200 = sma200;

        const bool bear = c < sma200;
        const bool slope_ok = sma50_hist_.size() >= 6 && sma50 > sma50_hist_.front();
        const bool recovery = bear && c > sma50 && slope_ok;
        st_.regime = !bear ? "BULL" : (recovery ? "RECOVERY" : "KNIFE");
        st_.exit_flag = c < ema9_;

        // reclaim cross on THIS completed bar (below-count is up to bar N-1)
        const bool crossed = c > ema9_ && below_before >= below_min;
        if (recovery && crossed) {
            const double a = atr14();
            const double stop = l - stop_atr * a;
            if (stop > 0 && stop < c) {
                st_.enter_signal = true; st_.stop = stop;
                st_.qty_usd = equity_ref * risk_pct / (c - stop) * c;
                const double cap_usd = equity_ref * max_pos_pct;
                if (st_.qty_usd > cap_usd) st_.qty_usd = cap_usd;
            }
        }

        if (!live) return;                    // seeding: indicators only
        if (pos_.active && st_.exit_flag) pending_exit_ = true;
        if (!pos_.active && st_.enter_signal) { pending_entry_ = true; pend_stop_ = st_.stop; }
    }
};

} // namespace omega

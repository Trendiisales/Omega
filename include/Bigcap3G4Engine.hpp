#pragma once
// Bigcap3G4Engine.hpp — +3%-day-mover multi-day hold on the BIGCAP-45, regime-
// gated G4 (SPY>200SMA AND SPY 20d realized vol < 20%) with the VS vol-shorten-
// hold lever (S-2026-07-23, BULLGATE_PROTECTION_SWEEPS build-queue item 6:
// "3% G4+VS — NEW engine (DualMom-pattern wire)").
//
// CERT (backtest/BULLGATE_PROTECTION_SWEEPS_2026-07-23.md §A "3% multi-day";
// published base harness = backtest/bigcap_3pct_regime_gate_bt.py; cert cell run
// in the S-2026-07-23 scratchpad sweep3pct.py + refine_both.py vth grid):
//   same_close h3 + VS(hold->1 when SPY 20d-vol>median) + gate SPY>200DMA AND
//   vol<20%: n=7878 (~656/yr) PF 1.29 mDD 545 (summed per-trade %) MAR 8.3,
//   2022 traded n=148 +23.5% PF 1.10, WF+/+, 2x +3749, ex-RGTI PASS, 10bp RT.
//   Caveat: the 2022 pass trades ~9% of bear signals, Jan-heavy — the gate earns
//   its keep by REFUSAL.
//
// MECHANISM — replicates the harness EXACTLY (line refs: sweep3pct.py = the S-23
// cert harness; matching shapes in backtest/bigcap_3pct_regime_gate_bt.py noted):
//   SIGNAL/ENTRY (sweep3pct.py L80-88 signals(), same_close branch
//   [bigcap_3pct_regime_gate_bt.py L102-111]): name's daily close >= prev close
//     * (1 + thr_pct/100) -> LONG at THAT close (e_idx=t, entry_px=c), cfg.lot
//     shares — IF the gate passes at the signal date. Signals on consecutive days
//     are INDEPENDENT overlapping trades (vector book, not per-symbol map).
//   GATE G4 (refine_both.py L17-26 eval_gv generalizing sweep3pct.py L95 g4;
//   SPY features sweep3pct.py L30-41 [bigcap_3pct_regime_gate_bt.py L42-79]):
//     SPY close > 200-day SMA  AND  SPY 20d realized vol < vol_gate_pct, both at
//     the SPY row on-or-before the signal date. Realized vol = POPULATION stdev
//     of the last 20 daily log returns (21 closes) * sqrt(252). Fail-CLOSED:
//     insufficient SPY history = no entries (DualMom gate_on() discipline).
//   VS hold-shorten (sweep3pct.py L99 hivol + run_exits L112 hh):
//     hold target = 1 row when SPY 20d vol > vol_median_pct at the SIGNAL date,
//     else hold_d rows. Decided AT ENTRY, fixed for the trade's life.
//     vol_median_pct default 11.8 = the harness's FULL-SAMPLE median (RVOL_MED,
//     sweep3pct.py L40-41) — live should periodically re-derive it from the
//     rolling SPY history (a frozen constant slowly drifts from the harness's
//     expanding-sample median; flag on any re-cert).
//   EXIT (run_exits sweep3pct.py L106-129 with trail=0, lc=0, be=0 — certified
//   time-stop, NO trail, NO loss-cut):
//     exit at the CLOSE of the hold-target-th name-bar after entry
//     (last = min(e_idx+hh, n-1), j==last -> exit_px=cj, L129). h3 -> close of
//     the 3rd row after entry; VS -> close of the next row.
//
// ADVERSE-PROTECTION: time-stop h3 + vol-shorten-hold certified; LC not
//   load-bearing under G4+VS (swept 5/8/12% — sweep3pct.py PROTS grid; inert on
//   daily bars, gap-through); gate REFUSAL is the protection (2022: gate admits
//   ~9% of bear signals, traded slice +23.5% PF 1.10).
//
// COST GATE: ExecutionCostGuard::is_viable injected via gate_fn_ before every
//   live order. TP proxy = px*0.0043 (the certified MEAN per-trade edge, +43bp)
//   — deliberately honest-thin so $1-2/order commission minimums at 1-share size
//   FAIL the gate rather than get flattered by a 2% proxy.
//
// NOTE — high trade count (~656/yr): real execution at 1 share has a thin
//   per-trade edge (+43bp mean) vs IBKR $1-2/order minimums; the edge does NOT
//   survive minimum commissions at proving size. PENDING OPERATOR SIZE/VENUE
//   DECISION — live_book stays false (book-only shadow of record) until a
//   post-commission cert at the operator's chosen size/venue exists.
//
// FEED: per-name daily closes (poller-driven, sdt cadence; each name-row
//   delivered ONCE — DualMom contract); SPY closes via push_spy_close (same feed
//   as DualMom/BigCapHi52, data/spy_close_hist.csv). PARITY CHECKPOINT: the
//   harness gate uses the SPY row AT the signal date — the wirer must push the
//   row's SPY close BEFORE the universe closes for exact parity, else the gate
//   lags one day. Deploy-forward; positions + refused-buy retries persist.
//   Heartbeat: NO internal registration — the wirer registers AND pulses.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "OpenPositionRegistry.hpp"

namespace omega {

class Bigcap3G4Engine {
public:
    struct Config {
        bool   enabled        = false;
        bool   live_book      = false;  // KEEP false until post-commission size/venue cert (header NOTE)
        double thr_pct        = 3.0;    // entry day-move threshold
        int    hold_d         = 3;      // time-stop, name-bars after entry (cert h3)
        double vol_gate_pct   = 20.0;   // G4: SPY 20d realized vol must be < this (annualized %)
        double vol_median_pct = 11.8;   // VS: hold->1 when vol > this (harness full-sample
                                        // median; live should periodically re-derive)
        int    max_names      = 0;      // 0 = uncapped (harness has no cap); >0 = live safety cap
        int    retry_rows     = 3;      // refused-buy retry TTL in daily rows
        double lot            = 1.0;    // shares per entry (proving size)
        std::string engine_tag = "Bigcap3G4";
        std::string state_path = "bigcap3g4_live.txt";
        // BIGCAP-45 (backtest/dualmom_sweep.py BIGCAP — identical to the 45-name
        // bigcap_clean_2026-07-21e panel the cert harness ran on; verified 1:1).
        std::vector<std::string> universe = {
            "NVDA","AMD","AVGO","MU","MRVL","SMCI","ARM","PLTR","TSLA","META",
            "NFLX","CRWD","SHOP","COIN","MSTR","SNOW","NOW","PANW","UBER","ABNB",
            "DELL","ORCL","QCOM","INTC","AMZN","GOOGL","MSFT","AAPL","CRM","ADBE",
            "IONQ","RGTI","QBTS","ASTS","RKLB","NBIS","CRWV","ALAB","CRDO","WDC",
            "STX","DD","TPR","BMY","SWKS"};
    };
    Config cfg;

    using OpenFn   = std::function<std::string(const std::string&, bool, double, double)>;
    using CloseFn  = std::function<void(const std::string&, bool, double, double, const std::string&)>;
    using GateFn   = std::function<bool(const std::string&, double, double)>;
    using LedgerFn = std::function<void(const std::string&, const std::string&, bool,
                                        double, double, double, int64_t, int64_t, const char*)>;
    void set_exec(OpenFn o, CloseFn c, GateFn g, LedgerFn l) {
        open_fn_ = std::move(o); close_fn_ = std::move(c);
        gate_fn_ = std::move(g); ledger_fn_ = std::move(l);
    }

    // SPY regime feed (fed by engine_init from spy_close_hist.csv rows; same feed
    // as DualMom/Hi52). Locked: gate accessors may be read cross-thread.
    void push_spy_close(double c) {
        std::lock_guard<std::mutex> lk(mu_);
        spy_.push_back(c); if (spy_.size() > 260) spy_.pop_front();
    }
    // unlocked internals: called under mu_ from the entry path (non-recursive
    // mutex — the DualMom gate_on()/spy_bull() split, DualMomentumEngine L75-86).
    bool gate_bull_() const {
        if (spy_.size() < 200) return false;               // fail-CLOSED
        double s = 0; for (size_t i = spy_.size() - 200; i < spy_.size(); ++i) s += spy_[i];
        return spy_.back() > s / 200.0;
    }
    // SPY 20d realized vol, annualized % (sweep3pct.py L37-39: POPULATION stdev
    // of the last 20 log returns * sqrt(252)). <0 = insufficient history.
    double spy_rvol_pct_() const {
        if (spy_.size() < 21) return -1.0;
        double lr[20]; double m = 0;
        for (int k = 0; k < 20; ++k) {
            double a = spy_[spy_.size() - 21 + (size_t)k], b = spy_[spy_.size() - 20 + (size_t)k];
            if (a <= 0 || b <= 0) return -1.0;
            lr[k] = std::log(b / a); m += lr[k];
        }
        m /= 20.0;
        double v = 0; for (int k = 0; k < 20; ++k) v += (lr[k] - m) * (lr[k] - m);
        return std::sqrt(v / 20.0) * std::sqrt(252.0) * 100.0;
    }
    bool gate_pass_() const {                              // G4, fail-CLOSED
        double rv = spy_rvol_pct_();
        return gate_bull_() && rv >= 0 && rv < cfg.vol_gate_pct;
    }
    // Locked cross-thread views (other engines / selftests may read these).
    bool spy_bull()          { std::lock_guard<std::mutex> lk(mu_); return gate_bull_(); }
    double spy_vol_pct()     { std::lock_guard<std::mutex> lk(mu_); return spy_rvol_pct_(); }
    bool gate_on()           { std::lock_guard<std::mutex> lk(mu_); return gate_pass_(); }

    // history seed: push closes only, no trading; call finalize_seed() after.
    void seed_close(const std::string& sym, double close) {
        auto& h = hist_[sym];
        h.push_back(close); if (h.size() > 30) h.pop_front();
    }
    void finalize_seed() {
        // deploy-forward: start FLAT; the entry rule only needs prev-close, the
        // gate needs 200 SPY closes (seed the SPY deque via push_spy_close).
        std::printf("[BIGCAP3G4] seed done: %zu names with history, spy=%zu bars\n",
                    hist_.size(), spy_.size());
        std::fflush(stdout);
    }

    // one daily close for one universe name (poller-driven, same cadence as sdt).
    // Signal + exits are evaluated PER NAME at its own bar (the harness iterates
    // each name's own bar series) — step_() at row completion does retry +
    // persistence only. (DualMom defers to step_ because its rebalance is
    // cross-sectional; this entry rule is per-name.)
    void on_daily_close(const std::string& sym, int64_t ts_sec, double close) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& h = hist_[sym];
        double prev = h.empty() ? 0.0 : h.back();
        h.push_back(close); if (h.size() > 30) h.pop_front();
        bool changed = manage_name_(sym, ts_sec, close);
        changed = try_entry_(sym, ts_sec, close, prev) || changed;
        seen_today_.insert(sym);
        if (seen_today_.size() >= cfg.universe.size() / 2) {   // most of the row is in
            seen_today_.clear();
            step_(ts_sec);            // retry pass + save (row heartbeat)
            changed = false;          // step_ saved
        }
        if (changed) save_();
    }

    int kill_all(double, int64_t now_sec) {
        std::lock_guard<std::mutex> lk(mu_);
        int n = 0;
        for (auto& p : open_) { close_pos_(p, last_px_(p.sym), now_sec, "MANUAL_KILL_ALL"); ++n; }
        open_.clear(); retry_.clear(); save_();
        return n;
    }

    std::vector<PositionSnapshot> collect_positions() {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<PositionSnapshot> v;
        for (auto& p : open_) {
            PositionSnapshot o;
            o.engine = cfg.engine_tag; o.symbol = p.sym; o.side = "LONG";
            o.size = cfg.lot; o.entry = p.entry;
            o.current = last_px_(p.sym);
            o.unrealized_pnl = (o.current - p.entry) * cfg.lot;
            o.entry_ts = p.entry_ts;      // seconds-native (poller now_s, epoch s)
            o.token = p.token;
            v.push_back(o);
        }
        return v;
    }

    void load_state() {
        std::ifstream f(cfg.state_path);
        if (!f.is_open()) return;
        f >> row_no_;
        std::string s, tok; double e; long long ts; int held, tgt;
        while (f >> s) {
            // RETRY rows persist broker-refused buys across a service restart
            // (DualMom pattern, ALAB/SNOW 2026-07-23). Extension vs DualMom: a
            // TTL (rows left) rides along — DualMom expires retries at the next
            // rebalance selection; this engine has no rebalance, so after
            // retry_rows rows the signal context is gone and the slot drops.
            if (s == "RETRY") {
                std::string r; int left = cfg.retry_rows;
                if (f >> r >> left) retry_[r] = left;
                continue;
            }
            if (!(f >> e >> ts >> held >> tgt >> tok)) break;
            Pos p; p.sym = s; p.entry = e; p.entry_ts = (int64_t)ts;
            p.held = held; p.hold_tgt = tgt;
            p.token = (tok == "-") ? std::string() : tok;
            open_.push_back(p);
        }
        if (!open_.empty() || !retry_.empty())
            std::printf("[BIGCAP3G4] restored %zu open position(s), %zu retry slot(s), row_no=%d\n",
                        open_.size(), retry_.size(), row_no_);
    }

private:
    struct Pos {
        std::string sym;
        double  entry = 0;
        int64_t entry_ts = 0;
        int     held = 0;         // name-bars since entry (harness j - e_idx)
        int     hold_tgt = 3;     // VS-resolved hold target, FIXED at entry (run_exits L112)
        std::string token;
    };
    std::map<std::string, std::deque<double>> hist_;
    std::deque<double> spy_;
    std::vector<Pos> open_;                 // vector: same-name overlap is harness-legal
    std::set<std::string> seen_today_;
    std::map<std::string, int> retry_;      // refused buys -> rows-left TTL
    int row_no_ = 0;
    std::mutex mu_;
    OpenFn open_fn_; CloseFn close_fn_; GateFn gate_fn_; LedgerFn ledger_fn_;

    double last_px_(const std::string& s) const {
        auto it = hist_.find(s);
        return (it != hist_.end() && !it->second.empty()) ? it->second.back() : 0.0;
    }

    // time-stop pass (run_exits sweep3pct.py L113-129 with trail/lc/be all 0):
    // the ONLY exit is the close of the hold_tgt-th name-bar after entry.
    bool manage_name_(const std::string& sym, int64_t ts, double c) {
        bool changed = false;
        for (auto it = open_.begin(); it != open_.end();) {
            if (it->sym != sym) { ++it; continue; }
            ++it->held;                                    // held = j - e_idx
            if (it->held >= it->hold_tgt) {
                close_pos_(*it, c, ts, it->hold_tgt == 1 ? "TIME_STOP_VS1" : "TIME_STOP");
                it = open_.erase(it); changed = true;
            } else ++it;
        }
        return changed;
    }

    // signal (sweep3pct.py L80-88): close >= prev*(1+thr), same_close entry at
    // THIS close — gated by G4 at the signal date; VS hold target fixed here.
    bool try_entry_(const std::string& sym, int64_t ts, double c, double prev) {
        if (prev <= 0 || c <= 0) return false;
        if (c < prev * (1.0 + cfg.thr_pct / 100.0)) return false;
        if (!gate_pass_()) return false;                   // G4 refusal IS the protection
        if (cfg.max_names > 0 && (int)open_.size() >= cfg.max_names) {
            std::printf("[BIGCAP3G4] SIGNAL %s @%.2f SKIPPED (max_names=%d cap)\n",
                        sym.c_str(), c, cfg.max_names);
            std::fflush(stdout);
            return false;
        }
        double rv = spy_rvol_pct_();
        int tgt = (rv > cfg.vol_median_pct) ? 1 : cfg.hold_d;   // VS lever, at-entry
        return open_at_(sym, ts, c, tgt, false);
    }

    bool open_at_(const std::string& sym, int64_t ts, double px, int tgt, bool is_retry) {
        std::string tok;
        if (cfg.live_book && open_fn_) {
            // honest-thin TP proxy = certified mean edge (+43bp), see COST GATE note
            if (gate_fn_ && !gate_fn_(sym, px * 0.0043, cfg.lot)) return false;
            tok = open_fn_(sym, true, cfg.lot, px);
            if (tok.empty()) {
                std::printf("[BIGCAP3G4] BUY %s @%.2f REFUSED by exec -- not booked, retry (%d rows)\n",
                            sym.c_str(), px, cfg.retry_rows);
                std::fflush(stdout);
                retry_[sym] = cfg.retry_rows;
                return true;
            }
        }
        Pos p; p.sym = sym; p.entry = px; p.entry_ts = ts; p.held = 0; p.hold_tgt = tgt;
        p.token = tok;
        open_.push_back(p);
        retry_.erase(sym);
        std::printf("[BIGCAP3G4] BUY %s @%.2f hold_tgt=%d%s tok=%s (open=%zu)\n",
                    sym.c_str(), px, tgt, is_retry ? " (RETRY after refusal)" : "",
                    tok.empty() ? "(book-only)" : tok.c_str(), open_.size());
        std::fflush(stdout);
        return true;
    }

    void close_pos_(const Pos& p, double px, int64_t ts, const char* why) {
        if (px <= 0) px = p.entry;
        if (cfg.live_book && !p.token.empty() && close_fn_)
            close_fn_(p.sym, true, cfg.lot, px, p.token);
        if (ledger_fn_)
            ledger_fn_(cfg.engine_tag, p.sym, true, p.entry, px, cfg.lot, p.entry_ts, ts, why);
        std::printf("[BIGCAP3G4] CLOSE %s entry=%.2f exit=%.2f held=%d (%s)\n",
                    p.sym.c_str(), p.entry, px, p.held, why);
        std::fflush(stdout);
    }

    // row completion: re-attempt refused buys (only while the gate STILL passes
    // — DualMom retries only when gate_on()), expire stale ones, persist.
    // NOTE: a retried fill books at the RETRY row's close with a freshly
    // VS-resolved hold target — exec-honesty behavior with no harness
    // counterpart (the harness has no refusal concept).
    void step_(int64_t ts) {
        ++row_no_;
        for (auto it = retry_.begin(); it != retry_.end();) {
            const std::string& s = it->first;
            double px = last_px_(s);
            bool filled = false;
            if (px > 0 && gate_pass_() &&
                !(cfg.max_names > 0 && (int)open_.size() >= cfg.max_names)) {
                double rv = spy_rvol_pct_();
                int tgt = (rv > cfg.vol_median_pct) ? 1 : cfg.hold_d;
                filled = retry_open_(s, ts, px, tgt);
            }
            if (filled) { it = retry_.erase(it); continue; }
            if (--(it->second) <= 0) {
                std::printf("[BIGCAP3G4] retry %s EXPIRED (signal context gone)\n", s.c_str());
                std::fflush(stdout);
                it = retry_.erase(it);
            } else ++it;
        }
        save_();
    }
    // retry-open that reports fill success without touching retry_ (caller iterates it)
    bool retry_open_(const std::string& sym, int64_t ts, double px, int tgt) {
        std::string tok;
        if (cfg.live_book && open_fn_) {
            if (gate_fn_ && !gate_fn_(sym, px * 0.0043, cfg.lot)) return false;
            tok = open_fn_(sym, true, cfg.lot, px);
            if (tok.empty()) return false;                // still refused, keep retrying
        }
        Pos p; p.sym = sym; p.entry = px; p.entry_ts = ts; p.held = 0; p.hold_tgt = tgt;
        p.token = tok;
        open_.push_back(p);
        std::printf("[BIGCAP3G4] BUY %s @%.2f hold_tgt=%d (RETRY after refusal) tok=%s\n",
                    sym.c_str(), px, tgt, tok.empty() ? "(book-only)" : tok.c_str());
        std::fflush(stdout);
        return true;
    }

    void save_() const {
        const std::string tmp = cfg.state_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          f << row_no_ << "\n";
          for (const auto& p : open_)
              f << p.sym << " " << p.entry << " " << (long long)p.entry_ts << " "
                << p.held << " " << p.hold_tgt << " "
                << (p.token.empty() ? "-" : p.token) << "\n";
          for (const auto& [s, left] : retry_) f << "RETRY " << s << " " << left << "\n"; }
#if defined(_WIN32)
        std::remove(cfg.state_path.c_str());
#endif
        std::rename(tmp.c_str(), cfg.state_path.c_str());
    }
};

inline Bigcap3G4Engine& bigcap3_g4_engine() noexcept {
    static Bigcap3G4Engine e;
    return e;
}

} // namespace omega

#pragma once
// DayMover7Engine.hpp — 7%-day-mover continuation rider on the 27-name validated
// largecap tier (S-2026-07-23, BULLGATE_PROTECTION_SWEEPS build-queue item 5:
// "5% DayMover thr7% — NEW engine (real exec path, DualMom-pattern wire)").
//
// CERT (backtest/BULLGATE_PROTECTION_SWEEPS_2026-07-23.md §A, cell "WIDE g7 NONE";
// base harness = tools/rdagent/daymover_solution_bt.py, re-swept on the threshold
// grid in the S-2026-07-23 scratchpad sweep5pct.py / refine_both.py / finalists.py):
//   threshold 7%, NO entry gate, WIDE ride (no floor / no trail), 60-bar time exit:
//   n=328 (~46/yr) PF 3.27 mDD 270 (units: summed per-trade %) MAR 12.7,
//   2022 traded +103% PF 1.52, ex-WDC PASS, WF+/+, 20bp RT, 2x-cost +.
//   Alt certified: thr6 + rev3d-12 cut (n=485, PF 3.16, 2022 +81.5%) for 2x flow.
//
// MECHANISM — replicates the harness EXACTLY (line refs: sweep5pct.py = the S-23
// sweep over daymover_solution_bt.py semantics; base-harness refs in brackets):
//   ENTRY (entry_indices, sweep5pct.py L57-65 [daymover_solution_bt.py L102-114]):
//     name's daily close >= prev close * (1 + thr_pct/100)  AND  close >= max of
//     the last hi_k+1 closes INCLUDING today (CONTIN_K=20 -> new 20d closing high)
//     -> LONG at THAT close, cfg.lot shares. Same-name re-arm allowed the very
//     next day: entries are INDEPENDENT overlapping trades (harness appends every
//     qualifying index), hence a vector book, not a per-symbol map.
//   EXIT (main_trade WIDE = be_arm=1e9, sweep5pct.py L81-102 [dm_solution L137-183]):
//     ride max_hold=60 name-bars, exit at that bar's CLOSE (L99
//     exit_i=min(i+MAX_HOLD,n-1)). NO init-stop, NO trail, NO BE-floor in the
//     certified cell. There is NO regime-switched exit anywhere in this family:
//     the harness bull/bear flag is an ENTRY-gate lever only, and the certified
//     cell sets it to NONE ("no gate" — BULLGATE doc: bull-gates make 2022 WORSE,
//     gate-lag residual trades are all losers).
//   OPTIONAL rev3d cut (sweep5pct.py L93): within the first rev_days bars, if
//     close-favorability <= -rev3d_cut_pct -> exit at that close ("rev_cut").
//     Certified at thr6 (rev3d-12); default OFF at thr7.
//   OPTIONAL trail (sweep5pct.py L94, checked BEFORE the peak update L95):
//     close <= peak*(1 - trail_pct/100) -> exit at close. Swept but NOT part of
//     the certified cell; default OFF. Exposed only because it is a real harness
//     lever — do not enable without its own cert.
//
// ADVERSE-PROTECTION: trail-only/threshold certified (BULLGATE_PROTECTION_SWEEPS
//   _2026-07-23.md §A): init-stops REJECTED — hard init-stops flip 2022 negative;
//   BE-and-ride floor kills 2022; rev3d-12 optional certified at thr6; the ENTRY
//   THRESHOLD IS the bear protection (2022 monotone g3 −992 -> g8 +164; thr7
//   2022 PF 1.52 traded). Certified cell rides 60 bars unprotected by design.
//
// COST GATE: ExecutionCostGuard::is_viable injected via gate_fn_ before every
//   live order (same wiring shape as DualMom/StockDipTurtle; 2%-of-price TP proxy).
//
// FEED: the wide daily-close CSV (data/rdagent/sp500_long_close.csv — the SAME
//   file the cert harness loads) polled by the sdt-style 15-min poller; poller
//   delivers each name-row ONCE (DualMom contract). Deploy-forward; positions and
//   refused-buy retries persist across restarts. 1 share/name proving size.
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

class DayMover7Engine {
public:
    struct Config {
        bool   enabled       = false;
        bool   live_book     = false;
        double thr_pct       = 7.0;   // entry day-move threshold (cert thr7)
        int    hi_k          = 20;    // new-K-day-closing-high continuation filter (CONTIN_K)
        int    max_hold      = 60;    // WIDE ride window in name-bars (MAX_HOLD)
        double rev3d_cut_pct = 0.0;   // 0 = OFF (cert at thr7); 12.0 = the thr6-certified variant
        int    rev_days      = 3;     // rev-cut window (harness rev_days=3)
        double trail_pct     = 0.0;   // 0 = OFF (cert); harness L94 lever, uncertified at thr7
        int    max_names     = 64;    // live safety cap (harness uncapped; observed max_conc 64)
        int    retry_rows    = 3;     // refused-buy retry TTL in daily rows (see retry note below)
        double lot           = 1.0;   // shares per entry (proving size)
        std::string engine_tag = "DayMover7";
        std::string state_path = "daymover7_live.txt";
        // The harness's 27-name VALIDATED universe: LARGECAP tier filtered to
        // >=90% coverage in sp500_long_close.csv minus glitch names (sweep5pct.py
        // load() L24-29 == daymover_solution_bt.py load()). Recomputed 2026-07-23.
        std::vector<std::string> universe = {
            "AAPL","MSFT","NVDA","AMZN","GOOGL","GOOG","AVGO","ORCL","AMD","MU",
            "INTC","QCOM","TXN","ADI","LRCX","AMAT","KLAC","NOW","CRM","ADBE",
            "NFLX","ANET","CDNS","SNPS","WDC","MCHP","HPQ"};
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

    // history seed: push closes only, no trading; call finalize_seed() after.
    void seed_close(const std::string& sym, double close) {
        auto& h = hist_[sym];
        h.push_back(close); if (h.size() > 90) h.pop_front();
    }
    void finalize_seed() {
        // deploy-forward: start FLAT; hi_k+1 closes per name is all the warmup the
        // entry rule needs (strict 20d-high; see try_entry_ warmup note).
        std::printf("[DAYMOVER7] seed done: %zu names with history, first live row eligible\n",
                    hist_.size());
        std::fflush(stdout);
    }

    // one daily close for one universe name (poller-driven, same cadence as sdt).
    // Signal + exits are evaluated PER NAME at its own bar (exactly the harness's
    // per-name bar loop) — NOT deferred to row completion, because names arriving
    // in the LATE half of a row would otherwise never be evaluated for that row
    // (DualMom can defer because its rebalance is cross-sectional; this entry
    // rule is per-name). step_() at row completion does retry + persistence only.
    void on_daily_close(const std::string& sym, int64_t ts_sec, double close) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& h = hist_[sym];
        double prev = h.empty() ? 0.0 : h.back();
        h.push_back(close); if (h.size() > 90) h.pop_front();
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
            o.mfe = (p.peak - p.entry) * cfg.lot;
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
        std::string s, tok; double e, pk; long long ts; int held;
        while (f >> s) {
            // RETRY rows persist broker-refused buys across a service restart
            // (DualMom pattern, ALAB/SNOW 2026-07-23). Extension vs DualMom: a
            // TTL (rows left) rides along — DualMom expires retries at the next
            // rebalance selection; this engine has no rebalance, so after
            // retry_rows rows the momentum context is gone and the slot drops.
            if (s == "RETRY") {
                std::string r; int left = cfg.retry_rows;
                if (f >> r >> left) retry_[r] = left;
                continue;
            }
            if (!(f >> e >> ts >> held >> pk >> tok)) break;
            Pos p; p.sym = s; p.entry = e; p.entry_ts = (int64_t)ts;
            p.held = held; p.peak = pk;
            p.token = (tok == "-") ? std::string() : tok;
            open_.push_back(p);
        }
        if (!open_.empty() || !retry_.empty())
            std::printf("[DAYMOVER7] restored %zu open position(s), %zu retry slot(s), row_no=%d\n",
                        open_.size(), retry_.size(), row_no_);
    }

private:
    struct Pos {
        std::string sym;
        double  entry = 0;
        int64_t entry_ts = 0;
        int     held = 0;         // name-bars since entry (harness held = k - i)
        double  peak = 0;         // peak close since entry (trail lever; MFE display)
        std::string token;
    };
    std::map<std::string, std::deque<double>> hist_;
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

    // one harness bar-loop pass (sweep5pct.py L87-99) for every open position in
    // this name. All exits in this family fill AT THE CLOSE (daily-close-grade
    // cert — no intraday stop levels exist here).
    bool manage_name_(const std::string& sym, int64_t ts, double c) {
        bool changed = false;
        for (auto it = open_.begin(); it != open_.end();) {
            if (it->sym != sym) { ++it; continue; }
            ++it->held;                                    // held = k - i
            double fav = c / it->entry - 1.0;
            const char* why = nullptr;
            // order matches the harness loop: rev-cut (L93), trail vs OLD peak
            // (L94), THEN peak update (L95), then time exit (L99).
            if (cfg.rev3d_cut_pct > 0 && it->held <= cfg.rev_days &&
                fav <= -cfg.rev3d_cut_pct / 100.0)
                why = "REV3D_CUT";
            else if (cfg.trail_pct > 0 && c <= it->peak * (1.0 - cfg.trail_pct / 100.0))
                why = "TRAIL";
            if (c > it->peak) it->peak = c;
            if (!why && it->held >= cfg.max_hold) why = "MAX_HOLD";
            if (why) { close_pos_(*it, c, ts, why); it = open_.erase(it); changed = true; }
            else ++it;
        }
        return changed;
    }

    // entry rule (sweep5pct.py L57-65): day move >= thr AND new hi_k-day closing
    // high (window INCLUDES today, so the condition is "today is the max").
    // Warmup: harness scans from i=15 with a truncated window; we require the
    // full hi_k+1 closes (strictly no-looser — differs only in a name's first
    // ~3 weeks of history).
    bool try_entry_(const std::string& sym, int64_t ts, double c, double prev) {
        if (prev <= 0 || c <= 0) return false;
        const auto& h = hist_[sym];
        if ((int)h.size() < cfg.hi_k + 1) return false;
        if (c < prev * (1.0 + cfg.thr_pct / 100.0)) return false;
        double hi = 0;
        for (size_t i = h.size() - (size_t)(cfg.hi_k + 1); i < h.size(); ++i)
            hi = std::max(hi, h[i]);
        if (c < hi) return false;                         // not the 20d-high close
        if ((int)open_.size() >= cfg.max_names) {
            std::printf("[DAYMOVER7] SIGNAL %s @%.2f SKIPPED (max_names=%d cap)\n",
                        sym.c_str(), c, cfg.max_names);
            std::fflush(stdout);
            return false;
        }
        return open_at_(sym, ts, c, false);
    }

    // shared open path (fresh signal + retry). Returns true if book/retry changed.
    bool open_at_(const std::string& sym, int64_t ts, double px, bool is_retry) {
        std::string tok;
        if (cfg.live_book && open_fn_) {
            if (gate_fn_ && !gate_fn_(sym, px * 0.02, cfg.lot)) return false;  // cost gate
            tok = open_fn_(sym, true, cfg.lot, px);
            if (tok.empty()) {
                // broker refused (boot-time qualify race etc.): do NOT book a
                // phantom; park in retry_ for per-row re-attempts (DualMom S-23e).
                std::printf("[DAYMOVER7] BUY %s @%.2f REFUSED by exec -- not booked, retry (%d rows)\n",
                            sym.c_str(), px, cfg.retry_rows);
                std::fflush(stdout);
                retry_[sym] = cfg.retry_rows;
                return true;
            }
        }
        Pos p; p.sym = sym; p.entry = px; p.entry_ts = ts; p.held = 0; p.peak = px;
        p.token = tok;
        open_.push_back(p);
        retry_.erase(sym);
        std::printf("[DAYMOVER7] BUY %s @%.2f%s tok=%s (open=%zu)\n",
                    sym.c_str(), px, is_retry ? " (RETRY after refusal)" : "",
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
        std::printf("[DAYMOVER7] CLOSE %s entry=%.2f exit=%.2f held=%d (%s)\n",
                    p.sym.c_str(), p.entry, px, p.held, why);
        std::fflush(stdout);
    }

    // row completion: re-attempt refused buys, expire stale ones, persist.
    void step_(int64_t ts) {
        ++row_no_;
        for (auto it = retry_.begin(); it != retry_.end();) {
            const std::string& s = it->first;
            double px = last_px_(s);
            bool filled = false;
            if (px > 0 && (int)open_.size() < cfg.max_names)
                filled = open_and_check_(s, ts, px);
            if (filled) { it = retry_.erase(it); continue; }
            if (--(it->second) <= 0) {
                std::printf("[DAYMOVER7] retry %s EXPIRED (signal context gone)\n", s.c_str());
                std::fflush(stdout);
                it = retry_.erase(it);
            } else ++it;
        }
        save_();
    }
    // retry-open that reports fill success without touching retry_ (caller iterates it)
    bool open_and_check_(const std::string& sym, int64_t ts, double px) {
        std::string tok;
        if (cfg.live_book && open_fn_) {
            if (gate_fn_ && !gate_fn_(sym, px * 0.02, cfg.lot)) return false;
            tok = open_fn_(sym, true, cfg.lot, px);
            if (tok.empty()) return false;                // still refused, keep retrying
        }
        Pos p; p.sym = sym; p.entry = px; p.entry_ts = ts; p.held = 0; p.peak = px;
        p.token = tok;
        open_.push_back(p);
        std::printf("[DAYMOVER7] BUY %s @%.2f (RETRY after refusal) tok=%s\n",
                    sym.c_str(), px, tok.empty() ? "(book-only)" : tok.c_str());
        std::fflush(stdout);
        return true;
    }

    void save_() const {
        const std::string tmp = cfg.state_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          f << row_no_ << "\n";
          for (const auto& p : open_)
              f << p.sym << " " << p.entry << " " << (long long)p.entry_ts << " "
                << p.held << " " << p.peak << " "
                << (p.token.empty() ? "-" : p.token) << "\n";
          for (const auto& [s, left] : retry_) f << "RETRY " << s << " " << left << "\n"; }
#if defined(_WIN32)
        std::remove(cfg.state_path.c_str());
#endif
        std::rename(tmp.c_str(), cfg.state_path.c_str());
    }
};

inline DayMover7Engine& day_mover7_engine() noexcept {
    static DayMover7Engine e;
    return e;
}

} // namespace omega

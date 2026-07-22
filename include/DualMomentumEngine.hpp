#pragma once
// DualMomentumEngine.hpp — monthly-class dual-momentum rotation on the BIGCAP-45
// (S-2026-07-23a, operator: "wire dual momentum, max protection, lower the DD").
//
// CERT (backtest/dualmom_sweep.py + DD-lever pass, current data, survivorship
// judged vs same-universe gated-EW control):
//   K=8 / rel-lookback 63d / abs 251d>0 / rebal 10d / SPY-200DMA cash gate /
//   per-name stop -20% / vol-target 25%:
//   Sharpe 1.86 (control 1.34), maxDD 21.5% (base 31.6%), 2022 -12.2%,
//   both cost levels (8/16bp) identical, both WF halves +.
//   The recorded-and-dead "2022=0" claim is NOT this cert — this one reproduces.
//
// ADVERSE-PROTECTION (backtested tonight): per-name -20% stop from entry IMPROVES
//   results (Sharpe 1.69->1.78 alone) — losers cut are dead weight; SPY-200DMA
//   gate exits the whole book to CASH in bear regimes (89% of 2022 in cash);
//   vol-target 25% (name-count scaling at 1-share granularity — documented
//   deviation) cuts maxDD ~1/3. KILL-ALL flattens via token-matched closes.
//
// COST GATE: ExecutionCostGuard::is_viable injected via gate_fn_ before every
//   live order (same wiring shape as StockDipTurtle).
//
// FEED: the wide daily-close CSV (data/rdagent/sp500_long_close.csv) polled by
//   the sdt-style 15-min poller (wired in engine_init); SPY regime from
//   data/spy_close_hist.csv (the BigCapHi52 feed). Deploy-forward; holdings and
//   day-counter persist across restarts. 1 share/name proving size.
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

class DualMomentumEngine {
public:
    struct Config {
        bool   enabled   = false;
        bool   live_book = false;
        int    K         = 8;
        int    lb_rel    = 63;
        int    lb_abs    = 251;
        int    rebal_d   = 10;
        double stop_pct  = 20.0;   // per-name from entry
        double voltgt    = 0.25;   // annualized; scales held-name count
        double lot       = 1.0;    // shares per name (proving size)
        std::string engine_tag = "DualMom";
        std::string state_path = "dualmom_live.txt";
        std::vector<std::string> universe;
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

    // SPY regime (fed by engine_init from spy_close_hist.csv rows; same feed as Hi52)
    void push_spy_close(double c) {
        spy_.push_back(c); if (spy_.size() > 260) spy_.pop_front();
    }
    bool gate_on() const {
        if (spy_.size() < 200) return false;               // fail-CLOSED: no regime = cash
        double s = 0; for (size_t i = spy_.size() - 200; i < spy_.size(); ++i) s += spy_[i];
        return spy_.back() > s / 200.0;
    }

    // history seed: push closes only, no trading; call finalize_seed() after.
    void seed_close(const std::string& sym, double close) {
        auto& h = hist_[sym];
        h.push_back(close); if (h.size() > 300) h.pop_front();
    }
    void finalize_seed() {
        // deploy-forward: start FLAT; align the counter so the FIRST live daily
        // row triggers a rebalance (entries at the next close, not day 10).
        day_no_ = cfg.rebal_d - 1;
        std::printf("[DUALMOM] seed done: %zu names with history, spy=%zu bars, first live row rebalances\n",
                    hist_.size(), spy_.size());
        std::fflush(stdout);
    }

    // one daily close for one universe name (poller-driven, same cadence as sdt)
    void on_daily_close(const std::string& sym, int64_t ts_sec, double close) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& h = hist_[sym];
        h.push_back(close); if (h.size() > 300) h.pop_front();
        seen_today_.insert(sym);
        if (seen_today_.size() < cfg.universe.size() / 2) return;   // wait for most of the row
        seen_today_.clear();
        ++day_no_;
        step_(ts_sec);
    }

    int kill_all(double, int64_t now_sec) {
        std::lock_guard<std::mutex> lk(mu_);
        int n = 0;
        for (auto& [s, p] : held_) {
            close_name_(s, p, last_px_(s), now_sec, "MANUAL_KILL_ALL"); ++n;
        }
        held_.clear(); save_();
        return n;
    }

    std::vector<PositionSnapshot> collect_positions() {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<PositionSnapshot> v;
        for (auto& [s, p] : held_) {
            PositionSnapshot o;
            o.engine = cfg.engine_tag; o.symbol = s; o.side = "LONG";
            o.size = cfg.lot; o.entry = p.entry;
            o.current = last_px_(s);
            o.unrealized_pnl = (o.current - p.entry) * cfg.lot;
            o.entry_ts = p.entry_ts;  // seconds-native (poller now_s, epoch s)
            o.token = p.token;
            v.push_back(o);
        }
        return v;
    }

    void load_state() {
        std::ifstream f(cfg.state_path);
        if (!f.is_open()) return;
        f >> day_no_;
        std::string s, tok; double e; long long ts;
        while (f >> s) {
            // RETRY rows persist broker-refused buys across a service restart --
            // without this the in-memory retry_ set dies with the process and the
            // refused slot silently sits empty until the next rebalance
            // (ALAB/SNOW 2026-07-23: refusal 17:05Z, deploy restart 17:18Z wiped it).
            if (s == "RETRY") { std::string r; if (f >> r) retry_.insert(r); continue; }
            if (!(f >> e >> ts >> tok)) break;
            Pos p; p.entry = e; p.entry_ts = (int64_t)ts;
            p.token = (tok == "-") ? std::string() : tok;
            held_[s] = p;
        }
        if (!held_.empty() || !retry_.empty())
            std::printf("[DUALMOM] restored %zu holding(s), %zu retry slot(s), day_no=%d\n",
                        held_.size(), retry_.size(), day_no_);
    }

private:
    struct Pos { double entry = 0; int64_t entry_ts = 0; std::string token; };
    std::map<std::string, std::deque<double>> hist_;
    std::deque<double> spy_;
    std::map<std::string, Pos> held_;
    std::set<std::string> seen_today_;
    std::deque<double> book_ret_;
    double last_book_val_ = 0;
    int day_no_ = 0;
    std::set<std::string> retry_;   // broker-refused buys awaiting per-row re-attempt
    std::mutex mu_;
    OpenFn open_fn_; CloseFn close_fn_; GateFn gate_fn_; LedgerFn ledger_fn_;

    double last_px_(const std::string& s) const {
        auto it = hist_.find(s);
        return (it != hist_.end() && !it->second.empty()) ? it->second.back() : 0.0;
    }

    void close_name_(const std::string& s, const Pos& p, double px, int64_t ts, const char* why) {
        if (px <= 0) px = p.entry;
        if (cfg.live_book && !p.token.empty() && close_fn_)
            close_fn_(s, true, cfg.lot, px, p.token);
        if (ledger_fn_)
            ledger_fn_(cfg.engine_tag, s, true, p.entry, px, cfg.lot, p.entry_ts, ts, why);
        std::printf("[DUALMOM] CLOSE %s entry=%.2f exit=%.2f (%s)\n", s.c_str(), p.entry, px, why);
        std::fflush(stdout);
    }

    void step_(int64_t ts) {
        // book daily return for the vol target
        double val = 0; int nn = 0;
        for (auto& [s, p] : held_) { double px = last_px_(s); if (px > 0) { val += px; ++nn; } }
        if (nn && last_book_val_ > 0) {
            book_ret_.push_back(val / last_book_val_ - 1.0);
            if (book_ret_.size() > 20) book_ret_.pop_front();
        }
        last_book_val_ = nn ? val : 0;

        // per-name stop, every day (max protection)
        for (auto it = held_.begin(); it != held_.end();) {
            double px = last_px_(it->first);
            if (px > 0 && px < it->second.entry * (1.0 - cfg.stop_pct / 100.0)) {
                close_name_(it->first, it->second, px, ts, "NAME_STOP20");
                it = held_.erase(it);
            } else ++it;
        }
        // gate exit any day
        if (!gate_on() && !held_.empty()) {
            for (auto& [s, p] : held_) close_name_(s, p, last_px_(s), ts, "REGIME_CASH");
            held_.clear(); save_(); return;
        }
        // refused-buy retry (broker rejected at rebalance, e.g. boot-time qualify
        // race): re-attempt on EVERY daily row until filled or rotated out, so a
        // slot doesn't sit empty for a whole rebal_d cycle.
        if (!retry_.empty() && gate_on()) {
            for (auto it = retry_.begin(); it != retry_.end();) {
                const std::string& s = *it;
                double px = last_px_(s);
                if (held_.count(s) || px <= 0) { ++it; continue; }
                std::string tok;
                if (cfg.live_book && open_fn_) {
                    if (gate_fn_ && !gate_fn_(s, px * 0.02, cfg.lot)) { ++it; continue; }
                    tok = open_fn_(s, true, cfg.lot, px);
                    if (tok.empty()) { ++it; continue; }   // still refused, keep retrying
                }
                Pos p; p.entry = px; p.entry_ts = ts; p.token = tok;
                held_[s] = p;
                std::printf("[DUALMOM] BUY %s @%.2f (RETRY after refusal) tok=%s\n",
                            s.c_str(), px, tok.empty() ? "(book-only)" : tok.c_str());
                std::fflush(stdout);
                it = retry_.erase(it);
            }
        }
        if (day_no_ % cfg.rebal_d != 0) { save_(); return; }
        if (!gate_on()) { save_(); return; }
        retry_.clear();   // fresh selection supersedes any stale refused slots

        // vol-target -> effective K (name-count scaling; documented deviation)
        int keff = cfg.K;
        if (book_ret_.size() >= 20 && cfg.voltgt > 0) {
            double m = 0; for (double r : book_ret_) m += r; m /= book_ret_.size();
            double v = 0; for (double r : book_ret_) v += (r - m) * (r - m);
            v = std::sqrt(v / book_ret_.size()) * std::sqrt(252.0);
            if (v > cfg.voltgt) keff = std::max(2, (int)std::round(cfg.K * cfg.voltgt / v));
        }
        // dual-momentum selection
        std::vector<std::pair<double, std::string>> sc;
        for (const auto& s : cfg.universe) {
            auto it = hist_.find(s);
            if (it == hist_.end()) continue;
            const auto& h = it->second;
            if ((int)h.size() <= cfg.lb_abs) continue;
            double c0 = h.back(), cr = h[h.size() - 1 - cfg.lb_rel], ca = h[h.size() - 1 - cfg.lb_abs];
            if (cr <= 0 || ca <= 0 || c0 / ca - 1.0 <= 0) continue;
            sc.push_back({c0 / cr - 1.0, s});
        }
        if ((int)sc.size() < keff) { save_(); return; }
        std::sort(sc.rbegin(), sc.rend());
        std::set<std::string> tgt;
        for (int i = 0; i < keff; ++i) tgt.insert(sc[i].second);
        // sells
        for (auto it = held_.begin(); it != held_.end();) {
            if (!tgt.count(it->first)) {
                close_name_(it->first, it->second, last_px_(it->first), ts, "ROTATE_OUT");
                it = held_.erase(it);
            } else ++it;
        }
        // buys
        for (const auto& s : tgt) {
            if (held_.count(s)) continue;
            double px = last_px_(s); if (px <= 0) continue;
            Pos p; p.entry = px; p.entry_ts = ts;
            if (cfg.live_book && open_fn_) {
                if (gate_fn_ && !gate_fn_(s, px * 0.02, cfg.lot)) continue;
                p.token = open_fn_(s, true, cfg.lot, px);
                // broker refused (e.g. contract qualify still pending on a boot-time
                // rebalance -- ALAB/SNOW 2026-07-23): do NOT book a phantom the broker
                // doesn't hold; leave the slot open so the next daily poll retries.
                if (p.token.empty()) {
                    std::printf("[DUALMOM] BUY %s @%.2f REFUSED by exec (no token) -- not booked, retry next row\n",
                                s.c_str(), px);
                    std::fflush(stdout);
                    retry_.insert(s);
                    continue;
                }
            }
            std::printf("[DUALMOM] BUY %s @%.2f (K_eff=%d) tok=%s\n",
                        s.c_str(), px, keff, p.token.empty() ? "(book-only)" : p.token.c_str());
            std::fflush(stdout);
            held_[s] = p;
        }
        save_();
    }

    void save_() const {
        const std::string tmp = cfg.state_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          f << day_no_ << "\n";
          for (const auto& [s, p] : held_)
              f << s << " " << p.entry << " " << (long long)p.entry_ts << " "
                << (p.token.empty() ? "-" : p.token) << "\n";
          for (const auto& s : retry_) f << "RETRY " << s << "\n"; }
#if defined(_WIN32)
        std::remove(cfg.state_path.c_str());
#endif
        std::rename(tmp.c_str(), cfg.state_path.c_str());
    }
};

inline DualMomentumEngine& dual_momentum_engine() noexcept {
    static DualMomentumEngine e;
    return e;
}

} // namespace omega

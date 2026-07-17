#pragma once
// =============================================================================
// BigCapHi52Engine — 52wk-high-proximity portfolio book on the 45-name BIGCAP
// universe (S-2026-07-17k scan candidate C, backtest/BIGCAP45_ENGINE_SCAN_
// FINDINGS_2026-07-17.md + harness backtest/bigcap45_engine_scan_bt.py):
//
//   RULE (exact scan port): every 5 trading days, if SPY close > SPY SMA200,
//   hold EQUAL-WEIGHT every universe name whose close is within 5% of its own
//   252-day (>=200 obs) closing high — membership is threshold-based (NO top-K
//   concentration bet, dodging the XS-rotation failure mode). Gate checked
//   DAILY between rebalances: gate off -> liquidate to flat. Cost 8bp/side on
//   turnover (2x-robust in the scan).
//
//   EVIDENCE (vs the same-universe gated-EW45 control — the only honest metric
//   on a survivorship-chosen universe): sh 1.27 v 1.18, shEx24 1.10 v 0.98,
//   mdd 29.4 v 31.7, 2022 -11.1 v -18.7, both halves +, 2x-cost robust.
//
// ADVERSE-PROTECTION: (S-2026-07-17k, backtested verdict) the SPY-200DMA gate +
//   weekly threshold-rebalance IS the protection: the gate held 2022 to -11.1%
//   (control -18.7%) and the scan's per-year table shows no left-tail year the
//   gate misses; a per-name stop inside an equal-weight threshold basket would
//   change the certified shape (names exit by falling >5% off their high at the
//   next rebal — a built-in per-name trailing exit at portfolio grade). No cold
//   cut, by design; book-level protection = the gate liquidation path.
//
// FEEDS: (1) the wide daily-close CSV data/rdagent/sp500_long_close.csv (same
//   OmegaStockMoverFeed nightly refresh the StockDipTurtle books poll) — own
//   15-min poller; (2) SPY closes for the gate: data/spy_close_hist.csv
//   ("ts_sec,close" rows, written nightly by tools/fetch_macro_regime.py — the
//   OmegaMacroRegime task). GATE STALENESS: SPY hist older than 6 calendar days
//   -> FREEZE (hold current members, block new rebalances, loud log) — never
//   liquidate on a dead feed, never open on stale data.
//
// PAPER BOOK: SHADOW, judged STANDALONE, $10k pool, deploy-forward ($0 until
//   the first forward daily step). No broker orders, no central-ledger rows —
//   a PORTFOLIO paper book (rdagent-basket class): state json hi52_state.json
//   is the record; equity/watermark persist across restarts (hi52_persist.txt).
//   NO real-money path whatsoever in this header.
// =============================================================================
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <mutex>
#include "SeedGuard.hpp"   // omega::resolve_seed_path

namespace omega {

class BigCapHi52Engine {
public:
    struct Config {
        std::vector<std::string> universe;                    // 45 BIGCAP names
        double pool_usd     = 10000.0;                        // paper pool
        int    rebal_days   = 5;                              // weekly (trading days)
        double x_pct        = 5.0;                            // within-X% of 52wk high
        int    hi_win       = 252;                            // 52wk closing-high window
        int    hi_min_obs   = 200;                            // scan parity
        double cost_side_bp = 8.0;                            // per-side on turnover
        int    spy_sma      = 200;
        int64_t gate_stale_sec = 6 * 86400;                   // freeze past this
        std::string spy_hist_path = "data/spy_close_hist.csv";
        std::string persist_path  = "hi52_persist.txt";       // watermark + eq + weights
        std::string state_path    = "hi52_state.json";
    };

    explicit BigCapHi52Engine(Config c) : cfg_(std::move(c)) {
        for (size_t k = 0; k < cfg_.universe.size(); ++k) idx_[cfg_.universe[k]] = k;
        closes_.assign(cfg_.universe.size(), {});
        load_persist_();
    }
    ~BigCapHi52Engine() { stop_poller(); }

    // ── boot: seed the wide CSV (indicators only up to watermark; beyond it the
    //   rows replay through the LIVE step so a restart never skips a rebal) ──
    size_t seed_from_wide_csv(const std::string& rel) noexcept {
        const std::string path = omega::resolve_seed_path(rel);
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[HI52][SEED] MISS %s\n", path.c_str()); std::fflush(stdout); return 0; }
        std::string header;
        if (!std::getline(f, header)) return 0;
        map_cols_(header);
        load_spy_();
        std::string line; size_t rows = 0, live_rows = 0;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            const int64_t ts = parse_date_ts_(line);
            if (ts <= 0 || ts <= last_ts_) continue;
            const bool live = (watermark_ > 0 && ts > watermark_);
            ingest_row_(line, ts, live);
            if (live) ++live_rows;
            ++rows;
        }
        if (watermark_ <= 0 && last_ts_ > 0) {
            // first-ever boot: deploy-forward anchor = newest seeded row
            deploy_ts_ = last_ts_;
        }
        watermark_ = last_ts_;
        save_persist_();
        write_state_();
        std::printf("[HI52][SEED] wide-csv %s: %zu rows (%zu live-replayed), %zu/%zu cols mapped, spy=%zu, last=%lld, deploy_ts=%lld, eq_fwd=%.4f\n",
                    path.c_str(), rows, live_rows, mapped_, cfg_.universe.size(),
                    spy_ts_.size(), (long long)last_ts_, (long long)deploy_ts_, eq_fwd_);
        std::fflush(stdout);
        return rows;
    }

    void start_poller(const std::string& rel, int poll_ms = 900000) {
        csv_rel_ = rel; poll_ms_ = poll_ms;
        running_.store(true);
        thread_ = std::thread([this]{ poll_loop_(); });
        std::printf("[HI52][POLL] watching %s (poll=%dms, resume-from=%lld)\n",
                    rel.c_str(), poll_ms_, (long long)last_ts_);
        std::fflush(stdout);
    }
    void stop_poller() {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
    }

    double pnl_usd() const { std::lock_guard<std::mutex> lk(mu_); return (eq_fwd_ - 1.0) * cfg_.pool_usd; }
    std::string state_json() const { std::lock_guard<std::mutex> lk(mu_); return state_json_unlocked_(); }

private:
    Config cfg_;
    std::unordered_map<std::string, size_t> idx_;
    std::vector<std::vector<double>> closes_;     // per-name closes on the row spine (NaN = missing)
    std::vector<int64_t> row_ts_;
    std::unordered_map<int, size_t> col2u_;       // csv col -> universe index
    size_t mapped_ = 0;
    int64_t last_ts_ = 0, watermark_ = 0, deploy_ts_ = 0;

    // SPY gate series
    std::vector<int64_t> spy_ts_;
    std::vector<double>  spy_c_;

    // portfolio state
    std::unordered_map<size_t, double> w_;        // name idx -> weight
    int    rows_since_rebal_ = 999;               // force a rebal decision on first live row
    double eq_fwd_ = 1.0;                         // forward equity (deploy-forward, 1.0 at deploy)
    bool   gate_frozen_logged_ = false;

    mutable std::mutex mu_;

    // poller
    std::string csv_rel_;
    int poll_ms_ = 900000;
    std::atomic<bool> running_{false};
    std::thread thread_;

    // ── feed plumbing ──
    void map_cols_(const std::string& header) {
        col2u_.clear(); mapped_ = 0;
        std::stringstream hs(header); std::string tok; int ci = 0;
        while (std::getline(hs, tok, ',')) {
            auto it = idx_.find(tok);
            if (it != idx_.end()) { col2u_[ci] = it->second; ++mapped_; }
            ++ci;
        }
    }
    static int64_t parse_date_ts_(const std::string& line) noexcept {
        int y = 0, m = 0, d = 0;
        if (std::sscanf(line.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return 0;
        if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
        return days_from_civil_(y, m, d) * 86400LL;
    }
    static int64_t days_from_civil_(int y, unsigned m, unsigned d) noexcept {
        y -= (m <= 2);
        const int64_t era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = (unsigned)(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097LL + (int64_t)doe - 719468LL;
    }

    void load_spy_() {
        spy_ts_.clear(); spy_c_.clear();
        std::ifstream f(omega::resolve_seed_path(cfg_.spy_hist_path));
        if (!f.is_open()) { std::printf("[HI52][GATE] SPY hist MISS %s — gate FROZEN until feed appears\n", cfg_.spy_hist_path.c_str()); std::fflush(stdout); return; }
        std::string line;
        while (std::getline(f, line)) {
            long long ts = 0; double cl = 0;
            if (std::sscanf(line.c_str(), "%lld,%lf", &ts, &cl) == 2 && cl > 0) {
                if (!spy_ts_.empty() && ts <= spy_ts_.back()) continue;
                spy_ts_.push_back((int64_t)ts); spy_c_.push_back(cl);
            }
        }
    }
    // gate state at row time ts: 1=on, 0=off, -1=frozen (stale/absent feed)
    int gate_at_(int64_t ts) const {
        if (spy_c_.size() < (size_t)cfg_.spy_sma) return -1;
        if (ts - spy_ts_.back() > cfg_.gate_stale_sec) return -1;
        // newest SPY close at-or-before ts
        size_t i = spy_ts_.size();
        while (i > 0 && spy_ts_[i-1] > ts) --i;
        if (i < (size_t)cfg_.spy_sma) return -1;
        double s = 0; for (size_t k = i - cfg_.spy_sma; k < i; ++k) s += spy_c_[k];
        return spy_c_[i-1] > s / cfg_.spy_sma ? 1 : 0;
    }

    // ── one daily row: append closes, then (live only) run the portfolio step ──
    void ingest_row_(const std::string& line, int64_t ts, bool live) {
        std::vector<double> px(cfg_.universe.size(), 0.0);
        { std::stringstream ls(line); std::string tok; int ci = 0;
          while (std::getline(ls, tok, ',')) {
              auto it = col2u_.find(ci);
              if (it != col2u_.end() && !tok.empty()) {
                  char* end = nullptr; const double v = std::strtod(tok.c_str(), &end);
                  if (end != tok.c_str() && v > 0.0) px[it->second] = v;
              }
              ++ci;
          } }
        // torn-read guard (house >50% single-day rule, portfolio grade: drop the
        // NAME for this row — a x1000 glitch must not enter the 52wk-high window)
        for (size_t u = 0; u < px.size(); ++u) {
            if (px[u] > 0 && !closes_[u].empty()) {
                const double prev = last_close_(u);
                if (prev > 0 && std::fabs(px[u] / prev - 1.0) > 0.50) px[u] = 0.0;
            }
        }
        row_ts_.push_back(ts);
        for (size_t u = 0; u < px.size(); ++u) closes_[u].push_back(px[u] > 0 ? px[u] : NAN);
        last_ts_ = ts;
        if (live) step_(ts);
    }
    double last_close_(size_t u) const {
        for (auto it = closes_[u].rbegin(); it != closes_[u].rend(); ++it)
            if (!std::isnan(*it)) return *it;
        return 0.0;
    }

    // exact scan-port portfolio step on the newest row (index N-1); pnl applies
    // yesterday's weights to today's close-close returns, THEN the rebal decision
    // runs on today's close (scan convention: select on close[i], earn RET[i+1]).
    void step_(int64_t ts) {
        std::lock_guard<std::mutex> lk(mu_);
        const size_t N = row_ts_.size();
        if (N < 2) return;
        const bool fwd = ts > deploy_ts_;
        // 1) earn yesterday's weights on today's returns
        double pr = 0.0;
        for (const auto& kv : w_) {
            const auto& c = closes_[kv.first];
            if (c.size() >= 2 && !std::isnan(c[N-1]) && !std::isnan(c[N-2]) && c[N-2] > 0)
                pr += kv.second * (c[N-1] / c[N-2] - 1.0);
        }
        if (fwd) eq_fwd_ *= (1.0 + pr);
        // 2) rebal / gate logic on today's close
        rows_since_rebal_ += 1;
        const int g = gate_at_(ts);
        const double cost = cfg_.cost_side_bp / 1e4;
        if (g < 0) {   // frozen feed: hold, no new decisions
            if (!gate_frozen_logged_) {
                gate_frozen_logged_ = true;
                std::printf("[HI52][GATE] SPY feed stale/short — FROZEN (holding %zu members, no rebal)\n", w_.size());
                std::fflush(stdout);
            }
        } else {
            gate_frozen_logged_ = false;
            if (rows_since_rebal_ >= cfg_.rebal_days) {
                rows_since_rebal_ = 0;
                std::unordered_map<size_t, double> tgt;
                if (g == 1) {
                    std::vector<size_t> mem;
                    for (size_t u = 0; u < cfg_.universe.size(); ++u) {
                        const auto& c = closes_[u];
                        if (std::isnan(c[N-1])) continue;
                        const size_t lo = N > (size_t)cfg_.hi_win ? N - cfg_.hi_win : 0;
                        double hi = 0; int obs = 0;
                        for (size_t k = lo; k < N; ++k) if (!std::isnan(c[k])) { hi = std::max(hi, c[k]); ++obs; }
                        if (obs >= cfg_.hi_min_obs && c[N-1] >= hi * (1.0 - cfg_.x_pct / 100.0)) mem.push_back(u);
                    }
                    for (size_t u : mem) tgt[u] = 1.0 / (double)mem.size();
                }
                double turn = 0.0;
                for (const auto& kv : tgt) turn += std::fabs(kv.second - (w_.count(kv.first) ? w_.at(kv.first) : 0.0));
                for (const auto& kv : w_)  if (!tgt.count(kv.first)) turn += std::fabs(kv.second);
                if (fwd) eq_fwd_ *= (1.0 - turn * cost);
                w_ = tgt;
            } else if (g == 0 && !w_.empty()) {   // gate dropped between rebals: liquidate
                double turn = 0.0; for (const auto& kv : w_) turn += kv.second;
                if (fwd) eq_fwd_ *= (1.0 - turn * cost);
                w_.clear();
            }
        }
        watermark_ = ts;
        save_persist_unlocked_();
        write_state_unlocked_();
    }

    // ── persist: watermark, deploy anchor, forward equity, weights ──
    void load_persist_() {
        std::ifstream f(cfg_.persist_path);
        if (!f.is_open()) return;
        std::string line;
        if (std::getline(f, line)) {
            long long wm = 0, dp = 0; double eq = 1.0;
            if (std::sscanf(line.c_str(), "%lld %lld %lf", &wm, &dp, &eq) == 3) {
                watermark_ = wm; deploy_ts_ = dp; eq_fwd_ = eq;
            }
        }
        while (std::getline(f, line)) {
            char nm[32] = {0}; double ww = 0;
            if (std::sscanf(line.c_str(), "%31s %lf", nm, &ww) == 2) {
                auto it = idx_.find(nm);
                if (it != idx_.end()) w_[it->second] = ww;
            }
        }
    }
    void save_persist_() { std::lock_guard<std::mutex> lk(mu_); save_persist_unlocked_(); }
    void save_persist_unlocked_() const {
        const std::string tmp = cfg_.persist_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          f << (long long)watermark_ << " " << (long long)deploy_ts_ << " " << eq_fwd_ << "\n";
          for (const auto& kv : w_) f << cfg_.universe[kv.first] << " " << kv.second << "\n"; }
#if defined(_WIN32)
        std::remove(cfg_.persist_path.c_str());
#endif
        std::rename(tmp.c_str(), cfg_.persist_path.c_str());
    }

    void write_state_() { std::lock_guard<std::mutex> lk(mu_); write_state_unlocked_(); }
    void write_state_unlocked_() const {
        std::ostringstream o; o << std::fixed;
        o << "{\"engine\":\"BigCapHi52\",\"shadow\":true,\"grade\":\"daily-close\",\"pool_usd\":";
        o.precision(0); o << cfg_.pool_usd << ",\"pnl_usd\":" << (eq_fwd_ - 1.0) * cfg_.pool_usd;
        o.precision(6); o << ",\"eq_fwd\":" << eq_fwd_;
        o << ",\"gate\":" << gate_at_(last_ts_) << ",\"deploy_ts\":" << (long long)deploy_ts_
          << ",\"ts\":" << (long long)last_ts_ << ",\"members\":[";
        bool first = true;
        for (const auto& kv : w_) {
            if (!first) o << ","; first = false;
            o.precision(4);
            o << "{\"sym\":\"" << cfg_.universe[kv.first] << "\",\"w\":" << kv.second << "}";
        }
        o << "]}";
        const std::string tmp = cfg_.state_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return; f << o.str(); }
#if defined(_WIN32)
        std::remove(cfg_.state_path.c_str());
#endif
        std::rename(tmp.c_str(), cfg_.state_path.c_str());
    }
    std::string state_json_unlocked_() const {
        std::ifstream f(cfg_.state_path); std::ostringstream o; o << f.rdbuf(); return o.str();
    }

    void poll_loop_() {
        while (running_.load()) {
            for (int slept = 0; slept < poll_ms_ && running_.load(); slept += 200)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (!running_.load()) break;
            const std::string path = omega::resolve_seed_path(csv_rel_);
            std::ifstream f(path);
            if (!f.is_open()) continue;
            std::string header;
            if (!std::getline(f, header)) continue;
            map_cols_(header);
            load_spy_();   // refresh the gate series alongside (nightly file)
            int fresh = 0; std::string line;
            while (std::getline(f, line)) {
                if (line.empty()) continue;
                const int64_t ts = parse_date_ts_(line);
                if (ts <= 0 || ts <= last_ts_) continue;
                ingest_row_(line, ts, /*live=*/true);
                ++fresh;
            }
            if (fresh > 0) {
                std::printf("[HI52][POLL] %d daily row(s), newest=%lld, members=%zu, eq_fwd=%.4f\n",
                            fresh, (long long)last_ts_, w_.size(), eq_fwd_);
                std::fflush(stdout);
            }
        }
    }
};

inline BigCapHi52Engine& bigcap_hi52_engine(BigCapHi52Engine::Config* boot = nullptr) noexcept {
    static BigCapHi52Engine inst(boot ? std::move(*boot)
                                      : BigCapHi52Engine::Config{});
    return inst;
}

} // namespace omega

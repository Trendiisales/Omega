#pragma once
// =============================================================================
// EngineGate.hpp -- automatic engine demotion gate.
//
// Accumulates PERSISTENT per-engine lifetime trade stats (n, wins, net) across
// restarts, and auto-disables any engine that proves unprofitable over a
// meaningful sample. Threshold mirrors the CLAUDE.md promotion gate:
//   demote when  n >= 30  AND  net < 0  AND  win_rate < 35%.
//
// Flow:
//   - record_close(engine, net_pnl) is called from the UNIVERSAL trade-close
//     path (trade_lifecycle.hpp), so every engine's every close is counted.
//     Stats are flushed to logs/engine_gate_stats.csv each close (survives
//     restart -- the 5-day trade journal alone can't reach n=30).
//   - At startup engine_init.hpp calls load() then, for each standalone engine
//     in its name->enabled-flag table, is_demoted(name) -> set enabled=false.
//
// Below the 30-trade floor the gate does NOTHING (insufficient evidence) -- it
// will NOT cut a validated trend engine in a short drawdown. It only fires once
// an engine has had a fair shot and still lost. Disable-only (never re-enables);
// to revive an engine, clear its row from the CSV and flip enabled back on.
//
// Name matching aggregates exact + cell-tag-prefixed keys: is_demoted("X")
// sums every stat key equal to "X" or beginning "X_" (so a multi-cell engine
// that logs "X_cellA"/"X_cellB" is judged as a whole).
// =============================================================================
#include <string>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <mutex>
#include <cstdio>
#include <cstdlib>

namespace omega {

struct EngineGateStat { long n = 0; long wins = 0; double net = 0.0; };

class EngineGate {
public:
    static constexpr long   MIN_TRADES = 30;     // sample floor before any cut
    static constexpr double MIN_WR     = 0.35;   // win-rate floor

    void set_path(const std::string& p) { std::lock_guard<std::mutex> lk(mtx_); path_ = p; }

    void load() {
        std::lock_guard<std::mutex> lk(mtx_);
        stats_.clear();
        std::ifstream f(path_);
        if (!f.is_open()) return;
        std::string line; std::getline(f, line);  // header
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string name, sn, sw, snet;
            std::getline(ss, name, ','); std::getline(ss, sn, ',');
            std::getline(ss, sw, ',');   std::getline(ss, snet, ',');
            if (name.empty()) continue;
            EngineGateStat s;
            s.n    = std::strtol(sn.c_str(),  nullptr, 10);
            s.wins = std::strtol(sw.c_str(),  nullptr, 10);
            s.net  = std::strtod(snet.c_str(), nullptr);
            stats_[name] = s;
        }
    }

    void record_close(const std::string& engine, double net_pnl) {
        if (engine.empty()) return;
        std::lock_guard<std::mutex> lk(mtx_);
        auto& s = stats_[engine];
        s.n++; if (net_pnl > 0.0) s.wins++; s.net += net_pnl;
        flush_locked();
    }

    bool is_demoted(const std::string& name) const {
        std::lock_guard<std::mutex> lk(mtx_);
        long n = 0, w = 0; double net = 0.0;
        for (const auto& kv : stats_) {
            const std::string& k = kv.first;
            const bool match = (k == name) ||
                (k.size() > name.size() &&
                 k.compare(0, name.size(), name) == 0 && k[name.size()] == '_');
            if (match) { n += kv.second.n; w += kv.second.wins; net += kv.second.net; }
        }
        if (n < MIN_TRADES) return false;
        if (net >= 0.0)     return false;
        return (static_cast<double>(w) / static_cast<double>(n)) < MIN_WR;
    }

    // Startup diagnostic: log every engine that has reached the sample floor.
    void evaluate_log() const {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto& kv : stats_) {
            const auto& s = kv.second;
            if (s.n < MIN_TRADES) continue;
            const double wr = s.n ? static_cast<double>(s.wins) / s.n : 0.0;
            const bool demote = (s.net < 0.0 && wr < MIN_WR);
            std::printf("[ENGINE-GATE] %-44s n=%ld WR=%.0f%% net=%.2f -> %s\n",
                        kv.first.c_str(), s.n, wr * 100.0, s.net,
                        demote ? "DEMOTE" : "keep");
        }
        std::fflush(stdout);
    }

private:
    void flush_locked() {
        std::ofstream f(path_, std::ios::trunc);
        if (!f.is_open()) return;
        f << "engine,n,wins,net\n";
        for (const auto& kv : stats_)
            f << kv.first << ',' << kv.second.n << ',' << kv.second.wins
              << ',' << kv.second.net << '\n';
    }

    mutable std::mutex mtx_;
    std::unordered_map<std::string, EngineGateStat> stats_;
    std::string path_ = "logs/engine_gate_stats.csv";
};

} // namespace omega

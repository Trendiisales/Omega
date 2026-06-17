#pragma once
// =============================================================================
// AuroraGate.hpp -- order-flow entry gate driven by the real MGC/NQ CME tape.
//
// The Aurora shelf engine (ibkr/aurora_flow.py) computes, per symbol, an
// order-flow micro-regime verdict (room to nearest opposing wall, ABS-wall
// block, FLIP+delta bias) and ibkr/aurora_snapshot.py writes it to a FLAT file
// logs/aurora/aurora_gate.tsv (one line per tradable symbol). This header reads
// that file and gates engine entries -- spot XAUUSD has no tape, so the MGC
// futures tape is the real volume proxy; NQ tape proxies NAS100/US500.
//
// FORMAT (tab-separated, '#' comment lines ignored):
//   SYMBOL  allow_long  allow_short  bias  room_long_atr  room_short_atr  stamp_ms
//   XAUUSD  1           0            long  2.10           0.40            1781679000000
//
// SAFETY -- the gate FAILS OPEN in every uncertain case, so a stale/missing/
// malformed Aurora feed can NEVER block the whole book:
//   - master enabled_ flag off            -> allow
//   - file missing / unreadable           -> allow
//   - newest stamp older than max_stale_ms -> allow
//   - symbol not present in the file       -> allow
//   - only an explicit fresh allow_*=0 for that symbol+side blocks.
// now_ms is passed in (wall-clock epoch ms) to avoid time-shim coupling.
// =============================================================================
#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <functional>

namespace omega {

struct AuroraGate {
    struct G {
        bool   allow_long  = true;
        bool   allow_short = true;
        int    bias        = 0;     // +1 long / -1 short / 0 neutral
        double room_long   = 99.0;
        double room_short  = 99.0;
    };

    bool        enabled_      = true;
    std::string path_         = "logs/aurora/aurora_gate.tsv";
    int64_t     max_stale_ms_ = 15LL * 60 * 1000;   // 15 min -> fail open
    int64_t     reload_ms_    = 20000;              // re-read file at most every 20s

    std::unordered_map<std::string, G> map_;
    int64_t  stamp_ms_  = 0;     // newest per-symbol stamp seen in the file
    int64_t  last_check_ = 0;
    std::mutex mtx_;

    void set_path(const std::string& p) { path_ = p; }
    void set_enabled(bool e)            { enabled_ = e; }

    void maybe_reload(int64_t now_ms) {
        if (last_check_ != 0 && now_ms - last_check_ < reload_ms_) return;
        last_check_ = now_ms;
        std::ifstream f(path_);
        if (!f) { std::lock_guard<std::mutex> lk(mtx_); map_.clear(); stamp_ms_ = 0; return; }
        std::unordered_map<std::string, G> m;
        int64_t newest = 0;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            std::string sym, biasstr;
            int al = 1, as = 1; double rl = 99.0, rs = 99.0; int64_t st = 0;
            if (!(ss >> sym >> al >> as >> biasstr >> rl >> rs >> st)) continue;
            G g;
            g.allow_long  = (al != 0);
            g.allow_short = (as != 0);
            g.bias        = (biasstr == "long") ? 1 : (biasstr == "short" ? -1 : 0);
            g.room_long   = rl;
            g.room_short  = rs;
            m[sym] = g;
            if (st > newest) newest = st;
        }
        std::lock_guard<std::mutex> lk(mtx_);
        map_.swap(m);
        stamp_ms_ = newest;
    }

    // True = entry permitted. Fail-open on every uncertain case.
    bool allow(const std::string& sym, bool is_long, int64_t now_ms) {
        if (!enabled_) return true;
        maybe_reload(now_ms);
        std::lock_guard<std::mutex> lk(mtx_);
        if (stamp_ms_ == 0 || (now_ms - stamp_ms_) > max_stale_ms_) return true;  // stale/missing
        auto it = map_.find(sym);
        if (it == map_.end()) return true;                                        // not covered
        return is_long ? it->second.allow_long : it->second.allow_short;
    }

    // +1 long bias / -1 short bias / 0 neutral-or-unknown.
    int bias(const std::string& sym, int64_t now_ms) {
        if (!enabled_) return 0;
        maybe_reload(now_ms);
        std::lock_guard<std::mutex> lk(mtx_);
        if (stamp_ms_ == 0 || (now_ms - stamp_ms_) > max_stale_ms_) return 0;
        auto it = map_.find(sym);
        return (it == map_.end()) ? 0 : it->second.bias;
    }

    // For logging: a short reason. side: true=long.
    std::string reason(const std::string& sym, bool is_long, int64_t now_ms) {
        maybe_reload(now_ms);
        std::lock_guard<std::mutex> lk(mtx_);
        if (!enabled_)                 return "gate-off";
        if (stamp_ms_ == 0)            return "no-file(allow)";
        if ((now_ms - stamp_ms_) > max_stale_ms_) return "stale(allow)";
        auto it = map_.find(sym);
        if (it == map_.end())          return "uncovered(allow)";
        const bool ok = is_long ? it->second.allow_long : it->second.allow_short;
        const double room = is_long ? it->second.room_long : it->second.room_short;
        return std::string(ok ? "allow" : "BLOCK") + " room=" +
               std::to_string(room).substr(0, 4) + "A";
    }
};

// -----------------------------------------------------------------------------
// Engine-side hook. Engine headers are compiled standalone in backtest harnesses
// (no globals.hpp / no g_aurora_gate), so they must NOT reference the global
// directly. Instead they call omega::aurora_allow(...), which routes through a
// hook the live binary installs in engine_init (lambda -> g_aurora_gate.allow).
// In harnesses the hook is empty -> aurora_allow returns true (fail-open, zero
// behavior change). Set once at startup, read-only during trading.
// Use this ONLY for trend / breakout engines (room-to-wall logic); mean-reversion
// engines fade INTO walls and must NOT be gated this way.
// -----------------------------------------------------------------------------
inline std::function<bool(const char*, bool, int64_t)>& aurora_gate_hook() {
    static std::function<bool(const char*, bool, int64_t)> h;
    return h;
}
inline bool aurora_allow(const char* sym, bool is_long, int64_t now_ms) {
    auto& h = aurora_gate_hook();
    return h ? h(sym, is_long, now_ms) : true;   // no hook -> allow
}

}  // namespace omega

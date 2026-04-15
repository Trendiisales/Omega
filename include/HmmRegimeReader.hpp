#pragma once
// =============================================================================
// HmmRegimeReader.hpp -- Lightweight polling reader for hmm_state.json
//
// Written by hmm_refit.py (runs hourly via Task Scheduler).
// C++ side polls the JSON file every POLL_INTERVAL_MS (30s).
// Thread-safe: all state stored in atomics, updated from a single
// call-site in tick_gold.hpp at H1 bar close.
//
// States:
//   CHOPPY   (0) -- low vol, mean-reverting -- H4 entry BLOCKED, H1 size reduced
//   TRENDING (1) -- directional move       -- H4/H1 proceed normally
//   VOLATILE (2) -- high vol, erratic      -- H4 entry BLOCKED, H1 size halved
//   UNKNOWN  (3) -- file stale / fit_ok=false -- all gates disabled (fail-open)
//
// Fail-open design: if the file is >STALE_THRESHOLD_MS stale, or fit_ok=false,
// HMM gating is disabled entirely. Engines fall back to their own ADX/EMA gates.
// This means a crashed hmm_refit.py script causes zero disruption to trading.
//
// Usage (in tick_gold.hpp H1 bar close block):
//   g_hmm_regime.poll_if_due(now_ms);
//   auto hmm = g_hmm_regime.get();
//   // hmm.label:   "CHOPPY" | "TRENDING" | "VOLATILE" | "UNKNOWN"
//   // hmm.state:   0|1|2|3
//   // hmm.p_flip:  probability of regime change next H1 bar
//   // hmm.gating:  true = HMM data is fresh and fit_ok
//
// In HTFSwingEngines.hpp entry gates:
//   H4: if (hmm.gating && hmm.state != HmmState::TRENDING) return sig; // BLOCK
//   H1: effective_risk *= hmm.size_scalar();  // 1.0, 0.75, or 0.50
// =============================================================================

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>

namespace omega {

// ---------------------------------------------------------------------------
// State enum
// ---------------------------------------------------------------------------
enum class HmmState : int {
    CHOPPY   = 0,
    TRENDING = 1,
    VOLATILE = 2,
    UNKNOWN  = 3,
};

// ---------------------------------------------------------------------------
// Snapshot -- what the engine receives
// ---------------------------------------------------------------------------
struct HmmSnapshot {
    HmmState    state      = HmmState::UNKNOWN;
    const char* label      = "UNKNOWN";  // static string -- no alloc
    float       p_flip     = 0.5f;
    float       p_stay     = 0.5f;
    float       vol_regime = 0.5f;
    bool        gating     = false;      // true = data fresh + fit_ok
    int64_t     ts_utc     = 0;
    int         bars_used  = 0;

    // Size scalar for H1SwingEngine: TRENDING=1.0, CHOPPY=0.75, VOLATILE=0.5
    // Only applied when gating=true.
    double size_scalar() const noexcept {
        if (!gating) return 1.0;
        switch (state) {
            case HmmState::TRENDING: return 1.0;
            case HmmState::CHOPPY:   return 0.75;
            case HmmState::VOLATILE: return 0.50;
            default:                 return 1.0;
        }
    }

    // True when this state should block new H4 Donchian breakout entries.
    bool blocks_h4_entry() const noexcept {
        return gating && (state == HmmState::CHOPPY || state == HmmState::VOLATILE);
    }

    // True when this state should block new H1 swing entries entirely.
    // Only block in VOLATILE -- CHOPPY still allows H1 at reduced size.
    bool blocks_h1_entry() const noexcept {
        return gating && (state == HmmState::VOLATILE);
    }
};

// ---------------------------------------------------------------------------
// HmmRegimeReader
// ---------------------------------------------------------------------------
class HmmRegimeReader {
public:
    // Poll interval: read JSON file at most every 30s
    static constexpr int64_t POLL_INTERVAL_MS    = 30'000LL;
    // If hmm_state.json is older than 90 minutes: stale, disable gating
    static constexpr int64_t STALE_THRESHOLD_SEC = 90 * 60LL;

    explicit HmmRegimeReader(const char* json_path)
        : json_path_(json_path)
    {}

    // Call this at H1 bar close (or any convenient low-frequency point).
    // Does nothing if called more frequently than POLL_INTERVAL_MS.
    void poll_if_due(int64_t now_ms) noexcept {
        if (now_ms - last_poll_ms_.load(std::memory_order_relaxed) < POLL_INTERVAL_MS)
            return;
        last_poll_ms_.store(now_ms, std::memory_order_relaxed);
        _load();
    }

    // Force an immediate reload regardless of poll interval.
    void force_reload() noexcept { _load(); }

    // Get current snapshot. Always safe to call -- returns UNKNOWN if not loaded.
    HmmSnapshot get() const noexcept {
        HmmSnapshot s;
        s.state      = static_cast<HmmState>(state_.load(std::memory_order_relaxed));
        s.p_flip     = p_flip_.load(std::memory_order_relaxed);
        s.p_stay     = p_stay_.load(std::memory_order_relaxed);
        s.vol_regime = vol_regime_.load(std::memory_order_relaxed);
        s.gating     = gating_.load(std::memory_order_relaxed);
        s.ts_utc     = ts_utc_.load(std::memory_order_relaxed);
        s.bars_used  = bars_used_.load(std::memory_order_relaxed);
        s.label      = _label_for(s.state);
        return s;
    }

    // Convenience: log current state (throttled to once per poll)
    void log_state(int64_t now_ms) const noexcept {
        static int64_t s_last_log = 0;
        if (now_ms - s_last_log < 3600000LL) return;
        s_last_log = now_ms;
        auto s = get();
        printf("[HMM] state=%s p_flip=%.3f vol=%.3f bars=%d gating=%s\n",
               s.label, s.p_flip, s.vol_regime, s.bars_used,
               s.gating ? "YES" : "NO (fail-open)");
        fflush(stdout);
    }

private:
    const char* json_path_;

    std::atomic<int>     state_      {static_cast<int>(HmmState::UNKNOWN)};
    std::atomic<float>   p_flip_     {0.5f};
    std::atomic<float>   p_stay_     {0.5f};
    std::atomic<float>   vol_regime_ {0.5f};
    std::atomic<bool>    gating_     {false};
    std::atomic<int64_t> ts_utc_     {0};
    std::atomic<int>     bars_used_  {0};
    std::atomic<int64_t> last_poll_ms_{0};

    // ---------------------------------------------------------------------------
    // Minimal JSON parser -- extracts key:value pairs without a full JSON lib.
    // Only handles the flat structure written by hmm_refit.py.
    // ---------------------------------------------------------------------------
    static bool _extract_string(const std::string& json,
                                 const char* key, std::string& out) noexcept {
        // Find "key": "value"
        std::string search = std::string("\"") + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return false;
        pos += search.size();
        // Skip whitespace and colon
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
        if (pos >= json.size() || json[pos] != '"') return false;
        ++pos; // skip opening quote
        size_t end = json.find('"', pos);
        if (end == std::string::npos) return false;
        out = json.substr(pos, end - pos);
        return true;
    }

    static bool _extract_double(const std::string& json,
                                  const char* key, double& out) noexcept {
        std::string search = std::string("\"") + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return false;
        pos += search.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
        if (pos >= json.size()) return false;
        try {
            size_t n = 0;
            out = std::stod(json.substr(pos), &n);
            return (n > 0);
        } catch (...) { return false; }
    }

    static bool _extract_int(const std::string& json,
                               const char* key, int64_t& out) noexcept {
        double d = 0;
        if (!_extract_double(json, key, d)) return false;
        out = static_cast<int64_t>(d);
        return true;
    }

    static bool _extract_bool(const std::string& json,
                                const char* key, bool& out) noexcept {
        std::string search = std::string("\"") + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return false;
        pos += search.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
        if (pos >= json.size()) return false;
        if (json.substr(pos, 4) == "true")  { out = true;  return true; }
        if (json.substr(pos, 5) == "false") { out = false; return true; }
        return false;
    }

    static HmmState _parse_state_label(const std::string& label) noexcept {
        if (label == "CHOPPY")   return HmmState::CHOPPY;
        if (label == "TRENDING") return HmmState::TRENDING;
        if (label == "VOLATILE") return HmmState::VOLATILE;
        return HmmState::UNKNOWN;
    }

    static const char* _label_for(HmmState s) noexcept {
        switch (s) {
            case HmmState::CHOPPY:   return "CHOPPY";
            case HmmState::TRENDING: return "TRENDING";
            case HmmState::VOLATILE: return "VOLATILE";
            default:                 return "UNKNOWN";
        }
    }

    // ---------------------------------------------------------------------------
    // Load and parse hmm_state.json
    // ---------------------------------------------------------------------------
    void _load() noexcept {
        std::ifstream f(json_path_);
        if (!f.good()) {
            // File doesn't exist yet -- not an error, refit may not have run
            gating_.store(false, std::memory_order_relaxed);
            return;
        }

        std::ostringstream ss;
        ss << f.rdbuf();
        const std::string json = ss.str();
        if (json.empty()) {
            gating_.store(false, std::memory_order_relaxed);
            return;
        }

        // Parse fields
        std::string label_str;
        double p_flip = 0.5, p_stay = 0.5, vol_regime = 0.5;
        int64_t ts_utc = 0, bars_used = 0;
        bool fit_ok = false;

        bool ok = true;
        ok &= _extract_string(json, "label",      label_str);
        ok &= _extract_double(json, "p_flip",     p_flip);
        ok &= _extract_double(json, "p_stay",     p_stay);
        ok &= _extract_double(json, "vol_regime", vol_regime);
        ok &= _extract_int   (json, "ts_utc",     ts_utc);
        ok &= _extract_int   (json, "bars_used",  bars_used);
        _extract_bool(json, "fit_ok", fit_ok);  // non-fatal if missing

        if (!ok) {
            printf("[HMM] Parse error in %s -- gating disabled\n", json_path_);
            fflush(stdout);
            gating_.store(false, std::memory_order_relaxed);
            return;
        }

        // Staleness check
        const int64_t now_sec = static_cast<int64_t>(std::time(nullptr));
        const bool stale = (now_sec - ts_utc > STALE_THRESHOLD_SEC);
        if (stale) {
            static int64_t s_stale_log = 0;
            if (now_sec - s_stale_log > 3600LL) {
                s_stale_log = now_sec;
                printf("[HMM] State file stale (age=%llds > %llds) -- gating disabled\n",
                       (long long)(now_sec - ts_utc),
                       (long long)STALE_THRESHOLD_SEC);
                fflush(stdout);
            }
            gating_.store(false, std::memory_order_relaxed);
            return;
        }

        HmmState new_state = _parse_state_label(label_str);
        bool new_gating    = fit_ok && (new_state != HmmState::UNKNOWN);

        // Log on state change
        {
            const HmmState prev = static_cast<HmmState>(state_.load(std::memory_order_relaxed));
            const bool prev_gating = gating_.load(std::memory_order_relaxed);
            if (new_state != prev || new_gating != prev_gating) {
                printf("[HMM] State -> %s (was %s)  p_flip=%.3f  vol=%.3f"
                       "  bars=%d  gating=%s%s\n",
                       _label_for(new_state), _label_for(prev),
                       (float)p_flip, (float)vol_regime, (int)bars_used,
                       new_gating ? "YES" : "NO",
                       new_gating ? "" : " (fail-open)");
                fflush(stdout);
            }
        }

        // Atomic store all fields
        state_     .store(static_cast<int>(new_state),   std::memory_order_relaxed);
        p_flip_    .store(static_cast<float>(p_flip),    std::memory_order_relaxed);
        p_stay_    .store(static_cast<float>(p_stay),    std::memory_order_relaxed);
        vol_regime_.store(static_cast<float>(vol_regime),std::memory_order_relaxed);
        ts_utc_    .store(ts_utc,                        std::memory_order_relaxed);
        bars_used_ .store(static_cast<int>(bars_used),   std::memory_order_relaxed);
        gating_    .store(new_gating,                    std::memory_order_relaxed);
    }
};

} // namespace omega

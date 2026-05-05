#pragma once
// =============================================================================
// EngineHeartbeat.hpp -- Per-engine liveness registry + miss detector
// -----------------------------------------------------------------------------
// 2026-05-05 (audit-fixes-40):
//
//   Why this exists
//   ---------------
//   On 2026-05-04 the system ran 19 hours producing 2 trades (both XAUUSD).
//   All 5 FX engines (EUR/GBP/USDJPY/AUD/NZD) and 3 of 4 index engines were
//   completely silent -- no DIAG, no ARMED, no ENTRY. The only existing
//   liveness layer was `stale_watchdog_ping(sym, bid)` (omega_runtime.hpp:88)
//   which monitors broker-side tick freshness PER SYMBOL. It correctly
//   reported all symbols as alive -- the broker WAS sending FX ticks every
//   30s. But there was no monitoring of whether each ENGINE was receiving
//   those ticks. So an engine whose dispatcher was severed looked, from a
//   monitoring point of view, identical to an engine that was quietly
//   waiting for setup conditions in its session window.
//
//   This header adds the missing layer: a per-engine registry that records
//   each pulse (one per tick the engine sees), runs a startup self-test
//   60s post-init to flag any LIVE-flagged engine that never pulsed, and
//   logs ongoing miss-detector alerts when an engine has gone silent
//   inside its expected session window.
//
//   Three event types are emitted:
//     [HEARTBEAT-INIT]   on engine registration
//     [STARTUP-FAIL]     60s post-init -- engine never pulsed
//     [HEARTBEAT-MISS]   periodic -- engine has not pulsed within cadence
//                                    while inside its declared session window
//
//   Wiring
//   ------
//   - One call per registered engine in engine_init.hpp at end of
//     init_engines(): g_engine_heartbeat.register_engine(...).
//   - One call per tick at the TOP of each on_tick_<symbol> in tick_*.hpp:
//     g_engine_heartbeat.pulse("EngineName").
//   - One call per ~30s from quote_loop.hpp main loop:
//     g_engine_heartbeat.check_misses(now_ms).
//   - One call ~60s after init_engines(): g_engine_heartbeat.run_startup_self_test().
//     Driven from quote_loop.hpp (cheap to call repeatedly; only fires once
//     thanks to the m_self_test_done_ flag).
//
//   Threading
//   ---------
//   pulse() is called from the tick thread (one tick handler at a time).
//   check_misses() and run_startup_self_test() are called from the main
//   loop thread. register_engine() is called from init_engines() before
//   any tick processing starts. All operations are guarded by the same
//   mutex; pulse() is the hot path but the mutex is uncontended in
//   normal operation (only the ticking thread holds it).
//
//   Format of registration
//   ----------------------
//     name                : human-readable engine identifier
//     live_required       : true  = pulse expected during session window;
//                                   absence triggers HEARTBEAT-MISS / STARTUP-FAIL
//                           false = informational; never alerts
//     expected_cadence_s  : max seconds between pulses while in session
//                           window before a HEARTBEAT-MISS is emitted
//     session_start_utc   : start of expected-active window (UTC hour 0..24)
//     session_end_utc     : end of expected-active window (UTC hour 0..24).
//                           If end <= start, treated as wraparound
//                           (e.g. AUDUSD 22-2 covers 22:00..23:59 + 00:00..01:59).
//   Always-on engines should pass session_start=0, session_end=24.
// =============================================================================

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace omega {

class EngineHeartbeat {
public:
    struct Registry {
        std::string name;
        bool        live_required      = false;
        int         expected_cadence_s = 600;
        int         session_start_utc  = 0;
        int         session_end_utc    = 24;
        int64_t     init_ms            = 0;
        int64_t     first_pulse_ms     = 0;
        int64_t     last_pulse_ms      = 0;
        int64_t     total_pulses       = 0;
        // Rate-limit miss alerts so the log isn't spammed once per check
        int64_t     last_miss_log_ms   = 0;
    };

    // Register an engine. No-op if already registered.
    void register_engine(const std::string& name,
                         bool live_required,
                         int  expected_cadence_s,
                         int  session_start_utc,
                         int  session_end_utc) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (registry_.find(name) != registry_.end()) return;
        Registry r;
        r.name               = name;
        r.live_required      = live_required;
        r.expected_cadence_s = expected_cadence_s;
        r.session_start_utc  = session_start_utc;
        r.session_end_utc    = session_end_utc;
        r.init_ms            = now_ms_();
        registry_.emplace(name, r);

        std::printf("[HEARTBEAT-INIT] %s registered (live_required=%s, cadence<=%ds, session=%02d-%02d UTC)\n",
                    name.c_str(),
                    live_required ? "true" : "false",
                    expected_cadence_s,
                    session_start_utc, session_end_utc);
        std::fflush(stdout);
    }

    // Record a tick for this engine. O(1) hashtable lookup. Hot path.
    void pulse(const std::string& name) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = registry_.find(name);
        if (it == registry_.end()) return;
        const int64_t now_ms = now_ms_();
        if (it->second.first_pulse_ms == 0) {
            it->second.first_pulse_ms = now_ms;
        }
        it->second.last_pulse_ms = now_ms;
        ++it->second.total_pulses;
    }

    // Periodic miss-detector. Call from the main loop every ~30s.
    // Emits [HEARTBEAT-MISS] for any live_required engine whose last
    // pulse is older than expected_cadence_s AND we are currently inside
    // its declared session window. Rate-limited to one log per engine
    // per 5 minutes.
    void check_misses(int64_t now_ms) {
        std::lock_guard<std::mutex> lk(mtx_);
        const int rate_limit_ms = 5 * 60 * 1000;  // 5 min between miss alerts
        for (auto& kv : registry_) {
            Registry& r = kv.second;
            if (!r.live_required) continue;
            if (!is_in_session_(r, now_ms)) continue;

            const int64_t threshold_ms = (int64_t)r.expected_cadence_s * 1000;
            const bool ever_pulsed = (r.first_pulse_ms != 0);
            const int64_t since_last_ms = ever_pulsed
                ? (now_ms - r.last_pulse_ms)
                : (now_ms - r.init_ms);

            if (since_last_ms > threshold_ms) {
                if (now_ms - r.last_miss_log_ms < rate_limit_ms) continue;
                r.last_miss_log_ms = now_ms;

                if (!ever_pulsed) {
                    std::printf("[HEARTBEAT-MISS] %s NEVER pulsed (init %llds ago, expected <%ds during %02d-%02d UTC, total_pulses=%lld)\n",
                                r.name.c_str(),
                                (long long)((now_ms - r.init_ms) / 1000),
                                r.expected_cadence_s,
                                r.session_start_utc, r.session_end_utc,
                                (long long)r.total_pulses);
                } else {
                    std::printf("[HEARTBEAT-MISS] %s no pulse in %llds (expected <%ds during %02d-%02d UTC, total_pulses=%lld)\n",
                                r.name.c_str(),
                                (long long)(since_last_ms / 1000),
                                r.expected_cadence_s,
                                r.session_start_utc, r.session_end_utc,
                                (long long)r.total_pulses);
                }
                std::fflush(stdout);
            }
        }
    }

    // Run-once startup self-test ~60s after init_engines(). Logs status
    // for every live_required engine: did it pulse at least once?
    // Idempotent: subsequent calls are no-ops.
    void run_startup_self_test(int64_t now_ms,
                               int delay_after_init_s = 60) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (self_test_done_) return;

        // All registrations share the same init ordering (they happen
        // sequentially inside init_engines). Use the LATEST init_ms among
        // live_required engines as the reference: only run the self-test
        // once delay_after_init_s has elapsed past that.
        int64_t latest_init_ms = 0;
        for (auto& kv : registry_) {
            if (kv.second.live_required) {
                if (kv.second.init_ms > latest_init_ms) latest_init_ms = kv.second.init_ms;
            }
        }
        if (latest_init_ms == 0) return;
        if ((now_ms - latest_init_ms) < (int64_t)delay_after_init_s * 1000) return;

        int n_total = 0, n_pulsed = 0, n_silent = 0;
        std::vector<std::string> silent_names;
        for (auto& kv : registry_) {
            const Registry& r = kv.second;
            if (!r.live_required) continue;
            ++n_total;
            if (r.first_pulse_ms == 0) {
                ++n_silent;
                silent_names.push_back(r.name);
                std::printf("[STARTUP-FAIL] %s never pulsed in %llds post-init (live_required=true, session=%02d-%02d UTC)\n",
                            r.name.c_str(),
                            (long long)((now_ms - r.init_ms) / 1000),
                            r.session_start_utc, r.session_end_utc);
            } else {
                ++n_pulsed;
            }
        }
        std::printf("[STARTUP-SELFTEST] live_required total=%d, pulsed=%d, silent=%d\n",
                    n_total, n_pulsed, n_silent);
        if (n_silent > 0) {
            std::printf("[STARTUP-SELFTEST] silent engines: ");
            for (size_t i = 0; i < silent_names.size(); ++i) {
                std::printf("%s%s", silent_names[i].c_str(),
                            (i + 1 < silent_names.size()) ? ", " : "");
            }
            std::printf("\n");
            std::printf("[STARTUP-SELFTEST] WARNING: silent live_required engines indicate dispatch / init problems. Investigate before next session.\n");
        }
        std::fflush(stdout);
        self_test_done_ = true;
    }

    // Snapshot for telemetry / GUI / diagnostic queries.
    std::vector<Registry> snapshot() const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<Registry> out;
        out.reserve(registry_.size());
        for (auto const& kv : registry_) out.push_back(kv.second);
        return out;
    }

    // For ad-hoc query (telemetry endpoint, GUI panel).
    Registry get(const std::string& name) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = registry_.find(name);
        if (it == registry_.end()) return Registry{};
        return it->second;
    }

    // Total registered count (includes both live_required and informational).
    size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return registry_.size();
    }

private:
    static int64_t now_ms_() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    static bool is_in_session_(const Registry& r, int64_t now_ms) {
        time_t t = static_cast<time_t>(now_ms / 1000);
        struct tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &t);
#else
        gmtime_r(&t, &utc);
#endif
        if (r.session_end_utc > r.session_start_utc) {
            // Forward window
            return (utc.tm_hour >= r.session_start_utc &&
                    utc.tm_hour <  r.session_end_utc);
        } else if (r.session_end_utc == r.session_start_utc) {
            // Always-on (0-0 or 24-24 etc.)
            return true;
        } else {
            // Wraparound window (e.g. 22-02)
            return (utc.tm_hour >= r.session_start_utc ||
                    utc.tm_hour <  r.session_end_utc);
        }
    }

    mutable std::mutex                          mtx_;
    std::unordered_map<std::string, Registry>   registry_;
    bool                                        self_test_done_ = false;
};

} // namespace omega

// Single global instance. Mirrors the g_news_blackout / g_macroDetector
// pattern -- main.cpp pulls every header into a single translation unit so
// internal-linkage statics resolve uniformly across the binary.
static omega::EngineHeartbeat g_engine_heartbeat;

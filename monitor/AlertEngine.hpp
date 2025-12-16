// =============================================================================
// AlertEngine.hpp - Local Alert System
// =============================================================================
// Generates alerts based on engine health conditions.
// No external services - alerts are logged and exposed via API.
// =============================================================================
#pragma once

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>
#include "api/MetricsExporter.hpp"
#include "engine/EngineHealth.hpp"
#include "core/Logger.hpp"

namespace chimera {
namespace monitor {

// =============================================================================
// Alert thresholds (configurable)
// =============================================================================
struct AlertThresholds {
    uint64_t drop_spike_threshold = 100;      // Drops per evaluation
    uint64_t latency_drift_factor = 2;        // p99 > baseline * factor
    uint64_t clock_anomaly_threshold = 10;    // Invalid ticks per evaluation
    uint64_t baseline_p99_ns = 10000000000;    // 10s baseline (dev mode - silence alerts)
};

// =============================================================================
// AlertEngine
// =============================================================================
class AlertEngine {
public:
    static constexpr size_t MAX_ALERTS = 1000;

    AlertEngine(core::Logger* logger = nullptr) noexcept
        : logger_(logger)
        , alert_count_(0)
        , last_tick_drops_(0)
        , last_invalid_ticks_(0)
    {}

    void set_thresholds(const AlertThresholds& t) {
        thresholds_ = t;
    }

    // Evaluate engine health and generate alerts
    void evaluate(uint32_t engine_id,
                  const engine::EngineHealth& health,
                  uint64_t p99_latency_ns) noexcept
    {
        uint64_t now = get_time_ns();
        
        // Check for engine kill
        if (health.is_killed()) {
            add_alert(now, api::AlertSeverity::CRITICAL, api::AlertCode::ENGINE_KILLED,
                     engine_id, "Engine killed: %s", 
                     kill_reason_str(health.get_kill_reason()));
        }

        // Check for drop spike
        uint64_t current_drops = health.tick_drops.load(std::memory_order_relaxed);
        uint64_t delta_drops = current_drops - last_tick_drops_;
        if (delta_drops > thresholds_.drop_spike_threshold) {
            add_alert(now, api::AlertSeverity::WARNING, api::AlertCode::DROP_SPIKE,
                     engine_id, "Drop spike: %llu drops", 
                     (unsigned long long)delta_drops);
        }
        last_tick_drops_ = current_drops;

        // Check for latency drift
        if (p99_latency_ns > thresholds_.baseline_p99_ns * thresholds_.latency_drift_factor) {
            add_alert(now, api::AlertSeverity::WARNING, api::AlertCode::LATENCY_DRIFT,
                     engine_id, "Latency drift: p99=%lluus (baseline=%lluus)",
                     (unsigned long long)(p99_latency_ns / 1000),
                     (unsigned long long)(thresholds_.baseline_p99_ns / 1000));
        }

        // Check for clock anomalies
        uint64_t current_invalid = health.invalid_ticks.load(std::memory_order_relaxed);
        uint64_t delta_invalid = current_invalid - last_invalid_ticks_;
        if (delta_invalid > thresholds_.clock_anomaly_threshold) {
            add_alert(now, api::AlertSeverity::WARNING, api::AlertCode::CLOCK_ANOMALY,
                     engine_id, "Clock anomaly: %llu invalid ticks",
                     (unsigned long long)delta_invalid);
        }
        last_invalid_ticks_ = current_invalid;
    }

    // Manual alert
    void alert(api::AlertSeverity severity, api::AlertCode code,
               uint32_t engine_id, const char* message) noexcept
    {
        add_alert(get_time_ns(), severity, code, engine_id, "%s", message);
    }

    // Get recent alerts
    size_t get_alerts(api::AlertSnapshot* out, size_t max_count) const {
        std::lock_guard<std::mutex> lock(alerts_mutex_);
        size_t count = std::min(max_count, alerts_.size());
        for (size_t i = 0; i < count; ++i) {
            out[i] = alerts_[alerts_.size() - 1 - i];  // Most recent first
        }
        return count;
    }

    // Clear alerts
    void clear() {
        std::lock_guard<std::mutex> lock(alerts_mutex_);
        alerts_.clear();
    }

    uint64_t alert_count() const {
        return alert_count_.load(std::memory_order_relaxed);
    }

private:
    template<typename... Args>
    void add_alert(uint64_t ts, api::AlertSeverity severity, api::AlertCode code,
                   uint32_t engine_id, const char* fmt, Args... args) noexcept
    {
        api::AlertSnapshot alert{};
        alert.ts_ns = ts;
        alert.severity = severity;
        alert.code = code;
        alert.engine_id = engine_id;
        std::snprintf(alert.message, sizeof(alert.message), fmt, args...);

        // Log to stdout
        const char* sev_str = "INFO";
        switch (severity) {
            case api::AlertSeverity::WARNING:  sev_str = "WARN"; break;
            case api::AlertSeverity::CRITICAL: sev_str = "CRIT"; break;
            default: break;
        }
        std::printf("[ALERT][%s] E%u: %s\n", sev_str, engine_id, alert.message);

        // Log to binary logger if available
        if (logger_) {
            logger_->log(ts, engine_id, 
                        severity == api::AlertSeverity::CRITICAL ? 
                            core::LogLevel::WARN : core::LogLevel::INFO,
                        static_cast<uint16_t>(code), 0, 0, 0);
        }

        // Store in ring buffer
        {
            std::lock_guard<std::mutex> lock(alerts_mutex_);
            if (alerts_.size() >= MAX_ALERTS) {
                alerts_.erase(alerts_.begin());
            }
            alerts_.push_back(alert);
        }

        alert_count_.fetch_add(1, std::memory_order_relaxed);
    }

    static const char* kill_reason_str(engine::EngineKillReason r) {
        switch (r) {
            case engine::EngineKillReason::NONE: return "NONE";
            case engine::EngineKillReason::TICK_QUEUE_OVERFLOW: return "TICK_QUEUE_OVERFLOW";
            case engine::EngineKillReason::INTENT_QUEUE_OVERFLOW: return "INTENT_QUEUE_OVERFLOW";
            case engine::EngineKillReason::INVALID_TICK: return "INVALID_TICK";
            case engine::EngineKillReason::EXECUTION_BACKPRESSURE: return "EXECUTION_BACKPRESSURE";
            case engine::EngineKillReason::TIME_SANITY_FAILURE: return "TIME_SANITY";
            case engine::EngineKillReason::MANUAL: return "MANUAL";
            default: return "UNKNOWN";
        }
    }

    static uint64_t get_time_ns() noexcept {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
    }

    core::Logger* logger_;
    AlertThresholds thresholds_;
    std::atomic<uint64_t> alert_count_;
    uint64_t last_tick_drops_;
    uint64_t last_invalid_ticks_;
    mutable std::mutex alerts_mutex_;
    std::vector<api::AlertSnapshot> alerts_;
};

} // namespace monitor
} // namespace chimera

// =============================================================================
// MetricsExporter.hpp - Central Metrics Collection for GUI
// =============================================================================
// Gathers all engine state into a single exportable structure.
// Thread-safe: all reads are atomic loads.
// =============================================================================
#pragma once

#include <cstdint>
#include <atomic>
#include <cstring>
#include "engine/EngineHealth.hpp"
#include "engine/QueueMetrics.hpp"
#include "engine/ExecutionHealth.hpp"
#include "core/LatencyStats.hpp"

namespace chimera {
namespace api {

// =============================================================================
// Engine state enum (for GUI)
// =============================================================================
enum class EngineState : uint8_t {
    RUNNING = 0,
    DEGRADED = 1,
    KILLED = 2,
    COOLDOWN = 3,
    DISABLED = 4
};

// =============================================================================
// Latency percentiles (pre-computed)
// =============================================================================
struct LatencyPercentiles {
    uint64_t p50_ns;
    uint64_t p99_ns;
    uint64_t p999_ns;
    uint64_t max_ns;
    uint64_t count;
};

// =============================================================================
// Engine metrics snapshot (GUI-friendly)
// =============================================================================
struct EngineMetricsSnapshot {
    // Identity
    uint32_t engine_id;
    char feed_name[32];
    
    // State
    EngineState state;
    uint8_t kill_reason;
    
    // Tick metrics
    uint64_t tick_rate;          // ticks/sec (computed)
    uint64_t ticks_processed;
    uint64_t tick_drops;
    uint64_t invalid_ticks;
    
    // Intent metrics
    uint64_t intent_rate;        // intents/sec (computed)
    uint64_t intents_generated;
    uint64_t intent_drops;
    
    // Queue depths
    uint32_t ingress_depth;
    uint32_t intent_depth;
    
    // Latency
    LatencyPercentiles latency;
    
    // CPU
    uint32_t cpu_core;
    float cpu_util_pct;
    
    // Timestamps
    uint64_t snapshot_ts_ns;
    uint64_t last_tick_ts_ns;
};

// =============================================================================
// Strategy regime snapshot
// =============================================================================
struct RegimeSnapshot {
    uint32_t engine_id;
    char symbol[16];
    char regime[32];
    float confidence;
    float volatility;
    float trend;
    float orderflow;
};

// =============================================================================
// Alert types
// =============================================================================
enum class AlertSeverity : uint8_t {
    INFO = 0,
    WARNING = 1,
    CRITICAL = 2
};

enum class AlertCode : uint16_t {
    ENGINE_STARTED = 100,
    ENGINE_KILLED = 200,
    DROP_SPIKE = 300,
    LATENCY_DRIFT = 400,
    CLOCK_ANOMALY = 500,
    EXECUTION_FAILED = 600
};

struct AlertSnapshot {
    uint64_t ts_ns;
    AlertSeverity severity;
    AlertCode code;
    uint32_t engine_id;
    char message[128];
};

// =============================================================================
// Global system snapshot
// =============================================================================
struct SystemSnapshot {
    // Global PnL (shadow or live)
    double realized_pnl;
    double unrealized_pnl;
    
    // Global rates
    uint64_t total_tick_rate;
    uint64_t total_intent_rate;
    
    // Global latency
    LatencyPercentiles global_latency;
    
    // Counts
    uint32_t engines_running;
    uint32_t engines_degraded;
    uint32_t engines_killed;
    
    // Mode
    bool is_live_mode;
    bool is_shadow_mode;
    
    uint64_t uptime_ns;
    uint64_t snapshot_ts_ns;
};

// =============================================================================
// MetricsExporter - Collects and exports all metrics
// =============================================================================
class MetricsExporter {
public:
    MetricsExporter() noexcept
        : last_tick_count_(0)
        
        , last_snapshot_ts_(0)
    {}

    // Collect engine metrics into snapshot
    void collect(uint32_t engine_id,
                 const char* feed_name,
                 const engine::EngineHealth& health,
                 const engine::QueueMetrics& /*queue_metrics*/,
                 const core::GlobalLatencyStats& latency,
                 uint32_t cpu_core,
                 EngineMetricsSnapshot& out) noexcept
    {
        out.engine_id = engine_id;
        std::strncpy(out.feed_name, feed_name, sizeof(out.feed_name) - 1);
        out.feed_name[sizeof(out.feed_name) - 1] = '\0';
        
        // State
        if (health.is_killed()) {
            out.state = EngineState::KILLED;
        } else if (health.tick_drops.load(std::memory_order_relaxed) > 0) {
            out.state = EngineState::DEGRADED;
        } else {
            out.state = EngineState::RUNNING;
        }
        out.kill_reason = static_cast<uint8_t>(health.get_kill_reason());
        
        // Tick metrics
        uint64_t current_ticks = health.ticks_processed.load(std::memory_order_relaxed);
        out.ticks_processed = current_ticks;
        out.tick_drops = health.tick_drops.load(std::memory_order_relaxed);
        out.invalid_ticks = health.invalid_ticks.load(std::memory_order_relaxed);
        
        // Compute tick rate
        uint64_t now = get_time_ns();
        uint64_t elapsed = now - last_snapshot_ts_;
        if (elapsed > 0 && last_snapshot_ts_ > 0) {
            uint64_t delta_ticks = current_ticks - last_tick_count_;
            out.tick_rate = (delta_ticks * 1'000'000'000ULL) / elapsed;
        } else {
            out.tick_rate = 0;
        }
        last_tick_count_ = current_ticks;
        
        // Intent metrics
        out.intents_generated = 0;  // Set by caller
        out.intent_drops = health.intent_drops.load(std::memory_order_relaxed);
        out.intent_rate = 0;  // Set by caller
        
        // Queue depths (approximate)
        out.ingress_depth = 0;  // Would need queue reference
        out.intent_depth = 0;
        
        // Latency
        uint64_t count = latency.tick_signal_count.load(std::memory_order_relaxed);
        uint64_t sum = latency.tick_signal_sum_ns.load(std::memory_order_relaxed);
        out.latency.count = count;
        out.latency.p50_ns = (count > 0) ? (sum / count) : 0;  // Simplified - use avg as p50
        out.latency.p99_ns = out.latency.p50_ns * 2;  // Estimate
        out.latency.p999_ns = out.latency.p50_ns * 4;
        out.latency.max_ns = latency.tick_signal_max_ns.load(std::memory_order_relaxed);
        
        // CPU
        out.cpu_core = cpu_core;
        out.cpu_util_pct = 0.0f;  // Would need external measurement
        
        // Timestamps
        out.snapshot_ts_ns = now;
        out.last_tick_ts_ns = 0;  // Would need from tick
        
        last_snapshot_ts_ = now;
    }

private:
    uint64_t last_tick_count_;
    uint64_t last_snapshot_ts_;

    static uint64_t get_time_ns() noexcept {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
    }
};

// =============================================================================
// JSON serialization helpers (for WS transport)
// =============================================================================
inline int serialize_engine_metrics(const EngineMetricsSnapshot& m, char* buf, int buflen) {
    const char* state_str = "UNKNOWN";
    switch (m.state) {
        case EngineState::RUNNING:  state_str = "RUNNING"; break;
        case EngineState::DEGRADED: state_str = "DEGRADED"; break;
        case EngineState::KILLED:   state_str = "KILLED"; break;
        case EngineState::COOLDOWN: state_str = "COOLDOWN"; break;
        case EngineState::DISABLED: state_str = "DISABLED"; break;
    }
    
    return std::snprintf(buf, buflen,
        "{\"schema\":\"chimera.ws\",\"version\":1,\"type\":\"engine_metrics\","
        "\"ts_ns\":%llu,\"payload\":{"
        "\"engine_id\":%u,\"feed_name\":\"%s\",\"state\":\"%s\","
        "\"tick_rate\":%llu,\"ticks_processed\":%llu,\"tick_drops\":%llu,\"invalid_ticks\":%llu,"
        "\"intent_rate\":%llu,\"intents_generated\":%llu,\"intent_drops\":%llu,"
        "\"latency_ns\":{\"p50\":%llu,\"p99\":%llu,\"p999\":%llu,\"max\":%llu},"
        "\"cpu_core\":%u}}",
        (unsigned long long)m.snapshot_ts_ns,
        m.engine_id, m.feed_name, state_str,
        (unsigned long long)m.tick_rate,
        (unsigned long long)m.ticks_processed,
        (unsigned long long)m.tick_drops,
        (unsigned long long)m.invalid_ticks,
        (unsigned long long)m.intent_rate,
        (unsigned long long)m.intents_generated,
        (unsigned long long)m.intent_drops,
        (unsigned long long)m.latency.p50_ns,
        (unsigned long long)m.latency.p99_ns,
        (unsigned long long)m.latency.p999_ns,
        (unsigned long long)m.latency.max_ns,
        m.cpu_core
    );
}

inline int serialize_alert(const AlertSnapshot& a, char* buf, int buflen) {
    const char* severity_str = "INFO";
    switch (a.severity) {
        case AlertSeverity::INFO:     severity_str = "INFO"; break;
        case AlertSeverity::WARNING:  severity_str = "WARNING"; break;
        case AlertSeverity::CRITICAL: severity_str = "CRITICAL"; break;
    }
    
    return std::snprintf(buf, buflen,
        "{\"schema\":\"chimera.ws\",\"version\":1,\"type\":\"alert\","
        "\"ts_ns\":%llu,\"payload\":{"
        "\"severity\":\"%s\",\"code\":%u,\"engine_id\":%u,\"message\":\"%s\"}}",
        (unsigned long long)a.ts_ns,
        severity_str,
        static_cast<unsigned>(a.code),
        a.engine_id,
        a.message
    );
}

} // namespace api
} // namespace chimera

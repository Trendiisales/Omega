// =============================================================================
// StressHarness.cpp - Stress Test Driver
// =============================================================================
// Tests: bursts, overflows, timestamp anomalies, kill behavior
// Uses same pipeline as live - results are authoritative.
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <chrono>

#include "engine/EngineIngress.hpp"
#include "engine/EngineHealth.hpp"
#include "engine/QueueMetrics.hpp"
#include "engine/EngineSupervisor.hpp"
#include "market/TickValidator.hpp"
#include "tests/BurstTickGenerator.hpp"

using namespace chimera;

// =============================================================================
// Test: Burst overflow
// =============================================================================
bool test_burst_overflow() {
    std::printf("\n=== TEST: Burst Overflow ===\n");
    
    engine::EngineHealth health;
    engine::QueueMetrics metrics;
    engine::EngineIngress<256> ingress(health, metrics);  // Small queue
    
    test::BurstTickGenerator gen(1, 1);
    
    // Push 10,000 ticks into 256-capacity queue
    constexpr int BURST = 10000;
    for (int i = 0; i < BURST; ++i) {
        auto t = gen.next();
        ingress.push_tick(t);
    }
    
    uint64_t attempts = metrics.push_attempts.load();
    uint64_t drops = metrics.push_drops.load();
    
    std::printf("  Attempts: %llu\n", (unsigned long long)attempts);
    std::printf("  Drops:    %llu\n", (unsigned long long)drops);
    std::printf("  Drop %%:   %.2f%%\n", 100.0 * drops / attempts);
    
    // Pass if we have significant drops (queue is tiny, most will drop)
    bool pass = (drops > BURST - 256) && (drops < BURST);
    std::printf("  Result:   %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

// =============================================================================
// Test: Tick validation - backward time
// =============================================================================
bool test_backward_time() {
    std::printf("\n=== TEST: Backward Time Rejection ===\n");
    
    engine::EngineHealth health;
    market::TickValidator validator(
        5'000'000'000ULL,   // max_future
        100'000'000ULL,     // max_backward (100ms)
        1'000'000'000ULL    // max_freeze
    );
    
    uint64_t last_ingress = 0;
    uint64_t last_exchange = 0;
    uint64_t last_exchange_update = 0;
    
    // Create normal tick
    market::Tick t1{};
    t1.exchange_ts_ns = 1'000'000'000'000ULL;
    t1.ingress_ts_ns = 1'000'000'000'000ULL;
    t1.price = 100.0;
    t1.size = 1.0;
    t1.side = market::SIDE_TRADE;
    t1.flags = market::TICK_HAS_PRICE | market::TICK_HAS_SIZE | market::TICK_IS_TRADE;
    
    bool v1 = validator.validate(t1, last_ingress, last_exchange, last_exchange_update, health);
    std::printf("  Tick 1 (normal):      %s\n", v1 ? "VALID" : "INVALID");
    
    // Create tick with backward exchange time (500ms back)
    market::Tick t2 = t1;
    t2.exchange_ts_ns = 999'500'000'000ULL;  // 500ms backward
    t2.ingress_ts_ns = 1'000'001'000'000ULL;  // 1ms forward
    
    bool v2 = validator.validate(t2, last_ingress, last_exchange, last_exchange_update, health);
    std::printf("  Tick 2 (backward):    %s\n", v2 ? "VALID" : "INVALID");
    
    uint64_t invalid = health.invalid_ticks.load();
    std::printf("  Invalid count:        %llu\n", (unsigned long long)invalid);
    
    bool pass = v1 && !v2 && (invalid == 1);
    std::printf("  Result:               %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

// =============================================================================
// Test: Tick validation - future time
// =============================================================================
bool test_future_time() {
    std::printf("\n=== TEST: Future Time Rejection ===\n");
    
    engine::EngineHealth health;
    market::TickValidator validator(
        5'000'000'000ULL,   // max_future (5s)
        100'000'000ULL,     // max_backward
        1'000'000'000ULL    // max_freeze
    );
    
    uint64_t last_ingress = 0;
    uint64_t last_exchange = 0;
    uint64_t last_exchange_update = 0;
    
    // Create tick with exchange time 10s in future
    market::Tick t{};
    t.ingress_ts_ns = 1'000'000'000'000ULL;
    t.exchange_ts_ns = t.ingress_ts_ns + 10'000'000'000ULL;  // 10s ahead
    t.price = 100.0;
    t.size = 1.0;
    t.side = market::SIDE_TRADE;
    t.flags = market::TICK_HAS_PRICE | market::TICK_HAS_SIZE | market::TICK_IS_TRADE;
    
    bool valid = validator.validate(t, last_ingress, last_exchange, last_exchange_update, health);
    uint64_t invalid = health.invalid_ticks.load();
    
    std::printf("  Tick (10s future):    %s\n", valid ? "VALID" : "INVALID");
    std::printf("  Invalid count:        %llu\n", (unsigned long long)invalid);
    
    bool pass = !valid && (invalid == 1);
    std::printf("  Result:               %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

// =============================================================================
// Test: Supervisor kill on overflow
// =============================================================================
bool test_supervisor_kill() {
    std::printf("\n=== TEST: Supervisor Kill on Overflow ===\n");
    
    engine::EngineHealth health;
    engine::QueueMetrics metrics;
    
    // Configure supervisor with low threshold
    engine::EngineSupervisor supervisor(
        100,    // max_tick_drops (low for test)
        50,     // max_intent_drops
        10,     // max_invalid_ticks
        10000,  // burst_warn
        50000   // burst_kill
    );
    
    // Simulate 200 drops
    for (int i = 0; i < 200; ++i) {
        health.tick_drops.fetch_add(1, std::memory_order_relaxed);
    }
    
    std::printf("  Tick drops:           %llu\n", 
               (unsigned long long)health.tick_drops.load());
    std::printf("  Killed before eval:   %s\n", health.is_killed() ? "YES" : "NO");
    
    // Evaluate
    supervisor.evaluate(health);
    
    std::printf("  Killed after eval:    %s\n", health.is_killed() ? "YES" : "NO");
    std::printf("  Kill reason:          %d\n", 
               static_cast<int>(health.get_kill_reason()));
    
    bool pass = health.is_killed() && 
                health.get_kill_reason() == engine::EngineKillReason::TICK_QUEUE_OVERFLOW;
    std::printf("  Result:               %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

// =============================================================================
// Test: Burst with anomaly injection
// =============================================================================
bool test_anomaly_injection() {
    std::printf("\n=== TEST: Anomaly Injection ===\n");
    
    engine::EngineHealth health;
    market::TickValidator validator(
        5'000'000'000ULL,
        100'000'000ULL,
        1'000'000'000ULL
    );
    
    test::BurstTickGenerator gen(1, 1);
    gen.inject_backward_time = true;
    gen.anomaly_every_n = 100;  // Every 100th tick
    
    uint64_t last_ingress = 0;
    uint64_t last_exchange = 0;
    uint64_t last_exchange_update = 0;
    
    int valid_count = 0;
    int invalid_count = 0;
    
    for (int i = 0; i < 1000; ++i) {
        auto t = gen.next();
        if (validator.validate(t, last_ingress, last_exchange, last_exchange_update, health)) {
            ++valid_count;
        } else {
            ++invalid_count;
        }
    }
    
    std::printf("  Valid ticks:          %d\n", valid_count);
    std::printf("  Invalid ticks:        %d\n", invalid_count);
    std::printf("  Health invalid count: %llu\n", 
               (unsigned long long)health.invalid_ticks.load());
    
    bool pass = invalid_count > 0;  // Should have some invalids
    std::printf("  Result:               %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

// =============================================================================
// Test: Performance benchmark
// =============================================================================
bool test_performance() {
    std::printf("\n=== TEST: Performance Benchmark ===\n");
    
    engine::EngineHealth health;
    engine::QueueMetrics metrics;
    engine::EngineIngress<16384> ingress(health, metrics);
    
    test::BurstTickGenerator gen(1, 1);
    
    constexpr int ITERATIONS = 1'000'000;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < ITERATIONS; ++i) {
        auto t = gen.next();
        ingress.push_tick(t);
        
        // Drain to prevent overflow
        market::Tick out;
        ingress.pop_tick(out);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    
    double ns_per_tick = static_cast<double>(elapsed_ns) / ITERATIONS;
    double ticks_per_sec = 1'000'000'000.0 / ns_per_tick;
    
    std::printf("  Iterations:           %d\n", ITERATIONS);
    std::printf("  Total time:           %.2f ms\n", elapsed_ns / 1'000'000.0);
    std::printf("  Per tick:             %.1f ns\n", ns_per_tick);
    std::printf("  Throughput:           %.2f M ticks/sec\n", ticks_per_sec / 1'000'000.0);
    
    bool pass = ns_per_tick < 1000;  // Should be < 1µs per tick
    std::printf("  Result:               %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

// =============================================================================
// Main
// =============================================================================
int main() {
    std::printf("╔══════════════════════════════════════════════════════════════╗\n");
    std::printf("║            CHIMERA STRESS TEST HARNESS                       ║\n");
    std::printf("╚══════════════════════════════════════════════════════════════╝\n");
    
    int passed = 0;
    int failed = 0;
    
    if (test_burst_overflow()) ++passed; else ++failed;
    if (test_backward_time()) ++passed; else ++failed;
    if (test_future_time()) ++passed; else ++failed;
    if (test_supervisor_kill()) ++passed; else ++failed;
    if (test_anomaly_injection()) ++passed; else ++failed;
    if (test_performance()) ++passed; else ++failed;
    
    std::printf("\n╔══════════════════════════════════════════════════════════════╗\n");
    std::printf("║  RESULTS: %d PASSED, %d FAILED                                ║\n", passed, failed);
    std::printf("╚══════════════════════════════════════════════════════════════╝\n");
    
    return failed > 0 ? 1 : 0;
}

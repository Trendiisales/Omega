// =============================================================================
// VenueHealth.hpp - Unified Venue Health Interface (AUTHORITATIVE) v6.4
// =============================================================================
// This is the SINGLE SOURCE OF TRUTH for all venues (Binance, FIX, future).
//
// DESIGN GUARANTEES:
//   - POD (Plain Old Data)
//   - One cache-line cluster (align if needed)
//   - Atomics only
//   - No protocol logic inside
//   - No locks, no IO, no allocation
//
// PROTOCOL MAPPING:
//   Binance:
//     WS connect        → state = UNAVAILABLE
//     Snapshot received → state = DEGRADED
//     First live tick   → state = HEALTHY, last_good_rx_ns = now
//     WS disconnect     → state = UNAVAILABLE
//     Silent stall      → state = DEGRADED
//     REST send         → pending_orders++, last_tx_ns = now
//     REST ack          → pending_orders--
//     REST reject       → recent_rejects++
//
//   FIX:
//     Logon ACK         → state = DEGRADED
//     Stable heartbeats → state = HEALTHY
//     Heartbeat delay   → state = DEGRADED
//     ResendRequest     → protocol_errors++, state = DEGRADED
//     Reject burst      → recent_rejects++
//     Session timeout   → state = UNAVAILABLE
//     Rx message        → last_good_rx_ns = now
//
// ARBITER CONTRACT:
//   Arbiter NEVER checks "Binance" or "FIX".
//   Arbiter ONLY sees VenueHealth + Intent.
//   No protocol knowledge leaks upward.
// =============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <chrono>

namespace Chimera {

// =============================================================================
// VenueState - Canonical Three-State Model (per spec)
// =============================================================================
// HEALTHY     - Normal operation, all intents allowed
// DEGRADED    - Partial data / high risk, low urgency only
// UNAVAILABLE - No valid market view, NEVER trade
// =============================================================================
enum class VenueState : uint8_t {
    HEALTHY     = 0,   // Normal operation - all intents allowed
    DEGRADED    = 1,   // Partial data - low urgency only
    UNAVAILABLE = 2    // No valid market view - NEVER trade
};

// =============================================================================
// VenueHealth - Canonical Venue Health Model (per spec)
// =============================================================================
// This struct is updated ONLY by venue-specific code (Binance engine, FIX engine).
// Arbiter reads this via atomic loads only.
// =============================================================================
struct alignas(64) VenueHealth {
    // =========================================================================
    // Core state
    // =========================================================================
    std::atomic<VenueState> state{VenueState::UNAVAILABLE};
    
    // =========================================================================
    // Time signals (nanoseconds, monotonic clock)
    // =========================================================================
    std::atomic<uint64_t> last_good_rx_ns{0};  // Last valid data received
    std::atomic<uint64_t> last_tx_ns{0};       // Last message sent
    
    // =========================================================================
    // Latency tracking
    // =========================================================================
    std::atomic<uint64_t> latency_ewma_ns{0};        // EWMA latency
    std::atomic<uint64_t> latency_max_window_ns{0}; // Tail latency (p99 proxy)
    
    // =========================================================================
    // Execution pressure
    // =========================================================================
    std::atomic<uint32_t> pending_orders{0};   // In-flight orders
    std::atomic<uint32_t> recent_rejects{0};   // Rolling reject count
    
    // =========================================================================
    // Protocol-specific stress (venue-agnostic counter)
    // =========================================================================
    std::atomic<uint32_t> protocol_errors{0};  // Sequence gaps, resends, etc.
    
    // =========================================================================
    // Administrative
    // =========================================================================
    std::atomic<bool> kill_switch{false};      // Per-venue kill switch
    
    // =========================================================================
    // Configurable thresholds
    // =========================================================================
    static constexpr uint64_t STALE_DATA_NS = 20'000'000;        // 20ms staleness
    static constexpr uint32_t MAX_PENDING_ORDERS = 32;           // Backpressure limit
    static constexpr uint32_t MAX_RECENT_REJECTS = 8;            // Reject limit
    static constexpr uint32_t MAX_PROTOCOL_ERRORS = 3;           // Protocol error limit
    static constexpr uint64_t REJECT_DECAY_INTERVAL_NS = 100'000'000; // 100ms decay
    
    // =========================================================================
    // State setters
    // =========================================================================
    inline void setHealthy() noexcept {
        state.store(VenueState::HEALTHY, std::memory_order_release);
    }
    
    inline void setDegraded() noexcept {
        state.store(VenueState::DEGRADED, std::memory_order_release);
    }
    
    inline void setUnavailable() noexcept {
        state.store(VenueState::UNAVAILABLE, std::memory_order_release);
    }
    
    // Legacy aliases for backward compatibility
    inline void setBlind() noexcept { setUnavailable(); }
    inline void setDead() noexcept { setUnavailable(); }
    
    // =========================================================================
    // State getters
    // =========================================================================
    inline VenueState getState() const noexcept {
        return state.load(std::memory_order_acquire);
    }
    
    inline bool isHealthy() const noexcept {
        return state.load(std::memory_order_relaxed) == VenueState::HEALTHY;
    }
    
    inline bool isDegraded() const noexcept {
        return state.load(std::memory_order_relaxed) == VenueState::DEGRADED;
    }
    
    inline bool isUnavailable() const noexcept {
        return state.load(std::memory_order_relaxed) == VenueState::UNAVAILABLE;
    }
    
    inline const char* stateStr() const noexcept {
        switch (state.load(std::memory_order_relaxed)) {
            case VenueState::HEALTHY:     return "HEALTHY";
            case VenueState::DEGRADED:    return "DEGRADED";
            case VenueState::UNAVAILABLE: return "UNAVAILABLE";
            default:                      return "UNKNOWN";
        }
    }
    
    // =========================================================================
    // Data reception events
    // =========================================================================
    inline void onDataReceived(uint64_t now_ns) noexcept {
        last_good_rx_ns.store(now_ns, std::memory_order_relaxed);
    }
    
    inline void onLiveTick(uint64_t now_ns) noexcept {
        last_good_rx_ns.store(now_ns, std::memory_order_relaxed);
        // Auto-promote DEGRADED → HEALTHY on live data
        if (state.load(std::memory_order_relaxed) == VenueState::DEGRADED) {
            state.store(VenueState::HEALTHY, std::memory_order_release);
        }
    }
    
    // =========================================================================
    // Staleness detection
    // =========================================================================
    inline bool hasStaleData(uint64_t now_ns) const noexcept {
        uint64_t last_rx = last_good_rx_ns.load(std::memory_order_relaxed);
        if (last_rx == 0) return false;  // No data yet
        return (now_ns - last_rx) > STALE_DATA_NS;
    }
    
    inline bool checkAndHandleStaleness(uint64_t now_ns) noexcept {
        if (hasStaleData(now_ns)) {
            if (state.load(std::memory_order_relaxed) == VenueState::HEALTHY) {
                state.store(VenueState::DEGRADED, std::memory_order_release);
            }
            return true;  // Data is stale
        }
        return false;
    }
    
    // =========================================================================
    // Order lifecycle events
    // =========================================================================
    inline void onOrderSent(uint64_t now_ns) noexcept {
        pending_orders.fetch_add(1, std::memory_order_relaxed);
        last_tx_ns.store(now_ns, std::memory_order_relaxed);
    }
    
    inline void onOrderComplete() noexcept {
        uint32_t current = pending_orders.load(std::memory_order_relaxed);
        if (current > 0) {
            pending_orders.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    
    inline void onOrderReject() noexcept {
        uint32_t current = pending_orders.load(std::memory_order_relaxed);
        if (current > 0) {
            pending_orders.fetch_sub(1, std::memory_order_relaxed);
        }
        recent_rejects.fetch_add(1, std::memory_order_relaxed);
    }
    
    inline void onOrderFill() noexcept {
        onOrderComplete();
    }
    
    inline void onOrderCancel() noexcept {
        onOrderComplete();
    }
    
    // =========================================================================
    // Protocol error events
    // =========================================================================
    inline void onProtocolError() noexcept {
        protocol_errors.fetch_add(1, std::memory_order_relaxed);
        // Auto-degrade on protocol error
        if (state.load(std::memory_order_relaxed) == VenueState::HEALTHY) {
            state.store(VenueState::DEGRADED, std::memory_order_release);
        }
    }
    
    // =========================================================================
    // Latency tracking
    // =========================================================================
    inline void updateLatency(uint64_t latency_ns) noexcept {
        // EWMA update (alpha = 0.1)
        uint64_t current = latency_ewma_ns.load(std::memory_order_relaxed);
        uint64_t updated = (current == 0)
            ? latency_ns
            : static_cast<uint64_t>(0.9 * current + 0.1 * latency_ns);
        latency_ewma_ns.store(updated, std::memory_order_relaxed);
        
        // Track max (tail latency proxy)
        uint64_t max_lat = latency_max_window_ns.load(std::memory_order_relaxed);
        if (latency_ns > max_lat) {
            latency_max_window_ns.store(latency_ns, std::memory_order_relaxed);
        }
    }
    
    // =========================================================================
    // Periodic maintenance (call every ~100ms, NOT hot path)
    // =========================================================================
    inline void decayCounters() noexcept {
        // Decay reject counter
        uint32_t rejects = recent_rejects.load(std::memory_order_relaxed);
        if (rejects > 0) {
            recent_rejects.store(rejects / 2, std::memory_order_relaxed);
        }
        
        // Decay protocol errors
        uint32_t errors = protocol_errors.load(std::memory_order_relaxed);
        if (errors > 0) {
            protocol_errors.store(errors / 2, std::memory_order_relaxed);
        }
        
        // Reset tail latency window
        latency_max_window_ns.store(0, std::memory_order_relaxed);
    }
    
    // =========================================================================
    // Kill switch
    // =========================================================================
    inline void setKillSwitch(bool enabled) noexcept {
        kill_switch.store(enabled, std::memory_order_release);
    }
    
    inline bool isKilled() const noexcept {
        return kill_switch.load(std::memory_order_relaxed);
    }
    
    // =========================================================================
    // Full reset
    // =========================================================================
    inline void reset() noexcept {
        state.store(VenueState::UNAVAILABLE, std::memory_order_relaxed);
        last_good_rx_ns.store(0, std::memory_order_relaxed);
        last_tx_ns.store(0, std::memory_order_relaxed);
        latency_ewma_ns.store(0, std::memory_order_relaxed);
        latency_max_window_ns.store(0, std::memory_order_relaxed);
        pending_orders.store(0, std::memory_order_relaxed);
        recent_rejects.store(0, std::memory_order_relaxed);
        protocol_errors.store(0, std::memory_order_relaxed);
        kill_switch.store(false, std::memory_order_relaxed);
    }
    
    // =========================================================================
    // Debug/metrics helpers
    // =========================================================================
    inline uint64_t getLatencyEwmaNs() const noexcept {
        return latency_ewma_ns.load(std::memory_order_relaxed);
    }
    
    inline uint64_t getLatencyMaxNs() const noexcept {
        return latency_max_window_ns.load(std::memory_order_relaxed);
    }
    
    inline uint32_t getPendingOrders() const noexcept {
        return pending_orders.load(std::memory_order_relaxed);
    }
    
    inline uint32_t getRecentRejects() const noexcept {
        return recent_rejects.load(std::memory_order_relaxed);
    }
    
    inline uint32_t getProtocolErrors() const noexcept {
        return protocol_errors.load(std::memory_order_relaxed);
    }
    
    inline uint64_t getLastRxNs() const noexcept {
        return last_good_rx_ns.load(std::memory_order_relaxed);
    }
    
    inline uint64_t getLastTxNs() const noexcept {
        return last_tx_ns.load(std::memory_order_relaxed);
    }
};

// =============================================================================
// venue_allows - Venue-Agnostic Arbiter Rule (per spec)
// =============================================================================
// The Arbiter NEVER checks "Binance" or "FIX".
// It checks VenueHealth only.
//
// This function fully subsumes:
//   - Binance blind-mode
//   - FIX degraded-mode
//   - Execution backpressure
//   - Stale-data protection
// =============================================================================
struct Intent;  // Forward declaration

inline bool venue_allows(
    const VenueHealth& v,
    double urgency,
    double degraded_urgency_threshold,
    uint64_t now_ns,
    uint64_t max_latency_ns = 5'000'000  // 5ms default
) noexcept {
    // 1. Kill switch
    if (v.kill_switch.load(std::memory_order_relaxed)) {
        return false;
    }
    
    // 2. State check
    const VenueState s = v.state.load(std::memory_order_relaxed);
    
    if (s == VenueState::UNAVAILABLE) {
        return false;  // NEVER trade
    }
    
    if (s == VenueState::DEGRADED && urgency > degraded_urgency_threshold) {
        return false;  // High urgency blocked in DEGRADED
    }
    
    // 3. Backpressure: pending orders
    if (v.pending_orders.load(std::memory_order_relaxed) > VenueHealth::MAX_PENDING_ORDERS) {
        return false;
    }
    
    // 4. Backpressure: recent rejects
    if (v.recent_rejects.load(std::memory_order_relaxed) > VenueHealth::MAX_RECENT_REJECTS) {
        return false;
    }
    
    // 5. Staleness check
    uint64_t last_rx = v.last_good_rx_ns.load(std::memory_order_relaxed);
    if (last_rx > 0 && (now_ns - last_rx) > VenueHealth::STALE_DATA_NS) {
        return false;
    }
    
    // 6. Latency check (optional, for HEALTHY/DEGRADED)
    if (v.latency_ewma_ns.load(std::memory_order_relaxed) > max_latency_ns) {
        return false;
    }
    
    return true;
}

// =============================================================================
// Utility: Get current time in nanoseconds
// =============================================================================
inline uint64_t nowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace Chimera

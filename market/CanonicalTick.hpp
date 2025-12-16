// =============================================================================
// CanonicalTick.hpp - Single Source of Truth for Market Data
// =============================================================================
// HARD RULES:
// - Hot-path safe
// - Fixed layout (64 bytes, one cache line)
// - No allocation
// - No inheritance
// - No virtuals
// - No optional semantics without flags
// - Exchange timestamp is NEVER overwritten
// - Ingress timestamp is ALWAYS local monotonic time
//
// This is the ONLY tick type allowed to enter:
//   - engines
//   - strategies
//   - execution logic
// =============================================================================
#pragma once

#include <cstdint>
#include <cstddef>

namespace chimera {
namespace market {

// =============================================================================
// Tick Flags (bitmask)
// =============================================================================
enum TickFlags : uint8_t {
    TICK_HAS_PRICE      = 1u << 0,
    TICK_HAS_SIZE       = 1u << 1,
    TICK_IS_TRADE       = 1u << 2,
    TICK_IS_BOOK        = 1u << 3,
    TICK_IS_AGGRESSOR   = 1u << 4,
    TICK_IS_SNAPSHOT    = 1u << 5
};

// =============================================================================
// Side encoding
// =============================================================================
enum TickSide : uint8_t {
    SIDE_BID   = 0,
    SIDE_ASK   = 1,
    SIDE_TRADE = 2
};

// =============================================================================
// CanonicalTick
// =============================================================================
struct alignas(64) CanonicalTick final {
    // --- Time (16 bytes) ---
    uint64_t exchange_ts_ns;   // Timestamp from venue (ns)
    uint64_t ingress_ts_ns;    // Local monotonic ingress time (ns)

    // --- Price / Size (16 bytes) ---
    double   price;            // Trade price OR book price
    double   size;             // Trade size OR book size

    // --- Identity (8 bytes) ---
    uint32_t symbol_id;        // Pre-mapped symbol id (dense)
    uint16_t venue;            // Venue id (BINANCE=1, FIX=2, etc.)
    uint8_t  side;             // TickSide
    uint8_t  flags;            // TickFlags bitmask

    // --- Padding to 64 bytes (24 bytes) ---
    uint8_t  _pad[24];
};

// =============================================================================
// Compile-time guarantees
// =============================================================================
static_assert(sizeof(CanonicalTick) == 64,
              "CanonicalTick must be exactly one cache line (64 bytes)");

static_assert(alignof(CanonicalTick) == 64,
              "CanonicalTick must be cache-line aligned");

// =============================================================================
// Helper predicates (inline, no branching cost)
// =============================================================================
inline bool is_trade(const CanonicalTick& t) noexcept {
    return (t.flags & TICK_IS_TRADE) != 0;
}

inline bool is_book(const CanonicalTick& t) noexcept {
    return (t.flags & TICK_IS_BOOK) != 0;
}

inline bool has_price(const CanonicalTick& t) noexcept {
    return (t.flags & TICK_HAS_PRICE) != 0;
}

inline bool has_size(const CanonicalTick& t) noexcept {
    return (t.flags & TICK_HAS_SIZE) != 0;
}

inline bool is_aggressor(const CanonicalTick& t) noexcept {
    return (t.flags & TICK_IS_AGGRESSOR) != 0;
}

} // namespace market
} // namespace chimera

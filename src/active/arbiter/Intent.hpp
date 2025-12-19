// =============================================================================
// Intent.hpp - Strategy → Arbiter Contract v6.4
// =============================================================================
// GUARANTEES:
//   - POD (Plain Old Data)
//   - No heap allocation
//   - Deterministic copy cost
//
// v6.4 CHANGES:
//   - Added venue preference (optional)
//   - Strategy can suggest preferred venue, Arbiter makes final decision
// =============================================================================
#pragma once
#include <cstdint>
#include "../strategy/Decision.hpp"  // Use existing Side enum
#include "../market/TickFull.hpp"    // Use existing Venue enum

namespace Chimera {

struct Intent {
    uint16_t symbol_id;      // unified symbol id
    Side     side;           // buy / sell (uses existing Side enum)
    double   size;           // absolute size
    double   urgency;        // 0.0 → 1.0
    double   confidence;     // 0.0 → 1.0
    uint64_t ts_ns;          // intent creation time
    Venue    venue;          // v6.4: preferred venue (UNKNOWN = no preference)
    
    // Default constructor
    Intent() 
        : symbol_id(0)
        , side(Side::None)
        , size(0.0)
        , urgency(0.0)
        , confidence(0.0)
        , ts_ns(0)
        , venue(Venue::UNKNOWN)
    {}
    
    // Constructor without venue (backward compatible)
    Intent(uint16_t sym, Side s, double sz, double urg, double conf, uint64_t ts)
        : symbol_id(sym)
        , side(s)
        , size(sz)
        , urgency(urg)
        , confidence(conf)
        , ts_ns(ts)
        , venue(Venue::UNKNOWN)
    {}
    
    // Constructor with venue preference
    Intent(uint16_t sym, Side s, double sz, double urg, double conf, uint64_t ts, Venue v)
        : symbol_id(sym)
        , side(s)
        , size(sz)
        , urgency(urg)
        , confidence(conf)
        , ts_ns(ts)
        , venue(v)
    {}
    
    // Helpers
    inline bool isBuy() const noexcept { return side == Side::Buy; }
    inline bool isSell() const noexcept { return side == Side::Sell; }
    inline bool isValid() const noexcept { return size > 0.0 && confidence > 0.0 && side != Side::None; }
    inline bool hasVenuePreference() const noexcept { return venue != Venue::UNKNOWN; }
};

// Compile-time verification
static_assert(sizeof(Intent) <= 56, "Intent must be small for cache efficiency");

} // namespace Chimera

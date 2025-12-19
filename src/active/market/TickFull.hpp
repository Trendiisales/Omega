// =============================================================================
// TickFull.hpp - Unified Tick Structure for Both Engines
// =============================================================================
// SPECIFICATION:
//   - Used by BOTH CryptoEngine and CfdEngine
//   - NO conditional logic based on venue
//   - Fixed-size, cache-line aligned for HFT
//   - No heap allocation
//
// KNOWN-GOOD FIELDS (from audit):
//   - ts_ns        : Nanosecond timestamp
//   - bid / ask    : Best bid/ask prices
//   - last / last_size : Last trade price/size
//   - venue        : 1=Binance, 2=cTrader
//   - symbol       : Fixed 16-char symbol
//   - flags        : Tick flags (BBO update, trade, etc.)
// =============================================================================
#pragma once

#include <cstdint>
#include <cstring>

namespace Chimera {

// Venue identifiers
enum class Venue : uint8_t {
    UNKNOWN = 0,
    BINANCE = 1,
    CTRADER = 2
};

// Tick flags
enum TickFlags : uint8_t {
    TICK_FLAG_NONE       = 0x00,
    TICK_FLAG_BBO_UPDATE = 0x01,  // Best bid/offer changed
    TICK_FLAG_TRADE      = 0x02,  // Trade occurred
    TICK_FLAG_DEPTH      = 0x04,  // Depth update
    TICK_FLAG_SNAPSHOT   = 0x08,  // Full snapshot (not incremental)
    TICK_FLAG_STALE      = 0x10,  // Data may be stale
    TICK_FLAG_GAP        = 0x20   // Sequence gap detected
};

// =============================================================================
// TickFull - The One True Tick Structure
// =============================================================================
// Size: 128 bytes (2 cache lines, but aligned to 64 for atomic access)
// =============================================================================
struct alignas(64) TickFull {
    // =========================================================================
    // IDENTITY (16 bytes)
    // =========================================================================
    char symbol[16];              // Fixed-size symbol (e.g., "BTCUSDT", "XAUUSD")
    
    // =========================================================================
    // TIMING (16 bytes)
    // =========================================================================
    uint64_t ts_ns;               // Nanosecond timestamp (monotonic)
    uint64_t ts_exchange;         // Exchange timestamp (if available)
    
    // =========================================================================
    // PRICES (32 bytes) - All doubles for precision
    // =========================================================================
    double bid;                   // Best bid price
    double ask;                   // Best ask price
    double last;                  // Last trade price
    double last_size;             // Last trade size
    
    // =========================================================================
    // SIZES (32 bytes)
    // =========================================================================
    double bid_size;              // Best bid size
    double ask_size;              // Best ask size
    double buy_vol;               // Buy volume (trade flow)
    double sell_vol;              // Sell volume (trade flow)
    
    // =========================================================================
    // DEPTH (40 bytes) - Top 5 levels
    // =========================================================================
    double bid_depth[5];          // Bid depth levels 1-5
    double ask_depth[5];          // Ask depth levels 1-5
    
    // =========================================================================
    // METADATA (8 bytes)
    // =========================================================================
    Venue venue;                  // 1=Binance, 2=cTrader
    uint8_t flags;                // TickFlags bitmap
    uint16_t symbol_id;           // Numeric symbol ID (for fast lookup)
    uint32_t sequence;            // Sequence number (per-venue)
    
    // =========================================================================
    // PADDING to 64-byte boundary
    // =========================================================================
    uint8_t _pad[8];
    
    // =========================================================================
    // CONSTRUCTORS
    // =========================================================================
    TickFull() {
        std::memset(this, 0, sizeof(TickFull));
    }
    
    TickFull(const char* sym, Venue v) : TickFull() {
        setSymbol(sym);
        venue = v;
    }
    
    // =========================================================================
    // HELPERS (inline for HFT)
    // =========================================================================
    inline void setSymbol(const char* sym) {
        std::strncpy(symbol, sym, 15);
        symbol[15] = '\0';
    }
    
    inline double mid() const {
        return (bid + ask) * 0.5;
    }
    
    inline double spread() const {
        return ask - bid;
    }
    
    inline double spreadBps() const {
        double m = mid();
        return m > 0.0 ? (spread() / m) * 10000.0 : 0.0;
    }
    
    inline double imbalance() const {
        double total = bid_size + ask_size;
        return total > 0.0 ? (bid_size - ask_size) / total : 0.0;
    }
    
    inline double totalBidDepth() const {
        return bid_depth[0] + bid_depth[1] + bid_depth[2] + bid_depth[3] + bid_depth[4];
    }
    
    inline double totalAskDepth() const {
        return ask_depth[0] + ask_depth[1] + ask_depth[2] + ask_depth[3] + ask_depth[4];
    }
    
    inline double depthImbalance() const {
        double bd = totalBidDepth();
        double ad = totalAskDepth();
        double total = bd + ad;
        return total > 0.0 ? (bd - ad) / total : 0.0;
    }
    
    inline bool isBinance() const { return venue == Venue::BINANCE; }
    inline bool isCTrader() const { return venue == Venue::CTRADER; }
    
    inline bool hasBBO() const { return flags & TICK_FLAG_BBO_UPDATE; }
    inline bool hasTrade() const { return flags & TICK_FLAG_TRADE; }
    inline bool hasDepth() const { return flags & TICK_FLAG_DEPTH; }
    inline bool isStale() const { return flags & TICK_FLAG_STALE; }
};

// Verify size at compile time
static_assert(sizeof(TickFull) == 192, "TickFull must be 192 bytes (3 cache lines)");
static_assert(alignof(TickFull) == 64, "TickFull must be 64-byte aligned");

} // namespace Chimera

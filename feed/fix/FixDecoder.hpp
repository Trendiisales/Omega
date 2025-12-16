// =============================================================================
// FixDecoder.hpp - FIX Message Decoder
// =============================================================================
// Decodes raw FIX messages into CanonicalTick.
// Used by FixSession for market data and execution reports.
// =============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include "market/MarketTypes.hpp"
#include "core/MonotonicClock.hpp"

namespace chimera {
namespace feed {
namespace fix {

// =============================================================================
// FixDecoder
// =============================================================================
class FixDecoder {
public:
    explicit FixDecoder(uint16_t venue_id) noexcept
        : venue_(venue_id)
    {}

    // Decode a FIX market data snapshot (35=W) into a tick
    // Returns true if tick was populated
    bool decode_market_data(const char* msg,
                           std::size_t len,
                           chimera::market::Tick& out) noexcept
    {
        if (!msg || len == 0) return false;
        
        // Initialize tick
        out.exchange_ts_ns = 0;
        out.ingress_ts_ns = chimera::core::MonotonicClock::now_ns();
        out.price = 0.0;
        out.size = 0.0;
        out.symbol_id = 0;
        out.venue = venue_;
        out.side = chimera::market::SIDE_BID;
        out.flags = 0;
        for (int i = 0; i < 24; ++i) out._pad[i] = 0;
        
        // Parse symbol (55=)
        const char* sym = find_tag(msg, len, "55=");
        if (sym) {
            out.symbol_id = hash_symbol(sym);
        }
        
        // Parse MDEntryType (269=) - 0=Bid, 1=Offer
        const char* entry_type = find_tag(msg, len, "269=");
        if (entry_type) {
            out.side = (entry_type[0] == '0') ? 
                chimera::market::SIDE_BID : chimera::market::SIDE_ASK;
        }
        
        // Parse MDEntryPx (270=)
        const char* price = find_tag(msg, len, "270=");
        if (price) {
            out.price = std::atof(price);
            out.flags |= chimera::market::TICK_HAS_PRICE;
        }
        
        // Parse MDEntrySize (271=)
        const char* size = find_tag(msg, len, "271=");
        if (size) {
            out.size = std::atof(size);
            out.flags |= chimera::market::TICK_HAS_SIZE;
        }
        
        out.flags |= chimera::market::TICK_IS_BOOK;
        
        return (out.flags & chimera::market::TICK_HAS_PRICE) != 0;
    }

    // Decode a FIX execution report (35=8) into a tick
    bool decode_execution(const char* msg,
                          std::size_t len,
                          chimera::market::Tick& out) noexcept
    {
        if (!msg || len == 0) return false;
        
        // Initialize tick
        out.exchange_ts_ns = 0;
        out.ingress_ts_ns = chimera::core::MonotonicClock::now_ns();
        out.price = 0.0;
        out.size = 0.0;
        out.symbol_id = 0;
        out.venue = venue_;
        out.side = chimera::market::SIDE_TRADE;
        out.flags = 0;
        for (int i = 0; i < 24; ++i) out._pad[i] = 0;
        
        // Parse symbol (55=)
        const char* sym = find_tag(msg, len, "55=");
        if (sym) {
            out.symbol_id = hash_symbol(sym);
        }
        
        // Parse ExecType (150=) - F=Trade (Fill)
        const char* exec_type = find_tag(msg, len, "150=");
        if (!exec_type || (exec_type[0] != 'F' && exec_type[0] != '1' && exec_type[0] != '2')) {
            return false; // Not a fill
        }
        
        // Parse Side (54=) - 1=Buy, 2=Sell
        const char* side = find_tag(msg, len, "54=");
        if (side) {
            // For fills, we mark as trade with aggressor flag based on side
            out.flags |= chimera::market::TICK_IS_AGGRESSOR;
        }
        
        // Parse LastPx (31=)
        const char* last_px = find_tag(msg, len, "31=");
        if (last_px) {
            out.price = std::atof(last_px);
            out.flags |= chimera::market::TICK_HAS_PRICE;
        }
        
        // Parse LastQty (32=)
        const char* last_qty = find_tag(msg, len, "32=");
        if (last_qty) {
            out.size = std::atof(last_qty);
            out.flags |= chimera::market::TICK_HAS_SIZE;
        }
        
        out.flags |= chimera::market::TICK_IS_TRADE;
        
        return (out.flags & chimera::market::TICK_HAS_PRICE) != 0;
    }

private:
    // Find a FIX tag value (returns pointer after '=', null-terminated by SOH)
    static const char* find_tag(const char* msg, std::size_t len, const char* tag) noexcept {
        const char* p = msg;
        const char* end = msg + len;
        std::size_t tag_len = std::strlen(tag);
        
        while (p < end - tag_len) {
            // Tags are preceded by SOH (0x01) or at start
            if ((p == msg || *(p-1) == '\x01') && 
                std::strncmp(p, tag, tag_len) == 0) {
                return p + tag_len;
            }
            ++p;
        }
        return nullptr;
    }
    
    // Simple hash for symbol string
    static uint32_t hash_symbol(const char* sym) noexcept {
        uint32_t hash = 0;
        while (*sym && *sym != '\x01') {
            hash = hash * 31 + static_cast<uint8_t>(*sym);
            ++sym;
        }
        return hash;
    }

    uint16_t venue_;
};

} // namespace fix
} // namespace feed
} // namespace chimera

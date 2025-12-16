// =============================================================================
// BinanceTradeNormalizer.hpp - Binance → CanonicalTick Converter
// =============================================================================
// No allocation.
// No branching beyond flags.
// =============================================================================
#pragma once

#include <cstdint>
#include "market/MarketTypes.hpp"

namespace chimera {
namespace feed {
namespace binance {

// =============================================================================
// BinanceTradeNormalizer
// =============================================================================
class BinanceTradeNormalizer final {
public:
    explicit BinanceTradeNormalizer(uint16_t venue_id) noexcept
        : venue_(venue_id) {}

    inline void normalize_trade(
        uint32_t symbol_id,
        uint64_t exchange_ts_ns,
        uint64_t ingress_ts_ns,
        double price,
        double size,
        bool is_buyer_maker,
        chimera::market::Tick& out
    ) const noexcept
    {
        out.exchange_ts_ns = exchange_ts_ns;
        out.ingress_ts_ns  = ingress_ts_ns;

        out.price = price;
        out.size  = size;

        out.symbol_id = symbol_id;
        out.venue     = venue_;

        out.side  = chimera::market::SIDE_TRADE;
        out.flags =
            chimera::market::TICK_HAS_PRICE |
            chimera::market::TICK_HAS_SIZE  |
            chimera::market::TICK_IS_TRADE  |
            (is_buyer_maker ? 0 : chimera::market::TICK_IS_AGGRESSOR);
        
        // Zero padding
        for (int i = 0; i < 24; ++i) out._pad[i] = 0;
    }

    inline void normalize_book(
        uint32_t symbol_id,
        uint64_t exchange_ts_ns,
        uint64_t ingress_ts_ns,
        double price,
        double size,
        bool is_bid,
        chimera::market::Tick& out
    ) const noexcept
    {
        out.exchange_ts_ns = exchange_ts_ns;
        out.ingress_ts_ns  = ingress_ts_ns;

        out.price = price;
        out.size  = size;

        out.symbol_id = symbol_id;
        out.venue     = venue_;

        out.side  = is_bid ? chimera::market::SIDE_BID : chimera::market::SIDE_ASK;
        out.flags =
            chimera::market::TICK_HAS_PRICE |
            chimera::market::TICK_HAS_SIZE  |
            chimera::market::TICK_IS_BOOK;
        
        // Zero padding
        for (int i = 0; i < 24; ++i) out._pad[i] = 0;
    }

private:
    uint16_t venue_;
};

} // namespace binance
} // namespace feed
} // namespace chimera

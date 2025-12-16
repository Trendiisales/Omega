// =============================================================================
// TradeFeed.hpp - Binance Trade Feed
// =============================================================================
// Isolated feed that pushes to EngineIngress.
// WebSocket connection management here.
// =============================================================================
#pragma once

#include <cstdint>
#include <atomic>
#include "market/MarketTypes.hpp"
#include "engine/EngineIngress.hpp"
#include "feed/binance/BinanceTradeNormalizer.hpp"
#include "core/MonotonicClock.hpp"

namespace chimera {
namespace feed {
namespace binance {

// =============================================================================
// TradeFeed
// =============================================================================
template <std::size_t IngressQ = 16384>
class TradeFeed {
public:
    TradeFeed(chimera::engine::EngineIngress<IngressQ>& ingress,
              uint16_t venue_id) noexcept
        : ingress_(ingress)
        , normalizer_(venue_id)
        , running_(false)
        , trades_received_(0)
    {}

    void start() noexcept {
        running_.store(true, std::memory_order_release);
    }

    void stop() noexcept {
        running_.store(false, std::memory_order_release);
    }

    bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    // Called from WebSocket callback
    inline void on_trade(uint32_t symbol_id,
                        uint64_t exchange_ts_ns,
                        double price,
                        double size,
                        bool is_buyer_maker) noexcept
    {
        if (!running_.load(std::memory_order_acquire)) return;

        chimera::market::Tick tick;
        const uint64_t ingress_ts = chimera::core::MonotonicClock::now_ns();

        normalizer_.normalize_trade(
            symbol_id,
            exchange_ts_ns,
            ingress_ts,
            price,
            size,
            is_buyer_maker,
            tick
        );

        ingress_.push_tick(tick);
        ++trades_received_;
    }

    uint64_t trades_received() const noexcept { return trades_received_; }

private:
    chimera::engine::EngineIngress<IngressQ>& ingress_;
    BinanceTradeNormalizer normalizer_;
    std::atomic<bool> running_;
    uint64_t trades_received_;
};

} // namespace binance
} // namespace feed
} // namespace chimera

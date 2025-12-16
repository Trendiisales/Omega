// =============================================================================
// BookFeed.hpp - Binance Order Book Feed
// =============================================================================
// Isolated feed that pushes to EngineIngress.
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
// BookFeed
// =============================================================================
template <std::size_t IngressQ = 16384>
class BookFeed {
public:
    BookFeed(chimera::engine::EngineIngress<IngressQ>& ingress,
             uint16_t venue_id) noexcept
        : ingress_(ingress)
        , normalizer_(venue_id)
        , running_(false)
        , updates_received_(0)
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
    inline void on_book_update(uint32_t symbol_id,
                               uint64_t exchange_ts_ns,
                               double price,
                               double size,
                               bool is_bid) noexcept
    {
        if (!running_.load(std::memory_order_acquire)) return;

        chimera::market::Tick tick;
        const uint64_t ingress_ts = chimera::core::MonotonicClock::now_ns();

        normalizer_.normalize_book(
            symbol_id,
            exchange_ts_ns,
            ingress_ts,
            price,
            size,
            is_bid,
            tick
        );

        ingress_.push_tick(tick);
        ++updates_received_;
    }

    uint64_t updates_received() const noexcept { return updates_received_; }

private:
    chimera::engine::EngineIngress<IngressQ>& ingress_;
    BinanceTradeNormalizer normalizer_;
    std::atomic<bool> running_;
    uint64_t updates_received_;
};

} // namespace binance
} // namespace feed
} // namespace chimera

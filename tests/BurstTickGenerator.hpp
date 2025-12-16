// =============================================================================
// BurstTickGenerator.hpp - Stress Test Tick Generator
// =============================================================================
// Generates high-rate ticks for stress testing.
// Can simulate: bursts, timestamp lies, backward time, frozen clocks.
// =============================================================================
#pragma once

#include <cstdint>
#include "market/MarketTypes.hpp"

namespace chimera {
namespace test {

struct BurstTickGenerator {
    uint32_t symbol_id;
    uint16_t venue;
    double   price;
    double   size;
    uint64_t exchange_ts;
    uint64_t ingress_ts;
    uint64_t seq;

    // Anomaly injection
    bool inject_backward_time;
    bool inject_frozen_exchange;
    bool inject_future_time;
    uint64_t anomaly_every_n;

    BurstTickGenerator(uint32_t s, uint16_t v) noexcept
        : symbol_id(s)
        , venue(v)
        , price(100.0)
        , size(1.0)
        , exchange_ts(1'000'000'000'000ULL)  // Start at 1 second
        , ingress_ts(1'000'000'000'000ULL)
        , seq(0)
        , inject_backward_time(false)
        , inject_frozen_exchange(false)
        , inject_future_time(false)
        , anomaly_every_n(0)
    {}

    inline chimera::market::Tick next() noexcept {
        chimera::market::Tick t{};

        // Normal time progression
        ingress_ts += 1'000;  // 1 µs
        exchange_ts += 1'000;

        // Inject anomalies if configured
        if (anomaly_every_n > 0 && seq % anomaly_every_n == 0 && seq > 0) {
            if (inject_backward_time) {
                exchange_ts -= 500'000'000;  // Jump back 500ms
            }
            if (inject_frozen_exchange) {
                exchange_ts -= 1'000;  // Don't advance exchange time
            }
            if (inject_future_time) {
                exchange_ts = ingress_ts + 10'000'000'000ULL;  // 10s in future
            }
        }

        // Price walk
        price += 0.01 * ((seq % 3) - 1);  // -0.01, 0, +0.01

        t.symbol_id = symbol_id;
        t.venue = venue;
        t.price = price;
        t.size = size;
        t.side = chimera::market::SIDE_TRADE;
        t.flags = chimera::market::TICK_HAS_PRICE |
                  chimera::market::TICK_HAS_SIZE  |
                  chimera::market::TICK_IS_TRADE;
        t.exchange_ts_ns = exchange_ts;
        t.ingress_ts_ns = ingress_ts;

        ++seq;
        return t;
    }

    void reset() noexcept {
        exchange_ts = 1'000'000'000'000ULL;
        ingress_ts = 1'000'000'000'000ULL;
        seq = 0;
        price = 100.0;
    }
};

} // namespace test
} // namespace chimera

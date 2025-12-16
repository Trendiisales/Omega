// =============================================================================
// NullExecutionRouter.hpp - Dry-Run Execution Router
// =============================================================================
// Logs intents but never sends to exchange.
// Compile-time safety: if CHIMERA_LIVE is OFF, this is the only router.
// =============================================================================
#pragma once

#include <atomic>
#include "engine/IExecutionRouter.hpp"
#include "engine/Intent.hpp"
#include "core/Logger.hpp"

namespace chimera {
namespace engine {

class NullExecutionRouter final : public IExecutionRouter {
public:
    explicit NullExecutionRouter(core::Logger* logger = nullptr) noexcept
        : logger_(logger)
        , intents_sent_(0)
        , intents_dropped_(0)
    {}

    bool send(const Intent& intent) noexcept override {
        intents_sent_.fetch_add(1, std::memory_order_relaxed);
        
        // Log intent for analysis (cold path acceptable here)
        if (logger_) {
            logger_->log(
                intent.ts_ns,
                0,
                core::LogLevel::INFO,
                0xDADA,  // Shadow intent marker
                static_cast<uint64_t>(intent.symbol_id),
                static_cast<uint64_t>(intent.side),
                static_cast<uint64_t>(intent.price * 100)
            );
        }
        
        // Always "succeeds" - never actually sends
        return true;
    }

    uint64_t intents_sent() const noexcept override {
        return intents_sent_.load(std::memory_order_relaxed);
    }

    uint64_t intents_dropped() const noexcept override {
        return intents_dropped_.load(std::memory_order_relaxed);
    }

    uint64_t intents_filled() const noexcept override {
        return 0;  // Shadow mode never has fills
    }

private:
    core::Logger* logger_;
    std::atomic<uint64_t> intents_sent_;
    std::atomic<uint64_t> intents_dropped_;
};

} // namespace engine
} // namespace chimera

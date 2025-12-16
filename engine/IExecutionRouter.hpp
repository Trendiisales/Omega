// =============================================================================
// IExecutionRouter.hpp - Execution Router Interface
// =============================================================================
// Abstract interface for execution routing.
// Implementations: NullExecutionRouter (dry-run), FixExecutionRouter (live)
// =============================================================================
#pragma once

#include "engine/Intent.hpp"

namespace chimera {
namespace engine {

class IExecutionRouter {
public:
    virtual ~IExecutionRouter() = default;
    
    // Send intent to execution venue
    // Returns true if accepted, false if dropped
    virtual bool send(const Intent& intent) noexcept = 0;
    
    // Get execution stats
    virtual uint64_t intents_sent() const noexcept = 0;
    virtual uint64_t intents_dropped() const noexcept = 0;
    virtual uint64_t intents_filled() const noexcept = 0;
};

} // namespace engine
} // namespace chimera

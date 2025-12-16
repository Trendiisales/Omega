// =============================================================================
// IntentQueue.hpp - Engine → Execution Boundary
// =============================================================================
// SPSC only. No allocation.
// =============================================================================
#pragma once

#include "core/SPSCQueue.hpp"
#include "engine/Intent.hpp"

namespace chimera {
namespace engine {

// Default capacity: 4096 intents
template <std::size_t Capacity = 4096>
using IntentQueue = chimera::core::SPSCQueue<Intent, Capacity>;

} // namespace engine
} // namespace chimera

// =============================================================================
// EngineConfig.hpp - Immutable Engine Configuration
// =============================================================================
#pragma once

#include <cstdint>

namespace chimera {
namespace engine {

// =============================================================================
// EngineConfig - Immutable, bound at construction
// =============================================================================
struct EngineConfig {
    uint32_t engine_id;        // unique per feed
    uint32_t symbol_count;     // number of symbols handled
    uint32_t ingress_queue_sz; // power of two
    uint32_t intent_queue_sz;  // power of two
    uint32_t cpu_core;         // CPU core for this engine
};

} // namespace engine
} // namespace chimera

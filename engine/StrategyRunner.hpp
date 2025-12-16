// =============================================================================
// StrategyRunner.hpp - CRTP-Compatible Strategy Runner
// =============================================================================
// Wraps a strategy for use in EngineCore.
// No virtuals.
// =============================================================================
#pragma once

#include <cstddef>
#include "market/MarketTypes.hpp"
#include "engine/Intent.hpp"

namespace chimera {
namespace engine {

// =============================================================================
// StrategyRunner
// =============================================================================
template <typename StrategyT>
class StrategyRunner {
public:
    explicit StrategyRunner(StrategyT& strat) noexcept
        : strat_(strat) {}

    inline bool on_tick(const chimera::market::Tick& t,
                        Intent& out) noexcept
    {
        return strat_.on_tick(t, out);
    }

private:
    StrategyT& strat_;
};

} // namespace engine
} // namespace chimera

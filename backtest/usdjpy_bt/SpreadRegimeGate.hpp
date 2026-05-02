#pragma once
// =============================================================================
// SpreadRegimeGate.hpp -- BACKTEST STUB
// -----------------------------------------------------------------------------
// Minimal stand-in for the production SpreadRegimeGate header. Provides
// matching method signatures but with no-op spread tracking and
// always-allow can_fire(). Real spread filtering already happens in the
// engine itself via MAX_SPREAD; the regime gate adds a 1-hour adaptive
// threshold which is unnecessary for a 14-month sweep on clean HistData.
// Production header is untouched.
// =============================================================================

#include <cstdint>

namespace omega {

class SpreadRegimeGate {
public:
    void on_tick(int64_t /*now_ms*/, double /*spread*/) noexcept {}
    bool can_fire() const noexcept { return true; }
};

} // namespace omega

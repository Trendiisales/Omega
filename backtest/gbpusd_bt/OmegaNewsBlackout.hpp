#pragma once
// =============================================================================
// OmegaNewsBlackout.hpp -- BACKTEST STUB (GBPUSD)
// -----------------------------------------------------------------------------
// No-op stub matching the production OmegaNewsBlackout signature.
// =============================================================================

#include <string>
#include <cstdint>

namespace omega {
namespace news {

class NewsBlackout {
public:
    bool is_blocked(const std::string& /*sym*/, int64_t /*now_s*/) const noexcept {
        return false;
    }
};

} // namespace news
} // namespace omega

static omega::news::NewsBlackout g_news_blackout;

#pragma once
// =============================================================================
// OmegaNewsBlackout.hpp -- BACKTEST STUB
// -----------------------------------------------------------------------------
// Minimal stand-in for the production OmegaNewsBlackout. The production
// header has an unguarded _mkgmtime reference (Windows-only platform call)
// that breaks Linux builds. For the historical backtest we don't have a
// time-aligned NFP/CPI/BoJ calendar anyway -- the 14-month sweep treats
// blackout as off. Live promotion uses the real header on Windows.
// Production header is untouched.
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

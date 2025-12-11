#pragma once
#include <string>
#include <cstdint>
#include "FIXMDOrderBook.hpp"
#include "../../data/UnifiedTick.hpp"

namespace Omega {

class FIXMDNormalizer {
public:
    FIXMDNormalizer();
    ~FIXMDNormalizer();

    UnifiedTick normalize(const FIXMDBook& book,
                          const std::string& symbol);

private:
    uint64_t tsLocal();
};

} // namespace Omega

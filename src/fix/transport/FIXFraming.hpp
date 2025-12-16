#pragma once
#include <string>

namespace Omega {

class FIXFraming {
public:
    static std::string normalizeDelimiters(const std::string& s);
    static std::string restoreSOH(const std::string& s);
};

} // namespace Omega

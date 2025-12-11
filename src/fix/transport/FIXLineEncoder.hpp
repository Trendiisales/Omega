#pragma once
#include <string>

namespace Omega {

class FIXLineEncoder {
public:
    FIXLineEncoder();
    ~FIXLineEncoder() = default;

    std::string encode(const std::string& raw);   // convert '|' to SOH, ensure trailing SOH
    std::string decode(const std::string& wire);  // restore printable format

private:
};

} // namespace Omega

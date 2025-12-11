#pragma once
#include <string>
#include <cstdint>

namespace Omega {

class FIXCodec {
public:
    FIXCodec();
    ~FIXCodec() = default;

    std::string injectChecksum(const std::string& msg);
    uint32_t computeChecksum(const std::string& msg);

    std::string stampSendingTime(const std::string& msg, const std::string& ts);

private:
    static void replaceTag(std::string& msg, const std::string& tag, const std::string& value);
};

} // namespace Omega

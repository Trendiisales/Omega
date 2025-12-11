#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace Omega {

class HMAC {
public:
    static std::vector<uint8_t> sha256(const std::vector<uint8_t>& data);
    static std::string hmac_sha256(const std::string& key, const std::string& msg);
};

}

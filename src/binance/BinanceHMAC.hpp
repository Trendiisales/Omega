#pragma once
#include <string>
#include <cstdint>
#include <vector>

namespace Omega {

class BinanceHMAC {
public:
    BinanceHMAC();
    ~BinanceHMAC();

    void setSecret(const std::string& key);
    std::string sign(const std::string& msg);
    
    bool hasSecret() const;

private:
    std::vector<uint8_t> secret;
};

} // namespace Omega

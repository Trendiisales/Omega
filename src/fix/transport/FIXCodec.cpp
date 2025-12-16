#include "FIXCodec.hpp"
#include <sstream>
#include <iomanip>

namespace Omega {

FIXCodec::FIXCodec() {}

uint32_t FIXCodec::computeChecksum(const std::string& msg) {
    uint32_t sum = 0;
    for (char c : msg) sum += static_cast<unsigned char>(c);
    return sum % 256;
}

std::string FIXCodec::injectChecksum(const std::string& msg) {
    uint32_t c = computeChecksum(msg);
    std::ostringstream oss;
    oss << msg << "10=" << std::setw(3) << std::setfill('0') << c << '\x01';
    return oss.str();
}

void FIXCodec::replaceTag(std::string& msg, const std::string& tag, const std::string& value) {
    std::string key = tag + "=";
    std::size_t pos = msg.find(key);
    if (pos != std::string::npos) {
        std::size_t end = msg.find('\x01', pos);
        if (end != std::string::npos) {
            msg.replace(pos, end - pos, key + value);
        }
    }
}

std::string FIXCodec::stampSendingTime(const std::string& msg, const std::string& ts) {
    std::string out = msg;
    replaceTag(out, "52", ts);
    return out;
}

} // namespace Omega

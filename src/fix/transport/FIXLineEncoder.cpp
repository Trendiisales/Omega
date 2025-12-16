#include "FIXLineEncoder.hpp"

namespace Omega {

FIXLineEncoder::FIXLineEncoder() {}

std::string FIXLineEncoder::encode(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (c == '|') out.push_back('\x01');
        else out.push_back(c);
    }
    if (out.empty() || out.back() != '\x01')
        out.push_back('\x01');
    return out;
}

std::string FIXLineEncoder::decode(const std::string& wire) {
    std::string out;
    out.reserve(wire.size());
    for (char c : wire) {
        if (c == '\x01') out.push_back('|');
        else out.push_back(c);
    }
    return out;
}

} // namespace Omega

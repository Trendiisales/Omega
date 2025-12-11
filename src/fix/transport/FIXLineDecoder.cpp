#include "FIXLineDecoder.hpp"
#include <algorithm>

namespace Omega {

FIXLineDecoder::FIXLineDecoder() {}

void FIXLineDecoder::reset() {
    buf.clear();
}

void FIXLineDecoder::append(const char* data, std::size_t n) {
    buf.insert(buf.end(), data, data + n);
}

bool FIXLineDecoder::nextMsg(std::string& out) {
    if (buf.size() < 8) return false;

    // look for FIX header "8=FIX" which marks start  
    // look for SOH-terminated "10=" checksum to mark end
    const char* hdr = "8=FIX";
    auto start = std::search(buf.begin(), buf.end(), hdr, hdr + 5);
    if (start == buf.end()) {
        buf.clear();
        return false;
    }

    const char* cs_pattern = "\x01""10=";
    auto cs = std::search(start, buf.end(), cs_pattern, cs_pattern + 4);
    if (cs == buf.end()) return false;

    auto end = std::find(cs + 1, buf.end(), '\x01');
    if (end == buf.end()) return false;

    std::size_t len = (end - start) + 1;
    out.assign(start, start + len);

    buf.erase(buf.begin(), start + len);
    return true;
}

} // namespace Omega

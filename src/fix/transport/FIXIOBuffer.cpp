#include "FIXIOBuffer.hpp"
#include <algorithm>

namespace Omega {

FIXIOBuffer::FIXIOBuffer() {}

void FIXIOBuffer::append(const char* data, std::size_t len) {
    buf.insert(buf.end(), data, data + len);
}

bool FIXIOBuffer::extractFrame(std::string& out) {
    if (buf.empty()) return false;

    // FIX frame ends with '10=XXX' + delimiter
    const char* pattern = "\x01""10=";
    auto it = std::search(buf.begin(), buf.end(),
                          pattern, pattern + 4);

    if (it == buf.end()) return false;

    // Find end of checksum field
    auto end = std::find(it + 1, buf.end(), '\x01');
    if (end == buf.end()) return false;

    std::size_t frameLen = (end - buf.begin()) + 1;
    out.assign(buf.begin(), buf.begin() + frameLen);
    buf.erase(buf.begin(), buf.begin() + frameLen);
    return true;
}

void FIXIOBuffer::clear() {
    buf.clear();
}

std::size_t FIXIOBuffer::size() const {
    return buf.size();
}

} // namespace Omega

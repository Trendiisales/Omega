#include "FIXFraming.hpp"

namespace Omega {

std::string FIXFraming::normalizeDelimiters(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '|') out.push_back('\x01');
        else out.push_back(c);
    }
    return out;
}

std::string FIXFraming::restoreSOH(const std::string& s) {
    return normalizeDelimiters(s);
}

} // namespace Omega

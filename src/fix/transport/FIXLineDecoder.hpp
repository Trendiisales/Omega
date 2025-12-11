#pragma once
#include <string>
#include <vector>

namespace Omega {

class FIXLineDecoder {
public:
    FIXLineDecoder();
    ~FIXLineDecoder() = default;

    void reset();
    void append(const char* data, std::size_t n);
    bool nextMsg(std::string& out);

private:
    std::vector<char> buf;
};

} // namespace Omega

#pragma once
#include <vector>
#include <string>
#include <cstdint>

namespace Omega {

class FIXIOBuffer {
public:
    FIXIOBuffer();
    ~FIXIOBuffer() = default;

    void append(const char* data, std::size_t len);
    bool extractFrame(std::string& out);

    void clear();
    std::size_t size() const;

private:
    std::vector<char> buf;
};

} // namespace Omega

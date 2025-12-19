#pragma once
#include <string>
#include <unordered_map>

namespace Chimera {

class FIXParser {
public:
    FIXParser();
    ~FIXParser() = default;

    std::unordered_map<std::string, std::string> parse(const std::string& msg);
    
    static std::string getTag(const std::string& msg, const std::string& tag);
    static bool hasTag(const std::string& msg, const std::string& tag);

private:
    char delimiter;
};

} // namespace Chimera

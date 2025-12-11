#include "FIXParser.hpp"
#include <sstream>

namespace Omega {

FIXParser::FIXParser() : delimiter('\x01') {}

std::unordered_map<std::string, std::string> FIXParser::parse(const std::string& msg) {
    std::unordered_map<std::string, std::string> result;
    
    std::string s = msg;
    size_t pos = 0;
    
    while (pos < s.size()) {
        size_t eq = s.find('=', pos);
        if (eq == std::string::npos) break;
        
        size_t end = s.find(delimiter, eq);
        if (end == std::string::npos) end = s.size();
        
        std::string tag = s.substr(pos, eq - pos);
        std::string val = s.substr(eq + 1, end - eq - 1);
        
        result[tag] = val;
        pos = end + 1;
    }
    
    return result;
}

std::string FIXParser::getTag(const std::string& msg, const std::string& tag) {
    std::string key = tag + "=";
    size_t pos = msg.find(key);
    if (pos == std::string::npos) return "";
    
    size_t start = pos + key.size();
    size_t end = msg.find('\x01', start);
    if (end == std::string::npos) end = msg.size();
    
    return msg.substr(start, end - start);
}

bool FIXParser::hasTag(const std::string& msg, const std::string& tag) {
    std::string key = tag + "=";
    return msg.find(key) != std::string::npos;
}

} // namespace Omega

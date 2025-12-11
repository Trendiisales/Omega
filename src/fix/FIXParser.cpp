#include "FIXParser.hpp"
#include <cstdlib>

namespace Omega {

bool FIXParser::parse(const std::string& raw,
                      std::unordered_map<int,std::string>& out)
{
    out.clear();
    size_t i=0;
    while(i < raw.size()) {
        size_t eq = raw.find('=', i);
        if(eq == std::string::npos) break;
        size_t fs = raw.find('\x01', eq);
        if(fs == std::string::npos) break;
        int tag = std::atoi(raw.substr(i, eq-i).c_str());
        std::string val = raw.substr(eq+1, fs-eq-1);
        out[tag] = val;
        i = fs + 1;
    }
    return true;
}

std::string FIXParser::get(const std::unordered_map<int,std::string>& m, int tag) {
    auto it = m.find(tag);
    return it==m.end() ? "" : it->second;
}

int FIXParser::getInt(const std::unordered_map<int,std::string>& m, int tag) {
    auto it = m.find(tag);
    if(it==m.end()) return 0;
    return std::atoi(it->second.c_str());
}

double FIXParser::getDouble(const std::unordered_map<int,std::string>& m, int tag) {
    auto it = m.find(tag);
    if(it==m.end()) return 0.0;
    return std::atof(it->second.c_str());
}

}

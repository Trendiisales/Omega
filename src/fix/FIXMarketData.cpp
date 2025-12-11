#include "FIXMarketData.hpp"
#include "FIXParser.hpp"

namespace Omega {

bool FIXMarketData::isMarketData(const std::unordered_map<int,std::string>& msg) {
    auto it = msg.find(35);
    // 35=W is Market Data Snapshot, 35=X Market Data Update
    return it!=msg.end() && (it->second=="W" || it->second=="X");
}

std::string FIXMarketData::symbol(const std::unordered_map<int,std::string>& msg) {
    return FIXParser::get(msg,55);
}

double FIXMarketData::bidPx(const std::unordered_map<int,std::string>& msg) {
    return FIXParser::getDouble(msg,132);
}

double FIXMarketData::askPx(const std::unordered_map<int,std::string>& msg) {
    return FIXParser::getDouble(msg,133);
}

double FIXMarketData::bidSize(const std::unordered_map<int,std::string>& msg) {
    return FIXParser::getDouble(msg,134);
}

double FIXMarketData::askSize(const std::unordered_map<int,std::string>& msg) {
    return FIXParser::getDouble(msg,135);
}

}

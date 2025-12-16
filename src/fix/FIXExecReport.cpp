#include "FIXExecReport.hpp"
#include "FIXParser.hpp"

namespace Omega {

bool FIXExecReport::isExecReport(const std::unordered_map<int,std::string>& msg) {
    auto it = msg.find(35);
    return it != msg.end() && it->second == "8";
}

std::string FIXExecReport::clOrdId(const std::unordered_map<int,std::string>& msg) {
    return FIXParser::get(msg,11);
}

std::string FIXExecReport::execID(const std::unordered_map<int,std::string>& msg) {
    return FIXParser::get(msg,17);
}

std::string FIXExecReport::ordID(const std::unordered_map<int,std::string>& msg) {
    return FIXParser::get(msg,37);
}

char FIXExecReport::execType(const std::unordered_map<int,std::string>& msg) {
    auto s = FIXParser::get(msg,150);
    return s.empty() ? '?' : s[0];
}

char FIXExecReport::ordStatus(const std::unordered_map<int,std::string>& msg) {
    auto s = FIXParser::get(msg,39);
    return s.empty() ? '?' : s[0];
}

double FIXExecReport::lastQty(const std::unordered_map<int,std::string>& msg) {
    return FIXParser::getDouble(msg,32);
}

double FIXExecReport::lastPx(const std::unordered_map<int,std::string>& msg) {
    return FIXParser::getDouble(msg,31);
}

double FIXExecReport::leaves(const std::unordered_map<int,std::string>& msg) {
    return FIXParser::getDouble(msg,151);
}

double FIXExecReport::cumQty(const std::unordered_map<int,std::string>& msg) {
    return FIXParser::getDouble(msg,14);
}

}

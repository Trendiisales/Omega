#pragma once
#include <string>
#include <unordered_map>

namespace Omega {

struct FIXExecReport {
    static bool isExecReport(const std::unordered_map<int,std::string>& msg);

    static std::string clOrdId(const std::unordered_map<int,std::string>& msg);
    static std::string execID(const std::unordered_map<int,std::string>& msg);
    static std::string ordID (const std::unordered_map<int,std::string>& msg);

    static char execType(const std::unordered_map<int,std::string>& msg);   // 150
    static char ordStatus(const std::unordered_map<int,std::string>& msg);  // 39
    static double lastQty(const std::unordered_map<int,std::string>& msg);  // 32
    static double lastPx (const std::unordered_map<int,std::string>& msg);  // 31
    static double leaves (const std::unordered_map<int,std::string>& msg);  // 151
    static double cumQty (const std::unordered_map<int,std::string>& msg);  // 14
};

}

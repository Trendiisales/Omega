#pragma once
#include <unordered_map>
#include <string>

namespace Omega {

struct FIXMarketData {
    static bool isMarketData(const std::unordered_map<int,std::string>& msg);

    static std::string symbol(const std::unordered_map<int,std::string>& msg); // 55
    static double bidPx (const std::unordered_map<int,std::string>& msg);     // 132
    static double askPx (const std::unordered_map<int,std::string>& msg);     // 133
    static double bidSize(const std::unordered_map<int,std::string>& msg);    // 134
    static double askSize(const std::unordered_map<int,std::string>& msg);    // 135
};

}

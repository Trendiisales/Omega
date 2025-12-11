#pragma once
#include <unordered_map>
#include <string>
#include "../market/Tick.hpp"

namespace Omega {

class BinanceTickNormalizer {
public:
    static bool parse(const std::string& msg, Tick& t);
};

}

#pragma once
#include <string>
#include "../market/Tick.hpp"

namespace Omega {

class TickCSV {
public:
    static std::string header();
    static std::string encode(const Tick&);
};

}

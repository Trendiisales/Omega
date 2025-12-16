#pragma once
#include <string>
#include "../micro/MicroStructureState.hpp"

namespace Omega {

class MicroCSV {
public:
    static std::string header();
    static std::string encode(const MicroStructureState&);
};

}

#pragma once
#include "../FIXMessage.hpp"

namespace Omega {

class FIXGapFill {
public:
    FIXGapFill();
    bool isGapFill(const FIXMessage&);
};

}

#include "FIXGapFill.hpp"

namespace Omega {

FIXGapFill::FIXGapFill(){}

bool FIXGapFill::isGapFill(const FIXMessage& m){
    return m.get(35)=="4" && m.getInt(43)==1;
}

}

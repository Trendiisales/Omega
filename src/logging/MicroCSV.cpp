#include "MicroCSV.hpp"
#include <sstream>

namespace Omega {

std::string MicroCSV::header(){
    return "ts,mid,ofi,vpin,imbalance,shock,volburst,flow,regime,depthRatio";
}

std::string MicroCSV::encode(const MicroStructureState& m){
    std::ostringstream o;
    o<<m.ts<<","<<m.mid<<","<<m.ofi<<","<<m.vpin<<","<<m.imbalance<<","
     <<m.shock<<","<<m.volBurst<<","<<m.flow<<","<<m.regime<<","<<m.depthRatio;
    return o.str();
}

}

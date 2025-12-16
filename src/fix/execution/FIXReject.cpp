#include "FIXReject.hpp"
#include <cstdlib>

namespace Omega {

FIXReject::FIXReject(){}

void FIXReject::setCallback(std::function<void(const FIXRejectInfo&)> cb){
    callback=cb;
}

bool FIXReject::parse(const FIXMessage& m, FIXRejectInfo& r){
    r = FIXRejectInfo{};
    r.refID  = m.get(45);
    r.reason = m.get(58);
    r.code   = m.getInt(371);

    if(callback) callback(r);
    return true;
}

}

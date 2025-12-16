#include "FIXDropCopy.hpp"

namespace Omega {

FIXDropCopy::FIXDropCopy(){}

void FIXDropCopy::setCallback(std::function<void(const FIXMessage&)> f){
    cb=f;
}

void FIXDropCopy::onFIX(const FIXMessage& m){
    if(cb) cb(m);
}

}

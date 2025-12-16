#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include "../md/FIXMDDecoder.hpp"
#include "../bridge/FIXBridge.hpp"
#include "FIXSymbolMap.hpp"

namespace Omega {

class FIXFeedMux {
public:
    FIXFeedMux();

    void setBridge(FIXBridge*);
    void setMap(FIXSymbolMap*);

    void onFIX(const FIXMessage&);

private:
    FIXBridge* bridge=nullptr;
    FIXSymbolMap* smap=nullptr;

    std::mutex lock;
};

}

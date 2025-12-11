#pragma once
#include <string>
#include <mutex>
#include "../../engine/MotherEngine.hpp"
#include "../bridge/FIXBridge.hpp"

namespace Omega {

class FIXMotherEngineLink {
public:
    FIXMotherEngineLink();

    void setMother(MotherEngine*);
    void setBridge(FIXBridge*);

    void onTick(const std::string& symbol,const Tick&);
    void onBook(const std::string& symbol,const OrderBook&);
    void onExec(const ExecReport&);
    void onReject(const FIXRejectInfo&);

private:
    MotherEngine* mother=nullptr;
    FIXBridge* bridge=nullptr;
    std::mutex lock;
};

}

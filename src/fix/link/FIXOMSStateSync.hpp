#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include "../session/FIXOrderState.hpp"
#include "../FIXMessage.hpp"

namespace Omega {

class FIXOMSStateSync {
public:
    FIXOMSStateSync();

    void update(const ExecReport&);
    void markPendingCancel(const std::string& clOrdId);
    void markReplace(const std::string& oldID,const std::string& newID);

    OrderStateRecord get(const std::string& clOrdId);

private:
    FIXOrderState state;
    std::unordered_map<std::string,std::string> pendingCancel;
    std::unordered_map<std::string,std::string> replaceMap;

    std::mutex lock;
};

}

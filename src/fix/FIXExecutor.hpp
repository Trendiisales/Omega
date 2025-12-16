#pragma once
#include <string>
#include "FIXSession.hpp"
#include "FIXMessageBuilder.hpp"

namespace Omega {

class FIXExecutor {
public:
    FIXExecutor();

    bool init(FIXSession* session, const std::string& account);
    bool sendNewOrder(const std::string& symbol,
                      double qty,
                      double px,
                      bool isBuy);

    bool sendCancel(const std::string& clOrdId);

private:
    FIXSession* sess;
    std::string acct;

    uint64_t orderCounter;

    std::string nextID();
};

}

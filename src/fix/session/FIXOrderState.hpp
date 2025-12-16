#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include "../execution/FIXExecHandler.hpp"

namespace Omega {

enum class OrderStatus {
    NONE,
    SUBMITTED,
    PARTIAL,
    FILLED,
    CANCELLED,
    REJECTED
};

struct OrderStateRecord {
    std::string clOrdId;
    std::string orderId;
    std::string symbol;
    OrderStatus status=OrderStatus::NONE;
    double qty=0;
    double filled=0;
    double leaves=0;
    double lastPrice=0;
    double lastQty=0;
    long ts=0;
};

class FIXOrderState {
public:
    FIXOrderState();

    void update(const ExecReport&);
    OrderStateRecord get(const std::string& clOrdId);

private:
    std::mutex lock;
    std::unordered_map<std::string,OrderStateRecord> map;
};

}

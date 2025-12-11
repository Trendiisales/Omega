#include "FIXOrderState.hpp"

namespace Omega {

FIXOrderState::FIXOrderState(){}

void FIXOrderState::update(const ExecReport& r){
    std::lock_guard<std::mutex> g(lock);

    OrderStateRecord& s = map[r.clOrdId];

    s.clOrdId  = r.clOrdId;
    s.orderId  = r.orderId;
    s.symbol   = r.symbol;
    s.qty      = r.qty;
    s.filled   = r.filled;
    s.lastPrice= r.price;
    s.lastQty  = r.filled;
    s.leaves   = r.leaves;
    s.ts       = r.ts;

    if(r.status=="0") s.status = OrderStatus::SUBMITTED;
    else if(r.status=="1") s.status = OrderStatus::PARTIAL;
    else if(r.status=="2") s.status = OrderStatus::FILLED;
    else if(r.status=="4") s.status = OrderStatus::CANCELLED;
    else if(r.status=="8") s.status = OrderStatus::REJECTED;
}

OrderStateRecord FIXOrderState::get(const std::string& id){
    std::lock_guard<std::mutex> g(lock);
    return map[id];
}

}

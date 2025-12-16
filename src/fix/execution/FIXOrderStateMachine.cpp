#include "FIXOrderStateMachine.hpp"
#include <chrono>

namespace Omega {

static uint64_t ts_now() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

FIXOrderStateMachine::FIXOrderStateMachine() {}
FIXOrderStateMachine::~FIXOrderStateMachine() {}

bool FIXOrderStateMachine::exists(const std::string& clId) const {
    std::lock_guard<std::mutex> g(lock);
    return book.find(clId) != book.end();
}

void FIXOrderStateMachine::createNew(const std::string& clId,
                                     const std::string& symbol,
                                     const std::string& side,
                                     double qty,
                                     double price)
{
    std::lock_guard<std::mutex> g(lock);
    
    OrderFSMRecord r;
    r.clOrdId = clId;
    r.symbol = symbol;
    r.side = side;
    r.qty = qty;
    r.price = price;
    r.filled = 0;
    r.leaves = qty;
    r.state = OrderState::PENDING_NEW;
    r.ts = ts_now();

    book[clId] = r;
}

void FIXOrderStateMachine::markPendingCancel(const std::string& clId) {
    std::lock_guard<std::mutex> g(lock);
    auto it = book.find(clId);
    if (it != book.end()) {
        it->second.state = OrderState::PENDING_CANCEL;
        it->second.ts = ts_now();
    }
}

OrderFSMRecord FIXOrderStateMachine::get(const std::string& clId) const {
    std::lock_guard<std::mutex> g(lock);
    auto it = book.find(clId);
    if (it == book.end()) {
        return OrderFSMRecord{};
    }
    return it->second;
}

OrderState FIXOrderStateMachine::state(const std::string& clId) const {
    std::lock_guard<std::mutex> g(lock);
    auto it = book.find(clId);
    if (it == book.end()) return OrderState::NONE;
    return it->second.state;
}

size_t FIXOrderStateMachine::activeCount() const {
    std::lock_guard<std::mutex> g(lock);
    size_t count = 0;
    for (const auto& kv : book) {
        OrderState s = kv.second.state;
        if (s == OrderState::PENDING_NEW || 
            s == OrderState::NEW || 
            s == OrderState::PARTIALLY_FILLED ||
            s == OrderState::PENDING_CANCEL) {
            count++;
        }
    }
    return count;
}

void FIXOrderStateMachine::clear() {
    std::lock_guard<std::mutex> g(lock);
    book.clear();
}

OrderState FIXOrderStateMachine::translate(const std::string& s) const {
    if (s == "0") return OrderState::NEW;
    if (s == "1") return OrderState::PARTIALLY_FILLED;
    if (s == "2") return OrderState::FILLED;
    if (s == "4") return OrderState::CANCELED;
    if (s == "8") return OrderState::REJECTED;
    return OrderState::NONE;
}

void FIXOrderStateMachine::applyExec(const std::string& clId,
                                     const std::string& status,
                                     double filled,
                                     double leaves,
                                     double px)
{
    std::lock_guard<std::mutex> g(lock);
    auto it = book.find(clId);
    if (it == book.end()) return;

    OrderFSMRecord& r = it->second;

    r.ts = ts_now();
    if (px > 0) r.price = px;

    if (filled >= 0) r.filled = filled;
    if (leaves >= 0) r.leaves = leaves;

    r.state = translate(status);
}

} // namespace Omega

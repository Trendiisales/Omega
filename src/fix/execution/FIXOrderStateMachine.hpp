#pragma once
#include <string>
#include <cstdint>
#include <unordered_map>
#include <mutex>

namespace Omega {

enum class OrderState {
    NONE = 0,
    PENDING_NEW,
    NEW,
    PARTIALLY_FILLED,
    FILLED,
    PENDING_CANCEL,
    CANCELED,
    REJECTED
};

struct OrderFSMRecord {
    std::string clOrdId;
    std::string symbol;
    std::string side;
    double qty = 0.0;
    double filled = 0.0;
    double leaves = 0.0;
    double price = 0.0;
    OrderState state = OrderState::NONE;
    uint64_t ts = 0;
};

class FIXOrderStateMachine {
public:
    FIXOrderStateMachine();
    ~FIXOrderStateMachine();

    void createNew(const std::string& clId,
                   const std::string& symbol,
                   const std::string& side,
                   double qty,
                   double price);

    void applyExec(const std::string& clId,
                   const std::string& status,
                   double filled,
                   double leaves,
                   double px);

    void markPendingCancel(const std::string& clId);

    OrderFSMRecord get(const std::string& clId) const;
    OrderState state(const std::string& clId) const;

    bool exists(const std::string& clId) const;
    size_t activeCount() const;
    
    void clear();

private:
    OrderState translate(const std::string& fixStatus) const;

private:
    mutable std::mutex lock;
    std::unordered_map<std::string, OrderFSMRecord> book;
};

} // namespace Omega

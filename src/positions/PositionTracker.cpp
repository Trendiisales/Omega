#include "PositionTracker.hpp"

namespace Omega {

PositionTracker::PositionTracker()
    : qty(0.0), avgPrice(0.0) {}

void PositionTracker::reset() {
    qty = 0.0;
    avgPrice = 0.0;
}

void PositionTracker::setSymbol(const std::string& s) {
    symbol = s;
}

void PositionTracker::update(const ExecReport& r) {
    if (r.symbol != symbol && !symbol.empty())
        return;

    double side = (r.side == "BUY" ? 1.0 : -1.0);
    double tradedQty = r.filled;
    if (tradedQty <= 0.0) return;

    double newQty = qty + side * tradedQty;

    if (qty == 0.0) {
        avgPrice = r.price;
    } else if (side * qty >= 0.0 && side * newQty > 0.0) {
        // add to same direction
        double notionalOld = avgPrice * qty;
        double notionalNew = r.price * (side * tradedQty);
        double totalQty    = qty + side * tradedQty;
        avgPrice = (notionalOld + notionalNew) / totalQty;
    } else {
        // reducing or flipping, avgPrice left as-is on flip for simplicity
        if (newQty == 0.0) {
            avgPrice = 0.0;
        }
    }

    qty = newQty;
}

PositionSnapshot PositionTracker::snapshot() const {
    PositionSnapshot s;
    s.qty = qty;
    s.avgPrice = avgPrice;
    s.unrealizedPnL = 0.0;
    return s;
}

} // namespace Omega

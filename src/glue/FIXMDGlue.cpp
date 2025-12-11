#include "FIXMDGlue.hpp"

namespace Omega {

FIXMDGlue::FIXMDGlue()
    : book(nullptr)
{
}

FIXMDGlue::~FIXMDGlue() {}

void FIXMDGlue::attach(FIXMDOrderBook* b) {
    book = b;
}

void FIXMDGlue::setSymbol(const std::string& s) {
    symbol = s;
}

void FIXMDGlue::setCallback(std::function<void(const UnifiedTick&)> cb) {
    onTick = cb;
}

void FIXMDGlue::onFIXUpdate(double bid,
                            double ask,
                            double bidSize,
                            double askSize,
                            uint64_t ts)
{
    if (!book) return;

    book->updateBid(bid, bidSize);
    book->updateAsk(ask, askSize);

    FIXMDBook snap = book->snapshot();
    UnifiedTick ut = normalizer.normalize(snap, symbol);

    if (onTick) onTick(ut);
}

} // namespace Omega

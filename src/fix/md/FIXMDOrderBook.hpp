#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <mutex>

namespace Omega {

struct FIXMDLevel {
    double price = 0.0;
    double size = 0.0;
};

struct FIXMDBook {
    std::vector<FIXMDLevel> bids;
    std::vector<FIXMDLevel> asks;
    uint64_t ts = 0;
};

class FIXMDOrderBook {
public:
    FIXMDOrderBook();
    ~FIXMDOrderBook();

    void updateBid(double price, double size);
    void updateAsk(double price, double size);

    void clear();

    FIXMDBook snapshot() const;

    double bestBid() const;
    double bestAsk() const;

    double bestBidSize() const;
    double bestAskSize() const;

    // Accessors for bid/ask vectors
    const std::vector<FIXMDLevel>& bids() const { return bids_; }
    const std::vector<FIXMDLevel>& asks() const { return asks_; }

private:
    mutable std::mutex mtx;
    std::vector<FIXMDLevel> bids_;
    std::vector<FIXMDLevel> asks_;
    uint64_t ts;
};

} // namespace Omega

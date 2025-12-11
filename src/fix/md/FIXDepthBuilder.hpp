#pragma once
#include <string>
#include <cstdint>
#include <vector>
#include "FIXMDOrderBook.hpp"

namespace Omega {

struct DepthSnapshot {
    std::string symbol;
    std::vector<double> bids;
    std::vector<double> bidSizes;
    std::vector<double> asks;
    std::vector<double> askSizes;
    uint64_t ts = 0;
    
    double bestBid() const { return bids.empty() ? 0.0 : bids[0]; }
    double bestAsk() const { return asks.empty() ? 0.0 : asks[0]; }
    double spread() const { 
        double b = bestBid();
        double a = bestAsk();
        return (b > 0 && a > 0) ? (a - b) : 0.0;
    }
};

class FIXDepthBuilder {
public:
    FIXDepthBuilder();
    ~FIXDepthBuilder();

    void setDepthLimit(size_t levels);
    void attachBook(FIXMDOrderBook* book);

    DepthSnapshot build(const std::string& symbol);

private:
    size_t depthLimit;
    FIXMDOrderBook* ob;
};

} // namespace Omega

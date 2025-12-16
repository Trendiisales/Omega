#pragma once
#include <string>
#include <deque>
#include <mutex>
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "MicroMetrics.hpp"

namespace Omega {

class TickPipelineExt {
public:
    TickPipelineExt();

    void init(const std::string& symbol);

    void pushTick(const Tick& t);
    void pushBook(const OrderBook& ob);

    bool compute(MicroMetrics& out);
    bool computeBook(MicroMetrics& out);

private:
    std::string sym;
    std::mutex lock;
    
    std::deque<Tick> tickBuffer;
    std::deque<OrderBook> bookBuffer;
    
    static const int MAX_BUFFER = 1000;
    
    Tick       lastTick;
    OrderBook  lastBook;
    bool       hasTick;
    bool       hasBook;
    
    // EMAs
    double midEMA = 0;
    double spreadEMA = 0;
    double volEMA = 0;
    double ofiAccum = 0;
    
    void computeMomentum(MicroMetrics& m);
    void computeVolatility(MicroMetrics& m);
    void computeOFI(MicroMetrics& m);
    void computeImbalance(MicroMetrics& m);
};

} // namespace Omega

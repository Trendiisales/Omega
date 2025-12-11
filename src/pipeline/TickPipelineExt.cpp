#include "TickPipelineExt.hpp"
#include <cmath>

namespace Omega {

TickPipelineExt::TickPipelineExt()
    : hasTick(false), hasBook(false) {}

void TickPipelineExt::init(const std::string& symbol) {
    sym = symbol;
    hasTick = false;
    hasBook = false;
    tickBuffer.clear();
    bookBuffer.clear();
    midEMA = spreadEMA = volEMA = ofiAccum = 0;
}

void TickPipelineExt::pushTick(const Tick& t) {
    std::lock_guard<std::mutex> g(lock);
    lastTick = t;
    hasTick  = true;
    tickBuffer.push_back(t);
    if (tickBuffer.size() > MAX_BUFFER) tickBuffer.pop_front();
    
    // Update EMAs
    double alpha = 0.1;
    double mid = (t.bid + t.ask) / 2.0;
    midEMA = midEMA * (1.0 - alpha) + mid * alpha;
    spreadEMA = spreadEMA * (1.0 - alpha) + t.spread * alpha;
}

void TickPipelineExt::pushBook(const OrderBook& ob) {
    std::lock_guard<std::mutex> g(lock);
    lastBook = ob;
    hasBook  = true;
    bookBuffer.push_back(ob);
    if (bookBuffer.size() > MAX_BUFFER) bookBuffer.pop_front();
}

bool TickPipelineExt::compute(MicroMetrics& out) {
    std::lock_guard<std::mutex> g(lock);
    if (!hasTick) return false;
    
    out.clear();
    
    out.mid = (lastTick.bid + lastTick.ask) / 2.0;
    out.spread = lastTick.spread;
    
    computeMomentum(out);
    computeVolatility(out);
    computeOFI(out);
    computeImbalance(out);
    
    // Trend and vol ratio for regime classification
    out.trendScore = std::abs(out.momentum) * 100.0;
    out.volRatio = (out.volatility > 0) ? out.volatility / 0.001 : 0;
    out.shockFlag = (out.volRatio > 3.0);
    
    return true;
}

bool TickPipelineExt::computeBook(MicroMetrics& out) {
    std::lock_guard<std::mutex> g(lock);
    if (!hasBook) return false;
    
    double bidDepth = 0, askDepth = 0;
    for (int i = 0; i < 10; i++) {
        bidDepth += lastBook.bidSize[i];
        askDepth += lastBook.askSize[i];
    }
    
    if (bidDepth + askDepth > 0) {
        out.depthRatio = bidDepth / (bidDepth + askDepth);
        out.imbalance = (bidDepth - askDepth) / (bidDepth + askDepth);
    }
    
    return true;
}

void TickPipelineExt::computeMomentum(MicroMetrics& m) {
    if (tickBuffer.size() < 20) return;
    
    double sum = 0;
    int n = std::min((int)tickBuffer.size(), 20);
    for (size_t i = tickBuffer.size() - n; i < tickBuffer.size(); i++) {
        double mid = (tickBuffer[i].bid + tickBuffer[i].ask) / 2.0;
        sum += mid;
    }
    double avg = sum / n;
    m.momentum = (m.mid - avg) / avg;
    m.v[0] = m.momentum;
}

void TickPipelineExt::computeVolatility(MicroMetrics& m) {
    if (tickBuffer.size() < 20) return;
    
    double sum = 0, sum2 = 0;
    int n = std::min((int)tickBuffer.size(), 20);
    for (size_t i = tickBuffer.size() - n; i < tickBuffer.size(); i++) {
        double mid = (tickBuffer[i].bid + tickBuffer[i].ask) / 2.0;
        sum += mid;
        sum2 += mid * mid;
    }
    double mean = sum / n;
    double var = sum2 / n - mean * mean;
    m.volatility = std::sqrt(std::max(0.0, var));
    m.v[16] = m.volatility;
}

void TickPipelineExt::computeOFI(MicroMetrics& m) {
    if (tickBuffer.size() < 2) return;
    
    const Tick& prev = tickBuffer[tickBuffer.size() - 2];
    const Tick& curr = tickBuffer.back();
    
    double deltaBid = curr.bid - prev.bid;
    double deltaAsk = curr.ask - prev.ask;
    
    double ofi = deltaBid - deltaAsk;
    ofiAccum = ofiAccum * 0.95 + ofi;
    m.ofi = ofiAccum;
    m.v[5] = ofi;
}

void TickPipelineExt::computeImbalance(MicroMetrics& m) {
    if (bookBuffer.empty()) return;
    
    const OrderBook& ob = bookBuffer.back();
    double bidL0 = ob.bidSize[0];
    double askL0 = ob.askSize[0];
    
    if (bidL0 + askL0 > 0) {
        m.v[1] = (bidL0 - askL0) / (bidL0 + askL0);
    }
}

} // namespace Omega

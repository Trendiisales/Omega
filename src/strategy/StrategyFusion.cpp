#include "StrategyFusion.hpp"
#include <cmath>
#include <chrono>

namespace Omega {

static uint64_t now_us() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

StrategyFusion::StrategyFusion()
    : threshold(0.5) {}

StrategyFusion::~StrategyFusion() {}

// New interface implementation
void StrategyFusion::setSymbol(const std::string& s) {
    sym = s;
}

void StrategyFusion::add(const MicroSignal& s) {
    std::lock_guard<std::mutex> g(lock);
    buffer.push_back(s);
    if (buffer.size() > 32) {
        buffer.erase(buffer.begin());
    }
}

double StrategyFusion::compute() {
    std::lock_guard<std::mutex> g(lock);
    
    if (buffer.empty()) return 0.0;

    double sum = 0.0;
    double conf = 0.0;

    for (const auto& x : buffer) {
        sum += x.value * (0.5 + x.confidence);
        conf += x.confidence;
    }

    double fused = sum / (buffer.size() + 1e-9);

    state.v1 = sum;
    state.v2 = conf;
    state.v3 = fused;
    state.lastSignal = fused;

    if (!buffer.empty()) {
        state.ts = buffer.back().ts;
    }

    buffer.clear();
    return fused;
}

void StrategyFusion::reset() {
    std::lock_guard<std::mutex> g(lock);
    buffer.clear();
    state = StrategyState{};
}

// Legacy interface implementation
void StrategyFusion::init(const std::string& symbol,
                          const std::vector<std::string>& strategies) {
    sym = symbol;
    names = strategies;
}

void StrategyFusion::init(const std::string& symbol,
                          const StrategySet& cfg) {
    sym = symbol;
    config = cfg;
}

double StrategyFusion::computeBase(const MicroMetrics& m) {
    double score = 0.0;
    score += m.momentum * 0.3;
    score += m.toxicity * -0.2;
    score += m.spread < 0.0001 ? 0.1 : -0.1;
    return score;
}

double StrategyFusion::computeQ2(const MicroMetrics& m) {
    double score = 0.0;
    score += m.volatility > 0.001 ? 0.2 : -0.1;
    score += m.ofi * 0.25;
    return score;
}

double StrategyFusion::computeHybrid(const MicroMetrics& m) {
    return (computeBase(m) + computeQ2(m)) * 0.5;
}

Decision StrategyFusion::compute(const Tick& t,
                                  const MicroMetrics& m,
                                  PositionTracker& pos) {
    Decision d;
    d.ts = now_us();
    
    double score = computeHybrid(m);
    d.score = score;
    
    if (score > threshold) {
        d.side = Side::Buy;
    } else if (score < -threshold) {
        d.side = Side::Sell;
    } else {
        d.side = Side::None;
    }
    
    return d;
}

} // namespace Omega

#pragma once
#include <string>
#include <fstream>
#include <mutex>
#include <vector>
#include "../../market/Tick.hpp"
#include "../../market/OrderBook.hpp"
#include "../../micro/MicroState.hpp"

namespace Omega {

struct StrategyState32 {
    double s[32] = {};
    double fused = 0.0;
};

struct EngineState {
    double pnl = 0.0;
    double equity = 0.0;
    double latency = 0.0;
    int regime = 0;
    int throttle = 0;
    int shock = 0;
    long ts = 0;
};

class MLLogger {
public:
    MLLogger();
    ~MLLogger();

    bool open(const std::string& path);
    void close();

    bool write(const Tick& t, const OrderBook& ob, const MicroState& ms,
               const StrategyState32& ss, const EngineState& es);

    void flush();

private:
    void writeHeader();

private:
    std::ofstream file;
    std::mutex mtx;
    bool ready;
};

} // namespace Omega

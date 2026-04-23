// HARNESS STUB — minimal omega::TradeRecord matching production shape.
// Real header lives in production include path; backtest uses this stub.
#pragma once
#include <cstdint>
#include <string>
namespace omega {
struct TradeRecord {
    int id = 0;
    std::string symbol;
    std::string side;
    double entryPrice = 0.0;
    double exitPrice  = 0.0;
    double sl         = 0.0;
    double size       = 0.0;
    double pnl        = 0.0;
    double mfe        = 0.0;
    double mae        = 0.0;
    int64_t entryTs   = 0;
    int64_t exitTs    = 0;
    std::string exitReason;
    std::string engine;
    std::string regime;
    double spreadAtEntry = 0.0;
    bool shadow = false;
};
}

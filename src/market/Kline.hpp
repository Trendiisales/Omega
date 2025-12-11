#pragma once
#include <string>

namespace Omega {

struct Kline {
    std::string symbol;
    long openTime;
    long closeTime;
    
    double open;
    double high;
    double low;
    double close;
    double volume;
    
    bool isClosed;
    
    Kline() : openTime(0), closeTime(0),
              open(0), high(0), low(0), close(0), volume(0),
              isClosed(false) {}
};

}

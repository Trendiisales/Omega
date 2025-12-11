#pragma once
#include <mutex>
#include "../FIXMessage.hpp"

namespace Omega {

class FIXRiskSentinel {
public:
    FIXRiskSentinel();

    void setMaxQty(double q);
    void setMaxNotional(double n);

    bool check(const FIXMessage&);

private:
    std::mutex lock;
    double maxQty=1000000;
    double maxNotional=50000000;
};

}

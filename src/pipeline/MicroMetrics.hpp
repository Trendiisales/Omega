#pragma once

namespace Omega {

struct MicroMetrics {
    double ofi       = 0.0;
    double vpin      = 0.0;
    double imbalance = 0.0;
    double depthRatio= 0.0;
    double trendScore= 0.0;
    double volRatio  = 0.0;
    bool   shockFlag = false;
    
    // Extended fields for MicroEngine outputs
    double mid = 0.0;
    double spread = 0.0;
    double toxicity = 0.0;
    double momentum = 0.0;
    double volatility = 0.0;
    double flow = 0.0;
    int regime = 0;
    
    double v[32] = {0};  // MicroEngine outputs
    
    void clear() {
        ofi = vpin = imbalance = depthRatio = 0;
        trendScore = volRatio = 0;
        shockFlag = false;
        mid = spread = toxicity = momentum = volatility = flow = 0;
        regime = 0;
        for(int i=0;i<32;i++) v[i]=0;
    }
};

} // namespace Omega

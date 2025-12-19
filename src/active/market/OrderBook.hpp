#pragma once

namespace Chimera {

struct OrderBook {
    double bidPrice[10];
    double askPrice[10];
    double bidSize[10];
    double askSize[10];
    
    double mid;
    double spread;
    double imbalance;
    double depthImbalance;
    double pressure;
    
    long ts;
    
    OrderBook() : mid(0), spread(0), imbalance(0),
                  depthImbalance(0), pressure(0), ts(0)
    {
        for(int i=0;i<10;i++){
            bidPrice[i]=askPrice[i]=0;
            bidSize[i]=askSize[i]=0;
        }
    }
    
    void clear() {
        for(int i=0;i<10;i++){
            bidPrice[i]=askPrice[i]=0;
            bidSize[i]=askSize[i]=0;
        }
        mid=spread=imbalance=depthImbalance=pressure=0;
        ts=0;
    }

    void computeDerived() {
        if(bidPrice[0] > 0 && askPrice[0] > 0) {
            mid = 0.5 * (bidPrice[0] + askPrice[0]);
            spread = askPrice[0] - bidPrice[0];
        }
        
        double bSum = 0, aSum = 0;
        for(int i=0;i<10;i++){
            bSum += bidSize[i];
            aSum += askSize[i];
        }
        
        if(bSum + aSum > 0) {
            imbalance = (bSum - aSum) / (bSum + aSum);
        }
        
        double bNear = bidSize[0] + bidSize[1] + bidSize[2];
        double aNear = askSize[0] + askSize[1] + askSize[2];
        if(bNear + aNear > 0) {
            depthImbalance = (bNear - aNear) / (bNear + aNear);
        }
        
        pressure = depthImbalance * 0.6 + imbalance * 0.4;
    }
};

}

#pragma once
#include <string>

namespace Omega {

struct Tick {
    std::string symbol;
    
    double bid;
    double ask;
    double spread;
    
    double buyVol;
    double sellVol;
    double liquidityGap;
    double delta;
    
    // Top-of-book sizes
    double b1, b2, b3, b4;
    double a1, a2, a3, a4;
    
    // Book snapshot (10 levels)
    double bookBids[10];
    double bookAsks[10];
    
    long ts;
    
    Tick() : bid(0), ask(0), spread(0),
             buyVol(0), sellVol(0), liquidityGap(0), delta(0),
             b1(0), b2(0), b3(0), b4(0),
             a1(0), a2(0), a3(0), a4(0), ts(0)
    {
        for(int i=0;i<10;i++){
            bookBids[i]=0;
            bookAsks[i]=0;
        }
    }
};

}

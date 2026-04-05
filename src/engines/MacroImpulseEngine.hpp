#pragma once
#include <deque>
#include <cmath>

class MacroImpulseEngine
{
public:

    struct Tick
    {
        double bid;
        double ask;
        uint64_t ts;
    };

    struct Signal
    {
        bool valid=false;
        bool long_side=false;
        double entry=0;
    };

private:

    static constexpr int VOL_WINDOW=25;
    static constexpr double IMPULSE=1.5;

    std::deque<double> moves;

    double last_mid=0;

public:

    Signal on_tick(const Tick& t)
    {
        Signal sig;

        double mid=(t.bid+t.ask)*0.5;

        if(last_mid==0)
        {
            last_mid=mid;
            return sig;
        }

        double move=mid-last_mid;
        last_mid=mid;

        moves.push_back(std::fabs(move));
        if(moves.size()>VOL_WINDOW)
            moves.pop_front();

        double vol=0;
        for(double m:moves) vol+=m;
        if(!moves.empty()) vol/=moves.size();

        if(vol==0) return sig;

        if(move>IMPULSE*vol)
        {
            sig.valid=true;
            sig.long_side=true;
            sig.entry=t.ask;
        }

        if(move<-IMPULSE*vol)
        {
            sig.valid=true;
            sig.long_side=false;
            sig.entry=t.bid;
        }

        return sig;
    }
};

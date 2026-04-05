#pragma once
#include <deque>
#include <cmath>

class GoldFlowEngine
{
public:

    struct Tick
    {
        double bid;
        double ask;
        double bid_size;
        double ask_size;
        uint64_t ts;
    };

    struct Signal
    {
        bool valid=false;
        bool long_side=false;
        double entry=0;
    };

private:

    static constexpr int ATR_WINDOW=40;
    static constexpr double L2_LONG=0.65;
    static constexpr double L2_SHORT=0.35;

    static constexpr double DRIFT=0.30;
    static constexpr int DRIFT_TICKS=6;

    static constexpr double VACUUM=0.8;

    std::deque<double> ranges;

    double last_mid=0;

    int drift_up=0;
    int drift_down=0;

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

        ranges.push_back(std::fabs(move));
        if(ranges.size()>ATR_WINDOW)
            ranges.pop_front();

        double atr=0;
        for(double r:ranges) atr+=r;
        if(!ranges.empty()) atr/=ranges.size();

        double imbalance=0.5;

        double total=t.bid_size+t.ask_size;
        if(total>0)
            imbalance=t.bid_size/total;

        bool l2_long=imbalance>L2_LONG;
        bool l2_short=imbalance<L2_SHORT;

        bool drift_long=move>DRIFT;
        bool drift_short=move<-DRIFT;

        if(drift_long) drift_up++; else drift_up=0;
        if(drift_short) drift_down++; else drift_down=0;

        bool drift_up_ok=drift_up>=DRIFT_TICKS;
        bool drift_down_ok=drift_down>=DRIFT_TICKS;

        bool vacuum_up=move>VACUUM;
        bool vacuum_down=move<-VACUUM;

        bool long_signal=l2_long||drift_up_ok||vacuum_up;
        bool short_signal=l2_short||drift_down_ok||vacuum_down;

        if(atr>0)
        {
            if(move>2.0*atr) long_signal=true;
            if(move<-2.0*atr) short_signal=true;
        }

        if(long_signal)
        {
            sig.valid=true;
            sig.long_side=true;
            sig.entry=t.ask;
        }

        if(short_signal)
        {
            sig.valid=true;
            sig.long_side=false;
            sig.entry=t.bid;
        }

        return sig;
    }
};

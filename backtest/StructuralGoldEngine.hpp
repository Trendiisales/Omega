#pragma once
#include <cmath>

struct StructuralGoldEngine
{
    bool in_position=false;
    bool long_side=false;

    double entry=0;
    double sl=0;
    double tp=0;

    double impulse_high=0;
    double impulse_low=0;

    double vwap=0;
    double cum_pv=0;
    double cum_vol=0;

    uint64_t entry_ts=0;

    static constexpr double MIN_IMPULSE = 8.0;
    static constexpr double MAX_PULLBACK = 3.0;
    static constexpr double VWAP_DEV = 1.2;
    static constexpr double EDGE_MIN = 4.5;

    void update_vwap(double price)
    {
        cum_pv += price;
        cum_vol += 1.0;
        vwap = cum_pv / cum_vol;
    }

    bool impulse_detect(double price)
    {
        if(impulse_high==0)
        {
            impulse_high=price;
            impulse_low=price;
            return false;
        }

        if(price>impulse_high) impulse_high=price;
        if(price<impulse_low) impulse_low=price;

        double range=impulse_high-impulse_low;

        return range>=MIN_IMPULSE;
    }

    bool pullback_ready(double price)
    {
        if(price < impulse_high && (impulse_high-price)<=MAX_PULLBACK)
            return true;

        if(price > impulse_low && (price-impulse_low)<=MAX_PULLBACK)
            return true;

        return false;
    }

    bool vwap_confirm(double price)
    {
        double dev = std::fabs(price-vwap);
        return dev>=VWAP_DEV;
    }

    bool should_enter(double price)
    {
        if(!impulse_detect(price))
            return false;

        if(!pullback_ready(price))
            return false;

        if(!vwap_confirm(price))
            return false;

        return true;
    }

    void enter_long(double price,uint64_t ts)
    {
        in_position=true;
        long_side=true;

        entry=price;
        sl=price-5;
        tp=price+10;

        entry_ts=ts;
    }

    void enter_short(double price,uint64_t ts)
    {
        in_position=true;
        long_side=false;

        entry=price;
        sl=price+5;
        tp=price-10;

        entry_ts=ts;
    }

    bool should_exit(double price,uint64_t ts)
    {
        if(!in_position)
            return false;

        if(long_side)
        {
            if(price>=tp) return true;
            if(price<=sl) return true;
        }
        else
        {
            if(price<=tp) return true;
            if(price>=sl) return true;
        }

        if(ts-entry_ts>1800)
            return true;

        return false;
    }

    void reset()
    {
        in_position=false;
        impulse_high=0;
        impulse_low=0;
    }
};

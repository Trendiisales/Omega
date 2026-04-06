#pragma once
#include <algorithm>

struct GoldHybridBracket
{
    double hi=0;
    double lo=0;
    double range=0;
    bool armed=false;
};

inline void gold_update(GoldHybridBracket& s,double mid)
{
    if(s.hi==0 && s.lo==0)
    {
        s.hi=mid;
        s.lo=mid;
        return;
    }

    s.hi=std::max(s.hi,mid);
    s.lo=std::min(s.lo,mid);
    s.range=s.hi-s.lo;
}

inline bool gold_arm(GoldHybridBracket& s)
{
    const double MIN_RANGE=2.5;

    if(!s.armed && s.range>=MIN_RANGE)
    {
        s.armed=true;
        return true;
    }
    return false;
}

inline bool gold_long(GoldHybridBracket& s,double mid)
{
    return s.armed && mid>s.hi;
}

inline bool gold_short(GoldHybridBracket& s,double mid)
{
    return s.armed && mid<s.lo;
}

inline void gold_reset(GoldHybridBracket& s)
{
    s.hi=0;
    s.lo=0;
    s.range=0;
    s.armed=false;
}

#pragma once
#include <algorithm>

struct IndexHybridBracket
{
    double hi=0;
    double lo=0;
    double range=0;
    bool armed=false;
};

inline void index_update(IndexHybridBracket& s,double mid)
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

inline bool index_arm(IndexHybridBracket& s,double min_range)
{
    if(!s.armed && s.range>=min_range)
    {
        s.armed=true;
        return true;
    }
    return false;
}

inline bool index_long(IndexHybridBracket& s,double mid)
{
    return s.armed && mid>s.hi;
}

inline bool index_short(IndexHybridBracket& s,double mid)
{
    return s.armed && mid<s.lo;
}

inline void index_reset(IndexHybridBracket& s)
{
    s.hi=0;
    s.lo=0;
    s.range=0;
    s.armed=false;
}

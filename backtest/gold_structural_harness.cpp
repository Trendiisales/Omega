#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <chrono>

struct Tick
{
    uint64_t ts;
    double ask;
    double bid;
};

bool parse_tick(const std::string& line, Tick& t)
{
    if(line.empty()) return false;

    if(line.find("timestamp") != std::string::npos)
        return false;

    std::stringstream ss(line);
    std::string a,b,c;

    if(!getline(ss,a,',')) return false;
    if(!getline(ss,b,',')) return false;
    if(!getline(ss,c,',')) return false;

    try
    {
        t.ts  = std::stoull(a);
        t.ask = std::stod(b);
        t.bid = std::stod(c);
    }
    catch(...)
    {
        return false;
    }

    return true;
}

double mean(const std::vector<double>& v)
{
    double s=0;
    for(double x:v) s+=x;
    return s/v.size();
}

double stdev(const std::vector<double>& v,double m)
{
    double s=0;
    for(double x:v) s+=(x-m)*(x-m);
    return sqrt(s/v.size());
}

struct Engine
{
    bool in=false;
    bool short_side=false;
    double entry=0;
    double pnl=0;
    int trades=0;
};

int main(int argc,char** argv)
{
    if(argc<2)
    {
        std::cout<<"usage: harness ticks.csv\n";
        return 0;
    }

    std::ifstream in(argv[1]);
    std::string line;

    std::vector<double> vwap;
    std::vector<double> impulse;
    std::vector<double> range;

    const int VWAP_N=3000;
    const int IMPULSE_N=600;
    const int RANGE_N=2000;

    const double COST=0.45;
    const double TP=3.0;
    const double SL=2.5;

    Engine sweep;
    Engine vwap_e;
    Engine breakout;

    uint64_t ticks=0;
    uint64_t last_ts=0;

    auto start=std::chrono::high_resolution_clock::now();

    while(getline(in,line))
    {
        Tick t;

        if(!parse_tick(line,t))
            continue;

        if(last_ts && (t.ts-last_ts) > 3600000)
        {
            vwap.clear();
            impulse.clear();
            range.clear();
        }

        last_ts = t.ts;

        ticks++;

        double mid=(t.ask+t.bid)/2;

        vwap.push_back(mid);
        impulse.push_back(mid);
        range.push_back(mid);

        if(vwap.size()>VWAP_N) vwap.erase(vwap.begin());
        if(impulse.size()>IMPULSE_N) impulse.erase(impulse.begin());
        if(range.size()>RANGE_N) range.erase(range.begin());

        if(vwap.size()<VWAP_N) continue;

        double m=mean(vwap);
        double s=stdev(vwap,m);

        double imp=mid-impulse.front();

        double hi=*std::max_element(range.begin(),range.end());
        double lo=*std::min_element(range.begin(),range.end());
        double r=hi-lo;

        /* SWEEP */

        if(!sweep.in)
        {
            if(imp>4)
            {
                sweep.in=true;
                sweep.short_side=true;
                sweep.entry=t.bid;
                sweep.trades++;
            }

            if(imp<-4)
            {
                sweep.in=true;
                sweep.short_side=false;
                sweep.entry=t.ask;
                sweep.trades++;
            }
        }
        else
        {
            double diff=sweep.short_side ? sweep.entry-t.ask : t.bid-sweep.entry;

            if(diff>=TP){ sweep.pnl+=TP-COST; sweep.in=false; }
            if(diff<=-SL){ sweep.pnl-=SL+COST; sweep.in=false; }
        }

        /* VWAP */

        if(!vwap_e.in)
        {
            if(mid>m+2.5*s)
            {
                vwap_e.in=true;
                vwap_e.short_side=true;
                vwap_e.entry=t.bid;
                vwap_e.trades++;
            }

            if(mid<m-2.5*s)
            {
                vwap_e.in=true;
                vwap_e.short_side=false;
                vwap_e.entry=t.ask;
                vwap_e.trades++;
            }
        }
        else
        {
            double diff=vwap_e.short_side ? vwap_e.entry-t.ask : t.bid-vwap_e.entry;

            if(diff>=TP){ vwap_e.pnl+=TP-COST; vwap_e.in=false; }
            if(diff<=-SL){ vwap_e.pnl-=SL+COST; vwap_e.in=false; }
        }

        /* BREAKOUT */

        if(!breakout.in && r<1.5)
        {
            if(mid>hi)
            {
                breakout.in=true;
                breakout.short_side=false;
                breakout.entry=t.ask;
                breakout.trades++;
            }

            if(mid<lo)
            {
                breakout.in=true;
                breakout.short_side=true;
                breakout.entry=t.bid;
                breakout.trades++;
            }
        }
        else if(breakout.in)
        {
            double diff=breakout.short_side ? breakout.entry-t.ask : t.bid-breakout.entry;

            if(diff>=TP){ breakout.pnl+=TP-COST; breakout.in=false; }
            if(diff<=-SL){ breakout.pnl-=SL+COST; breakout.in=false; }
        }
    }

    auto end=std::chrono::high_resolution_clock::now();

    std::cout<<"\nTICKS READ: "<<ticks<<"\n\n";

    std::cout<<"SWEEP\nTrades "<<sweep.trades<<"  PnL "<<sweep.pnl<<"\n";

    std::cout<<"VWAP\nTrades "<<vwap_e.trades<<"  PnL "<<vwap_e.pnl<<"\n";

    std::cout<<"BREAKOUT\nTrades "<<breakout.trades<<"  PnL "<<breakout.pnl<<"\n";

    std::cout<<"\nRuntime "
    <<std::chrono::duration<double>(end-start).count()<<" sec\n";
}

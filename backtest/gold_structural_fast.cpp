#include <iostream>
#include <fstream>
#include <sstream>
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
    if(line.find("timestamp")!=std::string::npos) return false;

    std::stringstream ss(line);
    std::string a,b,c;

    if(!getline(ss,a,',')) return false;
    if(!getline(ss,b,',')) return false;
    if(!getline(ss,c,',')) return false;

    try{
        t.ts=std::stoull(a);
        t.ask=std::stod(b);
        t.bid=std::stod(c);
    }catch(...){return false;}

    return true;
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

    Engine sweep,vwap,breakout;

    double vwap_mean=0;
    double vwap_var=0;

    double impulse_ref=0;

    double range_high=0;
    double range_low=0;

    uint64_t ticks=0;

    const double COST=0.45;
    const double TP=3.0;
    const double SL=2.5;

    auto start=std::chrono::high_resolution_clock::now();

    while(getline(in,line))
    {
        Tick t;

        if(!parse_tick(line,t))
            continue;

        ticks++;

        double mid=(t.ask+t.bid)/2;

        if(ticks==1)
        {
            impulse_ref=mid;
            range_high=mid;
            range_low=mid;
        }

        /* incremental VWAP stats */

        double delta=mid-vwap_mean;
        vwap_mean+=delta/((double)ticks);
        vwap_var+=delta*(mid-vwap_mean);

        double sigma=sqrt(vwap_var/ticks);

        double impulse=mid-impulse_ref;

        if(mid>range_high) range_high=mid;
        if(mid<range_low) range_low=mid;

        double range=range_high-range_low;

        /* SWEEP */

        if(!sweep.in)
        {
            if(impulse>4)
            {
                sweep.in=true;
                sweep.short_side=true;
                sweep.entry=t.bid;
                sweep.trades++;
            }

            if(impulse<-4)
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

            if(diff>=TP){sweep.pnl+=TP-COST; sweep.in=false;}
            if(diff<=-SL){sweep.pnl-=SL+COST; sweep.in=false;}
        }

        /* VWAP */

        if(!vwap.in)
        {
            if(mid>vwap_mean+2.5*sigma)
            {
                vwap.in=true;
                vwap.short_side=true;
                vwap.entry=t.bid;
                vwap.trades++;
            }

            if(mid<vwap_mean-2.5*sigma)
            {
                vwap.in=true;
                vwap.short_side=false;
                vwap.entry=t.ask;
                vwap.trades++;
            }
        }
        else
        {
            double diff=vwap.short_side ? vwap.entry-t.ask : t.bid-vwap.entry;

            if(diff>=TP){vwap.pnl+=TP-COST; vwap.in=false;}
            if(diff<=-SL){vwap.pnl-=SL+COST; vwap.in=false;}
        }

        /* BREAKOUT */

        if(!breakout.in && range<1.5)
        {
            if(mid>range_high)
            {
                breakout.in=true;
                breakout.short_side=false;
                breakout.entry=t.ask;
                breakout.trades++;
            }

            if(mid<range_low)
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

            if(diff>=TP){breakout.pnl+=TP-COST; breakout.in=false;}
            if(diff<=-SL){breakout.pnl-=SL+COST; breakout.in=false;}
        }

        if(ticks%20000000==0)
            std::cout<<"Processed "<<ticks<<" ticks\n";
    }

    auto end=std::chrono::high_resolution_clock::now();

    std::cout<<"\nTicks "<<ticks<<"\n\n";

    std::cout<<"SWEEP  "<<sweep.trades<<" trades  pnl "<<sweep.pnl<<"\n";
    std::cout<<"VWAP   "<<vwap.trades<<" trades  pnl "<<vwap.pnl<<"\n";
    std::cout<<"BREAK  "<<breakout.trades<<" trades  pnl "<<breakout.pnl<<"\n";

    std::cout<<"\nRuntime "
    <<std::chrono::duration<double>(end-start).count()<<" sec\n";
}

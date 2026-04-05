#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <cmath>

struct Tick
{
    uint64_t ts;
    double ask;
    double bid;
};

bool parse(const std::string& line, Tick& t)
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

int main(int argc,char** argv)
{
    if(argc<2)
    {
        std::cout<<"usage: scan ticks.csv\n";
        return 0;
    }

    std::ifstream in(argv[1]);
    std::string line;

    std::vector<double> mid;

    while(getline(in,line))
    {
        Tick t;
        if(!parse(line,t)) continue;

        mid.push_back((t.ask+t.bid)/2);
    }

    std::cout<<"Loaded "<<mid.size()<<" ticks\n";

    const int forward=2000;

    int impulse_events=0;
    int impulse_wins=0;

    int breakout_events=0;
    int breakout_wins=0;

    int vwap_events=0;
    int vwap_wins=0;

    double impulse_edge=0;
    double breakout_edge=0;
    double vwap_edge=0;

    for(size_t i=5000;i<mid.size()-forward;i++)
    {
        double p=mid[i];

        double impulse=p-mid[i-600];

        double high=p;
        double low=p;

        for(int j=0;j<1200;j++)
        {
            double v=mid[i-j];
            if(v>high) high=v;
            if(v<low) low=v;
        }

        double range=high-low;

        double future=mid[i+forward]-p;

        /* impulse event */

        if(fabs(impulse)>4)
        {
            impulse_events++;

            if((impulse>0 && future<0) ||
               (impulse<0 && future>0))
                impulse_wins++;

            impulse_edge+=future*(impulse>0?-1:1);
        }

        /* breakout */

        if(range<1.0)
        {
            breakout_events++;

            if(fabs(future)>2)
                breakout_wins++;

            breakout_edge+=future;
        }

        /* vwap deviation */

        double mean=0;

        for(int j=0;j<3000;j++)
            mean+=mid[i-j];

        mean/=3000;

        double dev=p-mean;

        if(fabs(dev)>3)
        {
            vwap_events++;

            if((dev>0 && future<0) ||
               (dev<0 && future>0))
                vwap_wins++;

            vwap_edge+=future*(dev>0?-1:1);
        }
    }

    std::cout<<"\nImpulse events "<<impulse_events
             <<" winrate "<<(double)impulse_wins/impulse_events
             <<" edge "<<impulse_edge/impulse_events<<"\n";

    std::cout<<"Breakout events "<<breakout_events
             <<" winrate "<<(double)breakout_wins/breakout_events
             <<" edge "<<breakout_edge/breakout_events<<"\n";

    std::cout<<"VWAP events "<<vwap_events
             <<" winrate "<<(double)vwap_wins/vwap_events
             <<" edge "<<vwap_edge/vwap_events<<"\n";
}

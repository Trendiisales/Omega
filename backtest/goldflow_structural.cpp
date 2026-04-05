#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
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

    std::stringstream ss(line);
    std::string tok;

    if(!getline(ss,tok,',')) return false;
    if(!isdigit(tok[0])) return false;

    t.ts = std::stoull(tok);

    getline(ss,tok,',');
    t.ask = std::stod(tok);

    getline(ss,tok,',');
    t.bid = std::stod(tok);

    return true;
}

int main(int argc,char** argv)
{
    if(argc<2)
    {
        std::cout<<"usage: structural_bt ticks.csv\n";
        return 0;
    }

    std::ifstream in(argv[1]);
    std::string line;

    std::vector<double> window;

    double pnl=0;
    int trades=0;

    const double MOVE_TRIGGER=1.8;
    const double TP=1.2;
    const double SL=0.9;

    double entry=0;
    bool in_trade=false;
    bool short_trade=false;

    auto start=std::chrono::high_resolution_clock::now();

    while(getline(in,line))
    {
        Tick t;

        if(!parse_tick(line,t))
            continue;

        double mid=(t.ask+t.bid)/2;

        window.push_back(mid);

        if(window.size()>600)
            window.erase(window.begin());

        if(window.size()<600)
            continue;

        double move = mid-window.front();

        if(!in_trade)
        {
            if(move>MOVE_TRIGGER)
            {
                entry=mid;
                short_trade=true;
                in_trade=true;
                trades++;
            }

            if(move<-MOVE_TRIGGER)
            {
                entry=mid;
                short_trade=false;
                in_trade=true;
                trades++;
            }
        }
        else
        {
            double diff = short_trade ?
                entry-mid :
                mid-entry;

            if(diff>=TP)
            {
                pnl+=TP;
                in_trade=false;
            }
            else if(diff<=-SL)
            {
                pnl-=SL;
                in_trade=false;
            }
        }
    }

    auto end=std::chrono::high_resolution_clock::now();
    double sec=std::chrono::duration<double>(end-start).count();

    std::cout<<"=============================\n";
    std::cout<<"Trades: "<<trades<<"\n";
    std::cout<<"PnL pts: "<<pnl<<"\n";
    std::cout<<"PnL USD: "<<pnl*100<<"\n";
    std::cout<<"Runtime: "<<sec<<" sec\n";
    std::cout<<"=============================\n";
}

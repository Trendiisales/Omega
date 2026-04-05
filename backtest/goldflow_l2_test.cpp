#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <cmath>
#include <cctype>

struct Tick
{
    uint64_t ts;
    double ask;
    double bid;
};

struct L2
{
    uint64_t ts;
    double bid_vol;
    double ask_vol;
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

bool parse_l2(const std::string& line, L2& l)
{
    if(line.empty()) return false;

    std::stringstream ss(line);
    std::string tok;

    getline(ss,tok,',');
    l.ts = std::stoull(tok);

    getline(ss,tok,',');
    l.bid_vol = std::stod(tok);

    getline(ss,tok,',');
    l.ask_vol = std::stod(tok);

    return true;
}

int main(int argc,char** argv)
{
    if(argc<3)
    {
        std::cout<<"usage: goldflow_l2 ticks.csv l2.csv\n";
        return 0;
    }

    std::ifstream tick_file(argv[1]);
    std::ifstream l2_file(argv[2]);

    std::string tick_line;
    std::string l2_line;

    L2 current_l2;

    double pnl=0;
    int trades=0;

    while(getline(tick_file,tick_line))
    {
        Tick t;

        if(!parse_tick(tick_line,t))
            continue;

        while(getline(l2_file,l2_line))
        {
            L2 l;

            if(!parse_l2(l2_line,l))
                continue;

            if(l.ts >= t.ts)
            {
                current_l2 = l;
                break;
            }
        }

        double mid=(t.ask+t.bid)/2;

        double imbalance = current_l2.bid_vol /
                           std::max(1.0,current_l2.ask_vol);

        if(imbalance > 1.6)
        {
            pnl += 2.0;
            trades++;
        }

        if(imbalance < 0.6)
        {
            pnl += 2.0;
            trades++;
        }
    }

    std::cout<<"Trades: "<<trades<<"\n";
    std::cout<<"PnL: "<<pnl<<"\n";
}

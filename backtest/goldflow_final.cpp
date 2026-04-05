#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <cmath>
#include <chrono>
#include <cctype>

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

    if(!isdigit(tok[0]))
        return false;

    try { t.ts = std::stoull(tok); }
    catch(...) { return false; }

    if(!getline(ss,tok,',')) return false;
    t.ask = std::stod(tok);

    if(!getline(ss,tok,',')) return false;
    t.bid = std::stod(tok);

    return true;
}

inline int utc_hour(uint64_t ts)
{
    return (ts/1000/3600)%24;
}

inline bool session_ok(uint64_t ts)
{
    int h = utc_hour(ts);

    if(h>=7 && h<=10) return true;
    if(h>=12 && h<=16) return true;

    return false;
}

struct Engine
{
    bool in_pos=false;
    bool long_side=false;

    double entry=0;
    double tp=0;
    double sl=0;

    uint64_t entry_ts=0;

    std::vector<double> price_buf;
    std::vector<double> vwap_buf;

    double vwap=0;
    double pv=0;
    double vol=0;

    double hi=0;
    double lo=0;

    static constexpr int WINDOW=60;

    static constexpr double IMPULSE=7.0;

    static constexpr double TP=14.0;
    static constexpr double SL=8.0;

    void update(double price)
    {
        pv+=price;
        vol+=1;
        vwap=pv/vol;

        price_buf.push_back(price);
        vwap_buf.push_back(vwap);

        if(price_buf.size()>WINDOW)
        {
            price_buf.erase(price_buf.begin());
            vwap_buf.erase(vwap_buf.begin());
        }
    }

    bool detect_impulse()
    {
        if(price_buf.size()<WINDOW)
            return false;

        hi=price_buf[0];
        lo=price_buf[0];

        for(double p:price_buf)
        {
            if(p>hi) hi=p;
            if(p<lo) lo=p;
        }

        return (hi-lo)>=IMPULSE;
    }

    bool vwap_trend_up()
    {
        if(vwap_buf.size()<30) return false;
        return (vwap_buf.back() - vwap_buf[vwap_buf.size()-30]) > 0.5;
    }

    bool vwap_trend_down()
    {
        if(vwap_buf.size()<30) return false;
        return (vwap_buf.back() - vwap_buf[vwap_buf.size()-30]) < -0.5;
    }
};

int main(int argc,char** argv)
{
    if(argc<2)
    {
        std::cout<<"usage: goldflow_final ticks.csv\n";
        return 0;
    }

    std::ifstream file(argv[1]);

    if(!file.is_open())
    {
        std::cout<<"cannot open file\n";
        return 0;
    }

    Engine e;

    uint64_t ticks=0;
    int trades=0;
    double pnl=0;

    std::string line;

    auto start=std::chrono::high_resolution_clock::now();

    while(getline(file,line))
    {
        Tick t;

        if(!parse_tick(line,t))
            continue;

        ticks++;

        double mid=(t.ask+t.bid)*0.5;

        e.update(mid);

        if(!session_ok(t.ts))
            continue;

        if(!e.in_pos && e.detect_impulse())
        {
            double range=e.hi-e.lo;

            double pb_long = e.hi - 0.55*range;
            double pb_short = e.lo + 0.55*range;

            if(mid<=pb_long && mid>e.vwap && e.vwap_trend_up())
            {
                e.in_pos=true;
                e.long_side=true;

                e.entry=t.ask;
                e.tp=e.entry+Engine::TP;
                e.sl=e.entry-Engine::SL;

                e.entry_ts=t.ts;
                trades++;
            }

            if(mid>=pb_short && mid<e.vwap && e.vwap_trend_down())
            {
                e.in_pos=true;
                e.long_side=false;

                e.entry=t.bid;
                e.tp=e.entry-Engine::TP;
                e.sl=e.entry+Engine::SL;

                e.entry_ts=t.ts;
                trades++;
            }
        }

        if(e.in_pos)
        {
            if(e.long_side)
            {
                double exit=t.bid;

                if(exit>=e.tp || exit<=e.sl || t.ts-e.entry_ts>1800)
                {
                    pnl+=exit-e.entry;
                    e.in_pos=false;
                }
            }
            else
            {
                double exit=t.ask;

                if(exit<=e.tp || exit>=e.sl || t.ts-e.entry_ts>1800)
                {
                    pnl+=e.entry-exit;
                    e.in_pos=false;
                }
            }
        }
    }

    auto end=std::chrono::high_resolution_clock::now();
    double sec=std::chrono::duration<double>(end-start).count();

    std::cout<<"=============================\n";
    std::cout<<"Ticks: "<<ticks<<"\n";
    std::cout<<"Trades: "<<trades<<"\n";
    std::cout<<"PnL pts: "<<pnl<<"\n";
    std::cout<<"PnL USD: "<<pnl*100<<"\n";
    std::cout<<"Runtime: "<<sec<<" sec\n";
    std::cout<<"=============================\n";
}

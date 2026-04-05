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

    try
    {
        t.ts = std::stoull(tok);
    }
    catch(...)
    {
        return false;
    }

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

struct GoldFlowEngine
{
    bool in_pos=false;
    bool long_side=false;

    double entry=0;
    double tp=0;
    double sl=0;

    uint64_t entry_ts=0;

    std::vector<double> buf;

    double vwap=0;
    double pv=0;
    double vol=0;

    double impulse_high=0;
    double impulse_low=0;

    bool impulse_active=false;

    static constexpr int IMPULSE_WINDOW=30;
    static constexpr double IMPULSE_THRESHOLD=6.0;

    static constexpr double TP=16.0;
    static constexpr double SL=8.0;

    void update_vwap(double price)
    {
        pv += price;
        vol += 1;
        vwap = pv / vol;
    }

    void update_buffer(double price)
    {
        buf.push_back(price);
        if(buf.size()>IMPULSE_WINDOW)
            buf.erase(buf.begin());
    }

    double impulse_size()
    {
        if(buf.size()<IMPULSE_WINDOW)
            return 0;

        double hi=buf[0];
        double lo=buf[0];

        for(double p:buf)
        {
            if(p>hi) hi=p;
            if(p<lo) lo=p;
        }

        impulse_high=hi;
        impulse_low=lo;

        return hi-lo;
    }

    void reset_impulse()
    {
        impulse_active=false;
    }
};

int main(int argc,char** argv)
{
    if(argc<2)
    {
        std::cout<<"usage: goldflow_bt ticks.csv\n";
        return 0;
    }

    std::ifstream file(argv[1]);

    if(!file.is_open())
    {
        std::cout<<"cannot open file\n";
        return 0;
    }

    GoldFlowEngine e;

    double pnl=0;
    int trades=0;
    uint64_t ticks=0;

    std::string line;

    auto start=std::chrono::high_resolution_clock::now();

    while(getline(file,line))
    {
        Tick t;

        if(!parse_tick(line,t))
            continue;

        ticks++;

        double mid=(t.ask+t.bid)*0.5;

        e.update_vwap(mid);
        e.update_buffer(mid);

        if(!session_ok(t.ts))
            continue;

        double impulse=e.impulse_size();

        if(!e.impulse_active && impulse>GoldFlowEngine::IMPULSE_THRESHOLD)
        {
            e.impulse_active=true;
        }

        if(!e.in_pos && e.impulse_active)
        {
            double pb_long = e.impulse_high - 0.35*(e.impulse_high-e.impulse_low);

            if(mid<=pb_long && mid>e.vwap)
            {
                e.in_pos=true;
                e.long_side=true;

                e.entry=t.ask;
                e.tp=e.entry+GoldFlowEngine::TP;
                e.sl=e.entry-GoldFlowEngine::SL;

                e.entry_ts=t.ts;
                trades++;

                e.reset_impulse();
            }

            double pb_short = e.impulse_low + 0.35*(e.impulse_high-e.impulse_low);

            if(mid>=pb_short && mid<e.vwap)
            {
                e.in_pos=true;
                e.long_side=false;

                e.entry=t.bid;
                e.tp=e.entry-GoldFlowEngine::TP;
                e.sl=e.entry+GoldFlowEngine::SL;

                e.entry_ts=t.ts;
                trades++;

                e.reset_impulse();
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

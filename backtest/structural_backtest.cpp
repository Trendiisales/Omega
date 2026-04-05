#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
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

inline bool tradable_session(uint64_t ts)
{
    int h = utc_hour(ts);

    if(h>=7 && h<=11) return true;
    if(h>=13 && h<=17) return true;

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

    double vwap=0;
    double pv=0;
    double vol=0;

    double impulse_high=0;
    double impulse_low=0;

    static constexpr double MIN_IMPULSE=12.0;
    static constexpr double PULLBACK_MAX=4.0;
    static constexpr double VWAP_DEV=2.0;

    void update_vwap(double price)
    {
        pv+=price;
        vol+=1;
        vwap=pv/vol;
    }

    void update_impulse(double price)
    {
        if(impulse_high==0)
        {
            impulse_high=price;
            impulse_low=price;
            return;
        }

        if(price>impulse_high) impulse_high=price;
        if(price<impulse_low) impulse_low=price;
    }

    bool impulse_ready()
    {
        return (impulse_high-impulse_low)>=MIN_IMPULSE;
    }

    bool pullback_long(double price)
    {
        return (impulse_high-price)<=PULLBACK_MAX;
    }

    bool pullback_short(double price)
    {
        return (price-impulse_low)<=PULLBACK_MAX;
    }

    void reset_impulse(double price)
    {
        impulse_high=price;
        impulse_low=price;
    }
};

int main(int argc,char** argv)
{
    if(argc<2)
    {
        std::cout<<"usage: structural_bt ticks.csv\n";
        return 0;
    }

    std::ifstream file(argv[1]);

    if(!file.is_open())
    {
        std::cout<<"cannot open file\n";
        return 0;
    }

    Engine e;

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
        e.update_impulse(mid);

        if(!tradable_session(t.ts))
            continue;

        if(!e.in_pos)
        {
            if(!e.impulse_ready())
                continue;

            if(mid>e.vwap && e.pullback_long(mid))
            {
                e.in_pos=true;
                e.long_side=true;

                e.entry=t.ask;
                e.tp=e.entry+10;
                e.sl=e.entry-5;

                e.entry_ts=t.ts;
                trades++;

                e.reset_impulse(mid);
            }
            else if(mid<e.vwap && e.pullback_short(mid))
            {
                e.in_pos=true;
                e.long_side=false;

                e.entry=t.bid;
                e.tp=e.entry-10;
                e.sl=e.entry+5;

                e.entry_ts=t.ts;
                trades++;

                e.reset_impulse(mid);
            }
        }
        else
        {
            if(e.long_side)
            {
                double exit=t.bid;

                if(exit>=e.tp || exit<=e.sl || t.ts-e.entry_ts>1800)
                {
                    pnl += exit-e.entry;
                    e.in_pos=false;
                }
            }
            else
            {
                double exit=t.ask;

                if(exit<=e.tp || exit>=e.sl || t.ts-e.entry_ts>1800)
                {
                    pnl += e.entry-exit;
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

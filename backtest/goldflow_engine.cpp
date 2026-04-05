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
    if(!isdigit(tok[0])) return false;

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
    bool short_side=false;

    double entry=0;
    double tp=0;
    double sl=0;

    double entry_spread=0;

    uint64_t entry_ts=0;

    int cooldown=0;

    std::vector<double> buf;

    double hi=0;
    double lo=0;

    static constexpr int WINDOW=60;
    static constexpr double IMPULSE=8.0;

    void update(double price)
    {
        buf.push_back(price);
        if(buf.size()>WINDOW)
            buf.erase(buf.begin());
    }

    bool detect_impulse()
    {
        if(buf.size()<WINDOW)
            return false;

        hi=buf[0];
        lo=buf[0];

        for(double p:buf)
        {
            if(p>hi) hi=p;
            if(p<lo) lo=p;
        }

        return (hi-lo)>=IMPULSE;
    }
};

int main(int argc,char** argv)
{
    if(argc<2)
    {
        std::cout<<"usage: goldflow_engine ticks.csv\n";
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

        double mid = (t.ask+t.bid)*0.5;
        double spread = t.ask - t.bid;

        if(spread > 0.40)
            continue;

        e.update(mid);

        if(!session_ok(t.ts))
            continue;

        if(e.cooldown>0)
        {
            e.cooldown--;
            continue;
        }

        if(!e.in_pos && e.detect_impulse())
        {
            double impulse = e.hi - e.lo;

            if(impulse < spread*3)
                continue;

            if(mid > e.hi - 0.2*impulse)
            {
                e.in_pos=true;
                e.short_side=true;

                e.entry = mid;
                e.tp = e.entry - 10;
                e.sl = e.entry + 6;

                e.entry_spread = spread;
                e.entry_ts = t.ts;

                trades++;
            }

            if(mid < e.lo + 0.2*impulse)
            {
                e.in_pos=true;
                e.short_side=false;

                e.entry = mid;
                e.tp = e.entry + 10;
                e.sl = e.entry - 6;

                e.entry_spread = spread;
                e.entry_ts = t.ts;

                trades++;
            }
        }

        if(e.in_pos)
        {
            double exit = mid;

            if(e.short_side)
            {
                if(exit <= e.tp || exit >= e.sl || t.ts-e.entry_ts > 1800)
                {
                    double gross = e.entry - exit;
                    double net = gross - e.entry_spread;

                    pnl += net;

                    e.in_pos=false;
                    e.cooldown=300;
                }
            }
            else
            {
                if(exit >= e.tp || exit <= e.sl || t.ts-e.entry_ts > 1800)
                {
                    double gross = exit - e.entry;
                    double net = gross - e.entry_spread;

                    pnl += net;

                    e.in_pos=false;
                    e.cooldown=300;
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

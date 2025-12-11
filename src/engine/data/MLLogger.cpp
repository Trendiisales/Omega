#include "MLLogger.hpp"
#include <iomanip>

namespace Omega {

MLLogger::MLLogger() : ready(false) {}
MLLogger::~MLLogger() { close(); }

bool MLLogger::open(const std::string& path)
{
    std::lock_guard<std::mutex> lock(mtx);

    file.open(path, std::ios::out | std::ios::trunc);
    if(!file.is_open()){
        ready = false;
        return false;
    }

    ready = true;
    writeHeader();
    return true;
}

void MLLogger::close()
{
    std::lock_guard<std::mutex> lock(mtx);
    if(file.is_open()) file.close();
    ready = false;
}

void MLLogger::writeHeader()
{
    file<<"ts,bid,ask,spread,delta,buyVol,sellVol,liqGap,b1,b2,a1,a2";

    for(int i=1;i<=10;i++){
        file<<",bpx"<<i<<",bsz"<<i<<",apx"<<i<<",asz"<<i;
    }

    for(int i=0;i<64;i++){
        file<<",micro"<<i;
    }

    for(int i=0;i<32;i++){
        file<<",strat"<<i;
    }

    file<<",fused";

    file<<",pnl,equity,latency,regime,throttle,shock,engineTS";

    file<<"\n";
}

bool MLLogger::write(const Tick& t,
                     const OrderBook& ob,
                     const MicroState& ms,
                     const StrategyState32& st,
                     const EngineState& es)
{
    if(!ready) return false;

    std::lock_guard<std::mutex> lock(mtx);

    file<<t.ts<<","<<t.bid<<","<<t.ask<<","<<t.spread<<","<<t.delta<<","
        <<t.buyVol<<","<<t.sellVol<<","<<t.liquidityGap<<","
        <<t.b1<<","<<t.b2<<","<<t.a1<<","<<t.a2;

    for(int i=0;i<10;i++){
        file<<","<<ob.bidPrice[i]<<","<<ob.bidSize[i]
            <<","<<ob.askPrice[i]<<","<<ob.askSize[i];
    }

    for(int i=0;i<64;i++){
        file<<","<<ms.v[i];
    }

    for(int i=0;i<32;i++){
        file<<","<<st.s[i];
    }

    file<<","<<st.fused;

    file<<","<<es.pnl
        <<","<<es.equity
        <<","<<es.latency
        <<","<<es.regime
        <<","<<es.throttle
        <<","<<es.shock
        <<","<<es.ts;

    file<<"\n";
    return true;
}

}

#include "SymbolThread.hpp"
#include "../../strategy/Decision.hpp"

namespace Omega {

SymbolThread::SymbolThread()
    : running(false) {}

SymbolThread::~SymbolThread() {
    stop();
}

void SymbolThread::init(const std::string& s,
                        StrategyFusion* f,
                        TickPipelineExt* p,
                        PositionTracker* pt,
                        RiskEngine* r,
                        ExecutionSupervisor* e) {
    symbol   = s;
    fusion   = f;
    pipeline = p;
    pos      = pt;
    risk     = r;
    exec     = e;
}

void SymbolThread::start() {
    if (running) return;
    running = true;
    th = std::thread(&SymbolThread::loop, this);
}

void SymbolThread::stop() {
    running = false;
    if (th.joinable()) th.join();
}

void SymbolThread::pushTick(const Tick& t) {
    std::lock_guard<std::mutex> g(qLock);
    lastTick = t;
    hasTick  = true;
}

void SymbolThread::pushBook(const OrderBook& ob) {
    std::lock_guard<std::mutex> g(qLock);
    lastBook = ob;
    hasBook  = true;
}

void SymbolThread::loop() {
    while (running) {
        Tick t;
        OrderBook ob;
        bool lt = false;
        bool lb = false;

        {
            std::lock_guard<std::mutex> g(qLock);
            if (hasTick) {
                t      = lastTick;
                lt     = true;
                hasTick = false;
            }
            if (hasBook) {
                ob     = lastBook;
                lb     = true;
                hasBook = false;
            }
        }

        if (lt && pipeline && fusion && pos && risk && exec) {
            pipeline->pushTick(t);
            MicroMetrics m;
            if (pipeline->compute(m)) {
                Decision d = fusion->compute(t, m, *pos);
                if (d.valid && risk->allow(symbol, d)) {
                    OrderIntent o;
                    o.symbol = symbol;
                    o.side   = (d.side == Side::Buy) ? OrderSide::BUY : OrderSide::SELL;
                    o.qty    = d.qty;
                    o.price  = d.price;
                    o.ts     = d.ts;
                    exec->route(o);
                    risk->onOrder(o);
                }
            }
        }

        if (lb && pipeline) {
            pipeline->pushBook(ob);
            MicroMetrics m2;
            pipeline->computeBook(m2);
        }
    }
}

} // namespace Omega

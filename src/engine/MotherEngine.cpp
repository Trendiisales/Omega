#include "MotherEngine.hpp"
#include "../strategy/Decision.hpp"

namespace Omega {

MotherEngine::MotherEngine()
    : running(false) {
    state.reset();
}

void MotherEngine::loadSymbols() {
    for (const auto& s : cfg.symbols) {
        posTracker[s] = PositionTracker();
        posTracker[s].setSymbol(s);
    }
}

void MotherEngine::buildPipelines() {
    for (const auto& s : cfg.symbols) {
        auto* p = new TickPipelineExt();
        p->init(s);
        pipe[s] = p;
    }
}

void MotherEngine::buildFusion() {
    for (const auto& s : cfg.symbols) {
        auto* f = new StrategyFusion();
        f->init(s, cfg.strategySet);
        strat[s] = f;
    }
}

void MotherEngine::buildRisk() {
    risk.init(cfg.riskConfig, &posTracker);
}

void MotherEngine::buildExec() {
    execSup.init(cfg.execConfig);
}

bool MotherEngine::init(const EngineConfig& cfgIn) {
    cfg = cfgIn;
    loadSymbols();
    buildPipelines();
    buildFusion();
    buildRisk();
    buildExec();
    running = false;
    state.mode = EngineMode::Idle;
    return true;
}

void MotherEngine::start() {
    if (running) return;
    running = true;
    state.mode = EngineMode::Running;
    state.lastStartTs = cfg.wallClockNow();
    mainThread = std::thread(&MotherEngine::mainLoop, this);
}

void MotherEngine::stop() {
    running = false;
    state.mode = EngineMode::Stopping;
    if (mainThread.joinable())
        mainThread.join();
    finalize();
    state.mode = EngineMode::Stopped;
}

void MotherEngine::onExternalTick(const std::string& symbol, const Tick& t) {
    {
        std::lock_guard<std::mutex> g(tickLock);
        lastTick[symbol] = t;
    }
    auto it = pipe.find(symbol);
    if (it != pipe.end() && it->second)
        it->second->pushTick(t);
    state.tickCount++;
}

void MotherEngine::onExternalBook(const std::string& symbol, const OrderBook& ob) {
    {
        std::lock_guard<std::mutex> g(bookLock);
        lastBook[symbol] = ob;
    }
    auto it = pipe.find(symbol);
    if (it != pipe.end() && it->second)
        it->second->pushBook(ob);
    state.bookCount++;
}

void MotherEngine::onExternalExec(const ExecReport& r) {
    auto it = posTracker.find(r.symbol);
    if (it != posTracker.end()) {
        it->second.update(r);
    }
    risk.onExecution(r);
    execSup.onExecution(r);
    state.execCount++;
}

void MotherEngine::onExternalReject(const FIXRejectInfo& r) {
    execSup.onReject(r);
    state.rejectCount++;
}

void MotherEngine::mainLoop() {
    while (running) {
        for (const auto& s : cfg.symbols) {
            Tick t;
            OrderBook ob;
            bool hasTick = false;
            bool hasBook = false;

            {
                std::lock_guard<std::mutex> g(tickLock);
                auto it = lastTick.find(s);
                if (it != lastTick.end()) {
                    t = it->second;
                    hasTick = true;
                }
            }

            {
                std::lock_guard<std::mutex> g(bookLock);
                auto it = lastBook.find(s);
                if (it != lastBook.end()) {
                    ob = it->second;
                    hasBook = true;
                }
            }

            if (hasTick) processTick(s, t);
            if (hasBook) processBook(s, ob);
        }

        state.mainLoopIterations++;
        cfg.yieldHook();
    }

    finalize();
}

void MotherEngine::processTick(const std::string& symbol, const Tick& t) {
    auto pit = pipe.find(symbol);
    auto sit = strat.find(symbol);
    if (pit == pipe.end() || sit == strat.end()) return;
    auto* p = pit->second;
    auto* f = sit->second;
    if (!p || !f) return;

    MicroMetrics metrics;
    if (!p->compute(metrics)) return;

    auto& pt = posTracker[symbol];
    Decision d = f->compute(t, metrics, pt);

    if (!d.valid) return;
    if (!risk.allow(symbol, d)) {
        state.riskBlocked++;
        return;
    }

    processDecision(symbol, d);
}

void MotherEngine::processBook(const std::string& symbol, const OrderBook& ob) {
    auto pit = pipe.find(symbol);
    if (pit == pipe.end()) return;
    auto* p = pit->second;
    if (!p) return;

    MicroMetrics m;
    p->computeBook(m);
    state.lastDepthUpdateTs = cfg.monotonicNow();
}

void MotherEngine::processDecision(const std::string& symbol, const Decision& d) {
    OrderIntent o;
    o.symbol = symbol;
    o.side   = (d.side == Side::Buy) ? OrderSide::BUY : OrderSide::SELL;
    o.qty    = d.qty;
    o.price  = d.price;
    o.ts     = d.ts;
    routeOrder(symbol, o);
}

void MotherEngine::routeOrder(const std::string& symbol, const OrderIntent& o) {
    execSup.route(o);
    risk.onOrder(o);
    state.ordersSent++;
}

void MotherEngine::finalize() {
    for (auto& kv : strat) {
        delete kv.second;
        kv.second = nullptr;
    }
    strat.clear();

    for (auto& kv : pipe) {
        delete kv.second;
        kv.second = nullptr;
    }
    pipe.clear();
}

} // namespace Omega

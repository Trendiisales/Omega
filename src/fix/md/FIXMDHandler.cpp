#include "FIXMDHandler.hpp"
#include <chrono>

namespace Omega {

static uint64_t md_ts() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

FIXMDHandler::FIXMDHandler()
    : tr(nullptr) {}

FIXMDHandler::~FIXMDHandler() {}

void FIXMDHandler::attach(FIXTransport* t) {
    tr = t;
    if (!tr) return;

    tr->setRxCallback([this](const std::string& msg){
        onRx(msg);
    });
}

void FIXMDHandler::setUpdateCallback(std::function<void(const MDUpdate&)> cb) {
    onUpdate = cb;
}

void FIXMDHandler::onRx(const std::string& msg) {
    FIXParser p;
    auto tags = p.parse(msg);

    auto it = tags.find("35");
    if (it == tags.end()) return;
    
    std::string type = it->second;

    // Market Data Snapshot 35=W
    // Market Data Incremental 35=X
    if (type == "W" || type == "X") {
        MDUpdate u = parseMD(tags);
        if (onUpdate) onUpdate(u);
    }
}

MDUpdate FIXMDHandler::parseMD(const std::unordered_map<std::string,std::string>& t) {
    MDUpdate m;

    auto get = [&](const std::string& key) -> std::string {
        auto it = t.find(key);
        return (it == t.end()) ? "" : it->second;
    };
    
    auto getDouble = [&](const std::string& key) -> double {
        std::string v = get(key);
        if (v.empty()) return 0.0;
        try { return std::stod(v); }
        catch (...) { return 0.0; }
    };

    m.symbol = get("55");

    m.last     = getDouble("270");
    m.lastSize = getDouble("271");

    std::string entryType = get("269");
    if (entryType == "0") { // bid
        m.bid = getDouble("270");
        m.bidSize = getDouble("271");
    }
    else if (entryType == "1") { // ask
        m.ask = getDouble("270");
        m.askSize = getDouble("271");
    }

    m.ts = md_ts();
    return m;
}

} // namespace Omega

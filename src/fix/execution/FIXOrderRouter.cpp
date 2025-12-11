#include "FIXOrderRouter.hpp"
#include <chrono>

namespace Omega {

static uint64_t ts_now() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

FIXOrderRouter::FIXOrderRouter()
    : tr(nullptr),
      counter(1000) {}

FIXOrderRouter::~FIXOrderRouter() {}

void FIXOrderRouter::attach(FIXTransport* t) {
    tr = t;
    if (!tr) return;

    tr->setRxCallback([this](const std::string& msg){
        onRx(msg);
    });
}

void FIXOrderRouter::setAckCallback(std::function<void(const RoutedOrder&)> cb) {
    onAck = cb;
}

void FIXOrderRouter::setFillCallback(std::function<void(const RoutedOrder&, double, double)> cb) {
    onFill = cb;
}

void FIXOrderRouter::setCancelAckCallback(std::function<void(const std::string&)> cb) {
    onCancelAck = cb;
}

std::string FIXOrderRouter::sendLimit(const std::string& symbol,
                                      const std::string& side,
                                      double qty,
                                      double price)
{
    if (!tr) return "";

    std::string clId = "OR" + std::to_string(counter++);

    std::string msg =
        "8=FIX.4.4\x01"
        "35=D\x01"
        "55=" + symbol + "\x01"
        "38=" + std::to_string(qty) + "\x01"
        "44=" + std::to_string(price) + "\x01"
        "54=" + side + "\x01"
        "11=" + clId + "\x01";

    tr->sendRaw(msg);
    return clId;
}

bool FIXOrderRouter::sendCancel(const std::string& clOrdId) {
    if (!tr) return false;

    std::string msg =
        "8=FIX.4.4\x01"
        "35=F\x01"
        "11=" + clOrdId + "\x01";

    return tr->sendRaw(msg);
}

void FIXOrderRouter::onRx(const std::string& msg) {
    FIXParser p;
    auto tags = p.parse(msg);

    auto it = tags.find("35");
    if (it != tags.end() && it->second == "8") {
        handleExec(tags);
    }
}

void FIXOrderRouter::handleExec(const std::unordered_map<std::string,std::string>& t) {
    // Safe tag accessor
    auto get = [&](const std::string& key) -> std::string {
        auto it = t.find(key);
        return (it != t.end()) ? it->second : "";
    };
    
    auto getDouble = [&](const std::string& key) -> double {
        std::string v = get(key);
        return v.empty() ? 0.0 : std::stod(v);
    };

    RoutedOrder r;
    r.symbol  = get("55");
    r.clOrdId = get("11");
    r.side    = get("54");
    r.ts      = ts_now();
    r.qty     = getDouble("38");
    r.price   = getDouble("44");

    std::string status = get("39");
    if (status.empty()) return;  // No status = invalid exec report

    if (status == "0") {       // new / ack
        if (onAck) onAck(r);
    }
    else if (status == "1" || status == "2") {  // partial or full fill
        double fillQty = getDouble("14");
        double fillPx  = getDouble("44");

        if (onFill) onFill(r, fillQty, fillPx);
    }
    else if (status == "4") {  // cancel ack
        if (onCancelAck) onCancelAck(r.clOrdId);
    }
}

} // namespace Omega

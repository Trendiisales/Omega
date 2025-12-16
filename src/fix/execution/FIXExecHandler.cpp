#include "FIXExecHandler.hpp"
#include <chrono>

namespace Omega {

static uint64_t now_ts() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
           ).count();
}

FIXExecHandler::FIXExecHandler()
    : tr(nullptr), clCounter(1) {}

FIXExecHandler::~FIXExecHandler() {}

void FIXExecHandler::attach(FIXTransport* t) {
    tr = t;
    if (!tr) return;

    tr->setRxCallback([this](const std::string& msg) {
        onRx(msg);
    });
}

void FIXExecHandler::setExecCallback(std::function<void(const ExecReport&)> cb) {
    onExec = cb;
}

void FIXExecHandler::setRejectCallback(std::function<void(const ExecReport&)> cb) {
    onReject = cb;
}

bool FIXExecHandler::sendNewOrder(const std::string& symbol,
                                  const std::string& side,
                                  double qty,
                                  double price)
{
    if (!tr) return false;

    std::string clId = "CL" + std::to_string(clCounter++);
    std::string msg =
        "8=FIX.4.4\x01"
        "35=D\x01"
        "55=" + symbol + "\x01"
        "38=" + std::to_string(qty) + "\x01"
        "44=" + std::to_string(price) + "\x01"
        "54=" + side + "\x01"
        "11=" + clId + "\x01";

    return tr->sendRaw(msg);
}

bool FIXExecHandler::sendCancel(const std::string& clOrdId) {
    if (!tr) return false;

    std::string msg =
        "8=FIX.4.4\x01"
        "35=F\x01"
        "11=" + clOrdId + "\x01";

    return tr->sendRaw(msg);
}

ExecReport FIXExecHandler::parseExec(const std::unordered_map<std::string,std::string>& t) {
    ExecReport r;
    auto get = [&](const std::string& k) -> std::string {
        auto it = t.find(k);
        return (it != t.end() ? it->second : "");
    };

    r.symbol  = get("55");
    r.orderId = get("37");
    r.clOrdId = get("11");
    r.execId  = get("17");
    r.side    = get("54");
    r.status  = get("39");

    r.price  = std::stod(get("44").empty() ? "0" : get("44"));
    r.filled = std::stod(get("14").empty() ? "0" : get("14"));
    r.leaves = std::stod(get("151").empty() ? "0" : get("151"));

    r.ts = now_ts();
    return r;
}

void FIXExecHandler::onRx(const std::string& msg) {
    FIXParser p;
    auto tags = p.parse(msg);

    std::string type = tags["35"];

    if (type == "8") {  // Execution Report
        ExecReport r = parseExec(tags);
        if (r.status == "8") {  // reject
            if (onReject) onReject(r);
        } else {
            if (onExec) onExec(r);
        }
    }
}

} // namespace Omega

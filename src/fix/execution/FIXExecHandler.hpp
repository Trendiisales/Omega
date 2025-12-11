#pragma once
#include <string>
#include <functional>
#include <unordered_map>
#include <cstdint>
#include "../transport/FIXTransport.hpp"
#include "../codec/FIXParser.hpp"

namespace Omega {

struct ExecReport {
    std::string symbol;
    std::string orderId;
    std::string clOrdId;
    std::string execId;
    double price = 0.0;
    double filled = 0.0;
    double leaves = 0.0;
    double qty = 0.0;
    std::string side;
    std::string status;
    std::uint64_t ts = 0;
};

class FIXExecHandler {
public:
    FIXExecHandler();
    ~FIXExecHandler();

    void attach(FIXTransport* t);

    void setExecCallback(std::function<void(const ExecReport&)> cb);
    void setRejectCallback(std::function<void(const ExecReport&)> cb);

    bool sendNewOrder(const std::string& symbol,
                      const std::string& side,
                      double qty,
                      double price);

    bool sendCancel(const std::string& clOrdId);

private:
    void onRx(const std::string& msg);
    ExecReport parseExec(const std::unordered_map<std::string,std::string>& tags);

private:
    FIXTransport* tr;
    std::function<void(const ExecReport&)> onExec;
    std::function<void(const ExecReport&)> onReject;

    std::uint64_t clCounter;
};

} // namespace Omega

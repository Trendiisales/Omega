#pragma once
#include <string>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include "../transport/FIXTransport.hpp"
#include "../codec/FIXParser.hpp"

namespace Omega {

struct MDUpdate {
    std::string symbol;
    double bid = 0.0;
    double ask = 0.0;
    double last = 0.0;
    double bidSize = 0.0;
    double askSize = 0.0;
    double lastSize = 0.0;
    uint64_t ts = 0;
};

class FIXMDHandler {
public:
    FIXMDHandler();
    ~FIXMDHandler();

    void attach(FIXTransport* t);

    void setUpdateCallback(std::function<void(const MDUpdate&)> cb);

private:
    void onRx(const std::string& msg);
    MDUpdate parseMD(const std::unordered_map<std::string,std::string>& t);

private:
    FIXTransport* tr;
    std::function<void(const MDUpdate&)> onUpdate;
};

} // namespace Omega

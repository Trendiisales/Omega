#include "BinanceMarketData.hpp"
#include "../json/Json.hpp"
#include <chrono>

namespace Omega {

static uint64_t md_ts() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

BinanceMarketData::BinanceMarketData()
    : ws(nullptr) {}

BinanceMarketData::~BinanceMarketData() {}

void BinanceMarketData::attachWS(WebSocketClient* w) {
    ws = w;
    if (!ws) return;

    ws->setMessageCallback([this](const std::string& m){
        onWs(m);
    });
}

void BinanceMarketData::setCallback(std::function<void(const BinanceTick&)> cb) {
    onTick = cb;
}

void BinanceMarketData::subscribe(const std::string& symbol) {
    if (!ws) return;

    std::string req =
        "{"
        "\"method\":\"SUBSCRIBE\","
        "\"params\":[\"" + symbol + "@bookTicker\"],"
        "\"id\":10"
        "}";

    ws->send(req);
}

BinanceTick BinanceMarketData::parse(const std::string& json) {
    BinanceTick t;
    t.ts = md_ts();

    auto j = Json::parse(json);

    if (!j.is_object()) return t;

    // Handle direct data or wrapped in "data" object
    const JsonValue* data = &j;
    if (j["data"].is_object()) {
        data = &j["data"];
    }

    if ((*data)["s"].is_string()) t.symbol = (*data)["s"].get_string();
    if ((*data)["b"].is_string()) {
        try { t.bid = std::stod((*data)["b"].get_string()); } catch (...) {}
    }
    if ((*data)["a"].is_string()) {
        try { t.ask = std::stod((*data)["a"].get_string()); } catch (...) {}
    }
    if ((*data)["B"].is_string()) {
        try { t.bidSize = std::stod((*data)["B"].get_string()); } catch (...) {}
    }
    if ((*data)["A"].is_string()) {
        try { t.askSize = std::stod((*data)["A"].get_string()); } catch (...) {}
    }

    return t;
}

void BinanceMarketData::onWs(const std::string& msg) {
    BinanceTick t = parse(msg);
    if (onTick) onTick(t);
}

} // namespace Omega

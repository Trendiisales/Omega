#include "BinanceOrderManager.hpp"
#include <chrono>
#include <sstream>
#include "../json/Json.hpp"

namespace Omega {

static uint64_t t_ms() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

BinanceOrderManager::BinanceOrderManager()
    : ws(nullptr),
      counter(1) {}

BinanceOrderManager::~BinanceOrderManager() {}

void BinanceOrderManager::setKeys(const std::string& a, const std::string& s) {
    apiKey = a;
    hmac.setSecret(s);
}

void BinanceOrderManager::attachWS(WebSocketClient* w) {
    ws = w;
    if (!ws) return;

    ws->setMessageCallback([this](const std::string& m){
        onWsMessage(m);
    });
}

void BinanceOrderManager::setOrderCallback(std::function<void(const BinanceOrder&)> cb) {
    onOrder = cb;
}

void BinanceOrderManager::setErrorCallback(std::function<void(const std::string&)> cb) {
    onError = cb;
}

std::string BinanceOrderManager::generateClientId() {
    return "OM" + std::to_string(t_ms()) + "_" + std::to_string(counter++);
}

std::string BinanceOrderManager::sendLimit(const std::string& symbol,
                                           const std::string& side,
                                           double qty,
                                           double price)
{
    if (!ws) return "";

    std::string cid = generateClientId();

    std::ostringstream ss;
    ss << "{"
       << "\"method\":\"order.place\","
       << "\"params\":{"
       << "\"symbol\":\"" << symbol << "\","
       << "\"side\":\"" << side << "\","
       << "\"type\":\"LIMIT\","
       << "\"timeInForce\":\"GTC\","
       << "\"price\":\"" << price << "\","
       << "\"quantity\":\"" << qty << "\","
       << "\"newClientOrderId\":\"" << cid << "\""
       << "},"
       << "\"id\":" << counter
       << "}";

    ws->send(ss.str());
    
    // Track order
    {
        std::lock_guard<std::mutex> g(lock);
        BinanceOrder o;
        o.clientId = cid;
        o.symbol = symbol;
        o.side = side;
        o.qty = qty;
        o.price = price;
        o.status = "PENDING";
        o.ts = t_ms();
        orders[cid] = o;
    }
    
    return cid;
}

std::string BinanceOrderManager::sendMarket(const std::string& symbol,
                                            const std::string& side,
                                            double qty)
{
    if (!ws) return "";

    std::string cid = generateClientId();

    std::ostringstream ss;
    ss << "{"
       << "\"method\":\"order.place\","
       << "\"params\":{"
       << "\"symbol\":\"" << symbol << "\","
       << "\"side\":\"" << side << "\","
       << "\"type\":\"MARKET\","
       << "\"quantity\":\"" << qty << "\","
       << "\"newClientOrderId\":\"" << cid << "\""
       << "},"
       << "\"id\":" << counter
       << "}";

    ws->send(ss.str());
    
    {
        std::lock_guard<std::mutex> g(lock);
        BinanceOrder o;
        o.clientId = cid;
        o.symbol = symbol;
        o.side = side;
        o.qty = qty;
        o.status = "PENDING";
        o.ts = t_ms();
        orders[cid] = o;
    }
    
    return cid;
}

void BinanceOrderManager::cancel(const std::string& symbol, const std::string& clientId) {
    if (!ws) return;

    std::ostringstream ss;
    ss << "{"
       << "\"method\":\"order.cancel\","
       << "\"params\":{"
       << "\"symbol\":\"" << symbol << "\","
       << "\"origClientOrderId\":\"" << clientId << "\""
       << "},"
       << "\"id\":" << counter++
       << "}";

    ws->send(ss.str());
}

void BinanceOrderManager::cancelAll(const std::string& symbol) {
    if (!ws) return;

    std::ostringstream ss;
    ss << "{"
       << "\"method\":\"openOrders.cancelAll\","
       << "\"params\":{"
       << "\"symbol\":\"" << symbol << "\""
       << "},"
       << "\"id\":" << counter++
       << "}";

    ws->send(ss.str());
}

BinanceOrder BinanceOrderManager::getOrder(const std::string& clientId) const {
    std::lock_guard<std::mutex> g(lock);
    auto it = orders.find(clientId);
    if (it != orders.end()) return it->second;
    return BinanceOrder{};
}

bool BinanceOrderManager::hasOrder(const std::string& clientId) const {
    std::lock_guard<std::mutex> g(lock);
    return orders.find(clientId) != orders.end();
}

BinanceOrder BinanceOrderManager::parseOrder(const std::string& json) {
    BinanceOrder o;
    o.ts = t_ms();

    try {
        auto j = Json::parse(json);

        // Check for error
        if (j["error"].is_object()) {
            if (onError) {
                std::string errMsg = j["error"]["msg"].is_string() ? 
                                     j["error"]["msg"].get_string() : "Unknown error";
                onError(errMsg);
            }
            return o;
        }

        // Parse result or data
        auto d = j["result"].is_object() ? j["result"] : 
                 (j["data"].is_object() ? j["data"] : j);

        if (d["symbol"].is_string()) o.symbol = d["symbol"].get_string();
        if (d["s"].is_string()) o.symbol = d["s"].get_string();
        
        if (d["side"].is_string()) o.side = d["side"].get_string();
        if (d["S"].is_string()) o.side = d["S"].get_string();
        
        if (d["orderId"].is_number()) o.orderId = std::to_string((int64_t)d["orderId"].get_number());
        if (d["i"].is_string()) o.orderId = d["i"].get_string();
        
        if (d["clientOrderId"].is_string()) o.clientId = d["clientOrderId"].get_string();
        if (d["c"].is_string()) o.clientId = d["c"].get_string();
        
        if (d["status"].is_string()) o.status = d["status"].get_string();
        if (d["X"].is_string()) o.status = d["X"].get_string();

        if (d["origQty"].is_string()) o.qty = std::stod(d["origQty"].get_string());
        if (d["q"].is_string()) o.qty = std::stod(d["q"].get_string());
        
        if (d["executedQty"].is_string()) o.filled = std::stod(d["executedQty"].get_string());
        if (d["z"].is_string()) o.filled = std::stod(d["z"].get_string());
        
        if (d["price"].is_string()) o.price = std::stod(d["price"].get_string());
        if (d["p"].is_string()) o.price = std::stod(d["p"].get_string());

    } catch (...) {
        // Parse error - return empty order
    }

    return o;
}

void BinanceOrderManager::onWsMessage(const std::string& msg) {
    BinanceOrder o = parseOrder(msg);
    
    if (!o.clientId.empty()) {
        std::lock_guard<std::mutex> g(lock);
        orders[o.clientId] = o;
    }
    
    if (onOrder && !o.symbol.empty()) {
        onOrder(o);
    }
}

} // namespace Omega

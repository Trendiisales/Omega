#include "BinanceREST.hpp"
#include <chrono>
#include <sstream>

namespace Omega {

static uint64_t ms_now() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

BinanceREST::BinanceREST() 
    : baseUrl("https://api.binance.com") {}

BinanceREST::~BinanceREST() {}

void BinanceREST::setKeys(const std::string& api, const std::string& secret) {
    apiKey = api;
    hmac.setSecret(secret);
}

void BinanceREST::setBaseUrl(const std::string& url) {
    baseUrl = url;
}

void BinanceREST::setCallback(std::function<void(const std::string&)> cb) {
    onReply = cb;
}

std::string BinanceREST::timestamp() const {
    return std::to_string(ms_now());
}

std::string BinanceREST::makeQuery(const std::string& qs) {
    std::string full = qs.empty() ? "timestamp=" + timestamp() 
                                   : qs + "&timestamp=" + timestamp();
    std::string sig = hmac.sign(full);
    return full + "&signature=" + sig;
}

void BinanceREST::getDepth(const std::string& symbol, int limit) {
    std::string path = "/api/v3/depth?symbol=" + symbol + "&limit=" + std::to_string(limit);
    http.get(baseUrl + path, "", [this](const std::string& res){
        if (onReply) onReply(res);
    });
}

void BinanceREST::getTicker(const std::string& symbol) {
    std::string path = "/api/v3/ticker/price?symbol=" + symbol;
    http.get(baseUrl + path, "", [this](const std::string& res){
        if (onReply) onReply(res);
    });
}

void BinanceREST::getExchangeInfo() {
    http.get(baseUrl + "/api/v3/exchangeInfo", "", [this](const std::string& res){
        if (onReply) onReply(res);
    });
}

void BinanceREST::getAccount() {
    std::string q = makeQuery("");
    http.get(baseUrl + "/api/v3/account?" + q, apiKey, [this](const std::string& r){
        if (onReply) onReply(r);
    });
}

void BinanceREST::getOpenOrders(const std::string& symbol) {
    std::string q = makeQuery("symbol=" + symbol);
    http.get(baseUrl + "/api/v3/openOrders?" + q, apiKey, [this](const std::string& r){
        if (onReply) onReply(r);
    });
}

void BinanceREST::getAllOpenOrders() {
    std::string q = makeQuery("");
    http.get(baseUrl + "/api/v3/openOrders?" + q, apiKey, [this](const std::string& r){
        if (onReply) onReply(r);
    });
}

void BinanceREST::newOrder(const std::string& symbol,
                           const std::string& side,
                           const std::string& type,
                           double qty,
                           double price)
{
    std::ostringstream qs;
    qs << "symbol=" << symbol
       << "&side=" << side
       << "&type=" << type
       << "&timeInForce=GTC"
       << "&quantity=" << qty
       << "&price=" << price;

    std::string q = makeQuery(qs.str());

    http.post(baseUrl + "/api/v3/order", apiKey, q, [this](const std::string& r){
        if (onReply) onReply(r);
    });
}

void BinanceREST::newMarketOrder(const std::string& symbol,
                                  const std::string& side,
                                  double qty)
{
    std::ostringstream qs;
    qs << "symbol=" << symbol
       << "&side=" << side
       << "&type=MARKET"
       << "&quantity=" << qty;

    std::string q = makeQuery(qs.str());

    http.post(baseUrl + "/api/v3/order", apiKey, q, [this](const std::string& r){
        if (onReply) onReply(r);
    });
}

void BinanceREST::cancelOrder(const std::string& symbol, 
                               const std::string& orderId)
{
    std::ostringstream qs;
    qs << "symbol=" << symbol
       << "&orderId=" << orderId;

    std::string q = makeQuery(qs.str());

    http.post(baseUrl + "/api/v3/order", apiKey, q, [this](const std::string& r){
        if (onReply) onReply(r);
    });
}

} // namespace Omega

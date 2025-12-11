#include "BinanceUnifiedFeed.hpp"
#include <unordered_map>
#include <vector>
#include <string>

namespace Omega {

BinanceUnifiedFeed::BinanceUnifiedFeed() {}

bool BinanceUnifiedFeed::start(const std::string& s) {
    ws.setCallback([this](const std::string& m){
        onMsg(m);
    });
    return ws.connectDepth(s) && ws.connectTicker(s);
}

void BinanceUnifiedFeed::stop() { ws.close(); }

void BinanceUnifiedFeed::setTickCallback(std::function<void(const Tick&)> cb) {
    tickCB=cb;
}

void BinanceUnifiedFeed::setBookCallback(std::function<void(const OrderBook&)> cb) {
    bookCB=cb;
}

void BinanceUnifiedFeed::onMsg(const std::string& s)
{
    if(s.find("\"bids\"")!=std::string::npos) {
        std::vector<std::pair<double,double>> bids,asks;
        JSON::parseDepth(s,bids,asks);
        BinanceDepthNormalizer::toOrderBook(bids,asks,ob);
        if(bookCB) bookCB(ob);
    } else {
        BinanceTickNormalizer::parse(s,tick);
        if(tickCB) tickCB(tick);
    }
}

}

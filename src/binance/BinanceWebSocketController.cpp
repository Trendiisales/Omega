#include "BinanceWebSocketController.hpp"
#include <algorithm>

namespace Omega {

BinanceWebSocketController::BinanceWebSocketController() {}
BinanceWebSocketController::~BinanceWebSocketController() { close(); }

void BinanceWebSocketController::close() { ws.close(); }

std::string BinanceWebSocketController::toLower(const std::string& s){
    std::string r=s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

bool BinanceWebSocketController::connectDepth(const std::string& sym) {
    std::string s = toLower(sym);
    std::string path = "/ws/" + s + "@depth10@100ms";
    return ws.connect("stream.binance.com", path, 9443);
}

bool BinanceWebSocketController::connectTicker(const std::string& sym) {
    std::string s = toLower(sym);
    std::string path = "/ws/" + s + "@ticker";
    return ws.connect("stream.binance.com", path, 9443);
}

void BinanceWebSocketController::setCallback(std::function<void(const std::string&)> cb) {
    callback=cb;
    ws.setCallback(cb);
}

}

#include "CTraderFIXClient.hpp"

#include <iostream>

namespace Chimera {

CTraderFIXClient::CTraderFIXClient()
    : loggedOn_(false) {}

CTraderFIXClient::~CTraderFIXClient() {
    disconnect();
}

void CTraderFIXClient::setConfig(const FIXConfig& cfg) {
    config_ = cfg;
}

bool CTraderFIXClient::connect() {
    std::cout
        << "[CTraderFIX][ROUTE2] connect "
        << config_.host << ":" << config_.port
        << " sender=" << config_.senderCompID
        << " target=" << config_.targetCompID
        << "\n";

    loggedOn_.store(true, std::memory_order_release);
    return true;
}

void CTraderFIXClient::disconnect() {
    if (!loggedOn_) return;

    loggedOn_.store(false, std::memory_order_release);
    std::cout << "[CTraderFIX][ROUTE2] disconnect\n";
}

bool CTraderFIXClient::isLoggedOn() const {
    return loggedOn_.load(std::memory_order_acquire);
}

void CTraderFIXClient::setMDCallback(MDCallback cb) {
    mdCallback_ = cb;
}

void CTraderFIXClient::subscribeMarketData(const std::string& symbol) {
    if (!loggedOn_) return;

    std::cout << "[CTraderFIX][ROUTE2] subscribe " << symbol << "\n";

    /* Optional synthetic tick hook if needed later
    if (mdCallback_) {
        mdCallback_(symbol, 0.0, 0.0, 0.0, 0.0);
    }
    */
}

void CTraderFIXClient::unsubscribeMarketData(const std::string& symbol) {
    if (!loggedOn_) return;
    std::cout << "[CTraderFIX][ROUTE2] unsubscribe " << symbol << "\n";
}

void CTraderFIXClient::sendNewOrder(
    const std::string& clOrdID,
    const std::string& symbol,
    char side,
    double qty,
    double price,
    char ordType
) {
    if (!loggedOn_) return;

    std::cout
        << "[CTraderFIX][ROUTE2] order "
        << clOrdID << " "
        << symbol << " "
        << side << " "
        << qty << " "
        << price << " "
        << ordType << "\n";
}

} // namespace Chimera

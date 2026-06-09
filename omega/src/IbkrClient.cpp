// omega/src/IbkrClient.cpp
#include "omega/IbkrClient.h"

#ifndef OMEGA_WITH_IBKR
// ---- CSV-only build: provide the stub's out-of-line failure path. ----
#include <stdexcept>
namespace omega {
void IbkrClient::fail() {
    throw std::runtime_error(
        "IBKR support not compiled in. Rebuild with -DOMEGA_WITH_IBKR and the "
        "TWS API include/sources on the command line.");
}
} // namespace omega

#else
// ---- IBKR build: real adapter over the TWS API C++ client. ----------------
// Adjust the #include paths to match your TWS API install if needed.
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

#include "EWrapper.h"
#include "DefaultEWrapper.h"
#include "EReaderOSSignal.h"
#include "EReader.h"
#include "EClientSocket.h"
#include "Contract.h"
#include "bar.h"
#include "Decimal.h"
#include "DecimalFunctions.h"

namespace omega {

namespace {

// IB daily bars arrive as "YYYYMMDD"; intraday as "YYYYMMDD  HH:MM:SS".
// Normalise the date portion to "YYYY-MM-DD" for our Series.
std::string normalizeDate(const std::string& t) {
    if (t.size() >= 8 && std::isdigit(static_cast<unsigned char>(t[0])))
        return t.substr(0, 4) + "-" + t.substr(4, 2) + "-" + t.substr(6, 2);
    return t;
}

double volToDouble(const Decimal& d) {
    // Modern TWS API stores volume as Decimal.
    return DecimalFunctions::decimalToDouble(d);
}

} // namespace

// EWrapper implementation. DefaultEWrapper supplies empty bodies for the ~100
// callbacks we do not care about; we override only the four we need.
//
// NOTE: the error() callback signature has changed across TWS API releases.
// This targets the 10.x signature. If your SDK is older, change error() to
// match your installed EWrapper.h (the rest is unaffected).
struct IbkrClient::Impl : public DefaultEWrapper {
    IbkrConfig cfg;
    EReaderOSSignal signal;
    EClientSocket* client = nullptr;
    EReader* reader = nullptr;

    int activeReqId = -1;
    bool done = false;
    bool errored = false;
    std::string errMsg;
    std::vector<Bar> bars;

    explicit Impl(IbkrConfig c) : cfg(c), signal(2000 /*ms wait*/) {
        client = new EClientSocket(this, &signal);
    }
    ~Impl() override {
        delete reader;
        delete client;
    }

    // ---- EWrapper overrides -------------------------------------------------
    void nextValidId(OrderId) override {}
    void connectAck() override {
        if (client->asyncEConnect()) client->startApi();
    }

    void historicalData(TickerId reqId, const ::Bar& bar) override {
        if (reqId != activeReqId) return;
        omega::Bar b;
        b.date   = normalizeDate(bar.time);
        b.open   = bar.open;
        b.high   = bar.high;
        b.low    = bar.low;
        b.close  = bar.close;
        b.volume = static_cast<long long>(volToDouble(bar.volume));
        bars.push_back(b);
    }

    void historicalDataEnd(int reqId, const std::string&, const std::string&) override {
        if (reqId == activeReqId) done = true;
    }

    void error(int id, time_t /*errorTime*/, int errorCode,
               const std::string& errorString,
               const std::string& /*advancedOrderRejectJson*/) override {
        // Codes 2104/2106/2158 are benign "data farm OK" notices.
        if (errorCode == 2104 || errorCode == 2106 || errorCode == 2158) return;
        if (id == activeReqId || id == -1) {
            errored = true;
            errMsg = "IB error " + std::to_string(errorCode) + ": " + errorString;
            done = true;
        }
    }
};

IbkrClient::IbkrClient(IbkrConfig cfg) : impl_(new Impl(cfg)) {}
IbkrClient::~IbkrClient() { disconnect(); delete impl_; }

bool IbkrClient::connect() {
    if (!impl_->client->eConnect(impl_->cfg.host.c_str(), impl_->cfg.port,
                                 impl_->cfg.clientId, /*extraAuth=*/false))
        return false;
    impl_->reader = new EReader(impl_->client, &impl_->signal);
    impl_->reader->start();
    return impl_->client->isConnected();
}

void IbkrClient::disconnect() {
    if (impl_ && impl_->client && impl_->client->isConnected())
        impl_->client->eDisconnect();
}

Series IbkrClient::fetchHistorical(const HistRequest& req) {
    if (!impl_->client->isConnected())
        throw std::runtime_error("IbkrClient: not connected (call connect() first)");

    Contract c;
    c.symbol   = req.symbol;
    c.secType  = req.secType;
    c.exchange = req.exchange;
    c.currency = req.currency;

    impl_->bars.clear();
    impl_->done = false;
    impl_->errored = false;
    impl_->errMsg.clear();
    impl_->activeReqId += 1;
    const int reqId = (impl_->activeReqId < 1) ? (impl_->activeReqId = 1)
                                               : impl_->activeReqId;

    impl_->client->reqHistoricalData(
        reqId, c, req.endDateTime, req.durationStr, req.barSize,
        req.whatToShow, req.useRTH ? 1 : 0, /*formatDate=*/1,
        /*keepUpToDate=*/false, TagValueListSPtr());

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(impl_->cfg.timeoutSec);
    while (!impl_->done) {
        impl_->signal.waitForSignal();
        impl_->reader->processMsgs();
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("IbkrClient: timeout waiting for " + req.symbol);
    }
    if (impl_->errored) throw std::runtime_error(impl_->errMsg);

    Series s;
    s.symbol = req.symbol;
    s.bars = impl_->bars;   // IB returns oldest-first for historical data
    return s;
}

} // namespace omega

#endif // OMEGA_WITH_IBKR

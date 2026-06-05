// IbkrClient.cpp — C++ IBKR TWS API client foundation for the gap-short system.
// Subclasses DefaultEWrapper; connects to the gateway, runs the gapper scanner.
// This is the reusable IBKR execution/data layer (also the basis for a future
// CFD migration). FIX/OmegaFIX stays untouched — this is additive.
//
// build: g++ -std=c++17 -I../third_party/twsapi/client IbkrClient.cpp \
//        ../third_party/twsapi/client/libtwsclient.a -o ibkrclient
// run (on the Omega VPS where the gateway lives): ./ibkrclient [port]
#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "ScannerSubscription.h"
#include "Contract.h"
#include <cstdio>
#include <string>
#include <memory>
#include <thread>

class IbkrClient : public DefaultEWrapper {
public:
    EReaderOSSignal signal_{2000};
    std::unique_ptr<EClientSocket> client_;
    std::unique_ptr<EReader> reader_;
    OrderId nextId_ = 0;
    bool done_ = false;

    IbkrClient(){ client_ = std::make_unique<EClientSocket>(this, &signal_); }

    bool connect(const char* host, int port, int clientId){
        bool ok = client_->eConnect(host, port, clientId, false);
        if(ok){
            reader_ = std::make_unique<EReader>(client_.get(), &signal_);
            reader_->start();
        }
        return ok;
    }
    void pump(){   // process incoming messages
        signal_.waitForSignal(); reader_->processMsgs();
    }

    // ---- callbacks we care about ----
    void nextValidId(OrderId id) override { nextId_ = id; printf("[IBKR] connected, nextValidId=%ld\n", (long)id); fflush(stdout);
        // fire the gapper scanner once connected
        ScannerSubscription s; s.instrument="STK"; s.locationCode="STK.US.MAJOR"; s.scanCode="TOP_PERC_GAIN";
        s.abovePrice=3; s.belowPrice=20; s.aboveVolume=100000;
        client_->reqScannerSubscription(7001, s, TagValueListSPtr(), TagValueListSPtr());
    }
    void scannerData(int reqId, int rank, const ContractDetails& cd, const std::string&, const std::string&, const std::string&, const std::string&) override {
        if(rank<12) printf("[IBKR] gapper rank=%d %s\n", rank, cd.contract.symbol.c_str()); fflush(stdout);
    }
    void scannerDataEnd(int reqId) override { printf("[IBKR] scanner done\n"); client_->cancelScannerSubscription(reqId); done_=true; }
    void error(int id, int code, const std::string& msg, const std::string&) override {
        if(code!=2104 && code!=2106 && code!=2158) printf("[IBKR] err %d: %s\n", code, msg.c_str());
    }
};

int main(int argc, char** argv){
    int port = argc>1 ? atoi(argv[1]) : 4002;
    IbkrClient c;
    if(!c.connect("127.0.0.1", port, 84)){ printf("connect FAILED\n"); return 1; }
    for(int i=0;i<200 && !c.done_;++i) c.pump();
    c.client_->eDisconnect();
    printf("[IBKR] done\n");
    return 0;
}

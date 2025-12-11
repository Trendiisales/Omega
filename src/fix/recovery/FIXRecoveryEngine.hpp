#pragma once
#include <string>
#include <unordered_map>
#include <cstdint>
#include <functional>
#include <mutex>
#include "../transport/FIXTransport.hpp"
#include "../codec/FIXParser.hpp"

namespace Omega {

struct RecoveryEvent {
    std::string type;
    std::string detail;
    uint64_t ts = 0;
};

class FIXRecoveryEngine {
public:
    FIXRecoveryEngine();
    ~FIXRecoveryEngine();

    void attach(FIXTransport* t);

    void setRecoveryCallback(std::function<void(const RecoveryEvent&)> cb);

    void requestResend(uint64_t beginSeq, uint64_t endSeq);
    void onInbound(const std::string& msg);
    
    uint64_t lastInboundSeq() const;
    uint64_t lastOutboundSeq() const;
    void resetSeq();

private:
    uint64_t parseSeq(const std::unordered_map<std::string,std::string>& tags);
    void handlePossDup(const std::unordered_map<std::string,std::string>& tags);
    void handleGapFill(const std::unordered_map<std::string,std::string>& tags);

private:
    FIXTransport* tr;
    mutable std::mutex lock;
    uint64_t lastSeqIn;
    uint64_t lastSeqOut;
    std::function<void(const RecoveryEvent&)> onRecovery;
};

} // namespace Omega

#include "FIXRecoveryEngine.hpp"
#include <chrono>

namespace Omega {

static uint64_t rec_ts() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

FIXRecoveryEngine::FIXRecoveryEngine()
    : tr(nullptr),
      lastSeqIn(0),
      lastSeqOut(0) {}

FIXRecoveryEngine::~FIXRecoveryEngine() {}

void FIXRecoveryEngine::attach(FIXTransport* t) {
    tr = t;
    if (!tr) return;

    tr->setRxCallback([this](const std::string& msg){
        onInbound(msg);
    });
}

void FIXRecoveryEngine::setRecoveryCallback(std::function<void(const RecoveryEvent&)> cb) {
    onRecovery = cb;
}

uint64_t FIXRecoveryEngine::lastInboundSeq() const {
    std::lock_guard<std::mutex> g(lock);
    return lastSeqIn;
}

uint64_t FIXRecoveryEngine::lastOutboundSeq() const {
    std::lock_guard<std::mutex> g(lock);
    return lastSeqOut;
}

void FIXRecoveryEngine::resetSeq() {
    std::lock_guard<std::mutex> g(lock);
    lastSeqIn = 0;
    lastSeqOut = 0;
}

uint64_t FIXRecoveryEngine::parseSeq(const std::unordered_map<std::string,std::string>& tags) {
    auto it = tags.find("34");
    if (it == tags.end()) return 0;
    try {
        return std::stoull(it->second);
    } catch (...) {
        return 0;
    }
}

void FIXRecoveryEngine::requestResend(uint64_t beginSeq, uint64_t endSeq) {
    if (!tr) return;

    std::string msg =
        "8=FIX.4.4\x01"
        "35=2\x01"
        "7=" + std::to_string(beginSeq) + "\x01"
        "16=" + std::to_string(endSeq) + "\x01";

    tr->sendRaw(msg);
    
    {
        std::lock_guard<std::mutex> g(lock);
        lastSeqOut++;
    }

    RecoveryEvent e;
    e.type = "ResendRequest";
    e.detail = "Requested " + std::to_string(beginSeq) + "-" + std::to_string(endSeq);
    e.ts = rec_ts();

    if (onRecovery) onRecovery(e);
}

void FIXRecoveryEngine::handlePossDup(const std::unordered_map<std::string,std::string>& tags) {
    auto it = tags.find("43");
    if (it != tags.end() && it->second == "Y") {
        RecoveryEvent e;
        e.type = "PossDup";
        e.detail = "Duplicate sequence";
        e.ts = rec_ts();
        if (onRecovery) onRecovery(e);
    }
}

void FIXRecoveryEngine::handleGapFill(const std::unordered_map<std::string,std::string>& tags) {
    auto it = tags.find("35");
    if (it != tags.end() && it->second == "4") {
        // Sequence Reset - Gap Fill
        auto newSeqIt = tags.find("36");
        if (newSeqIt != tags.end()) {
            try {
                uint64_t newSeq = std::stoull(newSeqIt->second);
                std::lock_guard<std::mutex> g(lock);
                if (newSeq > lastSeqIn) {
                    lastSeqIn = newSeq - 1;
                }
            } catch (...) {}
            
            RecoveryEvent e;
            e.type = "GapFill";
            e.detail = "New seq: " + newSeqIt->second;
            e.ts = rec_ts();
            if (onRecovery) onRecovery(e);
        }
    }
}

void FIXRecoveryEngine::onInbound(const std::string& msg) {
    FIXParser p;
    auto tags = p.parse(msg);

    // Check for gap fill first
    handleGapFill(tags);
    
    uint64_t seq = parseSeq(tags);

    if (seq > 0) {
        std::lock_guard<std::mutex> g(lock);
        if (seq <= lastSeqIn) {
            handlePossDup(tags);
        } else if (seq > lastSeqIn + 1) {
            // Gap detected - request resend
            // (handled externally via callback)
            RecoveryEvent e;
            e.type = "GapDetected";
            e.detail = "Expected " + std::to_string(lastSeqIn + 1) + 
                       ", got " + std::to_string(seq);
            e.ts = rec_ts();
            if (onRecovery) onRecovery(e);
        }
        lastSeqIn = seq;
    }
}

} // namespace Omega

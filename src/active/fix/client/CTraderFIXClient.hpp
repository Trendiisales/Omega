#pragma once
// =============================================================================
// CTraderFIXClient.hpp - cTrader FIX 4.4 Client
// =============================================================================
// Provides complete FIX session management for cTrader:
//   - SSL connection to demo/live servers
//   - Logon/Logout sequence
//   - Heartbeat management
//   - Market data subscription
//   - Order entry/cancel/replace
//   - Preallocated resend buffer (HFT-grade)
// =============================================================================
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <iostream>
#include <sstream>
#include <iomanip>

#include "../transport/FIXSSLTransport.hpp"
#include "../session/FIXResendRing.hpp"
#include "../FIXMessage.hpp"
#include "../FIXFastParse.hpp"

namespace Chimera {

// FIX 4.4 message types
namespace FIXMsgType {
    constexpr const char* HEARTBEAT = "0";
    constexpr const char* TEST_REQUEST = "1";
    constexpr const char* RESEND_REQUEST = "2";
    constexpr const char* LOGON = "A";
    constexpr const char* LOGOUT = "5";
    constexpr const char* REJECT = "3";
    constexpr const char* EXEC_REPORT = "8";
    constexpr const char* ORDER_CANCEL_REJECT = "9";
    constexpr const char* NEW_ORDER = "D";
    constexpr const char* ORDER_CANCEL = "F";
    constexpr const char* ORDER_REPLACE = "G";
    constexpr const char* MD_REQUEST = "V";
    constexpr const char* MD_SNAPSHOT = "W";
    constexpr const char* MD_INCREMENTAL = "X";
    constexpr const char* SEQUENCE_RESET = "4";
}

// FIX 4.4 tags
namespace FIXTag {
    constexpr int BeginString = 8;
    constexpr int BodyLength = 9;
    constexpr int MsgType = 35;
    constexpr int SenderCompID = 49;
    constexpr int TargetCompID = 56;
    constexpr int MsgSeqNum = 34;
    constexpr int SendingTime = 52;
    constexpr int CheckSum = 10;
    constexpr int EncryptMethod = 98;
    constexpr int HeartBtInt = 108;
    constexpr int ResetSeqNumFlag = 141;
    constexpr int Username = 553;
    constexpr int Password = 554;
    constexpr int TestReqID = 112;
    constexpr int Text = 58;
    
    // Resend tags
    constexpr int BeginSeqNo = 7;
    constexpr int EndSeqNo = 16;
    
    // Order tags
    constexpr int ClOrdID = 11;
    constexpr int OrigClOrdID = 41;
    constexpr int Symbol = 55;
    constexpr int Side = 54;
    constexpr int OrderQty = 38;
    constexpr int OrdType = 40;
    constexpr int Price = 44;
    constexpr int TimeInForce = 59;
    constexpr int TransactTime = 60;
    
    // Market data tags
    constexpr int MDReqID = 262;
    constexpr int SubscriptionRequestType = 263;
    constexpr int MarketDepth = 264;
    constexpr int MDUpdateType = 265;
    constexpr int NoMDEntryTypes = 267;
    constexpr int MDEntryType = 269;
    constexpr int NoRelatedSym = 146;
    constexpr int NoMDEntries = 268;
    constexpr int MDEntryPx = 270;
    constexpr int MDEntrySize = 271;
}

struct CTraderConfig {
    std::string host = "demo-uk-eqx-02.p.c-trader.com";
    int port = 5212;
    std::string senderCompID = "demo.blackbull.2067070";
    std::string targetCompID = "cServer";
    std::string username = "2067070";
    std::string password = "TQ$2UbnHJcwVm7@";
    int heartbeatInterval = 30;
};

// Callback for market data
using MDCallback = std::function<void(const std::string& symbol, double bid, double ask, 
                                       double bidSize, double askSize)>;

// Callback for execution reports
using ExecCallback = std::function<void(const std::string& clOrdID, const std::string& execType,
                                         const std::string& ordStatus, double fillPx, double fillQty)>;

class CTraderFIXClient {
public:
    CTraderFIXClient();
    ~CTraderFIXClient();
    
    void setConfig(const CTraderConfig& cfg) { config_ = cfg; }
    
    // Lifecycle
    bool connect();
    void disconnect();
    bool isConnected() const { return connected_.load(); }
    bool isLoggedOn() const { return loggedOn_.load(); }
    
    // Market data
    void subscribeMarketData(const std::string& symbol);
    void unsubscribeMarketData(const std::string& symbol);
    void setMDCallback(MDCallback cb) { mdCallback_ = cb; }
    
    // Orders
    void sendNewOrder(const std::string& clOrdID, const std::string& symbol, 
                      char side, double qty, double price = 0.0, char ordType = '1');
    void cancelOrder(const std::string& origClOrdID, const std::string& newClOrdID,
                     const std::string& symbol, char side);
    void setExecCallback(ExecCallback cb) { execCallback_ = cb; }
    
    // Stats
    uint64_t getMsgSent() const { return msgSent_.load(); }
    uint64_t getMsgRecv() const { return msgRecv_.load(); }

private:
    void onMessage(const std::string& raw);
    void handleLogon(const std::unordered_map<int, std::string>& fields);
    void handleLogout(const std::unordered_map<int, std::string>& fields);
    void handleHeartbeat(const std::unordered_map<int, std::string>& fields);
    void handleTestRequest(const std::unordered_map<int, std::string>& fields);
    void handleMDSnapshot(const std::unordered_map<int, std::string>& fields);
    void handleMDIncremental(const std::unordered_map<int, std::string>& fields);
    void handleExecReport(const std::unordered_map<int, std::string>& fields);
    void handleReject(const std::unordered_map<int, std::string>& fields);
    void handleResendRequest(const std::unordered_map<int, std::string>& fields);
    
    void heartbeatLoop();
    
    std::string buildMessage(const std::unordered_map<int, std::string>& fields);
    static std::unordered_map<int, std::string> parseMessage(const std::string& raw);
    static std::string checksum(const std::string& msg);
    static std::string timestamp();
    
    bool sendLogon();
    void sendLogout();
    void sendHeartbeat(const std::string& testReqID = "");
    
    // Centralized send with resend ring storage (HFT-grade)
    bool sendFIX(const std::string& msg) noexcept;

private:
    CTraderConfig config_;
    FIXSSLTransport transport_;
    
    // Preallocated resend buffer (4096 msgs, no heap on hot path)
    FIXResendRing resendRing_;
    
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> loggedOn_{false};
    
    std::atomic<int> outSeqNum_{1};
    std::atomic<int> inSeqNum_{1};
    
    std::thread heartbeatThread_;
    // COLD_PATH_ONLY: state synchronization

    std::mutex sendMtx_;
    
    MDCallback mdCallback_;
    ExecCallback execCallback_;
    
    std::atomic<uint64_t> msgSent_{0};
    std::atomic<uint64_t> msgRecv_{0};
    
    std::chrono::steady_clock::time_point lastRecvTime_;
    std::chrono::steady_clock::time_point lastSendTime_;
};

} // namespace Chimera

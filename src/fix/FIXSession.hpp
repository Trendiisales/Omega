#pragma once
#include <string>
#include <functional>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include "transport/FIXTransport.hpp"
#include "FIXMessage.hpp"

namespace Omega {

class FIXSession {
public:
    FIXSession();
    ~FIXSession();

    void setCredentials(const std::string& sender, const std::string& target);
    void setTransport(FIXTransport* t);

    bool logon(const std::string& username, const std::string& password);
    void logout();

    // Send with map of tag->value
    bool sendMessage(const std::unordered_map<int,std::string>& fields);
    
    // Send with pipe-delimited string (e.g. "35=D|11=ORD1|55=BTCUSDT|")
    bool sendMessage(const std::string& pipeDelimited);
    
    // Send with FIXMessage object
    bool sendMessage(const FIXMessage& msg);

    void setCallback(std::function<void(const std::unordered_map<int,std::string>&)> cb);

private:
    FIXTransport* transport;
    std::string senderCompID;
    std::string targetCompID;
    std::atomic<int> msgSeq;
    std::function<void(const std::unordered_map<int,std::string>&)> callback;

    void onRaw(const std::string& raw);
    std::string buildFIX(const std::unordered_map<int,std::string>& fields);
    static bool parseFIX(const std::string& raw, std::unordered_map<int,std::string>& out);
    static std::unordered_map<int,std::string> parsePipeDelimited(const std::string& s);
    static std::string checksum(const std::string& s);
};

}

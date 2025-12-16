#include "FIXSession.hpp"
#include <sstream>
#include <iomanip>
#include <chrono>

namespace Omega {

FIXSession::FIXSession()
    : transport(nullptr), msgSeq(1)
{}

FIXSession::~FIXSession() {}

void FIXSession::setCredentials(const std::string& sender, const std::string& target) {
    senderCompID = sender;
    targetCompID = target;
}

void FIXSession::setTransport(FIXTransport* t) {
    transport = t;
    if (transport) {
        transport->setRxCallback([this](const std::string& raw) {
            onRaw(raw);
        });
    }
}

bool FIXSession::logon(const std::string& username, const std::string& password) {
    std::unordered_map<int, std::string> fields;
    fields[35] = "A";  // Logon
    fields[98] = "0";  // EncryptMethod
    fields[108] = "30"; // HeartBtInt
    if (!username.empty()) fields[553] = username;
    if (!password.empty()) fields[554] = password;
    
    return sendMessage(fields);
}

void FIXSession::logout() {
    std::unordered_map<int, std::string> fields;
    fields[35] = "5";  // Logout
    sendMessage(fields);
}

std::unordered_map<int,std::string> FIXSession::parsePipeDelimited(const std::string& s) {
    std::unordered_map<int, std::string> result;
    std::istringstream iss(s);
    std::string token;
    
    while (std::getline(iss, token, '|')) {
        size_t eq = token.find('=');
        if (eq != std::string::npos) {
            try {
                int tag = std::stoi(token.substr(0, eq));
                std::string val = token.substr(eq + 1);
                result[tag] = val;
            } catch (...) {}
        }
    }
    return result;
}

bool FIXSession::sendMessage(const std::string& pipeDelimited) {
    auto fields = parsePipeDelimited(pipeDelimited);
    return sendMessage(fields);
}

bool FIXSession::sendMessage(const FIXMessage& msg) {
    return sendMessage(msg.fields);
}

bool FIXSession::sendMessage(const std::unordered_map<int,std::string>& fields) {
    if (!transport) return false;
    
    std::string msg = buildFIX(fields);
    return transport->sendRaw(msg);
}

void FIXSession::setCallback(std::function<void(const std::unordered_map<int,std::string>&)> cb) {
    callback = cb;
}

void FIXSession::onRaw(const std::string& raw) {
    std::unordered_map<int, std::string> fields;
    if (parseFIX(raw, fields)) {
        if (callback) callback(fields);
    }
}

std::string FIXSession::buildFIX(const std::unordered_map<int,std::string>& fields) {
    // Build body first
    std::ostringstream body;
    
    // Add standard headers
    body << "35=" << (fields.count(35) ? fields.at(35) : "0") << "\x01";
    body << "49=" << senderCompID << "\x01";
    body << "56=" << targetCompID << "\x01";
    body << "34=" << msgSeq++ << "\x01";
    
    // Add timestamp
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::gmtime(&time);
    body << "52=" << std::put_time(&tm, "%Y%m%d-%H:%M:%S") << "\x01";
    
    // Add other fields
    for (const auto& [tag, val] : fields) {
        if (tag == 35 || tag == 49 || tag == 56 || tag == 34 || tag == 52) continue;
        body << tag << "=" << val << "\x01";
    }
    
    std::string bodyStr = body.str();
    
    // Build complete message
    std::ostringstream msg;
    msg << "8=FIX.4.4\x01";
    msg << "9=" << bodyStr.size() << "\x01";
    msg << bodyStr;
    
    std::string msgStr = msg.str();
    msgStr += "10=" + checksum(msgStr) + "\x01";
    
    return msgStr;
}

bool FIXSession::parseFIX(const std::string& raw, std::unordered_map<int,std::string>& out) {
    std::istringstream iss(raw);
    std::string token;
    
    while (std::getline(iss, token, '\x01')) {
        size_t eq = token.find('=');
        if (eq != std::string::npos) {
            try {
                int tag = std::stoi(token.substr(0, eq));
                std::string val = token.substr(eq + 1);
                out[tag] = val;
            } catch (...) {}
        }
    }
    return !out.empty();
}

std::string FIXSession::checksum(const std::string& s) {
    int sum = 0;
    for (char c : s) sum += (unsigned char)c;
    sum %= 256;
    
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(3) << sum;
    return oss.str();
}

} // namespace Omega

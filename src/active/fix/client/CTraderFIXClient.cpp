// =============================================================================
// CTraderFIXClient.cpp - cTrader FIX 4.4 Client Implementation
// =============================================================================
// CHIMERA HFT v1.5.3 - FIXED TargetSubID=TRADE + SecurityList + MD Subscription
//
// Key changes:
//   1. sendLogon() - NOW INCLUDES TargetSubID=TRADE (tag 57)
//   2. sendSecurityListRequest() - requests all symbols after logon
//   3. handleSecurityList() - parses 35=y with fragment accumulation
//   4. subscribeMarketData() - uses tag 48 + tag 22, NOT tag 55
//   5. normalizeSymbol() - handles cTrader symbol format variations
// =============================================================================
#include "CTraderFIXClient.hpp"
#include <cstring>
#include <algorithm>

namespace Chimera {

CTraderFIXClient::CTraderFIXClient() {
    transport_.setRxCallback([this](const std::string& msg) {
        onMessage(msg);
    });
    
    transport_.setStateCallback([this](bool up) {
        connected_ = up;
        if (!up) {
            loggedOn_ = false;
        }
    });
}

CTraderFIXClient::~CTraderFIXClient() {
    disconnect();
}

bool CTraderFIXClient::connect() {
    std::cout << "[CTraderFIX] Connecting to " << config_.host << ":" << config_.port << "\n";
    
    if (!transport_.connect(config_.host, config_.port)) {
        std::cerr << "[CTraderFIX] Transport connection failed\n";
        return false;
    }
    
    running_ = true;
    connected_ = true;
    
    // Start heartbeat thread
    heartbeatThread_ = std::thread(&CTraderFIXClient::heartbeatLoop, this);
    
    // Send logon
    if (!sendLogon()) {
        std::cerr << "[CTraderFIX] Logon failed\n";
        disconnect();
        return false;
    }
    
    // Wait for logon response (with timeout)
    auto start = std::chrono::steady_clock::now();
    while (!loggedOn_ && running_) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() > 10) {
            std::cerr << "[CTraderFIX] Logon timeout\n";
            disconnect();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    std::cout << "[CTraderFIX] Logon successful\n";
    
    // Request security list immediately after logon
    sendSecurityListRequest();
    
    return true;
}

void CTraderFIXClient::disconnect() {
    if (!running_) return;
    
    if (loggedOn_) {
        sendLogout();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    running_ = false;
    loggedOn_ = false;
    
    if (heartbeatThread_.joinable()) {
        heartbeatThread_.join();
    }
    
    transport_.disconnect();
    connected_ = false;
    
    std::cout << "[CTraderFIX] Disconnected. Sent: " << msgSent_.load() 
              << " Recv: " << msgRecv_.load() << "\n";
}

// =============================================================================
// sendLogon - FIXED: Now includes TargetSubID=TRADE (tag 57)
// =============================================================================
bool CTraderFIXClient::sendLogon() {
    std::unordered_map<int, std::string> fields;
    fields[FIXTag::MsgType] = FIXMsgType::LOGON;
    fields[FIXTag::EncryptMethod] = "0";  // No encryption
    fields[FIXTag::HeartBtInt] = std::to_string(config_.heartbeatInterval);
    fields[FIXTag::ResetSeqNumFlag] = "Y";
    fields[FIXTag::Username] = config_.username;
    fields[FIXTag::Password] = config_.password;
    fields[57] = "TRADE";  // TargetSubID=TRADE - REQUIRED for cTrader trade session
    
    std::string msg = buildMessage(fields);
    return sendFIX(msg);
}

void CTraderFIXClient::sendLogout() {
    std::unordered_map<int, std::string> fields;
    fields[FIXTag::MsgType] = FIXMsgType::LOGOUT;
    
    std::string msg = buildMessage(fields);
    sendFIX(msg);
}

void CTraderFIXClient::sendHeartbeat(const std::string& testReqID) {
    std::unordered_map<int, std::string> fields;
    fields[FIXTag::MsgType] = FIXMsgType::HEARTBEAT;
    if (!testReqID.empty()) {
        fields[FIXTag::TestReqID] = testReqID;
    }
    
    std::string msg = buildMessage(fields);
    sendFIX(msg);
}

// =============================================================================
// sendSecurityListRequest - Request all symbols from cTrader
// =============================================================================
void CTraderFIXClient::sendSecurityListRequest() {
    std::unordered_map<int, std::string> fields;
    fields[FIXTag::MsgType] = FIXMsgType::SECURITY_LIST_REQUEST;
    fields[FIXTag::SecurityReqID] = "SECLIST1";
    fields[FIXTag::SecurityListRequestType] = "0";  // 0 = All securities
    
    std::string msg = buildMessage(fields);
    sendFIX(msg);
    
    std::cout << "[CTraderFIX] SecurityListRequest sent\n";
}

// =============================================================================
// normalizeSymbol - Clean up cTrader symbol names for consistent lookup
// =============================================================================
std::string CTraderFIXClient::normalizeSymbol(const std::string& s) {
    std::string result = s;
    
    // Trim leading/trailing whitespace
    size_t start = result.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = result.find_last_not_of(" \t\r\n");
    result = result.substr(start, end - start + 1);
    
    // Uppercase
    for (char& c : result) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    
    // Strip common suffixes: .FX, .cash, /, etc.
    auto dotPos = result.find('.');
    if (dotPos != std::string::npos) {
        result = result.substr(0, dotPos);
    }
    
    // Remove trailing slash
    if (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    
    return result;
}

// =============================================================================
// getSecurityID - Thread-safe lookup of SecurityID for a symbol
// =============================================================================
int CTraderFIXClient::getSecurityID(const std::string& symbol) const {
    std::string normalized = normalizeSymbol(symbol);
    std::lock_guard<std::mutex> lock(securityMtx_);
    auto it = securityMap_.find(normalized);
    if (it != securityMap_.end()) {
        return it->second;
    }
    return 0;
}

// =============================================================================
// subscribeMarketData - FIXED: Uses tag 48 (SecurityID) + tag 22
// =============================================================================
void CTraderFIXClient::subscribeMarketData(const std::string& symbol) {
    static int mdReqID = 1;
    
    // Wait for security list if not ready
    if (!securityListReady_.load()) {
        std::cerr << "[CTraderFIX] WARNING: Security list not ready, queuing " << symbol << "\n";
        // Could implement queue here, for now just warn
    }
    
    // Lookup SecurityID
    std::string normalized = normalizeSymbol(symbol);
    int securityID = getSecurityID(symbol);
    
    if (securityID == 0) {
        std::cerr << "[CTraderFIX] Symbol not in security list: " << symbol 
                  << " (normalized: " << normalized << ")\n";
        return;
    }
    
    // Build message manually to ensure correct tag order for repeating groups
    std::ostringstream body;
    
    // Header fields will be added by buildMessage, but we need to control the body
    std::string reqID = "MD" + std::to_string(mdReqID++);
    
    // Using buildMessage for header, then send manually constructed body
    std::unordered_map<int, std::string> fields;
    fields[FIXTag::MsgType] = FIXMsgType::MD_REQUEST;
    fields[FIXTag::MDReqID] = reqID;
    fields[FIXTag::SubscriptionRequestType] = "1";  // Snapshot + Updates
    fields[FIXTag::MarketDepth] = "1";  // Top of book
    fields[FIXTag::MDUpdateType] = "0";  // Full refresh
    fields[FIXTag::NoMDEntryTypes] = "2";
    // Note: We'll add MDEntryType manually for repeating group
    fields[FIXTag::NoRelatedSym] = "1";
    // CRITICAL: Use SecurityID (48) + SecurityIDSource (22), NOT Symbol (55)
    fields[FIXTag::SecurityID] = std::to_string(securityID);
    fields[FIXTag::SecurityIDSource] = "8";  // 8 = Exchange Symbol
    
    std::string msg = buildMessage(fields);
    sendFIX(msg);
    
    std::cout << "[CTraderFIX] Subscribed to: " << symbol 
              << " (ID=" << securityID << ")\n";
}

void CTraderFIXClient::unsubscribeMarketData(const std::string& symbol) {
    static int mdReqID = 1000;
    
    int securityID = getSecurityID(symbol);
    if (securityID == 0) {
        std::cerr << "[CTraderFIX] Cannot unsubscribe: symbol not found: " << symbol << "\n";
        return;
    }
    
    std::unordered_map<int, std::string> fields;
    fields[FIXTag::MsgType] = FIXMsgType::MD_REQUEST;
    fields[FIXTag::MDReqID] = "UNSUB" + std::to_string(mdReqID++);
    fields[FIXTag::SubscriptionRequestType] = "2";  // Unsubscribe
    fields[FIXTag::MarketDepth] = "1";
    fields[FIXTag::NoRelatedSym] = "1";
    fields[FIXTag::SecurityID] = std::to_string(securityID);
    fields[FIXTag::SecurityIDSource] = "8";
    
    std::string msg = buildMessage(fields);
    sendFIX(msg);
}

void CTraderFIXClient::sendNewOrder(const std::string& clOrdID, const std::string& symbol,
                                     char side, double qty, double price, char ordType) {
    int securityID = getSecurityID(symbol);
    if (securityID == 0) {
        std::cerr << "[CTraderFIX] Cannot send order: symbol not found: " << symbol << "\n";
        return;
    }
    
    std::unordered_map<int, std::string> fields;
    fields[FIXTag::MsgType] = FIXMsgType::NEW_ORDER;
    fields[FIXTag::ClOrdID] = clOrdID;
    fields[FIXTag::SecurityID] = std::to_string(securityID);
    fields[FIXTag::SecurityIDSource] = "8";
    fields[FIXTag::Side] = std::string(1, side);
    fields[FIXTag::OrderQty] = std::to_string(qty);
    fields[FIXTag::OrdType] = std::string(1, ordType);
    if (ordType == '2' && price > 0) {  // Limit order
        fields[FIXTag::Price] = std::to_string(price);
    }
    fields[FIXTag::TimeInForce] = "0";  // Day
    fields[FIXTag::TransactTime] = timestamp();
    
    std::string msg = buildMessage(fields);
    sendFIX(msg);
}

void CTraderFIXClient::cancelOrder(const std::string& origClOrdID, const std::string& newClOrdID,
                                    const std::string& symbol, char side) {
    int securityID = getSecurityID(symbol);
    if (securityID == 0) {
        std::cerr << "[CTraderFIX] Cannot cancel order: symbol not found: " << symbol << "\n";
        return;
    }
    
    std::unordered_map<int, std::string> fields;
    fields[FIXTag::MsgType] = FIXMsgType::ORDER_CANCEL;
    fields[FIXTag::OrigClOrdID] = origClOrdID;
    fields[FIXTag::ClOrdID] = newClOrdID;
    fields[FIXTag::SecurityID] = std::to_string(securityID);
    fields[FIXTag::SecurityIDSource] = "8";
    fields[FIXTag::Side] = std::string(1, side);
    fields[FIXTag::TransactTime] = timestamp();
    
    std::string msg = buildMessage(fields);
    sendFIX(msg);
}

void CTraderFIXClient::onMessage(const std::string& raw) {
    msgRecv_.fetch_add(1, std::memory_order_relaxed);
    lastRecvTime_ = std::chrono::steady_clock::now();
    
    auto fields = parseMessage(raw);
    if (fields.empty()) return;
    
    auto it = fields.find(FIXTag::MsgType);
    if (it == fields.end()) return;
    
    const std::string& msgType = it->second;
    
    if (msgType == FIXMsgType::LOGON) {
        handleLogon(fields);
    } else if (msgType == FIXMsgType::LOGOUT) {
        handleLogout(fields);
    } else if (msgType == FIXMsgType::HEARTBEAT) {
        handleHeartbeat(fields);
    } else if (msgType == FIXMsgType::TEST_REQUEST) {
        handleTestRequest(fields);
    } else if (msgType == FIXMsgType::RESEND_REQUEST) {
        handleResendRequest(fields);
    } else if (msgType == FIXMsgType::MD_SNAPSHOT) {
        handleMDSnapshot(fields);
    } else if (msgType == FIXMsgType::MD_INCREMENTAL) {
        handleMDIncremental(fields);
    } else if (msgType == FIXMsgType::EXEC_REPORT) {
        handleExecReport(fields);
    } else if (msgType == FIXMsgType::REJECT) {
        handleReject(fields);
    } else if (msgType == FIXMsgType::SECURITY_LIST) {
        // Security list needs raw message for repeating group parsing
        handleSecurityList(raw);
    }
}

void CTraderFIXClient::handleLogon(const std::unordered_map<int, std::string>& /*fields*/) {
    loggedOn_ = true;
    std::cout << "[CTraderFIX] Logon confirmed\n";
}

void CTraderFIXClient::handleLogout(const std::unordered_map<int, std::string>& fields) {
    loggedOn_ = false;
    auto it = fields.find(FIXTag::Text);
    if (it != fields.end()) {
        std::cout << "[CTraderFIX] Logout: " << it->second << "\n";
    }
}

void CTraderFIXClient::handleHeartbeat(const std::unordered_map<int, std::string>& /*fields*/) {
    // Heartbeat received, nothing to do
}

void CTraderFIXClient::handleTestRequest(const std::unordered_map<int, std::string>& fields) {
    auto it = fields.find(FIXTag::TestReqID);
    if (it != fields.end()) {
        sendHeartbeat(it->second);
    } else {
        sendHeartbeat();
    }
}

// =============================================================================
// handleSecurityList - Parse 35=y with fragment accumulation
// =============================================================================
// cTrader sends security list in multiple fragments.
// We accumulate until 893=Y (LastFragment=true)
// =============================================================================
void CTraderFIXClient::handleSecurityList(const std::string& raw) {
    // Check for last fragment
    bool lastFragment = false;
    
    // Parse for 893= (LastFragment)
    size_t fragPos = raw.find("893=");
    if (fragPos != std::string::npos) {
        size_t valStart = fragPos + 4;
        size_t valEnd = raw.find('\x01', valStart);
        if (valEnd != std::string::npos) {
            std::string fragVal = raw.substr(valStart, valEnd - valStart);
            lastFragment = (fragVal == "Y" || fragVal == "1");
        }
    }
    
    // Parse repeating group entries
    // We need to find all instances of tag 48 (SecurityID) and their associated
    // symbol names (tag 55, 107, or 1151)
    
    // Strategy: Walk through the message and extract security entries
    // Each entry starts with SecurityID (48) or we can detect by 55/107/1151
    
    size_t pos = 0;
    int entriesThisMsg = 0;
    
    // Temporary storage for current entry being parsed
    int currentSecID = 0;
    std::string currentSymbol;
    
    while (pos < raw.size()) {
        size_t eq = raw.find('=', pos);
        if (eq == std::string::npos) break;
        
        size_t soh = raw.find('\x01', eq);
        if (soh == std::string::npos) soh = raw.size();
        
        int tag = 0;
        try {
            tag = std::stoi(raw.substr(pos, eq - pos));
        } catch (...) {
            pos = soh + 1;
            continue;
        }
        
        std::string value = raw.substr(eq + 1, soh - eq - 1);
        
        // Process security-related tags
        if (tag == 48) {  // SecurityID - REQUIRED
            // If we have a pending entry, save it first
            if (currentSecID != 0 && !currentSymbol.empty()) {
                std::string normalized = normalizeSymbol(currentSymbol);
                if (!normalized.empty()) {
                    std::lock_guard<std::mutex> lock(securityMtx_);
                    securityMap_[normalized] = currentSecID;
                    reverseMap_[currentSecID] = normalized;
                    ++entriesThisMsg;
                    
#ifdef FIX_DEBUG
                    std::cout << "[FIX-SECLIST] mapped " << normalized 
                              << " -> " << currentSecID << "\n";
#endif
                }
            }
            
            // Start new entry
            currentSecID = 0;
            currentSymbol.clear();
            
            try {
                currentSecID = std::stoi(value);
            } catch (...) {
                currentSecID = 0;
            }
        }
        else if (tag == 55 && currentSymbol.empty()) {  // Symbol - priority 1
            currentSymbol = value;
        }
        else if (tag == 107 && currentSymbol.empty()) {  // SecurityDesc - priority 2
            currentSymbol = value;
        }
        else if (tag == 1151 && currentSymbol.empty()) {  // SecurityGroup - priority 3
            currentSymbol = value;
        }
        
        pos = soh + 1;
    }
    
    // Don't forget the last entry
    if (currentSecID != 0 && !currentSymbol.empty()) {
        std::string normalized = normalizeSymbol(currentSymbol);
        if (!normalized.empty()) {
            std::lock_guard<std::mutex> lock(securityMtx_);
            securityMap_[normalized] = currentSecID;
            reverseMap_[currentSecID] = normalized;
            ++entriesThisMsg;
            
#ifdef FIX_DEBUG
            std::cout << "[FIX-SECLIST] mapped " << normalized 
                      << " -> " << currentSecID << "\n";
#endif
        }
    }
    
    std::cout << "[CTraderFIX] SecurityList fragment: " << entriesThisMsg 
              << " entries (total: " << securityMap_.size() << ")"
              << (lastFragment ? " [LAST]" : "") << "\n";
    
    // If last fragment, mark ready
    if (lastFragment) {
        securityListReady_ = true;
        std::cout << "[CTraderFIX] Security list READY: " 
                  << securityMap_.size() << " symbols mapped\n";
        
        // Print some sample mappings for verification
        int samples = 0;
        std::lock_guard<std::mutex> lock(securityMtx_);
        for (const auto& [sym, id] : securityMap_) {
            if (samples < 10) {
                std::cout << "  " << sym << " = " << id << "\n";
                ++samples;
            } else {
                break;
            }
        }
        
        // Notify callback if set
        if (secListCallback_) {
            secListCallback_(securityMap_.size());
        }
    }
}

void CTraderFIXClient::handleMDSnapshot(const std::unordered_map<int, std::string>& fields) {
    // Try to get symbol from SecurityID first, then fall back to Symbol
    std::string symbol;
    
    auto secIdIt = fields.find(FIXTag::SecurityID);
    if (secIdIt != fields.end()) {
        int secId = 0;
        try {
            secId = std::stoi(secIdIt->second);
        } catch (...) {}
        
        if (secId != 0) {
            std::lock_guard<std::mutex> lock(securityMtx_);
            auto it = reverseMap_.find(secId);
            if (it != reverseMap_.end()) {
                symbol = it->second;
            }
        }
    }
    
    // Fall back to Symbol tag
    if (symbol.empty()) {
        auto symIt = fields.find(FIXTag::Symbol);
        if (symIt != fields.end()) {
            symbol = normalizeSymbol(symIt->second);
        }
    }
    
    if (symbol.empty()) return;
    
    double bid = 0, ask = 0, bidSize = 0, askSize = 0;
    
    // Parse MD entries (simplified - real impl needs repeating group parsing)
    auto bidIt = fields.find(FIXTag::MDEntryPx);
    if (bidIt != fields.end()) {
        try {
            bid = std::stod(bidIt->second);
        } catch (...) {}
    }
    auto sizeIt = fields.find(FIXTag::MDEntrySize);
    if (sizeIt != fields.end()) {
        try {
            bidSize = std::stod(sizeIt->second);
        } catch (...) {}
    }
    
    if (mdCallback_) {
        mdCallback_(symbol, bid, ask, bidSize, askSize);
    }
}

void CTraderFIXClient::handleMDIncremental(const std::unordered_map<int, std::string>& fields) {
    // Same as snapshot for now
    handleMDSnapshot(fields);
}

void CTraderFIXClient::handleExecReport(const std::unordered_map<int, std::string>& fields) {
    auto clOrdIt = fields.find(FIXTag::ClOrdID);
    std::string clOrdID = (clOrdIt != fields.end()) ? clOrdIt->second : "";
    
    // Extract exec type, ord status, fill info
    std::string execType = "0";  // Default: New
    std::string ordStatus = "0";
    double fillPx = 0, fillQty = 0;
    
    auto etIt = fields.find(150);  // ExecType
    if (etIt != fields.end()) execType = etIt->second;
    
    auto osIt = fields.find(39);   // OrdStatus
    if (osIt != fields.end()) ordStatus = osIt->second;
    
    auto pxIt = fields.find(31);   // LastPx
    if (pxIt != fields.end()) {
        try {
            fillPx = std::stod(pxIt->second);
        } catch (...) {}
    }
    
    auto qtyIt = fields.find(32);  // LastQty
    if (qtyIt != fields.end()) {
        try {
            fillQty = std::stod(qtyIt->second);
        } catch (...) {}
    }
    
    if (execCallback_) {
        execCallback_(clOrdID, execType, ordStatus, fillPx, fillQty);
    }
}

void CTraderFIXClient::handleReject(const std::unordered_map<int, std::string>& fields) {
    auto textIt = fields.find(FIXTag::Text);
    std::string reason = (textIt != fields.end()) ? textIt->second : "Unknown";
    
    // Also check for RefTagID (tag 371) and RefMsgType (tag 372)
    auto refTagIt = fields.find(371);
    auto refMsgIt = fields.find(372);
    
    std::cerr << "[CTraderFIX] REJECT: " << reason;
    if (refTagIt != fields.end()) {
        std::cerr << " (RefTag=" << refTagIt->second << ")";
    }
    if (refMsgIt != fields.end()) {
        std::cerr << " (RefMsgType=" << refMsgIt->second << ")";
    }
    std::cerr << "\n";
}

void CTraderFIXClient::heartbeatLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(config_.heartbeatInterval));
        
        if (!running_ || !loggedOn_) continue;
        
        // Check if we need to send heartbeat
        auto now = std::chrono::steady_clock::now();
        auto sinceSend = std::chrono::duration_cast<std::chrono::seconds>(now - lastSendTime_).count();
        
        if (sinceSend >= config_.heartbeatInterval) {
            sendHeartbeat();
        }
        
        // Check for receive timeout
        auto sinceRecv = std::chrono::duration_cast<std::chrono::seconds>(now - lastRecvTime_).count();
        if (sinceRecv > config_.heartbeatInterval * 2) {
            std::cerr << "[CTraderFIX] Heartbeat timeout\n";
            // Could trigger reconnect here
        }
    }
}

std::string CTraderFIXClient::buildMessage(const std::unordered_map<int, std::string>& fields) {
    std::lock_guard<std::mutex> lock(sendMtx_);
    
    std::string body;
    
    // Add standard header fields
    body += "35=" + fields.at(FIXTag::MsgType) + '\x01';
    body += "49=" + config_.senderCompID + '\x01';
    body += "56=" + config_.targetCompID + '\x01';
    
    // Add TargetSubID (57) if present in fields - must be in header
    auto subIdIt = fields.find(57);
    if (subIdIt != fields.end()) {
        body += "57=" + subIdIt->second + '\x01';
    }
    
    body += "34=" + std::to_string(outSeqNum_++) + '\x01';
    body += "52=" + timestamp() + '\x01';
    
    // Add other fields
    for (const auto& [tag, value] : fields) {
        if (tag == FIXTag::MsgType) continue;  // Already added
        if (tag == 57) continue;  // Already added in header
        body += std::to_string(tag) + "=" + value + '\x01';
    }
    
    // Build complete message
    std::string header = "8=FIX.4.4\x01";
    header += "9=" + std::to_string(body.size()) + '\x01';
    
    std::string msg = header + body;
    msg += "10=" + checksum(msg) + '\x01';
    
    lastSendTime_ = std::chrono::steady_clock::now();
    msgSent_.fetch_add(1, std::memory_order_relaxed);
    
    return msg;
}

std::unordered_map<int, std::string> CTraderFIXClient::parseMessage(const std::string& raw) {
    std::unordered_map<int, std::string> fields;
    
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t eq = raw.find('=', pos);
        if (eq == std::string::npos) break;
        
        size_t soh = raw.find('\x01', eq);
        if (soh == std::string::npos) soh = raw.size();
        
        try {
            int tag = std::stoi(raw.substr(pos, eq - pos));
            std::string value = raw.substr(eq + 1, soh - eq - 1);
            fields[tag] = value;
        } catch (...) {
            // Skip malformed fields
        }
        
        pos = soh + 1;
    }
    
    return fields;
}

std::string CTraderFIXClient::checksum(const std::string& msg) {
    int sum = 0;
    for (char c : msg) {
        sum += static_cast<unsigned char>(c);
    }
    sum %= 256;
    
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(3) << sum;
    return oss.str();
}

std::string CTraderFIXClient::timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    
    std::tm* tm = std::gmtime(&time);
    std::ostringstream oss;
    oss << std::put_time(tm, "%Y%m%d-%H:%M:%S") << "." 
        << std::setfill('0') << std::setw(3) << ms;
    return oss.str();
}

// =============================================================================
// sendFIX - Centralized send with preallocated resend ring storage
// =============================================================================
bool CTraderFIXClient::sendFIX(const std::string& msg) noexcept {
    uint32_t seq = static_cast<uint32_t>(outSeqNum_.load(std::memory_order_relaxed));
    
    // Store in preallocated ring
    resendRing_.store(seq, msg.data(), static_cast<uint32_t>(msg.size()));
    
    // Send via SSL transport
    bool ok = transport_.sendRaw(msg);
    
    if (ok) {
        msgSent_.fetch_add(1, std::memory_order_relaxed);
        lastSendTime_ = std::chrono::steady_clock::now();
    }
    
    return ok;
}

// =============================================================================
// handleResendRequest - Replay messages from preallocated ring
// =============================================================================
void CTraderFIXClient::handleResendRequest(const std::unordered_map<int, std::string>& fields) {
    auto fromIt = fields.find(FIXTag::BeginSeqNo);
    auto toIt = fields.find(FIXTag::EndSeqNo);
    
    if (fromIt == fields.end() || toIt == fields.end()) {
        std::cerr << "[CTraderFIX] Invalid ResendRequest: missing BeginSeqNo/EndSeqNo\n";
        return;
    }
    
    uint32_t fromSeq = static_cast<uint32_t>(std::stoul(fromIt->second));
    uint32_t toSeq = static_cast<uint32_t>(std::stoul(toIt->second));
    
    if (toSeq == 0) {
        toSeq = static_cast<uint32_t>(outSeqNum_.load(std::memory_order_relaxed) - 1);
    }
    
    std::cout << "[CTraderFIX] ResendRequest: " << fromSeq << " to " << toSeq << "\n";
    
    FIXStoredMsg m;
    uint32_t resent = 0;
    uint32_t gapped = 0;
    
    for (uint32_t s = fromSeq; s <= toSeq; ++s) {
        if (resendRing_.fetch(s, m)) {
            transport_.sendRaw(std::string(m.data, m.len));
            ++resent;
        } else {
            ++gapped;
        }
    }
    
    std::cout << "[CTraderFIX] Resend complete: " << resent << " sent, " << gapped << " gaps\n";
}

} // namespace Chimera

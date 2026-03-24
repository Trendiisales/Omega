#pragma once
// =============================================================================
// CTraderDepthClient.hpp — cTrader Open API v2 Depth-of-Market feed
//
// PURPOSE:
//   Replace the FIX 264=1 single-level L2 book with full multi-level real
//   depth data from the cTrader Open API. BlackBull confirmed that FIX only
//   supports MarketDepth 0 or 1. The cTrader Open API has no such restriction
//   and delivers real incremental order book updates via ProtoOADepthEvent.
//
// PROTOCOL:
//   TCP/SSL to live.ctraderapi.com:5036 (JSON mode — no protobuf library needed)
//   Each message: [4-byte big-endian length][2-byte payloadType][JSON body]
//   cTrader Open API v2 — Spotware documentation (help.ctrader.com/open-api)
//
// AUTH FLOW (one-time):
//   1. ProtoOAApplicationAuthReq  (clientId + clientSecret)
//   2. ProtoOAAccountAuthReq      (ctidTraderAccountId + accessToken)
//   3. ProtoOASymbolsListReq      (map name -> symbolId)
//   4. ProtoOASubscribeDepthQuotesReq (symbolIds for our whitelist)
//   => ProtoOADepthEvent stream   (incremental: newQuotes + deletedQuotes)
//
// DEPTH QUOTE FORMAT (ProtoOADepthQuote):
//   id:   uint64  — unique quote ID (used to delete specific levels)
//   size: uint64  — size in cents (divide by 100 for lots)
//   bid:  uint64  — price in 1/100000 units (only set for bid quotes)
//   ask:  uint64  — price in 1/100000 units (only set for ask quotes)
//   A quote has either bid OR ask set, never both.
//
// INCREMENTAL BOOK MAINTENANCE:
//   newQuotes     — add/update these quote IDs in the book
//   deletedQuotes — remove these quote IDs from the book
//   Rebuild L2Book from the maintained map on each event.
//
// INTEGRATION:
//   Runs as a separate std::thread alongside FIX loops.
//   On each DepthEvent: acquires g_l2_mtx and updates g_l2_books[sym].
//   FIX imbalance() still works — cTrader data just fills in real multi-level
//   sizes instead of the single-level estimate from FIX 264=1.
//
// TOKEN:
//   accessToken is a 30-day OAuth2 bearer token. Store in omega_config.ini.
//   ctidTraderAccountId is the numeric cTrader account ID for BlackBull live.
//   Both are obtained once via the OAuth2 flow and refreshed automatically
//   using the refreshToken before expiry.
//
// HOW TO GET YOUR ACCESS TOKEN (one-time setup):
//   1. Open in browser:
//      https://id.ctrader.com/my/settings/openapi/grantingaccess/?
//        client_id=20304_NqeKlH3FEECOWqeP1JvoT2czQV9xkUHE7UXxfPU2dRuDXrZsIM
//        &redirect_uri=https://localhost
//        &scope=trading
//        &product=web
//   2. Log in with your BlackBull cTrader ID credentials.
//   3. You are redirected to: https://localhost/?code=XXXXXXXXX
//      Copy the code= value.
//   4. Exchange for access token:
//      curl "https://openapi.ctrader.com/apps/token?grant_type=authorization_code
//        &code=XXXXXXXXX
//        &redirect_uri=https://localhost
//        &client_id=20304_NqeKlH3FEECOWqeP1JvoT2czQV9xkUHE7UXxfPU2dRuDXrZsIM
//        &client_secret=jeYwDPzelIYSoDppuhSZoRpaRi1q572FcBJ44dXNviuSEKxdB9"
//   5. Response contains accessToken and refreshToken.
//      Add to omega_config.ini:
//        [ctrader_api]
//        access_token=<accessToken>
//        refresh_token=<refreshToken>
//        ctid_trader_account_id=<ctidTraderAccountId from GetAccountListByAccessTokenRes>
//
// =============================================================================

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <chrono>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <sys/socket.h>
  #include <netdb.h>
  #include <unistd.h>
  #define closesocket close
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "OmegaFIX.hpp"  // L2Book, g_l2_books, g_l2_mtx

// =============================================================================
// cTrader Open API payload type IDs (from ProtoOAPayloadType enum)
// =============================================================================
namespace CTraderPayload {
    static constexpr uint16_t APPLICATION_AUTH_REQ         = 2100;
    static constexpr uint16_t APPLICATION_AUTH_RES         = 2101;
    static constexpr uint16_t ACCOUNT_AUTH_REQ             = 2102;
    static constexpr uint16_t ACCOUNT_AUTH_RES             = 2103;
    static constexpr uint16_t ERROR_RES                    = 2142;
    static constexpr uint16_t GET_ACCOUNTS_BY_TOKEN_REQ    = 2149;
    static constexpr uint16_t GET_ACCOUNTS_BY_TOKEN_RES    = 2150;
    static constexpr uint16_t SYMBOLS_LIST_REQ             = 2114;
    static constexpr uint16_t SYMBOLS_LIST_RES             = 2115;
    static constexpr uint16_t SUBSCRIBE_DEPTH_QUOTES_REQ   = 2156;
    static constexpr uint16_t SUBSCRIBE_DEPTH_QUOTES_RES   = 2157;
    static constexpr uint16_t DEPTH_EVENT                  = 2155;
    static constexpr uint16_t REFRESH_TOKEN_REQ            = 2173;
    static constexpr uint16_t REFRESH_TOKEN_RES            = 2174;
    // Common heartbeat
    static constexpr uint16_t HEARTBEAT_EVENT              = 51;
}

// =============================================================================
// Minimal JSON extractor — no external library needed
// Handles: string fields, integer fields, arrays of objects
// =============================================================================
namespace CTraderJSON {

// Extract a string value for a given key from a flat JSON object
// e.g. extract_str(json, "accessToken") -> "abc123"
inline std::string extract_str(const std::string& json, const std::string& key) {
    const std::string search = "\"" + key + "\"";
    const auto kpos = json.find(search);
    if (kpos == std::string::npos) return "";
    const auto colon = json.find(':', kpos + search.size());
    if (colon == std::string::npos) return "";
    const auto q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    const auto q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

// Extract a uint64 value for a given key
inline uint64_t extract_u64(const std::string& json, const std::string& key) {
    const std::string search = "\"" + key + "\"";
    const auto kpos = json.find(search);
    if (kpos == std::string::npos) return 0;
    const auto colon = json.find(':', kpos + search.size());
    if (colon == std::string::npos) return 0;
    size_t p = colon + 1;
    while (p < json.size() && (json[p]==' '||json[p]=='\t')) ++p;
    if (p >= json.size()) return 0;
    // Handle both quoted numbers ("123") and unquoted (123)
    bool quoted = (json[p] == '"');
    if (quoted) ++p;
    uint64_t val = 0;
    while (p < json.size() && json[p] >= '0' && json[p] <= '9') {
        val = val * 10 + (json[p++] - '0');
    }
    return val;
}

inline int64_t extract_i64(const std::string& json, const std::string& key) {
    return static_cast<int64_t>(extract_u64(json, key));
}

// Extract array of objects between "key":[...] and return each {...} as string
inline std::vector<std::string> extract_array(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    const std::string search = "\"" + key + "\"";
    const auto kpos = json.find(search);
    if (kpos == std::string::npos) return result;
    const auto bracket = json.find('[', kpos + search.size());
    if (bracket == std::string::npos) return result;
    // Find matching close bracket
    int depth = 0;
    size_t start = bracket;
    size_t obj_start = std::string::npos;
    for (size_t i = bracket; i < json.size(); ++i) {
        if (json[i] == '[' || json[i] == '{') {
            if (json[i] == '{' && depth == 1) obj_start = i;
            ++depth;
        } else if (json[i] == ']' || json[i] == '}') {
            --depth;
            if (json[i] == '}' && depth == 1 && obj_start != std::string::npos) {
                result.push_back(json.substr(obj_start, i - obj_start + 1));
                obj_start = std::string::npos;
            }
            if (depth == 0) break;
        }
    }
    (void)start;
    return result;
}

// Extract flat array of uint64 values: "key":[1,2,3]
inline std::vector<uint64_t> extract_u64_array(const std::string& json, const std::string& key) {
    std::vector<uint64_t> result;
    const std::string search = "\"" + key + "\"";
    const auto kpos = json.find(search);
    if (kpos == std::string::npos) return result;
    const auto bracket = json.find('[', kpos + search.size());
    if (bracket == std::string::npos) return result;
    size_t p = bracket + 1;
    while (p < json.size() && json[p] != ']') {
        while (p < json.size() && (json[p]==' '||json[p]==','||json[p]=='\n')) ++p;
        if (json[p] == ']') break;
        // skip quotes around numbers
        bool q = (json[p] == '"');
        if (q) ++p;
        uint64_t val = 0;
        bool has_digit = false;
        while (p < json.size() && json[p] >= '0' && json[p] <= '9') {
            val = val * 10 + (json[p++] - '0');
            has_digit = true;
        }
        if (q && p < json.size() && json[p] == '"') ++p;
        if (has_digit) result.push_back(val);
    }
    return result;
}

// Build minimal JSON request strings
inline std::string app_auth_req(const std::string& clientId, const std::string& secret) {
    return "{\"payloadType\":2100,\"clientId\":\"" + clientId + "\",\"clientSecret\":\"" + secret + "\"}";
}

inline std::string get_accounts_req(const std::string& accessToken) {
    return "{\"payloadType\":2149,\"accessToken\":\"" + accessToken + "\"}";
}

inline std::string account_auth_req(int64_t ctidAccountId, const std::string& accessToken) {
    return "{\"payloadType\":2102,\"ctidTraderAccountId\":" + std::to_string(ctidAccountId) +
           ",\"accessToken\":\"" + accessToken + "\"}";
}

inline std::string symbols_list_req(int64_t ctidAccountId) {
    return "{\"payloadType\":2114,\"ctidTraderAccountId\":" + std::to_string(ctidAccountId) + "}";
}

inline std::string subscribe_depth_req(int64_t ctidAccountId, const std::vector<int64_t>& symbolIds) {
    std::ostringstream b;
    b << "{\"payloadType\":2156,\"ctidTraderAccountId\":" << ctidAccountId << ",\"symbolId\":[";
    for (size_t i = 0; i < symbolIds.size(); ++i) {
        if (i) b << ',';
        b << symbolIds[i];
    }
    b << "]}";
    return b.str();
}

inline std::string refresh_token_req(const std::string& refreshToken) {
    return "{\"payloadType\":2173,\"refreshToken\":\"" + refreshToken + "\"}";
}

inline std::string heartbeat() {
    return "{\"payloadType\":51}";
}

} // namespace CTraderJSON

// =============================================================================
// DepthBook — incremental order book maintained per symbol
// Maps quote_id -> {price, size, is_bid}
// Rebuilt to L2Book after each DepthEvent
// =============================================================================
struct DepthQuote {
    uint64_t price_raw = 0;   // in 1/100000 units
    uint64_t size_raw  = 0;   // in cents
    bool     is_bid    = false;
};

struct DepthBook {
    std::unordered_map<uint64_t, DepthQuote> quotes;  // id -> quote

    void apply_new(uint64_t id, uint64_t price, uint64_t size, bool is_bid) {
        quotes[id] = {price, size, is_bid};
    }
    void apply_delete(uint64_t id) {
        quotes.erase(id);
    }

    // Rebuild L2Book from maintained incremental map
    // Price: raw / 100000.0 = actual price
    // Size:  raw / 100.0    = lots (size is in cents, 1 lot = 100 cents)
    L2Book to_l2book() const {
        L2Book book;
        // Collect bid and ask levels, sort by price
        struct Level { double price; double size; };
        std::vector<Level> bids, asks;
        for (const auto& kv : quotes) {
            const auto& q = kv.second;
            if (q.price_raw == 0 || q.size_raw == 0) continue;
            const double price = static_cast<double>(q.price_raw) / 100000.0;
            const double size  = static_cast<double>(q.size_raw)  / 100.0;
            if (q.is_bid) bids.push_back({price, size});
            else          asks.push_back({price, size});
        }
        // Sort: bids descending (best bid first), asks ascending (best ask first)
        std::sort(bids.begin(), bids.end(), [](const Level& a, const Level& b){ return a.price > b.price; });
        std::sort(asks.begin(), asks.end(), [](const Level& a, const Level& b){ return a.price < b.price; });
        // Fill L2Book (max 5 levels)
        book.bid_count = static_cast<int>(std::min(bids.size(), size_t(5)));
        book.ask_count = static_cast<int>(std::min(asks.size(), size_t(5)));
        for (int i = 0; i < book.bid_count; ++i) book.bids[i] = {bids[i].price, bids[i].size};
        for (int i = 0; i < book.ask_count; ++i) book.asks[i] = {asks[i].price, asks[i].size};
        return book;
    }
};

// =============================================================================
// CTraderDepthClient — main class
// =============================================================================
class CTraderDepthClient {
public:
    // Credentials — set before calling start()
    std::string client_id;
    std::string client_secret;
    std::string access_token;
    std::string refresh_token;
    int64_t     ctid_account_id = 0;

    // Symbols to subscribe (names from PASSIVE_WHITELIST + our traded symbols)
    // Populated automatically from broker symbol list during startup
    std::unordered_set<std::string> symbol_whitelist;

    // References to Omega's shared L2 book (from main.cpp globals)
    std::mutex*                              l2_mtx    = nullptr;
    std::unordered_map<std::string,L2Book>*  l2_books  = nullptr;

    // Status
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    std::atomic<bool> authorized{false};

    // ==========================================================================
    // Config loading — call after reading omega_config.ini
    // ==========================================================================
    bool configured() const {
        return !client_id.empty() && !client_secret.empty() &&
               !access_token.empty() && ctid_account_id > 0;
    }

    // ==========================================================================
    // Start background thread
    // ==========================================================================
    void start() {
        if (!configured()) {
            std::cout << "[CTRADER] Not configured — depth feed disabled.\n"
                      << "[CTRADER] Add [ctrader_api] section to omega_config.ini.\n";
            return;
        }
        if (running.load()) return;
        running.store(true);
        thread_ = std::thread([this]{ loop(); });
        std::cout << "[CTRADER] Depth client thread started.\n";
    }

    void stop() {
        running.store(false);
        if (thread_.joinable()) thread_.join();
    }

    ~CTraderDepthClient() { stop(); }

private:
    std::thread thread_;
    // Per-symbol incremental depth books (maintained inside loop thread)
    std::unordered_map<std::string, DepthBook>  depth_books_;   // name -> book
    std::unordered_map<uint64_t,    std::string> id_to_name_;   // symbolId -> name

    // ==========================================================================
    // Main reconnect loop
    // ==========================================================================
    void loop() {
        int backoff_ms = 2000;
        const int max_backoff = 60000;

        while (running.load()) {
            connected.store(false);
            authorized.store(false);

            std::cout << "[CTRADER] Connecting live.ctraderapi.com:5036\n";
            int sock = -1;
            SSL* ssl = connect_ssl("live.ctraderapi.com", 5036, sock);
            if (!ssl) {
                std::cerr << "[CTRADER] Connect failed — retry " << backoff_ms << "ms\n";
                sleep_ms(backoff_ms);
                backoff_ms = std::min(backoff_ms * 2, max_backoff);
                continue;
            }
            connected.store(true);
            backoff_ms = 2000;

            // Reset recv buffer
            recv_buf_.clear();

            // Auth sequence
            if (!do_auth(ssl)) {
                std::cerr << "[CTRADER] Auth failed — reconnecting\n";
                ssl_close(ssl, sock);
                sleep_ms(5000);
                continue;
            }
            authorized.store(true);
            std::cout << "[CTRADER] Authorized — depth feed active\n";

            // Main receive loop
            recv_loop(ssl, sock);

            ssl_close(ssl, sock);
            connected.store(false);
            authorized.store(false);

            if (running.load()) {
                std::cerr << "[CTRADER] Disconnected — reconnecting in " << backoff_ms << "ms\n";
                sleep_ms(backoff_ms);
                backoff_ms = std::min(backoff_ms * 2, max_backoff);
            }
        }
        std::cout << "[CTRADER] Depth client stopped.\n";
    }

    // ==========================================================================
    // Auth sequence: AppAuth -> GetAccounts -> AccountAuth -> SymbolsList -> Subscribe
    // ==========================================================================
    bool do_auth(SSL* ssl) {
        // Step 1: Application auth
        if (!send_msg(ssl, CTraderPayload::APPLICATION_AUTH_REQ,
                      CTraderJSON::app_auth_req(client_id, client_secret))) return false;
        std::cout << "[CTRADER] ApplicationAuthReq sent\n";

        // Read until we get AppAuthRes
        if (!wait_for(ssl, CTraderPayload::APPLICATION_AUTH_RES, 10000)) {
            std::cerr << "[CTRADER] ApplicationAuthRes timeout\n";
            return false;
        }
        std::cout << "[CTRADER] Application authorized\n";

        // Step 2: Get account list to find ctidTraderAccountId if not set
        if (ctid_account_id == 0) {
            if (!send_msg(ssl, CTraderPayload::GET_ACCOUNTS_BY_TOKEN_REQ,
                          CTraderJSON::get_accounts_req(access_token))) return false;
            std::string res;
            if (!wait_for_msg(ssl, CTraderPayload::GET_ACCOUNTS_BY_TOKEN_RES, 10000, res)) {
                std::cerr << "[CTRADER] GetAccountList timeout\n";
                return false;
            }
            // Extract first account ID
            const auto accounts = CTraderJSON::extract_array(res, "ctidTraderAccount");
            if (accounts.empty()) {
                std::cerr << "[CTRADER] No accounts found for access token\n";
                return false;
            }
            ctid_account_id = CTraderJSON::extract_i64(accounts[0], "ctidTraderAccountId");
            const bool is_live = (CTraderJSON::extract_str(accounts[0], "isLive") == "true");
            std::cout << "[CTRADER] Account: ctid=" << ctid_account_id
                      << " live=" << is_live << "\n";
        }

        // Step 3: Account auth
        if (!send_msg(ssl, CTraderPayload::ACCOUNT_AUTH_REQ,
                      CTraderJSON::account_auth_req(ctid_account_id, access_token))) return false;
        std::cout << "[CTRADER] AccountAuthReq sent\n";

        if (!wait_for(ssl, CTraderPayload::ACCOUNT_AUTH_RES, 10000)) {
            std::cerr << "[CTRADER] AccountAuthRes timeout\n";
            return false;
        }
        std::cout << "[CTRADER] Account " << ctid_account_id << " authorized\n";

        // Step 4: Get symbol list to map names -> IDs
        if (!send_msg(ssl, CTraderPayload::SYMBOLS_LIST_REQ,
                      CTraderJSON::symbols_list_req(ctid_account_id))) return false;
        std::string sym_res;
        if (!wait_for_msg(ssl, CTraderPayload::SYMBOLS_LIST_RES, 15000, sym_res)) {
            std::cerr << "[CTRADER] SymbolsListRes timeout\n";
            return false;
        }

        // Parse symbol list
        const auto symbols = CTraderJSON::extract_array(sym_res, "symbol");
        std::cout << "[CTRADER] Symbol list received: " << symbols.size() << " symbols\n";

        std::vector<int64_t> sub_ids;
        id_to_name_.clear();
        depth_books_.clear();

        for (const auto& sym_obj : symbols) {
            const std::string name = CTraderJSON::extract_str(sym_obj, "symbolName");
            const int64_t id = CTraderJSON::extract_i64(sym_obj, "symbolId");
            if (name.empty() || id <= 0) continue;
            id_to_name_[static_cast<uint64_t>(id)] = name;
            // Subscribe only to whitelisted symbols
            if (symbol_whitelist.count(name)) {
                sub_ids.push_back(id);
                depth_books_[name] = DepthBook{};
                std::cout << "[CTRADER] Will subscribe depth: " << name << " id=" << id << "\n";
            }
        }

        if (sub_ids.empty()) {
            std::cerr << "[CTRADER] No matching symbols found — check whitelist\n";
            return false;
        }

        // Step 5: Subscribe to depth quotes
        if (!send_msg(ssl, CTraderPayload::SUBSCRIBE_DEPTH_QUOTES_REQ,
                      CTraderJSON::subscribe_depth_req(ctid_account_id, sub_ids))) return false;

        if (!wait_for(ssl, CTraderPayload::SUBSCRIBE_DEPTH_QUOTES_RES, 10000)) {
            std::cerr << "[CTRADER] SubscribeDepthQuotesRes timeout\n";
            return false;
        }
        std::cout << "[CTRADER] Subscribed to " << sub_ids.size() << " symbols\n";
        return true;
    }

    // ==========================================================================
    // Receive loop — process DepthEvents and heartbeats
    // ==========================================================================
    void recv_loop(SSL* ssl, int sock) {
        auto last_heartbeat = std::chrono::steady_clock::now();
        auto last_token_check = std::chrono::steady_clock::now();
        (void)sock;

        while (running.load()) {
            const auto now = std::chrono::steady_clock::now();

            // Send heartbeat every 25s (server disconnects at ~30s)
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat).count() >= 25) {
                send_msg(ssl, CTraderPayload::HEARTBEAT_EVENT, CTraderJSON::heartbeat());
                last_heartbeat = now;
            }

            // Check token expiry (~28 days — refresh before 30-day expiry)
            // Token is stored in days since epoch; we just check every hour
            if (std::chrono::duration_cast<std::chrono::hours>(now - last_token_check).count() >= 1) {
                last_token_check = now;
                // Proactive refresh check would go here — token_expires_at_ tracking
            }

            // Read one message (non-blocking with 100ms poll)
            uint16_t payload_type = 0;
            std::string body;
            const int rc = read_msg(ssl, payload_type, body, 100);
            if (rc < 0) {
                std::cerr << "[CTRADER] Read error — disconnected\n";
                return;
            }
            if (rc == 0) continue;  // timeout, loop

            dispatch_msg(payload_type, body);
        }
    }

    // ==========================================================================
    // Dispatch received message
    // ==========================================================================
    void dispatch_msg(uint16_t payload_type, const std::string& body) {
        switch (payload_type) {
        case CTraderPayload::DEPTH_EVENT:
            on_depth_event(body);
            break;

        case CTraderPayload::HEARTBEAT_EVENT:
            // Echo heartbeat back
            break;

        case CTraderPayload::REFRESH_TOKEN_RES:
            {
                const std::string new_access  = CTraderJSON::extract_str(body, "accessToken");
                const std::string new_refresh = CTraderJSON::extract_str(body, "refreshToken");
                if (!new_access.empty()) {
                    access_token  = new_access;
                    refresh_token = new_refresh;
                    std::cout << "[CTRADER] Access token refreshed\n";
                }
            }
            break;

        case CTraderPayload::ERROR_RES:
            {
                const std::string err  = CTraderJSON::extract_str(body, "errorCode");
                const std::string desc = CTraderJSON::extract_str(body, "description");
                std::cerr << "[CTRADER] Error: " << err << " — " << desc << "\n";
                if (err == "OA_AUTH_TOKEN_EXPIRED") {
                    std::cerr << "[CTRADER] Token expired — need refresh\n";
                    // Will reconnect and fail auth, triggering operator refresh
                }
            }
            break;

        default:
            break;
        }
    }

    // ==========================================================================
    // Handle ProtoOADepthEvent — the core data path
    // ==========================================================================
    void on_depth_event(const std::string& body) {
        // Extract symbolId
        const uint64_t symbol_id = CTraderJSON::extract_u64(body, "symbolId");
        if (symbol_id == 0) return;

        // Find symbol name
        const auto name_it = id_to_name_.find(symbol_id);
        if (name_it == id_to_name_.end()) return;  // not in our map
        const std::string& name = name_it->second;

        auto& depth_book = depth_books_[name];

        // Apply newQuotes
        const auto new_quotes = CTraderJSON::extract_array(body, "newQuotes");
        for (const auto& q_str : new_quotes) {
            const uint64_t id   = CTraderJSON::extract_u64(q_str, "id");
            const uint64_t size = CTraderJSON::extract_u64(q_str, "size");
            const uint64_t bid  = CTraderJSON::extract_u64(q_str, "bid");
            const uint64_t ask  = CTraderJSON::extract_u64(q_str, "ask");
            if (id == 0 || size == 0) continue;
            if (bid > 0) {
                depth_book.apply_new(id, bid, size, true);
            } else if (ask > 0) {
                depth_book.apply_new(id, ask, size, false);
            }
        }

        // Apply deletedQuotes (array of uint64 IDs)
        const auto del_ids = CTraderJSON::extract_u64_array(body, "deletedQuotes");
        for (uint64_t del_id : del_ids) {
            depth_book.apply_delete(del_id);
        }

        // Rebuild L2Book and write to shared g_l2_books
        if (l2_mtx && l2_books) {
            const L2Book rebuilt = depth_book.to_l2book();
            std::lock_guard<std::mutex> lk(*l2_mtx);
            (*l2_books)[name] = rebuilt;
        }
    }

    // ==========================================================================
    // Wire-level: send a framed JSON message
    // Frame: [4-byte length BE][2-byte payloadType BE][JSON body]
    // Length covers the 2-byte payloadType + body bytes.
    // ==========================================================================
    bool send_msg(SSL* ssl, uint16_t payload_type, const std::string& body) {
        const uint32_t body_len    = static_cast<uint32_t>(body.size());
        const uint32_t total_len   = 2 + body_len;  // payloadType (2) + body
        const uint32_t frame_len   = 4 + 2 + body_len;  // length field (4) + payloadType (2) + body

        std::vector<uint8_t> frame(frame_len);
        // 4-byte length (big-endian): covers payloadType + body
        frame[0] = (total_len >> 24) & 0xFF;
        frame[1] = (total_len >> 16) & 0xFF;
        frame[2] = (total_len >>  8) & 0xFF;
        frame[3] = (total_len >>  0) & 0xFF;
        // 2-byte payloadType (big-endian)
        frame[4] = (payload_type >> 8) & 0xFF;
        frame[5] = (payload_type >> 0) & 0xFF;
        // Body
        if (!body.empty()) std::memcpy(frame.data() + 6, body.data(), body_len);

        const int sent = SSL_write(ssl, frame.data(), static_cast<int>(frame.size()));
        return sent == static_cast<int>(frame.size());
    }

    // ==========================================================================
    // Wire-level: read one framed message with timeout_ms
    // Returns: 1 = got message, 0 = timeout, -1 = error
    // ==========================================================================
    int read_msg(SSL* ssl, uint16_t& payload_type_out, std::string& body_out, int timeout_ms) {
        // Try to parse from existing buffer first
        if (try_parse_msg(payload_type_out, body_out)) return 1;

        // Set non-blocking read with timeout
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);

        while (std::chrono::steady_clock::now() < deadline) {
            uint8_t buf[4096];
            const int n = SSL_read(ssl, buf, static_cast<int>(sizeof(buf)));
            if (n > 0) {
                recv_buf_.insert(recv_buf_.end(), buf, buf + n);
                if (try_parse_msg(payload_type_out, body_out)) return 1;
            } else {
                const int err = SSL_get_error(ssl, n);
                if (err == SSL_ERROR_WANT_READ) {
                    // No data yet — sleep briefly
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                return -1;  // real error
            }
        }
        return 0;  // timeout
    }

    // Try to parse one complete message from recv_buf_
    bool try_parse_msg(uint16_t& payload_type_out, std::string& body_out) {
        // Need at least 6 bytes (4 length + 2 payloadType)
        if (recv_buf_.size() < 6) return false;

        const uint32_t msg_len = (static_cast<uint32_t>(recv_buf_[0]) << 24) |
                                 (static_cast<uint32_t>(recv_buf_[1]) << 16) |
                                 (static_cast<uint32_t>(recv_buf_[2]) <<  8) |
                                 (static_cast<uint32_t>(recv_buf_[3]) <<  0);

        if (msg_len < 2) { recv_buf_.erase(recv_buf_.begin(), recv_buf_.begin() + 4); return false; }
        if (recv_buf_.size() < 4 + msg_len) return false;  // incomplete

        payload_type_out = (static_cast<uint16_t>(recv_buf_[4]) << 8) |
                           (static_cast<uint16_t>(recv_buf_[5]) << 0);

        const uint32_t body_len = msg_len - 2;
        body_out.assign(reinterpret_cast<const char*>(recv_buf_.data() + 6), body_len);

        recv_buf_.erase(recv_buf_.begin(), recv_buf_.begin() + 4 + msg_len);
        return true;
    }

    // ==========================================================================
    // Wait for a specific payloadType (discard others while waiting)
    // ==========================================================================
    bool wait_for(SSL* ssl, uint16_t expected_type, int timeout_ms) {
        std::string body;
        return wait_for_msg(ssl, expected_type, timeout_ms, body);
    }

    bool wait_for_msg(SSL* ssl, uint16_t expected_type, int timeout_ms, std::string& body_out) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            uint16_t pt = 0;
            std::string body;
            const int rc = read_msg(ssl, pt, body, 500);
            if (rc < 0) return false;
            if (rc == 0) continue;
            if (pt == expected_type) { body_out = body; return true; }
            // Handle errors during wait
            if (pt == CTraderPayload::ERROR_RES) {
                const std::string err  = CTraderJSON::extract_str(body, "errorCode");
                const std::string desc = CTraderJSON::extract_str(body, "description");
                std::cerr << "[CTRADER] Error during auth: " << err << " — " << desc << "\n";
                return false;
            }
            // Discard other messages (heartbeats etc.) during wait
        }
        return false;
    }

    // ==========================================================================
    // SSL connection setup — same pattern as Omega FIX connections
    // ==========================================================================
    static SSL* connect_ssl(const char* host, int port, int& sock_out) {
        sock_out = -1;
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        const std::string port_str = std::to_string(port);
        if (getaddrinfo(host, port_str.c_str(), &hints, &res) != 0 || !res) return nullptr;

        SOCKET sock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock == INVALID_SOCKET) { freeaddrinfo(res); return nullptr; }

        if (::connect(sock, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
            freeaddrinfo(res); closesocket(sock); return nullptr;
        }
        freeaddrinfo(res);

        // Set recv/send timeouts (100ms — non-blocking feel for select-less loop)
        DWORD timeout = 100;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));

        static SSL_CTX* ctx = nullptr;
        if (!ctx) {
            ctx = SSL_CTX_new(TLS_client_method());
            if (!ctx) { closesocket(sock); return nullptr; }
            SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
            SSL_CTX_set_quiet_shutdown(ctx, 1);
        }

        SSL* ssl = SSL_new(ctx);
        if (!ssl) { closesocket(sock); return nullptr; }
        SSL_set_fd(ssl, static_cast<int>(sock));
        // Set SNI hostname — required by live.ctraderapi.com
        SSL_set_tlsext_host_name(ssl, host);

        if (SSL_connect(ssl) <= 0) {
            ERR_print_errors_fp(stderr);
            closesocket(sock); SSL_free(ssl); return nullptr;
        }
        sock_out = static_cast<int>(sock);
        return ssl;
    }

    static void ssl_close(SSL* ssl, int sock) {
        if (ssl) SSL_free(ssl);
        if (sock >= 0) closesocket(sock);
    }

    static void sleep_ms(int ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    // Receive buffer (reassembly)
    std::vector<uint8_t> recv_buf_;
};

// =============================================================================
// Global instance — declared here, defined in main.cpp
// =============================================================================
// In main.cpp add:
//   CTraderDepthClient g_ctrader_depth;
// And in main() after config load:
//   g_ctrader_depth.l2_mtx   = &g_l2_mtx;
//   g_ctrader_depth.l2_books = &g_l2_books;
//   g_ctrader_depth.client_id           = "20304_NqeKlH3FEECOWqeP1JvoT2czQV9xkUHE7UXxfPU2dRuDXrZsIM";
//   g_ctrader_depth.client_secret       = "jeYwDPzelIYSoDppuhSZoRpaRi1q572FcBJ44dXNviuSEKxdB9";
//   g_ctrader_depth.access_token        = cfg.ctrader_access_token;
//   g_ctrader_depth.refresh_token       = cfg.ctrader_refresh_token;
//   g_ctrader_depth.ctid_account_id     = cfg.ctrader_ctid_account_id;
//   // Whitelist: all symbols we care about (traded + passive cross-pairs)
//   for (int i = 0; i < OMEGA_NSYMS; ++i)
//       g_ctrader_depth.symbol_whitelist.insert(OMEGA_SYMS[i].name);
//   for (const auto& e : g_ext_syms)
//       g_ctrader_depth.symbol_whitelist.insert(e.name);
//   for (const auto& name : PASSIVE_WHITELIST)
//       g_ctrader_depth.symbol_whitelist.insert(name);
//   g_ctrader_depth.start();
// =============================================================================

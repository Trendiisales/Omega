#pragma once
// =============================================================================
// CTraderDepthClient.hpp — cTrader Open API v2 Depth-of-Market feed
//
// PROTOCOL (verified against official SDK source code):
//
// TCP FRAMING: switching to port 5035 (raw TCP) with newline-delimited JSON.
//   Port 5036 appears to require WebSocket upgrade for raw TCP connections.
//   Port 5035 accepts raw TCP for both protobuf and JSON.
//   Send:    <json_string>\n
//   Receive: <json_string>\n
//
// JSON MESSAGE FORMAT (send) — from official docs:
//   {"clientMsgId":"<id>","payloadType":<int>,"payload":{<fields>}}
//   All request-specific fields go inside the nested "payload" object.
//
// JSON MESSAGE FORMAT (receive):
//   {"payloadType":<int>,"clientMsgId":"<id>","payload":{<fields>}}
//   payloadType at top level; all response fields inside "payload".
//
// BUGS FIXED vs v1:
//   v1 sent [4-byte length][2-byte payloadType][JSON] — WRONG
//   v1 put fields at top level of JSON — WRONG
//   v1 read payloadType as 2-byte binary prefix — WRONG
//   This version: correct 4-byte framing, correct nested payload structure,
//   payloadType parsed from JSON "payloadType" field in received messages.
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
#include <iomanip>
#include <chrono>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <sys/socket.h>
  #include <netdb.h>
  #include <unistd.h>
  #define closesocket close
  #define SOCKET int
  #define INVALID_SOCKET (-1)
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>
#include "OmegaFIX.hpp"

namespace CTraderPT {
    static constexpr int APPLICATION_AUTH_REQ  = 2100;
    static constexpr int APPLICATION_AUTH_RES  = 2101;
    static constexpr int ACCOUNT_AUTH_REQ      = 2102;
    static constexpr int ACCOUNT_AUTH_RES      = 2103;
    static constexpr int SYMBOLS_LIST_REQ      = 2114;
    static constexpr int SYMBOLS_LIST_RES      = 2115;
    static constexpr int SUBSCRIBE_DEPTH_REQ   = 2156;
    static constexpr int SUBSCRIBE_DEPTH_RES   = 2157;
    static constexpr int DEPTH_EVENT           = 2155;
    static constexpr int REFRESH_TOKEN_REQ     = 2173;
    static constexpr int REFRESH_TOKEN_RES     = 2174;
    static constexpr int ERROR_RES             = 2142;
    static constexpr int HEARTBEAT             = 51;
}

namespace CTJSON {

inline std::string make_id() {
    static std::atomic<uint32_t> ctr{0};
    return "cto_" + std::to_string(++ctr);
}

// Extract string value for key — searches recursively
inline std::string get_str(const std::string& json, const std::string& key) {
    const std::string search = "\"" + key + "\"";
    size_t kpos = 0;
    while ((kpos = json.find(search, kpos)) != std::string::npos) {
        size_t colon = json.find(':', kpos + search.size());
        if (colon == std::string::npos) break;
        size_t p = colon + 1;
        while (p < json.size() && (json[p]==' '||json[p]=='\t')) ++p;
        if (p < json.size() && json[p] == '"') {
            size_t q2 = json.find('"', p + 1);
            if (q2 != std::string::npos) return json.substr(p + 1, q2 - p - 1);
        }
        ++kpos;
    }
    return "";
}

// Extract integer — handles quoted and unquoted, searches anywhere in json
inline int64_t get_int(const std::string& json, const std::string& key) {
    const std::string search = "\"" + key + "\"";
    size_t kpos = json.find(search);
    if (kpos == std::string::npos) return 0;
    size_t colon = json.find(':', kpos + search.size());
    if (colon == std::string::npos) return 0;
    size_t p = colon + 1;
    while (p < json.size() && (json[p]==' '||json[p]=='\t')) ++p;
    bool neg = false;
    if (p < json.size() && json[p] == '-') { neg=true; ++p; }
    bool q = (p < json.size() && json[p] == '"'); if (q) ++p;
    int64_t v = 0; bool has = false;
    while (p < json.size() && json[p] >= '0' && json[p] <= '9') { v=v*10+(json[p++]-'0'); has=true; }
    return has ? (neg ? -v : v) : 0;
}

inline uint64_t get_u64(const std::string& j, const std::string& k) {
    return static_cast<uint64_t>(std::max(int64_t(0), get_int(j, k)));
}

// Extract the "payload" nested object  
inline std::string get_payload(const std::string& json) {
    const size_t kpos = json.find("\"payload\"");
    if (kpos == std::string::npos) return {};
    size_t brace = json.find('{', kpos + 9);
    if (brace == std::string::npos) return {};
    int d = 0;
    for (size_t i = brace; i < json.size(); ++i) {
        if (json[i]=='{') ++d;
        else if (json[i]=='}') { --d; if (!d) return json.substr(brace, i-brace+1); }
    }
    return {};
}

// Extract array of objects
inline std::vector<std::string> get_array(const std::string& json, const std::string& key) {
    std::vector<std::string> res;
    const std::string search = "\"" + key + "\"";
    size_t kpos = json.find(search);
    if (kpos == std::string::npos) return res;
    size_t bracket = json.find('[', kpos + search.size());
    if (bracket == std::string::npos) return res;
    int depth = 0; size_t obj_start = std::string::npos;
    for (size_t i = bracket; i < json.size(); ++i) {
        if      (json[i]=='['||json[i]=='{') { if(json[i]=='{'&&depth==1) obj_start=i; ++depth; }
        else if (json[i]==']'||json[i]=='}') {
            --depth;
            if (json[i]=='}'&&depth==1&&obj_start!=std::string::npos) {
                res.push_back(json.substr(obj_start, i-obj_start+1));
                obj_start = std::string::npos;
            }
            if (!depth) break;
        }
    }
    return res;
}

inline std::vector<uint64_t> get_u64_array(const std::string& json, const std::string& key) {
    std::vector<uint64_t> res;
    const std::string search = "\"" + key + "\"";
    size_t kpos = json.find(search);
    if (kpos == std::string::npos) return res;
    size_t bracket = json.find('[', kpos + search.size());
    if (bracket == std::string::npos) return res;
    size_t p = bracket + 1;
    while (p < json.size() && json[p] != ']') {
        while (p < json.size() && (json[p]==' '||json[p]==','||json[p]=='\n'||json[p]=='\r')) ++p;
        if (p >= json.size() || json[p] == ']') break;
        bool q = (json[p] == '"'); if (q) ++p;
        uint64_t v = 0; bool has = false;
        while (p < json.size() && json[p] >= '0' && json[p] <= '9') { v=v*10+(json[p++]-'0'); has=true; }
        if (q && p < json.size() && json[p] == '"') ++p;
        if (has) res.push_back(v);
    }
    return res;
}

// Build correct cTrader JSON messages per spec:
// {"clientMsgId":"id","payloadType":N,"payload":{fields}}
inline std::string build(int pt, const std::string& fields) {
    return "{\"clientMsgId\":\"" + make_id() + "\",\"payloadType\":" +
           std::to_string(pt) + ",\"payload\":{" + fields + "}}";
}

inline std::string app_auth(const std::string& cid, const std::string& sec) {
    return build(CTraderPT::APPLICATION_AUTH_REQ,
                 "\"clientId\":\"" + cid + "\",\"clientSecret\":\"" + sec + "\"");
}
inline std::string account_auth(int64_t ctid, const std::string& tok) {
    return build(CTraderPT::ACCOUNT_AUTH_REQ,
                 "\"ctidTraderAccountId\":" + std::to_string(ctid) +
                 ",\"accessToken\":\"" + tok + "\"");
}
inline std::string symbols_list(int64_t ctid) {
    return build(CTraderPT::SYMBOLS_LIST_REQ,
                 "\"ctidTraderAccountId\":" + std::to_string(ctid));
}
inline std::string subscribe_depth(int64_t ctid, const std::vector<int64_t>& ids) {
    std::ostringstream b;
    b << "\"ctidTraderAccountId\":" << ctid << ",\"symbolId\":[";
    for (size_t i = 0; i < ids.size(); ++i) { if (i) b << ','; b << ids[i]; }
    b << "]";
    return build(CTraderPT::SUBSCRIBE_DEPTH_REQ, b.str());
}
inline std::string refresh_token_req(const std::string& rt) {
    return build(CTraderPT::REFRESH_TOKEN_REQ, "\"refreshToken\":\"" + rt + "\"");
}
inline std::string heartbeat() {
    return build(CTraderPT::HEARTBEAT, "");
}

} // namespace CTJSON

// Incremental order book — maintained from ProtoOADepthEvent
struct CTDepthQuote { uint64_t price_raw=0, size_raw=0; bool is_bid=false; };

struct CTDepthBook {
    std::unordered_map<uint64_t,CTDepthQuote> quotes;

    // Previous snapshot totals for stateful signals (updated on each event)
    double prev_bid1_size = 0.0;  // L1 bid size before last event
    double prev_ask1_size = 0.0;  // L1 ask size before last event
    double prev_total_vol = 0.0;  // total book volume before last event

    void apply_new(uint64_t id, uint64_t p, uint64_t s, bool bid) { quotes[id]={p,s,bid}; }
    void apply_del(uint64_t id) { quotes.erase(id); }

    // Snapshot current state for stateful signals before updating
    void snapshot_prev(const L2Book& current) noexcept {
        prev_bid1_size = (current.bid_count > 0) ? current.bids[0].size : 0.0;
        prev_ask1_size = (current.ask_count > 0) ? current.asks[0].size : 0.0;
        prev_total_vol = 0.0;
        for (int i=0;i<current.bid_count;++i) prev_total_vol += current.bids[i].size;
        for (int i=0;i<current.ask_count;++i) prev_total_vol += current.asks[i].size;
    }

    // ── Queue pull signals ────────────────────────────────────────────────────
    // Ask L1 shrank >50% → sellers pulled liquidity → upward impulse likely
    // Bid L1 shrank >50% → buyers pulled liquidity → downward impulse likely
    bool queue_pull_up(const L2Book& current, double pull_thresh = 0.50) const noexcept {
        if (prev_ask1_size <= 0.0 || current.ask_count == 0) return false;
        const double cur = current.asks[0].size;
        if (cur <= 0.0) return false;
        return (prev_ask1_size - cur) / prev_ask1_size > pull_thresh;
    }
    bool queue_pull_down(const L2Book& current, double pull_thresh = 0.50) const noexcept {
        if (prev_bid1_size <= 0.0 || current.bid_count == 0) return false;
        const double cur = current.bids[0].size;
        if (cur <= 0.0) return false;
        return (prev_bid1_size - cur) / prev_bid1_size > pull_thresh;
    }

    // ── Pull ratio ────────────────────────────────────────────────────────────
    // Total book volume shrank significantly → thin book → fast move alert
    // Returns 0..1 where 1.0 = book completely emptied
    double pull_ratio(const L2Book& current) const noexcept {
        if (prev_total_vol <= 0.0) return 0.0;
        double cur_total = 0.0;
        for (int i=0;i<current.bid_count;++i) cur_total += current.bids[i].size;
        for (int i=0;i<current.ask_count;++i) cur_total += current.asks[i].size;
        const double delta = prev_total_vol - cur_total;
        if (delta <= 0.0) return 0.0;  // book grew, no pull
        return delta / prev_total_vol;
    }

    L2Book to_l2book() const {
        L2Book book;
        struct Lv { double price, size; };
        std::vector<Lv> bids, asks;
        for (const auto& kv : quotes) {
            if (!kv.second.price_raw || !kv.second.size_raw) continue;
            Lv lv{kv.second.price_raw/100000.0, kv.second.size_raw/100.0};
            if (kv.second.is_bid) bids.push_back(lv); else asks.push_back(lv);
        }
        std::sort(bids.begin(),bids.end(),[](const Lv&a,const Lv&b){return a.price>b.price;});
        std::sort(asks.begin(),asks.end(),[](const Lv&a,const Lv&b){return a.price<b.price;});
        book.bid_count=(int)std::min(bids.size(),size_t(5));
        book.ask_count=(int)std::min(asks.size(),size_t(5));
        for(int i=0;i<book.bid_count;++i) book.bids[i]={bids[i].price,bids[i].size};
        for(int i=0;i<book.ask_count;++i) book.asks[i]={asks[i].price,asks[i].size};
        return book;
    }
};

class CTraderDepthClient {
public:
    std::string client_id, client_secret, access_token, refresh_token;
    int64_t     ctid_account_id = 0;
    std::unordered_set<std::string> symbol_whitelist;
    std::mutex*                             l2_mtx   = nullptr;
    std::unordered_map<std::string,L2Book>* l2_books = nullptr;

    std::atomic<bool>     running{false};
    std::atomic<bool>     depth_active{false};
    std::atomic<uint64_t> depth_events_total{0};
    std::atomic<uint64_t> depth_events_last_min{0};

    bool configured() const {
        return !client_id.empty() && !access_token.empty() && ctid_account_id > 0;
    }

    void start() {
        if (!configured()) {
            std::cout << "[CTRADER] Not configured — add [ctrader_api] to omega_config.ini\n"; return;
        }
        if (running.exchange(true)) return;
        thread_ = std::thread([this]{ loop(); });
        std::cout << "[CTRADER] Depth client started (ctid=" << ctid_account_id << ")\n";
    }
    void stop() { running.store(false); if (thread_.joinable()) thread_.join(); }
    ~CTraderDepthClient() { stop(); }

private:
    std::thread thread_;
    std::unordered_map<std::string,CTDepthBook> depth_books_;
    std::unordered_map<uint64_t,std::string>    id_to_name_;
    std::vector<uint8_t> recv_buf_;

    void loop() {
        // Delay start by 8s to let FIX connections establish first
        // Prevents cTrader thread from interfering with FIX logon timing
        sleep_ms(8000);
        int backoff = 2000;
        while (running.load()) {
            depth_active.store(false);
            std::cout << "[CTRADER] Connecting live.ctraderapi.com:5035 (TCP+JSON)\n";
            int sock = -1;
            SSL* ssl = connect_ssl("live.ctraderapi.com", 5035, sock);
            if (!ssl) {
                std::cerr << "[CTRADER] Connect failed — retry " << backoff << "ms\n";
                sleep_ms(backoff); backoff = std::min(backoff*2, 60000); continue;
            }
            recv_buf_.clear();
            backoff = 2000;
            if (!ws_upgrade(ssl, "live.ctraderapi.com")) {
                std::cerr << "[CTRADER] WebSocket upgrade failed\n";
                ssl_close(ssl,sock); sleep_ms(3000); continue;
            }
            if (!do_auth(ssl)) {
                std::cerr << "[CTRADER] Auth failed — retry 10s\n";
                ssl_close(ssl,sock); sleep_ms(3000); continue;  // interruptible 3s
            }
            depth_active.store(true);
            std::cout << "[CTRADER] Depth feed ACTIVE — " << depth_books_.size() << " symbols subscribed\n";
            recv_loop(ssl, sock);
            ssl_close(ssl, sock); depth_active.store(false);
            if (running.load()) {
                std::cerr << "[CTRADER] Disconnected — retry " << backoff << "ms\n";
                sleep_ms(backoff); backoff = std::min(backoff*2, 60000);
            }
        }
        std::cout << "[CTRADER] Stopped\n";
    }

    bool do_auth(SSL* ssl) {
        // Step 1: Application auth
        // Send immediately after WebSocket upgrade — server expects auth within ~1s
        if (!send_json(ssl, CTJSON::app_auth(client_id, client_secret))) return false;
        std::cout << "[CTRADER] ApplicationAuthReq sent (clientId=" << client_id.substr(0,12) << "...)\n";
        int pt; std::string body;
        if (!wait_for(ssl, CTraderPT::APPLICATION_AUTH_RES, 15000, pt, body)) {
            std::cerr << "[CTRADER] ApplicationAuthRes failed — last_pt=" << pt << " body_len=" << body.size() << "\n";
            if (!body.empty()) std::cerr << "[CTRADER] Last body: " << body.substr(0,200) << "\n";
            return false;
        }
        std::cout << "[CTRADER] Application authorized\n";

        // Step 2: Account auth
        if (!send_json(ssl, CTJSON::account_auth(ctid_account_id, access_token))) return false;
        std::cout << "[CTRADER] AccountAuthReq sent\n";
        if (!wait_for(ssl, CTraderPT::ACCOUNT_AUTH_RES, 10000, pt, body)) return false;
        std::cout << "[CTRADER] Account " << ctid_account_id << " authorized\n";

        // Step 3: Symbol list
        if (!send_json(ssl, CTJSON::symbols_list(ctid_account_id))) return false;
        if (!wait_for(ssl, CTraderPT::SYMBOLS_LIST_RES, 20000, pt, body)) {
            std::cerr << "[CTRADER] SymbolsListRes timeout\n"; return false;
        }
        const std::string pl = CTJSON::get_payload(body);
        const std::string& src = pl.empty() ? body : pl;
        const auto syms = CTJSON::get_array(src, "symbol");
        std::cout << "[CTRADER] Symbol list: " << syms.size() << " symbols\n";

        std::vector<int64_t> sub_ids;
        id_to_name_.clear(); depth_books_.clear();
        for (const auto& s : syms) {
            const std::string name = CTJSON::get_str(s, "symbolName");
            const int64_t id = CTJSON::get_int(s, "symbolId");
            if (name.empty() || id <= 0) continue;
            id_to_name_[uint64_t(id)] = name;
            if (symbol_whitelist.count(name)) {
                sub_ids.push_back(id);
                depth_books_[name] = CTDepthBook{};
                std::cout << "[CTRADER] Subscribe: " << name << " id=" << id << "\n";
            }
        }
        if (sub_ids.empty()) {
            std::cerr << "[CTRADER] No whitelisted symbols found\n";
            int n=0; for (const auto& s:syms) {
                std::cout << "[CTRADER]  available: " << CTJSON::get_str(s,"symbolName") << "\n";
                if (++n>=15) break;
            }
            return false;
        }

        // Step 4: Subscribe depth
        if (!send_json(ssl, CTJSON::subscribe_depth(ctid_account_id, sub_ids))) return false;
        if (!wait_for(ssl, CTraderPT::SUBSCRIBE_DEPTH_RES, 10000, pt, body)) {
            std::cerr << "[CTRADER] SubscribeDepthRes timeout\n"; return false;
        }
        std::cout << "[CTRADER] Subscribed to " << sub_ids.size() << " symbols\n";
        return true;
    }

    void recv_loop(SSL* ssl, int) {
        auto last_hb   = std::chrono::steady_clock::now();
        auto last_diag = std::chrono::steady_clock::now();
        uint64_t events_min = 0;
        while (running.load()) {
            const auto now = std::chrono::steady_clock::now();
            // Heartbeat every 10s
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_hb).count() >= 10) {
                send_json(ssl, CTJSON::heartbeat()); last_hb = now;
            }
            // Diagnostic log every 60s
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_diag).count() >= 60) {
                const uint64_t tot = depth_events_total.load();
                std::cout << "[CTRADER-STATUS] events_total=" << tot
                          << " this_min=" << events_min
                          << " symbols=" << depth_books_.size() << "\n";
                if (l2_mtx && l2_books) {
                    std::lock_guard<std::mutex> lk(*l2_mtx);
                    for (const char* sym : {"GOLD.F","EURUSD","XAGUSD","US500.F","GBPUSD"}) {
                        auto it = l2_books->find(sym);
                        if (it!=l2_books->end() && it->second.bid_count>0) {
                            const auto& b = it->second;
                            std::cout << "[CTRADER-L2] " << sym
                                      << " levels=" << b.depth_levels()
                                      << " imb=" << std::fixed << std::setprecision(3) << b.imbalance()
                                      << " bid=" << b.bids[0].price << "x" << b.bids[0].size
                                      << " ask=" << b.asks[0].price << "x" << b.asks[0].size << "\n";
                        }
                    }
                }
                depth_events_last_min.store(events_min); events_min=0; last_diag=now;
            }
            int pt; std::string body;
            const int rc = read_one(ssl, pt, body, 100);
            if (rc < 0) { std::cerr << "[CTRADER] Connection error\n"; return; }
            if (rc == 0) continue;
            if (pt == CTraderPT::DEPTH_EVENT) {
                on_depth_event(body); ++depth_events_total; ++events_min;
            } else if (pt == CTraderPT::ERROR_RES) {
                const std::string pl = CTJSON::get_payload(body);
                const std::string& s = pl.empty()?body:pl;
                std::cerr << "[CTRADER] Error: " << CTJSON::get_str(s,"errorCode")
                          << " — " << CTJSON::get_str(s,"description") << "\n";
                if (CTJSON::get_str(s,"errorCode") == "OA_AUTH_TOKEN_EXPIRED") return;
            } else if (pt == CTraderPT::REFRESH_TOKEN_RES) {
                const std::string pl = CTJSON::get_payload(body);
                const std::string& s = pl.empty()?body:pl;
                const std::string na = CTJSON::get_str(s,"accessToken");
                if (!na.empty()) {
                    access_token = na;
                    refresh_token = CTJSON::get_str(s,"refreshToken");
                    std::cout << "[CTRADER] Token refreshed\n";
                }
            }
        }
    }

    void on_depth_event(const std::string& body) {
        const std::string pl = CTJSON::get_payload(body);
        const std::string& src = pl.empty() ? body : pl;
        const uint64_t sym_id = CTJSON::get_u64(src, "symbolId");
        if (!sym_id) return;
        const auto it = id_to_name_.find(sym_id);
        if (it == id_to_name_.end()) return;
        const std::string& name = it->second;
        auto& book = depth_books_[name];

        // Snapshot current state BEFORE applying new event (for stateful signals)
        if (l2_mtx && l2_books) {
            std::lock_guard<std::mutex> lk(*l2_mtx);
            const auto bit = l2_books->find(name);
            if (bit != l2_books->end()) book.snapshot_prev(bit->second);
        }

        // Apply incremental updates
        for (const auto& q : CTJSON::get_array(src, "newQuotes")) {
            uint64_t id=CTJSON::get_u64(q,"id"), sz=CTJSON::get_u64(q,"size");
            uint64_t bid=CTJSON::get_u64(q,"bid"), ask=CTJSON::get_u64(q,"ask");
            if (!id||!sz) continue;
            if (bid)      book.apply_new(id,bid,sz,true);
            else if (ask) book.apply_new(id,ask,sz,false);
        }
        for (uint64_t did : CTJSON::get_u64_array(src,"deletedQuotes")) book.apply_del(did);

        // Rebuild and write to shared L2 book
        if (l2_mtx && l2_books) {
            const L2Book rebuilt = book.to_l2book();
            std::lock_guard<std::mutex> lk(*l2_mtx);
            (*l2_books)[name] = rebuilt;
        }
    }

    // Send JSON with newline terminator.
    // Port 5035 raw TCP accepts newline-delimited JSON (no 4-byte length prefix).
    // Confirmed: server responds with raw {"payloadType":...}\n (no length prefix).
    bool send_json(SSL* ssl, const std::string& json) { return ws_send(ssl, json); }

    // Read one newline-delimited JSON message from SSL stream
    int read_one(SSL* ssl, int& pt_out, std::string& body_out, int timeout_ms) {
        if (try_parse(pt_out, body_out)) return 1;
        const auto dead = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (running.load() && std::chrono::steady_clock::now() < dead) {
            uint8_t buf[8192];
            int n = SSL_read(ssl, buf, sizeof(buf));
            if (n > 0) {
                recv_buf_.insert(recv_buf_.end(), buf, buf+n);
                if (try_parse(pt_out, body_out)) return 1;
            } else {
                int e = SSL_get_error(ssl, n);
                if (e == SSL_ERROR_WANT_READ) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
                return -1;
            }
        }
        if (!running.load()) return -1;  // shutdown signal
        return 0;
    }

    // Parse one newline-delimited JSON message from recv_buf_
    // Each message is a complete JSON string terminated by \n
    bool try_parse(int& pt_out, std::string& body_out) { return ws_try_parse(pt_out, body_out); }

    bool wait_for(SSL* ssl, int expected, int timeout_ms, int& pt_out, std::string& body_out) {
        const auto dead = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < dead) {
            int pt; std::string body;
            int rc = read_one(ssl, pt, body, 500);
            if (rc < 0) { std::cerr << "[CTRADER] Connection lost while waiting for pt=" << expected << "\n"; return false; }
            if (rc == 0) continue;
            // Log every received message during auth for full diagnostics
            std::cout << "[CTRADER-RECV] payloadType=" << pt << " body_len=" << body.size()
                      << " preview=" << body.substr(0,120) << "\n";
            std::cout.flush();
            if (pt == -1) { std::cerr << "[CTRADER] Server sent WS close frame\n"; return false; }
            if (pt == expected) { pt_out=pt; body_out=body; return true; }
            // Heartbeat (52): echo back to keep connection alive
            if (pt == CTraderPT::HEARTBEAT) {
                send_json(ssl, CTJSON::heartbeat());
                std::cout << "[CTRADER] Heartbeat echoed\n";
                continue;
            }
            if (pt == CTraderPT::ERROR_RES) {
                const std::string pl = CTJSON::get_payload(body);
                const std::string& s = pl.empty()?body:pl;
                std::cerr << "[CTRADER] ERROR_RES: " << CTJSON::get_str(s,"errorCode")
                          << " — " << CTJSON::get_str(s,"description") << "\n";
                return false;
            }
        }
        std::cerr << "[CTRADER] Timeout waiting for payloadType=" << expected << "\n";
        return false;
    }


    // =========================================================================
    // WebSocket support — cTrader requires WS handshake before JSON exchange
    // =========================================================================
    bool ws_upgrade(SSL* ssl, const char* host) {
        const char* key = "dGhlIHNhbXBsZSBub25jZQ==";
        std::string req = std::string("GET / HTTP/1.1\r\nHost: ") + host +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " +
            key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        if (SSL_write(ssl, req.c_str(), (int)req.size()) != (int)req.size()) return false;
        char buf[1024] = {}; int n = 0;
        const auto dead = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (running.load() && std::chrono::steady_clock::now() < dead && n < 1000) {
            int r = SSL_read(ssl, buf+n, 1023-n);
            if (r > 0) {
                n += r; std::string s(buf, n);
                if (s.find("101") != std::string::npos && s.find("\r\n\r\n") != std::string::npos) {
                    std::cout << "[CTRADER] WebSocket upgrade OK\n"; return true;
                }
                if (s.find("\r\n\r\n") != std::string::npos) {
                    std::cerr << "[CTRADER] WS upgrade failed: " << s.substr(0,100) << "\n"; return false;
                }
            } else {
                if (SSL_get_error(ssl,r)==SSL_ERROR_WANT_READ) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue;
                }
                return false;
            }
        }
        return false;
    }

    bool ws_send(SSL* ssl, const std::string& json) {
        const size_t plen = json.size();
        std::vector<uint8_t> f;
        f.push_back(0x81);
        if (plen <= 125)      { f.push_back(0x80|(uint8_t)plen); }
        else if (plen<=65535) { f.push_back(0x80|126); f.push_back((plen>>8)&0xFF); f.push_back(plen&0xFF); }
        else { f.push_back(0x80|127); for(int i=7;i>=0;--i) f.push_back((plen>>(8*i))&0xFF); }
        const uint8_t mk[4]={0x37,0xfa,0x21,0x3d};
        f.insert(f.end(),mk,mk+4);
        for(size_t i=0;i<plen;++i) f.push_back((uint8_t)json[i]^mk[i%4]);
        return SSL_write(ssl,f.data(),(int)f.size())==(int)f.size();
    }

    bool ws_try_parse(int& pt_out, std::string& body_out) {
        if (recv_buf_.size() < 2) return false;
        const uint8_t b0=recv_buf_[0], b1=recv_buf_[1];
        const uint8_t opcode = b0 & 0x0F;
        const bool masked = (b1 & 0x80) != 0;
        size_t plen = b1 & 0x7F, hlen = 2;
        if (plen==126) { if(recv_buf_.size()<4) return false; plen=((size_t)recv_buf_[2]<<8)|recv_buf_[3]; hlen=4; }
        else if (plen==127) { if(recv_buf_.size()<10) return false; plen=0; for(int i=0;i<8;++i) plen=(plen<<8)|recv_buf_[2+i]; hlen=10; }
        if (masked) hlen += 4;
        if (recv_buf_.size() < hlen+plen) return false;
        if (opcode==9) { recv_buf_.erase(recv_buf_.begin(),recv_buf_.begin()+hlen+plen); return false; }
        if (opcode==8) { recv_buf_.clear(); pt_out=-1; return true; }
        const uint8_t* ps = recv_buf_.data()+hlen;
        if (masked) {
            const uint8_t* mk=recv_buf_.data()+hlen-4;
            body_out.resize(plen);
            for(size_t i=0;i<plen;++i) body_out[i]=(char)(ps[i]^mk[i%4]);
        } else {
            body_out.assign((const char*)ps, plen);
        }
        recv_buf_.erase(recv_buf_.begin(), recv_buf_.begin()+hlen+plen);
        while(!body_out.empty()&&(body_out.back()=='\n'||body_out.back()=='\r')) body_out.pop_back();
        if (body_out.empty()) return false;
        pt_out = int(CTJSON::get_int(body_out,"payloadType"));
        return true;
    }

    static SSL* connect_ssl(const char* host, int port, int& sock_out) {
        sock_out = -1;
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
        const std::string ps = std::to_string(port);
        if (getaddrinfo(host, ps.c_str(), &hints, &res)!=0 || !res) return nullptr;
        SOCKET s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s==INVALID_SOCKET) { freeaddrinfo(res); return nullptr; }
        if (::connect(s, res->ai_addr, (int)res->ai_addrlen)!=0) {
            freeaddrinfo(res); closesocket(s); return nullptr; }
        freeaddrinfo(res);
        DWORD to=100;
        setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(const char*)&to,sizeof(to));
        setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,(const char*)&to,sizeof(to));
        static SSL_CTX* ctx=nullptr;
        if (!ctx) {
            ctx=SSL_CTX_new(TLS_client_method());
            SSL_CTX_set_min_proto_version(ctx,TLS1_2_VERSION);
            SSL_CTX_set_quiet_shutdown(ctx,1);
        }
        SSL* ssl=SSL_new(ctx);
        SSL_set_fd(ssl,(int)s);
        SSL_set_tlsext_host_name(ssl,host);
        if (SSL_connect(ssl)<=0) { ERR_print_errors_fp(stderr); closesocket(s); SSL_free(ssl); return nullptr; }
        sock_out=(int)s; return ssl;
    }
    static void ssl_close(SSL* ssl, int sock) { if(ssl) SSL_free(ssl); if(sock>=0) closesocket(sock); }
    void sleep_ms(int ms) {
        // Interruptible: wakes every 50ms to check running flag
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (running.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
};

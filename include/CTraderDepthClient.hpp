#pragma once
// =============================================================================
// CTraderDepthClient.hpp — cTrader Open API v2 Depth-of-Market feed
//
// PROTOCOL (from official proto source):
//
// TRANSPORT: raw TCP TLS to live.ctraderapi.com:5035 (NO WebSocket)
//
// FRAMING (every message both directions):
//   [4 bytes big-endian uint32: length of protobuf payload that follows]
//   [N bytes: protobuf-encoded ProtoMessage]
//
// ProtoMessage wrapper (OpenApiCommonMessages.proto):
//   field 1 (varint):  payloadType  (uint32)
//   field 2 (bytes):   payload      (serialized inner message)
//   field 3 (bytes):   clientMsgId  (optional string)
//
// AUTH SEQUENCE:
//   1. ApplicationAuthReq  (pt=2100)  clientId(f2), clientSecret(f3)
//   2. ApplicationAuthRes  (pt=2101)
//   3. AccountAuthReq      (pt=2102)  ctidTraderAccountId(f2), accessToken(f3)
//   4. AccountAuthRes      (pt=2103)
//   5. SymbolsListReq      (pt=2114)  ctidTraderAccountId(f2)
//   6. SymbolsListRes      (pt=2115)  repeated LightSymbol: symbolId(f2), symbolName(f3)
//   7. SubscribeDepthReq   (pt=2156)  ctidTraderAccountId(f2), repeated symbolId(f3)
//   8. SubscribeDepthRes   (pt=2157)
//   9. DepthEvent          (pt=2155)  continuous stream
//      symbolId(f3), newQuotes(f4,repeated), deletedQuotes(f5,packed)
//      DepthQuote: id(f1), size(f3), bid(f4), ask(f5)  — official proto field numbers (OpenApiModelMessages.proto)
//
// HEARTBEAT: pt=51, empty payload, every 10s
// =============================================================================

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <chrono>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib,"ws2_32.lib")
  #define sock_close closesocket
  typedef SOCKET sock_t;
  static const sock_t BAD_SOCK = INVALID_SOCKET;
#else
  #include <sys/socket.h>
  #include <netdb.h>
  #include <unistd.h>
  #define sock_close close
  typedef int sock_t;
  static const sock_t BAD_SOCK = -1;
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>
#include "OmegaFIX.hpp"

// =============================================================================
// Minimal hand-rolled protobuf encode/decode — no external library
// =============================================================================
namespace PB {

inline void write_varint(std::vector<uint8_t>& out, uint64_t v) {
    do { uint8_t b = v & 0x7F; v >>= 7; if (v) b |= 0x80; out.push_back(b); } while (v);
}
inline void write_field_varint(std::vector<uint8_t>& out, int f, uint64_t v) {
    write_varint(out, (static_cast<uint64_t>(f) << 3) | 0);
    write_varint(out, v);
}
inline void write_field_string(std::vector<uint8_t>& out, int f, const std::string& s) {
    write_varint(out, (static_cast<uint64_t>(f) << 3) | 2);
    write_varint(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}
inline void write_field_bytes(std::vector<uint8_t>& out, int f, const std::vector<uint8_t>& b) {
    write_varint(out, (static_cast<uint64_t>(f) << 3) | 2);
    write_varint(out, b.size());
    out.insert(out.end(), b.begin(), b.end());
}

// Wrap inner bytes in ProtoMessage and add 4-byte length prefix
inline std::vector<uint8_t> frame_msg(uint32_t pt, const std::vector<uint8_t>& inner,
                                       const std::string& msg_id = "") {
    static std::atomic<uint32_t> seq{0};
    std::vector<uint8_t> outer;
    write_field_varint(outer, 1, pt);
    if (!inner.empty()) write_field_bytes(outer, 2, inner);
    if (!msg_id.empty()) write_field_string(outer, 3, msg_id);
    else { std::string id = "ct_" + std::to_string(++seq); write_field_string(outer, 3, id); }
    const uint32_t len = static_cast<uint32_t>(outer.size());
    std::vector<uint8_t> framed;
    framed.push_back((len >> 24) & 0xFF);
    framed.push_back((len >> 16) & 0xFF);
    framed.push_back((len >>  8) & 0xFF);
    framed.push_back( len        & 0xFF);
    framed.insert(framed.end(), outer.begin(), outer.end());
    return framed;
}

struct Field {
    int field_num = 0; int wire_type = 0;
    uint64_t varint = 0;
    std::vector<uint8_t> bytes;
};

inline uint64_t read_varint(const uint8_t* d, size_t len, size_t& pos) {
    uint64_t v = 0; int sh = 0;
    while (pos < len) { uint8_t b = d[pos++]; v |= (uint64_t(b&0x7F)<<sh); if(!(b&0x80)) break; sh+=7; }
    return v;
}

inline std::vector<Field> parse(const uint8_t* data, size_t len) {
    std::vector<Field> fields; size_t pos = 0;
    while (pos < len) {
        const uint64_t tag = read_varint(data, len, pos);
        if (pos > len) break;
        Field f; f.field_num = int(tag>>3); f.wire_type = int(tag&7);
        if      (f.wire_type == 0) { f.varint = read_varint(data, len, pos); }
        else if (f.wire_type == 2) { uint64_t sz = read_varint(data,len,pos); if(pos+sz>len) break; f.bytes.assign(data+pos,data+pos+sz); pos+=sz; }
        else if (f.wire_type == 1) { pos+=8; continue; }
        else if (f.wire_type == 5) { pos+=4; continue; }
        else break;
        fields.push_back(std::move(f));
    }
    return fields;
}
inline std::vector<Field> parse(const std::vector<uint8_t>& v) { return parse(v.data(), v.size()); }

inline uint64_t    get_varint(const std::vector<Field>& ff, int fn, uint64_t def=0)      { for(const auto& f:ff) if(f.field_num==fn&&f.wire_type==0) return f.varint; return def; }
inline std::string get_string(const std::vector<Field>& ff, int fn, const std::string& d="") { for(const auto& f:ff) if(f.field_num==fn&&f.wire_type==2) return {f.bytes.begin(),f.bytes.end()}; return d; }
inline std::vector<std::vector<uint8_t>> get_repeated_bytes(const std::vector<Field>& ff, int fn) {
    std::vector<std::vector<uint8_t>> out; for(const auto& f:ff) if(f.field_num==fn&&f.wire_type==2) out.push_back(f.bytes); return out;
}
inline std::vector<uint64_t> get_packed_varints(const std::vector<Field>& ff, int fn) {
    std::vector<uint64_t> out;
    for(const auto& f:ff) { if(f.field_num!=fn||f.wire_type!=2) continue; size_t p=0; while(p<f.bytes.size()) out.push_back(read_varint(f.bytes.data(),f.bytes.size(),p)); }
    return out;
}

// Message builders
inline std::vector<uint8_t> heartbeat() { return frame_msg(51, {}, "hb"); }

inline std::vector<uint8_t> app_auth_req(const std::string& cid, const std::string& sec) {
    std::vector<uint8_t> inner;
    write_field_string(inner, 2, cid);
    write_field_string(inner, 3, sec);
    return frame_msg(2100, inner);
}
inline std::vector<uint8_t> account_auth_req(int64_t ctid, const std::string& tok) {
    std::vector<uint8_t> inner;
    write_field_varint(inner, 2, uint64_t(ctid));
    write_field_string(inner, 3, tok);
    return frame_msg(2102, inner);
}
inline std::vector<uint8_t> symbols_list_req(int64_t ctid) {
    std::vector<uint8_t> inner; write_field_varint(inner, 2, uint64_t(ctid)); return frame_msg(2114, inner);
}
inline std::vector<uint8_t> subscribe_depth_req(int64_t ctid, const std::vector<int64_t>& ids) {
    std::vector<uint8_t> inner;
    write_field_varint(inner, 2, uint64_t(ctid));
    for (int64_t id : ids) write_field_varint(inner, 3, uint64_t(id));
    return frame_msg(2156, inner);
}
inline std::vector<uint8_t> refresh_token_req(const std::string& rt) {
    std::vector<uint8_t> inner; write_field_string(inner, 2, rt); return frame_msg(2173, inner);
}

} // namespace PB


// =============================================================================
// Incremental order book
// =============================================================================
struct CTDepthQuote { uint64_t price_raw=0, size_raw=0; bool is_bid=false; };

struct CTDepthBook {
    std::unordered_map<uint64_t,CTDepthQuote> quotes;
    void apply_new(uint64_t id, uint64_t p, uint64_t s, bool bid) { quotes[id]={p,s,bid}; }
    void apply_del(uint64_t id) { quotes.erase(id); }

    L2Book to_l2book() const {
        L2Book book;
        struct Lv { double price, size; };
        std::vector<Lv> bids, asks;
        for (const auto& kv : quotes) {
            if (!kv.second.price_raw || !kv.second.size_raw) continue;
            Lv lv{ kv.second.price_raw/100000.0, kv.second.size_raw/100.0 };
            if (kv.second.is_bid) bids.push_back(lv); else asks.push_back(lv);
        }
        std::sort(bids.begin(),bids.end(),[](const Lv&a,const Lv&b){return a.price>b.price;});
        std::sort(asks.begin(),asks.end(),[](const Lv&a,const Lv&b){return a.price<b.price;});
        book.bid_count=int(std::min(bids.size(),size_t(5)));
        book.ask_count=int(std::min(asks.size(),size_t(5)));
        for(int i=0;i<book.bid_count;++i) book.bids[i]={bids[i].price,bids[i].size};
        for(int i=0;i<book.ask_count;++i) book.asks[i]={asks[i].price,asks[i].size};
        return book;
    }
};


// =============================================================================
// CTraderDepthClient
// =============================================================================
class CTraderDepthClient {
public:
    std::string client_id, client_secret, access_token, refresh_token;
    int64_t     ctid_account_id = 0;
    std::unordered_set<std::string> symbol_whitelist;
    bool        dump_all_symbols = false;  // if true, log ALL broker symbols on connect

    // Alias map: broker_name → internal_name
    // Populated when broker uses different names than our internal names.
    std::unordered_map<std::string,std::string> name_alias;

    std::mutex*                             l2_mtx   = nullptr;
    std::unordered_map<std::string,L2Book>* l2_books = nullptr;

    // ── Price tick callback — DRIVES TRADING DECISIONS ───────────────────────
    // Called on every depth event with best bid/ask from cTrader matching engine.
    // This replaces the FIX quote feed as the primary price source for on_tick().
    // FIX feed becomes fallback only (used when cTrader depth is stale/disconnected).
    // Signature: (symbol, best_bid, best_ask)
    std::function<void(const std::string&, double, double)> on_tick_fn;

    // Callback: write derived L2 scalars (imbalance, microprice_bias, has_data)
    // to per-symbol atomics — called after every depth event, no lock required.
    // Registered by main.cpp at startup. Signature:
    //   (internal_name, imbalance, microprice_bias, has_data)
    std::function<void(const std::string&, double, double, bool)> atomic_l2_write_fn;

    std::atomic<bool>     running{false};
    std::atomic<bool>     depth_active{false};
    std::atomic<uint64_t> depth_events_total{0};
    // Timestamp (ms since epoch) of most recent depth event — used to detect
    // a silent feed stall: connection alive but broker stopped sending quotes.
    // Common during thin Asian session. Checked by main loop: if age > 30s,
    // l2_active is set to 0 and the L2 badge goes grey even if TCP is up.
    std::atomic<int64_t>  last_depth_event_ms{0};

    bool configured() const { return !client_id.empty() && !access_token.empty() && ctid_account_id > 0; }

    void start() {
        if (!configured()) { std::cout << "[CTRADER] Depth feed disabled — add [ctrader_api] to omega_config.ini\n"; return; }
        if (running.exchange(true)) return;
        thread_ = std::thread([this]{ loop(); });
        std::cout << "[CTRADER] Depth client started (ctid=" << ctid_account_id << ")\n";
    }
    void stop() { running.store(false); if (thread_.joinable()) thread_.join(); }
    ~CTraderDepthClient() { stop(); }

private:
    std::thread thread_;
    std::unordered_map<std::string,CTDepthBook>  depth_books_;
    std::unordered_map<uint64_t,std::string>     id_to_name_;
    std::unordered_map<uint64_t,std::string>     id_to_internal_;  // id → internal name (with alias)
    std::vector<uint8_t> recv_buf_;

    void loop() {
        sleep_ms(30000);  // wait for FIX quote+trade sessions to fully connect first
        int backoff = 5000;
        while (running.load()) {
            depth_active.store(false);
            std::cout << "[CTRADER] Connecting live.ctraderapi.com:5035\n";
            sock_t sock = BAD_SOCK;
            SSL* ssl = connect_ssl("live.ctraderapi.com", 5035, sock);
            if (!ssl) { std::cerr << "[CTRADER] Connect failed — retry " << backoff << "ms\n"; sleep_ms(backoff); backoff=std::min(backoff*2,300000); continue; }
            recv_buf_.clear();
            backoff = 5000;
            if (!do_auth(ssl)) { ssl_close(ssl,sock); sleep_ms(5000); continue; }
            depth_active.store(true);
            std::cout << "[CTRADER] Depth feed ACTIVE — " << depth_books_.size() << " symbols\n";
            recv_loop(ssl, sock);
            ssl_close(ssl,sock); depth_active.store(false);
            if (running.load()) { std::cerr << "[CTRADER] Disconnected — retry " << backoff << "ms\n"; sleep_ms(backoff); backoff=std::min(backoff*2,300000); }
        }
        std::cout << "[CTRADER] Stopped\n";
    }

    bool do_auth(SSL* ssl) {
        if (!send_msg(ssl, PB::app_auth_req(client_id, client_secret))) return false;
        std::cout << "[CTRADER] ApplicationAuthReq sent\n";
        uint32_t pt; std::vector<uint8_t> payload;
        if (!wait_for(ssl, 2101, 15000, pt, payload)) { std::cerr << "[CTRADER] ApplicationAuthRes failed (pt=" << pt << ")\n"; return false; }
        std::cout << "[CTRADER] Application authorized\n";

        if (!send_msg(ssl, PB::account_auth_req(ctid_account_id, access_token))) return false;
        std::cout << "[CTRADER] AccountAuthReq sent\n";
        if (!wait_for(ssl, 2103, 10000, pt, payload)) { std::cerr << "[CTRADER] AccountAuthRes failed\n"; return false; }
        std::cout << "[CTRADER] Account " << ctid_account_id << " authorized\n";

        if (!send_msg(ssl, PB::symbols_list_req(ctid_account_id))) return false;
        if (!wait_for(ssl, 2115, 20000, pt, payload)) { std::cerr << "[CTRADER] SymbolsListRes timeout\n"; return false; }

        // ── XAUUSD spot pin — mirrors FIX side g_id_to_sym[41]="XAUUSD" ─────────
        // BlackBull cTrader symbol list contains two gold entries:
        //   id=41   -> "XAUUSD"  (spot  ~$4580)  <- the one we want
        //   id=2660 -> "GOLD.F"  (futures ~$5200) <- must never be subscribed
        // Confirmed 2026-03-31 via dump_all_symbols diagnostic run.
        // XAUUSD_SPOT_ID=41 is IMMUTABLE — hardcoded exactly as FIX pins spot to id=41.
        // Even if broker renames the symbol or reorders the list, we subscribe
        // id=41 directly and route it to internal name "XAUUSD".
        static constexpr uint64_t XAUUSD_SPOT_ID = 41;  // BlackBull XAUUSD spot — immutable

        // Parse SymbolsListRes — field 3 = repeated ProtoOALightSymbol
        // ProtoOALightSymbol: field 1=symbolId(int64), field 2=symbolName(string)
        id_to_name_.clear(); depth_books_.clear(); id_to_internal_.clear();
        std::vector<int64_t> sub_ids;
        bool xauusd_pinned = false;
        for (const auto& f : PB::parse(payload)) {
            if (f.field_num != 3 || f.wire_type != 2) continue;
            const auto sf = PB::parse(f.bytes);
            const int64_t sid = int64_t(PB::get_varint(sf, 1));
            const std::string sname = PB::get_string(sf, 2);
            if (sid <= 0 || sname.empty()) continue;
            id_to_name_[uint64_t(sid)] = sname;

            // ── XAUUSD spot pin — id=41 is hardcoded, non-negotiable ──────────────────
            if (uint64_t(sid) == XAUUSD_SPOT_ID) {
                sub_ids.push_back(sid);
                depth_books_["XAUUSD"] = CTDepthBook{};
                id_to_internal_[uint64_t(sid)] = "XAUUSD";
                xauusd_pinned = true;
                std::cout << "[CTRADER] PINNED: XAUUSD spot id=" << sid
                          << " (" << sname << ") -- futures blocked\n";
                continue;
            }
            // Block all other XAU/GOLD variants — any ID that is not 41 must not
            // route to XAUUSD. This permanently excludes GOLD.F (id=2660) and any
            // future renamed contracts.
            if (sname.find("XAU") != std::string::npos ||
                sname == "GOLD.F" || sname == "GOLD") {
                std::cout << "[CTRADER] BLOCKED: " << sname << " id=" << sid
                          << " -- not XAUUSD spot (pinned id=" << XAUUSD_SPOT_ID << ")\n";
                continue;
            }

            // All other whitelisted symbols — normal name-based subscription
            if (symbol_whitelist.count(sname)) {
                sub_ids.push_back(sid);
                const std::string internal_name = name_alias.count(sname) ? name_alias.at(sname) : sname;
                depth_books_[internal_name] = CTDepthBook{};
                id_to_internal_[uint64_t(sid)] = internal_name;
                std::cout << "[CTRADER] Subscribe depth: " << sname << " id=" << sid
                          << (internal_name != sname ? " (alias->" + internal_name + ")" : "") << "\n";
            }
        }
        if (!xauusd_pinned) {
            std::cerr << "[CTRADER] CRITICAL: XAUUSD spot id=" << XAUUSD_SPOT_ID
                      << " not in symbol list -- broker may have changed IDs\n";
        }
        std::cout << "[CTRADER] Symbol list: " << id_to_name_.size() << " total, " << sub_ids.size() << " to subscribe\n";
        if (dump_all_symbols) {
            std::cout << "[CTRADER] Available symbols:\n";
            for (const auto& kv : id_to_name_)
                std::cout << "[CTRADER]  avail: " << kv.second << " id=" << kv.first << "\n";
        }
        if (sub_ids.empty()) {
            std::cerr << "[CTRADER] No symbols to subscribe\n";
            return false;
        }

        if (!send_msg(ssl, PB::subscribe_depth_req(ctid_account_id, sub_ids))) return false;
        if (!wait_for(ssl, 2157, 10000, pt, payload)) { std::cerr << "[CTRADER] SubscribeDepthRes timeout\n"; return false; }
        std::cout << "[CTRADER] Subscribed to " << sub_ids.size() << " symbols\n";
        return true;
    }

    void recv_loop(SSL* ssl, sock_t) {
        auto last_hb = std::chrono::steady_clock::now();
        auto last_diag = std::chrono::steady_clock::now();
        uint64_t ev_min = 0;
        std::unordered_map<std::string,uint64_t> ev_per_sym;
        while (running.load()) {
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now-last_hb).count()>=10) { send_msg(ssl,PB::heartbeat()); last_hb=now; }
            if (std::chrono::duration_cast<std::chrono::seconds>(now-last_diag).count()>=60) {
                std::cout<<"[CTRADER-STATUS] events_total="<<depth_events_total.load()<<" this_min="<<ev_min<<" symbols="<<depth_books_.size()<<"\n";
                // Log which symbols are receiving events
                for (const auto& kv : ev_per_sym)
                    if (kv.first == "XAUUSD" || kv.second == 0)
                        std::cout<<"[CTRADER-EVTS] "<<kv.first<<"="<<kv.second<<"\n";
                ev_min=0; ev_per_sym.clear(); last_diag=now;
            }
            uint32_t pt; std::vector<uint8_t> payload;
            const int rc = read_one(ssl, pt, payload, 100);
            if (rc < 0) { std::cerr<<"[CTRADER] Connection error\n"; return; }
            if (rc == 0) continue;
            if      (pt==2155) { on_depth_event(payload); ++depth_events_total; ++ev_min;
                                  // Count per-symbol for diagnostics
                                  const auto fields2 = PB::parse(payload);
                                  const uint64_t sid2 = PB::get_varint(fields2, 3);
                                  const auto iit = id_to_internal_.find(sid2);
                                  if (iit != id_to_internal_.end()) ++ev_per_sym[iit->second];
                                }
            else if (pt==2142) { const auto ef=PB::parse(payload); std::cerr<<"[CTRADER] Error: "<<PB::get_string(ef,2)<<" — "<<PB::get_string(ef,3)<<"\n"; if(PB::get_string(ef,2)=="OA_AUTH_TOKEN_EXPIRED"){if(!refresh_token.empty())send_msg(ssl,PB::refresh_token_req(refresh_token)); return;} }
            else if (pt==2174) { const auto rf=PB::parse(payload); const std::string na=PB::get_string(rf,2); if(!na.empty()){access_token=na;refresh_token=PB::get_string(rf,3);std::cout<<"[CTRADER] Token refreshed\n";} }
            else if (pt==51)   { send_msg(ssl,PB::heartbeat()); }
            else if (pt==2148||pt==2164) { std::cerr<<"[CTRADER] Disconnect pt="<<pt<<"\n"; return; }
        }
    }

    void on_depth_event(const std::vector<uint8_t>& payload) {
        const auto fields = PB::parse(payload);
        const uint64_t sym_id = PB::get_varint(fields, 3);
        if (!sym_id) return;
        // Use internal name (alias-resolved) for book lookup
        const auto it = id_to_internal_.find(sym_id);
        if (it == id_to_internal_.end()) return;
        const std::string& name = it->second;
        // DIAGNOSTIC: log first 5 depth events per symbol to verify sizes are real
        static std::unordered_map<std::string,int> s_diag_count;
        if (s_diag_count[name] < 5) {
            ++s_diag_count[name];
            const auto& qbs = PB::get_repeated_bytes(fields, 4);
            std::cout << "[CTRADER-BOOK] " << name << " id=" << sym_id
                      << " levels=" << qbs.size();
            for (const auto& qb : qbs) {
                const auto qf = PB::parse(qb);
                const uint64_t id=PB::get_varint(qf,1), sz=PB::get_varint(qf,3);
                const uint64_t bid=PB::get_varint(qf,4), ask=PB::get_varint(qf,5);
                std::cout << " [id=" << id
                          << " sz=" << sz
                          << (bid ? " bid=" : " ask=")
                          << (bid ? bid : ask) << "/100000=" 
                          << (bid ? bid : ask)/100000.0 << "]";
            }
            std::cout << "\n";
            std::cout.flush();
        }
        auto& book = depth_books_[name];
        for (const auto& qb : PB::get_repeated_bytes(fields, 4)) {
            const auto qf = PB::parse(qb);
            const uint64_t id=PB::get_varint(qf,1), sz=PB::get_varint(qf,3);
            const uint64_t bid=PB::get_varint(qf,4), ask=PB::get_varint(qf,5);
            if (!id) continue;  // id=0 is invalid
            // cTrader sends real sizes for all symbols including XAUUSD.
            // sz is in cents (sz=200 = 2 lots). Default to 100 (1 lot) if
            // sz=0 is received — should not happen on cTrader but guards against it.
            const uint64_t eff_sz = (sz > 0) ? sz : 100;
            if (bid)      book.apply_new(id, bid, eff_sz, true);
            else if (ask) book.apply_new(id, ask, eff_sz, false);
        }
        for (uint64_t did : PB::get_packed_varints(fields, 5)) book.apply_del(did);
        // Build L2Book snapshot outside the lock — to_l2book() is O(N log N) sort
        const L2Book rebuilt = book.to_l2book();

        // Hot path: write atomic derived scalars — zero lock, zero contention with FIX tick
        if (atomic_l2_write_fn) {
            atomic_l2_write_fn(name,
                rebuilt.imbalance(),
                rebuilt.microprice_bias(),
                rebuilt.has_data());
        }

        // ── Primary price tick — drives on_tick() with cTrader real-time price ─
        // Extract best bid/ask from rebuilt book and call on_tick_fn.
        // This is faster and more accurate than the FIX quote feed which can lag
        // by 0.5-2pts during fast markets due to FIX gateway batching/throttling.
        if (on_tick_fn && rebuilt.bid_count > 0 && rebuilt.ask_count > 0) {
            const double best_bid = rebuilt.bids[0].price;
            const double best_ask = rebuilt.asks[0].price;
            if (best_bid > 0.0 && best_ask > 0.0 && best_ask > best_bid) {
                on_tick_fn(name, best_bid, best_ask);
            }
        }

        // Cold path: write full book under mutex for GUI depth panel (walls, vacuums, slopes)
        if (l2_mtx && l2_books) {
            std::lock_guard<std::mutex> lk(*l2_mtx);
            (*l2_books)[name] = rebuilt;
        }
        // Stamp time of last real depth event for stale detection in main loop
        last_depth_event_ms.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

    bool send_msg(SSL* ssl, const std::vector<uint8_t>& msg) {
        if (!ssl||msg.empty()) return false;
        return SSL_write(ssl,msg.data(),int(msg.size()))==int(msg.size());
    }

    int read_one(SSL* ssl, uint32_t& pt_out, std::vector<uint8_t>& payload_out, int timeout_ms) {
        if (try_parse_frame(pt_out, payload_out)) return 1;
        const auto dead = std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout_ms);
        while (running.load() && std::chrono::steady_clock::now()<dead) {
            uint8_t buf[8192]; int n=SSL_read(ssl,buf,sizeof(buf));
            if (n>0) { recv_buf_.insert(recv_buf_.end(),buf,buf+n); if(try_parse_frame(pt_out,payload_out)) return 1; }
            else {
                int e=SSL_get_error(ssl,n);
                if (e==SSL_ERROR_WANT_READ) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
                // On Windows SO_RCVTIMEO expiry comes back as SSL_ERROR_SYSCALL + WSAETIMEDOUT — treat as timeout not error
                if (e==SSL_ERROR_SYSCALL) {
                    #ifdef _WIN32
                    if (WSAGetLastError()==WSAETIMEDOUT||WSAGetLastError()==0) break;
                    #else
                    if (errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR) break;
                    #endif
                }
                return -1;
            }
        }
        return running.load() ? 0 : -1;
    }

    bool try_parse_frame(uint32_t& pt_out, std::vector<uint8_t>& payload_out) {
        if (recv_buf_.size()<4) return false;
        const uint32_t msg_len = (uint32_t(recv_buf_[0])<<24)|(uint32_t(recv_buf_[1])<<16)|(uint32_t(recv_buf_[2])<<8)|recv_buf_[3];
        if (msg_len>1048576u) { std::cerr<<"[CTRADER] Oversized frame "<<msg_len<<"\n"; recv_buf_.clear(); return false; }
        if (recv_buf_.size()<4+msg_len) return false;
        const auto outer = PB::parse(recv_buf_.data()+4, msg_len);
        pt_out = uint32_t(PB::get_varint(outer, 1));
        const std::string ps = PB::get_string(outer, 2);
        payload_out.assign(ps.begin(), ps.end());
        recv_buf_.erase(recv_buf_.begin(), recv_buf_.begin()+4+msg_len);
        return true;
    }

    bool wait_for(SSL* ssl, uint32_t expected, int timeout_ms, uint32_t& pt_out, std::vector<uint8_t>& payload_out) {
        const auto dead = std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now()<dead) {
            uint32_t pt; std::vector<uint8_t> payload;
            int rc=read_one(ssl,pt,payload,500);
            if (rc<0) { std::cerr<<"[CTRADER] Lost connection waiting for pt="<<expected<<"\n"; return false; }
            if (rc==0) continue;
            if (pt==expected) { pt_out=pt; payload_out=payload; return true; }
            if (pt==51)   { send_msg(ssl,PB::heartbeat()); continue; }
            if (pt==2142) { const auto ef=PB::parse(payload); std::cerr<<"[CTRADER] Error: "<<PB::get_string(ef,2)<<" — "<<PB::get_string(ef,3)<<"\n"; return false; }
        }
        std::cerr<<"[CTRADER] Timeout waiting for pt="<<expected<<"\n"; return false;
    }

    static SSL* connect_ssl(const char* host, int port, sock_t& sock_out) {
        sock_out=BAD_SOCK;
        struct addrinfo hints{},*res=nullptr; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
        const std::string ps=std::to_string(port);
        if (getaddrinfo(host,ps.c_str(),&hints,&res)!=0||!res) return nullptr;
        sock_t s=::socket(res->ai_family,res->ai_socktype,res->ai_protocol);
        if (s==BAD_SOCK) { freeaddrinfo(res); return nullptr; }
        if (::connect(s,res->ai_addr,int(res->ai_addrlen))!=0) { freeaddrinfo(res); sock_close(s); return nullptr; }
        freeaddrinfo(res);
#ifdef _WIN32
        DWORD to=100; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(const char*)&to,sizeof(to)); setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,(const char*)&to,sizeof(to));
#else
        struct timeval to{0,100000}; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&to,sizeof(to)); setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,&to,sizeof(to));
#endif
        static SSL_CTX* ctx=nullptr;
        if (!ctx) { ctx=SSL_CTX_new(TLS_client_method()); SSL_CTX_set_min_proto_version(ctx,TLS1_2_VERSION); SSL_CTX_set_quiet_shutdown(ctx,1); }
        SSL* ssl=SSL_new(ctx); SSL_set_fd(ssl,int(s)); SSL_set_tlsext_host_name(ssl,host);
        if (SSL_connect(ssl)<=0) { ERR_print_errors_fp(stderr); sock_close(s); SSL_free(ssl); return nullptr; }
        sock_out=s; return ssl;
    }
    static void ssl_close(SSL* ssl, sock_t sock) { if(ssl){SSL_shutdown(ssl);SSL_free(ssl);} if(sock!=BAD_SOCK) sock_close(sock); }
    void sleep_ms(int ms) {
        const auto d=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
        while(running.load()&&std::chrono::steady_clock::now()<d) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
};




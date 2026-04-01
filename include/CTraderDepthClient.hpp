#pragma once
// =============================================================================
// CTraderDepthClient.hpp -- cTrader Open API v2 Depth-of-Market feed
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
//      DepthQuote: id(f1), size(f3), bid(f4), ask(f5)  -- official proto field numbers (OpenApiModelMessages.proto)
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
#include "OHLCBarEngine.hpp"

// =============================================================================
// Minimal hand-rolled protobuf encode/decode -- no external library
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
        else if (f.wire_type == 1) {
            // fixed64 -- read 8 bytes little-endian into varint so get_varint() works
            if (pos+8 > len) break;
            f.varint = uint64_t(data[pos]) | (uint64_t(data[pos+1])<<8) |
                       (uint64_t(data[pos+2])<<16) | (uint64_t(data[pos+3])<<24) |
                       (uint64_t(data[pos+4])<<32) | (uint64_t(data[pos+5])<<40) |
                       (uint64_t(data[pos+6])<<48) | (uint64_t(data[pos+7])<<56);
            pos+=8;
        }
        else if (f.wire_type == 5) {
            // fixed32 -- read 4 bytes little-endian
            if (pos+4 > len) break;
            f.varint = uint64_t(data[pos]) | (uint64_t(data[pos+1])<<8) |
                       (uint64_t(data[pos+2])<<16) | (uint64_t(data[pos+3])<<24);
            pos+=4;
        }
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
// PB trendbar message builders -- defined here so write_field_varint/frame_msg
// are in scope. OHLCBarEngine.hpp is included AFTER this namespace.
// =============================================================================
namespace PB {

// ProtoOAGetTrendbarsReq (pt=2137)
// period: 1=M1, 5=M5, 7=M15, 8=M30, 9=H1  (ProtoOATrendbarPeriod enum)
// fromTimestamp (field 5) and toTimestamp (field 6) are REQUIRED by the cTrader API.
// Sending only count without timestamps causes INVALID_REQUEST and disconnects.
// count: max bars -- we derive from_ms = now - count*period_minutes*60*1000
inline std::vector<uint8_t> get_trendbars_req(
    int64_t ctid, int64_t sym_id, uint32_t period, uint32_t count = 200)
{
    // cTrader ProtoOAGetTrendbarsReq timestamps are in MILLISECONDS (UTC epoch ms).
    // fromTimestamp (field 5) and toTimestamp (field 6) are required.
    // Max range per request: 5000 bars. M1x200 = 200 minutes -- well within limit.
    const int64_t now_ms    = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t period_ms = int64_t(period) * 60LL * 1000LL;
    const int64_t from_ms   = now_ms - int64_t(count) * period_ms;
    const int64_t to_ms     = now_ms;
    std::vector<uint8_t> inner;
    write_field_varint(inner, 2, uint64_t(ctid));
    write_field_varint(inner, 3, uint64_t(sym_id));
    write_field_varint(inner, 4, uint64_t(period));
    write_field_varint(inner, 5, uint64_t(from_ms));
    write_field_varint(inner, 6, uint64_t(to_ms));
    return frame_msg(2137, inner);
}

// ProtoOASubscribeLiveTrendbarReq (pt=2220)
inline std::vector<uint8_t> subscribe_trendbar_req(
    int64_t ctid, int64_t sym_id, uint32_t period)
{
    std::vector<uint8_t> inner;
    write_field_varint(inner, 2, uint64_t(ctid));
    write_field_varint(inner, 3, uint64_t(sym_id));
    write_field_varint(inner, 4, uint64_t(period));
    return frame_msg(2220, inner);
}

// Parse ProtoOATrendbar bytes into OHLCBar
// Prices scaled by 100000 (e.g. 464254 = 4642.54)
// Deltas are relative to close: open=close+openDelta, high=close+highDelta, low=close-lowDelta
// openDelta uses sint64 zigzag encoding; high/lowDelta are plain uint64
inline OHLCBar parse_trendbar(const std::vector<uint8_t>& bytes,
                               double price_scale = 100000.0)
{
    OHLCBar bar;
    const auto f = parse(bytes);
    const uint64_t ts_min    = get_varint(f, 2);
    const uint64_t close_raw = get_varint(f, 3);
    const uint64_t vol       = get_varint(f, 5);
    // openDelta: sint64 zigzag -- iterate fields to find wire_type=0 field 6
    int64_t open_delta = 0;
    for (const auto& fi : f) {
        if (fi.field_num == 6 && fi.wire_type == 0) {
            const uint64_t zz = fi.varint;
            open_delta = static_cast<int64_t>((zz >> 1) ^ (-(int64_t)(zz & 1)));
            break;
        }
    }
    const uint64_t high_delta = get_varint(f, 7);
    const uint64_t low_delta  = get_varint(f, 8);
    bar.ts_min = int64_t(ts_min);
    bar.close  = double(close_raw) / price_scale;
    bar.open   = double(int64_t(close_raw) + open_delta) / price_scale;
    bar.high   = double(close_raw + high_delta) / price_scale;
    bar.low    = double(int64_t(close_raw) - int64_t(low_delta)) / price_scale;
    bar.volume = vol;
    return bar;
}

} // namespace PB (trendbar additions)


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
            if (!kv.second.price_raw) continue;  // no price = invalid, skip
            // size_raw=0 means broker sent no tag-271 size -- substitute 1 lot (100 raw)
            // so imbalance() returns 0.5 (neutral) rather than NaN/0 which breaks L2 path.
            // This keeps the price levels visible and prevents has_data()=false falsely
            // killing GoldFlow's L2 path on brokers that omit size data.
            const uint64_t eff_sz = kv.second.size_raw > 0 ? kv.second.size_raw : 100;
            Lv lv{ kv.second.price_raw/100000.0, eff_sz/100.0 };
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
    std::unordered_set<std::string> bar_failed_reqs; // persists across reconnects

    // Alias map: broker_name ? internal_name
    // Populated when broker uses different names than our internal names.
    std::unordered_map<std::string,std::string> name_alias;

    std::mutex*                             l2_mtx   = nullptr;
    std::unordered_map<std::string,L2Book>* l2_books = nullptr;

    // ?? Price tick callback -- DRIVES TRADING DECISIONS ???????????????????????
    // Called on every depth event with best bid/ask from cTrader matching engine.
    // This replaces the FIX quote feed as the primary price source for on_tick().
    // FIX feed becomes fallback only (used when cTrader depth is stale/disconnected).
    // Signature: (symbol, best_bid, best_ask)
    std::function<void(const std::string&, double, double)> on_tick_fn;

    // ?? OHLC bar engines -- trendbar API ??????????????????????????????????????
    // Wire before start(). Client requests 200 M1 + 100 M5 bars on startup,
    // then subscribes to live bar close pushes (pt=2220).
    // bar_subscriptions: internal_name ? {symbol_id, SymBarState*}
    struct BarSub { int64_t sym_id = 0; SymBarState* state = nullptr; };
    std::unordered_map<std::string, BarSub> bar_subscriptions;

    // Optional: called after each bar close + indicator recompute
    std::function<void(const std::string&, int /*period*/, const OHLCBar&)> on_bar_fn;

    // Callback: write derived L2 scalars (imbalance, microprice_bias, has_data)
    // to per-symbol atomics -- called after every depth event, no lock required.
    // Registered by main.cpp at startup. Signature:
    //   (internal_name, imbalance, microprice_bias, has_data)
    std::function<void(const std::string&, double, double, bool)> atomic_l2_write_fn;

    // Check if a symbol has an active depth subscription (by internal name)
    bool has_depth_subscription(const std::string& internal_name) const noexcept {
        return depth_books_.count(internal_name) > 0;
    }

    std::atomic<bool>     running{false};
    std::atomic<bool>     depth_active{false};
    std::atomic<uint64_t> depth_events_total{0};
    // Timestamp (ms since epoch) of most recent depth event -- used to detect
    // a silent feed stall: connection alive but broker stopped sending quotes.
    // Common during thin Asian session. Checked by main loop: if age > 30s,
    // l2_active is set to 0 and the L2 badge goes grey even if TCP is up.
    std::atomic<int64_t>  last_depth_event_ms{0};

    bool configured() const { return !client_id.empty() && !access_token.empty() && ctid_account_id > 0; }

    void start() {
        if (!configured()) { std::cout << "[CTRADER] Depth feed disabled -- add [ctrader_api] to omega_config.ini\n"; return; }
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
    std::unordered_map<uint64_t,std::string>     id_to_internal_;  // id ? internal name (with alias)
    std::vector<uint8_t> recv_buf_;

    void loop() {
        sleep_ms(30000);  // wait for FIX quote+trade sessions to fully connect first
        int backoff = 5000;
        while (running.load()) {
            depth_active.store(false);
            std::cout << "[CTRADER] Connecting live.ctraderapi.com:5035\n";
            sock_t sock = BAD_SOCK;
            SSL* ssl = connect_ssl("live.ctraderapi.com", 5035, sock);
            if (!ssl) { std::cerr << "[CTRADER] Connect failed -- retry " << backoff << "ms\n"; sleep_ms(backoff); backoff=std::min(backoff*2,300000); continue; }
            recv_buf_.clear();
            backoff = 5000;
            if (!do_auth(ssl)) { ssl_close(ssl,sock); sleep_ms(5000); continue; }
            depth_active.store(true);
            std::cout << "[CTRADER] Depth feed ACTIVE -- " << depth_books_.size() << " symbols\n";
            recv_loop(ssl, sock);
            ssl_close(ssl,sock); depth_active.store(false);
            if (running.load()) { std::cerr << "[CTRADER] Disconnected -- retry " << backoff << "ms\n"; sleep_ms(backoff); backoff=std::min(backoff*2,300000); }
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

        // XAUUSD spot identification strategy (two-pass):
        //
        // Pass 1: collect the full symbol list -- build id_to_name_ and a
        //         candidate list of all XAU/GOLD entries.
        //
        // Pass 2: pick the SINGLE correct XAUUSD spot entry:
        //   Primary:  id == 41  (BlackBull FIX confirmed spot ID, immutable)
        //   Fallback: sname == "XAUUSD" and id != known-futures id and not a .P contract
        //   Never subscribe: gold futures, XAUUSD.P, or any XAU* other than
        //                    the one chosen spot entry.
        //
        // Gold futures (id=2660) is ALWAYS blocked regardless of whitelist.
        // No alias may remap "XAUUSD" to anything -- it is its own internal name.

        static constexpr uint64_t XAUUSD_FIX_ID      = 41;    // BlackBull FIX/cTrader spot -- confirmed
        static constexpr uint64_t GOLD_FUTURES_ID     = 2660;  // BlackBull gold futures -- always blocked

        // Pass 1: parse the full symbol list
        id_to_name_.clear(); depth_books_.clear(); id_to_internal_.clear();
        struct SymEntry { int64_t sid; std::string sname; };
        std::vector<SymEntry> all_syms;
        for (const auto& f : PB::parse(payload)) {
            if (f.field_num != 3 || f.wire_type != 2) continue;
            const auto sf = PB::parse(f.bytes);
            const int64_t sid = int64_t(PB::get_varint(sf, 1));
            const std::string sname = PB::get_string(sf, 2);
            if (sid <= 0 || sname.empty()) continue;
            id_to_name_[uint64_t(sid)] = sname;
            all_syms.push_back({sid, sname});
        }

        // Pass 2: find the correct XAUUSD spot entry
        // Primary: id == XAUUSD_FIX_ID (41)
        // Fallback: sname == "XAUUSD" and id != GOLD_FUTURES_ID and not a .P contract
        int64_t xauusd_spot_id = -1;
        std::string xauusd_spot_name;
        for (const auto& e : all_syms) {
            if (uint64_t(e.sid) == XAUUSD_FIX_ID) {
                xauusd_spot_id   = e.sid;
                xauusd_spot_name = e.sname;
                std::cout << "[CTRADER] XAUUSD-PIN: id=" << e.sid
                          << " name=" << e.sname << " (matched FIX id=41)\n";
                break;
            }
        }
        if (xauusd_spot_id < 0) {
            // FIX id not found -- name-based fallback
            for (const auto& e : all_syms) {
                if (e.sname == "XAUUSD"
                    && uint64_t(e.sid) != GOLD_FUTURES_ID
                    && e.sname.find(".P") == std::string::npos) {
                    xauusd_spot_id   = e.sid;
                    xauusd_spot_name = e.sname;
                    std::cout << "[CTRADER] XAUUSD-PIN: id=" << e.sid
                              << " name=" << e.sname << " (name fallback -- FIX id=41 not found)\n";
                    break;
                }
            }
        }
        if (xauusd_spot_id >= 0) {
            depth_books_["XAUUSD"] = CTDepthBook{};
            id_to_internal_[uint64_t(xauusd_spot_id)] = "XAUUSD";
            std::cout << "[CTRADER] PINNED: XAUUSD spot id=" << xauusd_spot_id
                      << " (" << xauusd_spot_name << ") -- all other XAU/gold blocked\n";
        } else {
            std::cerr << "[CTRADER] CRITICAL: cannot identify XAUUSD spot -- no id=41 and no"
                         " name=XAUUSD in symbol list. L2 will be unavailable for gold.\n";
        }

        // Pass 3: subscribe non-gold symbols and resolve bar subscription IDs
        std::vector<int64_t> sub_ids;
        std::unordered_set<std::string> subscribed_internals;  // dedup by internal name
        if (xauusd_spot_id >= 0) {
            sub_ids.push_back(xauusd_spot_id);
            subscribed_internals.insert("XAUUSD");
        }

        for (const auto& e : all_syms) {
            const int64_t sid         = e.sid;
            const std::string& sname  = e.sname;

            // Skip: the XAUUSD spot we already registered
            if (sid == xauusd_spot_id) {
                // Still resolve bar subscription for XAUUSD
                auto bit = bar_subscriptions.find("XAUUSD");
                if (bit != bar_subscriptions.end() && bit->second.sym_id == 0)
                    bit->second.sym_id = sid;
                continue;
            }

            // HARD BLOCK: gold futures and every other XAU/gold variant.
            // Unconditional -- whitelist cannot override.
            if (uint64_t(sid) == GOLD_FUTURES_ID ||
                sname.find("XAU") != std::string::npos ||
                sname == "GOLD" || sname == "XAUUSD") {
                std::cout << "[CTRADER] BLOCKED: " << sname << " id=" << sid
                          << " (gold non-spot)\n";
                continue;
            }

            // All other whitelisted symbols -- normal name-based subscription
            if (symbol_whitelist.count(sname)) {
                // Alias: translate broker name to internal name.
                // XAUUSD is never in the alias map -- handled above.
                if (name_alias.count("XAUUSD")) {
                    std::cerr << "[CTRADER] BUG: name_alias[\"XAUUSD\"] exists -- removing\n";
                    name_alias.erase("XAUUSD");
                }
                const std::string internal_name = name_alias.count(sname) ? name_alias.at(sname) : sname;
                // Dedup: skip if this internal name is already subscribed via another broker name.
                // Example: VIX.F (direct) + VIX (alias->VIX.F) would otherwise send two sub requests.
                if (subscribed_internals.count(internal_name)) {
                    std::cout << "[CTRADER] SKIP-DUP: " << sname << " id=" << sid
                              << " -> " << internal_name << " (already subscribed)\n";
                } else {
                    sub_ids.push_back(sid);
                    subscribed_internals.insert(internal_name);
                    depth_books_[internal_name] = CTDepthBook{};
                    id_to_internal_[uint64_t(sid)] = internal_name;
                    std::cout << "[CTRADER] Subscribe depth: " << sname << " id=" << sid
                              << (internal_name != sname ? " (alias->" + internal_name + ")" : "") << "\n";
                }
            }

            // Resolve bar subscription IDs (for US500.F, USTEC.F, GER40 etc.)
            // EXACT MATCH ONLY -- never resolve via alias. SPX500/NAS100/etc. must
            // not resolve US500.F/USTEC.F bar subs -- wrong instrument, causes
            // INVALID_REQUEST when broker rejects trendbar req for cash index id.
            for (auto& bkv : bar_subscriptions) {
                const std::string& binternal = bkv.first;
                if (bkv.second.sym_id != 0) continue;
                if (sname == binternal) {
                    bkv.second.sym_id = sid;
                    std::cout << "[CTRADER-BARS] Resolved bar sub: " << binternal
                              << " id=" << sid << "\n";
                }
            }
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

        // ?? Bar history requests deferred to recv_loop (staggered) ??
        // Sending trendbar reqs during auth causes INVALID_REQUEST / broker
        // connection drop -- confirmed in prior sessions. Fired from recv_loop
        // 2s after depth is stable, 200ms between each send.

        return true;
    }

    void recv_loop(SSL* ssl, sock_t) {
        auto last_hb   = std::chrono::steady_clock::now();
        auto last_diag = std::chrono::steady_clock::now();
        uint64_t ev_min = 0;
        std::unordered_map<std::string,uint64_t> ev_per_sym;

        // ?? Staggered bar requests ??
        // Fired once per connection, 2s after entering recv_loop so the depth
        // feed is stable. 200ms gap between each message avoids broker rate-limit.
        // Struct: {sym_name, sym_id, period, count}
        struct BarReq { std::string name; int64_t sid; uint32_t period; uint32_t count; };
        // Track bar requests that previously got INVALID_REQUEST -- skip them.
        // Persists across reconnects within this process lifetime.
        // Key format: "SYMBOL:PERIOD" e.g. "XAUUSD:1"
        std::vector<BarReq> pending_bar_reqs;
        for (const auto& bkv : bar_subscriptions) {
            const int64_t sid = bkv.second.sym_id;
            if (sid <= 0) continue;
            const bool is_gold = (bkv.first == "XAUUSD");
            // Skip requests that previously caused INVALID_REQUEST
            auto skip = [&](uint32_t p) {
                return bar_failed_reqs.count(bkv.first + ":" + std::to_string(p)) > 0;
            };
            if (!skip(1)) pending_bar_reqs.push_back({bkv.first, sid, 1, 200});
            else std::cout << "[CTRADER-BARS] Skipping " << bkv.first << " M1 (prev INVALID_REQUEST)\n";
            if (!skip(5)) pending_bar_reqs.push_back({bkv.first, sid, 5, 100});
            if (is_gold && !skip(7)) pending_bar_reqs.push_back({bkv.first, sid, 7, 50});
        }
        // Live subscriptions queued after history requests (sent in same staggered loop)
        struct LiveSub { std::string name; int64_t sid; uint32_t period; };
        std::vector<LiveSub> pending_live_subs;
        // Live trendbar subscriptions (pt=2220) cause UNSUPPORTED_MESSAGE on some brokers.
        // Skip live subs for any symbol/period that already failed a history request.
        // For symbols that work, add live sub after history req.
        for (const auto& bkv : bar_subscriptions) {
            const int64_t sid = bkv.second.sym_id;
            if (sid <= 0) continue;
            const bool is_gold = (bkv.first == "XAUUSD");
            auto skip = [&](uint32_t p) {
                return bar_failed_reqs.count(bkv.first + ":" + std::to_string(p)) > 0;
            };
            if (!skip(1)) pending_live_subs.push_back({bkv.first, sid, 1});
            if (!skip(5)) pending_live_subs.push_back({bkv.first, sid, 5});
            if (is_gold && !skip(7)) pending_live_subs.push_back({bkv.first, sid, 7});
        }
        // Merge: send all history reqs first, then live subs only for non-failed symbols
        struct PendingSend { std::string name; int64_t sid; uint32_t period; uint32_t count; bool is_live; };
        std::vector<PendingSend> bar_send_queue;
        for (const auto& r : pending_bar_reqs)  bar_send_queue.push_back({r.name, r.sid, r.period, r.count, false});
        for (const auto& s : pending_live_subs)  bar_send_queue.push_back({s.name, s.sid, s.period, 0, true});
        size_t bar_send_idx = 0;
        auto   bar_send_next   = std::chrono::steady_clock::now() + std::chrono::seconds(10); // wait for depth subscription ACK before sending bar reqs
        auto   bar_repoll_next = std::chrono::steady_clock::now() + std::chrono::seconds(65);
        bool   bar_repoll_disabled = false;  // set true when broker rejects bar reqs

        while (running.load()) {
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now-last_hb).count()>=10) { send_msg(ssl,PB::heartbeat()); last_hb=now; }

            // Periodic re-poll of bar history (every 65s) for symbols that don't support
            // live trendbar subscriptions. Keeps bar indicators fresh without live push.
            if (!bar_repoll_disabled && now >= bar_repoll_next) {
                bar_repoll_next = now + std::chrono::seconds(65);
                for (const auto& bkv : bar_subscriptions) {
                    const int64_t sid = bkv.second.sym_id;
                    if (sid <= 0) continue;
                    const bool is_gold = (bkv.first == "XAUUSD");
                    auto failed = [&](uint32_t p) {
                        return bar_failed_reqs.count(bkv.first + ":" + std::to_string(p)) > 0;
                    };
                    if (!failed(1)) send_msg(ssl, PB::get_trendbars_req(ctid_account_id, sid, 1, 60));
                    if (!failed(5)) send_msg(ssl, PB::get_trendbars_req(ctid_account_id, sid, 5, 20));
                    if (is_gold && !failed(7)) send_msg(ssl, PB::get_trendbars_req(ctid_account_id, sid, 7, 10));
                }
            }

            // ?? Staggered bar request dispatch ??
            // One message per 200ms tick until queue is drained.
            // History requests go first, then live subscriptions.
            if (bar_send_idx < bar_send_queue.size() && now >= bar_send_next) {
                const auto& req = bar_send_queue[bar_send_idx];
                if (req.is_live) {
                    send_msg(ssl, PB::subscribe_trendbar_req(ctid_account_id, req.sid, req.period));
                    std::cout << "[CTRADER-BARS] " << req.name << " live sub period=" << req.period << "\n";
                } else {
                    send_msg(ssl, PB::get_trendbars_req(ctid_account_id, req.sid, req.period, req.count));
                    std::cout << "[CTRADER-BARS] " << req.name << " history req period=" << req.period
                              << " count=" << req.count << "\n";
                }
                ++bar_send_idx;
                bar_send_next = now + std::chrono::milliseconds(200);
            }

            if (std::chrono::duration_cast<std::chrono::seconds>(now-last_diag).count()>=60) {
                std::cout<<"[CTRADER-STATUS] events_total="<<depth_events_total.load()<<" this_min="<<ev_min<<" symbols="<<depth_books_.size()<<"\n";
                // Always log XAUUSD event count so a stuck feed is immediately visible.
                // Also log any symbol with zero events (possible missed subscription).
                {
                    const auto xit = ev_per_sym.find("XAUUSD");
                    const uint64_t xau_cnt = (xit != ev_per_sym.end()) ? xit->second : 0;
                    std::cout<<"[CTRADER-EVTS] XAUUSD="<<xau_cnt
                             <<(xau_cnt==0?" <-- NO EVENTS -- L2 unavailable":"")<<"\n";
                }
                for (const auto& kv : ev_per_sym)
                    if (kv.first != "XAUUSD")
                        std::cout<<"[CTRADER-EVTS] "<<kv.first<<"="<<kv.second<<"\n";
                // Log subscribed books that received zero events this minute
                for (const auto& bk : depth_books_)
                    if (ev_per_sym.find(bk.first) == ev_per_sym.end())
                        std::cout<<"[CTRADER-EVTS] "<<bk.first<<"=0 (no events this minute)\n";
                ev_min=0; ev_per_sym.clear(); last_diag=now;
            }
            uint32_t pt; std::vector<uint8_t> payload;
            const int rc = read_one(ssl, pt, payload, 100);
            if (rc < 0) {
                // Connection error. If bar requests were recently sent, broker may have
                // rejected them at TCP level (sends RST after INVALID_REQUEST in some
                // BlackBull firmware versions). Mark any in-flight bar request as failed
                // so it's skipped on reconnect -- prevents the infinite reconnect loop.
                if (bar_send_idx > 0 && bar_send_idx <= bar_send_queue.size()) {
                    const auto& last_sent = bar_send_queue[bar_send_idx - 1];
                    const std::string key = last_sent.name + ":" + std::to_string(last_sent.period);
                    if (!bar_failed_reqs.count(key)) {
                        bar_failed_reqs.insert(key);
                        std::cerr << "[CTRADER] Connection dropped after bar req "
                                  << last_sent.name << " period=" << last_sent.period
                                  << " -- marking as failed, will skip on reconnect\n";
                    }
                }
                std::cerr<<"[CTRADER] Connection error\n";
                return;
            }
            if (rc == 0) continue;
            if      (pt==2155) { on_depth_event(payload); ++depth_events_total; ++ev_min;
                                  const auto fields2 = PB::parse(payload);
                                  const uint64_t sid2 = PB::get_varint(fields2, 3);
                                  const auto iit = id_to_internal_.find(sid2);
                                  if (iit != id_to_internal_.end()) ++ev_per_sym[iit->second];
                                }
            else if (pt==2138) { on_trendbars_res(payload); }   // historical bars response
            else if (pt==2217) { on_live_trendbar(payload); }   // live bar close push (ProtoOALiveTrendBar)
            else if (pt==2220) { /* subscribe trendbar req echo -- ignore */ }
            else if (pt==2221) { /* subscribe trendbar res -- no action needed */ }
            else if (pt==2142) {
                const auto ef=PB::parse(payload);
                const std::string ec=PB::get_string(ef,2);
                const std::string em=PB::get_string(ef,3);
                std::cerr<<"[CTRADER] Error: "<<ec<<" -- "<<em<<"\n";
                if (ec=="OA_AUTH_TOKEN_EXPIRED") {
                    if (!refresh_token.empty()) send_msg(ssl,PB::refresh_token_req(refresh_token));
                    return;
                }
                // INVALID_REQUEST or UNSUPPORTED_MESSAGE on a bar request.
                // UNSUPPORTED_MESSAGE = broker has no trendbar support at all -- drain the
                // entire queue immediately to stop the reconnect loop.
                // The connection itself stays alive (do NOT return here).
                const bool is_bar_error = (ec == "INVALID_REQUEST" || ec == "UNSUPPORTED_MESSAGE");
                if (is_bar_error && bar_send_idx > 0 && bar_send_idx <= bar_send_queue.size()) {
                    const auto& failed = bar_send_queue[bar_send_idx - 1];
                    bar_failed_reqs.insert(failed.name + ":" + std::to_string(failed.period));
                    std::cerr << "[CTRADER-BARS] " << ec << " for " << failed.name
                              << " period=" << failed.period << " -- skipping on future reconnects\n";
                    if (ec == "UNSUPPORTED_MESSAGE") {
                        // Full protocol unsupported -- drain all and stop repoll
                        for (size_t qi = bar_send_idx; qi < bar_send_queue.size(); ++qi)
                            bar_failed_reqs.insert(bar_send_queue[qi].name + ":" + std::to_string(bar_send_queue[qi].period));
                        bar_send_idx = bar_send_queue.size();
                        bar_repoll_disabled = true;
                        std::cerr << "[CTRADER-BARS] UNSUPPORTED_MESSAGE -- trendbar protocol disabled\n"
                                  << "[CTRADER-BARS] Gate 0d fallback activates after 5min\n";
                    }
                    if (failed.name == "XAUUSD" && failed.period == 1)
                        std::cerr << "[CTRADER-BARS] *** XAUUSD M1 rejected -- Gate 0d will block GoldFlow ***\n";
                }
                // Do NOT return -- one bad bar request must not kill the depth feed.
            }
            else if (pt==2174) { const auto rf=PB::parse(payload); const std::string na=PB::get_string(rf,2); if(!na.empty()){access_token=na;refresh_token=PB::get_string(rf,3);std::cout<<"[CTRADER] Token refreshed\n";} }
            else if (pt==51)   { send_msg(ssl,PB::heartbeat()); }
            else if (pt==2148||pt==2164) { std::cerr<<"[CTRADER] Disconnect pt="<<pt<<"\n"; return; }
        }
    }

    // ?? ProtoOAGetTrendbarsRes (pt=2138) -- historical bars response ???????????
    // field 2: ctidTraderAccountId
    // field 3: symbolId
    // field 5: period (uint32: 1=M1, 5=M5, etc.)
    // field 4: repeated ProtoOATrendbar bytes
    void on_trendbars_res(const std::vector<uint8_t>& payload) {
        const auto f = PB::parse(payload);
        const uint64_t sym_id = PB::get_varint(f, 3);
        const uint32_t period = uint32_t(PB::get_varint(f, 5));
        const auto it = id_to_internal_.find(sym_id);
        if (it == id_to_internal_.end()) return;
        const std::string& name = it->second;
        const auto bit = bar_subscriptions.find(name);
        if (bit == bar_subscriptions.end() || !bit->second.state) return;
        SymBarState* state = bit->second.state;

        // Parse repeated trendbar messages (field 4)
        const auto bar_fields = PB::get_repeated_bytes(f, 4);
        if (bar_fields.empty()) return;

        std::vector<OHLCBar> bars;
        bars.reserve(bar_fields.size());
        for (const auto& bf : bar_fields) {
            const OHLCBar bar = PB::parse_trendbar(bf);
            if (bar.close > 0 && bar.ts_min > 0) bars.push_back(bar);
        }
        // Sort chronologically (oldest first)
        std::sort(bars.begin(), bars.end(),
                  [](const OHLCBar& a, const OHLCBar& b){ return a.ts_min < b.ts_min; });

        if (period == 1) {
            state->m1.seed(bars);
            std::cout << "[CTRADER-BARS] " << name << " M1: seeded " << bars.size()
                      << " bars, ATR=" << std::fixed << std::setprecision(2)
                      << state->m1.ind.atr14.load()
                      << " RSI=" << std::setprecision(1) << state->m1.ind.rsi14.load()
                      << " trend=" << state->m5.ind.trend_state.load() << "\n";
        } else if (period == 5) {
            state->m5.seed(bars);
            std::cout << "[CTRADER-BARS] " << name << " M5: seeded " << bars.size()
                      << " bars, trend=" << state->m5.ind.trend_state.load()
                      << " swing_hi=" << std::setprecision(2) << state->m5.ind.swing_high.load()
                      << " swing_lo=" << state->m5.ind.swing_low.load() << "\n";
        } else if (period == 7) {
            state->m15.seed(bars);
            std::cout << "[CTRADER-BARS] " << name << " M15: seeded " << bars.size()
                      << " bars, trend=" << state->m15.ind.trend_state.load()
                      << " EMA9=" << std::setprecision(2) << state->m15.ind.ema9.load()
                      << " EMA21=" << state->m15.ind.ema21.load()
                      << " EMA50=" << state->m15.ind.ema50.load()
                      << " ATR=" << state->m15.ind.atr14.load() << "\n";
        }
        if (on_bar_fn && !bars.empty()) {
            on_bar_fn(name, int(period), bars.back());
        }
    }

    // ?? ProtoOASpotEvent with live trendbar (pt=2220 push) ????????????????????
    // ProtoOALiveTrendBar response -- same structure as trendbar but sent live
    // field 2: ctidTraderAccountId
    // field 3: symbolId
    // field 4: ProtoOATrendbar (bytes, same as historical)
    // field 5: period
    void on_live_trendbar(const std::vector<uint8_t>& payload) {
        const auto f = PB::parse(payload);
        const uint64_t sym_id = PB::get_varint(f, 3);
        const uint32_t period = uint32_t(PB::get_varint(f, 5));
        const auto it = id_to_internal_.find(sym_id);
        if (it == id_to_internal_.end()) return;
        const std::string& name = it->second;
        const auto bit = bar_subscriptions.find(name);
        if (bit == bar_subscriptions.end() || !bit->second.state) return;
        SymBarState* state = bit->second.state;

        // Parse the single trendbar (field 4)
        for (const auto& fi : f) {
            if (fi.field_num == 4 && fi.wire_type == 2) {
                const OHLCBar bar = PB::parse_trendbar(fi.bytes);
                if (bar.close <= 0) continue;
                if (period == 1) {
                    state->m1.add_bar(bar);
                    printf("[CTRADER-BARS] %s M1 bar close=%.2f RSI=%.1f ATR=%.2f BB_PCT=%.2f "
                           "EMA9=%.2f trend=%d\n",
                           name.c_str(), bar.close,
                           state->m1.ind.rsi14.load(), state->m1.ind.atr14.load(),
                           state->m1.ind.bb_pct.load(), state->m1.ind.ema9.load(),
                           state->m5.ind.trend_state.load());
                } else if (period == 5) {
                    state->m5.add_bar(bar);
                    printf("[CTRADER-BARS] %s M5 bar trend=%d swing_hi=%.2f swing_lo=%.2f\n",
                           name.c_str(), state->m5.ind.trend_state.load(),
                           state->m5.ind.swing_high.load(), state->m5.ind.swing_low.load());
                } else if (period == 7) {
                    state->m15.add_bar(bar);
                    printf("[CTRADER-BARS] %s M15 bar close=%.2f trend=%d EMA9=%.2f EMA21=%.2f EMA50=%.2f ATR=%.2f\n",
                           name.c_str(), bar.close,
                           state->m15.ind.trend_state.load(),
                           state->m15.ind.ema9.load(), state->m15.ind.ema21.load(),
                           state->m15.ind.ema50.load(), state->m15.ind.atr14.load());
                }
                if (on_bar_fn) on_bar_fn(name, int(period), bar);
                break;
            }
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
        // DIAGNOSTIC: log first 10 depth events per symbol -- full raw field dump
        static std::unordered_map<std::string,int> s_diag_count;
        if (s_diag_count[name] < 10) {
            ++s_diag_count[name];
            const auto& qbs = PB::get_repeated_bytes(fields, 4);
            printf("[CTRADER-BOOK] %s id=%llu levels=%zu\n",
                   name.c_str(), (unsigned long long)sym_id, qbs.size());
            for (size_t qi = 0; qi < qbs.size() && qi < 3; ++qi) {
                const auto& qb = qbs[qi];
                const auto qf = PB::parse(qb);
                // Dump ALL parsed fields so we can see exactly what broker sends
                printf("  quote[%zu] raw_bytes=%zu fields=%zu:\n",
                       qi, qb.size(), qf.size());
                for (const auto& fld : qf) {
                    printf("    field_%d wire%d = %llu\n",
                           fld.field_num, fld.wire_type,
                           (unsigned long long)fld.varint);
                }
                const uint64_t qid=PB::get_varint(qf,1);
                const uint64_t sz =PB::get_varint(qf,3);
                const uint64_t bid=PB::get_varint(qf,4);
                const uint64_t ask=PB::get_varint(qf,5);
                printf("  -> id=%llu sz=%llu bid=%llu ask=%llu price=%.5f\n",
                       (unsigned long long)qid,
                       (unsigned long long)sz,
                       (unsigned long long)bid,
                       (unsigned long long)ask,
                       (bid?bid:ask)/100000.0);
            }
            // Also dump raw hex of first quote bytes for verification
            if (!qbs.empty() && qbs[0].size() <= 32) {
                printf("  raw_hex:");
                for (uint8_t b : qbs[0]) printf(" %02x", b);
                printf("\n");
            }
            fflush(stdout);
        }
        auto& book = depth_books_[name];
        for (const auto& qb : PB::get_repeated_bytes(fields, 4)) {
            const auto qf = PB::parse(qb);
            const uint64_t id=PB::get_varint(qf,1), sz=PB::get_varint(qf,3);
            const uint64_t bid=PB::get_varint(qf,4), ask=PB::get_varint(qf,5);
            if (!id) continue;  // id=0 is invalid
            // cTrader sends real sizes for all symbols including XAUUSD.
            // sz is in cents (sz=200 = 2 lots). Default to 100 (1 lot) if
            // sz=0 is received -- should not happen on cTrader but guards against it.
            const uint64_t eff_sz = (sz > 0) ? sz : 100;
            if (bid)      book.apply_new(id, bid, eff_sz, true);
            else if (ask) book.apply_new(id, ask, eff_sz, false);
        }
        for (uint64_t did : PB::get_packed_varints(fields, 5)) book.apply_del(did);
        // Build L2Book snapshot outside the lock -- to_l2book() is O(N log N) sort
        const L2Book rebuilt = book.to_l2book();

        // Hot path: write atomic derived scalars -- zero lock, zero contention with FIX tick
        if (atomic_l2_write_fn) {
            atomic_l2_write_fn(name,
                rebuilt.imbalance(),
                rebuilt.microprice_bias(),
                rebuilt.has_data());
        }

        // Cold path: write full book under mutex for GUI depth panel (walls, vacuums, slopes)
        // MUST happen BEFORE on_tick_fn so trading decisions see the freshly updated book.
        if (l2_mtx && l2_books) {
            std::lock_guard<std::mutex> lk(*l2_mtx);
            (*l2_books)[name] = rebuilt;
        }

        // ?? Primary price tick -- drives on_tick() with cTrader real-time price ?
        // Called AFTER l2_books update so cold_snap inside on_tick sees current book.
        if (on_tick_fn && rebuilt.bid_count > 0 && rebuilt.ask_count > 0) {
            const double best_bid = rebuilt.bids[0].price;
            const double best_ask = rebuilt.asks[0].price;
            if (best_bid > 0.0 && best_ask > 0.0 && best_ask > best_bid) {
                on_tick_fn(name, best_bid, best_ask);
            }
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
                // On Windows SO_RCVTIMEO expiry comes back as SSL_ERROR_SYSCALL + WSAETIMEDOUT -- treat as timeout not error
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
            if (pt==2142) { const auto ef=PB::parse(payload); std::cerr<<"[CTRADER] Error: "<<PB::get_string(ef,2)<<" -- "<<PB::get_string(ef,3)<<"\n"; return false; }
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




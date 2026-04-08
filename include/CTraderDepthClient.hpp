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

// ProtoOASubscribeSpotsReq (pt=2129) -- REQUIRED before live trendbar subscription
// field 2: ctidTraderAccountId, field 3: repeated symbolId
inline std::vector<uint8_t> subscribe_spots_req(int64_t ctid, int64_t sym_id) {
    std::vector<uint8_t> inner;
    write_field_varint(inner, 2, uint64_t(ctid));
    write_field_varint(inner, 3, uint64_t(sym_id));
    return frame_msg(2129, inner);
}

// ProtoOASubscribeLiveTrendbarReq (pt=2135) -- requires spots subscription first
// field 2: ctidTraderAccountId, field 3: symbolId, field 4: period
inline std::vector<uint8_t> subscribe_live_trendbar_req(int64_t ctid, int64_t sym_id, uint32_t period) {
    std::vector<uint8_t> inner;
    write_field_varint(inner, 2, uint64_t(ctid));
    write_field_varint(inner, 3, uint64_t(sym_id));
    write_field_varint(inner, 4, uint64_t(period));
    return frame_msg(2135, inner);
}

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

// ProtoOAGetTickDataReq (pt=2145)
// Request raw tick history -- BlackBull serves this even when GetTrendbarsReq is blocked.
// We use this to build M15/M5 bars ourselves from tick data.
// type: 1=BID, 2=ASK  -- we request BID (field 4)
// fromTimestamp/toTimestamp: milliseconds UTC (fields 5/6)
// hours_back: how many hours of ticks to fetch (200 M15 bars = 50 hours)
inline std::vector<uint8_t> get_tick_data_req(
    int64_t ctid, int64_t sym_id, int64_t from_ms, int64_t to_ms, int type = 1)
{
    std::vector<uint8_t> inner;
    write_field_varint(inner, 2, uint64_t(ctid));
    write_field_varint(inner, 3, uint64_t(sym_id));
    write_field_varint(inner, 4, uint64_t(type)); // 1=BID, 2=ASK
    write_field_varint(inner, 5, uint64_t(from_ms));
    write_field_varint(inner, 6, uint64_t(to_ms));
    return frame_msg(2145, inner);
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
// Forward declaration -- defined in globals.hpp.
// CTraderDepthClient.hpp may be included before globals.hpp in the PCH,
// so we forward-declare the atomic here to avoid C2065 undeclared identifier.
// The actual definition in globals.hpp is the single authoritative instance.
#include <atomic>
extern std::atomic<bool> g_feed_stale_xauusd;

class CTraderDepthClient {
public:
    std::string client_id, client_secret, access_token, refresh_token;
    int64_t     ctid_account_id = 0;
    std::unordered_set<std::string> symbol_whitelist;
    bool        dump_all_symbols = false;
    std::unordered_set<std::string> bar_failed_reqs;
    std::string bar_failed_path_ = "logs/ctrader_bar_failed.txt"; // path for persistence

    // Persist bar_failed_reqs across process restarts so M1/M5 that caused
    // INVALID_REQUEST on one run don't reconnect-loop on the next run.
    // Save bar_failed_reqs to disk.
    // INVARIANT: only period 0 and period 1 entries are ever written.
    // Periods 5 (M5) and 7 (M15) must NEVER be persisted -- they block the
    // M15 tick fallback request on the next restart, causing bars to never seed.
    // This function is the single write point for the file; enforcing the filter
    // here means no other code path can corrupt the file regardless of what's
    // in bar_failed_reqs in memory.
    void save_bar_failed(const std::string& path) const {
        FILE* f = fopen(path.c_str(), "w");
        if (!f) return;
        int written = 0;
        for (const auto& k : bar_failed_reqs) {
            const auto colon = k.rfind(':');
            if (colon != std::string::npos) {
                try {
                    const int period = std::stoi(k.substr(colon + 1));
                    if (period != 0 && period != 1) continue;  // NEVER persist M5/M15
                } catch (...) { continue; }
            }
            fprintf(f, "%s\n", k.c_str());
            ++written;
        }
        fclose(f);
        printf("[CTRADER-BARS] Saved %d failed req entries (periods 5/7 excluded)\n", written);
        fflush(stdout);
    }
    void load_bar_failed(const std::string& path) {
        FILE* f = fopen(path.c_str(), "r");
        if (!f) return;
        char buf[128];
        bool rewrite = false;
        while (fgets(buf, sizeof(buf), f)) {
            // Strip BOM and whitespace
            char* p = buf;
            if ((unsigned char)p[0]==0xEF&&(unsigned char)p[1]==0xBB&&(unsigned char)p[2]==0xBF) p+=3;
            size_t n = strlen(p);
            while (n > 0 && (p[n-1]=='\n'||p[n-1]=='\r'||p[n-1]==' ')) p[--n]=0;
            if (n == 0) continue;
            // SANITISE: only period=0 and period=1 should ever be persisted.
            // Periods 5 (M5) and 7 (M15) were incorrectly written by older versions
            // when GetTrendbarsReq was rejected -- they poisoned the failed list and
            // caused the M15 tick fallback to also be skipped every restart.
            // Strip them here and rewrite the file clean.
            const std::string key(p);
            const auto colon = key.rfind(':');
            if (colon != std::string::npos) {
                try {
                    const int period = std::stoi(key.substr(colon + 1));
                    if (period != 0 && period != 1) {
                        std::cout << "[CTRADER-BARS] Dropping stale failed entry '" << key
                                  << "' (period=" << period << " should not be persisted)\n";
                        rewrite = true;
                        continue;  // do NOT add to bar_failed_reqs
                    }
                } catch (...) {}
            }
            bar_failed_reqs.insert(key);
            std::cout << "[CTRADER-BARS] Loaded failed req from disk: " << key << "\n";
        }
        fclose(f);
        // Always rewrite the file on load -- guarantees it's clean even if
        // no stale entries were found. Prevents accumulation of corrupted state.
        save_bar_failed(path);
        if (rewrite) {
            std::cout << "[CTRADER-BARS] Rewrote " << path << " -- removed stale period entries\n";
        }
    }

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
    // Called when a symbol's bars are seeded from LIVE cTrader tick data (not external seeds).
    std::function<void(const std::string& /*sym*/)> on_live_bars_fn;
    // Called on every depth event for a symbol -- used to stamp per-symbol live tick time
    // so ctrader_depth_is_live() works correctly even before both book sides are present.
    std::function<void(const std::string& /*sym*/)> on_live_tick_ms_fn;

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
        // cTrader connects FIRST -- L2 data must be flowing before FIX starts trading.
        // Old 30s delay was backwards: FIX was making decisions with l2_imb=0.500
        // for 30+ seconds every restart. Brief 2s delay only to let thread init settle.
        sleep_ms(2000);
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

        // Send SubscribeSpotsReq (pt=2129) for XAUUSD BEFORE the depth subscription.
        // BlackBull requires spots subscription before it streams depth events for gold.
        // Other symbols receive depth events without a spots sub -- XAUUSD is special.
        // Without this: XAUUSD depth subscription ACKs but zero events ever arrive.
        // The spots sub in the bar queue fires 10s later -- too late for depth events.
        if (xauusd_spot_id >= 0) {
            send_msg(ssl, PB::subscribe_spots_req(ctid_account_id, xauusd_spot_id));
            std::cout << "[CTRADER] XAUUSD spots sub sent (pre-depth, enables depth events)\n";
            // Brief wait -- spots sub ACK (pt=2130) arrives quickly
            // Don't block on it -- depth sub follows immediately
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
            // GetTrendbarsReq (pt=2137) crashes BlackBull -- permanently blocked.
            // GetTickDataReq (pt=2145) -- only sent when OHLCBarEngine state files
            // are missing (cold start). When bars_gold_m15.dat exists (normal case),
            // m1_ready is already true from load_indicators() and no request is needed.
            //
            // BUG FIX: M15 tick request was gated on !skip(1) -- but "XAUUSD:1" is
            // pre-seeded into bar_failed_reqs to block GetTrendbarsReq for M1.
            // This caused skip(1)=true which ALSO blocked the M15 tick request,
            // leaving bars permanently unseeded every session. The two are unrelated:
            // M1 trendbar being blocked does NOT mean M15 tick data is unavailable.
            // Fix: gate M15 tick request on !skip(107) instead -- its own period key.
            // skip(107) is never set, so M15 request always fires on cold start.
            {
                const bool already_seeded = is_gold
                    && bkv.second.state
                    && bkv.second.state->m15.ind.m1_ready.load(std::memory_order_relaxed);
                if (is_gold && !already_seeded && !skip(107)) {
                    pending_bar_reqs.push_back({bkv.first, sid, 107, 200}); // M15 via tick (cold start only)
                    std::cout << "[CTRADER-BARS] " << bkv.first
                              << " cold start -- requesting M15 tick data (no bars_gold_m15.dat)\n";
                } else if (is_gold && already_seeded) {
                    std::cout << "[CTRADER-BARS] " << bkv.first
                              << " bars already seeded from disk -- skipping tick data request\n";
                }
            }
        }
        // Live subscriptions queued after history requests (sent in same staggered loop)
        struct LiveSub { std::string name; int64_t sid; uint32_t period; };
        std::vector<LiveSub> pending_live_subs;
        // Live trendbar subscriptions (pt=2220) cause UNSUPPORTED_MESSAGE on some brokers.
        // Skip live subs for any symbol/period that already failed a history request,
        // OR that previously caused a TCP RST (blacklisted with ":live:" sentinel key).
        for (const auto& bkv : bar_subscriptions) {
            const int64_t sid = bkv.second.sym_id;
            if (sid <= 0) continue;
            const bool is_gold = (bkv.first == "XAUUSD");
            // Check both history-failed key (sym:period) and live-crashed key (sym:live:period)
            auto skip = [&](uint32_t p) {
                return bar_failed_reqs.count(bkv.first + ":" + std::to_string(p)) > 0
                    || bar_failed_reqs.count(bkv.first + ":live:" + std::to_string(p)) > 0;
            };
            // LIVE TRENDBAR SUBS PERMANENTLY DISABLED for BlackBull.
            // BlackBull accepts the sub initially then sends INVALID_REQUEST after
            // ~8min, crashing the cTrader connection and triggering a full FIX
            // reconnect (115s gap). Confirmed across entire session (33 drops).
            // Bars are seeded from disk on warm restart -- live subs add nothing.
            // if (!skip(1)) pending_live_subs.push_back({bkv.first, sid, 1});
            // if (!skip(5)) pending_live_subs.push_back({bkv.first, sid, 5});
            // if (is_gold && !skip(7)) pending_live_subs.push_back({bkv.first, sid, 7});
            (void)is_gold; // suppress unused warning
        }
        // Merge: send all history reqs first, then live subs only for non-failed symbols
        // Spots subscription required before live trendbar subscription.
        // Send spots sub for each symbol first, then live trendbar sub.
        struct PendingSend { std::string name; int64_t sid; uint32_t period; uint32_t count; bool is_live; bool is_spots; };
        std::vector<PendingSend> bar_send_queue;
        // 1. Spots subscriptions (required first)
        for (const auto& bkv : bar_subscriptions) {
            const int64_t sid = bkv.second.sym_id;
            if (sid <= 0) continue;
            bar_send_queue.push_back({bkv.first, sid, 0, 0, false, true});
        }
        // 2. Historical bar requests
        for (const auto& r : pending_bar_reqs)  bar_send_queue.push_back({r.name, r.sid, r.period, r.count, false, false});
        // 3. Live trendbar subscriptions (pt=2135, requires spots sub)
        for (const auto& s : pending_live_subs)  bar_send_queue.push_back({s.name, s.sid, s.period, 0, true, false});
        size_t bar_send_idx = 0;
        auto   bar_send_next   = std::chrono::steady_clock::now() + std::chrono::seconds(10); // wait for depth subscription ACK before sending bar reqs
        // bar_repoll_next and bar_repoll_disabled removed -- repoll disabled (#if 0 below)

        while (running.load()) {
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now-last_hb).count()>=10) { send_msg(ssl,PB::heartbeat()); last_hb=now; }

            // Periodic repoll disabled -- broker (BlackBull) returns INVALID_REQUEST
            // for GetTrendbarsReq (pt=2137) on the repoll cycle, dropping the connection.
            // Only the initial startup request succeeds. TrendPullback EMAs are seeded
            // once at startup and remain valid for the session (EMAs move slowly).
            // Re-enable this block if the broker ever supports live trendbar push (pt=2220).
            // Periodic repoll disabled -- broker rejects GetTrendbarsReq on repoll with INVALID_REQUEST.
            // Re-enable if broker ever supports it.
#if 0
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
                    if (!failed(5)) send_msg(ssl, PB::get_trendbars_req(ctid_account_id, sid, 5, 50));
                    if (is_gold && !failed(7)) send_msg(ssl, PB::get_trendbars_req(ctid_account_id, sid, 7, 200));
                }
            }
#endif

            // ?? Staggered bar request dispatch ??
            // One message per 200ms tick until queue is drained.
            // History requests go first, then live subscriptions.
            if (bar_send_idx < bar_send_queue.size() && now >= bar_send_next) {
                const auto& req = bar_send_queue[bar_send_idx];
                if (req.is_spots) {
                    // CRITICAL: skip spots sub for XAUUSD -- it was already sent before
                    // the depth subscription. Sending it again here (10s later) cancels
                    // the broker-side depth stream for XAUUSD, producing zero depth events.
                    // All other symbols are safe to re-subscribe here.
                    if (req.name == "XAUUSD") {
                        std::cout << "[CTRADER-BARS] XAUUSD spots sub SKIPPED (already sent pre-depth)\n";
                    } else {
                    send_msg(ssl, PB::subscribe_spots_req(ctid_account_id, req.sid));
                    std::cout << "[CTRADER-BARS] " << req.name << " spots sub sent\n";
                    }
                } else if (req.is_live) {
                    send_msg(ssl, PB::subscribe_live_trendbar_req(ctid_account_id, req.sid, req.period));
                    std::cout << "[CTRADER-BARS] " << req.name << " live trendbar sub period=" << req.period << "\n";
                } else if (req.period == 105 || req.period == 107 || req.period == 1) {
                    // Tick data fallback for ALL bar history requests.
                    // GetTrendbarsReq (pt=2137) crashes the TCP connection on BlackBull with
                    // INVALID_REQUEST -- confirmed across all periods (M1/M5/M15).
                    // GetTickDataReq (pt=2145) serves the same raw price history without
                    // crashing. We build OHLC bars from ticks in on_tick_data_res().
                    // period sentinels: 105=M5, 107=M15, 1=M1 (all use tick fallback now)
                    // Window: 90 minutes = 6 M15 bars.
                    // Previous 6hr window (~72k ticks) was causing BlackBull to drop
                    // the TCP connection before responding -- confirmed by the
                    // restart-every-10-15min pattern in April 2 logs.
                    // 90min = ~54k ticks -- still may be borderline.
                    // The live FIX tick stream builds bars independently; after 15 M15
                    // bar closes (3h45m) m1_ready=true regardless of this request.
                    // This request just accelerates warmup on cold start.
                    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    const int64_t from_ms = now_ms - 90LL * 60LL * 1000LL;  // 90 min
                    last_bar_req_name_ = req.name;
                    const int display_period = (req.period > 100) ? (req.period - 100) : req.period;
                    send_msg(ssl, PB::get_tick_data_req(ctid_account_id, req.sid, from_ms, now_ms, 1));
                    std::cout << "[CTRADER-BARS] " << req.name << " tick data req (period="
                              << display_period << " via pt=2145, avoids pt=2137 crash)\n";
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

                // ?? PER-SYMBOL STARVATION WATCHDOG ?????????????????????????????????????
                // Root cause of April 5/6 frozen session: cTrader TCP connected and
                // SubscribeDepthRes ACK received, but XAUUSD depth events never flowed.
                // FIX fallback activated with stale cached price from prior session.
                // 452 identical ticks at 4677.03/4677.25 all session, vol_range=0.
                //
                // Detection: XAUUSD events == 0 this minute AND depth has been active
                // for >= XAUUSD_STARVE_GRACE_S seconds (allow time for book fill at startup).
                //
                // Escalation ladder (resets each time events resume):
                //   Level 1 (first zero-minute after grace): re-subscribe XAUUSD depth only.
                //     Sends SubscribeDepthReq for XAUUSD id alone.
                //     Sets g_feed_stale_xauusd=true to block all XAUUSD entries.
                //     If events resume next minute: clear stale, log restored, done.
                //
                //   Level 2 (second consecutive zero-minute): full TCP reconnect.
                //     broker ACK'd the re-subscribe but still sends nothing.
                //     Drop the connection -- loop() will reconnect and re-auth.
                //     This is the nuclear option and is rate-limited by the existing
                //     backoff in loop() (5s -> 10s -> 20s -> ... -> 300s max).
                //
                // Circuit breaker: track XAUUSD reconnect times. If level-2 fires 3x
                // within 5 minutes, disable escalation to prevent reconnect loops.
                // Log clearly -- operator must intervene if broker is structurally broken.
                static constexpr int64_t XAUUSD_STARVE_GRACE_S = 180; // raised 90->180s: bar queue spots skip needs time to take effect
                static int64_t xauusd_starve_since_s   = 0; // epoch sec when starvation first detected
                static int     xauusd_resub_count       = 0; // consecutive failed re-subscribe attempts
                static int64_t xauusd_reconnect_ts[3]  = {0,0,0}; // ring buffer of level-2 reconnect times
                static int     xauusd_reconnect_idx     = 0;
                static bool    xauusd_escalation_blocked = false;
                static int64_t connect_epoch_s          = 0; // when this TCP session was established

                if (connect_epoch_s == 0) {
                    connect_epoch_s = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                }
                const int64_t now_epoch_s = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                const int64_t session_age_s = now_epoch_s - connect_epoch_s;

                const auto xauusd_it = depth_books_.find("XAUUSD");
                const bool xauusd_subscribed = (xauusd_it != depth_books_.end());
                const uint64_t xauusd_ev_this_min = [&]{
                    const auto xit = ev_per_sym.find("XAUUSD");
                    return (xit != ev_per_sym.end()) ? xit->second : 0ULL;
                }();

                if (xauusd_subscribed && session_age_s >= XAUUSD_STARVE_GRACE_S) {
                    if (xauusd_ev_this_min == 0) {
                        // Zero XAUUSD events this minute -- starvation detected or ongoing
                        if (xauusd_starve_since_s == 0) {
                            xauusd_starve_since_s = now_epoch_s;
                        }
                        const int64_t starve_secs = now_epoch_s - xauusd_starve_since_s;

                        // Always set feed stale immediately on first zero-event minute
                        // (after grace period). This blocks entries and forces [FEED-STALE]
                        // log so the condition is unmissable in monitoring.
                        if (!g_feed_stale_xauusd.load(std::memory_order_relaxed)) {
                            g_feed_stale_xauusd.store(true, std::memory_order_relaxed);
                            printf("[FEED-STALE] XAUUSD depth events=0 this minute after %llds uptime "                                   "-- entries BLOCKED, escalating\n",
                                   (long long)session_age_s);
                            fflush(stdout);
                        }

                        if (!xauusd_escalation_blocked) {
                            if (xauusd_resub_count == 0) {
                                // LEVEL 1: re-subscribe XAUUSD spots then depth
                                // MUST send spots sub first -- same as startup sequence.
                                int64_t xauusd_id = -1;
                                for (const auto& idm : id_to_internal_) {
                                    if (idm.second == "XAUUSD") { xauusd_id = (int64_t)idm.first; break; }
                                }
                                if (xauusd_id > 0) {
                                    printf("[FEED-STALE] LEVEL-1: re-subscribing XAUUSD spots+depth (id=%lld) starve=%llds\n",
                                           (long long)xauusd_id, (long long)starve_secs);
                                    fflush(stdout);
                                    send_msg(ssl, PB::subscribe_spots_req(ctid_account_id, xauusd_id));
                                    send_msg(ssl, PB::subscribe_depth_req(ctid_account_id, {xauusd_id}));
                                    ++xauusd_resub_count;
                                } else {
                                    printf("[FEED-STALE] LEVEL-1 FAIL: XAUUSD id not found in symbol map -- escalating to level 2\n");
                                    fflush(stdout);
                                    ++xauusd_resub_count; // skip to level 2 next minute
                                }
                            } else {
                                // LEVEL 2: re-subscribe did not recover events -- full TCP reconnect
                                // Circuit breaker: max 3 reconnects within 5 minutes
                                xauusd_reconnect_ts[xauusd_reconnect_idx % 3] = now_epoch_s;
                                ++xauusd_reconnect_idx;
                                const int64_t oldest_reconnect = xauusd_reconnect_ts[(xauusd_reconnect_idx) % 3];
                                const bool loop_detected = (xauusd_reconnect_idx >= 3)
                                    && (now_epoch_s - oldest_reconnect < 300);
                                if (loop_detected) {
                                    xauusd_escalation_blocked = true;
                                    printf("[FEED-STALE] ESCALATION BLOCKED: 3 reconnects within 5min "                                           "-- broker feed structurally broken. "                                           "Manual intervention required. Entries remain BLOCKED.\n");
                                    fflush(stdout);
                                } else {
                                    printf("[FEED-STALE] LEVEL-2: full TCP reconnect -- XAUUSD depth "                                           "subscribed but no events after %llds resub attempt. "                                           "Dropping connection.\n",
                                           (long long)starve_secs);
                                    fflush(stdout);
                                    // Returning from recv_loop drops this TCP connection.
                                    // loop() will reconnect with backoff, re-auth, re-subscribe.
                                    // Reset local starvation state -- new session restarts from scratch.
                                    xauusd_resub_count    = 0;
                                    xauusd_starve_since_s = 0;
                                    connect_epoch_s       = 0;
                                    // DO NOT clear g_feed_stale_xauusd here --
                                    // it must remain set until events actually resume.
                                    ev_min=0; ev_per_sym.clear(); last_diag=now;
                                    return; // drops TCP, triggers reconnect in loop()
                                }
                            }
                        }
                    } else {
                        // XAUUSD events flowing normally this minute -- always clear stale gate
                        if (g_feed_stale_xauusd.load(std::memory_order_relaxed)) {
                            // Was blocked -- now recovered
                            const int64_t starve_secs = (xauusd_starve_since_s > 0) ? (now_epoch_s - xauusd_starve_since_s) : 0;
                            printf("[FEED-STALE] XAUUSD depth RESTORED -- events=%llu this minute, starvation=%llds. GoldFlow entries UNBLOCKED.\n",
                                   (unsigned long long)xauusd_ev_this_min, (long long)starve_secs);
                            fflush(stdout);
                        } else if (xauusd_starve_since_s > 0) {
                            const int64_t starve_secs = now_epoch_s - xauusd_starve_since_s;
                            printf("[FEED-STALE] XAUUSD depth restored after %llds starvation -- events=%llu\n",
                                   (long long)starve_secs, (unsigned long long)xauusd_ev_this_min);
                            fflush(stdout);
                        }
                        // Clear all starvation state
                        g_feed_stale_xauusd.store(false, std::memory_order_relaxed);
                        xauusd_starve_since_s    = 0;
                        xauusd_resub_count       = 0;
                        xauusd_escalation_blocked = false;
                    }
                }

                ev_min=0; ev_per_sym.clear(); last_diag=now;
            }
            uint32_t pt; std::vector<uint8_t> payload;
            const int rc = read_one(ssl, pt, payload, 100);
            if (rc < 0) {
                // TCP connection dropped.
                // Two known causes:
                //   1. GetTrendbarsReq (pt=2137) -- BlackBull sends TCP RST. Now routed
                //      through GetTickDataReq (pt=2145) so this should no longer fire.
                //   2. subscribe_live_trendbar_req (pt=2135) -- confirmed cause of the
                //      15s crash loop in logs. BlackBull rejects the live sub with a TCP RST
                //      instead of an application-layer INVALID_REQUEST error response.
                //      The old guard only blacklisted period=0/1, so period=5 live subs
                //      were never blacklisted and re-sent on every reconnect -- crash loop.
                // FIX: blacklist ANY live sub that triggers a TCP drop, not just period 0/1.
                //      History requests (not is_live) keep their period=0/1 guard to avoid
                //      blacklisting M5/M15 tick data requests which are valid and must fire.
                if (bar_send_idx > 0 && bar_send_idx <= bar_send_queue.size()) {
                    const auto& last_sent = bar_send_queue[bar_send_idx - 1];
                    if (last_sent.is_live) {
                        // Live trendbar sub caused TCP RST -- blacklist this symbol:period
                        // for the session so it is never re-sent on reconnect.
                        // Use period=9999 as a sentinel so save_bar_failed() (which only
                        // persists period=0/1) never writes it to disk -- it is session-only.
                        const std::string key = last_sent.name + ":live:" + std::to_string(last_sent.period);
                        if (!bar_failed_reqs.count(key)) {
                            bar_failed_reqs.insert(key);
                            std::cerr << "[CTRADER] TCP drop after LIVE trendbar sub "
                                      << last_sent.name << " period=" << last_sent.period
                                      << " -- live sub blacklisted for session (not persisted)\n";
                        }
                        // Also drain all remaining live subs from the send queue -- if one
                        // live sub crashes, the broker is not accepting any of them.
                        for (size_t qi = bar_send_idx; qi < bar_send_queue.size(); ++qi) {
                            if (bar_send_queue[qi].is_live) {
                                const std::string qk = bar_send_queue[qi].name + ":live:"
                                                      + std::to_string(bar_send_queue[qi].period);
                                bar_failed_reqs.insert(qk);
                            }
                        }
                        std::cerr << "[CTRADER] All live trendbar subs disabled for session"
                                     " -- depth feed continues, bar history only\n";
                    } else if (last_sent.period == 0 || last_sent.period == 1) {
                        // History request (not live) with period=0/1 -- original guard
                        const std::string key = last_sent.name + ":" + std::to_string(last_sent.period);
                        if (!bar_failed_reqs.count(key)) {
                            bar_failed_reqs.insert(key);
                            save_bar_failed(bar_failed_path_);
                            std::cerr << "[CTRADER] TCP drop after bar req " << last_sent.name
                                      << " period=" << last_sent.period << " -- marked failed, skipping on reconnect\n";
                        }
                    } else if (last_sent.period == 105 || last_sent.period == 107) {
                        // Tick data request (M5/M15) dropped the connection.
                        // Mark as attempted so we don't retry and cause another drop.
                        // Live FIX tick stream builds bars independently over ~4hr.
                        // NOT persisted to disk -- only session-level suppression.
                        const std::string key = last_sent.name + ":" + std::to_string(last_sent.period);
                        if (!bar_failed_reqs.count(key)) {
                            bar_failed_reqs.insert(key);
                            std::cerr << "[CTRADER] TCP drop after tick data req " << last_sent.name
                                      << " period=" << last_sent.period
                                      << " -- suppressing retry this session. Live FIX ticks will warm bars in ~4hr.\n";
                        }
                    }
                }
                std::cerr << "[CTRADER] Connection error -- reconnecting\n";
                return;
            }
            if (rc == 0) continue;
            if      (pt==2130) { std::cout << "[CTRADER-BARS] Spots sub ACK\n"; } // ProtoOASubscribeSpotsRes
            else if (pt==2131) { on_spot_event(payload); }  // ProtoOASpotEvent -- may contain trendbar field 6
            else if (pt==2136) { std::cout << "[CTRADER-BARS] Live trendbar sub ACK\n"; } // ProtoOASubscribeLiveTrendbarRes
            else if (pt==2155) { on_depth_event(payload); ++depth_events_total; ++ev_min;
                                  const auto fields2 = PB::parse(payload);
                                  const uint64_t sid2 = PB::get_varint(fields2, 3);
                                  const auto iit = id_to_internal_.find(sid2);
                                  if (iit != id_to_internal_.end()) ++ev_per_sym[iit->second];
                                }
            else if (pt==2138) { on_trendbars_res(payload); }   // historical bars response
            else if (pt==2146) { on_tick_data_res(payload); }   // tick data response -- used to build bars when trendbar blocked
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
                // KEY DISTINCTION:
                //   UNSUPPORTED_MESSAGE on a LIVE sub (pt=2220) = broker doesn't support
                //   live trendbar streaming. But GetTrendbarsReq (pt=2137) for HISTORY
                //   still works fine -- the cTrader chart itself uses this exact call.
                //   Solution: only disable LIVE subs on UNSUPPORTED_MESSAGE.
                //   History repoll (pt=2137 every 65s) continues unaffected.
                //
                //   INVALID_REQUEST = specific req malformed -- skip that symbol/period.
                const bool is_bar_error = (ec == "INVALID_REQUEST" || ec == "UNSUPPORTED_MESSAGE");
                if (is_bar_error && bar_send_idx > 0 && bar_send_idx <= bar_send_queue.size()) {
                    const auto& failed = bar_send_queue[bar_send_idx - 1];
                    // Only add to failed set for INVALID_REQUEST (malformed req) -- not UNSUPPORTED
                    if (ec == "INVALID_REQUEST") {
                        // Only insert period 0 and period 1 into bar_failed_reqs.
                        // Periods 5 (M5) and 7 (M15) must NEVER be marked as failed --
                        // they are valid tick data periods. If GetTrendbarsReq fails for
                        // them, we fall back to GetTickDataReq (below), not blacklist them.
                        if (failed.period == 0 || failed.period == 1) {
                            bar_failed_reqs.insert(failed.name + ":" + std::to_string(failed.period));
                            save_bar_failed(bar_failed_path_);
                        }
                        // TICK FALLBACK: if GetTrendbarsReq was rejected for M5 or M15,
                        // try GetTickDataReq instead -- same data, different API endpoint.
                        // BlackBull serves tick history even when trendbar history is blocked.
                        if ((failed.period == 5 || failed.period == 7) && !failed.is_live) {
                            const auto bit = bar_subscriptions.find(failed.name);
                            if (bit != bar_subscriptions.end() && bit->second.sym_id > 0) {
                                const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch()).count();
                                // 50 hours back = 200 M15 bars worth of tick data
                                const int64_t from_ms = now_ms - 50LL * 3600LL * 1000LL;
                                std::cerr << "[CTRADER-BARS] GetTrendbarsReq rejected for " << failed.name
                                          << " -- falling back to GetTickDataReq (pt=2145)\n";
                                last_bar_req_name_ = failed.name;
                                send_msg(ssl, PB::get_tick_data_req(ctid_account_id, bit->second.sym_id,
                                                                     from_ms, now_ms, 1)); // 1=BID
                            }
                        }
                    }
                    std::cerr << "[CTRADER-BARS] " << ec << " for " << failed.name
                              << " period=" << failed.period << " is_live=" << failed.is_live << "\n";
                    if (ec == "UNSUPPORTED_MESSAGE") {
                        // Broker doesn't support live trendbar streaming (pt=2220).
                        // Drain only the REMAINING LIVE subs from the queue -- leave history reqs alone.
                        // History repoll (pt=2137) still works and cTrader chart proves it.
                        for (size_t qi = bar_send_idx; qi < bar_send_queue.size(); ++qi) {
                            if (bar_send_queue[qi].is_live) {
                                // Skip live subs only
                                bar_send_idx = qi + 1;
                            }
                        }
                        // Do NOT set bar_repoll_disabled -- history repoll continues every 65s
                        std::cerr << "[CTRADER-BARS] UNSUPPORTED live trendbar sub -- "
                                  << "live subs disabled but history repoll (pt=2137) continues\n";
                    }
                    if (failed.name == "XAUUSD" && failed.period == 1 && ec == "INVALID_REQUEST")
                        std::cerr << "[CTRADER-BARS] *** XAUUSD M1 rejected -- Gate 0d will block GoldFlow ***\n";
                }
                // Do NOT return -- one bad bar request must not kill the depth feed.
            }
            else if (pt==2174) { const auto rf=PB::parse(payload); const std::string na=PB::get_string(rf,2); if(!na.empty()){access_token=na;refresh_token=PB::get_string(rf,3);std::cout<<"[CTRADER] Token refreshed\n";} }
            else if (pt==51)   { send_msg(ssl,PB::heartbeat()); }
            else if (pt==2148||pt==2164) { std::cerr<<"[CTRADER] Disconnect pt="<<pt<<"\n"; return; }
        }
    }

    // Tracks the last bar request symbol name -- used by on_tick_data_res
    // to know which symbol the tick response belongs to
    std::string last_bar_req_name_;

    // ProtoOAGetTickDataRes (pt=2146)
    // Ticks are delta-encoded: first tick has absolute timestamp ms, subsequent ticks have delta ms.
    // ProtoOATickData fields: 1=timestamp (ms absolute/delta), 2=bid (scaled), 3=ask (scaled)
    // Scale: XAUUSD/XAGUSD = /1000, FX = /100000, indices = /100
    // We build M15 and M5 OHLC bars from ticks and seed bar state directly.
    void on_tick_data_res(const std::vector<uint8_t>& payload) {
        const std::string name = last_bar_req_name_;
        if (name.empty()) return;
        const auto bit = bar_subscriptions.find(name);
        if (bit == bar_subscriptions.end() || !bit->second.state) return;
        SymBarState* state = bit->second.state;

        const auto f = PB::parse(payload);
        // field 3 = repeated ProtoOATickData
        const auto tick_fields = PB::get_repeated_bytes(f, 3);
        if (tick_fields.empty()) {
            std::cout << "[CTRADER-TICKS] " << name << ": empty response\n";
            return;
        }

        // Price scale: gold/silver /1000, FX /100000, indices /100
        double scale = 1.0/100000.0;
        if (name == "XAUUSD" || name == "XAGUSD") scale = 1.0/1000.0;
        else if (name == "US500.F" || name == "USTEC.F" || name == "DJ30.F" ||
                 name == "NAS100"  || name == "GER40"   || name == "UK100" ||
                 name == "ESTX50"  || name == "USOIL.F" || name == "BRENT") scale = 1.0/100.0;

        // Delta-decode and build bars
        std::vector<OHLCBar> m15_bars, m5_bars;
        int64_t cur_ts_ms = 0;
        OHLCBar cur15{}, cur5{};
        bool in15 = false, in5 = false;
        const int64_t M15_MS = 15LL * 60LL * 1000LL;
        const int64_t M5_MS  =  5LL * 60LL * 1000LL;

        for (const auto& tf : tick_fields) {
            const auto td = PB::parse(tf);
            const int64_t ts_delta  = (int64_t)PB::get_varint(td, 1);
            const uint64_t bid_raw  = PB::get_varint(td, 2);
            if (bid_raw == 0) continue;
            cur_ts_ms += ts_delta;
            const double mid = (double)bid_raw * scale;
            if (mid < 0.0001 || mid > 1000000.0) continue;

            // M15 aggregation
            const int64_t b15 = (cur_ts_ms / M15_MS) * M15_MS;
            if (!in15 || b15 != cur15.ts_min * 60000LL) {
                if (in15) m15_bars.push_back(cur15);
                cur15.ts_min = b15 / 60000LL;
                cur15.open = cur15.high = cur15.low = cur15.close = mid;
                in15 = true;
            } else {
                if (mid > cur15.high) cur15.high = mid;
                if (mid < cur15.low)  cur15.low  = mid;
                cur15.close = mid;
            }
            // M5 aggregation
            const int64_t b5 = (cur_ts_ms / M5_MS) * M5_MS;
            if (!in5 || b5 != cur5.ts_min * 60000LL) {
                if (in5) m5_bars.push_back(cur5);
                cur5.ts_min = b5 / 60000LL;
                cur5.open = cur5.high = cur5.low = cur5.close = mid;
                in5 = true;
            } else {
                if (mid > cur5.high) cur5.high = mid;
                if (mid < cur5.low)  cur5.low  = mid;
                cur5.close = mid;
            }
        }
        if (in15) m15_bars.push_back(cur15);
        if (in5)  m5_bars.push_back(cur5);

        // Sort chronologically (should already be in order)
        auto cmp = [](const OHLCBar& a, const OHLCBar& b){ return a.ts_min < b.ts_min; };
        std::sort(m15_bars.begin(), m15_bars.end(), cmp);
        std::sort(m5_bars.begin(),  m5_bars.end(),  cmp);

        if (!m5_bars.empty()) {
            state->m5.seed(m5_bars);
            std::cout << "[CTRADER-BARS] " << name << " M5 (ticks): seeded " << m5_bars.size()
                      << " bars, trend=" << state->m5.ind.trend_state.load() << "\n";
        }
        if (!m15_bars.empty()) {
            state->m15.seed(m15_bars);
            std::cout << "[CTRADER-BARS] " << name << " M15 (ticks): seeded " << m15_bars.size()
                      << " bars, EMA9=" << std::fixed << std::setprecision(2) << state->m15.ind.ema9.load()
                      << " EMA50=" << state->m15.ind.ema50.load()
                      << " ATR=" << state->m15.ind.atr14.load() << "\n";
            // Signal that TrendPullback can now use live-aligned bars
            if (on_live_bars_fn) on_live_bars_fn(name);
        }
        if (on_bar_fn && (!m15_bars.empty() || !m5_bars.empty())) {
            const auto& last = m15_bars.empty() ? m5_bars.back() : m15_bars.back();
            on_bar_fn(name, m15_bars.empty() ? 5 : 7, last);
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

    // ?? ProtoOASpotEvent (pt=2131) -- contains trendbar in field 6 after live sub ????
    // field 2: ctidTraderAccountId, field 3: symbolId, field 4: bid, field 5: ask
    // field 6: repeated ProtoOATrendbar (only present after SubscribeLiveTrendbarReq)
    void on_spot_event(const std::vector<uint8_t>& payload) {
        const auto f = PB::parse(payload);
        const uint64_t sym_id = PB::get_varint(f, 3);
        const auto it = id_to_internal_.find(sym_id);
        if (it == id_to_internal_.end()) return;
        const std::string& name = it->second;
        const auto bit = bar_subscriptions.find(name);
        if (bit == bar_subscriptions.end() || !bit->second.state) return;
        SymBarState* state = bit->second.state;

        // Extract trendbar from field 6 (repeated ProtoOATrendbar)
        for (const auto& fi : f) {
            if (fi.field_num == 6 && fi.wire_type == 2) {
                // ProtoOATrendbar: field 2=period, field 3=low, field 4=deltaOpen,
                //                  field 5=deltaClose, field 6=deltaHigh, field 7=volume,
                //                  field 8=utcTimestampInMinutes
                const auto tb = PB::parse(fi.bytes);
                uint32_t period = uint32_t(PB::get_varint(tb, 2));
                const OHLCBar bar = PB::parse_trendbar(fi.bytes);
                if (bar.close <= 0) continue;
                if (period == 1) {
                    state->m1.add_bar(bar);
                    printf("[CTRADER-BARS] %s M1 spot-bar close=%.2f RSI=%.1f ATR=%.2f\n",
                           name.c_str(), bar.close,
                           state->m1.ind.rsi14.load(), state->m1.ind.atr14.load());
                } else if (period == 5) {
                    state->m5.add_bar(bar);
                } else if (period == 7) {
                    state->m15.add_bar(bar);
                }
            }
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
            // Log the computed imbalance_level after this event -- proves the fix is working.
            // After all quotes are applied below, rebuild and log. Use a separate block
            // so we log the LIVE book state including this event, not just these 3 quotes.
            // (Logged as [CTRADER-L2-CHECK] so VERIFY_STARTUP and MONITOR can find it)
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

        // Log imbalance_level on first 20 events per symbol -- confirms fix is working.
        // [CTRADER-L2-CHECK] is grep-able from VERIFY_STARTUP and MONITOR.
        {
            static std::unordered_map<std::string,int> s_imb_log_count;
            if (s_imb_log_count[name] < 20) {
                ++s_imb_log_count[name];
                printf("[CTRADER-L2-CHECK] %s event=%d bid_lvls=%d ask_lvls=%d "
                       "imb_level=%.3f imb_vol=%.3f sz_sample=%llu\n",
                       name.c_str(), s_imb_log_count[name],
                       rebuilt.bid_count, rebuilt.ask_count,
                       rebuilt.imbalance_level(),
                       rebuilt.imbalance(),
                       (unsigned long long)(depth_events_total.load()));
                fflush(stdout);
            }
        }

        // Hot path: write atomic derived scalars -- zero lock, zero contention with FIX tick
        //
        // IMBALANCE SIGNAL SELECTION:
        // BlackBull Markets sends size_raw=0 on all XAUUSD depth quotes. The parse
        // path substitutes eff_sz=100 (1 lot) per level so the book is never empty,
        // but this makes bid_vol == ask_vol == N×1.0 whenever bid_count == ask_count,
        // producing imbalance()=0.500 permanently. GoldFlowEngine thresholds are
        // 0.75 (long) and 0.25 (short) -- 0.500 never satisfies either, silencing
        // the entire engine for the whole session.
        //
        // FIX: use imbalance_level() = bid_count/(bid_count+ask_count).
        // This counts how many price levels are active on each side regardless of size.
        // When the DOM has 4 bid levels and 1 ask level, the market is bid-heavy (0.80).
        // When the DOM has 1 bid and 4 ask, it is ask-heavy (0.20).
        // This is the only reliable directional DOM signal available from BlackBull.
        //
        // imbalance() (volume-based) is preserved for brokers that send real sizes.
        // imbalance_level() is used here because it works correctly for ALL brokers.
        if (atomic_l2_write_fn) {
            atomic_l2_write_fn(name,
                rebuilt.imbalance_level(),
                rebuilt.microprice_bias(),
                rebuilt.has_data());
        }

        // Cold path: write full book under mutex for GUI depth panel (walls, vacuums, slopes)
        // MUST happen BEFORE on_tick_fn so trading decisions see the freshly updated book.
        if (l2_mtx && l2_books) {
            std::lock_guard<std::mutex> lk(*l2_mtx);
            (*l2_books)[name] = rebuilt;
        }

        // Stamp per-symbol depth event time BEFORE on_tick_fn check.
        // ctrader_depth_is_live() uses this to decide FIX vs cTrader price.
        // Previously only stamped inside on_tick_fn (requires both sides) --
        // that caused ctrader_depth_is_live=false during incremental book build,
        // triggering spurious L2-DEAD restarts that cleared the book before it filled.
        if (on_live_tick_ms_fn) on_live_tick_ms_fn(name);

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




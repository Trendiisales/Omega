#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PumpFeedConsumer — TCP client thread that streams the pump feed from
// pump/pump_feed_bridge.py (server mode) into the in-process PumpScalpManager.
//
// Mirrors IbkrDomConsumer: dedicated thread, blocking recv, reconnect every 2s,
// never blocks the trading loop. Default OFF — started only when the env
// OMEGA_PUMP_BRIDGE=1 is set at launch (see omega_main.hpp). When off, the live
// service is byte-for-byte unchanged.
//
// Feed protocol (newline-delimited, from the bridge):
//   S,SYM,TF,o,h,l,c,v,ts_ms   seed bar (warm only, no entry)
//   B,SYM,TF,o,h,l,c,v,ts_ms   live closed bar (may enter)
//   P,SYM,px,ts_ms             price tick (drives the fast trailing exit)
//   C,SYM,px,day_open,up,ts_ms scanner candidate (for the GUI scanner panel)
//   R,SYM                      reset-for-reseed (precedes a seed replay batch)
//
// The manager registers its positions with g_open_positions, so pump trades show
// in the live_trades GUI panel + ring the entry bell exactly like every engine.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#include "PumpScalpManager.hpp"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
#endif

namespace omega {
namespace pump_feed {

#ifdef _WIN32
  using socket_t = SOCKET;
  static constexpr socket_t BAD_SOCK = INVALID_SOCKET;
  inline void close_sock(socket_t s) noexcept { ::closesocket(s); }
  inline void wsa_once() { static bool d=false; if(!d){ WSADATA w; WSAStartup(MAKEWORD(2,2),&w); d=true; } }
#else
  using socket_t = int;
  static constexpr socket_t BAD_SOCK = -1;
  inline void close_sock(socket_t s) noexcept { ::close(s); }
  inline void wsa_once() {}
#endif

inline socket_t connect_localhost(const char* host, uint16_t port) noexcept {
    wsa_once();
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == BAD_SOCK) return BAD_SOCK;
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    ::inet_pton(AF_INET, host, &addr.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) { close_sock(s); return BAD_SOCK; }
    return s;
}

// S-2026-06-25 G3 STALE-DATA GUARD: the bridge stamps P (price) and C (candidate)
// lines with the current wall clock, so a fresh line has age ~0. If the bridge ever
// stalls or replays a buffered batch, those lines arrive with a large age -> DROP them
// so the engine never enters/exits off a stale price. Belt-and-suspenders behind the
// bridge-side freshness gate (which already suppresses stale names at the source).
// NOT applied to B (5m bar close: ts is the bucket start, legitimately up to a TF old)
// nor S (historical seed) nor R (metadata).
inline int64_t feed_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
static constexpr int64_t FEED_STALE_MS = 60000;   // drop now-stamped P/C lines older than 60s

// S-2026-07-16: optional TEE of each fresh per-name live price (P/C lines) to a second consumer.
// Used to feed the bigcap up-jump LADDER's live-confirm gate (stockmover_ladder_book().on_live_tick)
// off THIS :7784 bridge — the 45 bigcap STK names ride here, not the :9701 IBKR bridge, so the ladder
// had no live-tick source and never opened a leg. Default {} = no tee = byte-identical to the old path.
using PriceTickCb = std::function<void(const char* sym, double px, int64_t ts_ms)>;

// Parse + dispatch one feed line into the manager. Tolerant: bad lines dropped.
// mgr_b (optional, default null): A/B twin fed the SAME lines so it gets IDENTICAL
// entries (S-2026-06-24). When null, byte-identical to the single-manager path.
// live_cb (optional, default {}): teed each fresh P/C price for the ladder live-confirm gate.
inline void dispatch_line(PumpScalpManager& mgr, const char* ln, PumpScalpManager* mgr_b=nullptr,
                          const PriceTickCb& live_cb={}) {
    if (!ln[0]) return;
    char sym[64]; double o,h,l,c,v,px,dopen,up; int tf; long long ts;
    if (ln[0]=='B' || ln[0]=='S') {
        if (std::sscanf(ln+1, ",%63[^,],%d,%lf,%lf,%lf,%lf,%lf,%lld", sym,&tf,&o,&h,&l,&c,&v,&ts)==8) {
            mgr.on_bar(sym, tf, o,h,l,c,v, (int64_t)ts, ln[0]=='S');
            if (mgr_b) mgr_b->on_bar(sym, tf, o,h,l,c,v, (int64_t)ts, ln[0]=='S');
        }
    } else if (ln[0]=='P') {
        if (std::sscanf(ln+1, ",%63[^,],%lf,%lld", sym,&px,&ts)==3) {
            if (feed_now_ms() - (int64_t)ts > FEED_STALE_MS) return;   // G3: stale price -> drop
            mgr.on_price(sym, px, (int64_t)ts);
            if (mgr_b) mgr_b->on_price(sym, px, (int64_t)ts);
            if (live_cb) live_cb(sym, px, (int64_t)ts);               // tee -> ladder live-confirm gate
        }
    } else if (ln[0]=='C') {
        if (std::sscanf(ln+1, ",%63[^,],%lf,%lf,%lf,%lld", sym,&px,&dopen,&up,&ts)==5) {
            if (feed_now_ms() - (int64_t)ts > FEED_STALE_MS) return;   // G3: stale candidate -> drop
            mgr.set_candidate(sym, px, dopen, up, (int64_t)ts);
            if (mgr_b) mgr_b->set_candidate(sym, px, dopen, up, (int64_t)ts);
            if (live_cb) live_cb(sym, px, (int64_t)ts);               // tee -> ladder live-confirm gate
        }
    } else if (ln[0]=='R') {
        if (std::sscanf(ln+1, ",%63[^,\n]", sym)==1) {
            mgr.reset_symbol(sym);       // clean re-warm before a seed replay
            if (mgr_b) mgr_b->reset_symbol(sym);
        }
    }
}

// Thread body: connect -> read lines -> dispatch -> reconnect on drop.
// mgr_b (optional): A/B twin driven off the SAME feed lines (default null = unchanged).
inline void run(PumpScalpManager& mgr, std::atomic<bool>& stop,
                const char* host, uint16_t port, PumpScalpManager* mgr_b=nullptr,
                const PriceTickCb& live_cb={}) {
    std::string buf; char tmp[8192];
    while (!stop.load(std::memory_order_relaxed)) {
        socket_t s = connect_localhost(host, port);
        if (s == BAD_SOCK) { std::this_thread::sleep_for(std::chrono::seconds(2)); continue; }
        std::printf("[PUMP-CONSUMER] connected %s:%u\n", host, (unsigned)port); std::fflush(stdout);
        buf.clear();
        while (!stop.load(std::memory_order_relaxed)) {
            int got = (int)::recv(s, tmp, sizeof(tmp), 0);
            if (got <= 0) break;                       // disconnect -> reconnect
            buf.append(tmp, got);
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (!line.empty() && line[0] != '#') dispatch_line(mgr, line.c_str(), mgr_b, live_cb);
            }
            if (buf.size() > 1u<<20) buf.clear();      // runaway guard
        }
        close_sock(s);
        std::printf("[PUMP-CONSUMER] disconnected, retrying\n"); std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

}  // namespace pump_feed
}  // namespace omega

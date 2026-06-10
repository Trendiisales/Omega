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

// Parse + dispatch one feed line into the manager. Tolerant: bad lines dropped.
inline void dispatch_line(PumpScalpManager& mgr, const char* ln) {
    if (!ln[0]) return;
    char sym[64]; double o,h,l,c,v,px,dopen,up; int tf; long long ts;
    if (ln[0]=='B' || ln[0]=='S') {
        if (std::sscanf(ln+1, ",%63[^,],%d,%lf,%lf,%lf,%lf,%lf,%lld", sym,&tf,&o,&h,&l,&c,&v,&ts)==8)
            mgr.on_bar(sym, tf, o,h,l,c,v, (int64_t)ts, ln[0]=='S');
    } else if (ln[0]=='P') {
        if (std::sscanf(ln+1, ",%63[^,],%lf,%lld", sym,&px,&ts)==3)
            mgr.on_price(sym, px, (int64_t)ts);
    } else if (ln[0]=='C') {
        if (std::sscanf(ln+1, ",%63[^,],%lf,%lf,%lf,%lld", sym,&px,&dopen,&up,&ts)==5)
            mgr.set_candidate(sym, px, dopen, up, (int64_t)ts);
    }
}

// Thread body: connect -> read lines -> dispatch -> reconnect on drop.
inline void run(PumpScalpManager& mgr, std::atomic<bool>& stop,
                const char* host, uint16_t port) {
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
                if (!line.empty() && line[0] != '#') dispatch_line(mgr, line.c_str());
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

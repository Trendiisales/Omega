// =============================================================================
//  RealDomReceiver.hpp
//
//  Connects to OmegaDomStreamer cBot on localhost:8765 and parses real
//  XAUUSD DOM sizes into g_real_dom_gold (atomically).
//
//  Data format (newline-delimited JSON):
//    {"ts":...,"bids":[[price,vol],...],"asks":[[price,vol],...],"bid_levels":N,"ask_levels":N,"seq":N}
//
//  Thread: runs on a dedicated background thread, never blocks the tick path.
//  Output: g_real_dom_gold -- read by CandleFlowEngine/MomentumBreakoutEngine
//          on every tick via real_dom_gold() inline function.
//
//  Liveness: g_real_dom_stale_ms tracks ms since last update.
//  If > 5000ms, real DOM considered stale and engine falls back to level-counts.
// =============================================================================

#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <array>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <sstream>

// sock_t, BAD_SOCK, sock_close defined in CTraderDepthClient.hpp (included before this)
#ifndef _WIN32
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <unistd.h>
#endif

// ---------------------------------------------------------------------------
//  Real DOM snapshot -- up to 20 levels per side
// ---------------------------------------------------------------------------
struct RealDomLevel {
    double price      = 0.0;
    double volume     = 0.0;   // volumeInUnits (e.g. 6000 = 6 lots)
};

struct RealDomSnapshot {
    std::array<RealDomLevel, 20> bids;
    std::array<RealDomLevel, 20> asks;
    int    bid_levels = 0;
    int    ask_levels = 0;
    int64_t ts_ms     = 0;
    int64_t seq       = 0;
    bool    valid     = false;
};

// Global: written by RealDomReceiver thread, read by engine tick path
// Protected by simple spinlock (engine reads are very fast)
static std::atomic<bool>       g_real_dom_lock{false};
static RealDomSnapshot         g_real_dom_gold;
static std::atomic<int64_t>    g_real_dom_last_ms{0};
static std::atomic<int64_t>    g_real_dom_seq{0};

inline int64_t now_ms_realdom() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
//  Acquire/release spinlock for g_real_dom_gold
// ---------------------------------------------------------------------------
inline void real_dom_lock()   { bool e=false; while(!g_real_dom_lock.compare_exchange_weak(e,true)) e=false; }
inline void real_dom_unlock() { g_real_dom_lock.store(false); }

// ---------------------------------------------------------------------------
//  Read-side helper: copies snapshot under lock
// ---------------------------------------------------------------------------
inline RealDomSnapshot real_dom_snapshot() {
    real_dom_lock();
    RealDomSnapshot s = g_real_dom_gold;
    real_dom_unlock();
    return s;
}

// ---------------------------------------------------------------------------
//  Convenience: compute real imbalance from top N levels
//  Returns 0.5 if no data or stale.
// ---------------------------------------------------------------------------
inline double real_dom_imbalance(int levels = 5) {
    const int64_t MAX_STALE_MS = 5000;
    if ((now_ms_realdom() - g_real_dom_last_ms.load()) > MAX_STALE_MS) return 0.5;

    RealDomSnapshot s = real_dom_snapshot();
    if (!s.valid || s.bid_levels == 0 || s.ask_levels == 0) return 0.5;

    double bid_vol = 0.0, ask_vol = 0.0;
    int n = std::min(levels, std::min(s.bid_levels, s.ask_levels));
    for (int i = 0; i < n; i++) {
        bid_vol += s.bids[i].volume;
        ask_vol += s.asks[i].volume;
    }
    double total = bid_vol + ask_vol;
    if (total <= 0.0) return 0.5;
    return bid_vol / total;
}

// ---------------------------------------------------------------------------
//  JSON parser -- minimal, no dependencies
//  Parses: {"ts":N,"bids":[[p,v],...],"asks":[[p,v],...],...}
// ---------------------------------------------------------------------------
static bool parse_dom_json(const std::string& line, RealDomSnapshot& out) {
    out = RealDomSnapshot{};

    // Extract ts
    auto find_val = [&](const std::string& key) -> int64_t {
        auto pos = line.find("\"" + key + "\":");
        if (pos == std::string::npos) return 0;
        pos += key.size() + 3;
        return std::stoll(line.substr(pos));
    };

    try {
        out.ts_ms = find_val("ts");
        out.seq   = find_val("seq");

        // Parse bids array
        auto parse_levels = [&](const std::string& key, std::array<RealDomLevel,20>& levels, int& count) {
            count = 0;
            auto pos = line.find("\"" + key + "\":[");
            if (pos == std::string::npos) return;
            pos += key.size() + 4;
            while (count < 20 && pos < line.size()) {
                auto lb = line.find('[', pos);
                if (lb == std::string::npos) break;
                auto cm = line.find(',', lb+1);
                auto rb = line.find(']', cm+1);
                if (cm == std::string::npos || rb == std::string::npos) break;
                levels[count].price  = std::stod(line.substr(lb+1, cm-lb-1));
                levels[count].volume = std::stod(line.substr(cm+1, rb-cm-1));
                count++;
                pos = rb + 1;
                if (pos < line.size() && line[pos] == ',') pos++;
                else break;
            }
        };

        parse_levels("bids", out.bids, out.bid_levels);
        parse_levels("asks", out.asks, out.ask_levels);
        out.valid = (out.bid_levels > 0 || out.ask_levels > 0);
        return out.valid;
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------
//  RealDomReceiver -- background thread connecting to cBot
// ---------------------------------------------------------------------------
class RealDomReceiver {
public:
    explicit RealDomReceiver(int port = 8765) : m_port(port) {}

    void start() {
        m_thread = std::thread(&RealDomReceiver::run_loop, this);
        m_thread.detach();
    }

private:
    int         m_port;
    std::thread m_thread;

    void run_loop() {
        printf("[REAL-DOM] Receiver started -- connecting to cBot on localhost:%d\n", m_port);
        fflush(stdout);

        while (true) {
            sock_t sock = connect_to_cbot();
            if (sock == BAD_SOCK) {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                continue;
            }

            printf("[REAL-DOM] Connected to cBot DOM streamer\n");
            fflush(stdout);

            read_loop(sock);
            sock_close(sock);

            printf("[REAL-DOM] Disconnected -- reconnecting in 3s\n");
            fflush(stdout);
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }

    sock_t connect_to_cbot() {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2,2), &wsa);
#endif
        sock_t s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == BAD_SOCK) return BAD_SOCK;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons((uint16_t)m_port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
            sock_close(s);
            return BAD_SOCK;
        }
        return s;
    }

    void read_loop(sock_t s) {
        char buf[65536];
        std::string partial;
        int64_t last_log_ms = 0;
        int64_t updates = 0;

        while (true) {
            int n = recv(s, buf, sizeof(buf)-1, 0);
            if (n <= 0) break;
            buf[n] = '\0';
            partial += buf;

            // Split on newlines
            size_t pos = 0;
            while (true) {
                auto nl = partial.find('\n', pos);
                if (nl == std::string::npos) break;

                std::string line = partial.substr(pos, nl - pos);
                pos = nl + 1;

                if (line.empty()) continue;

                RealDomSnapshot snap;
                if (parse_dom_json(line, snap)) {
                    real_dom_lock();
                    g_real_dom_gold = snap;
                    real_dom_unlock();
                    g_real_dom_last_ms.store(now_ms_realdom());
                    g_real_dom_seq.store(snap.seq);
                    updates++;

                    // Log every 10s
                    auto now = now_ms_realdom();
                    if (now - last_log_ms > 10000) {
                        last_log_ms = now;
                        double imb = real_dom_imbalance(5);
                        printf("[REAL-DOM] seq=%lld bids=%d asks=%d best_bid=%.2f/%.0f best_ask=%.2f/%.0f imb=%.3f\n",
                            (long long)snap.seq,
                            snap.bid_levels, snap.ask_levels,
                            snap.bid_levels>0 ? snap.bids[0].price : 0.0,
                            snap.bid_levels>0 ? snap.bids[0].volume : 0.0,
                            snap.ask_levels>0 ? snap.asks[0].price : 0.0,
                            snap.ask_levels>0 ? snap.asks[0].volume : 0.0,
                            imb);
                        fflush(stdout);
                    }
                }
            }
            if (pos > 0) partial = partial.substr(pos);
            if (partial.size() > 65536) partial.clear(); // safety
        }
    }
};

// Global instance -- call g_real_dom_receiver.start() from omega_main
static RealDomReceiver g_real_dom_receiver(8765);

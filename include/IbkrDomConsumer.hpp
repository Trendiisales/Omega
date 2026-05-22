#pragma once
// IbkrDomConsumer.hpp -- TCP client thread that reads newline-delimited JSON
// from tools/ibkr_dom_bridge.py and updates g_ibkr_l2 atomics.
//
// Read-only signal source. Default OFF (gated by env OMEGA_IBKR_BRIDGE=1).
// Adds NO new entry path -- engines must opt-in by querying g_ibkr_l2.
//
// Failure modes:
//   - bridge not running       -> consumer retries connect every 2s, silent.
//   - bridge dies mid-session  -> reconnects; g_ibkr_l2.<sym>.fresh() goes false
//                                 ~2s after last message. Engines must check.
//   - malformed JSON line      -> dropped, counter incremented, no crash.
//
// Performance:
//   - cadence ~20-50 events/sec/symbol from TWS -> trivial CPU.
//   - blocking recv on dedicated thread; never touches engine state directly.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

// Cross-platform socket layer. Windows uses Winsock2; POSIX uses BSD sockets.
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
  namespace omega::ibkr_sock {
    using socket_t = SOCKET;
    static constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
    inline void close_sock(socket_t s) noexcept { ::closesocket(s); }
    inline int  last_err() noexcept { return ::WSAGetLastError(); }
    inline bool ensure_init() noexcept {
        static std::atomic<bool> done{false};
        static std::mutex m;
        if (done.load(std::memory_order_acquire)) return true;
        std::lock_guard<std::mutex> lk(m);
        if (done.load(std::memory_order_acquire)) return true;
        WSADATA w{};
        if (::WSAStartup(MAKEWORD(2, 2), &w) != 0) return false;
        done.store(true, std::memory_order_release);
        return true;
    }
  }
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  #include <cerrno>
  namespace omega::ibkr_sock {
    using socket_t = int;
    static constexpr socket_t INVALID_SOCK = -1;
    inline void close_sock(socket_t s) noexcept { ::close(s); }
    inline int  last_err() noexcept { return errno; }
    inline bool ensure_init() noexcept { return true; }
  }
#endif

namespace omega::ibkr {

struct L2Slot {
    static constexpr int MAX_LEVELS = 5;

    std::atomic<double>  imb{0.5};
    std::atomic<double>  bid_vol{0.0};
    std::atomic<double>  ask_vol{0.0};
    std::atomic<double>  bid{0.0};
    std::atomic<double>  ask{0.0};
    std::atomic<int>     bid_levels{0};
    std::atomic<int>     ask_levels{0};
    std::atomic<int64_t> last_ms{0};

    // Per-level arrays. Written under level_mtx, read under same lock.
    // Used by GUI depth panel + book_slope/wall calculations when fresh.
    mutable std::mutex level_mtx;
    double bid_prices[MAX_LEVELS]{};
    double bid_sizes [MAX_LEVELS]{};
    double ask_prices[MAX_LEVELS]{};
    double ask_sizes [MAX_LEVELS]{};
    int    bid_n = 0;
    int    ask_n = 0;

    // True if last update within max_age_ms.
    bool fresh(int64_t now_ms, int64_t max_age_ms = 2000) const noexcept {
        const int64_t lm = last_ms.load(std::memory_order_relaxed);
        return lm > 0 && (now_ms - lm) < max_age_ms;
    }

    // Copy snapshot of per-level arrays under lock. Returns counts written.
    void snapshot_levels(double bp_out[MAX_LEVELS], double bs_out[MAX_LEVELS], int& nb,
                         double ap_out[MAX_LEVELS], double as_out[MAX_LEVELS], int& na) const {
        std::lock_guard<std::mutex> lk(level_mtx);
        nb = bid_n;
        na = ask_n;
        for (int i = 0; i < nb; ++i) { bp_out[i] = bid_prices[i]; bs_out[i] = bid_sizes[i]; }
        for (int i = 0; i < na; ++i) { ap_out[i] = ask_prices[i]; as_out[i] = ask_sizes[i]; }
    }
};

// One slot per bridged symbol. Add symbols here as the bridge expands.
struct L2Bus {
    L2Slot xau;     // XAUUSD              (IBKR XAUUSD CMDTY/SMART)
    L2Slot xag;     // XAGUSD              (IBKR XAGUSD CMDTY/SMART)
    L2Slot us500;   // US500.F / US500     (IBKR ES   front-month CME)
    L2Slot nas100;  // NAS100 / USTEC.F    (IBKR NQ   front-month CME; USTEC aliases NAS100)
    L2Slot dj30;    // DJ30.F  / DJ30      (IBKR YM   front-month CBOT)
    L2Slot ger40;   // GER40               (IBKR FDAX front-month EUREX)
    L2Slot uk100;   // UK100               (IBKR Z    front-month ICEEU)
    L2Slot estx50;  // ESTX50              (IBKR FESX front-month EUREX)

    // Map bridge-emitted symbol string to the slot it updates.
    // Bridge symbols come in either "US500.F"/"NAS100"/etc (Blackbull naming
    // -- the bridge writes whatever was passed via --symbols), so we accept
    // both forms here. USTEC.F maps to nas100 (same NQ underlying).
    L2Slot* lookup(const char* sym) noexcept {
        if (std::strcmp(sym, "XAUUSD")  == 0) return &xau;
        if (std::strcmp(sym, "XAGUSD")  == 0) return &xag;
        if (std::strcmp(sym, "US500")   == 0 || std::strcmp(sym, "US500.F")  == 0) return &us500;
        if (std::strcmp(sym, "NAS100")  == 0
         || std::strcmp(sym, "USTEC")   == 0 || std::strcmp(sym, "USTEC.F")  == 0) return &nas100;
        if (std::strcmp(sym, "DJ30")    == 0 || std::strcmp(sym, "DJ30.F")   == 0) return &dj30;
        if (std::strcmp(sym, "GER40")   == 0) return &ger40;
        if (std::strcmp(sym, "UK100")   == 0) return &uk100;
        if (std::strcmp(sym, "ESTX50")  == 0) return &estx50;
        return nullptr;
    }
};

// Stats for /api/v1/omega health -- read by status endpoint if wired.
struct ConsumerStats {
    std::atomic<int64_t> msgs_total{0};
    std::atomic<int64_t> parse_errors{0};
    std::atomic<int64_t> reconnects{0};
    std::atomic<bool>    connected{false};
};

// Parse array of doubles "1.23,4.56,7.89" into out[]. Returns count, max=max_out.
inline int parse_double_array(const char* p, double* out, int max_out) noexcept {
    if (!p) return 0;
    int n = 0;
    while (*p && n < max_out) {
        // Skip leading whitespace / brackets / commas
        while (*p == ' ' || *p == '[' || *p == ',') ++p;
        if (*p == ']' || *p == '\0') break;
        char* end = nullptr;
        double v = std::strtod(p, &end);
        if (end == p) break;
        out[n++] = v;
        p = end;
    }
    return n;
}

// Minimal lossy JSON extract -- fields fixed schema from sidecar.
// Returns true if all required fields parsed. No allocation, no exceptions.
inline bool parse_line(const char* line, size_t n,
                       char sym_out[32],
                       double& bid, double& ask,
                       double& bid_vol, double& ask_vol, double& imb,
                       int& bid_levels, int& ask_levels,
                       int64_t& ts_ms,
                       double bid_px_arr[L2Slot::MAX_LEVELS], double bid_sz_arr[L2Slot::MAX_LEVELS], int& bid_n,
                       double ask_px_arr[L2Slot::MAX_LEVELS], double ask_sz_arr[L2Slot::MAX_LEVELS], int& ask_n) noexcept
{
    // Expected: {"ts":...,"s":"XAUUSD","b":...,"a":...,"bv":...,"av":...,"i":...,"bl":...,"al":...}
    auto find_after = [&](const char* key) -> const char* {
        // Hand-rolled needle search (memmem is non-portable: not in libstdc++ <cstring>).
        const size_t klen = std::strlen(key);
        if (klen == 0 || klen > n) return static_cast<const char*>(nullptr);
        for (size_t i = 0; i + klen <= n; ++i) {
            if (std::memcmp(line + i, key, klen) == 0) {
                return line + i + klen;
            }
        }
        return static_cast<const char*>(nullptr);
    };
    const char* p_ts = find_after("\"ts\":");
    const char* p_s  = find_after("\"s\":\"");
    const char* p_b  = find_after("\"b\":");
    const char* p_a  = find_after("\"a\":");
    const char* p_bv = find_after("\"bv\":");
    const char* p_av = find_after("\"av\":");
    const char* p_i  = find_after("\"i\":");
    const char* p_bl = find_after("\"bl\":");
    const char* p_al = find_after("\"al\":");
    if (!p_ts || !p_s || !p_b || !p_a || !p_bv || !p_av || !p_i) return false;

    // ts: integer
    ts_ms = std::strtoll(p_ts, nullptr, 10);
    if (ts_ms <= 0) return false;

    // sym: copy until closing quote
    size_t i = 0;
    while (i < 31 && p_s[i] != '"' && p_s[i] != '\0') {
        sym_out[i] = p_s[i];
        ++i;
    }
    sym_out[i] = '\0';
    if (i == 0) return false;

    bid     = std::strtod(p_b,  nullptr);
    ask     = std::strtod(p_a,  nullptr);
    bid_vol = std::strtod(p_bv, nullptr);
    ask_vol = std::strtod(p_av, nullptr);
    imb     = std::strtod(p_i,  nullptr);
    bid_levels = p_bl ? static_cast<int>(std::strtol(p_bl, nullptr, 10)) : 0;
    ask_levels = p_al ? static_cast<int>(std::strtol(p_al, nullptr, 10)) : 0;

    if (bid <= 0.0 || ask <= 0.0 || ask < bid) return false;

    // Optional per-level arrays. Sidecar emits "bp":[..],"bs":[..],"ap":[..],"as":[..]
    bid_n = 0;
    ask_n = 0;
    const char* p_bp = find_after("\"bp\":");
    const char* p_bs = find_after("\"bs\":");
    const char* p_ap = find_after("\"ap\":");
    const char* p_as = find_after("\"as\":");
    if (p_bp && p_bs) {
        const int nb = parse_double_array(p_bp, bid_px_arr, L2Slot::MAX_LEVELS);
        const int sb = parse_double_array(p_bs, bid_sz_arr, L2Slot::MAX_LEVELS);
        bid_n = (nb < sb) ? nb : sb;
    }
    if (p_ap && p_as) {
        const int na = parse_double_array(p_ap, ask_px_arr, L2Slot::MAX_LEVELS);
        const int sa = parse_double_array(p_as, ask_sz_arr, L2Slot::MAX_LEVELS);
        ask_n = (na < sa) ? na : sa;
    }
    return true;
}

inline ibkr_sock::socket_t connect_localhost(const char* host, uint16_t port) noexcept {
    if (!ibkr_sock::ensure_init()) return ibkr_sock::INVALID_SOCK;
    ibkr_sock::socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == ibkr_sock::INVALID_SOCK) return ibkr_sock::INVALID_SOCK;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        ibkr_sock::close_sock(s);
        return ibkr_sock::INVALID_SOCK;
    }
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ibkr_sock::close_sock(s);
        return ibkr_sock::INVALID_SOCK;
    }
    // Read timeout so we don't block forever if peer stalls.
    // Windows: DWORD milliseconds. POSIX: timeval.
#ifdef _WIN32
    DWORD timeout_ms = 5000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{};
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return s;
}

// Long-running thread body. Stops when stop_flag goes true.
inline void run_consumer(L2Bus& bus, ConsumerStats& stats,
                        std::atomic<bool>& stop_flag,
                        const char* host, uint16_t port) noexcept
{
    using namespace std::chrono;
    std::string buf;
    buf.reserve(8192);
    char tmp[4096];

    while (!stop_flag.load(std::memory_order_relaxed)) {
        ibkr_sock::socket_t sock = connect_localhost(host, port);
        if (sock == ibkr_sock::INVALID_SOCK) {
            stats.connected.store(false, std::memory_order_relaxed);
            std::this_thread::sleep_for(seconds(2));
            continue;
        }
        stats.connected.store(true, std::memory_order_relaxed);
        stats.reconnects.fetch_add(1, std::memory_order_relaxed);
        std::printf("[IBKR-CONSUMER] connected %s:%u\n", host, port);
        std::fflush(stdout);

        while (!stop_flag.load(std::memory_order_relaxed)) {
            // recv returns int on Windows, ssize_t on POSIX. Use intmax-safe.
            int got = static_cast<int>(::recv(sock, tmp, sizeof(tmp), 0));
            if (got <= 0) break;
            buf.append(tmp, static_cast<size_t>(got));

            for (;;) {
                size_t nl = buf.find('\n');
                if (nl == std::string::npos) break;
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (line.empty()) continue;

                char     sym[32]    = {};
                double   bid = 0, ask = 0, bv = 0, av = 0, imb = 0;
                int      bl = 0, al = 0;
                int64_t  ts = 0;
                double   bp_a[L2Slot::MAX_LEVELS]{}, bs_a[L2Slot::MAX_LEVELS]{};
                double   ap_a[L2Slot::MAX_LEVELS]{}, as_a[L2Slot::MAX_LEVELS]{};
                int      bn_a = 0, an_a = 0;
                if (!parse_line(line.c_str(), line.size(),
                                sym, bid, ask, bv, av, imb, bl, al, ts,
                                bp_a, bs_a, bn_a,
                                ap_a, as_a, an_a)) {
                    stats.parse_errors.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                L2Slot* slot = bus.lookup(sym);
                if (!slot) continue;
                slot->bid.store(bid, std::memory_order_relaxed);
                slot->ask.store(ask, std::memory_order_relaxed);
                slot->bid_vol.store(bv, std::memory_order_relaxed);
                slot->ask_vol.store(av, std::memory_order_relaxed);
                slot->imb.store(imb, std::memory_order_relaxed);
                slot->bid_levels.store(bl, std::memory_order_relaxed);
                slot->ask_levels.store(al, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lk(slot->level_mtx);
                    slot->bid_n = bn_a;
                    slot->ask_n = an_a;
                    for (int i = 0; i < bn_a; ++i) {
                        slot->bid_prices[i] = bp_a[i];
                        slot->bid_sizes [i] = bs_a[i];
                    }
                    for (int i = 0; i < an_a; ++i) {
                        slot->ask_prices[i] = ap_a[i];
                        slot->ask_sizes [i] = as_a[i];
                    }
                }
                slot->last_ms.store(ts, std::memory_order_release);
                stats.msgs_total.fetch_add(1, std::memory_order_relaxed);
            }
        }
        ibkr_sock::close_sock(sock);
        stats.connected.store(false, std::memory_order_relaxed);
        std::printf("[IBKR-CONSUMER] disconnected, reconnecting in 2s\n");
        std::fflush(stdout);
        std::this_thread::sleep_for(seconds(2));
    }
}

} // namespace omega::ibkr

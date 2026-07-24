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
#include <functional>
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
    L2Slot ger40;   // GER40               (IBKR DAX  front-month EUREX)
    L2Slot uk100;   // UK100               (IBKR Z    front-month ICEEU)
    L2Slot estx50;  // ESTX50              (IBKR ESTX50 front-month EUREX)
    // S-2026-07-09 COMPLETE FEED MIGRATION: the remaining BlackBull-fed symbols
    // now ride the IBKR bridge as L1 (reqMktData top-of-book, NO depth-slot cost),
    // broadcast under contract.symbol (aliased below to these slots).
    L2Slot usoil;   // USOIL.F             (IBKR CL   front-month NYMEX; contract.symbol "CL")
    L2Slot vix;     // VIX.F               (IBKR VX   front-month CFE;   contract.symbol "VX")
    L2Slot dxy;     // DX.F                (IBKR DX   front-month NYBOT; contract.symbol "DX")
    L2Slot ngas;    // NGAS.F              (IBKR NG   front-month NYMEX; contract.symbol "NG")
    L2Slot brent;   // BRENT / UKBRENT     (IBKR COIL front-month IPE;   contract.symbol "COIL")
    // FX majors (S-2026-07-06): IBKR IDEALPRO L1 (reqMktData top-of-book, NOT
    // reqMktDepth -- L1 carries NO depth-slot cost so these coexist with the 3
    // capped depth streams). Fed by the bridge L1Recorder; carries only bid/ask
    // (no book levels -- bid_vol/ask_vol/imb are synthetic 0.5). Purpose: route
    // FX quotes off frozen BlackBull FIX onto the live IBKR link.
    L2Slot eurusd;  // EURUSD              (IBKR EUR.USD CASH/IDEALPRO L1)
    L2Slot gbpusd;  // GBPUSD              (IBKR GBP.USD CASH/IDEALPRO L1)
    L2Slot usdjpy;  // USDJPY              (IBKR USD.JPY CASH/IDEALPRO L1)
    L2Slot audusd;  // AUDUSD              (IBKR AUD.USD CASH/IDEALPRO L1)
    L2Slot nzdusd;  // NZDUSD              (IBKR NZD.USD CASH/IDEALPRO L1)
    L2Slot usdcad;  // USDCAD              (IBKR USD.CAD CASH/IDEALPRO L1; S-2026-07-08c short-ladder feed)

    // Map bridge-emitted symbol string to the slot it updates.
    // Bridge symbols come in either "US500.F"/"NAS100"/etc (Blackbull naming
    // -- the bridge writes whatever was passed via --symbols), so we accept
    // both forms here. USTEC.F maps to nas100 (same NQ underlying).
    L2Slot* lookup(const char* sym) noexcept {
        if (std::strcmp(sym, "XAUUSD")  == 0) return &xau;
        if (std::strcmp(sym, "XAGUSD")  == 0) return &xag;
        if (std::strcmp(sym, "US500")   == 0 || std::strcmp(sym, "US500.F")  == 0
         || std::strcmp(sym, "ES")      == 0) return &us500;  // S-2026-07-09: US500 now on --symbols as L1; bridge broadcasts contract.symbol "ES" (CME front-month)
        if (std::strcmp(sym, "NAS100")  == 0
         || std::strcmp(sym, "USTEC")   == 0 || std::strcmp(sym, "USTEC.F")  == 0
         || std::strcmp(sym, "NQ")      == 0) return &nas100;  // S-2026-07-09: bridge DomRecorder broadcasts contract.symbol "NQ" (CME front-month) for the NAS100 --symbols key; without this alias the nas100 slot was NEVER fed (mirrors the "YM"->dj30 case)
        if (std::strcmp(sym, "DJ30")    == 0 || std::strcmp(sym, "DJ30.F")   == 0
         || std::strcmp(sym, "YM")      == 0) return &dj30;  // bridge emits contract.symbol "YM" for the DJ30 key (CBOT front-month)
        if (std::strcmp(sym, "GER40")   == 0 || std::strcmp(sym, "DAX")      == 0) return &ger40;  // bridge L1 broadcasts contract.symbol "DAX" (EUREX front-month)
        if (std::strcmp(sym, "UK100")   == 0 || std::strcmp(sym, "Z")        == 0) return &uk100;  // bridge L1 broadcasts contract.symbol "Z" (ICEEU FTSE front-month)
        if (std::strcmp(sym, "ESTX50")  == 0 || std::strcmp(sym, "SX5E")     == 0) return &estx50; // bridge L1 broadcasts contract.symbol "ESTX50"; "SX5E" defensive alias
        // S-2026-07-09 COMPLETE FEED MIGRATION: index/commodity L1 symbols. Accept
        // BOTH the BlackBull FIX name (US500.F/USOIL.F/VIX.F/DX.F/NGAS.F/BRENT --
        // used by the fix_dispatch gate) AND the IBKR contract.symbol the bridge
        // broadcasts (CL/VX/DX/NG/COIL -- used by the consumer feed path).
        if (std::strcmp(sym, "USOIL")   == 0 || std::strcmp(sym, "USOIL.F")  == 0
         || std::strcmp(sym, "CL")      == 0) return &usoil;
        if (std::strcmp(sym, "VIX")     == 0 || std::strcmp(sym, "VIX.F")    == 0
         || std::strcmp(sym, "VX")      == 0) return &vix;
        if (std::strcmp(sym, "DX")      == 0 || std::strcmp(sym, "DX.F")     == 0) return &dxy;  // FIX "DX.F"; IBKR contract.symbol "DX" -- both here
        if (std::strcmp(sym, "NGAS")    == 0 || std::strcmp(sym, "NGAS.F")   == 0
         || std::strcmp(sym, "NG")      == 0) return &ngas;
        if (std::strcmp(sym, "BRENT")   == 0 || std::strcmp(sym, "UKBRENT")  == 0
         || std::strcmp(sym, "COIL")    == 0) return &brent;
        // FX majors (IBKR IDEALPRO L1). Bridge broadcasts the 6-char pair as "s".
        if (std::strcmp(sym, "EURUSD")  == 0) return &eurusd;
        if (std::strcmp(sym, "GBPUSD")  == 0) return &gbpusd;
        if (std::strcmp(sym, "USDJPY")  == 0) return &usdjpy;
        if (std::strcmp(sym, "AUDUSD")  == 0) return &audusd;
        if (std::strcmp(sym, "NZDUSD")  == 0) return &nzdusd;
        if (std::strcmp(sym, "USDCAD")  == 0) return &usdcad;
        return nullptr;
    }
};

// True for the FX majors the bridge carries on the IBKR IDEALPRO L1 line.
// Used to (a) route FX book updates to on_tick in omega_main, and (b) gate the
// frozen BlackBull FIX FX quotes out of the book WHEN the IBKR slot is fresh
// (bridge down -> not fresh -> BlackBull remains the fallback, no FX blackout).
inline bool is_fx_major(const char* s) noexcept {
    return std::strcmp(s, "EURUSD") == 0 || std::strcmp(s, "GBPUSD") == 0
        || std::strcmp(s, "USDJPY") == 0 || std::strcmp(s, "AUDUSD") == 0
        || std::strcmp(s, "NZDUSD") == 0
        || std::strcmp(s, "USDCAD") == 0;  // S-2026-07-08c: short-ladder pair (IBKR-only; no BlackBull feed to gate)
}

// True for the NON-FX symbols that now ride the IBKR bridge as their PRIMARY
// price feed. Argument is the BlackBull FIX symbol name (as seen in fix_dispatch).
// Same gate mechanics as is_fx_major: when the IBKR slot is fresh, the BlackBull
// FIX quote for this symbol is dropped so the live IBKR price drives on_tick;
// bridge down / no market-data entitlement (Error 354) -> slot stale >5s ->
// BlackBull resumes as the automatic fallback (no blackout for any symbol).
//
// S-2026-07-09 COMPLETE FEED MIGRATION (operator: "nothing on BlackBull, IBKR is
// cheaper"). EVERY remaining BlackBull-fed symbol is now here -- the depth pair
// (XAUUSD spot, NAS100 NQ) plus the L1-migrated index/commodity/vol set:
//   XAUUSD  -> IBKR XAUUSD CMDTY/SMART SPOT   (same scale as BlackBull spot; nil shift)
//   NAS100  -> IBKR NQ  CME  FUTURES (depth)  (~0.75%/~220pt basis ABOVE the CFD)
//   USTEC.F -> IBKR NQ  (== NAS100; on_book posts BOTH; gated off the nas100 slot)
//   US500.F -> IBKR ES  CME  FUTURES (L1)     (basis ABOVE the CFD; ladder re-seeded)
//   GER40   -> IBKR DAX EUREX FUTURES (L1)    (basis ABOVE the CFD; ladder re-seeded)
//   UK100   -> IBKR Z   ICEEU FTSE FUT (L1)
//   ESTX50  -> IBKR ESTX50 EUREX FUT (L1)
//   USOIL.F -> IBKR CL  NYMEX FUTURES (L1)
//   XAGUSD  -> IBKR XAGUSD CMDTY/SMART SPOT (L1)
//   VIX.F   -> IBKR VX  CFE  FUTURES (L1)
//   DX.F    -> IBKR DX  NYBOT FUTURES (L1)
//   NGAS.F  -> IBKR NG  NYMEX FUTURES (L1)
//   BRENT   -> IBKR COIL IPE FUTURES (L1)
// The futures-vs-CFD basis is a cosmetic level shift on a SHADOW/paper book (only
// 1 MGC micro is real, on a separate COMEX feed). See outputs/FEED_AUDIT_2026-07-09.md.
inline bool is_ibkr_primary_index(const char* s) noexcept {
    return std::strcmp(s, "XAUUSD")  == 0 || std::strcmp(s, "NAS100")  == 0
        || std::strcmp(s, "USTEC.F") == 0 || std::strcmp(s, "US500.F") == 0
        || std::strcmp(s, "GER40")   == 0 || std::strcmp(s, "UK100")   == 0
        || std::strcmp(s, "ESTX50")  == 0 || std::strcmp(s, "USOIL.F") == 0
        || std::strcmp(s, "XAGUSD")  == 0 || std::strcmp(s, "VIX.F")   == 0
        || std::strcmp(s, "DX.F")    == 0 || std::strcmp(s, "NGAS.F")  == 0
        || std::strcmp(s, "BRENT")   == 0
        || std::strcmp(s, "M2K")     == 0;  // 2026-07-09 micro Russell (CME), IBKR-only L1
}

// HARD-DROP set (operator "STOP BLACKBULL"): symbols where IBKR is entitled +
// actively flowing and a BlackBull FIX tick must be dropped UNCONDITIONALLY --
// no freshness fallback. If IBKR dies the tile goes DARK (honest) rather than
// riding a stale/CFD price. Promoted from the local lambda in fix_dispatch.hpp
// (S-2026-07-24) to a single shared definition so the boot feed-audit below can
// reuse the SAME list (derive-don't-copy; a second copy would drift).
//   DJ30.F added S-2026-07-24: it already rides a real IBKR YM/CBOT feed (the
//   `dj30` L2 slot + the omega_main on_book synthetic-tick pump) and BlackBull
//   streams ZERO DJ30 ticks, so hard-drop carries no dark-risk and closes the
//   last BlackBull-UNCONDITIONAL traded symbol (it was in none of the routing
//   sets -> fell through to the unconditional engine_dispatch_post_tick).
inline bool is_ibkr_hard(const char* s) noexcept {
    return std::strcmp(s, "XAUUSD")  == 0 || std::strcmp(s, "NAS100")  == 0
        || std::strcmp(s, "USTEC.F") == 0 || std::strcmp(s, "US500.F") == 0
        || std::strcmp(s, "XAGUSD")  == 0 || std::strcmp(s, "GER40")   == 0
        || std::strcmp(s, "ESTX50")  == 0 || std::strcmp(s, "DJ30.F")  == 0;
}

// Boot-time feed-routing audit (S-2026-07-24). Asserts NO traded symbol can take
// an UNCONDITIONAL BlackBull FIX tick (operator hard rule "STOP BLACKBULL"). Every
// traded symbol must be classified into exactly one IBKR routing set:
//   * is_ibkr_hard         -> BlackBull dropped unconditionally (dark on IBKR loss)
//   * is_fx_major          -> BlackBull dropped unconditionally (dark on IBKR loss)
//   * is_ibkr_primary_index-> BlackBull is a FRESHNESS-GATED fallback only (never
//                             unconditional; UNENTITLED index/energy set keeps this
//                             so it is not dark when IBKR has no data at all)
// A symbol in NONE of the three sets reaches the unconditional post in fix_dispatch
// -> a VIOLATION. Prints one summary line + a per-symbol VIOLATION line; returns the
// violation count (0 = clean = STOP-BLACKBULL invariant holds). Read by boot/protection
// monitors that assert "0 VIOLATION".
inline int audit_blackbull_unconditional(const char* const* traded, int n) noexcept {
    int viol = 0, hard = 0, fx = 0, gated = 0;
    for (int i = 0; i < n; ++i) {
        const char* s = traded[i];
        if (is_ibkr_hard(s))                 { ++hard;  continue; }
        if (is_fx_major(s))                  { ++fx;    continue; }
        if (is_ibkr_primary_index(s))        { ++gated; continue; }
        ++viol;
        std::fprintf(stderr,
            "[FEED-AUDIT] *** VIOLATION *** traded symbol '%s' is BlackBull-UNCONDITIONAL "
            "(in no IBKR routing set) -- add to is_ibkr_hard or is_ibkr_primary_index\n", s);
    }
    std::printf(
        "[FEED-AUDIT] BlackBull-unconditional traded symbols: %d VIOLATION (0=clean) "
        "[%d hard-drop / %d fx-major / %d freshness-gated of %d traded]\n",
        viol, hard, fx, gated, n);
    std::fflush(stdout);
    std::fflush(stderr);
    return viol;
}

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

// Per-book-update callback. Fires AFTER a slot is updated, with the bridge
// symbol string + best bid/ask. Used to synthesize a tick for symbols that
// have NO native FIX quote feed (DJ30 -- BlackBull streams zero DJ30 ticks),
// so the on_tick() engine family for that symbol actually runs. Empty by
// default -> pure depth-only consumer (unchanged legacy behavior). The
// callback MUST filter to only the symbols it wants to drive; feeding a
// symbol that ALSO has a FIX tick would double-feed its engines.
using BookUpdateCb = std::function<void(const char* sym, double bid, double ask)>;

// Long-running thread body. Stops when stop_flag goes true.
inline void run_consumer(L2Bus& bus, ConsumerStats& stats,
                        std::atomic<bool>& stop_flag,
                        const char* host, uint16_t port,
                        BookUpdateCb on_book = {}) noexcept
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
            if (got == 0) break;  // graceful close from peer (FIN)
            if (got < 0) {
                // Distinguish RCVTIMEO (idle, expected during weekends or
                // pre-market) from a real socket error. The 5s SO_RCVTIMEO
                // set in connect_localhost() will fire every 5s when the
                // bridge has no ticks to send; treating those as disconnect
                // produced a tight reconnect loop. Only real errors should
                // tear down the session.
#ifdef _WIN32
                const int err = ibkr_sock::last_err();
                if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) continue;
#else
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
#endif
                break;  // real error -> reconnect
            }
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

                // Synthetic-tick hook. Fires for symbols with no native FIX
                // quote feed (see BookUpdateCb). Filtering is the callback's
                // job -- pass every update through.
                if (on_book) on_book(sym, bid, ask);
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

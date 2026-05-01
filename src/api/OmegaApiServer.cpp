// ==============================================================================
// OmegaApiServer.cpp
// HTTP/1.1 :7781 (loopback) -- read-API for the omega-terminal React UI.
//
// Step 2 of the Omega Terminal build plan. Excluded from backtest builds
// (the entire translation unit is gated on #ifndef OMEGA_BACKTEST below).
//
// Routes:
//   GET /api/v1/omega/engines    -> [Engine]
//   GET /api/v1/omega/positions  -> [Position]      (skeleton until Step 3)
//   GET /api/v1/omega/ledger     -> [LedgerEntry]   from g_omegaLedger
//   GET /api/v1/omega/equity     -> [EquityPoint]   (skeleton until Step 3)
//   GET /                        -> static file from omega-terminal/dist/
//   GET /<asset>                 -> static file (or index.html SPA fallback)
//
// Static file serving (added 2026-05-01): any GET that does not match an
// /api/v1/omega/ route falls through to try_serve_static(), which reads from
// omega-terminal/dist/ relative to the service's working directory (C:\Omega
// in production). Paths without an extension fall back to index.html so the
// React-Router client-side routes resolve. Hashed Vite assets (e.g.
// /assets/index-abc123.js) get an immutable cache header; index.html is
// served no-cache so deploys take effect immediately. Path traversal is
// blocked by rejecting any path containing "..".
//
// This is what makes "http://VPS_IP:7781/" load the omega-terminal UI
// directly from the engine binary -- no separate node/nginx process needed.
//
// Engine and ledger reads use their own internal mutexes (EngineRegistry::mu_,
// OmegaTradeLedger::m_mtx). Positions/equity are stub responses for now --
// Step 2's exit gate is "engines endpoint returns the live engine list", and
// Step 3 wires up CC/ENG/POS panels which will demand richer accessors. That
// matches docs/omega_terminal/STEP2_OPENER.md § "No panel changes yet."
// ==============================================================================

#ifndef OMEGA_BACKTEST

#include "OmegaApiServer.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)
#define closesocket    ::close
#define Sleep(ms)      ::usleep((ms) * 1000)
#endif

#include "EngineRegistry.hpp"   // EngineSnapshot + EngineRegistry + g_engines (extern)
#include "OmegaTradeLedger.hpp" // omega::OmegaTradeLedger / TradeRecord

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern omega::OmegaTradeLedger g_omegaLedger;
// g_engines is declared `extern` in EngineRegistry.hpp; defined in globals.hpp.

namespace omega {

// ─────────────────────────────────────────────────────────────────────────────
// JSON helpers (no third-party deps).
// Hand-rolled to match the STEP2_OPENER constraint of reusing the same lib
// choices as OmegaTelemetryServer.cpp -- which uses none.
// ─────────────────────────────────────────────────────────────────────────────

static std::string json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

static std::string json_str(const std::string& s)
{
    std::string out = "\"";
    out += json_escape(s);
    out += "\"";
    return out;
}

static std::string json_num(double d)
{
    // Use %.10g for compact, lossless-enough textual rep. Inf/NaN are coerced
    // to 0 because JSON has no representation for them and the UI surfaces
    // them as "broken data" rather than gracefully degrading.
    if (!std::isfinite(d)) return "0";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.10g", d);
    return std::string(buf);
}

static std::string json_int(int64_t v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    return std::string(buf);
}

static std::string json_bool(bool b)
{
    return b ? "true" : "false";
}

// ─────────────────────────────────────────────────────────────────────────────
// Route builders
// ─────────────────────────────────────────────────────────────────────────────

static std::string build_engines_json()
{
    const auto snaps = g_engines.snapshot_all();
    std::string out = "[";
    for (size_t i = 0; i < snaps.size(); ++i) {
        const EngineSnapshot& s = snaps[i];
        if (i > 0) out += ",";
        out += "{";
        out += "\"name\":"            + json_str(s.name)             + ",";
        out += "\"enabled\":"         + json_bool(s.enabled)         + ",";
        out += "\"mode\":"            + json_str(s.mode)             + ",";
        out += "\"state\":"           + json_str(s.state)            + ",";
        out += "\"last_signal_ts\":"  + json_int(s.last_signal_ts)   + ",";
        out += "\"last_pnl\":"        + json_num(s.last_pnl);
        out += "}";
    }
    out += "]";
    return out;
}

// Stub: open-positions reporting is Step 3 work (panel-side wiring is when
// we'll need a stable open-position accessor across engines). For now we
// return an empty array so the route is wired and the UI fetch contract
// can be validated end-to-end via the proxy.
static std::string build_positions_json()
{
    return "[]";
}

// Trade ledger -> JSON, with optional filters.
//
//   from / to    : ISO-8601 timestamps. We accept them as opaque strings here
//                  and convert via parse_iso_to_unix_ms() below; if parsing
//                  fails the filter is dropped (no 4xx).
//   engine       : exact-match filter on TradeRecord::engine.
//   limit        : hard cap. Default 1000, clamped to [1, 10000].
static int64_t parse_iso_to_unix_ms(const std::string& iso)
{
    if (iso.empty()) return 0;
    // Accept "YYYY-MM-DDTHH:MM:SS" with optional "Z" or fractional seconds.
    // No timezone offsets honored beyond Z (UTC). On any parse failure return
    // 0 so the caller can treat 0 as "no filter".
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    int matched = std::sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d",
                              &y, &mo, &d, &h, &mi, &s);
    if (matched < 6) {
        // Try a "YYYY-MM-DD" date-only form.
        matched = std::sscanf(iso.c_str(), "%4d-%2d-%2d", &y, &mo, &d);
        if (matched < 3) return 0;
        h = 0; mi = 0; s = 0;
    }
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon  = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min  = mi;
    t.tm_sec  = s;
#ifdef _WIN32
    const time_t epoch = _mkgmtime(&t);
#else
    const time_t epoch = timegm(&t);
#endif
    if (epoch == static_cast<time_t>(-1)) return 0;
    return static_cast<int64_t>(epoch) * 1000LL;
}

static std::string build_ledger_json(const std::unordered_map<std::string, std::string>& q)
{
    int64_t from_ms = 0;
    int64_t to_ms   = 0;
    std::string engine_filter;
    int limit = 1000;

    auto it = q.find("from");
    if (it != q.end()) from_ms = parse_iso_to_unix_ms(it->second);
    it = q.find("to");
    if (it != q.end()) to_ms = parse_iso_to_unix_ms(it->second);
    it = q.find("engine");
    if (it != q.end()) engine_filter = it->second;
    it = q.find("limit");
    if (it != q.end()) {
        try {
            limit = std::stoi(it->second);
        } catch (...) {
            limit = 1000;
        }
    }
    if (limit < 1)     limit = 1;
    if (limit > 10000) limit = 10000;

    const auto trades = g_omegaLedger.snapshot();

    std::string out = "[";
    int emitted = 0;
    // Walk newest-to-oldest so `limit` returns the most recent N matching.
    for (auto rit = trades.rbegin(); rit != trades.rend(); ++rit) {
        if (emitted >= limit) break;
        const TradeRecord& tr = *rit;
        const int64_t entry_ms = static_cast<int64_t>(tr.entryTs) * 1000LL;
        const int64_t exit_ms  = static_cast<int64_t>(tr.exitTs)  * 1000LL;
        if (from_ms != 0 && entry_ms <  from_ms) continue;
        if (to_ms   != 0 && entry_ms >= to_ms)   continue;
        if (!engine_filter.empty() && tr.engine != engine_filter) continue;

        if (emitted > 0) out += ",";
        out += "{";
        out += "\"id\":"        + json_str(std::to_string(tr.id))   + ",";
        out += "\"engine\":"    + json_str(tr.engine)               + ",";
        out += "\"symbol\":"    + json_str(tr.symbol)               + ",";
        out += "\"side\":"      + json_str(tr.side)                 + ",";
        out += "\"entry_ts\":"  + json_int(entry_ms)                + ",";
        out += "\"exit_ts\":"   + json_int(exit_ms)                 + ",";
        out += "\"entry\":"     + json_num(tr.entryPrice)           + ",";
        out += "\"exit\":"      + json_num(tr.exitPrice)            + ",";
        // Prefer net_pnl when costs were applied; fall back to gross.
        out += "\"pnl\":"       + json_num(tr.net_pnl != 0.0 ? tr.net_pnl : tr.pnl) + ",";
        out += "\"reason\":"    + json_str(tr.exitReason);
        out += "}";
        ++emitted;
    }
    out += "]";
    return out;
}

// Stub: equity time-series is Step 3 work. Server-side aggregation across
// 1m / 1h / 1d intervals will be implemented when CC's equity strip lands.
// Returning an empty array lets the UI layer's typing flow through end-to-end
// without divergence.
static std::string build_equity_json(const std::unordered_map<std::string, std::string>& q)
{
    (void)q;
    return "[]";
}

// ─────────────────────────────────────────────────────────────────────────────
// HTTP request parsing
// ─────────────────────────────────────────────────────────────────────────────

// Returns the request-target (path?query) from the request line, or empty.
// The buffer is the raw recv() payload, NUL-terminated by the caller.
static std::string parse_request_target(const char* req)
{
    // Format: METHOD SP request-target SP HTTP/1.1 CRLF ...
    const char* sp1 = std::strchr(req, ' ');
    if (!sp1) return "";
    const char* tgt = sp1 + 1;
    const char* sp2 = std::strchr(tgt, ' ');
    if (!sp2) return "";
    return std::string(tgt, sp2 - tgt);
}

// Split "/api/v1/omega/ledger?from=...&engine=..." into (path, query map).
static void split_target(const std::string& target,
                         std::string& path,
                         std::unordered_map<std::string, std::string>& q)
{
    const auto qpos = target.find('?');
    if (qpos == std::string::npos) {
        path = target;
        return;
    }
    path = target.substr(0, qpos);
    const std::string qs = target.substr(qpos + 1);
    size_t i = 0;
    while (i < qs.size()) {
        size_t amp = qs.find('&', i);
        if (amp == std::string::npos) amp = qs.size();
        const std::string pair = qs.substr(i, amp - i);
        const auto eq = pair.find('=');
        if (eq != std::string::npos) {
            // No URL-decoding here -- ISO-8601 and engine names don't need it
            // and Step 3's panels don't pass anything that would. If/when we
            // need it, add a percent-decoder helper.
            q[pair.substr(0, eq)] = pair.substr(eq + 1);
        }
        i = amp + 1;
    }
}

// ──────────────────────────────────────────────────────────────────────────────────
// Static file serving (added 2026-05-01)
//
// Serves the omega-terminal React build from omega-terminal/dist/ relative
// to the service working directory (C:\Omega in production -- NSSM sets
// AppDirectory to C:\Omega so this resolves correctly). Allows
// http://VPS_IP:7781/ to load the UI directly from the engine binary, with
// no separate node/nginx process to manage.
//
// Path resolution rules:
//   "/" or ""              -> serve index.html (no-cache)
//   "/something.ext"       -> serve omega-terminal/dist/something.ext if it
//                             exists, else 404 (we DO NOT fall back to
//                             index.html for asset-shaped paths because
//                             /robots.txt etc. should genuinely 404)
//   "/something" (no ext)  -> SPA fallback: serve index.html so client-side
//                             React Router can resolve the route
//
// Path traversal is blocked by rejecting any request path containing "..".
// We don't bother decoding percent-escapes -- the React build emits ASCII
// asset paths (no spaces, no unicode) and any UI-side fetch using percent
// escapes is hitting /api/v1/omega/* which routes before this helper runs.
//
// MIME types cover the Vite default output: index.html, assets/*.js,
// assets/*.css, assets/*.svg, plus common image/font/source-map extensions.
// Anything unrecognised gets application/octet-stream -- the browser will
// refuse to execute it (X-Content-Type-Options: nosniff is set globally),
// which is the right outcome for unexpected files in dist/.
//
// Cache policy is split: index.html no-cache (deploys must take effect on
// next page load), hashed Vite assets immutable for one year.
// ──────────────────────────────────────────────────────────────────────────────────

static bool ends_with(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    return s.size() >= n && std::memcmp(s.data() + s.size() - n, suffix, n) == 0;
}

static bool try_serve_static(const std::string& path,
                             std::string& body,
                             std::string& ctype,
                             std::string& cache,
                             int& status)
{
    // Path-traversal guard. Anything trying to escape dist/ via .. is rejected
    // outright; we do not bother trying to canonicalise.
    if (path.find("..") != std::string::npos) return false;

    // Resolve "/" -> "/index.html". rel always begins with "/".
    std::string rel = path;
    if (rel.empty() || rel == "/") rel = "/index.html";

    const std::string dist_root = "omega-terminal/dist";
    std::string fs_path = dist_root + rel;

    bool is_index = (rel == "/index.html");
    std::ifstream f(fs_path, std::ios::binary);
    if (!f.good()) {
        // SPA fallback: paths with no extension are treated as client-side
        // routes (e.g. /cc, /eng/HBG) and resolve to index.html.
        // Paths with an extension that we couldn't find are real 404s --
        // we deliberately do not serve index.html for /favicon.ico misses,
        // because that would confuse browsers expecting an image.
        const size_t slash = rel.find_last_of('/');
        const std::string leaf = (slash == std::string::npos) ? rel : rel.substr(slash + 1);
        if (leaf.find('.') != std::string::npos) {
            return false;   // asset-shaped miss -> 404
        }
        f.open(dist_root + "/index.html", std::ios::binary);
        if (!f.good()) return false;
        is_index = true;
        rel = "/index.html";
    }

    // Read the file as binary into body. std::string handles arbitrary bytes
    // (length-tracked, not NUL-terminated) so this is safe for js/css/png/etc.
    f.seekg(0, std::ios::end);
    const std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    body.resize(static_cast<size_t>(sz));
    if (sz > 0) f.read(&body[0], sz);

    // MIME type by extension.
    if      (ends_with(rel, ".html"))   ctype = "text/html; charset=utf-8";
    else if (ends_with(rel, ".js"))     ctype = "application/javascript; charset=utf-8";
    else if (ends_with(rel, ".mjs"))    ctype = "application/javascript; charset=utf-8";
    else if (ends_with(rel, ".css"))    ctype = "text/css; charset=utf-8";
    else if (ends_with(rel, ".json"))   ctype = "application/json; charset=utf-8";
    else if (ends_with(rel, ".svg"))    ctype = "image/svg+xml";
    else if (ends_with(rel, ".png"))    ctype = "image/png";
    else if (ends_with(rel, ".jpg") ||
             ends_with(rel, ".jpeg"))   ctype = "image/jpeg";
    else if (ends_with(rel, ".webp"))   ctype = "image/webp";
    else if (ends_with(rel, ".ico"))    ctype = "image/x-icon";
    else if (ends_with(rel, ".woff2"))  ctype = "font/woff2";
    else if (ends_with(rel, ".woff"))   ctype = "font/woff";
    else if (ends_with(rel, ".ttf"))    ctype = "font/ttf";
    else if (ends_with(rel, ".map"))    ctype = "application/json; charset=utf-8";
    else if (ends_with(rel, ".txt"))    ctype = "text/plain; charset=utf-8";
    else                                 ctype = "application/octet-stream";

    // index.html is no-cache so deploys take effect immediately. Hashed Vite
    // assets (vite emits /assets/<name>-<hash>.{js,css}) are content-addressed
    // and safe to mark immutable; this avoids the network round-trip on every
    // page load for the bundled JS/CSS.
    if (is_index) {
        cache = "no-store, no-cache, must-revalidate";
    } else {
        cache = "public, max-age=31536000, immutable";
    }

    status = 200;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// OmegaApiServer ctor / dtor / start / stop
// ─────────────────────────────────────────────────────────────────────────────

OmegaApiServer::OmegaApiServer()
    : running_(false)
    , server_fd_(INVALID_SOCKET)
{}

OmegaApiServer::~OmegaApiServer() { stop(); }

void OmegaApiServer::start(int http_port)
{
    if (running_.load()) return;
    running_.store(true);
    thread_ = std::thread(&OmegaApiServer::run, this, http_port);
}

void OmegaApiServer::stop()
{
    if (!running_.load()) return;
    running_.store(false);
    if (server_fd_ != INVALID_SOCKET) closesocket(server_fd_);
    if (thread_.joinable()) thread_.join();
    server_fd_ = INVALID_SOCKET;
}

// ─────────────────────────────────────────────────────────────────────────────
// HTTP run loop -- mirrors OmegaTelemetryServer::run idioms (SO_REUSEADDR,
// SO_RCVTIMEO=200ms so closesocket() in stop() wakes the accept thread).
// 127.0.0.1 bind ONLY -- this server is internal infrastructure for the
// Vite dev proxy; nothing on the network should reach it.
// ─────────────────────────────────────────────────────────────────────────────

void OmegaApiServer::run(int port)
{
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ == INVALID_SOCKET) {
        std::cerr << "[OmegaApi] socket() failed\n";
        return;
    }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

#ifdef _WIN32
    DWORD accept_timeout_ms = 200;
    setsockopt(server_fd_, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&accept_timeout_ms),
               sizeof(accept_timeout_ms));
#else
    timeval accept_timeout{};
    accept_timeout.tv_sec  = 0;
    accept_timeout.tv_usec = 200 * 1000;
    setsockopt(server_fd_, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&accept_timeout),
               sizeof(accept_timeout));
#endif

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    // 127.0.0.1 ONLY -- this is the security boundary that lets us run a
    // hand-rolled HTTP/1.1 stack without an auth layer.
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(static_cast<u_short>(port));

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(server_fd_, 16) == SOCKET_ERROR) {
        std::cerr << "[OmegaApi] bind/listen failed on port " << port << "\n";
        closesocket(server_fd_);
        server_fd_ = INVALID_SOCKET;
        return;
    }

    std::cout << "[OmegaApi] port " << port << " (loopback)\n" << std::flush;

    while (running_.load()) {
        sockaddr_in ca{};
#ifdef _WIN32
        int cl = sizeof(ca);
        SOCKET c = accept(server_fd_, reinterpret_cast<sockaddr*>(&ca), &cl);
#else
        socklen_t cl = sizeof(ca);
        int c = accept(server_fd_, reinterpret_cast<sockaddr*>(&ca), &cl);
#endif
        if (c == INVALID_SOCKET) {
            if (!running_.load()) break;
            Sleep(5);
            continue;
        }

        char buf[4096];
        const int n = recv(c, buf, static_cast<int>(sizeof(buf)) - 1, 0);
        if (n <= 0) { closesocket(c); continue; }
        buf[n] = '\0';

        const std::string target = parse_request_target(buf);
        std::string path;
        std::unordered_map<std::string, std::string> q;
        split_target(target, path, q);

        std::string body;
        std::string ctype = "application/json";
        // Cache-Control default for API responses; try_serve_static() may
        // overwrite this with an asset-appropriate policy below.
        std::string cache = "no-store, no-cache, must-revalidate";
        int status = 200;

        // Only GET is supported. Anything else gets 405. We don't bother
        // parsing the method explicitly -- the request line either starts
        // with "GET " or it's a 405. Keeps the dispatch logic flat.
        if (std::strncmp(buf, "GET ", 4) != 0) {
            status = 405;
            body   = "{\"error\":\"method not allowed\"}";
        }
        else if (path == "/api/v1/omega/engines") {
            body = build_engines_json();
        }
        else if (path == "/api/v1/omega/positions") {
            body = build_positions_json();
        }
        else if (path == "/api/v1/omega/ledger") {
            body = build_ledger_json(q);
        }
        else if (path == "/api/v1/omega/equity") {
            body = build_equity_json(q);
        }
        else if (try_serve_static(path, body, ctype, cache, status)) {
            // Handled by static file serving (status/body/ctype/cache filled).
        }
        else {
            status = 404;
            body   = "{\"error\":\"not found\",\"path\":" + json_str(path) + "}";
        }

        const char* status_text = "OK";
        if      (status == 404) status_text = "Not Found";
        else if (status == 405) status_text = "Method Not Allowed";
        else if (status != 200) status_text = "Error";

        char hdr[512];
        std::snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "X-Content-Type-Options: nosniff\r\n"
            "Cache-Control: %s\r\n"
            "Connection: close\r\n\r\n",
            status,
            status_text,
            ctype.c_str(),
            body.size(),
            cache.c_str());
        send(c, hdr,           static_cast<int>(std::strlen(hdr)), 0);
        send(c, body.c_str(),  static_cast<int>(body.size()),      0);
        closesocket(c);
    }

    if (server_fd_ != INVALID_SOCKET) {
        closesocket(server_fd_);
        server_fd_ = INVALID_SOCKET;
    }
}

} // namespace omega

#endif // OMEGA_BACKTEST

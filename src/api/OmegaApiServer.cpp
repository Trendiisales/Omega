// ==============================================================================
// OmegaApiServer.cpp
// HTTP/1.1 :7781 (loopback) -- read-API for the omega-terminal React UI.
//
// Step 2 of the Omega Terminal build plan. Excluded from backtest builds
// (the entire translation unit is gated on #ifndef OMEGA_BACKTEST below).
//
// Routes:
//   GET /api/v1/omega/engines    -> [Engine]                       (Step 2)
//   GET /api/v1/omega/positions  -> [Position]                     (Step 3)
//   GET /api/v1/omega/ledger     -> [LedgerEntry] from g_omegaLedger
//   GET /api/v1/omega/equity     -> [EquityPoint]                  (Step 3)
//   GET /api/v1/omega/intel      -> OpenBB news envelope           (Step 5)
//   GET /api/v1/omega/curv       -> OpenBB treasury_rates envelope (Step 5)
//   GET /api/v1/omega/wei        -> OpenBB equity quote envelope   (Step 5)
//   GET /api/v1/omega/mov        -> OpenBB discovery envelope      (Step 5)
//   GET /                        -> static file from omega-terminal/dist/
//   GET /<asset>                 -> static file (or index.html SPA fallback)
//
// Step 5 OpenBB routes:
//   These four routes proxy to OpenBB Hub (https://api.openbb.co/api/v1/) via
//   src/api/OpenBbProxy. Each returns the OpenBB OBBject envelope verbatim --
//   no JSON parsing in C++; the UI dereferences `.results` directly. The
//   proxy reads OMEGA_OPENBB_TOKEN from the env at process start; if unset
//   and OMEGA_OPENBB_MOCK is also unset, every Step-5 route returns 503 with
//   a structured error body so the UI's red retry banner surfaces a clear
//   "API token not configured" message.
//
//   Token strategy is server-side on purpose: keeping the token in the engine
//   binary's env var (rather than in the React bundle) means it never ships
//   in the JS the browser downloads, and rotating it does not require a UI
//   redeploy. This is the right shape for the omega-terminal -> production
//   GUI cutover at Step 7.
//
//   Per-route default cache TTLs are sized just below each panel's polling
//   cadence so repeat polls within a single second hit the cache and a real
//   OpenBB call only fires when the cache window expires:
//
//     INTEL      pollMs 30000  ttl 25000   (news; slow churn)
//     CURV       pollMs 60000  ttl 50000   (yields; daily-ish)
//     WEI        pollMs  5000  ttl  4000   (index quotes; light)
//     MOV        pollMs  1000  ttl   750   (movers; live)
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
#include "OpenBbProxy.hpp"

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

#include "EngineRegistry.hpp"      // EngineSnapshot + EngineRegistry + g_engines (extern)
#include "OpenPositionRegistry.hpp"// PositionSnapshot + OpenPositionRegistry + g_open_positions (extern, Step 3)
#include "OmegaTradeLedger.hpp"    // omega::OmegaTradeLedger / TradeRecord

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

extern omega::OmegaTradeLedger     g_omegaLedger;
// g_engines and g_open_positions are declared `extern` in their respective
// headers; both are defined in include/globals.hpp inside main.cpp's TU.

namespace omega {

// Step 3 equity anchor. Stored atomically so the equity route reader thread
// and any setter caller (currently init_engines via set_equity_anchor()) are
// safe without a lock. Defaults to 10000.0 -- matches the schema default in
// include/omega_types.hpp so a process that never calls set_equity_anchor()
// still produces a sensible curve.
static std::atomic<double> s_equity_anchor{10000.0};

void set_equity_anchor(double anchor)
{
    if (anchor > 0.0 && std::isfinite(anchor)) {
        s_equity_anchor.store(anchor, std::memory_order_relaxed);
    }
}

} // namespace omega

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

// Step 3: open-position read-API. Walks g_open_positions, which is populated
// by snapshotter callbacks registered in init_engines() (engine_init.hpp).
// Step 3 ships only the HybridGold source; other engines (Tsmom/Donchian/
// EmaPullback/TrendRider/HBI) land in a follow-up. The PositionSnapshot
// struct mirrors the JSON keys in omega-terminal/src/api/types.ts (Position
// interface) -- field names are byte-identical and must be kept in sync if
// either side changes.
static std::string build_positions_json()
{
    const auto positions = g_open_positions.snapshot_all();
    std::string out = "[";
    for (size_t i = 0; i < positions.size(); ++i) {
        const PositionSnapshot& p = positions[i];
        if (i > 0) out += ",";
        out += "{";
        out += "\"symbol\":"         + json_str(p.symbol)         + ",";
        out += "\"side\":"           + json_str(p.side)           + ",";
        out += "\"size\":"           + json_num(p.size)           + ",";
        out += "\"entry\":"          + json_num(p.entry)          + ",";
        out += "\"current\":"        + json_num(p.current)        + ",";
        out += "\"unrealized_pnl\":" + json_num(p.unrealized_pnl) + ",";
        out += "\"mfe\":"            + json_num(p.mfe)            + ",";
        out += "\"mae\":"            + json_num(p.mae)            + ",";
        out += "\"engine\":"         + json_str(p.engine);
        out += "}";
    }
    out += "]";
    return out;
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

// Step 3: equity time-series. Walks g_omegaLedger.snapshot() (closed trades,
// chronologically ordered) and emits a series of (ts, equity) points where
// equity_at_t = s_equity_anchor + sum(net_pnl for trades closed at or before t).
//
// Bucketing:
//   - "1m" / "1h" / "1d" specify the bucket size on the wire; we parse it
//     here and round each trade's exit_ts down to the bucket start. The
//     emitted series is one point per bucket the ledger has activity in.
//   - Default (when interval is omitted) is "1h", which matches the
//     STEP2_OPENER hint that 1h is the live preferred bucket.
//   - "from"/"to" filter on exit_ts (ISO-8601 unix-ms, parsed via the same
//     parse_iso_to_unix_ms helper as the ledger route).
//
// Anchor:
//   - s_equity_anchor defaults to 10000.0; init_engines() should call
//     set_equity_anchor(g_cfg.account_equity) so the absolute equity values
//     match the live account. The series shape is correct regardless.
//
// Limitations:
//   - PARTIAL_1R/PARTIAL_2R rows are recorded in the ledger by
//     handle_closed_trade for the partial-only fast path, so they will
//     contribute to the cumulative equity walk just like full closes. This
//     matches the pattern that "the ledger is the truth" and the
//     panel/equity should reflect every recorded close.
//   - No deduplication by trade id here; OmegaTradeLedger::record already
//     dedups upstream.
static std::string build_equity_json(const std::unordered_map<std::string, std::string>& q)
{
    // ---- parse query: from / to / interval ----
    int64_t from_ms = 0;
    int64_t to_ms   = 0;
    std::string interval = "1h";

    auto it = q.find("from");
    if (it != q.end()) from_ms = parse_iso_to_unix_ms(it->second);
    it = q.find("to");
    if (it != q.end()) to_ms = parse_iso_to_unix_ms(it->second);
    it = q.find("interval");
    if (it != q.end() && !it->second.empty()) interval = it->second;

    int64_t bucket_ms = 60LL * 60LL * 1000LL;            // 1h default
    if      (interval == "1m") bucket_ms =      60LL * 1000LL;
    else if (interval == "1h") bucket_ms = 60LL*60LL * 1000LL;
    else if (interval == "1d") bucket_ms = 24LL*60LL*60LL * 1000LL;
    // Anything unrecognised falls back to 1h silently. The TS side restricts
    // EquityInterval to "1m" | "1h" | "1d" so we do not surface an error.

    const double anchor = s_equity_anchor.load(std::memory_order_relaxed);

    // Walk the ledger in chronological order (snapshot() returns the internal
    // m_trades vector which is appended in record() order, i.e. by exit time).
    const auto trades = g_omegaLedger.snapshot();

    // Aggregate net_pnl into per-bucket buckets. We use a plain vector of
    // (bucket_start_ms, bucket_pnl) so the output order is preserved without
    // a sort step. The vector is small in practice (a few hundred buckets
    // for a multi-month run at 1h cadence) so linear lookup of the most
    // recent bucket is fine.
    struct BucketAcc {
        int64_t bucket_ms;
        double  pnl_in_bucket;
    };
    std::vector<BucketAcc> buckets;
    buckets.reserve(trades.size());

    for (const TradeRecord& tr : trades) {
        const int64_t exit_ms = static_cast<int64_t>(tr.exitTs) * 1000LL;
        if (from_ms != 0 && exit_ms <  from_ms) continue;
        if (to_ms   != 0 && exit_ms >= to_ms)   continue;

        const int64_t bucket_start = (exit_ms / bucket_ms) * bucket_ms;
        const double  net = (tr.net_pnl != 0.0 ? tr.net_pnl : tr.pnl);

        if (!buckets.empty() && buckets.back().bucket_ms == bucket_start) {
            buckets.back().pnl_in_bucket += net;
        } else {
            buckets.push_back(BucketAcc{bucket_start, net});
        }
    }

    // Emit one point per bucket: equity = anchor + cumulative net_pnl up to
    // (and including) this bucket.
    std::string out = "[";
    double running = 0.0;
    for (size_t i = 0; i < buckets.size(); ++i) {
        running += buckets[i].pnl_in_bucket;
        if (i > 0) out += ",";
        out += "{";
        out += "\"ts\":"     + json_int(buckets[i].bucket_ms) + ",";
        out += "\"equity\":" + json_num(anchor + running);
        out += "}";
    }
    out += "]";
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Step 5: OpenBB-backed routes
//
// Each builder converts the panel-facing query parameters into an OpenBB Hub
// path + query, calls OpenBbProxy::get(), and returns the OBBject envelope
// verbatim. The proxy handles cache + token + mock-mode fallbacks; we just
// pick the right route + cadence. status is set in-place so the dispatcher
// can propagate the HTTP code (e.g. 503 when the token is not configured).
// ─────────────────────────────────────────────────────────────────────────────

// INTEL <screen-id>  -> OpenBB /news/world (Step 5 default screen)
//
// The screen-id parameter is parsed but currently maps every value to the
// same /news/world call. Step 6 onward can branch into sector / earnings /
// macro screens by switching on the screen-id and composing different OpenBB
// calls (or aggregating multiple). Keeping the parameter wired now means the
// UI side does not change when we expand the engine-side mapping.
static std::string build_intel_json(
    const std::unordered_map<std::string, std::string>& q,
    int& status)
{
    (void)q.count("screen");  // accepted but ignored for v1
    auto it = q.find("limit");
    int limit = 20;
    if (it != q.end()) {
        try { limit = std::stoi(it->second); } catch (...) { limit = 20; }
    }
    if (limit < 1)  limit = 1;
    if (limit > 50) limit = 50;

    char qbuf[64];
    std::snprintf(qbuf, sizeof(qbuf), "limit=%d", limit);

    const OpenBbResult r = OpenBbProxy::instance().get(
        "news/world", qbuf, /*ttl_ms=*/25000);
    status = r.status;
    return r.body;
}

// CURV <region>  -> OpenBB /fixedincome/government/treasury_rates
//
// region:
//   "US" (default): federal_reserve provider. Fully wired in v1.
//   "EU" / "JP":    not yet wired -- returns 200 with a structured note so
//                   the UI can show a "region not yet supported" message
//                   instead of a hard error. Step 6 can wire ECB / BoJ via
//                   their respective OpenBB providers.
static std::string build_curv_json(
    const std::unordered_map<std::string, std::string>& q,
    int& status)
{
    std::string region = "US";
    auto it = q.find("region");
    if (it != q.end() && !it->second.empty()) region = it->second;
    // Normalise to upper-case.
    for (char& c : region) {
        if (c >= 'a' && c <= 'z') c -= 32;
    }

    if (region == "US") {
        const OpenBbResult r = OpenBbProxy::instance().get(
            "fixedincome/government/treasury_rates",
            "provider=federal_reserve",
            /*ttl_ms=*/50000);
        status = r.status;
        return r.body;
    }

    // EU / JP / other: Step-6 follow-up. Surface a friendly empty envelope.
    status = 200;
    std::string out = "{\"results\":[],\"provider\":\"omega-stub\",";
    out += "\"warnings\":[{\"message\":\"region '";
    out += region;
    out += "' not yet wired -- US is the only fully-wired CURV region in ";
    out += "Step 5; Step 6 adds ECB / BoJ providers.\"}],";
    out += "\"chart\":null,\"extra\":{\"region\":";
    out += json_str(region);
    out += "}}";
    return out;
}

// WEI <region>  -> OpenBB /equity/price/quote for a curated symbol list
//
// region:
//   "US"    (default): SPY,QQQ,DIA,IWM,VTI       (broad-market index ETFs)
//   "EU":              VGK,EZU,FEZ,EWG,EWU
//   "ASIA"  / "AS":    EWJ,FXI,EWY,EWT,EWA
//   "WORLD" / "W":     VT,ACWI,VEA,VWO,EFA
//   anything else: forwarded as a literal symbol list (comma-separated) so
//                  power users can type `WEI AAPL,MSFT,GOOGL` from the
//                  command bar.
//
// provider=yfinance: free-tier on OpenBB Hub. If the user has paid providers
// configured on their OpenBB account they can override via ?provider=fmp etc.
static std::string build_wei_json(
    const std::unordered_map<std::string, std::string>& q,
    int& status)
{
    std::string region = "US";
    auto it = q.find("region");
    if (it != q.end() && !it->second.empty()) region = it->second;

    std::string symbols;
    {
        std::string up = region;
        for (char& c : up) {
            if (c >= 'a' && c <= 'z') c -= 32;
        }
        if      (up == "US")                 symbols = "SPY,QQQ,DIA,IWM,VTI";
        else if (up == "EU")                 symbols = "VGK,EZU,FEZ,EWG,EWU";
        else if (up == "ASIA" || up == "AS") symbols = "EWJ,FXI,EWY,EWT,EWA";
        else if (up == "WORLD" || up == "W") symbols = "VT,ACWI,VEA,VWO,EFA";
        else                                 symbols = region;  // literal list
    }

    std::string provider = "yfinance";
    auto pit = q.find("provider");
    if (pit != q.end() && !pit->second.empty()) provider = pit->second;

    std::string qs = "symbol=";
    qs += symbols;
    qs += "&provider=";
    qs += provider;

    const OpenBbResult r = OpenBbProxy::instance().get(
        "equity/price/quote", qs, /*ttl_ms=*/4000);
    status = r.status;
    return r.body;
}

// MOV <universe>  -> OpenBB /equity/discovery/<universe>
//
// universe ∈ {active, gainers, losers}. Anything else is coerced to "active"
// so a typo does not 502.
static std::string build_mov_json(
    const std::unordered_map<std::string, std::string>& q,
    int& status)
{
    std::string universe = "active";
    auto it = q.find("universe");
    if (it != q.end() && !it->second.empty()) universe = it->second;
    // Normalise.
    for (char& c : universe) {
        if (c >= 'A' && c <= 'Z') c += 32;
    }
    if (universe != "active" && universe != "gainers" && universe != "losers") {
        universe = "active";
    }

    std::string provider = "yfinance";
    auto pit = q.find("provider");
    if (pit != q.end() && !pit->second.empty()) provider = pit->second;

    std::string route = "equity/discovery/";
    route += universe;

    std::string qs = "provider=";
    qs += provider;

    const OpenBbResult r = OpenBbProxy::instance().get(
        route, qs, /*ttl_ms=*/750);
    status = r.status;
    return r.body;
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
    // INADDR_ANY (all interfaces) -- matches src/gui/OmegaTelemetryServer.cpp
    // (which binds the existing :7779 / :7780 publicly the same way) so the
    // omega-terminal UI is reachable from a browser at http://VPS_IP:7781/.
    //
    // Security: this exposes the JSON read-API publicly with no auth, same
    // as the legacy telemetry server. To restrict source IPs, add a Windows
    // Firewall rule on port 7781 (out of scope for this binary). The original
    // 127.0.0.1-only design (Step 2) assumed Vite proxy from a developer Mac
    // would be the only consumer; serving the UI from this same server made
    // that assumption obsolete (2026-05-01).
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(static_cast<u_short>(port));

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(server_fd_, 16) == SOCKET_ERROR) {
        std::cerr << "[OmegaApi] bind/listen failed on port " << port << "\n";
        closesocket(server_fd_);
        server_fd_ = INVALID_SOCKET;
        return;
    }

    std::cout << "[OmegaApi] port " << port << " (all interfaces)\n" << std::flush;

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
        else if (path == "/api/v1/omega/intel") {
            body = build_intel_json(q, status);
        }
        else if (path == "/api/v1/omega/curv") {
            body = build_curv_json(q, status);
        }
        else if (path == "/api/v1/omega/wei") {
            body = build_wei_json(q, status);
        }
        else if (path == "/api/v1/omega/mov") {
            body = build_mov_json(q, status);
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
        else if (status == 503) status_text = "Service Unavailable";
        else if (status == 504) status_text = "Gateway Timeout";
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

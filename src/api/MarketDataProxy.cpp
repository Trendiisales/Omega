// ==============================================================================
// MarketDataProxy.cpp
//
// libcurl-based HTTPS client for free market-data providers (Yahoo Finance +
// FRED). See MarketDataProxy.hpp for the full design notes. Step 7 of the
// Omega Terminal build; replaces the Step-5/6 OpenBbProxy after the OpenBB
// Hub was found to return HTTP 404 (no public REST endpoint exists).
//
// This translation unit is gated on `#ifndef OMEGA_BACKTEST` so the backtester
// targets stay free of the libcurl dependency. The matching CMake side links
// CURL::libcurl into the Omega target only.
//
// Endpoint base URLs:
//   https://query1.finance.yahoo.com/v7/finance/quote
//   https://query1.finance.yahoo.com/v8/finance/chart/<sym>
//   https://query2.finance.yahoo.com/v7/finance/options/<sym>
//   https://query1.finance.yahoo.com/v10/finance/quoteSummary/<sym>
//   https://query1.finance.yahoo.com/v1/finance/search
//   https://query1.finance.yahoo.com/v1/finance/screener/predefined/saved
//   https://api.stlouisfed.org/fred/series/observations
//
// Authentication:
//   Yahoo Finance: none. We send a browser-like User-Agent because Yahoo
//   sometimes 403s on default libcurl UAs.
//
//   FRED: api_key=<OMEGA_FRED_KEY> as a query param. The free key is one
//   form-fill at fredaccount.stlouisfed.org/apikeys. If the variable is unset
//   the /curv route returns a structured 503; other routes are unaffected.
//
// Mock mode:
//   OMEGA_MARKETDATA_MOCK=1 bypasses the network and returns canned synthetic
//   JSON shaped like the legacy OpenBB OBBject (so the route reshape code in
//   OmegaApiServer.cpp does not branch on mock-vs-real). The mock substrate
//   is the same per-route dataset the Step-5/6 OpenBbProxy shipped, lifted
//   verbatim, just gated under the new env var.
//
// Reshape contract:
//   Every fetcher in this TU returns OpenBB-OBBject-shaped JSON:
//
//     {"results": [...], "provider": "yahoo"|"fred"|"mock"|"omega-stub",
//      "warnings": null|[...], "chart": null, "extra": {...}}
//
//   so existing parsers in OmegaApiServer.cpp and WatchScheduler.cpp keep
//   working without changes. Field names inside `results[]` rows match the
//   Step-5/6 envelope (symbol, last_price, change_percent, volume, etc.) --
//   each fetcher reshapes the upstream's native field set to ours.
//
// JSON parsing:
//   Per the engine-side convention (see WatchScheduler.cpp comment "tiny
//   brace/bracket-aware string scanners"), we never pull a third-party JSON
//   library. The handful of helpers in the anonymous namespace below
//   (json_find_value_of, json_extract_string, json_extract_number,
//   json_extract_object, json_extract_array, json_for_each_object,
//   json_for_each_number) are sufficient for what we need to extract from
//   Yahoo + FRED responses. Yahoo's `{"raw": <num>, "fmt": "<s>"}` pattern
//   is handled by json_extract_number transparently.
//
// Windows include order:
//   <winsock2.h> + <ws2tcpip.h> MUST come BEFORE <curl/curl.h>. libcurl's
//   header references curl_socket_t / sockaddr / fd_set which are typedef'd
//   in winsock2.h on Windows; including curl.h before winsock2.h gives a
//   cascade of MSVC C2061 / C2079 / C3646 errors. The repo-level CMake
//   defines _WINSOCKAPI_ globally so <windows.h> doesn't drag in the older
//   winsock.h. Same idiom appears in OmegaApiServer.cpp and the retired
//   OpenBbProxy.cpp.
// ==============================================================================

#ifndef OMEGA_BACKTEST

#include "MarketDataProxy.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace omega {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

constexpr const char* kYahooQuote   = "https://query1.finance.yahoo.com/v7/finance/quote";
constexpr const char* kYahooChart   = "https://query1.finance.yahoo.com/v8/finance/chart/";
constexpr const char* kYahooOptions = "https://query2.finance.yahoo.com/v7/finance/options/";
constexpr const char* kYahooSummary = "https://query1.finance.yahoo.com/v10/finance/quoteSummary/";
constexpr const char* kYahooSearch  = "https://query1.finance.yahoo.com/v1/finance/search";
constexpr const char* kYahooScreener= "https://query1.finance.yahoo.com/v1/finance/screener/predefined/saved";
constexpr const char* kFredObs      = "https://api.stlouisfed.org/fred/series/observations";

// Yahoo Finance occasionally 403s on default libcurl UAs. A browser-like
// header passes the gate. FRED is happy with anything; we keep our own UA
// for FRED to make the access logs more legible.
constexpr const char* kYahooUserAgent =
    "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
constexpr const char* kFredUserAgent =
    "User-Agent: Omega-Terminal/1.0 (+marketdata)";

// ─────────────────────────────────────────────────────────────────────────────
// curl_global_init guard
// ─────────────────────────────────────────────────────────────────────────────

std::once_flag g_curl_init_flag;

void ensure_curl_global_init()
{
    std::call_once(g_curl_init_flag, []() {
        const CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (rc != CURLE_OK) {
            std::cerr << "[MarketDataProxy] curl_global_init failed: "
                      << curl_easy_strerror(rc) << "\n";
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Generic helpers
// ─────────────────────────────────────────────────────────────────────────────

int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch()).count();
}

size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    std::string* body = static_cast<std::string*>(userdata);
    const size_t bytes = size * nmemb;
    body->append(ptr, bytes);
    return bytes;
}

std::string env_or_empty(const char* name)
{
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

bool env_truthy(const char* name)
{
    const std::string v = env_or_empty(name);
    if (v.empty()) return false;
    return (v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "YES");
}

// Minimal URL encoder for a few characters that show up in ticker symbols
// passed by the user (^ in indices like ^GSPC; / in FX pairs like EUR/USD).
// We keep alphanumerics, '-', '_', '.', '~', and ',' (Yahoo accepts comma
// in symbols=...). Everything else is %-encoded. RFC 3986 / unreserved set,
// with comma kept as a literal because Yahoo treats commas as the multi-
// symbol separator in ?symbols=.
std::string url_encode(const std::string& s, bool keep_comma = true)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        const bool unreserved =
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved || (keep_comma && c == ',')) {
            out.push_back(static_cast<char>(c));
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<int>(c));
            out += buf;
        }
    }
    return out;
}

// JSON-escape a string for embedding in our reshape output. Same rules used
// by WatchScheduler.cpp's json_escape; duplicated here for TU isolation.
std::string json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<int>(c) & 0xFF);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// Pull a query-string field's value out of a "k1=v1&k2=v2" string. Returns
// empty on miss. Does NOT decode percent-encoding -- Yahoo and FRED don't
// require us to round-trip arbitrary query input.
std::string qs_get(const std::string& query, const char* key)
{
    std::string needle = key;
    needle += '=';
    size_t pos = 0;
    while (pos < query.size()) {
        if (query.compare(pos, needle.size(), needle) == 0 &&
            (pos == 0 || query[pos - 1] == '&')) {
            const size_t start = pos + needle.size();
            const size_t end   = query.find('&', start);
            return query.substr(start,
                                (end == std::string::npos)
                                    ? std::string::npos
                                    : end - start);
        }
        const size_t amp = query.find('&', pos);
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return std::string();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tiny brace/bracket-aware JSON helpers (no third-party JSON lib).
// Same idiom used by WatchScheduler.cpp.
// ─────────────────────────────────────────────────────────────────────────────

size_t json_skip_ws(const std::string& s, size_t i)
{
    while (i < s.size()) {
        const char c = s[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        ++i;
    }
    return i;
}

// Find the position of the value associated with "key". Returns the index of
// the first non-whitespace char AFTER the colon, or string::npos on miss.
// Skips over JSON strings cleanly so a key that lives inside a string value
// (e.g. {"text":"...key:..."}) does not match.
size_t json_find_value_of(const std::string& body,
                          const std::string& key,
                          size_t start = 0)
{
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    size_t i = start;
    bool in_str = false;
    bool esc    = false;
    while (i < body.size()) {
        const char c = body[i];
        if (in_str) {
            if (esc)       { esc = false; ++i; continue; }
            if (c == '\\') { esc = true;  ++i; continue; }
            if (c == '"')  { in_str = false; ++i; continue; }
            ++i;
            continue;
        }
        if (c == '"') {
            // Possible key match starts here.
            if (body.compare(i, needle.size(), needle) == 0) {
                size_t after = i + needle.size();
                after = json_skip_ws(body, after);
                if (after < body.size() && body[after] == ':') {
                    after = json_skip_ws(body, after + 1);
                    return after;
                }
            }
            in_str = true;
            ++i;
            continue;
        }
        ++i;
    }
    return std::string::npos;
}

// Read a JSON string value at `pos` (must point at the opening quote). Stores
// the decoded content in `out` and advances `pos` to just past the closing
// quote. Returns false on miss.
bool json_read_string_at(const std::string& body, size_t& pos, std::string& out)
{
    out.clear();
    if (pos >= body.size() || body[pos] != '"') return false;
    ++pos;
    while (pos < body.size()) {
        const char c = body[pos];
        if (c == '\\') {
            if (pos + 1 >= body.size()) return false;
            const char nc = body[pos + 1];
            switch (nc) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'u':  // \uXXXX -- pass through as-is (we don't need full unicode here)
                    if (pos + 5 < body.size()) {
                        out.append(body, pos, 6);
                        pos += 6;
                        continue;
                    }
                    return false;
                default:   out.push_back(nc); break;
            }
            pos += 2;
            continue;
        }
        if (c == '"') { ++pos; return true; }
        out.push_back(c);
        ++pos;
    }
    return false;
}

// Read a JSON number at `pos`. Returns the value or NaN on miss. Advances
// `pos` past the parsed number.
double json_read_number_at(const std::string& body, size_t& pos)
{
    const char* p = body.c_str() + pos;
    char* end = nullptr;
    const double v = std::strtod(p, &end);
    if (end == p) return std::nan("");
    pos += static_cast<size_t>(end - p);
    return v;
}

// Read whatever value sits at `pos`: number, "raw"-wrapped object, JSON
// string number ("4.20"), or null. Returns NaN for the latter two when the
// content doesn't parse, otherwise the decoded number. This is the workhorse
// for Yahoo's `{"raw":<num>,"fmt":"<s>"}` and FRED's `"value":"4.20"` shapes.
double json_read_any_number_at(const std::string& body, size_t pos)
{
    pos = json_skip_ws(body, pos);
    if (pos >= body.size()) return std::nan("");
    const char c = body[pos];
    if (c == '"') {
        // Quoted number -- FRED returns its `value` field as a string. The
        // sentinel "." marks "missing data" in FRED's series.
        std::string s;
        if (!json_read_string_at(body, pos, s)) return std::nan("");
        if (s.empty() || s == ".") return std::nan("");
        char* end = nullptr;
        const double v = std::strtod(s.c_str(), &end);
        if (end == s.c_str()) return std::nan("");
        return v;
    }
    if (c == '{') {
        // Yahoo's {"raw": <num>, "fmt": "<s>"} -- pull `raw`.
        const size_t inner = json_find_value_of(body, "raw", pos);
        if (inner == std::string::npos) return std::nan("");
        size_t p2 = inner;
        return json_read_any_number_at(body, p2);
    }
    if (c == 'n') {
        // null
        return std::nan("");
    }
    return json_read_number_at(body, pos);
}

// Convenience: pull a numeric field by key. Returns NaN on miss.
double json_extract_number(const std::string& body,
                           const char* key,
                           size_t start = 0)
{
    const size_t pos = json_find_value_of(body, key, start);
    if (pos == std::string::npos) return std::nan("");
    return json_read_any_number_at(body, pos);
}

// Convenience: pull a string field by key. Returns empty on miss.
std::string json_extract_string(const std::string& body,
                                const char* key,
                                size_t start = 0)
{
    const size_t pos = json_find_value_of(body, key, start);
    if (pos == std::string::npos) return std::string();
    if (pos >= body.size() || body[pos] != '"') return std::string();
    size_t p = pos;
    std::string out;
    if (!json_read_string_at(body, p, out)) return std::string();
    return out;
}

// Read the substring of an object value (including the surrounding {}). The
// scanner is brace/bracket-aware and skips over JSON strings cleanly.
std::string json_read_object_at(const std::string& body, size_t pos)
{
    pos = json_skip_ws(body, pos);
    if (pos >= body.size() || body[pos] != '{') return std::string();
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    const size_t start = pos;
    while (pos < body.size()) {
        const char c = body[pos];
        if (in_str) {
            if (esc)       { esc = false; ++pos; continue; }
            if (c == '\\') { esc = true;  ++pos; continue; }
            if (c == '"')  { in_str = false; ++pos; continue; }
            ++pos;
            continue;
        }
        if (c == '"') { in_str = true; ++pos; continue; }
        if (c == '{') ++depth;
        else if (c == '}') {
            --depth;
            if (depth == 0) return body.substr(start, pos - start + 1);
        }
        ++pos;
    }
    return std::string();
}

// Read the substring of an array value (including the surrounding []).
std::string json_read_array_at(const std::string& body, size_t pos)
{
    pos = json_skip_ws(body, pos);
    if (pos >= body.size() || body[pos] != '[') return std::string("[]");
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    const size_t start = pos;
    while (pos < body.size()) {
        const char c = body[pos];
        if (in_str) {
            if (esc)       { esc = false; ++pos; continue; }
            if (c == '\\') { esc = true;  ++pos; continue; }
            if (c == '"')  { in_str = false; ++pos; continue; }
            ++pos;
            continue;
        }
        if (c == '"') { in_str = true; ++pos; continue; }
        if (c == '[') ++depth;
        else if (c == ']') {
            --depth;
            if (depth == 0) return body.substr(start, pos - start + 1);
        }
        ++pos;
    }
    return std::string("[]");
}

// Extract the {...} value associated with a key. Empty on miss.
std::string json_extract_object(const std::string& body,
                                const char* key,
                                size_t start = 0)
{
    const size_t pos = json_find_value_of(body, key, start);
    if (pos == std::string::npos) return std::string();
    return json_read_object_at(body, pos);
}

// Extract the [...] value associated with a key. "[]" on miss.
std::string json_extract_array(const std::string& body,
                               const char* key,
                               size_t start = 0)
{
    const size_t pos = json_find_value_of(body, key, start);
    if (pos == std::string::npos) return std::string("[]");
    return json_read_array_at(body, pos);
}

// Iterate top-level objects in an array substring (the kind json_extract_array
// returns -- starts with '[' and ends with ']'). Calls cb(obj_substring) for
// each top-level object. Skips whitespace + commas between objects.
template <class Cb>
void json_for_each_object(const std::string& arr_with_brackets, Cb&& cb)
{
    if (arr_with_brackets.size() < 2) return;
    if (arr_with_brackets.front() != '[' || arr_with_brackets.back() != ']') return;
    const size_t end = arr_with_brackets.size() - 1;
    size_t i = 1;
    while (i < end) {
        i = json_skip_ws(arr_with_brackets, i);
        while (i < end && arr_with_brackets[i] == ',') {
            ++i;
            i = json_skip_ws(arr_with_brackets, i);
        }
        if (i >= end) break;
        if (arr_with_brackets[i] != '{') break;
        size_t depth = 0;
        bool in_str = false;
        bool esc = false;
        const size_t start = i;
        for (; i < end; ++i) {
            const char c = arr_with_brackets[i];
            if (in_str) {
                if (esc)       { esc = false; continue; }
                if (c == '\\') { esc = true;  continue; }
                if (c == '"')  in_str = false;
                continue;
            }
            if (c == '"') { in_str = true; continue; }
            if (c == '{') ++depth;
            else if (c == '}') {
                --depth;
                if (depth == 0) { ++i; break; }
            }
        }
        cb(arr_with_brackets.substr(start, i - start));
    }
}

// Iterate top-level numeric values in an array substring. Yahoo's chart API
// returns `timestamp: [ ... ]` and `quote.open: [ ... ]` as flat number arrays.
template <class Cb>
void json_for_each_number(const std::string& arr_with_brackets, Cb&& cb)
{
    if (arr_with_brackets.size() < 2) return;
    if (arr_with_brackets.front() != '[' || arr_with_brackets.back() != ']') return;
    const size_t end = arr_with_brackets.size() - 1;
    size_t i = 1;
    while (i < end) {
        i = json_skip_ws(arr_with_brackets, i);
        while (i < end && arr_with_brackets[i] == ',') {
            ++i;
            i = json_skip_ws(arr_with_brackets, i);
        }
        if (i >= end) break;
        // null -> NaN
        if (i + 3 < arr_with_brackets.size() &&
            arr_with_brackets.compare(i, 4, "null") == 0) {
            cb(std::nan(""));
            i += 4;
            continue;
        }
        size_t pre = i;
        const double v = json_read_number_at(arr_with_brackets, i);
        if (i == pre) {
            // Non-numeric token; bail to avoid an infinite loop.
            break;
        }
        cb(v);
    }
}

// Format a double for embedding in our reshape JSON. Uses %.6g for general
// numbers; NaN and infinity render as `null` so JSON.parse stays valid.
std::string fmt_num(double v)
{
    if (!std::isfinite(v)) return "null";
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return std::string(buf);
}

// Format an unsigned epoch-second timestamp as an ISO-8601 string in UTC.
std::string fmt_iso_utc(int64_t epoch_s)
{
    if (epoch_s <= 0) return std::string();
    std::time_t t = static_cast<std::time_t>(epoch_s);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                  tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    return std::string(buf);
}

// Format an epoch-second timestamp as YYYY-MM-DD in UTC.
std::string fmt_date_utc(int64_t epoch_s)
{
    if (epoch_s <= 0) return std::string();
    std::time_t t = static_cast<std::time_t>(epoch_s);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday);
    return std::string(buf);
}

// Wrap a `results_json` array body into our standard envelope. provider is
// one of "yahoo" / "fred" / "mock" / "omega-stub". warnings_json may be
// "null" or a JSON array literal.
std::string wrap_envelope(const std::string& results_json,
                          const char*        provider,
                          const std::string& warnings_json = "null",
                          bool               mock_extra = false)
{
    std::string out = "{\"results\":";
    out += results_json;
    out += ",\"provider\":\"";
    out += provider;
    out += "\",\"warnings\":";
    out += warnings_json;
    out += ",\"chart\":null,\"extra\":{";
    if (mock_extra) out += "\"mock\":true";
    out += "}}";
    return out;
}

// Build a one-element warnings array literal. Used for partial-coverage
// notices on EE / FA / KEY.
std::string warning_one(const std::string& msg)
{
    return std::string("[{\"message\":\"") + json_escape(msg) + "\"}]";
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Singleton + ctor
// ─────────────────────────────────────────────────────────────────────────────

MarketDataProxy& MarketDataProxy::instance()
{
    static MarketDataProxy s_instance;
    return s_instance;
}

MarketDataProxy::MarketDataProxy()
    : fred_key_(env_or_empty("OMEGA_FRED_KEY"))
    , mock_(env_truthy("OMEGA_MARKETDATA_MOCK"))
{
    if (mock_) {
        std::cout << "[MarketDataProxy] mock mode (OMEGA_MARKETDATA_MOCK=1) -- "
                  << "synthetic data, no network calls\n";
    } else {
        std::cout << "[MarketDataProxy] live mode -- Yahoo Finance + FRED ("
                  << (fred_key_.empty() ? "no FRED key; /curv will return 503"
                                        : "FRED key configured")
                  << ")\n";
        ensure_curl_global_init();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// upstream_provider_for_route
// ─────────────────────────────────────────────────────────────────────────────

const char* MarketDataProxy::upstream_provider_for_route(const std::string& route)
{
    if (route == "fixedincome/government/treasury_rates") return "fred";
    if (route == "news/world"                   ||
        route == "news/company"                 ||
        route == "equity/price/quote"           ||
        route == "equity/price/historical"      ||
        route == "equity/discovery/active"      ||
        route == "equity/discovery/gainers"     ||
        route == "equity/discovery/losers"      ||
        route == "derivatives/options/chains"   ||
        route == "equity/fundamental/income"    ||
        route == "equity/fundamental/balance"   ||
        route == "equity/fundamental/cash"      ||
        route == "equity/fundamental/key_metrics" ||
        route == "equity/fundamental/multiples" ||
        route == "equity/fundamental/dividends" ||
        route == "equity/estimates/consensus"   ||
        route == "equity/estimates/surprise"    ||
        route == "equity/profile"               ||
        route == "currency/price/quote"         ||
        route == "crypto/price/quote") {
        return "yahoo";
    }
    return "omega-stub";
}

// ─────────────────────────────────────────────────────────────────────────────
// Public get()
// ─────────────────────────────────────────────────────────────────────────────

MarketDataResult MarketDataProxy::get(const std::string& route,
                                      const std::string& query,
                                      int64_t            ttl_ms)
{
    // Cache key is the route + query (post-reshape envelope is what we
    // store, so callers see byte-identical bodies).
    std::string key = route;
    if (!query.empty()) {
        key += "?";
        key += query;
    }

    if (ttl_ms > 0) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            const int64_t age = now_ms() - it->second.stored_ms;
            if (age <= it->second.ttl_ms) {
                MarketDataResult r = it->second.result;
                r.from_cache = true;
                lru_.remove(key);
                lru_.push_front(key);
                return r;
            }
            cache_.erase(it);
            lru_.remove(key);
        }
    }

    MarketDataResult r;
    if (mock_) {
        r = fetch_mock(route, query);
    } else {
        r = dispatch(route, query);
    }

    if (ttl_ms > 0 && r.status >= 200 && r.status < 300) {
        std::lock_guard<std::mutex> lk(mu_);
        cache_[key] = CacheEntry{r, now_ms(), ttl_ms};
        lru_.push_front(key);
        while (lru_.size() > kMaxCacheEntries) {
            const std::string victim = lru_.back();
            lru_.pop_back();
            cache_.erase(victim);
        }
    }

    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// dispatch -- route -> per-provider fetcher
// ─────────────────────────────────────────────────────────────────────────────

MarketDataResult MarketDataProxy::dispatch(const std::string& route,
                                           const std::string& query)
{
    if (route == "news/world")                          return yahoo_search_news(query, /*per_symbol=*/false);
    if (route == "news/company")                        return yahoo_search_news(query, /*per_symbol=*/true);
    if (route == "fixedincome/government/treasury_rates") return fred_treasury_curve(query);
    if (route == "equity/price/quote")                  return yahoo_quote(query);
    if (route == "equity/price/historical")             return yahoo_chart(query);
    if (route == "equity/discovery/active"  ||
        route == "equity/discovery/gainers" ||
        route == "equity/discovery/losers")             return yahoo_screener(route);
    if (route == "derivatives/options/chains")          return yahoo_options(query);
    if (route == "equity/fundamental/income"      ||
        route == "equity/fundamental/balance"     ||
        route == "equity/fundamental/cash"        ||
        route == "equity/fundamental/key_metrics" ||
        route == "equity/fundamental/multiples"   ||
        route == "equity/estimates/consensus"     ||
        route == "equity/estimates/surprise"      ||
        route == "equity/profile")                      return yahoo_quote_summary(route, query);
    if (route == "equity/fundamental/dividends")        return yahoo_dividends(query);
    if (route == "currency/price/quote")                return yahoo_fx_or_crypto(query, "fx");
    if (route == "crypto/price/quote")                  return yahoo_fx_or_crypto(query, "crypto");

    // Fallthrough -- preserved from Step 5/6.
    return MarketDataResult{
        200,
        wrap_envelope("[]", "omega-stub"),
        false
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// http_get -- libcurl GET with custom headers
// ─────────────────────────────────────────────────────────────────────────────

MarketDataResult MarketDataProxy::http_get(const std::string& url,
                                           const std::vector<std::string>& extra_headers)
{
    ensure_curl_global_init();

    std::string body;
    long        http_code = 0;

    CURL* curl = curl_easy_init();
    if (!curl) {
        return MarketDataResult{504, R"({"error":"curl_init_failed"})", false};
    }

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    for (const auto& h : extra_headers) {
        headers = curl_slist_append(headers, h.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // accept gzip/deflate

    const CURLcode rc = curl_easy_perform(curl);

    if (rc != CURLE_OK) {
        std::string err = R"({"error":"curl_perform_failed","detail":")";
        const char* cerr = curl_easy_strerror(rc);
        for (const char* p = cerr; *p; ++p) {
            if (*p == '"' || *p == '\\') err += '\'';
            else                          err += *p;
        }
        err += "\"}";
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return MarketDataResult{504, err, false};
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return MarketDataResult{static_cast<int>(http_code), body, false};
}

// ─────────────────────────────────────────────────────────────────────────────
// yahoo_quote -- /v7/finance/quote?symbols=...
//
// Reshape: quoteResponse.result[].{symbol, longName/shortName,
// regularMarketPrice, regularMarketChange, regularMarketChangePercent, ...}
// -> {symbol, name, last_price, change, change_percent, volume, prev_close,
//     open, high, low, bid, ask}.
// ─────────────────────────────────────────────────────────────────────────────

MarketDataResult MarketDataProxy::yahoo_quote(const std::string& query)
{
    std::string symbols = qs_get(query, "symbol");
    if (symbols.empty()) symbols = qs_get(query, "symbols");
    if (symbols.empty()) symbols = "SPY,QQQ,DIA,IWM";

    std::string url = kYahooQuote;
    url += "?symbols=";
    url += url_encode(symbols, /*keep_comma=*/true);

    MarketDataResult raw = http_get(url, { kYahooUserAgent });
    if (raw.status < 200 || raw.status >= 300) {
        return MarketDataResult{
            raw.status,
            wrap_envelope("[]", "yahoo",
                          warning_one("Yahoo quote upstream HTTP error")),
            false
        };
    }

    // quoteResponse.result is an array of per-symbol objects.
    const std::string qr = json_extract_object(raw.body, "quoteResponse");
    const std::string arr = json_extract_array(qr, "result");

    std::string results = "[";
    bool first = true;
    json_for_each_object(arr, [&](const std::string& obj) {
        const std::string sym = json_extract_string(obj, "symbol");
        if (sym.empty()) return;
        std::string name = json_extract_string(obj, "longName");
        if (name.empty()) name = json_extract_string(obj, "shortName");
        if (name.empty()) name = sym;

        const double last  = json_extract_number(obj, "regularMarketPrice");
        const double chg   = json_extract_number(obj, "regularMarketChange");
        const double pct   = json_extract_number(obj, "regularMarketChangePercent");
        const double vol   = json_extract_number(obj, "regularMarketVolume");
        const double pc    = json_extract_number(obj, "regularMarketPreviousClose");
        const double op    = json_extract_number(obj, "regularMarketOpen");
        const double hi    = json_extract_number(obj, "regularMarketDayHigh");
        const double lo    = json_extract_number(obj, "regularMarketDayLow");
        const double bid   = json_extract_number(obj, "bid");
        const double ask   = json_extract_number(obj, "ask");

        if (!first) results += ",";
        first = false;
        results += "{\"symbol\":\"" + json_escape(sym) + "\"";
        results += ",\"name\":\"" + json_escape(name) + "\"";
        results += ",\"last_price\":" + fmt_num(last);
        results += ",\"change\":" + fmt_num(chg);
        results += ",\"change_percent\":" + fmt_num(pct);
        results += ",\"volume\":" + fmt_num(vol);
        results += ",\"prev_close\":" + fmt_num(pc);
        results += ",\"open\":" + fmt_num(op);
        results += ",\"high\":" + fmt_num(hi);
        results += ",\"low\":" + fmt_num(lo);
        results += ",\"bid\":" + fmt_num(bid);
        results += ",\"ask\":" + fmt_num(ask);
        results += "}";
    });
    results += "]";

    return MarketDataResult{200, wrap_envelope(results, "yahoo"), false};
}

// ─────────────────────────────────────────────────────────────────────────────
// yahoo_chart -- /v8/finance/chart/<sym>?interval=...&range=...
//
// Reshape: chart.result[0].{timestamp[], indicators.quote[0].{open,high,low,
// close,volume}, indicators.adjclose[0].adjclose} -> [{date,open,high,low,
// close,volume,adj_close}, ...]
// ─────────────────────────────────────────────────────────────────────────────

MarketDataResult MarketDataProxy::yahoo_chart(const std::string& query)
{
    std::string sym = qs_get(query, "symbol");
    if (sym.empty()) sym = "SPY";

    // BarInterval mapping. Yahoo: 1m,2m,5m,15m,30m,60m,90m,1h,1d,5d,1wk,1mo,3mo
    std::string ts_interval = qs_get(query, "interval");
    if (ts_interval.empty()) ts_interval = "1d";
    if (ts_interval == "4h") ts_interval = "1h";   // Yahoo has no native 4h
    if (ts_interval == "1W") ts_interval = "1wk";
    if (ts_interval == "1M") ts_interval = "1mo";

    // If start_date/end_date provided, prefer period1/period2; else use range.
    const std::string start_date = qs_get(query, "start_date");
    const std::string end_date   = qs_get(query, "end_date");

    std::string url = kYahooChart;
    url += url_encode(sym, /*keep_comma=*/false);
    url += "?interval=";
    url += url_encode(ts_interval, false);
    if (!start_date.empty() || !end_date.empty()) {
        // Best-effort: parse "YYYY-MM-DD" to epoch seconds. If parse fails we
        // fall back to range=3mo so the panel still renders.
        auto parse_date = [](const std::string& s) -> int64_t {
            if (s.size() < 10) return 0;
            int y, m, d;
            if (std::sscanf(s.c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3) return 0;
            std::tm tm_utc{};
            tm_utc.tm_year = y - 1900;
            tm_utc.tm_mon  = m - 1;
            tm_utc.tm_mday = d;
#ifdef _WIN32
            return static_cast<int64_t>(_mkgmtime(&tm_utc));
#else
            return static_cast<int64_t>(timegm(&tm_utc));
#endif
        };
        int64_t p1 = parse_date(start_date);
        int64_t p2 = parse_date(end_date);
        if (p1 > 0 || p2 > 0) {
            char buf[64];
            if (p1 > 0) {
                std::snprintf(buf, sizeof(buf), "&period1=%lld",
                              static_cast<long long>(p1));
                url += buf;
            }
            if (p2 > 0) {
                std::snprintf(buf, sizeof(buf), "&period2=%lld",
                              static_cast<long long>(p2));
                url += buf;
            }
        } else {
            url += "&range=3mo";
        }
    } else {
        url += "&range=3mo";
    }

    MarketDataResult raw = http_get(url, { kYahooUserAgent });
    if (raw.status < 200 || raw.status >= 300) {
        return MarketDataResult{
            raw.status,
            wrap_envelope("[]", "yahoo",
                          warning_one("Yahoo chart upstream HTTP error")),
            false
        };
    }

    // chart.result[0].timestamp[]
    const std::string chart   = json_extract_object(raw.body, "chart");
    const std::string result  = json_extract_array(chart, "result");

    // The result array always has one element for a single-symbol chart query.
    std::vector<int64_t> ts;
    std::vector<double>  open, high, low, close, volume, adjclose;
    json_for_each_object(result, [&](const std::string& r0) {
        const std::string ts_arr = json_extract_array(r0, "timestamp");
        json_for_each_number(ts_arr, [&](double v) {
            ts.push_back(static_cast<int64_t>(v));
        });
        const std::string indicators = json_extract_object(r0, "indicators");
        const std::string quote_arr  = json_extract_array(indicators, "quote");
        json_for_each_object(quote_arr, [&](const std::string& q) {
            json_for_each_number(json_extract_array(q, "open"),   [&](double v){ open.push_back(v); });
            json_for_each_number(json_extract_array(q, "high"),   [&](double v){ high.push_back(v); });
            json_for_each_number(json_extract_array(q, "low"),    [&](double v){ low.push_back(v); });
            json_for_each_number(json_extract_array(q, "close"),  [&](double v){ close.push_back(v); });
            json_for_each_number(json_extract_array(q, "volume"), [&](double v){ volume.push_back(v); });
        });
        const std::string adj_arr = json_extract_array(indicators, "adjclose");
        json_for_each_object(adj_arr, [&](const std::string& a) {
            json_for_each_number(json_extract_array(a, "adjclose"),
                                 [&](double v){ adjclose.push_back(v); });
        });
    });

    const size_t N = ts.size();
    std::string results = "[";
    bool first = true;
    for (size_t i = 0; i < N; ++i) {
        const double oo = i < open.size()     ? open[i]     : std::nan("");
        const double hh = i < high.size()     ? high[i]     : std::nan("");
        const double ll = i < low.size()      ? low[i]      : std::nan("");
        const double cc = i < close.size()    ? close[i]    : std::nan("");
        const double vv = i < volume.size()   ? volume[i]   : std::nan("");
        const double ac = i < adjclose.size() ? adjclose[i] : cc;
        if (!std::isfinite(cc)) continue;     // Yahoo emits null bars for thin sessions
        if (!first) results += ",";
        first = false;
        results += "{\"date\":\"" + fmt_date_utc(ts[i]) + "\"";
        results += ",\"open\":"     + fmt_num(oo);
        results += ",\"high\":"     + fmt_num(hh);
        results += ",\"low\":"      + fmt_num(ll);
        results += ",\"close\":"    + fmt_num(cc);
        results += ",\"volume\":"   + fmt_num(vv);
        results += ",\"adj_close\":" + fmt_num(ac);
        results += "}";
    }
    results += "]";

    return MarketDataResult{200, wrap_envelope(results, "yahoo"), false};
}

// ─────────────────────────────────────────────────────────────────────────────
// yahoo_options -- /v7/finance/options/<sym>
//
// Reshape: optionChain.result[0].options[].{calls[], puts[]} -> flattened
// OptionsRow array with our Step-6 field names.
// ─────────────────────────────────────────────────────────────────────────────

MarketDataResult MarketDataProxy::yahoo_options(const std::string& query)
{
    std::string sym = qs_get(query, "symbol");
    if (sym.empty()) sym = "SPY";

    std::string url = kYahooOptions;
    url += url_encode(sym, /*keep_comma=*/false);

    MarketDataResult raw = http_get(url, { kYahooUserAgent });
    if (raw.status < 200 || raw.status >= 300) {
        return MarketDataResult{
            raw.status,
            wrap_envelope("[]", "yahoo",
                          warning_one("Yahoo options upstream HTTP error")),
            false
        };
    }

    const std::string oc      = json_extract_object(raw.body, "optionChain");
    const std::string result  = json_extract_array(oc, "result");

    std::string results = "[";
    bool first = true;
    json_for_each_object(result, [&](const std::string& r0) {
        const std::string und = json_extract_string(r0, "underlyingSymbol");
        const std::string options_arr = json_extract_array(r0, "options");
        json_for_each_object(options_arr, [&](const std::string& exp_obj) {
            const int64_t exp_ts = static_cast<int64_t>(
                json_extract_number(exp_obj, "expirationDate"));
            const std::string exp_iso = fmt_date_utc(exp_ts);

            auto emit_side = [&](const char* arr_key, const char* side_label) {
                const std::string side_arr = json_extract_array(exp_obj, arr_key);
                json_for_each_object(side_arr, [&](const std::string& c) {
                    if (!first) results += ",";
                    first = false;
                    results += "{\"underlying_symbol\":\"" + json_escape(und) + "\"";
                    results += ",\"contract_symbol\":\""
                               + json_escape(json_extract_string(c, "contractSymbol"))
                               + "\"";
                    results += ",\"expiration\":\"" + json_escape(exp_iso) + "\"";
                    results += ",\"strike\":" + fmt_num(json_extract_number(c, "strike"));
                    results += ",\"option_type\":\"" + std::string(side_label) + "\"";
                    results += ",\"bid\":"  + fmt_num(json_extract_number(c, "bid"));
                    results += ",\"ask\":"  + fmt_num(json_extract_number(c, "ask"));
                    results += ",\"last_trade_price\":" + fmt_num(json_extract_number(c, "lastPrice"));
                    results += ",\"implied_volatility\":" + fmt_num(json_extract_number(c, "impliedVolatility"));
                    results += ",\"open_interest\":" + fmt_num(json_extract_number(c, "openInterest"));
                    results += ",\"volume\":" + fmt_num(json_extract_number(c, "volume"));
                    results += ",\"delta\":null";   // Yahoo doesn't ship greeks; OmonPanel computes IV-based proxies
                    results += "}";
                });
            };
            emit_side("calls", "call");
            emit_side("puts",  "put");
        });
    });
    results += "]";

    return MarketDataResult{200, wrap_envelope(results, "yahoo"), false};
}

// ─────────────────────────────────────────────────────────────────────────────
// yahoo_quote_summary -- /v10/finance/quoteSummary/<sym>?modules=...
//
// One Yahoo endpoint serves several Step-5/6 routes via different `modules=`:
//   equity/fundamental/income       -> incomeStatementHistory
//   equity/fundamental/balance      -> balanceSheetHistory
//   equity/fundamental/cash         -> cashflowStatementHistory
//   equity/fundamental/key_metrics  -> summaryDetail + defaultKeyStatistics + financialData
//   equity/fundamental/multiples    -> summaryDetail + defaultKeyStatistics
//   equity/estimates/consensus      -> earningsTrend
//   equity/estimates/surprise       -> earningsHistory
//   equity/profile                  -> assetProfile + summaryProfile
//
// Each branch reshapes the Yahoo module(s) into a results[] array matching
// the Step-5/6 envelope's per-row schema.
//
// Coverage caveat: Yahoo's free quoteSummary occasionally returns 401 with
// the message "Invalid Cookie" or "Invalid Crumb" for some symbols (especially
// non-US tickers / small caps). We surface this as a warning and let the
// panel render an empty-state. Set OMEGA_MARKETDATA_MOCK=1 to bypass entirely
// for local development.
// ─────────────────────────────────────────────────────────────────────────────

MarketDataResult MarketDataProxy::yahoo_quote_summary(const std::string& route,
                                                     const std::string& query)
{
    std::string sym = qs_get(query, "symbol");
    if (sym.empty()) sym = "AAPL";

    // Pick the Yahoo modules string for this route.
    std::string modules;
    if      (route == "equity/fundamental/income")      modules = "incomeStatementHistory";
    else if (route == "equity/fundamental/balance")     modules = "balanceSheetHistory";
    else if (route == "equity/fundamental/cash")        modules = "cashflowStatementHistory";
    else if (route == "equity/fundamental/key_metrics") modules = "summaryDetail,defaultKeyStatistics,financialData";
    else if (route == "equity/fundamental/multiples")   modules = "summaryDetail,defaultKeyStatistics";
    else if (route == "equity/estimates/consensus")     modules = "earningsTrend";
    else if (route == "equity/estimates/surprise")      modules = "earningsHistory";
    else if (route == "equity/profile")                 modules = "assetProfile,summaryProfile,price";
    else                                                modules = "summaryDetail";

    std::string url = kYahooSummary;
    url += url_encode(sym, /*keep_comma=*/false);
    url += "?modules=";
    url += url_encode(modules, /*keep_comma=*/true);

    MarketDataResult raw = http_get(url, { kYahooUserAgent });
    if (raw.status < 200 || raw.status >= 300) {
        return MarketDataResult{
            raw.status,
            wrap_envelope("[]", "yahoo",
                          warning_one("Yahoo quoteSummary upstream HTTP error "
                                      "(may need crumb on some symbols)")),
            false
        };
    }

    // Drill into quoteSummary.result[0].
    const std::string qs   = json_extract_object(raw.body, "quoteSummary");
    const std::string rarr = json_extract_array(qs, "result");
    std::string r0;
    json_for_each_object(rarr, [&](const std::string& o) { if (r0.empty()) r0 = o; });
    if (r0.empty()) {
        return MarketDataResult{
            200,
            wrap_envelope("[]", "yahoo",
                          warning_one("No data returned by Yahoo for this symbol")),
            false
        };
    }

    // -------- income / balance / cash ------------------------------------
    auto reshape_statement = [&](const char* mod_key,
                                 const char* arr_key,
                                 std::initializer_list<std::pair<const char*, const char*>> field_map)
        -> std::string
    {
        const std::string mod = json_extract_object(r0, mod_key);
        const std::string arr = json_extract_array(mod, arr_key);
        std::string out = "[";
        bool first = true;
        json_for_each_object(arr, [&](const std::string& row) {
            const int64_t end_ts = static_cast<int64_t>(
                json_extract_number(row, "endDate"));
            const std::string end_iso = fmt_date_utc(end_ts);
            if (!first) out += ",";
            first = false;
            out += "{\"period_ending\":\"" + json_escape(end_iso) + "\"";
            out += ",\"fiscal_period\":\"" + json_escape(end_iso.substr(0, 4)) + "\"";
            for (const auto& kv : field_map) {
                out += ",\"" + std::string(kv.second) + "\":"
                       + fmt_num(json_extract_number(row, kv.first));
            }
            out += "}";
        });
        out += "]";
        return out;
    };

    if (route == "equity/fundamental/income") {
        const std::string body = reshape_statement(
            "incomeStatementHistory", "incomeStatementHistory",
            {
                {"totalRevenue",      "revenue"},
                {"costOfRevenue",     "cost_of_revenue"},
                {"grossProfit",       "gross_profit"},
                {"operatingIncome",   "operating_income"},
                {"ebit",              "ebitda"},
                {"netIncome",         "net_income"},
                {"basicEPS",          "eps_basic"},
                {"dilutedEPS",        "eps_diluted"},
            });
        return MarketDataResult{200, wrap_envelope(body, "yahoo"), false};
    }

    if (route == "equity/fundamental/balance") {
        const std::string body = reshape_statement(
            "balanceSheetHistory", "balanceSheetStatements",
            {
                {"totalAssets",                        "total_assets"},
                {"totalCurrentAssets",                 "total_current_assets"},
                {"cash",                               "cash_and_short_term_investments"},
                {"totalLiab",                          "total_liabilities"},
                {"totalCurrentLiabilities",            "total_current_liabilities"},
                {"longTermDebt",                       "long_term_debt"},
                {"shortLongTermDebt",                  "short_term_debt"},
                {"totalStockholderEquity",             "total_equity"},
            });
        return MarketDataResult{200, wrap_envelope(body, "yahoo"), false};
    }

    if (route == "equity/fundamental/cash") {
        const std::string body = reshape_statement(
            "cashflowStatementHistory", "cashflowStatements",
            {
                {"totalCashFromOperatingActivities", "cash_from_operating_activities"},
                {"totalCashflowsFromInvestingActivities", "cash_from_investing_activities"},
                {"totalCashFromFinancingActivities", "cash_from_financing_activities"},
                {"capitalExpenditures",              "capital_expenditure"},
                {"changeInCash",                     "free_cash_flow"},
            });
        return MarketDataResult{200, wrap_envelope(body, "yahoo"), false};
    }

    // -------- key_metrics ------------------------------------------------
    if (route == "equity/fundamental/key_metrics") {
        const std::string sd  = json_extract_object(r0, "summaryDetail");
        const std::string dks = json_extract_object(r0, "defaultKeyStatistics");
        const std::string fd  = json_extract_object(r0, "financialData");

        std::string row = "{";
        row += "\"market_cap\":"        + fmt_num(json_extract_number(sd,  "marketCap"));
        row += ",\"enterprise_value\":" + fmt_num(json_extract_number(dks, "enterpriseValue"));
        row += ",\"pe_ratio\":"         + fmt_num(json_extract_number(sd,  "trailingPE"));
        row += ",\"forward_pe\":"       + fmt_num(json_extract_number(sd,  "forwardPE"));
        row += ",\"peg_ratio\":"        + fmt_num(json_extract_number(dks, "pegRatio"));
        row += ",\"price_to_book\":"    + fmt_num(json_extract_number(dks, "priceToBook"));
        row += ",\"price_to_sales\":"   + fmt_num(json_extract_number(sd,  "priceToSalesTrailing12Months"));
        row += ",\"ev_to_sales\":"      + fmt_num(json_extract_number(dks, "enterpriseToRevenue"));
        row += ",\"ev_to_ebitda\":"     + fmt_num(json_extract_number(dks, "enterpriseToEbitda"));
        row += ",\"dividend_yield\":"   + fmt_num(json_extract_number(sd,  "dividendYield"));
        row += ",\"payout_ratio\":"     + fmt_num(json_extract_number(sd,  "payoutRatio"));
        row += ",\"beta\":"             + fmt_num(json_extract_number(sd,  "beta"));
        row += ",\"return_on_equity\":" + fmt_num(json_extract_number(fd,  "returnOnEquity"));
        row += ",\"return_on_assets\":" + fmt_num(json_extract_number(fd,  "returnOnAssets"));
        row += ",\"debt_to_equity\":"   + fmt_num(json_extract_number(fd,  "debtToEquity"));
        row += ",\"current_ratio\":"    + fmt_num(json_extract_number(fd,  "currentRatio"));
        row += ",\"quick_ratio\":"      + fmt_num(json_extract_number(fd,  "quickRatio"));
        row += ",\"profit_margin\":"    + fmt_num(json_extract_number(fd,  "profitMargins"));
        row += ",\"operating_margin\":" + fmt_num(json_extract_number(fd,  "operatingMargins"));
        row += "}";
        return MarketDataResult{200, wrap_envelope("[" + row + "]", "yahoo"), false};
    }

    // -------- multiples --------------------------------------------------
    if (route == "equity/fundamental/multiples") {
        const std::string sd  = json_extract_object(r0, "summaryDetail");
        const std::string dks = json_extract_object(r0, "defaultKeyStatistics");
        std::string row = "{";
        row += "\"pe_ratio_ttm\":"            + fmt_num(json_extract_number(sd,  "trailingPE"));
        row += ",\"ev_to_ebitda_ttm\":"       + fmt_num(json_extract_number(dks, "enterpriseToEbitda"));
        row += ",\"price_to_sales_ttm\":"     + fmt_num(json_extract_number(sd,  "priceToSalesTrailing12Months"));
        row += ",\"price_to_book_quarterly\":" + fmt_num(json_extract_number(dks, "priceToBook"));
        const double pe = json_extract_number(sd, "trailingPE");
        const double ey = std::isfinite(pe) && pe > 0 ? 1.0 / pe : std::nan("");
        row += ",\"earnings_yield_ttm\":"     + fmt_num(ey);
        row += ",\"free_cash_flow_yield_ttm\":null";
        row += "}";
        return MarketDataResult{200, wrap_envelope("[" + row + "]", "yahoo"), false};
    }

    // -------- earnings consensus (earningsTrend) -------------------------
    if (route == "equity/estimates/consensus") {
        const std::string et   = json_extract_object(r0, "earningsTrend");
        const std::string trend = json_extract_array(et, "trend");
        std::string out = "[";
        bool first = true;
        bool any = false;
        json_for_each_object(trend, [&](const std::string& t) {
            // Yahoo's `period` is one of "0q","+1q","0y","+1y","-1q",... -- we
            // emit only forward periods (those starting with '+' or "0").
            const std::string period = json_extract_string(t, "period");
            if (period.empty() || period[0] == '-') return;
            const std::string endDate = json_extract_string(t, "endDate");
            const std::string eps_est = json_extract_object(t, "earningsEstimate");
            const std::string rev_est = json_extract_object(t, "revenueEstimate");
            if (eps_est.empty() && rev_est.empty()) return;
            if (!first) out += ",";
            first = false;
            any = true;
            out += "{\"symbol\":\"" + json_escape(sym) + "\"";
            out += ",\"fiscal_period\":\"" + json_escape(period) + "\"";
            out += ",\"fiscal_year\":null";
            out += ",\"eps_avg\":"            + fmt_num(json_extract_number(eps_est, "avg"));
            out += ",\"eps_high\":"           + fmt_num(json_extract_number(eps_est, "high"));
            out += ",\"eps_low\":"            + fmt_num(json_extract_number(eps_est, "low"));
            out += ",\"revenue_avg\":"        + fmt_num(json_extract_number(rev_est, "avg"));
            out += ",\"revenue_high\":"       + fmt_num(json_extract_number(rev_est, "high"));
            out += ",\"revenue_low\":"        + fmt_num(json_extract_number(rev_est, "low"));
            out += ",\"number_of_analysts\":" + fmt_num(json_extract_number(eps_est, "numberOfAnalysts"));
            (void)endDate;
            out += "}";
        });
        out += "]";
        const std::string warn = any ? "null" :
            warning_one("Partial coverage: Yahoo earningsTrend returned no rows for this symbol");
        return MarketDataResult{200, wrap_envelope(out, "yahoo", warn), false};
    }

    // -------- earnings surprise (earningsHistory) -----------------------
    if (route == "equity/estimates/surprise") {
        const std::string eh   = json_extract_object(r0, "earningsHistory");
        const std::string hist = json_extract_array(eh, "history");
        std::string out = "[";
        bool first = true;
        bool any = false;
        json_for_each_object(hist, [&](const std::string& h) {
            const int64_t qd = static_cast<int64_t>(
                json_extract_number(h, "quarter"));
            const std::string date = fmt_date_utc(qd);
            const std::string period = json_extract_string(h, "period");
            const double actual    = json_extract_number(h, "epsActual");
            const double estimate  = json_extract_number(h, "epsEstimate");
            const double surprise  = json_extract_number(h, "epsDifference");
            const double surprisepct = json_extract_number(h, "surprisePercent");
            if (!first) out += ",";
            first = false;
            any = true;
            out += "{\"symbol\":\"" + json_escape(sym) + "\"";
            out += ",\"date\":\"" + json_escape(date) + "\"";
            out += ",\"fiscal_period\":\"" + json_escape(period) + "\"";
            out += ",\"fiscal_year\":null";
            out += ",\"eps_actual\":"      + fmt_num(actual);
            out += ",\"eps_estimate\":"    + fmt_num(estimate);
            out += ",\"eps_surprise\":"    + fmt_num(surprise);
            out += ",\"surprise_percent\":" + fmt_num(surprisepct * 100.0);
            out += "}";
        });
        out += "]";
        const std::string warn = any ? "null" :
            warning_one("Partial coverage: Yahoo earningsHistory returned no rows for this symbol");
        return MarketDataResult{200, wrap_envelope(out, "yahoo", warn), false};
    }

    // -------- profile ----------------------------------------------------
    if (route == "equity/profile") {
        const std::string ap = json_extract_object(r0, "assetProfile");
        const std::string sp = json_extract_object(r0, "summaryProfile");
        const std::string pr = json_extract_object(r0, "price");
        const std::string profile = ap.empty() ? sp : ap;

        const std::string industry  = json_extract_string(profile, "industry");
        const std::string sector    = json_extract_string(profile, "sector");
        const std::string website   = json_extract_string(profile, "website");
        const std::string country   = json_extract_string(profile, "country");
        const std::string state     = json_extract_string(profile, "state");
        const std::string city      = json_extract_string(profile, "city");
        const double      employees = json_extract_number(profile, "fullTimeEmployees");
        const std::string longBus   = json_extract_string(profile, "longBusinessSummary");
        const std::string name      = json_extract_string(pr, "longName");
        const std::string exchange  = json_extract_string(pr, "exchangeName");
        const std::string currency  = json_extract_string(pr, "currency");
        const double      mktcap    = json_extract_number(pr, "marketCap");
        // Officers[0].name is the closest analog to CEO (officers is sorted
        // by importance). Skipped to keep parser simple.
        std::string row = "{";
        row += "\"symbol\":\""      + json_escape(sym) + "\"";
        row += ",\"name\":\""       + json_escape(name.empty() ? sym : name) + "\"";
        row += ",\"description\":\"" + json_escape(longBus) + "\"";
        row += ",\"industry\":\""   + json_escape(industry) + "\"";
        row += ",\"sector\":\""     + json_escape(sector) + "\"";
        row += ",\"ipo_date\":null";
        row += ",\"ceo\":\"\"";
        row += ",\"hq_country\":\"" + json_escape(country) + "\"";
        row += ",\"hq_state\":\""   + json_escape(state) + "\"";
        row += ",\"hq_city\":\""    + json_escape(city) + "\"";
        row += ",\"employees\":"    + fmt_num(employees);
        row += ",\"website\":\""    + json_escape(website) + "\"";
        row += ",\"exchange\":\""   + json_escape(exchange) + "\"";
        row += ",\"currency\":\""   + json_escape(currency) + "\"";
        row += ",\"market_cap\":"   + fmt_num(mktcap);
        row += "}";
        return MarketDataResult{200, wrap_envelope("[" + row + "]", "yahoo"), false};
    }

    return MarketDataResult{200, wrap_envelope("[]", "yahoo"), false};
}

// ─────────────────────────────────────────────────────────────────────────────
// yahoo_search_news -- /v1/finance/search?q=...&newsCount=...
//
// per_symbol=true: NI (per-symbol news, q=<symbol>)
// per_symbol=false: INTEL (broad world news; we use q=SPY since SPY captures
//                   broad US-market headlines on Yahoo's news feed)
// ─────────────────────────────────────────────────────────────────────────────

MarketDataResult MarketDataProxy::yahoo_search_news(const std::string& query,
                                                   bool per_symbol)
{
    std::string q;
    if (per_symbol) {
        q = qs_get(query, "symbol");
        if (q.empty()) q = qs_get(query, "symbols");
        if (q.empty()) q = "SPY";
    } else {
        q = qs_get(query, "q");
        if (q.empty()) q = "SPY";   // proxy for "broad market news"
    }
    std::string limit = qs_get(query, "limit");
    if (limit.empty()) limit = "20";

    std::string url = kYahooSearch;
    url += "?q=";
    url += url_encode(q, /*keep_comma=*/true);
    url += "&newsCount=";
    url += url_encode(limit, false);
    url += "&quotesCount=0";

    MarketDataResult raw = http_get(url, { kYahooUserAgent });
    if (raw.status < 200 || raw.status >= 300) {
        return MarketDataResult{
            raw.status,
            wrap_envelope("[]", "yahoo",
                          warning_one("Yahoo search upstream HTTP error")),
            false
        };
    }

    const std::string news_arr = json_extract_array(raw.body, "news");
    std::string results = "[";
    bool first = true;
    json_for_each_object(news_arr, [&](const std::string& n) {
        const std::string title  = json_extract_string(n, "title");
        const std::string url2   = json_extract_string(n, "link");
        const std::string source = json_extract_string(n, "publisher");
        const int64_t pub_ts     = static_cast<int64_t>(
            json_extract_number(n, "providerPublishTime"));
        const std::string date_iso = fmt_iso_utc(pub_ts);
        // Symbols list (per_symbol clients use it; INTEL ignores it).
        const std::string syms_arr = json_extract_array(n, "relatedTickers");
        std::string syms_out = "[";
        bool sf = true;
        json_for_each_object(syms_arr, [&](const std::string&) { (void)sf; });
        // relatedTickers is an array of strings; we read it manually.
        if (syms_arr.size() > 2) {
            // Walk the array as strings.
            size_t i = 1;
            const size_t end = syms_arr.size() - 1;
            while (i < end) {
                i = json_skip_ws(syms_arr, i);
                while (i < end && syms_arr[i] == ',') {
                    ++i;
                    i = json_skip_ws(syms_arr, i);
                }
                if (i >= end) break;
                if (syms_arr[i] != '"') break;
                std::string s;
                if (!json_read_string_at(syms_arr, i, s)) break;
                if (!sf) syms_out += ",";
                sf = false;
                syms_out += "\"" + json_escape(s) + "\"";
            }
        }
        syms_out += "]";

        if (!first) results += ",";
        first = false;
        results += "{\"title\":\"" + json_escape(title) + "\"";
        results += ",\"text\":\"\"";
        results += ",\"date\":\"" + json_escape(date_iso) + "\"";
        results += ",\"url\":\""  + json_escape(url2) + "\"";
        results += ",\"source\":\"" + json_escape(source) + "\"";
        results += ",\"symbols\":" + syms_out;
        results += "}";
    });
    results += "]";

    return MarketDataResult{200, wrap_envelope(results, "yahoo"), false};
}

// ─────────────────────────────────────────────────────────────────────────────
// yahoo_screener -- /v1/finance/screener/predefined/saved?scrIds=...
//
// Maps the Step-5/6 active/gainers/losers route to Yahoo's predefined
// screeners (most_actives, day_gainers, day_losers).
// ─────────────────────────────────────────────────────────────────────────────

MarketDataResult MarketDataProxy::yahoo_screener(const std::string& route)
{
    const char* scrid = "most_actives";
    if (route == "equity/discovery/gainers") scrid = "day_gainers";
    if (route == "equity/discovery/losers")  scrid = "day_losers";

    std::string url = kYahooScreener;
    url += "?scrIds=";
    url += scrid;
    // Step 7: Yahoo's free predefined-screener tier supports up to 100
    // rows per call. Was 25 (default cap), which was making the MOV panel
    // feel "very limited" -- 4x more rows here, still inside Yahoo's
    // free-tier ceiling and still small enough that the on-wire payload
    // stays under the 30 s libcurl timeout for cold cache.
    url += "&count=100";

    MarketDataResult raw = http_get(url, { kYahooUserAgent });
    if (raw.status < 200 || raw.status >= 300) {
        return MarketDataResult{
            raw.status,
            wrap_envelope("[]", "yahoo",
                          warning_one("Yahoo screener upstream HTTP error")),
            false
        };
    }

    // finance.result[0].quotes[]
    const std::string fin   = json_extract_object(raw.body, "finance");
    const std::string rarr  = json_extract_array(fin, "result");
    std::string r0;
    json_for_each_object(rarr, [&](const std::string& o) { if (r0.empty()) r0 = o; });
    const std::string quotes = json_extract_array(r0, "quotes");

    std::string results = "[";
    bool first = true;
    json_for_each_object(quotes, [&](const std::string& q) {
        const std::string sym = json_extract_string(q, "symbol");
        if (sym.empty()) return;
        std::string name = json_extract_string(q, "longName");
        if (name.empty()) name = json_extract_string(q, "shortName");
        if (name.empty()) name = sym;
        const double price = json_extract_number(q, "regularMarketPrice");
        const double chg   = json_extract_number(q, "regularMarketChange");
        const double pct   = json_extract_number(q, "regularMarketChangePercent");
        const double vol   = json_extract_number(q, "regularMarketVolume");
        if (!first) results += ",";
        first = false;
        results += "{\"symbol\":\"" + json_escape(sym) + "\"";
        results += ",\"name\":\""   + json_escape(name) + "\"";
        results += ",\"price\":"    + fmt_num(price);
        results += ",\"change\":"   + fmt_num(chg);
        results += ",\"percent_change\":" + fmt_num(pct);
        results += ",\"change_percent\":" + fmt_num(pct);
        results += ",\"volume\":"   + fmt_num(vol);
        results += "}";
    });
    results += "]";

    return MarketDataResult{200, wrap_envelope(results, "yahoo"), false};
}

// ─────────────────────────────────────────────────────────────────────────────
// yahoo_dividends -- /v8/finance/chart/<sym>?events=div&interval=1d&range=10y
//
// Reshape: chart.result[0].events.dividends (object keyed by epoch)
// -> [{ex_dividend_date, amount, ...}]
// ─────────────────────────────────────────────────────────────────────────────

MarketDataResult MarketDataProxy::yahoo_dividends(const std::string& query)
{
    std::string sym = qs_get(query, "symbol");
    if (sym.empty()) sym = "AAPL";

    std::string url = kYahooChart;
    url += url_encode(sym, /*keep_comma=*/false);
    url += "?interval=1d&range=10y&events=div";

    MarketDataResult raw = http_get(url, { kYahooUserAgent });
    if (raw.status < 200 || raw.status >= 300) {
        return MarketDataResult{
            raw.status,
            wrap_envelope("[]", "yahoo",
                          warning_one("Yahoo dividends upstream HTTP error")),
            false
        };
    }

    const std::string chart = json_extract_object(raw.body, "chart");
    const std::string rarr  = json_extract_array(chart, "result");
    std::string r0;
    json_for_each_object(rarr, [&](const std::string& o) { if (r0.empty()) r0 = o; });
    const std::string events = json_extract_object(r0, "events");
    const std::string divs   = json_extract_object(events, "dividends");

    // Yahoo's `dividends` is a map keyed by epoch-second strings, each value
    // an object {amount, date}. We walk the keys manually.
    std::vector<std::pair<int64_t, double>> rows;
    if (divs.size() > 2) {
        size_t i = 1;
        const size_t end = divs.size() - 1;
        while (i < end) {
            i = json_skip_ws(divs, i);
            while (i < end && divs[i] == ',') { ++i; i = json_skip_ws(divs, i); }
            if (i >= end) break;
            if (divs[i] != '"') break;
            std::string keyk;
            if (!json_read_string_at(divs, i, keyk)) break;
            i = json_skip_ws(divs, i);
            if (i < end && divs[i] == ':') ++i;
            i = json_skip_ws(divs, i);
            const std::string val = json_read_object_at(divs, i);
            i += val.size();
            const double amt = json_extract_number(val, "amount");
            char* endp = nullptr;
            const int64_t ts = std::strtoll(keyk.c_str(), &endp, 10);
            if (endp != keyk.c_str() && std::isfinite(amt)) {
                rows.emplace_back(ts, amt);
            }
        }
    }

    std::sort(rows.begin(), rows.end(),
              [](const std::pair<int64_t, double>& a,
                 const std::pair<int64_t, double>& b) { return a.first > b.first; });

    std::string results = "[";
    bool first = true;
    for (const auto& kv : rows) {
        const std::string d = fmt_date_utc(kv.first);
        if (!first) results += ",";
        first = false;
        results += "{\"ex_dividend_date\":\"" + d + "\"";
        results += ",\"amount\":" + fmt_num(kv.second);
        results += ",\"date\":\"" + d + "\"";
        results += "}";
    }
    results += "]";

    return MarketDataResult{200, wrap_envelope(results, "yahoo"), false};
}

// ─────────────────────────────────────────────────────────────────────────────
// yahoo_fx_or_crypto -- /v7/finance/quote with =X (fx) / -USD (crypto) suffix
// ─────────────────────────────────────────────────────────────────────────────

MarketDataResult MarketDataProxy::yahoo_fx_or_crypto(const std::string& query,
                                                    const std::string& kind)
{
    std::string raw_syms = qs_get(query, "symbol");
    if (raw_syms.empty()) raw_syms = qs_get(query, "symbols");
    if (raw_syms.empty()) {
        raw_syms = (kind == "fx") ? "EURUSD,GBPUSD,USDJPY"
                                  : "BTC-USD,ETH-USD,SOL-USD";
    }

    // Reshape the symbol list into Yahoo's expected suffix form. Each comma
    // splits one symbol; we add `=X` for fx if missing, and `-USD` for crypto
    // if missing.
    auto needs_suffix = [&](const std::string& s) -> std::string {
        if (kind == "fx") {
            if (s.find('=') != std::string::npos) return s;
            return s + "=X";
        }
        // crypto
        if (s.find('-') != std::string::npos) return s;
        return s + "-USD";
    };

    std::string yahoo_syms;
    {
        size_t start = 0;
        bool first = true;
        while (start <= raw_syms.size()) {
            const size_t comma = raw_syms.find(',', start);
            const std::string sym = raw_syms.substr(
                start,
                (comma == std::string::npos) ? std::string::npos : comma - start);
            if (!sym.empty()) {
                if (!first) yahoo_syms += ",";
                first = false;
                yahoo_syms += needs_suffix(sym);
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }

    std::string url = kYahooQuote;
    url += "?symbols=";
    url += url_encode(yahoo_syms, /*keep_comma=*/true);

    MarketDataResult raw = http_get(url, { kYahooUserAgent });
    if (raw.status < 200 || raw.status >= 300) {
        return MarketDataResult{
            raw.status,
            wrap_envelope("[]", "yahoo",
                          warning_one("Yahoo quote upstream HTTP error")),
            false
        };
    }

    const std::string qr = json_extract_object(raw.body, "quoteResponse");
    const std::string arr = json_extract_array(qr, "result");

    std::string results = "[";
    bool first = true;
    json_for_each_object(arr, [&](const std::string& obj) {
        std::string sym = json_extract_string(obj, "symbol");
        if (sym.empty()) return;
        // Strip the Yahoo suffix to give callers back what they sent.
        if (kind == "fx") {
            const auto eq = sym.find('=');
            if (eq != std::string::npos) sym = sym.substr(0, eq);
        }
        std::string name = json_extract_string(obj, "longName");
        if (name.empty()) name = json_extract_string(obj, "shortName");
        if (name.empty()) name = sym;

        const double last = json_extract_number(obj, "regularMarketPrice");
        const double chg  = json_extract_number(obj, "regularMarketChange");
        const double pct  = json_extract_number(obj, "regularMarketChangePercent");
        const double vol  = json_extract_number(obj, "regularMarketVolume");
        const double bid  = json_extract_number(obj, "bid");
        const double ask  = json_extract_number(obj, "ask");
        const double mcap = json_extract_number(obj, "marketCap");

        if (!first) results += ",";
        first = false;
        results += "{\"symbol\":\"" + json_escape(sym) + "\"";
        results += ",\"name\":\""   + json_escape(name) + "\"";
        results += ",\"last_price\":" + fmt_num(last);
        results += ",\"change\":"     + fmt_num(chg);
        results += ",\"change_percent\":" + fmt_num(pct);
        results += ",\"volume\":"     + fmt_num(vol);
        if (kind == "fx") {
            results += ",\"bid\":" + fmt_num(bid);
            results += ",\"ask\":" + fmt_num(ask);
        } else {
            results += ",\"market_cap\":" + fmt_num(mcap);
        }
        results += "}";
    });
    results += "]";

    return MarketDataResult{200, wrap_envelope(results, "yahoo"), false};
}

// ─────────────────────────────────────────────────────────────────────────────
// fred_treasury_curve -- assemble the US Treasury curve from FRED series.
//
// One request per maturity (FRED's API is one-series-per-call). We fan out
// 11 GETs serially; each is fast (FRED serves these from CDN). The free tier
// is 120 reqs/min so 11 sequential calls is well under budget.
//
// Reshape: each FRED observation -> {date, maturity, rate}.
// ─────────────────────────────────────────────────────────────────────────────

MarketDataResult MarketDataProxy::fred_treasury_curve(const std::string& /*query*/)
{
    if (fred_key_.empty()) {
        return MarketDataResult{
            503,
            R"({"error":"OMEGA_FRED_KEY_NOT_SET","detail":)"
            R"("Set OMEGA_FRED_KEY at Machine scope and restart Omega.exe. )"
            R"(Free key: https://fredaccount.stlouisfed.org/apikeys"})",
            false
        };
    }

    struct Tenor { const char* series; const char* label; };
    static const Tenor kTenors[] = {
        { "DGS1MO", "month_1"  },
        { "DGS3MO", "month_3"  },
        { "DGS6MO", "month_6"  },
        { "DGS1",   "year_1"   },
        { "DGS2",   "year_2"   },
        { "DGS3",   "year_3"   },
        { "DGS5",   "year_5"   },
        { "DGS7",   "year_7"   },
        { "DGS10",  "year_10"  },
        { "DGS20",  "year_20"  },
        { "DGS30",  "year_30"  },
    };
    constexpr size_t N = sizeof(kTenors) / sizeof(kTenors[0]);

    std::string results = "[";
    bool first = true;
    bool any_ok = false;
    for (size_t i = 0; i < N; ++i) {
        std::string url = kFredObs;
        url += "?series_id=";
        url += kTenors[i].series;
        url += "&api_key=";
        url += url_encode(fred_key_, /*keep_comma=*/false);
        url += "&file_type=json&sort_order=desc&limit=1";

        MarketDataResult r = http_get(url, { kFredUserAgent });
        if (r.status < 200 || r.status >= 300) continue;

        const std::string obs = json_extract_array(r.body, "observations");
        // Take the first observation in the array (latest because sort_order=desc).
        std::string first_obs;
        json_for_each_object(obs, [&](const std::string& o) {
            if (first_obs.empty()) first_obs = o;
        });
        if (first_obs.empty()) continue;

        const std::string date = json_extract_string(first_obs, "date");
        const double rate = json_extract_number(first_obs, "value");
        if (!std::isfinite(rate)) continue;

        if (!first) results += ",";
        first = false;
        any_ok = true;
        results += "{\"date\":\"" + json_escape(date) + "\"";
        results += ",\"maturity\":\"" + std::string(kTenors[i].label) + "\"";
        results += ",\"rate\":" + fmt_num(rate);
        results += "}";
    }
    results += "]";

    const std::string warn = any_ok ? "null" :
        warning_one("FRED upstream returned no observations for any tenor");
    return MarketDataResult{any_ok ? 200 : 502,
                            wrap_envelope(results, "fred", warn),
                            false};
}

// ─────────────────────────────────────────────────────────────────────────────
// fetch_mock -- synthetic JSON shaped like the legacy OpenBB OBBject.
//
// Lifted verbatim from the retired OpenBbProxy.cpp's fetch_mock(); the canned
// data and route coverage are identical so the panels render the same MOCK
// content as Step 6. The only changes are (a) the env var that gates it
// (OMEGA_MARKETDATA_MOCK now, not OMEGA_OPENBB_MOCK -- hard-cut) and (b) the
// `provider` field stays "mock" verbatim.
//
// Routes covered (same as Step 6):
//   news/world                                     -> INTEL
//   fixedincome/government/treasury_rates          -> CURV
//   equity/price/quote                             -> WEI / QR
//   equity/discovery/{active|gainers|losers}       -> MOV
//   derivatives/options/chains                     -> OMON
//   equity/fundamental/{income,balance,cash}       -> FA components
//   equity/fundamental/{key_metrics,multiples}     -> KEY components
//   equity/fundamental/dividends                   -> DVD
//   equity/estimates/{consensus,surprise}          -> EE components
//   news/company                                   -> NI
//   equity/price/historical                        -> GP / HP
//   equity/profile                                 -> DES
//   currency/price/quote                           -> FXC
//   crypto/price/quote                             -> CRYPTO
// Anything else returns an empty results array with provider "mock".
// ─────────────────────────────────────────────────────────────────────────────

MarketDataResult MarketDataProxy::fetch_mock(const std::string& route,
                                             const std::string& query)
{
    auto envelope = [](const std::string& results_json) {
        std::string out = "{\"results\":";
        out += results_json;
        out += ",\"provider\":\"mock\",\"warnings\":null,\"chart\":null,";
        out += "\"extra\":{\"mock\":true}}";
        return out;
    };

    if (route == "news/world") {
        const std::string results = R"([
  {"title":"Fed holds rates as inflation cools","text":"Synthetic Step-7 mock body. Replace with live data once Yahoo Finance is reachable.","date":"2026-05-01T13:30:00Z","url":"https://example.com/mock-1","source":"MOCK"},
  {"title":"Treasury 10Y yield slips below 4.20%","text":"Synthetic Step-7 mock body.","date":"2026-05-01T13:00:00Z","url":"https://example.com/mock-2","source":"MOCK"},
  {"title":"S&P 500 reclaims prior week high on broadening breadth","text":"Synthetic Step-7 mock body.","date":"2026-05-01T12:30:00Z","url":"https://example.com/mock-3","source":"MOCK"},
  {"title":"Crude oil holds $78 amid OPEC chatter","text":"Synthetic Step-7 mock body.","date":"2026-05-01T11:45:00Z","url":"https://example.com/mock-4","source":"MOCK"},
  {"title":"Gold steady near $2,310 as USD eases","text":"Synthetic Step-7 mock body.","date":"2026-05-01T11:00:00Z","url":"https://example.com/mock-5","source":"MOCK"}
])";
        return MarketDataResult{200, envelope(results), false};
    }

    if (route == "fixedincome/government/treasury_rates") {
        const std::string results = R"([
  {"date":"2026-05-01","maturity":"month_1","rate":4.95},
  {"date":"2026-05-01","maturity":"month_3","rate":4.88},
  {"date":"2026-05-01","maturity":"month_6","rate":4.72},
  {"date":"2026-05-01","maturity":"year_1","rate":4.55},
  {"date":"2026-05-01","maturity":"year_2","rate":4.30},
  {"date":"2026-05-01","maturity":"year_3","rate":4.18},
  {"date":"2026-05-01","maturity":"year_5","rate":4.10},
  {"date":"2026-05-01","maturity":"year_7","rate":4.15},
  {"date":"2026-05-01","maturity":"year_10","rate":4.20},
  {"date":"2026-05-01","maturity":"year_20","rate":4.45},
  {"date":"2026-05-01","maturity":"year_30","rate":4.40}
])";
        return MarketDataResult{200, envelope(results), false};
    }

    if (route == "equity/price/quote") {
        std::string symbols = "SPY,QQQ,DIA,IWM";
        const std::string needle = "symbol=";
        const auto pos = query.find(needle);
        if (pos != std::string::npos) {
            const auto end = query.find('&', pos);
            symbols = query.substr(pos + needle.size(),
                                   (end == std::string::npos)
                                     ? std::string::npos
                                     : end - pos - needle.size());
        }
        std::string results = "[";
        size_t start = 0;
        bool first = true;
        while (start <= symbols.size()) {
            const auto comma = symbols.find(',', start);
            const std::string sym = symbols.substr(
                start,
                (comma == std::string::npos) ? std::string::npos : comma - start);
            if (!sym.empty()) {
                if (!first) results += ",";
                first = false;
                int seed = 0;
                for (char c : sym) seed = (seed * 31 + (unsigned char)c) & 0xFFF;
                const double price  = 100.0 + (seed % 400);
                const double change = ((seed % 41) - 20) / 10.0;
                const double pct    = change / price * 100.0;
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                    R"X({"symbol":"%s","name":"%s ETF (mock)","last_price":%.2f,)X"
                    R"X("change":%.2f,"change_percent":%.4f,"volume":%d,)X"
                    R"X("prev_close":%.2f})X",
                    sym.c_str(), sym.c_str(), price, change, pct,
                    1000000 + (seed * 137), price - change);
                results += buf;
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        results += "]";
        return MarketDataResult{200, envelope(results), false};
    }

    if (route == "equity/discovery/active" ||
        route == "equity/discovery/gainers" ||
        route == "equity/discovery/losers") {
        const bool losers = (route == "equity/discovery/losers");
        const double sgn  = losers ? -1.0 : 1.0;
        char buf[2048];
        std::snprintf(buf, sizeof(buf), R"([
  {"symbol":"AAPL","name":"Apple Inc","price":182.50,"change":%.2f,"percent_change":%.4f,"volume":58000000},
  {"symbol":"NVDA","name":"NVIDIA Corp","price":910.25,"change":%.2f,"percent_change":%.4f,"volume":42000000},
  {"symbol":"MSFT","name":"Microsoft","price":410.10,"change":%.2f,"percent_change":%.4f,"volume":31000000},
  {"symbol":"TSLA","name":"Tesla Inc","price":188.60,"change":%.2f,"percent_change":%.4f,"volume":97000000},
  {"symbol":"AMD","name":"Advanced Micro Devices","price":162.40,"change":%.2f,"percent_change":%.4f,"volume":48000000},
  {"symbol":"META","name":"Meta Platforms","price":498.75,"change":%.2f,"percent_change":%.4f,"volume":22000000},
  {"symbol":"AMZN","name":"Amazon.com","price":182.30,"change":%.2f,"percent_change":%.4f,"volume":36000000},
  {"symbol":"GOOGL","name":"Alphabet Inc","price":171.20,"change":%.2f,"percent_change":%.4f,"volume":24000000}
])",
            sgn *  3.10, sgn * 1.7299,
            sgn * 18.50, sgn * 2.0763,
            sgn *  4.60, sgn * 1.1340,
            sgn * 11.20, sgn * 6.3373,
            sgn *  6.80, sgn * 4.3675,
            sgn *  7.50, sgn * 1.5267,
            sgn *  2.40, sgn * 1.3358,
            sgn *  1.95, sgn * 1.1502);
        return MarketDataResult{200, envelope(std::string(buf)), false};
    }

    if (route == "derivatives/options/chains") {
        std::string sym = "MOCK";
        const std::string needle = "symbol=";
        const auto pos = query.find(needle);
        if (pos != std::string::npos) {
            const auto end = query.find('&', pos);
            sym = query.substr(pos + needle.size(),
                               (end == std::string::npos)
                                 ? std::string::npos
                                 : end - pos - needle.size());
        }
        std::string results = "[";
        const char* expiries[2] = { "2026-06-20", "2026-09-19" };
        const double strikes[5] = { 90, 95, 100, 105, 110 };
        bool first = true;
        for (size_t e = 0; e < 2; ++e) {
            for (size_t s = 0; s < 5; ++s) {
                for (int t = 0; t < 2; ++t) {
                    if (!first) results += ",";
                    first = false;
                    const bool is_call = (t == 0);
                    const double k = strikes[s];
                    const double iv = 0.22 + 0.01 * s + 0.05 * e;
                    const double bid = is_call ? std::max(0.05, 100.0 - k + 1.5)
                                               : std::max(0.05, k - 100.0 + 1.5);
                    const double ask = bid + 0.10;
                    const double mid = (bid + ask) / 2.0;
                    const double oi  = 1500 + 350 * (5 - (int)s);
                    const double vol = 250 + 80 * (int)s;
                    const double delta = is_call ? std::max(0.05, 1.0 - 0.1 * (k - 95))
                                                 : std::min(-0.05, -1.0 + 0.1 * (k - 95));
                    char buf[512];
                    std::snprintf(buf, sizeof(buf),
                        R"X({"underlying_symbol":"%s","contract_symbol":"%s%s%s%05d","expiration":"%s",)X"
                        R"X("strike":%.2f,"option_type":"%s","bid":%.2f,"ask":%.2f,"last_trade_price":%.2f,)X"
                        R"X("implied_volatility":%.4f,"open_interest":%.0f,"volume":%.0f,"delta":%.4f})X",
                        sym.c_str(), sym.c_str(), expiries[e], is_call ? "C" : "P", (int)(k * 1000),
                        expiries[e], k, is_call ? "call" : "put", bid, ask, mid, iv, oi, vol, delta);
                    results += buf;
                }
            }
        }
        results += "]";
        return MarketDataResult{200, envelope(results), false};
    }

    if (route == "equity/fundamental/income") {
        const std::string results = R"X([
  {"period_ending":"2025-12-31","fiscal_period":"FY2025","revenue":390000000000,"cost_of_revenue":215000000000,"gross_profit":175000000000,"operating_income":118000000000,"ebitda":133000000000,"net_income":98000000000,"eps_basic":6.40,"eps_diluted":6.36},
  {"period_ending":"2024-12-31","fiscal_period":"FY2024","revenue":383000000000,"cost_of_revenue":214000000000,"gross_profit":169000000000,"operating_income":114000000000,"ebitda":129000000000,"net_income":97000000000,"eps_basic":6.10,"eps_diluted":6.05},
  {"period_ending":"2023-12-31","fiscal_period":"FY2023","revenue":366000000000,"cost_of_revenue":213000000000,"gross_profit":153000000000,"operating_income":108000000000,"ebitda":123000000000,"net_income":94000000000,"eps_basic":5.90,"eps_diluted":5.85}
])X";
        return MarketDataResult{200, envelope(results), false};
    }

    if (route == "equity/fundamental/balance") {
        const std::string results = R"X([
  {"period_ending":"2025-12-31","fiscal_period":"FY2025","total_assets":350000000000,"total_current_assets":140000000000,"cash_and_short_term_investments":62000000000,"total_liabilities":260000000000,"total_current_liabilities":150000000000,"long_term_debt":95000000000,"short_term_debt":12000000000,"total_equity":90000000000},
  {"period_ending":"2024-12-31","fiscal_period":"FY2024","total_assets":340000000000,"total_current_assets":135000000000,"cash_and_short_term_investments":61000000000,"total_liabilities":255000000000,"total_current_liabilities":148000000000,"long_term_debt":97000000000,"short_term_debt":11000000000,"total_equity":85000000000},
  {"period_ending":"2023-12-31","fiscal_period":"FY2023","total_assets":332000000000,"total_current_assets":131000000000,"cash_and_short_term_investments":60000000000,"total_liabilities":250000000000,"total_current_liabilities":146000000000,"long_term_debt":99000000000,"short_term_debt":10000000000,"total_equity":82000000000}
])X";
        return MarketDataResult{200, envelope(results), false};
    }

    if (route == "equity/fundamental/cash") {
        const std::string results = R"X([
  {"period_ending":"2025-12-31","fiscal_period":"FY2025","cash_from_operating_activities":120000000000,"cash_from_investing_activities":-25000000000,"cash_from_financing_activities":-95000000000,"capital_expenditure":-12000000000,"free_cash_flow":108000000000},
  {"period_ending":"2024-12-31","fiscal_period":"FY2024","cash_from_operating_activities":115000000000,"cash_from_investing_activities":-22000000000,"cash_from_financing_activities":-92000000000,"capital_expenditure":-11000000000,"free_cash_flow":104000000000},
  {"period_ending":"2023-12-31","fiscal_period":"FY2023","cash_from_operating_activities":110000000000,"cash_from_investing_activities":-21000000000,"cash_from_financing_activities":-89000000000,"capital_expenditure":-11000000000,"free_cash_flow":99000000000}
])X";
        return MarketDataResult{200, envelope(results), false};
    }

    if (route == "equity/fundamental/key_metrics") {
        const std::string results = R"X([
  {"market_cap":2900000000000,"enterprise_value":2950000000000,"pe_ratio":29.5,"forward_pe":27.1,"peg_ratio":2.0,"price_to_book":42.0,"price_to_sales":7.4,"ev_to_sales":7.6,"ev_to_ebitda":22.2,"dividend_yield":0.0050,"payout_ratio":0.16,"beta":1.20,"return_on_equity":1.55,"return_on_assets":0.28,"debt_to_equity":1.18,"current_ratio":0.92,"quick_ratio":0.88,"profit_margin":0.25,"operating_margin":0.30}
])X";
        return MarketDataResult{200, envelope(results), false};
    }

    if (route == "equity/fundamental/multiples") {
        const std::string results = R"X([
  {"pe_ratio_ttm":29.5,"ev_to_ebitda_ttm":22.2,"price_to_sales_ttm":7.4,"price_to_book_quarterly":42.0,"earnings_yield_ttm":0.034,"free_cash_flow_yield_ttm":0.037}
])X";
        return MarketDataResult{200, envelope(results), false};
    }

    if (route == "equity/fundamental/dividends") {
        const std::string results = R"X([
  {"ex_dividend_date":"2026-04-25","amount":0.25,"record_date":"2026-04-26","payment_date":"2026-05-15","declaration_date":"2026-04-10"},
  {"ex_dividend_date":"2026-01-25","amount":0.24,"record_date":"2026-01-26","payment_date":"2026-02-15","declaration_date":"2026-01-10"},
  {"ex_dividend_date":"2025-10-25","amount":0.24,"record_date":"2025-10-26","payment_date":"2025-11-15","declaration_date":"2025-10-10"},
  {"ex_dividend_date":"2025-07-25","amount":0.23,"record_date":"2025-07-26","payment_date":"2025-08-15","declaration_date":"2025-07-10"},
  {"ex_dividend_date":"2025-04-25","amount":0.23,"record_date":"2025-04-26","payment_date":"2025-05-15","declaration_date":"2025-04-10"}
])X";
        return MarketDataResult{200, envelope(results), false};
    }

    if (route == "equity/estimates/consensus") {
        const std::string results = R"X([
  {"symbol":"MOCK","fiscal_period":"Q2","fiscal_year":2026,"eps_avg":1.65,"eps_high":1.78,"eps_low":1.52,"revenue_avg":98000000000,"revenue_high":102000000000,"revenue_low":94000000000,"number_of_analysts":36}
])X";
        return MarketDataResult{200, envelope(results), false};
    }

    if (route == "equity/estimates/surprise") {
        const std::string results = R"X([
  {"symbol":"MOCK","date":"2026-04-30","fiscal_period":"Q1","fiscal_year":2026,"eps_actual":1.71,"eps_estimate":1.62,"eps_surprise":0.09,"surprise_percent":5.55},
  {"symbol":"MOCK","date":"2026-01-31","fiscal_period":"Q4","fiscal_year":2025,"eps_actual":2.18,"eps_estimate":2.10,"eps_surprise":0.08,"surprise_percent":3.81},
  {"symbol":"MOCK","date":"2025-10-30","fiscal_period":"Q3","fiscal_year":2025,"eps_actual":1.49,"eps_estimate":1.55,"eps_surprise":-0.06,"surprise_percent":-3.87},
  {"symbol":"MOCK","date":"2025-07-31","fiscal_period":"Q2","fiscal_year":2025,"eps_actual":1.40,"eps_estimate":1.36,"eps_surprise":0.04,"surprise_percent":2.94},
  {"symbol":"MOCK","date":"2025-04-30","fiscal_period":"Q1","fiscal_year":2025,"eps_actual":1.53,"eps_estimate":1.50,"eps_surprise":0.03,"surprise_percent":2.00}
])X";
        return MarketDataResult{200, envelope(results), false};
    }

    if (route == "news/company") {
        const std::string results = R"X([
  {"title":"MOCK Corp announces Q2 product launch","text":"Synthetic Step-7 mock body for the NI panel.","date":"2026-05-01T13:30:00Z","url":"https://example.com/ni-1","source":"MOCK","symbols":["MOCK"]},
  {"title":"Analyst upgrade lifts MOCK to $200 PT","text":"Synthetic Step-7 mock body.","date":"2026-05-01T12:00:00Z","url":"https://example.com/ni-2","source":"MOCK","symbols":["MOCK"]},
  {"title":"MOCK CFO sells 10k shares in pre-arranged plan","text":"Synthetic Step-7 mock body.","date":"2026-05-01T11:00:00Z","url":"https://example.com/ni-3","source":"MOCK","symbols":["MOCK"]},
  {"title":"Insider buying continues across MOCK suppliers","text":"Synthetic Step-7 mock body.","date":"2026-04-30T19:00:00Z","url":"https://example.com/ni-4","source":"MOCK","symbols":["MOCK"]},
  {"title":"MOCK and partner ink multi-year cloud deal","text":"Synthetic Step-7 mock body.","date":"2026-04-30T16:00:00Z","url":"https://example.com/ni-5","source":"MOCK","symbols":["MOCK"]}
])X";
        return MarketDataResult{200, envelope(results), false};
    }

    if (route == "equity/price/historical") {
        std::string results = "[";
        const int N = 60;
        double price = 150.0;
        bool first = true;
        for (int i = 0; i < N; ++i) {
            const int days_back = N - 1 - i;
            char date[16];
            std::snprintf(date, sizeof(date), "2026-%02d-%02d",
                          ((days_back / 30) ? 3 : 4),
                          1 + (days_back % 28));
            const double bar_drift = (i - N / 2) * 0.15;
            const double noise     = ((i * 31) % 17) / 10.0 - 0.85;
            const double close = price + bar_drift + noise;
            const double open  = close - 0.30;
            const double high  = std::max(open, close) + 0.85;
            const double low   = std::min(open, close) - 0.85;
            const double vol   = 1500000 + ((i * 137) % 700000);
            char buf[256];
            if (!first) results += ",";
            first = false;
            std::snprintf(buf, sizeof(buf),
                R"X({"date":"%s","open":%.2f,"high":%.2f,"low":%.2f,"close":%.2f,"volume":%.0f,"adj_close":%.2f})X",
                date, open, high, low, close, vol, close);
            results += buf;
        }
        results += "]";
        return MarketDataResult{200, envelope(results), false};
    }

    if (route == "equity/profile") {
        std::string sym = "MOCK";
        const std::string needle = "symbol=";
        const auto pos = query.find(needle);
        if (pos != std::string::npos) {
            const auto end = query.find('&', pos);
            sym = query.substr(pos + needle.size(),
                               (end == std::string::npos)
                                 ? std::string::npos
                                 : end - pos - needle.size());
        }
        char buf[1024];
        std::snprintf(buf, sizeof(buf),
            R"X([{"symbol":"%s","name":"%s Corp (mock)","description":"Synthetic Step-7 mock company used by the DES panel for local development without external network access. Replace with live data by leaving OMEGA_MARKETDATA_MOCK unset.","industry":"Software - Infrastructure","sector":"Technology","ipo_date":"2010-01-15","ceo":"Jane Doe","hq_country":"United States","hq_state":"California","hq_city":"Cupertino","employees":154000,"website":"https://example.com","exchange":"NASDAQ","currency":"USD","market_cap":2900000000000}])X",
            sym.c_str(), sym.c_str());
        return MarketDataResult{200, envelope(std::string(buf)), false};
    }

    if (route == "currency/price/quote") {
        std::string symbols = "EURUSD,GBPUSD,USDJPY";
        const std::string needle = "symbol=";
        const auto pos = query.find(needle);
        if (pos != std::string::npos) {
            const auto end = query.find('&', pos);
            symbols = query.substr(pos + needle.size(),
                                   (end == std::string::npos)
                                     ? std::string::npos
                                     : end - pos - needle.size());
        }
        std::string results = "[";
        size_t start = 0;
        bool first = true;
        while (start <= symbols.size()) {
            const auto comma = symbols.find(',', start);
            const std::string sym = symbols.substr(
                start,
                (comma == std::string::npos) ? std::string::npos : comma - start);
            if (!sym.empty()) {
                if (!first) results += ",";
                first = false;
                int seed = 0;
                for (char c : sym) seed = (seed * 31 + (unsigned char)c) & 0xFFF;
                const double price  = 0.5 + (seed % 200) / 100.0;
                const double change = ((seed % 21) - 10) / 10000.0;
                const double pct    = change / price * 100.0;
                char buf[320];
                std::snprintf(buf, sizeof(buf),
                    R"X({"symbol":"%s","name":"%s (mock)","last_price":%.5f,"bid":%.5f,"ask":%.5f,)X"
                    R"X("change":%.5f,"change_percent":%.4f,"volume":%d})X",
                    sym.c_str(), sym.c_str(), price, price - 0.00005, price + 0.00005,
                    change, pct, 250000 + (seed * 11));
                results += buf;
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        results += "]";
        return MarketDataResult{200, envelope(results), false};
    }

    if (route == "crypto/price/quote") {
        std::string symbols = "BTC-USD,ETH-USD,SOL-USD";
        const std::string needle = "symbol=";
        const auto pos = query.find(needle);
        if (pos != std::string::npos) {
            const auto end = query.find('&', pos);
            symbols = query.substr(pos + needle.size(),
                                   (end == std::string::npos)
                                     ? std::string::npos
                                     : end - pos - needle.size());
        }
        std::string results = "[";
        size_t start = 0;
        bool first = true;
        while (start <= symbols.size()) {
            const auto comma = symbols.find(',', start);
            const std::string sym = symbols.substr(
                start,
                (comma == std::string::npos) ? std::string::npos : comma - start);
            if (!sym.empty()) {
                if (!first) results += ",";
                first = false;
                int seed = 0;
                for (char c : sym) seed = (seed * 31 + (unsigned char)c) & 0xFFF;
                const double price  = 100.0 + (seed * 23) % 70000;
                const double change = ((seed % 41) - 20) * 1.5;
                const double pct    = change / price * 100.0;
                const double mcap   = price * (10000000 + (seed * 71));
                char buf[320];
                std::snprintf(buf, sizeof(buf),
                    R"X({"symbol":"%s","name":"%s (mock)","last_price":%.2f,)X"
                    R"X("change":%.2f,"change_percent":%.4f,"volume":%d,"market_cap":%.0f})X",
                    sym.c_str(), sym.c_str(), price, change, pct,
                    1000000 + (seed * 211), mcap);
                results += buf;
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        results += "]";
        return MarketDataResult{200, envelope(results), false};
    }

    return MarketDataResult{200, envelope("[]"), false};
}

} // namespace omega

#endif // OMEGA_BACKTEST

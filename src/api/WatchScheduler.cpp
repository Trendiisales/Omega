// ==============================================================================
// WatchScheduler.cpp
//
// Step 6 of the Omega Terminal build, updated at Step 7 to talk to
// MarketDataProxy (Yahoo Finance + FRED) instead of the retired OpenBbProxy.
// See WatchScheduler.hpp for the design notes. Implementation reuses the
// MarketDataProxy substrate for upstream quote pulls so backtest targets
// stay free of libcurl (this TU is gated on #ifndef OMEGA_BACKTEST and
// MarketDataProxy is similarly gated).
//
// Threading: one std::thread per scheduler. The worker waits on cv_ until
// either (a) next_scan_ms() arrives, (b) trigger_now() is called, or
// (c) stop() is requested. After each wake we run the three universes
// sequentially -- ALL is the union of SP500 and NDX, so we run SP500 and
// NDX separately and synthesize ALL from their hits without re-pulling
// quote data.
//
// JSON parsing: we never pull a third-party JSON lib. The screener works on
// raw envelope bodies via tiny, brace/bracket-aware string scanners
// (extract_results_array + iterate_objects below) -- same idiom used by
// OmegaApiServer.cpp for the merged FA/KEY/EE envelopes. The Step-7
// MarketDataProxy preserves the Step-5/6 envelope shape verbatim, so these
// scanners do not need any changes.
// ==============================================================================

#ifndef OMEGA_BACKTEST

#include "WatchScheduler.hpp"
#include "MarketDataProxy.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>

namespace omega {

namespace {

// ----------------------------------------------------------------------------
// Constituent universes. Baked-in constants; if/when the engine grows a
// canonical universe registry these can switch to reading from there.
// ----------------------------------------------------------------------------

// S&P 500 (top-of-the-list 100 + a representative spread; ~150 entries to
// keep the v1 implementation deployable while the engine-side canonical
// list is wired in. The universe label is "SP500" regardless of count;
// expand the list here without breaking the API contract.)
static const char* const kSP500[] = {
    "AAPL","MSFT","NVDA","GOOGL","GOOG","AMZN","META","BRK-B","TSLA","LLY",
    "AVGO","JPM","V","XOM","UNH","MA","PG","JNJ","HD","COST",
    "MRK","ABBV","ORCL","CVX","ADBE","KO","WMT","BAC","CRM","PEP",
    "AMD","TMO","NFLX","ACN","MCD","LIN","ABT","CSCO","DHR","WFC",
    "DIS","INTU","TXN","VZ","CAT","NEE","PM","IBM","AMGN","CMCSA",
    "QCOM","SPGI","UNP","NOW","RTX","HON","COP","PFE","T","NKE",
    "GS","UPS","SBUX","BLK","BKNG","ELV","INTC","BA","ISRG","LOW",
    "AXP","BMY","MDT","MS","DE","MMC","SCHW","C","ETN","PLD",
    "GILD","ADP","SYK","TJX","ADI","CB","REGN","FI","MO","SO",
    "ZTS","BDX","PGR","CI","DUK","BX","EQIX","ITW","SLB","NOC",
    "CME","KLAC","CL","AON","WM","MMM","ICE","SHW","ATVI","FCX",
    "MAR","APD","MCK","FDX","ECL","TGT","PSX","HCA","HUM","EW",
    "EOG","ROP","PYPL","NSC","EMR","PH","GD","TT","MNST","AEP",
    "ORLY","EXC","ROST","D","CTAS","MPC","KMB","MCO","SRE","AFL",
    "OXY","KMI","ADSK","DOW","KHC","JCI","SPG","HSY","WMB","STZ",
};

static const char* const kNDX[] = {
    "AAPL","MSFT","NVDA","GOOGL","GOOG","AMZN","META","TSLA","AVGO","ADBE",
    "COST","NFLX","AMD","INTC","CSCO","PEP","CMCSA","TMUS","INTU","TXN",
    "QCOM","HON","AMGN","SBUX","BKNG","GILD","ADI","ISRG","REGN","ADP",
    "MDLZ","VRTX","LRCX","KLAC","SNPS","CDNS","PANW","MU","ASML","CHTR",
    "ABNB","ORLY","MAR","PYPL","CRWD","FTNT","CTAS","NXPI","KDP","MNST",
    "ADSK","PCAR","ROST","DXCM","WDAY","FANG","AZN","CSGP","LULU","KHC",
    "ON","TEAM","FAST","ODFL","BKR","EA","IDXX","BIIB","MCHP","MRVL",
    "ZS","CPRT","DDOG","CTSH","VRSK","EXC","XEL","MRNA","CCEP","ANSS",
    "GEHC","GFS","ALGN","TTD","SMCI","ARM","TTWO","WBD","MELI","DLTR",
    "CDW","ROP","DASH","AEP","CEG","SIRI","WBA","ENPH","ILMN","ZBRA",
};

static constexpr size_t kSP500Count = sizeof(kSP500) / sizeof(kSP500[0]);
static constexpr size_t kNDXCount   = sizeof(kNDX)   / sizeof(kNDX[0]);

// ----------------------------------------------------------------------------
// Tiny brace/bracket-aware string scanners (no JSON lib). Identical to the
// helpers in OmegaApiServer.cpp / the retired OpenBbProxy.cpp -- duplicated
// here for TU isolation. Step 7's MarketDataProxy preserves the legacy
// OpenBB-OBBject envelope shape so these scanners do not need to change.
// ----------------------------------------------------------------------------

// Returns the substring of `body` covering the value of the top-level
// "results" array, including the surrounding [ and ]. Returns "[]" on miss.
static std::string extract_results_array(const std::string& body)
{
    const std::string key = "\"results\"";
    auto pos = body.find(key);
    if (pos == std::string::npos) return std::string("[]");
    auto colon = body.find(':', pos + key.size());
    if (colon == std::string::npos) return std::string("[]");
    auto i = colon + 1;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t' || body[i] == '\n' || body[i] == '\r')) i++;
    if (i >= body.size() || body[i] != '[') return std::string("[]");
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    auto start = i;
    for (; i < body.size(); ++i) {
        const char c = body[i];
        if (in_str) {
            if (esc) { esc = false; continue; }
            if (c == '\\') { esc = true; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '[') depth++;
        else if (c == ']') {
            depth--;
            if (depth == 0) return body.substr(start, i - start + 1);
        }
    }
    return std::string("[]");
}

// Walk the comma-separated, brace-balanced top-level objects of an array
// substring (the kind extract_results_array returns). Calls `cb` with each
// object's substring (without the surrounding commas). Skips strings cleanly.
template <class Cb>
static void iterate_objects(const std::string& arr_with_brackets, Cb&& cb)
{
    if (arr_with_brackets.size() < 2) return;
    if (arr_with_brackets.front() != '[' || arr_with_brackets.back() != ']') return;
    const auto end = arr_with_brackets.size() - 1;
    size_t i = 1;
    while (i < end) {
        // Skip whitespace + commas.
        while (i < end && (arr_with_brackets[i] == ' ' || arr_with_brackets[i] == '\t' ||
                           arr_with_brackets[i] == '\n' || arr_with_brackets[i] == '\r' ||
                           arr_with_brackets[i] == ',')) i++;
        if (i >= end) break;
        if (arr_with_brackets[i] != '{') break;
        size_t start = i;
        int depth = 0;
        bool in_str = false;
        bool esc = false;
        for (; i < end; ++i) {
            const char c = arr_with_brackets[i];
            if (in_str) {
                if (esc) { esc = false; continue; }
                if (c == '\\') { esc = true; continue; }
                if (c == '"') in_str = false;
                continue;
            }
            if (c == '"') { in_str = true; continue; }
            if (c == '{') depth++;
            else if (c == '}') {
                depth--;
                if (depth == 0) { i++; break; }
            }
        }
        cb(arr_with_brackets.substr(start, i - start));
    }
}

// Pull a numeric field's value out of a single result object (string
// substring of the form "{...}" containing at least one "key": value pair).
// Returns NaN on miss / parse failure.
static double extract_number(const std::string& obj, const char* key)
{
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    auto pos = obj.find(needle);
    if (pos == std::string::npos) return std::nan("");
    auto colon = obj.find(':', pos + needle.size());
    if (colon == std::string::npos) return std::nan("");
    size_t i = colon + 1;
    while (i < obj.size() && (obj[i] == ' ' || obj[i] == '\t')) i++;
    if (i >= obj.size()) return std::nan("");
    char* end = nullptr;
    const double v = std::strtod(obj.c_str() + i, &end);
    if (end == obj.c_str() + i) return std::nan("");
    return v;
}

// Pull a string field's value (returns empty string on miss).
static std::string extract_string(const std::string& obj, const char* key)
{
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    auto pos = obj.find(needle);
    if (pos == std::string::npos) return std::string();
    auto colon = obj.find(':', pos + needle.size());
    if (colon == std::string::npos) return std::string();
    size_t i = colon + 1;
    while (i < obj.size() && (obj[i] == ' ' || obj[i] == '\t')) i++;
    if (i >= obj.size() || obj[i] != '"') return std::string();
    i++; // skip opening "
    std::string out;
    bool esc = false;
    for (; i < obj.size(); ++i) {
        const char c = obj[i];
        if (esc) { out.push_back(c); esc = false; continue; }
        if (c == '\\') { esc = true; continue; }
        if (c == '"') break;
        out.push_back(c);
    }
    return out;
}

static std::string extract_provider(const std::string& body)
{
    const std::string key = "\"provider\"";
    auto pos = body.find(key);
    if (pos == std::string::npos) return std::string();
    auto colon = body.find(':', pos + key.size());
    if (colon == std::string::npos) return std::string();
    size_t i = colon + 1;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) i++;
    if (i >= body.size() || body[i] != '"') return std::string();
    i++;
    std::string out;
    bool esc = false;
    for (; i < body.size(); ++i) {
        const char c = body[i];
        if (esc) { out.push_back(c); esc = false; continue; }
        if (c == '\\') { esc = true; continue; }
        if (c == '"') break;
        out.push_back(c);
    }
    return out;
}

// ----------------------------------------------------------------------------
// Time helpers
// ----------------------------------------------------------------------------

static int64_t unix_now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Compute the next 00:30 UTC after now_ms. Returns unix ms.
static int64_t compute_next_scan_ms(int64_t now)
{
    using namespace std::chrono;
    const auto now_tp = system_clock::time_point(milliseconds(now));
    const std::time_t t = system_clock::to_time_t(now_tp);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    // Set to today's 00:30 UTC.
    tm_utc.tm_hour = 0;
    tm_utc.tm_min  = 30;
    tm_utc.tm_sec  = 0;
#ifdef _WIN32
    const std::time_t today_0030 = _mkgmtime(&tm_utc);
#else
    const std::time_t today_0030 = timegm(&tm_utc);
#endif
    int64_t scan = static_cast<int64_t>(today_0030) * 1000;
    if (scan <= now) scan += 24LL * 60 * 60 * 1000;
    return scan;
}

static std::string json_escape(const std::string& s)
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
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<int>(c) & 0xFF);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// Default upstream-provider label when no upstream call has yet been made
// (e.g. when /watch is queried before the first nightly scan completes).
// Reads from MarketDataProxy so the value tracks mock vs live cleanly.
static std::string default_provider_label()
{
    return MarketDataProxy::instance().mock_mode() ? "mock" : "yahoo";
}

} // anonymous namespace

// ----------------------------------------------------------------------------
// WatchScheduler implementation
// ----------------------------------------------------------------------------

WatchScheduler::WatchScheduler() = default;

WatchScheduler::~WatchScheduler()
{
    stop();
}

WatchScheduler& WatchScheduler::instance()
{
    static WatchScheduler s_inst;
    return s_inst;
}

int64_t WatchScheduler::now_ms() const     { return unix_now_ms(); }
int64_t WatchScheduler::next_scan_ms() const { return compute_next_scan_ms(unix_now_ms()); }

void WatchScheduler::start()
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    stop_requested_.store(false, std::memory_order_release);
    thread_ = std::thread(&WatchScheduler::worker_loop, this);
    std::cerr << "[WatchScheduler] started; next scan @ " << next_scan_ms() << " ms (UTC unix)\n";
}

void WatchScheduler::stop()
{
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_requested_.store(true, std::memory_order_release);
        cv_.notify_all();
    }
    if (thread_.joinable()) thread_.join();
    std::cerr << "[WatchScheduler] stopped\n";
}

void WatchScheduler::trigger_now()
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        scan_now_.store(true, std::memory_order_release);
        cv_.notify_all();
    }
}

WatchSnapshot WatchScheduler::snapshot(const std::string& universe)
{
    std::string u = universe;
    for (char& c : u) {
        if (c >= 'a' && c <= 'z') c -= 32;
    }
    if (u != "SP500" && u != "NDX" && u != "ALL") u = "SP500";

    std::lock_guard<std::mutex> lk(mu_);
    auto it = registry_.find(u);
    if (it == registry_.end()) {
        WatchSnapshot empty;
        empty.universe    = u;
        empty.last_run_ms = 0;
        empty.next_run_ms = next_scan_ms();
        empty.scanning    = false;
        empty.provider    = default_provider_label();
        return empty;
    }
    return it->second;
}

void WatchScheduler::worker_loop()
{
    while (!stop_requested_.load(std::memory_order_acquire)) {
        const int64_t now  = unix_now_ms();
        const int64_t next = compute_next_scan_ms(now);
        {
            std::unique_lock<std::mutex> lk(mu_);
            // Update the registry's next_run_ms for all known universes so
            // /watch reflects the upcoming tick before the first scan ever runs.
            for (const char* u : { "SP500", "NDX", "ALL" }) {
                auto& slot = registry_[u];
                if (slot.universe.empty()) slot.universe = u;
                slot.next_run_ms = next;
            }
            cv_.wait_for(lk, std::chrono::milliseconds(next - now), [&] {
                return stop_requested_.load(std::memory_order_acquire) ||
                       scan_now_.load(std::memory_order_acquire);
            });
            if (stop_requested_.load(std::memory_order_acquire)) break;
            scan_now_.store(false, std::memory_order_release);
        }

        run_scan("SP500");
        run_scan("NDX");
        // ALL is synthesised from SP500 + NDX without re-pulling.
        {
            std::lock_guard<std::mutex> lk(mu_);
            const auto sit = registry_.find("SP500");
            const auto nit = registry_.find("NDX");
            WatchSnapshot all;
            all.universe = "ALL";
            all.scanning = false;
            all.last_run_ms = unix_now_ms();
            all.next_run_ms = compute_next_scan_ms(all.last_run_ms);
            all.provider = (sit != registry_.end() ? sit->second.provider :
                            (nit != registry_.end() ? nit->second.provider :
                             default_provider_label()));
            std::unordered_set<std::string> seen;
            auto absorb = [&](const WatchSnapshot& s) {
                for (const auto& h : s.hits) {
                    if (seen.insert(h.symbol).second) all.hits.push_back(h);
                }
            };
            if (sit != registry_.end()) absorb(sit->second);
            if (nit != registry_.end()) absorb(nit->second);
            std::sort(all.hits.begin(), all.hits.end(),
                      [](const WatchHit& a, const WatchHit& b) { return a.score > b.score; });
            registry_["ALL"] = std::move(all);
        }
    }
}

void WatchScheduler::run_scan(const std::string& universe)
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto& slot = registry_[universe];
        slot.universe   = universe;
        slot.scanning   = true;
        slot.next_run_ms = compute_next_scan_ms(unix_now_ms());
        // Keep last_run_ms / hits as-is during the scan so /watch can still
        // serve the previous run's results while the new scan is in flight.
    }

    const char* const* list = nullptr;
    size_t count = 0;
    if (universe == "SP500") { list = kSP500; count = kSP500Count; }
    else if (universe == "NDX") { list = kNDX; count = kNDXCount; }
    else return;

    std::vector<WatchHit> hits;
    hits.reserve(64);
    std::string upstream_provider = default_provider_label();

    // Batch in groups of 100 to respect provider rate limits.
    constexpr size_t kBatch = 100;
    for (size_t i = 0; i < count; i += kBatch) {
        const size_t end = std::min(count, i + kBatch);
        std::string symbols;
        for (size_t j = i; j < end; ++j) {
            if (j > i) symbols.push_back(',');
            symbols += list[j];
        }
        std::string qs = "symbol=";
        qs += symbols;
        // The Step-5/6 query suffix &provider=yfinance is no longer meaningful
        // -- MarketDataProxy chooses the upstream by route. Left out.
        const MarketDataResult r = MarketDataProxy::instance().get(
            "equity/price/quote", qs, /*ttl_ms=*/0);
        if (r.status >= 400) continue;
        const std::string up = extract_provider(r.body);
        if (!up.empty()) upstream_provider = up;

        const std::string arr = extract_results_array(r.body);
        iterate_objects(arr, [&](const std::string& obj) {
            const std::string sym = extract_string(obj, "symbol");
            if (sym.empty()) return;
            const double last  = extract_number(obj, "last_price");
            const double pct   = extract_number(obj, "change_percent");
            const double chg   = extract_number(obj, "change");
            const double vol   = extract_number(obj, "volume");
            // v1 screener: |Δ%| ≥ 5 % AND volume ≥ 100k. Score = abs(Δ%).
            const double pct_v = std::isfinite(pct) ? pct : 0.0;
            const double vol_v = std::isfinite(vol) ? vol : 0.0;
            if (std::fabs(pct_v) < 5.0) return;
            if (vol_v < 100000.0) return;
            WatchHit h;
            h.symbol = sym;
            h.signal = pct_v >= 0 ? "MOMO-UP" : "MOMO-DN";
            h.score  = std::fabs(pct_v);
            h.last_price = std::isfinite(last) ? last : 0.0;
            h.change_percent = pct_v;
            h.volume = vol_v;
            h.flagged_at_ms = unix_now_ms();
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "Δ%%=%.2f, change=%.2f, volume=%.0f (v1 momentum + relative-volume rule)",
                          pct_v, std::isfinite(chg) ? chg : 0.0, vol_v);
            h.rationale = buf;
            hits.push_back(std::move(h));
        });
    }
    std::sort(hits.begin(), hits.end(),
              [](const WatchHit& a, const WatchHit& b) { return a.score > b.score; });

    {
        std::lock_guard<std::mutex> lk(mu_);
        auto& slot = registry_[universe];
        slot.hits        = std::move(hits);
        slot.scanning    = false;
        slot.last_run_ms = unix_now_ms();
        slot.next_run_ms = compute_next_scan_ms(slot.last_run_ms);
        slot.provider    = upstream_provider;
    }
    std::cerr << "[WatchScheduler] scan " << universe << " complete; "
              << registry_[universe].hits.size() << " hits\n";
}

// Suppress the json_escape unused-static warning when the helper isn't
// referenced by any active code path (we keep it because OmegaApiServer.cpp
// and related TUs share the same idiom and we want consistency across the
// engine-side API layer).
static void omega_watchscheduler_suppress_unused() {
    (void)&json_escape;
}

} // namespace omega

#endif // OMEGA_BACKTEST

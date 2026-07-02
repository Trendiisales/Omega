// crypto_bear_recovery.cpp -- BearRecovery signal CLI (C++, replaces the
// earlier Python emitter per operator rule "no python in Omega").
//
// Thin shell around include/CryptoBearRecoveryEngine.hpp: feed it daily
// candles on stdin, it prints one JSON line with the regime-ladder state and
// any ENTER/EXIT signal for the last COMPLETED UTC daily bar. Consumed by the
// ~/Crypto book / omega_crypto_bridge.py (paper 4002) or read manually.
//
// Build:  g++ -O2 -std=c++17 -Iinclude -o /tmp/cbr tools/crypto_bear_recovery.cpp
// Usage (Coinbase raw JSON, newest-first, 300 daily candles -- no key needed):
//   curl -s "https://api.exchange.coinbase.com/products/BTC-USD/candles?granularity=86400" \
//     | /tmp/cbr BTC-USD
//   /tmp/cbr BTC-USD --equity 100000 --risk-pct 0.02 < daily.csv   # ts,o,h,l,c also accepted
//
// Output:
//   {"sym":"BTC-USD","regime":"KNIFE","signal":"NONE","asof":"2026-07-01",
//    "close":...,"ema9":...,"sma50":...,"sma200":...,"stop":...,"qty_usd":...,
//    "be_arm_pct":0.02,"be_lock":0.0,"exit_flag":true,"detail":"..."}
// signal: ENTER (reclaim cross fired) | EXIT (close<EMA9) | HOLD-RULES | NONE.
// Protection semantics + backtested verdict: see the engine header
// (ADVERSE-PROTECTION block) and backtest/crypto_bear_bounce/FINDINGS.md.
#include "../include/CryptoBearRecoveryEngine.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

struct Bar { long long ts; double o, h, l, c; };

// Coinbase candle JSON: [[time,low,high,open,close,volume],...] newest-first.
// Minimal parser: pull numbers, group by 6.
static std::vector<Bar> parse_coinbase_json(const std::string& s) {
    std::vector<double> nums;
    const char* p = s.c_str();
    while (*p) {
        if (std::isdigit((unsigned char)*p) || (*p == '-' && std::isdigit((unsigned char)p[1]))) {
            char* end = nullptr;
            nums.push_back(std::strtod(p, &end));
            p = end;
        } else ++p;
    }
    std::vector<Bar> out;
    for (size_t i = 0; i + 5 < nums.size(); i += 6) {
        Bar b; b.ts = (long long)nums[i];
        b.l = nums[i + 1]; b.h = nums[i + 2]; b.o = nums[i + 3]; b.c = nums[i + 4];
        out.push_back(b);
    }
    return out;
}

static std::vector<Bar> parse_csv(const std::string& s) {
    std::vector<Bar> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) nl = s.size();
        Bar b;
        if (std::sscanf(s.c_str() + pos, "%lld,%lf,%lf,%lf,%lf", &b.ts, &b.o, &b.h, &b.l, &b.c) == 5)
            out.push_back(b);
        pos = nl + 1;
    }
    return out;
}

int main(int argc, char** argv) {
    std::string sym = argc > 1 ? argv[1] : "BTC-USD";
    double equity = 100000.0, risk = 0.02;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--equity"))   equity = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--risk-pct")) risk   = std::atof(argv[i + 1]);
    }

    std::string in;
    char buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, stdin)) > 0) in.append(buf, n);
    const size_t first = in.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { std::printf("{\"sym\":\"%s\",\"error\":\"empty stdin\"}\n", sym.c_str()); return 1; }
    auto bars = in[first] == '[' ? parse_coinbase_json(in) : parse_csv(in);
    std::sort(bars.begin(), bars.end(), [](const Bar& a, const Bar& b) { return a.ts < b.ts; });
    bars.erase(std::unique(bars.begin(), bars.end(),
                           [](const Bar& a, const Bar& b) { return a.ts == b.ts; }), bars.end());

    // drop the incomplete current UTC day
    const long long today0 = (long long)(time(nullptr) / 86400) * 86400;
    while (!bars.empty() && bars.back().ts >= today0) bars.pop_back();
    if (bars.size() < 206) {
        std::printf("{\"sym\":\"%s\",\"error\":\"only %zu completed daily bars; need 206\"}\n",
                    sym.c_str(), bars.size());
        return 1;
    }

    omega::CryptoBearRecoveryEngine eng;
    eng.symbol = sym; eng.equity_ref = equity; eng.risk_pct = risk;
    for (const auto& b : bars) eng.seed_bar(b.o, b.h, b.l, b.c, b.ts * 1000LL);
    const auto st = eng.state();

    const char* signal = "NONE"; const char* detail;
    if (!std::strcmp(st.regime, "RECOVERY") && st.enter_signal) {
        signal = "ENTER";
        detail = "EMA9 reclaim in recovery sub-regime; stop=day-low-0.5*ATR; "
                 "BE floor arms at +2% MFE; exit first daily close<EMA9";
    } else if (!std::strcmp(st.regime, "RECOVERY")) {
        signal = st.exit_flag ? "EXIT" : "HOLD-RULES";
        detail = "recovery sub-regime; waiting for EMA9 reclaim cross (>=3d below)";
    } else if (!std::strcmp(st.regime, "KNIFE")) {
        signal = st.exit_flag ? "EXIT" : "NONE";
        detail = "KNIFE phase: FLAT by design -- no long-only entry survives here "
                 "(backtest/crypto_bear_bounce/FINDINGS.md sect.3)";
    } else if (!std::strcmp(st.regime, "BULL")) {
        detail = "BULL regime: Luke daily system owns this phase";
    } else {
        detail = "warming up (needs 206 completed daily bars)";
    }

    time_t bt = (time_t)(st.bar_ts_ms / 1000);
    struct tm g; gmtime_r(&bt, &g);
    std::printf("{\"sym\":\"%s\",\"regime\":\"%s\",\"signal\":\"%s\","
                "\"asof\":\"%04d-%02d-%02d\",\"close\":%.2f,\"ema9\":%.2f,"
                "\"sma50\":%.2f,\"sma200\":%.2f,\"stop\":%.2f,\"qty_usd\":%.2f,"
                "\"be_arm_pct\":0.02,\"be_lock\":0.0,\"exit_flag\":%s,\"detail\":\"%s\"}\n",
                sym.c_str(), st.regime, signal,
                g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
                st.close, st.ema9, st.sma50, st.sma200, st.stop, st.qty_usd,
                st.exit_flag ? "true" : "false", detail);
    return 0;
}

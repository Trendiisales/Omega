// bb_backtest.cpp -- engine-driven backtest harness for BreakBounceEngine.
//
// This is THROWAWAY scaffolding, not the product. It streams a bid/ask tick
// CSV through omega::BreakBounceEngine (the live engine) tick-by-tick and
// collects the TradeRecords the engine emits, so the SAME code path that
// trades live is what gets measured -- no separate sim model to drift.
//
// Build:
//   g++ -O2 -std=c++17 -I../../include bb_backtest.cpp -o bb
// Run:
//   ./bb <ticks.csv> [bias_tf_sec break_tf_sec retest_tf_sec]
//
// Accepts two tick formats:
//   "ts_ms,bid,ask"                       (header row skipped)
//   "YYYYMMDD,HH:MM:SS,bid,ask"           (Dukascopy-style)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "BreakBounceEngine.hpp"

static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

// Parse one CSV line into (ts_ms, bid, ask). Returns false on junk/header.
static bool parse_line(const char* s, int64_t& ts_ms, double& bid, double& ask) {
    // Format A: ts_ms,bid,ask
    if (s[0] >= '0' && s[0] <= '9') {
        // Peek: is the first field a long epoch (>= 1e11) or a YYYYMMDD date?
        char* end = nullptr;
        double f0 = std::strtod(s, &end);
        if (end && *end == ',') {
            if (f0 >= 1e11) {                       // epoch ms
                const char* p = end + 1;
                char* e2 = nullptr;
                bid = std::strtod(p, &e2);
                if (!e2 || *e2 != ',') return false;
                ask = std::strtod(e2 + 1, nullptr);
                ts_ms = (int64_t)f0;
                if (ask < bid) std::swap(bid, ask);
                return bid > 0 && ask > 0;
            }
            // else fall through to date format using the raw string
        } else {
            return false;
        }
    }
    // Format B: YYYYMMDD,HH:MM:SS,bid,ask
    if (std::strlen(s) < 19) return false;
    int y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    int mo = (s[4]-'0')*10 + (s[5]-'0');
    int da = (s[6]-'0')*10 + (s[7]-'0');
    if (s[8] != ',') return false;
    int hh = (s[9]-'0')*10 + (s[10]-'0');
    int mi = (s[12]-'0')*10 + (s[13]-'0');
    int se = (s[15]-'0')*10 + (s[16]-'0');
    if (y < 1971 || mo < 1 || mo > 12 || da < 1 || da > 31) return false;
    const char* p = s + 18;            // after "HH:MM:SS,"
    char* e = nullptr;
    bid = std::strtod(p, &e);
    if (!e || *e != ',') return false;
    ask = std::strtod(e + 1, nullptr);
    int64_t days = days_from_civil(y, (unsigned)mo, (unsigned)da);
    ts_ms = (days*86400 + hh*3600 + mi*60 + se) * 1000LL;
    if (ask < bid) std::swap(bid, ask);
    return bid > 0 && ask > 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::cout << "usage: bb <ticks.csv> [bias break retest sec]\n"; return 1; }

    omega::BreakBounceEngine eng;
    eng.shadow_mode = true;
    if (argc >= 5) {
        eng.BIAS_TF_SEC   = std::atoll(argv[2]);
        eng.BREAK_TF_SEC  = std::atoll(argv[3]);
        eng.RETEST_TF_SEC = std::atoll(argv[4]);
    }
    eng.init();

    std::vector<omega::TradeRecord> trades;
    trades.reserve(4096);
    eng.on_trade_record = [&](const omega::TradeRecord& tr){ trades.push_back(tr); };

    std::ifstream in(argv[1]);
    if (!in.is_open()) { std::cout << "cannot open " << argv[1] << "\n"; return 1; }

    std::string line;
    int64_t nticks = 0;
    double last_bid = 0, last_ask = 0; int64_t last_ts = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        int64_t ts; double bid, ask;
        if (!parse_line(line.c_str(), ts, bid, ask)) continue;
        eng.on_tick(bid, ask, ts);
        last_bid = bid; last_ask = ask; last_ts = ts;
        ++nticks;
    }
    if (eng.has_open_position()) eng.force_close(last_bid, last_ask, last_ts);

    // ---- summary ----
    double net=0, gw=0, gl=0, peak=0, eq=0, mdd=0; int wins=0, L=0, S=0; double hold=0;
    for (auto& t : trades) {
        net += t.pnl; eq += t.pnl;
        if (t.pnl > 0) { gw += t.pnl; wins++; } else { gl += -t.pnl; }
        if (eq > peak) peak = eq;
        if (peak - eq > mdd) mdd = peak - eq;
        if (t.side == "LONG") L++; else S++;
        hold += (t.exitTs - t.entryTs);
    }
    const int n = (int)trades.size();
    const double pf = gl > 0 ? gw/gl : (gw>0?999:0);
    const double wr = n ? 100.0*wins/n : 0;
    const double avg = n ? net/n : 0;
    const double avghold = n ? hold/n/60.0 : 0;

    std::printf("ticks=%lld  TF(bias/break/retest)=%lld/%lld/%lld s\n",
        (long long)nticks, (long long)eng.BIAS_TF_SEC, (long long)eng.BREAK_TF_SEC, (long long)eng.RETEST_TF_SEC);
    std::printf("trades=%d  long=%d short=%d  WR=%.1f%%  PF=%.2f\n", n, L, S, wr, pf);
    std::printf("net=%.2f  avg=%.3f  maxDD=%.2f  avgHold=%.1f min\n", net, avg, mdd, avghold);
    return 0;
}

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
    // Optional session window (UTC hours): argv[5]=start argv[6]=end.
    // start==end (e.g. 0 0) => no session filter (24h).
    if (argc >= 7) {
        const int sh = std::atoi(argv[5]), eh = std::atoi(argv[6]);
        eng.SESSION_START_H = sh; eng.SESSION_END_H = eh;
        eng.USE_SESSION = (sh != eh);
    }
    // Optional MAX_SPREAD override (argv[7], price units) -- needed for
    // cross-symbol runs (gold's 0.60 would reject every index entry).
    if (argc >= 8) eng.MAX_SPREAD = std::atof(argv[7]);
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
    if (trades.empty()) { std::printf("no trades\n"); return 0; }
    int64_t first_entry = trades.front().entryTs, last_exit = trades.back().exitTs;
    for (auto& t : trades) { first_entry = std::min(first_entry, t.entryTs); last_exit = std::max(last_exit, t.exitTs); }
    const int64_t split = first_entry + (int64_t)((last_exit - first_entry) * 0.60);

    auto metrics = [&](int which) {            // 0=full 1=IS 2=OOS
        double net=0, gw=0, gl=0, peak=0, eq=0, mdd=0, sum=0, sum2=0, hold=0;
        int n=0, wins=0; double winsum=0, losssum=0;
        for (auto& t : trades) {
            if (which==1 && t.entryTs >= split) continue;
            if (which==2 && t.entryTs <  split) continue;
            n++; net += t.pnl; eq += t.pnl; sum += t.pnl; sum2 += t.pnl*t.pnl;
            if (t.pnl > 0) { gw += t.pnl; wins++; winsum += t.pnl; } else { gl += -t.pnl; losssum += -t.pnl; }
            if (eq > peak) peak = eq; if (peak - eq > mdd) mdd = peak - eq;
            hold += (t.exitTs - t.entryTs);
        }
        const double mean = n ? sum/n : 0;
        const double var  = n>1 ? (sum2 - sum*sum/n)/(n-1) : 0;
        const double sd   = var>0 ? std::sqrt(var) : 0;
        const double span_days = (last_exit - first_entry)/86400.0;
        const double tpy  = span_days>0 ? n/(span_days/365.25) : 0;
        const double sharpe = sd>0 ? (mean/sd)*std::sqrt(tpy) : 0;
        const double pf = gl>0 ? gw/gl : (gw>0?999:0);
        const double wr = n ? 100.0*wins/n : 0;
        const double avgW = wins ? winsum/wins : 0;
        const double avgL = (n-wins) ? losssum/(n-wins) : 0;
        const double payoff = avgL>0 ? avgW/avgL : 0;
        const char* lbl = which==0?"FULL":which==1?"IS  ":"OOS ";
        std::printf("%-4s | %5d | %5.1f | %5.2f | %6.2f | %7.1f | %6.3f | %6.2f | %6.2f | %5.2f | %6.1f\n",
            lbl, n, wr, pf, sharpe, net, mean, avgW, avgL, payoff, mdd);
    };

    std::printf("BreakBounce XAUUSD  TF=%lld/%lld/%lld s  STOP_ATR=%.1f RR=%.1f  (price-points / 1 lot)\n",
        (long long)eng.BIAS_TF_SEC,(long long)eng.BREAK_TF_SEC,(long long)eng.RETEST_TF_SEC, eng.STOP_ATR, eng.REWARD_RISK);
    std::printf("seg  | trades |  WR%% |  PF  | Sharpe |   net   |  avg   | avgWin | avgLos | payf |  maxDD\n");
    std::printf("-----+--------+------+------+--------+---------+--------+--------+--------+------+-------\n");
    metrics(0); metrics(1); metrics(2);
    std::printf("(Sharpe = per-trade mean/sd annualized by trades/yr; ~%.0f trades/yr)\n",
        trades.size()/(((last_exit-first_entry)/86400.0)/365.25));
    return 0;
}

// =============================================================================
// xau_jpy_trend_audit.cpp -- XAU/JPY 50/200 SMA trend signal evaluation.
//
// Hypothesis (research agent task #20):
//   XAU/JPY structural long: BoJ tightening lags Fed, yen weak. Trend-follow
//   the synthetic XAU/JPY cross with 50/200 SMA crossover. Code as XAUUSD
//   directional trade gated by XAUJPY trend regime.
//
// Method:
//   1. Load XAUUSD H4 close series + USDJPY H4 close series.
//   2. Synthesize XAU/JPY = XAUUSD * USDJPY at matching timestamps.
//   3. Aggregate to daily closes (UTC midnight).
//   4. Compute SMA50, SMA200 on the synthetic XAU/JPY series.
//   5. Signal: regime = LONG when SMA50 > SMA200, FLAT otherwise.
//   6. Trade XAUUSD: enter long at next daily close when regime flips to LONG,
//      exit when regime flips back. Long-only (paper baseline).
//   7. Cost: $0.30 RT spread per round-trip (BlackBull XAU live estimate).
//
// Output:
//   Net-of-cost Sharpe, n trades, win rate, gross PnL in XAUUSD points.
//   Per-quarter breakdown. Verdict on whether to ship as
//   XauJpyTrendH4Engine.
//
// Build:  cmake --build build --target xau_jpy_trend_audit
// Run:    ./build/xau_jpy_trend_audit /Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv \
//                                     /Users/jo/Tick/USDJPY_merged.h4.csv
// =============================================================================

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

struct H4Bar { int64_t ts_sec; double o, h, l, c; };

static std::vector<H4Bar> load_h4(const std::string& path) {
    std::vector<H4Bar> bars;
    std::ifstream f(path);
    if (!f) return bars;
    std::string line; std::getline(f, line);
    while (std::getline(f, line)) {
        H4Bar b; const char* pp = line.c_str(); char* e;
        b.ts_sec = std::strtoll(pp, &e, 10); if (*e == ',') pp = e + 1;
        b.o = std::strtod(pp, &e); if (*e == ',') pp = e + 1;
        b.h = std::strtod(pp, &e); if (*e == ',') pp = e + 1;
        b.l = std::strtod(pp, &e); if (*e == ',') pp = e + 1;
        b.c = std::strtod(pp, &e);
        if (b.ts_sec > 10'000'000'000LL) b.ts_sec /= 1000;
        bars.push_back(b);
    }
    return bars;
}

struct DailyClose { int64_t day_utc; double xau; double jpy; double xaujpy; };

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <xau_h4_csv> <usdjpy_h4_csv> [report_file]\n", argv[0]);
        return 1;
    }
    const std::string xau_path = argv[1];
    const std::string jpy_path = argv[2];
    const std::string rpt_path = (argc >= 4) ? argv[3] : std::string("/tmp/xau_jpy_trend.txt");

    auto xau_bars = load_h4(xau_path);
    auto jpy_bars = load_h4(jpy_path);
    std::fprintf(stderr, "[XJP] xau=%zu jpy=%zu bars\n", xau_bars.size(), jpy_bars.size());
    if (xau_bars.empty() || jpy_bars.empty()) return 2;

    // Build day_utc -> last H4 close maps.
    std::unordered_map<int64_t, double> xau_close, jpy_close;
    for (auto& b : xau_bars) xau_close[b.ts_sec / 86400] = b.c;
    for (auto& b : jpy_bars) jpy_close[b.ts_sec / 86400] = b.c;

    // Daily series at days where BOTH exist.
    std::vector<DailyClose> daily;
    for (auto& kv : xau_close) {
        auto it = jpy_close.find(kv.first);
        if (it == jpy_close.end()) continue;
        DailyClose d;
        d.day_utc = kv.first;
        d.xau = kv.second;
        d.jpy = it->second;
        d.xaujpy = d.xau * d.jpy;
        daily.push_back(d);
    }
    std::sort(daily.begin(), daily.end(),
              [](const DailyClose& a, const DailyClose& b){ return a.day_utc < b.day_utc; });
    std::fprintf(stderr, "[XJP] daily overlapping=%zu (range %lld..%lld)\n",
                 daily.size(),
                 (long long)(daily.empty() ? 0 : daily.front().day_utc),
                 (long long)(daily.empty() ? 0 : daily.back().day_utc));
    if (daily.size() < 250) {
        std::fprintf(stderr, "[XJP] insufficient overlap (<250d). SMA200 needs ~9mo.\n");
        return 3;
    }

    // SMA50 / SMA200 + signal flip detection.
    const int SMA_FAST = 50;
    const int SMA_SLOW = 200;
    const double RT_COST_XAU = 0.30;   // per round-trip slip in XAUUSD price units

    std::vector<double> fast_sma(daily.size(), 0.0);
    std::vector<double> slow_sma(daily.size(), 0.0);

    double sum_fast = 0, sum_slow = 0;
    for (size_t i = 0; i < daily.size(); ++i) {
        sum_fast += daily[i].xaujpy;
        sum_slow += daily[i].xaujpy;
        if ((int)i >= SMA_FAST) sum_fast -= daily[i - SMA_FAST].xaujpy;
        if ((int)i >= SMA_SLOW) sum_slow -= daily[i - SMA_SLOW].xaujpy;
        fast_sma[i] = ((int)i + 1 >= SMA_FAST) ? sum_fast / SMA_FAST : 0.0;
        slow_sma[i] = ((int)i + 1 >= SMA_SLOW) ? sum_slow / SMA_SLOW : 0.0;
    }

    struct Trade { int64_t entry_day, exit_day; double entry_px, exit_px, pnl; };
    std::vector<Trade> trades;
    bool in_pos = false;
    Trade cur{};

    for (size_t i = SMA_SLOW; i < daily.size(); ++i) {
        const bool bull = fast_sma[i] > slow_sma[i] && fast_sma[i] > 0.0;
        if (!in_pos && bull) {
            // Enter at NEXT day close
            if (i + 1 < daily.size()) {
                cur.entry_day = daily[i + 1].day_utc;
                cur.entry_px  = daily[i + 1].xau;
                in_pos = true;
            }
        } else if (in_pos && !bull) {
            // Exit at NEXT day close
            if (i + 1 < daily.size()) {
                cur.exit_day = daily[i + 1].day_utc;
                cur.exit_px  = daily[i + 1].xau;
                cur.pnl      = (cur.exit_px - cur.entry_px) - RT_COST_XAU;
                trades.push_back(cur);
                in_pos = false;
                cur = Trade{};
            }
        }
    }
    if (in_pos) {
        cur.exit_day = daily.back().day_utc;
        cur.exit_px  = daily.back().xau;
        cur.pnl      = (cur.exit_px - cur.entry_px) - RT_COST_XAU;
        trades.push_back(cur);
    }

    // Aggregate
    int n = (int)trades.size(), wins = 0;
    double gross = 0, sum_sq = 0, worst = 0, peak = 0, running = 0, mdd = 0;
    for (auto& t : trades) {
        if (t.pnl > 0) ++wins;
        gross += t.pnl;
        sum_sq += t.pnl * t.pnl;
        if (t.pnl < worst) worst = t.pnl;
        running += t.pnl;
        if (running > peak) peak = running;
        if (peak - running > mdd) mdd = peak - running;
    }
    const double mean = n ? gross / n : 0.0;
    const double var = (n > 1) ? (sum_sq - n * mean * mean) / (n - 1) : 0.0;
    const double sd = var > 0 ? std::sqrt(var) : 0.0;
    // Approx ann: trades/yr * (mean / sd). XAU/JPY 50/200 typ ~3-6 trades/yr.
    const double tr_per_yr = n > 0
        ? (n * 365.0 / (daily.back().day_utc - daily.front().day_utc))
        : 0.0;
    const double sharpe_ann = (sd > 1e-9) ? (mean / sd) * std::sqrt(tr_per_yr) : 0.0;

    FILE* RPT = std::fopen(rpt_path.c_str(), "w");
    if (!RPT) RPT = stderr;
    std::fprintf(RPT, "==============================================================\n");
    std::fprintf(RPT, "  xau_jpy_trend_audit  --  XAU/JPY 50/200 SMA -> XAUUSD trade\n");
    std::fprintf(RPT, "  xau=%s\n  jpy=%s\n", xau_path.c_str(), jpy_path.c_str());
    std::fprintf(RPT, "  overlapping daily=%zu  RT cost=$%.2f per trade\n",
                 daily.size(), RT_COST_XAU);
    std::fprintf(RPT, "==============================================================\n");
    std::fprintf(RPT, "OVERALL\n");
    std::fprintf(RPT, "  n=%-4d  WR=%.2f%%  mean=%+.3f  std=%.3f  Sharpe_ann=%+.3f  gross=%+.2f  MaxDD=%.2f  worst=%+.3f\n",
                 n, n ? 100.0 * wins / n : 0.0, mean, sd, sharpe_ann, gross, mdd, worst);
    std::fprintf(RPT, "  approx %.2f trades/yr\n", tr_per_yr);
    std::fprintf(RPT, "\nTRADE JOURNAL\n");
    for (size_t i = 0; i < trades.size(); ++i) {
        const auto& t = trades[i];
        const time_t ts = t.entry_day * 86400;
        std::tm tm{}; gmtime_r(&ts, &tm);
        const time_t ts2 = t.exit_day * 86400;
        std::tm tm2{}; gmtime_r(&ts2, &tm2);
        std::fprintf(RPT, "  #%-2zu  %04d-%02d-%02d -> %04d-%02d-%02d  entry=%.2f exit=%.2f pnl=%+.3f\n",
                     i + 1,
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm2.tm_year + 1900, tm2.tm_mon + 1, tm2.tm_mday,
                     t.entry_px, t.exit_px, t.pnl);
    }
    if (RPT != stderr) std::fclose(RPT);
    std::fprintf(stderr, "[XJP] n=%d  Sharpe=%+.3f  gross=%+.2f  -> %s\n",
                 n, sharpe_ann, gross, rpt_path.c_str());
    return 0;
}

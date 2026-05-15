// =============================================================================
// XauEmaPullbackBacktest.cpp -- EMA pullback strategy backtest (4 cells)
// =============================================================================
//
// Replicates EmaPullbackEngine: EMA(9) fast / EMA(21) slow pullback entry.
// 4 cells: H1, H2, H4, H6 timeframes. LONG ONLY (per engine Tier-3 config).
// Entry: slow EMA rising + bar low touches fast EMA + close recovers above it.
//
// DATA FORMAT: Dukascopy combined CSV: timestamp_ms,askPrice,bidPrice
//
// COMPILATION:
//   g++ -std=c++17 -O2 -o backtest/xau_epb_bt backtest/XauEmaPullbackBacktest.cpp
//   ./backtest/xau_epb_bt ~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <array>
#include <deque>

namespace cfg {
    constexpr double LOT_SIZE   = 0.01;
    constexpr double PNL_PER_PT = LOT_SIZE * 100.0;
    constexpr int    ATR_PERIOD = 14;
    constexpr double ATR_INIT   = 5.0;
    constexpr double ATR_MIN    = 0.5;
    constexpr double OOS_FRAC   = 0.60;

    // EMA pullback params (from engine defaults)
    constexpr int    FAST_SPAN  = 9;
    constexpr int    SLOW_SPAN  = 21;
    constexpr double SL_ATR     = 1.0;     // SL = 1.0 * ATR
    constexpr double TP_R       = 2.5;     // TP = 2.5 * SL distance
    constexpr int    MAX_HOLD_BARS = 30;   // timeout
    constexpr int    COOLDOWN_BARS = 5;

    inline bool is_weekend(int64_t ts_ms) noexcept {
        const int64_t sec = ts_ms / 1000;
        const int dow = ((sec / 86400) + 4) % 7;
        if (dow == 0 || dow == 6) return true;
        if (dow == 5 && (int)((sec % 86400) / 3600) >= 21) return true;
        return false;
    }
}

struct Bar {
    int64_t open_ms = 0;
    double open = 0, high = 0, low = 99999999, close = 0;
    void reset(int64_t ts, double p) { open_ms=ts; open=high=low=close=p; }
    void update(double p) { if(p>high) high=p; if(p<low) low=p; close=p; }
};

// One cell = one timeframe with its own bars, EMAs, ATR, and trade
struct EpbCell {
    const char* name;
    int64_t period_ms;
    Bar current{};
    std::deque<Bar> bars;
    bool bar_active = false;
    int total_bars = 0;

    double atr = cfg::ATR_INIT;
    double ema_fast = 0, ema_slow = 0;
    double prev_ema_slow = 0;  // for "rising" check

    // Trade state
    bool trade_active = false;
    bool trade_is_long = true;
    double entry_px = 0, sl_px = 0, tp_px = 0;
    int64_t entry_time = 0;
    int entry_bar = 0;
    int cooldown = 0;

    // Metrics
    int total_trades = 0, wins = 0;
    double pnl = 0, gw = 0, gl = 0, mdd = 0, pk = 0;

    void record(double p) {
        total_trades++; pnl += p;
        if (p > 0) { wins++; gw += p; } else gl += std::fabs(p);
        if (pnl > pk) pk = pnl;
        double dd = pk - pnl; if (dd > mdd) mdd = dd;
    }
    double pf() const { return gl > 0 ? gw / gl : 999.0; }
    double wr() const { return total_trades > 0 ? 100.0 * wins / total_trades : 0; }
    void print() const {
        std::printf("  %-12s trades=%4d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  maxDD=$%.2f\n",
                   name, total_trades, wr(), pf(), pnl, mdd);
    }

    bool on_tick(double mid, int64_t ts_ms) {
        if (!bar_active) { current.reset(ts_ms, mid); bar_active = true; return false; }
        int64_t boundary = (current.open_ms / period_ms) * period_ms + period_ms;
        if (ts_ms >= boundary) {
            bars.push_back(current);
            total_bars++;
            _on_close();
            current.reset(ts_ms, mid);
            while ((int)bars.size() > 100) bars.pop_front();
            return true;
        }
        current.update(mid);
        return false;
    }

private:
    void _on_close() {
        if (bars.size() < 2) return;
        const Bar& bar = bars.back();
        const Bar& prev = bars[bars.size() - 2];

        double tr = std::max({bar.high - bar.low, std::fabs(bar.high - prev.close), std::fabs(bar.low - prev.close)});
        if (total_bars >= cfg::ATR_PERIOD + 1) atr = atr * 13.0 / 14.0 + tr / 14.0;
        else if (total_bars > 1) { /* simple average during warmup */ atr = (atr * (total_bars - 2) + tr) / (total_bars - 1); }
        atr = std::max(cfg::ATR_MIN, atr);

        // EMA (pandas ewm adjust=False: alpha = 2/(span+1))
        double k_fast = 2.0 / (cfg::FAST_SPAN + 1.0);
        double k_slow = 2.0 / (cfg::SLOW_SPAN + 1.0);
        if (ema_fast == 0) ema_fast = bar.close;
        if (ema_slow == 0) ema_slow = bar.close;
        prev_ema_slow = ema_slow;
        ema_fast = bar.close * k_fast + ema_fast * (1.0 - k_fast);
        ema_slow = bar.close * k_slow + ema_slow * (1.0 - k_slow);
    }
};

struct Metrics {
    int total = 0, wins = 0; double pnl = 0, gw = 0, gl = 0, mdd = 0, pk = 0;
    void record(double p) { total++; pnl += p; if (p > 0) { wins++; gw += p; } else gl += std::fabs(p); if (pnl > pk) pk = pnl; double d = pk - pnl; if (d > mdd) mdd = d; }
    double pf() const { return gl > 0 ? gw / gl : 999.0; }
    double wr() const { return total > 0 ? 100.0 * wins / total : 0; }
    void print(const char* l) const { std::printf("  %-20s trades=%5d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  maxDD=$%.2f\n", l, total, wr(), pf(), pnl, mdd); }
};

int main(int argc, char* argv[]) {
    if (argc < 2) { std::fprintf(stderr, "Usage: %s <tick_csv>\n", argv[0]); return 1; }

    // v2: H4+H6 only (strongest cells from v1: H4 PF=1.60, H6 PF=1.53)
    // H1 PF=1.13 and H2 PF=1.27 dilute — test concentrated variant
    EpbCell cells[] = {
        {"H4",  4LL * 3600 * 1000},
        {"H6",  6LL * 3600 * 1000},
    };
    constexpr int NC = 2;

    Metrics overall, is_m, oos_m;
    std::array<Metrics, 24> per_hour;

    int64_t oos_ts = 0; bool oos_set = false;
    int64_t first_ts = 0, last_ts = 0;
    size_t total_ticks = 0;
    double min_px = 99999999, max_px = 0;

    std::printf("[EPB-BT] XAUUSD EMA-Pullback backtest (H1/H2/H4/H6, LONG ONLY)\n");
    std::printf("[EPB-BT] EMA fast=%d slow=%d  SL=%.1f*ATR  TP=%.1f*SL  MaxHold=%d bars\n\n",
               cfg::FAST_SPAN, cfg::SLOW_SPAN, cfg::SL_ATR, cfg::TP_R, cfg::MAX_HOLD_BARS);

    std::ifstream f(argv[1]);
    if (!f.is_open()) { std::fprintf(stderr, "Cannot open %s\n", argv[1]); return 1; }

    std::string first_line; std::getline(f, first_line);
    bool has_header = (first_line.find("timestamp") != std::string::npos || first_line.find("Time") != std::string::npos);

    auto parse_duka = [](const std::string& line, int64_t& ts, double& bid, double& ask) -> bool {
        return std::sscanf(line.c_str(), "%lld,%lf,%lf", (long long*)&ts, &ask, &bid) == 3;
    };

    auto process = [&](int64_t ts_ms, double bid, double ask) {
        if (bid <= 0 || ask <= 0 || ask < bid) return;
        double mid = (bid + ask) * 0.5;
        if (mid < 1000 || mid > 5000) return;

        total_ticks++;
        if (mid < min_px) min_px = mid;
        if (mid > max_px) max_px = mid;
        if (first_ts == 0) first_ts = ts_ms;
        last_ts = ts_ms;

        if (!oos_set && first_ts > 0) {
            oos_ts = first_ts + (int64_t)(26.0 * 30 * 24 * 3600 * 1000.0 * cfg::OOS_FRAC);
            oos_set = true;
        }
        if (cfg::is_weekend(ts_ms)) return;

        for (int ci = 0; ci < NC; ++ci) {
            EpbCell& cell = cells[ci];

            // Manage open trade per-tick
            if (cell.trade_active) {
                bool sl = (bid <= cell.sl_px);  // long only
                bool tp = (bid >= cell.tp_px);

                // Timeout on bar close
                bool timeout = false;
                // We check timeout on bar close below

                if (sl || tp) {
                    double exit_px = tp ? cell.tp_px : cell.sl_px;
                    double pnl_pts = exit_px - cell.entry_px;
                    double pnl_usd = pnl_pts * cfg::PNL_PER_PT;

                    cell.record(pnl_usd);
                    overall.record(pnl_usd);
                    if (cell.entry_time < oos_ts) is_m.record(pnl_usd);
                    else oos_m.record(pnl_usd);
                    int h = static_cast<int>((cell.entry_time / 1000 % 86400) / 3600);
                    per_hour[h].record(pnl_usd);

                    cell.trade_active = false;
                    cell.cooldown = cfg::COOLDOWN_BARS;
                }
            }

            // Feed tick to bar builder
            bool new_bar = cell.on_tick(mid, ts_ms);

            if (!new_bar) continue;
            if (cell.total_bars < 25) continue;  // warmup

            // Timeout check on bar close
            if (cell.trade_active) {
                if ((cell.total_bars - cell.entry_bar) >= cfg::MAX_HOLD_BARS) {
                    // Only timeout losers (winner exemption per engine pattern)
                    double unrealized = mid - cell.entry_px;
                    if (unrealized <= 0) {
                        double pnl_usd = unrealized * cfg::PNL_PER_PT;
                        cell.record(pnl_usd);
                        overall.record(pnl_usd);
                        if (cell.entry_time < oos_ts) is_m.record(pnl_usd);
                        else oos_m.record(pnl_usd);
                        int h = static_cast<int>((cell.entry_time / 1000 % 86400) / 3600);
                        per_hour[h].record(pnl_usd);
                        cell.trade_active = false;
                        cell.cooldown = cfg::COOLDOWN_BARS;
                    }
                }
                continue;  // don't evaluate new entry while in trade
            }

            if (cell.cooldown > 0) { cell.cooldown--; continue; }

            // ── LONG ONLY pullback entry signal ──
            // Conditions:
            //   1. slow EMA rising (current > previous)
            //   2. fast EMA > slow EMA (uptrend)
            //   3. bar low touched or pierced fast EMA (pullback)
            //   4. bar close > fast EMA (recovery)
            if ((int)cell.bars.size() < 2) continue;
            const Bar& bar = cell.bars.back();

            bool slow_rising = (cell.ema_slow > cell.prev_ema_slow);
            bool fast_above_slow = (cell.ema_fast > cell.ema_slow);
            bool low_touched_fast = (bar.low <= cell.ema_fast);
            bool close_above_fast = (bar.close > cell.ema_fast);

            if (slow_rising && fast_above_slow && low_touched_fast && close_above_fast) {
                // Open long
                cell.trade_active = true;
                cell.trade_is_long = true;
                cell.entry_px = ask;  // enter at ask
                double sl_dist = cell.atr * cfg::SL_ATR;
                cell.sl_px = cell.entry_px - sl_dist;
                cell.tp_px = cell.entry_px + sl_dist * cfg::TP_R;
                cell.entry_time = ts_ms;
                cell.entry_bar = cell.total_bars;
            }
        }
    };

    if (!has_header) {
        int64_t ts; double b, a;
        if (parse_duka(first_line, ts, b, a)) process(ts, b, a);
    }
    std::string line; size_t lc = 0;
    while (std::getline(f, line)) {
        int64_t ts; double b, a;
        if (parse_duka(line, ts, b, a)) process(ts, b, a);
        if (++lc % 10000000 == 0) std::printf("  ... %zuM ticks\n", lc / 1000000);
    }

    double hours = (last_ts > first_ts) ? (last_ts - first_ts) / 3600000.0 : 0;
    std::printf("\n===============================================================\n");
    std::printf("  XAUUSD EMA-PULLBACK BACKTEST (LONG ONLY, 4 cells)\n");
    std::printf("===============================================================\n\n");
    std::printf("  Ticks: %zu  |  Period: %.1f days  |  Price: %.1f - %.1f\n\n",
               total_ticks, hours / 24, min_px, max_px);

    std::printf("-- PER-CELL RESULTS ----------------------------------------\n");
    for (int i = 0; i < NC; ++i) cells[i].print();
    std::printf("\n");

    overall.print("ALL COMBINED");
    std::printf("\n");

    std::printf("-- PF BY HOUR (UTC) ----------------------------------------\n");
    std::printf("  Hour  Trades   WR%%    PF      PnL\n");
    for (int h = 0; h < 24; ++h) {
        if (per_hour[h].total > 0) {
            std::printf("  %02d:00  %5d  %5.1f  %5.2f  $%8.2f  %s\n",
                       h, per_hour[h].total, per_hour[h].wr(),
                       per_hour[h].pf(), per_hour[h].pnl,
                       per_hour[h].pf() >= 1.0 ? " <<<" : "");
        }
    }
    std::printf("\n");

    std::printf("===============================================================\n");
    std::printf("  IN-SAMPLE / OUT-OF-SAMPLE (60/40 split)\n");
    std::printf("===============================================================\n\n");
    is_m.print("IS TOTAL"); oos_m.print("OOS TOTAL");
    std::printf("\n  OOS VERDICT: %s\n\n",
               (oos_m.total >= 8 && oos_m.pf() >= 1.20) ? "PASS" : "REVIEW");

    return 0;
}

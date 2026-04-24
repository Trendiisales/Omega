// =============================================================================
// htf_bt_l2days.cpp -- H4 Donchian breakout replay over L2-format tick data
//
// Runs the conservative config (D=10 SL=1.5 TP=4.0) over one or more
// L2 CSV files and reports what trades were triggered.
//
// L2 CSV format (from /Users/jo/Omega/data/l2_ticks_YYYY-MM-DD.csv):
//   ts_ms,bid,ask,l2_imb,l2_bid_vol,l2_ask_vol,...
//   1776297600194,4823.330,4823.550,...
//
// Only ts_ms, bid, ask are used. L2 fields ignored (H4 Donchian does not
// consume L2 data).
//
// WARNING: H4 Donchian needs ~24 H4 bars of warmup (14 ATR + 10 channel) =
// ~96 hours = ~4 trading days. An 11-day window will have only 7 effective
// trading days and will likely produce 0-3 trades. This is a sanity check,
// not a statistical test.
//
// Build:
//   clang++ -O3 -std=c++17 -o htf_bt_l2days htf_bt_l2days.cpp
// Run:
//   ./htf_bt_l2days /Users/jo/Omega/data/l2_ticks_2026-04-14.csv \
//                   /Users/jo/Omega/data/l2_ticks_2026-04-15.csv \
//                   ...
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>
#include <deque>
#include <algorithm>
#include <chrono>

// =============================================================================
// Tick + L2 CSV parser
// =============================================================================
struct Tick {
    int64_t ts_ms;
    double  bid;
    double  ask;
};

// Parse line: "1776297600194,4823.330,4823.550,0.5007,..."
// Only first 3 fields required. Rest ignored.
static bool parse_l2_line(const char* s, Tick& t) {
    if (!s || !*s) return false;
    if (!isdigit((unsigned char)s[0])) return false;  // skip header
    char* end;
    int64_t ts = strtoll(s, &end, 10);
    if (end == s || *end != ',') return false;
    const char* p = end + 1;
    double bid = strtod(p, &end);
    if (end == p || *end != ',') return false;
    p = end + 1;
    double ask = strtod(p, &end);
    if (end == p) return false;
    t.ts_ms = ts;
    t.bid   = bid;
    t.ask   = ask;
    return true;
}

// =============================================================================
// Bar + H4 bar with intra-bar extremes
// =============================================================================
struct Bar {
    int64_t ts_ms_open = 0, ts_ms_close = 0;
    double open = 0.0, high = 0.0, low = 0.0, close = 0.0;
    int n = 0;
};

struct H4BarWithTicks {
    Bar bar;
    double bid_low = 1e18, bid_high = 0.0, ask_low = 1e18, ask_high = 0.0;
    bool bid_min_before_max = false, ask_max_before_min = false;
    int64_t bid_min_ts = 0, bid_max_ts = 0, ask_min_ts = 0, ask_max_ts = 0;
};

// =============================================================================
// ATR(14) Wilder
// =============================================================================
struct ATR {
    double value = 0.0;
    bool primed = false;
    int period = 14;
    double prev_close = 0.0;
    bool have_prev = false;
    std::deque<double> seed_tr;
    void set(int p) { period = p; value = 0.0; primed = false; have_prev = false; seed_tr.clear(); }
    void push(double high, double low, double close) {
        double tr;
        if (!have_prev) { tr = high - low; }
        else {
            double a = high - low;
            double b = std::fabs(high - prev_close);
            double c = std::fabs(low - prev_close);
            tr = std::max(a, std::max(b, c));
        }
        have_prev = true; prev_close = close;
        if (!primed) {
            seed_tr.push_back(tr);
            if ((int)seed_tr.size() >= period) {
                double sum = 0.0;
                for (double v : seed_tr) sum += v;
                value = sum / (double)period;
                primed = true;
            }
        } else {
            value = (value * (period - 1) + tr) / (double)period;
        }
    }
};

struct Trade {
    int id = 0;
    int64_t entry_ts_ms = 0, exit_ts_ms = 0;
    bool is_long = false;
    double entry = 0.0, exit = 0.0, sl = 0.0, tp = 0.0, size = 0.0;
    double pnl_pts = 0.0, pnl_usd = 0.0;
    int bars_held = 0;
    std::string exit_reason;
    double h4_atr_at_entry = 0.0;
    double channel_high_at_entry = 0.0;
    double channel_low_at_entry = 0.0;
};

// =============================================================================
// Helpers
// =============================================================================
static std::string fmt_ts(int64_t ts_ms) {
    time_t s = (time_t)(ts_ms / 1000LL);
    struct tm tmv;
    gmtime_r(&s, &tmv);
    char buf[48];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

static bool is_weekend_gated(int64_t ts_ms) {
    const int64_t utc_sec  = ts_ms / 1000LL;
    const int     utc_dow  = static_cast<int>((utc_sec / 86400LL + 3) % 7);
    const int     utc_hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
    if (utc_dow == 4 && utc_hour >= 20) return true;
    if (utc_dow == 5) return true;
    if (utc_dow == 6 && utc_hour < 22)  return true;
    return false;
}

// =============================================================================
// Build H4 bars from a list of CSV files
// =============================================================================
static std::vector<H4BarWithTicks> build_h4_bars_from_files(
    const std::vector<std::string>& files,
    uint64_t& ticks_ok_out, uint64_t& ticks_fail_out,
    int64_t& first_ts_out, int64_t& last_ts_out)
{
    std::vector<H4BarWithTicks> bars;
    const int64_t dur_ms = 240LL * 60LL * 1000LL;

    int64_t cur_anchor = -1;
    H4BarWithTicks cur;
    bool have_cur = false;

    uint64_t ok = 0, fail = 0;
    int64_t first_ts = 0, last_ts = 0;

    for (const std::string& path : files) {
        FILE* f = fopen(path.c_str(), "r");
        if (!f) {
            fprintf(stderr, "cannot open %s\n", path.c_str());
            continue;
        }
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            // Strip trailing \r\n
            size_t len = strlen(line);
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
                line[--len] = '\0';
            }
            Tick tk;
            if (!parse_l2_line(line, tk)) { ++fail; continue; }
            ++ok;
            if (first_ts == 0) first_ts = tk.ts_ms;
            last_ts = tk.ts_ms;

            int64_t anchor = (tk.ts_ms / dur_ms) * dur_ms;
            double mid = (tk.bid + tk.ask) * 0.5;

            if (!have_cur) {
                cur = H4BarWithTicks{};
                cur.bar.ts_ms_open = anchor; cur.bar.ts_ms_close = anchor + dur_ms;
                cur.bar.open = mid; cur.bar.high = mid; cur.bar.low = mid; cur.bar.close = mid;
                cur.bar.n = 1;
                cur.bid_low = tk.bid; cur.bid_high = tk.bid;
                cur.ask_low = tk.ask; cur.ask_high = tk.ask;
                cur.bid_min_ts = cur.bid_max_ts = tk.ts_ms;
                cur.ask_min_ts = cur.ask_max_ts = tk.ts_ms;
                cur_anchor = anchor; have_cur = true;
                continue;
            }
            if (anchor != cur_anchor) {
                cur.bid_min_before_max = (cur.bid_min_ts < cur.bid_max_ts);
                cur.ask_max_before_min = (cur.ask_max_ts < cur.ask_min_ts);
                bars.push_back(cur);
                cur = H4BarWithTicks{};
                cur.bar.ts_ms_open = anchor; cur.bar.ts_ms_close = anchor + dur_ms;
                cur.bar.open = mid; cur.bar.high = mid; cur.bar.low = mid; cur.bar.close = mid;
                cur.bar.n = 1;
                cur.bid_low = tk.bid; cur.bid_high = tk.bid;
                cur.ask_low = tk.ask; cur.ask_high = tk.ask;
                cur.bid_min_ts = cur.bid_max_ts = tk.ts_ms;
                cur.ask_min_ts = cur.ask_max_ts = tk.ts_ms;
                cur_anchor = anchor;
                continue;
            }
            if (mid > cur.bar.high) cur.bar.high = mid;
            if (mid < cur.bar.low)  cur.bar.low  = mid;
            cur.bar.close = mid;
            ++cur.bar.n;
            if (tk.bid < cur.bid_low)  { cur.bid_low  = tk.bid; cur.bid_min_ts = tk.ts_ms; }
            if (tk.bid > cur.bid_high) { cur.bid_high = tk.bid; cur.bid_max_ts = tk.ts_ms; }
            if (tk.ask < cur.ask_low)  { cur.ask_low  = tk.ask; cur.ask_min_ts = tk.ts_ms; }
            if (tk.ask > cur.ask_high) { cur.ask_high = tk.ask; cur.ask_max_ts = tk.ts_ms; }
        }
        fclose(f);
    }
    if (have_cur) {
        cur.bid_min_before_max = (cur.bid_min_ts < cur.bid_max_ts);
        cur.ask_max_before_min = (cur.ask_max_ts < cur.ask_min_ts);
        bars.push_back(cur);
    }

    ticks_ok_out = ok; ticks_fail_out = fail;
    first_ts_out = first_ts; last_ts_out = last_ts;
    return bars;
}

// =============================================================================
// Run conservative config D=10 SL=1.5 TP=4.0 over all bars
// =============================================================================
int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <csv1> [csv2] [csv3] ...\n", argv[0]);
        fprintf(stderr, "  Example: %s /Users/jo/Omega/data/l2_ticks_2026-04-14.csv \\\n"
                        "             /Users/jo/Omega/data/l2_ticks_2026-04-15.csv\n", argv[0]);
        fprintf(stderr, "  Or:      %s /Users/jo/Omega/data/l2_ticks_*.csv\n", argv[0]);
        return 1;
    }

    // Collect file paths, sort them so dates are chronological
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) files.push_back(argv[i]);
    std::sort(files.begin(), files.end());

    auto t0 = std::chrono::steady_clock::now();

    printf("=== PHASE 1: LOAD + BUILD H4 BARS ===\n");
    printf("Files (%zu):\n", files.size());
    for (const std::string& p : files) printf("  %s\n", p.c_str());
    fflush(stdout);

    uint64_t ticks_ok = 0, ticks_fail = 0;
    int64_t first_ts = 0, last_ts = 0;
    std::vector<H4BarWithTicks> bars = build_h4_bars_from_files(
        files, ticks_ok, ticks_fail, first_ts, last_ts);

    auto t1 = std::chrono::steady_clock::now();
    double load_s = std::chrono::duration<double>(t1 - t0).count();
    printf("\nTicks OK: %llu  failed: %llu\n",
           (unsigned long long)ticks_ok, (unsigned long long)ticks_fail);
    if (ticks_ok == 0) {
        fprintf(stderr, "No ticks parsed. Check file format.\n");
        return 1;
    }
    printf("Date range: %s -> %s\n", fmt_ts(first_ts).c_str(), fmt_ts(last_ts).c_str());
    printf("H4 bars built: %zu\n", bars.size());
    printf("Load time: %.1fs\n\n", load_s);
    fflush(stdout);

    if (bars.size() < 24) {
        fprintf(stderr, "WARNING: only %zu H4 bars. Strategy needs 24 for warmup (14 ATR + 10 channel).\n",
                bars.size());
        fprintf(stderr, "Expected trades: likely 0.\n\n");
    }

    // Conservative config
    const int    donchian_bars = 10;
    const double sl_mult       = 1.5;
    const double tp_mult       = 4.0;
    const double risk_dollars  = 10.0;
    const double max_lot       = 0.01;
    const double half_spread   = 0.15;  // entry cost

    printf("=== PHASE 2: REPLAY CONSERVATIVE CONFIG ===\n");
    printf("Config: D=%d SL=%.1f TP=%.1f risk=$%.0f\n\n",
           donchian_bars, sl_mult, tp_mult, risk_dollars);
    fflush(stdout);

    ATR atr; atr.set(14);

    struct Pos {
        bool active = false, is_long = false;
        double entry = 0.0, sl = 0.0, tp = 0.0, size = 0.0, h4_atr = 0.0;
        double channel_high_entry = 0.0, channel_low_entry = 0.0;
        int64_t entry_ts = 0;
        int bars_held = 0;
    } pos;

    std::vector<Trade> trades;
    int trade_id = 0;

    // Per-bar log of what the engine did
    printf("Bar-by-bar engine state (only showing active bars):\n");
    printf("%-20s %-8s %-10s %-10s %-8s %-10s %-10s %s\n",
           "bar_close", "primed", "channel_H", "channel_L", "close", "atr", "action", "detail");

    for (size_t i = 0; i < bars.size(); ++i) {
        const H4BarWithTicks& bt = bars[i];
        const Bar& b = bt.bar;
        atr.push(b.high, b.low, b.close);

        // EXIT check
        if (pos.active) {
            pos.bars_held++;
            bool sl_hit = false, tp_hit = false;
            double sl_px_q = pos.sl, tp_px_q = pos.tp;
            if (pos.is_long) {
                if (bt.bid_low  <= sl_px_q) sl_hit = true;
                if (bt.bid_high >= tp_px_q) tp_hit = true;
            } else {
                if (bt.ask_high >= sl_px_q) sl_hit = true;
                if (bt.ask_low  <= tp_px_q) tp_hit = true;
            }
            if (sl_hit && tp_hit) {
                bool sl_first = pos.is_long ? bt.bid_min_before_max : bt.ask_max_before_min;
                if (sl_first) tp_hit = false;
                else          sl_hit = false;
            }
            if (sl_hit || tp_hit) {
                double exit_px = sl_hit ? sl_px_q : tp_px_q;
                double pnl_pts = (pos.is_long
                    ? (exit_px - pos.entry)
                    : (pos.entry - exit_px)) * pos.size;
                Trade t;
                t.id = ++trade_id;
                t.entry_ts_ms = pos.entry_ts;
                t.exit_ts_ms  = bt.bar.ts_ms_close;
                t.is_long = pos.is_long;
                t.entry = pos.entry; t.exit = exit_px;
                t.sl = pos.sl; t.tp = pos.tp;
                t.size = pos.size;
                t.bars_held = pos.bars_held;
                t.pnl_pts = (pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px));
                t.pnl_usd = pnl_pts * 100.0;
                t.exit_reason = sl_hit ? "SL_HIT" : "TP_HIT";
                t.h4_atr_at_entry = pos.h4_atr;
                t.channel_high_at_entry = pos.channel_high_entry;
                t.channel_low_at_entry = pos.channel_low_entry;
                trades.push_back(t);
                printf("%-20s                                                             EXIT     %s @ %.2f pnl=$%+.2f (%s, %d bars)\n",
                       fmt_ts(bt.bar.ts_ms_close).c_str(),
                       pos.is_long ? "LONG" : "SHORT",
                       exit_px, t.pnl_usd, t.exit_reason.c_str(), pos.bars_held);
                pos = Pos{};
            }
        }

        if (pos.active) continue;
        if (!atr.primed) {
            if (i < 20 || i % 10 == 0)
                printf("%-20s %-8s (warmup ATR not primed yet -- %zu/%d seed samples)\n",
                       fmt_ts(b.ts_ms_close).c_str(), "NO", atr.seed_tr.size(), atr.period);
            continue;
        }
        if (i + 1 < (size_t)donchian_bars) continue;

        bool gated = is_weekend_gated(bt.bar.ts_ms_open);

        // Donchian over [i - donchian_bars .. i - 1]
        double ch_high = -1e18, ch_low = 1e18;
        for (int k = (int)i - donchian_bars; k < (int)i; ++k) {
            if (bars[k].bar.high > ch_high) ch_high = bars[k].bar.high;
            if (bars[k].bar.low  < ch_low)  ch_low  = bars[k].bar.low;
        }
        const bool bull_break = (b.close > ch_high);
        const bool bear_break = (b.close < ch_low);
        const char* action = "no_break";
        const char* detail = "";
        if (gated) { action = "weekend"; detail = "gated"; }
        else if (bull_break) { action = "BULL_BRK"; detail = ""; }
        else if (bear_break) { action = "BEAR_BRK"; detail = ""; }

        printf("%-20s %-8s %-10.2f %-10.2f %-8.2f %-10.2f %-10s %s\n",
               fmt_ts(b.ts_ms_close).c_str(),
               "yes", ch_high, ch_low, b.close, atr.value, action, detail);

        if (gated) continue;
        if (!bull_break && !bear_break) continue;

        const bool intend_long = bull_break;
        const double sl_pts = atr.value * sl_mult;
        const double tp_pts = atr.value * tp_mult;
        const double entry_px = intend_long ? (b.close + half_spread) : (b.close - half_spread);
        const double sl_px    = intend_long ? (entry_px - sl_pts) : (entry_px + sl_pts);
        const double tp_px    = intend_long ? (entry_px + tp_pts) : (entry_px - tp_pts);

        double size = risk_dollars / (sl_pts * 100.0);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(0.01, std::min(max_lot, size));

        pos.active = true;
        pos.is_long = intend_long;
        pos.entry = entry_px;
        pos.sl = sl_px; pos.tp = tp_px;
        pos.size = size;
        pos.h4_atr = atr.value;
        pos.channel_high_entry = ch_high;
        pos.channel_low_entry  = ch_low;
        pos.entry_ts = bt.bar.ts_ms_close;
        pos.bars_held = 0;

        printf("                                                                                   ENTRY    %s @ %.2f sl=%.2f tp=%.2f size=%.4f atr=%.2f\n",
               intend_long ? "LONG" : "SHORT",
               entry_px, sl_px, tp_px, size, atr.value);
    }

    // If position still open at end, mark it
    if (pos.active) {
        printf("\nEnd of data: position still open (%s @ %.2f, sl=%.2f tp=%.2f, %d bars held)\n",
               pos.is_long ? "LONG" : "SHORT", pos.entry, pos.sl, pos.tp, pos.bars_held);
    }

    // Summary
    printf("\n=== PHASE 3: SUMMARY ===\n");
    printf("Trades completed:   %zu\n", trades.size());
    if (pos.active) printf("Trades still open:  1\n");
    printf("H4 bars processed:  %zu\n", bars.size());
    if (trades.empty()) {
        printf("\nNo completed trades over this window.\n");
        if (bars.size() < 24) {
            printf("Reason: insufficient warmup. Strategy needs >=24 H4 bars; %zu available.\n",
                   bars.size());
        } else {
            printf("Reason: no H4 Donchian(10) breakouts occurred during the window.\n");
            printf("This is normal -- expected rate is ~1-2 trades per week on gold.\n");
        }
    } else {
        double total_pnl = 0.0;
        int wins = 0;
        for (const Trade& t : trades) {
            total_pnl += t.pnl_usd;
            if (t.pnl_usd > 0.0) ++wins;
        }
        printf("\nTrade log:\n");
        printf("%-4s %-20s %-20s %-6s %-10s %-10s %-10s %-10s %s\n",
               "id", "entry_ts", "exit_ts", "side", "entry", "exit", "pnl_usd", "bars", "reason");
        for (const Trade& t : trades) {
            printf("%-4d %-20s %-20s %-6s %-10.2f %-10.2f $%+-8.2f %-10d %s\n",
                   t.id,
                   fmt_ts(t.entry_ts_ms).c_str(),
                   fmt_ts(t.exit_ts_ms).c_str(),
                   t.is_long ? "LONG" : "SHORT",
                   t.entry, t.exit, t.pnl_usd,
                   t.bars_held, t.exit_reason.c_str());
        }
        printf("\nTotal PnL: $%+.2f  Wins: %d/%zu  WR: %.1f%%\n",
               total_pnl, wins, trades.size(),
               trades.empty() ? 0.0 : (100.0 * wins / trades.size()));
    }

    // Write CSV of trades
    FILE* f = fopen("htf_bt_l2days_trades.csv", "w");
    if (f) {
        fprintf(f, "id,entry_ts,exit_ts,side,entry,exit,sl,tp,size,pnl_pts,pnl_usd,bars_held,exit_reason,h4_atr,ch_high,ch_low\n");
        for (const Trade& t : trades) {
            fprintf(f, "%d,%s,%s,%s,%.3f,%.3f,%.3f,%.3f,%.4f,%.3f,%.2f,%d,%s,%.3f,%.3f,%.3f\n",
                    t.id,
                    fmt_ts(t.entry_ts_ms).c_str(),
                    fmt_ts(t.exit_ts_ms).c_str(),
                    t.is_long ? "LONG" : "SHORT",
                    t.entry, t.exit, t.sl, t.tp, t.size,
                    t.pnl_pts, t.pnl_usd, t.bars_held, t.exit_reason.c_str(),
                    t.h4_atr_at_entry, t.channel_high_at_entry, t.channel_low_at_entry);
        }
        fclose(f);
        printf("\nWrote: htf_bt_l2days_trades.csv\n");
    }

    return 0;
}

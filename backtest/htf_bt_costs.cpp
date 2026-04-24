// =============================================================================
// htf_bt_costs.cpp -- Realistic cost stress test for H4 Donchian breakout
//
// Takes a configuration (D, SL, TP) as command-line arguments and reruns
// the strategy across 5 cost scenarios from optimistic to pessimistic.
//
// The backtest already paid 0.15pt entry half-spread. This tool replaces that
// assumption with progressively worse cost models:
//
//   round-trip spread: paid at entry + exit (half each side)
//   SL slippage:       SL fills this many points worse than quoted price
//   TP slippage:       TP fills this many points worse than quoted price
//                      (conservative: TP slippage also modelled)
//
// Usage:
//   ./htf_bt_costs <tick_csv> <D> <sl_mult> <tp_mult>
//   ./htf_bt_costs /Users/jo/tick/2yr_XAUUSD_tick.csv 10 2.0 3.0
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
// Tick + parser (identical)
// =============================================================================
struct Tick {
    int64_t ts_ms;
    double  bid;
    double  ask;
};

static bool parse_line(const char* s, Tick& t) {
    if (strlen(s) < 18) return false;
    if (!isdigit((unsigned char)s[0])) return false;
    int year  = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    int month = (s[4]-'0')*10 + (s[5]-'0');
    int day   = (s[6]-'0')*10 + (s[7]-'0');
    if (s[8] != ',') return false;
    int H = (s[9]-'0')*10 + (s[10]-'0');
    int M = (s[12]-'0')*10 + (s[13]-'0');
    int S = (s[15]-'0')*10 + (s[16]-'0');
    if (s[17] != ',') return false;

    const char* p = s + 18;
    char* end;
    double bid = strtod(p, &end);
    if (end == p || *end != ',') return false;
    p = end + 1;
    double ask = strtod(p, &end);
    if (end == p) return false;

    int y = year;
    int m = month;
    if (m <= 2) { y -= 1; }
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe/4 - yoe/100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    int64_t sec  = days * 86400 + H * 3600 + M * 60 + S;

    t.ts_ms = sec * 1000LL;
    t.bid   = bid;
    t.ask   = ask;
    return true;
}

// =============================================================================
// Types (identical)
// =============================================================================
struct Bar {
    int64_t ts_ms_open = 0;
    int64_t ts_ms_close = 0;
    double open = 0.0, high = 0.0, low = 0.0, close = 0.0;
    int n = 0;
};
struct H4BarWithTicks {
    Bar bar;
    double bid_low = 1e18, bid_high = 0.0, ask_low = 1e18, ask_high = 0.0;
    bool bid_min_before_max = false, ask_max_before_min = false;
    int64_t bid_min_ts = 0, bid_max_ts = 0, ask_min_ts = 0, ask_max_ts = 0;
};

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
};

struct Config {
    int donchian_bars;
    double sl_mult;
    double tp_mult;
};

struct CostModel {
    const char* name;
    double round_trip_spread;   // points, paid half on entry + half on exit
    double sl_slippage;         // points worse than quoted SL price
    double tp_slippage;         // points worse than quoted TP price
};

struct Result {
    Config cfg;
    CostModel cm;
    int n_trades = 0, n_wins = 0;
    double total_pnl = 0.0, avg_win = 0.0, avg_loss = 0.0;
    double win_rate = 0.0, expectancy = 0.0, max_dd = 0.0, profit_factor = 0.0;
};

// =============================================================================
// Helpers (identical)
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

static std::vector<H4BarWithTicks> build_h4_bars(const char* csv_path,
                                                   uint64_t& ticks_ok_out,
                                                   uint64_t& ticks_fail_out,
                                                   int64_t& first_ts_out,
                                                   int64_t& last_ts_out)
{
    std::vector<H4BarWithTicks> bars;
    FILE* f = fopen(csv_path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", csv_path); return bars; }
    const int64_t dur_ms = 240LL * 60LL * 1000LL;
    int64_t cur_anchor = -1;
    H4BarWithTicks cur;
    bool have_cur = false;
    char line[256];
    uint64_t ok = 0, fail = 0;
    int64_t first_ts = 0, last_ts = 0;
    while (fgets(line, sizeof(line), f)) {
        Tick tk;
        if (!parse_line(line, tk)) { ++fail; continue; }
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
    if (have_cur) {
        cur.bid_min_before_max = (cur.bid_min_ts < cur.bid_max_ts);
        cur.ask_max_before_min = (cur.ask_max_ts < cur.ask_min_ts);
        bars.push_back(cur);
    }
    fclose(f);
    ticks_ok_out = ok; ticks_fail_out = fail;
    first_ts_out = first_ts; last_ts_out = last_ts;
    return bars;
}

// =============================================================================
// Run with configurable costs
// =============================================================================
static Result run_with_costs(const std::vector<H4BarWithTicks>& bars,
                             const Config& cfg,
                             const CostModel& cm)
{
    Result r;
    r.cfg = cfg; r.cm = cm;

    ATR atr; atr.set(14);

    struct Pos {
        bool active = false, is_long = false;
        double entry = 0.0, sl = 0.0, tp = 0.0, size = 0.0, h4_atr = 0.0;
        int64_t entry_ts = 0;
        int bars_held = 0;
    } pos;

    std::vector<Trade> trades;
    int trade_id = 0;

    const double half_spread = cm.round_trip_spread * 0.5;

    for (size_t i = 0; i < bars.size(); ++i) {
        const H4BarWithTicks& bt = bars[i];
        const Bar& b = bt.bar;
        atr.push(b.high, b.low, b.close);

        if (pos.active) {
            pos.bars_held++;
            bool sl_hit = false, tp_hit = false;
            double sl_px_quoted = pos.sl;
            double tp_px_quoted = pos.tp;
            if (pos.is_long) {
                if (bt.bid_low  <= sl_px_quoted) { sl_hit = true; }
                if (bt.bid_high >= tp_px_quoted) { tp_hit = true; }
            } else {
                if (bt.ask_high >= sl_px_quoted) { sl_hit = true; }
                if (bt.ask_low  <= tp_px_quoted) { tp_hit = true; }
            }
            if (sl_hit && tp_hit) {
                bool sl_first = pos.is_long ? bt.bid_min_before_max : bt.ask_max_before_min;
                if (sl_first) tp_hit = false;
                else          sl_hit = false;
            }
            if (sl_hit || tp_hit) {
                // Apply slippage to the exit price. Slippage is always against us.
                double exit_px_raw = sl_hit ? sl_px_quoted : tp_px_quoted;
                double slip = sl_hit ? cm.sl_slippage : cm.tp_slippage;
                double exit_px;
                if (pos.is_long) {
                    exit_px = exit_px_raw - slip;   // sell at worse bid
                } else {
                    exit_px = exit_px_raw + slip;   // buy-to-cover at worse ask
                }
                double pnl_pts = (pos.is_long
                    ? (exit_px - pos.entry)
                    : (pos.entry - exit_px)) * pos.size;
                Trade t;
                t.id = ++trade_id;
                t.entry_ts_ms = pos.entry_ts;
                t.exit_ts_ms  = bt.bar.ts_ms_close;
                t.is_long = pos.is_long;
                t.entry = pos.entry;
                t.exit = exit_px;
                t.sl = pos.sl;
                t.tp = pos.tp;
                t.size = pos.size;
                t.bars_held = pos.bars_held;
                t.pnl_pts = (pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px));
                t.pnl_usd = pnl_pts * 100.0;
                t.exit_reason = sl_hit ? "SL_HIT" : "TP_HIT";
                trades.push_back(t);
                pos = Pos{};
            }
        }

        if (pos.active) continue;
        if (!atr.primed) continue;
        // Guard: need i >= cfg.donchian_bars so that k=i-N is >= 0 in the loop below.
        // (Fix applied 2026-04-24 Session 11 -- off-by-one surfaced via h1_bt_minimal.)
        if ((int)i < cfg.donchian_bars) continue;
        if (is_weekend_gated(bt.bar.ts_ms_open)) continue;

        double ch_high = -1e18, ch_low = 1e18;
        for (int k = (int)i - cfg.donchian_bars; k < (int)i; ++k) {
            if (bars[k].bar.high > ch_high) ch_high = bars[k].bar.high;
            if (bars[k].bar.low  < ch_low)  ch_low  = bars[k].bar.low;
        }
        const bool bull_break = (b.close > ch_high);
        const bool bear_break = (b.close < ch_low);
        if (!bull_break && !bear_break) continue;

        const bool intend_long = bull_break;
        const double sl_pts = atr.value * cfg.sl_mult;
        const double tp_pts = atr.value * cfg.tp_mult;
        // Entry pays configured half-spread
        const double entry_px = intend_long ? (b.close + half_spread) : (b.close - half_spread);
        const double sl_px    = intend_long ? (entry_px - sl_pts) : (entry_px + sl_pts);
        const double tp_px    = intend_long ? (entry_px + tp_pts) : (entry_px - tp_pts);

        const double risk_dollars = 10.0;
        const double max_lot = 0.01;
        double size = risk_dollars / (sl_pts * 100.0);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(0.01, std::min(max_lot, size));

        pos.active = true;
        pos.is_long = intend_long;
        pos.entry = entry_px;
        pos.sl = sl_px; pos.tp = tp_px;
        pos.size = size;
        pos.h4_atr = atr.value;
        pos.entry_ts = bt.bar.ts_ms_close;
        pos.bars_held = 0;
    }

    double cum = 0.0, peak = 0.0, max_dd = 0.0;
    double gw = 0.0, gl = 0.0;
    int nw = 0, nl = 0;
    for (const Trade& t : trades) {
        cum += t.pnl_usd;
        if (cum > peak) peak = cum;
        double dd = peak - cum;
        if (dd > max_dd) max_dd = dd;
        if (t.pnl_usd > 0.0) { ++nw; gw += t.pnl_usd; }
        else                 { ++nl; gl += -t.pnl_usd; }
    }
    r.n_trades = (int)trades.size();
    r.n_wins = nw;
    r.total_pnl = cum;
    r.max_dd = max_dd;
    if (nw > 0)         r.avg_win  = gw / nw;
    if (nl > 0)         r.avg_loss = gl / nl;
    if (r.n_trades > 0) { r.win_rate = 100.0 * nw / r.n_trades;
                          r.expectancy = r.total_pnl / r.n_trades; }
    if (gl > 0.0)       r.profit_factor = gw / gl;
    return r;
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s <tick_csv> <D> <sl_mult> <tp_mult>\n", argv[0]);
        fprintf(stderr, "  example: %s /Users/jo/tick/2yr_XAUUSD_tick.csv 10 2.0 3.0\n", argv[0]);
        return 1;
    }
    const char* csv_path = argv[1];
    Config cfg;
    cfg.donchian_bars = std::atoi(argv[2]);
    cfg.sl_mult = std::atof(argv[3]);
    cfg.tp_mult = std::atof(argv[4]);

    auto t0 = std::chrono::steady_clock::now();

    printf("=== PHASE 1: LOAD + BUILD H4 BARS ===\n");
    fflush(stdout);
    uint64_t ticks_ok = 0, ticks_fail = 0;
    int64_t first_ts = 0, last_ts = 0;
    std::vector<H4BarWithTicks> bars = build_h4_bars(csv_path,
                                                       ticks_ok, ticks_fail,
                                                       first_ts, last_ts);
    auto t1 = std::chrono::steady_clock::now();
    double load_s = std::chrono::duration<double>(t1 - t0).count();
    printf("Ticks OK: %llu  failed: %llu\n", (unsigned long long)ticks_ok,
           (unsigned long long)ticks_fail);
    printf("Date range: %s -> %s\n", fmt_ts(first_ts).c_str(), fmt_ts(last_ts).c_str());
    printf("H4 bars built: %zu\n", bars.size());
    printf("Load time: %.1fs\n\n", load_s);

    printf("=== PHASE 2: COST SCENARIOS ===\n");
    printf("Config: D=%d SL=%.1f TP=%.1f\n\n", cfg.donchian_bars, cfg.sl_mult, cfg.tp_mult);

    std::vector<CostModel> scenarios = {
        { "Backtest baseline",  0.30,  0.0,  0.0 },
        { "BlackBull tight",    0.40,  0.3,  0.2 },
        { "BlackBull normal",   0.60,  0.5,  0.3 },
        { "BlackBull wide",     1.00,  1.0,  0.5 },
        { "Pessimistic",        1.50,  1.5,  1.0 },
    };

    std::vector<Result> results;
    for (const CostModel& cm : scenarios) {
        Result r = run_with_costs(bars, cfg, cm);
        results.push_back(r);
        printf("  %-22s spread=%.2fpt sl_slip=%.1fpt tp_slip=%.1fpt\n"
               "                           n=%-4d WR=%.1f%%  PnL=$%+9.2f  DD=$-%.2f  PF=%.2f\n\n",
               cm.name, cm.round_trip_spread, cm.sl_slippage, cm.tp_slippage,
               r.n_trades, r.win_rate, r.total_pnl, r.max_dd, r.profit_factor);
        fflush(stdout);
    }

    auto t2 = std::chrono::steady_clock::now();
    double total_s = std::chrono::duration<double>(t2 - t0).count();

    // Write report
    FILE* f = fopen("htf_bt_costs_results.txt", "w");
    if (f) {
        fprintf(f, "========================================================================\n");
        fprintf(f, "  htf_bt_costs -- cost stress test for H4 Donchian breakout\n");
        fprintf(f, "========================================================================\n\n");
        fprintf(f, "Ticks OK:        %llu\n", (unsigned long long)ticks_ok);
        fprintf(f, "Date range:      %s -> %s\n",
                fmt_ts(first_ts).c_str(), fmt_ts(last_ts).c_str());
        fprintf(f, "H4 bars:         %zu\n", bars.size());
        fprintf(f, "Config:          D=%d SL=%.1f TP=%.1f\n",
                cfg.donchian_bars, cfg.sl_mult, cfg.tp_mult);
        fprintf(f, "Runtime:         %.1fs\n\n", total_s);

        fprintf(f, "%-22s %-8s %-8s %-8s | %-5s %-6s %-9s %-10s %-6s\n",
                "Scenario", "spread", "sl_slip", "tp_slip", "n", "WR%", "PnL$", "DD", "PF");
        fprintf(f, "--------------------------------------------------------------------------------\n");
        for (const Result& r : results) {
            fprintf(f, "%-22s %-8.2f %-8.1f %-8.1f | %-5d %-6.1f %+9.2f %-10.2f %-6.2f\n",
                    r.cm.name, r.cm.round_trip_spread, r.cm.sl_slippage, r.cm.tp_slippage,
                    r.n_trades, r.win_rate, r.total_pnl, r.max_dd, r.profit_factor);
        }

        // Break-even analysis
        fprintf(f, "\nBREAK-EVEN ANALYSIS:\n");
        fprintf(f, "  Edge survives through scenario: ");
        const char* last_survives = nullptr;
        for (const Result& r : results) {
            if (r.total_pnl > 0.0 && r.profit_factor >= 1.1) last_survives = r.cm.name;
        }
        fprintf(f, "%s\n", last_survives ? last_survives : "(none -- edge collapses under any friction)");

        // First failing scenario
        const char* first_fails = nullptr;
        for (const Result& r : results) {
            if (r.total_pnl < 0.0 || r.profit_factor < 1.0) {
                first_fails = r.cm.name;
                break;
            }
        }
        fprintf(f, "  First scenario where edge breaks: %s\n",
                first_fails ? first_fails : "(none -- edge robust under all tested conditions)");

        fclose(f);
    }

    printf("\nWrote: htf_bt_costs_results.txt\n");
    printf("Total runtime: %.1fs\n", total_s);
    return 0;
}

// =============================================================================
// htf_bt_walkforward.cpp -- Walk-forward validation of H4 Donchian breakout
//
// Protocol:
//   1. Split 2yr XAUUSD tick data into Year 1 (first half) and Year 2 (second).
//   2. Run full 27-config sweep on Year 1 only.
//   3. Identify best config by Year 1 total PnL (requires n >= 30).
//   4. Run that exact config on Year 2 (no re-tuning).
//   5. Run that exact config on full 2yr for sanity check.
//   6. Also report Year 1 top-5 configs and their Year 2 performance
//      (parameter stability / ranking consistency check).
//   7. Also report Year 2 best config in-isolation -- did we get lucky?
//
// Strategy identical to htf_bt_minimal.cpp. Only the evaluation protocol
// differs.
//
// Build:
//   clang++ -O3 -std=c++17 -o htf_bt_walkforward htf_bt_walkforward.cpp
// Run:
//   ./htf_bt_walkforward /Users/jo/tick/2yr_XAUUSD_tick.csv
// Output:
//   htf_bt_walkforward_results.txt    -- full walk-forward report
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
// Tick + parser
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
// Bar + H4 bar with intra-bar extremes
// =============================================================================
struct Bar {
    int64_t ts_ms_open  = 0;
    int64_t ts_ms_close = 0;
    double  open   = 0.0;
    double  high   = 0.0;
    double  low    = 0.0;
    double  close  = 0.0;
    int     n      = 0;
};

struct H4BarWithTicks {
    Bar bar;
    double  bid_low  = 1e18;
    double  bid_high = 0.0;
    double  ask_low  = 1e18;
    double  ask_high = 0.0;
    bool    bid_min_before_max = false;
    bool    ask_max_before_min = false;
    int64_t bid_min_ts = 0, bid_max_ts = 0;
    int64_t ask_min_ts = 0, ask_max_ts = 0;
};

// =============================================================================
// ATR(14) Wilder
// =============================================================================
struct ATR {
    double value   = 0.0;
    bool   primed  = false;
    int    period  = 14;
    double prev_close = 0.0;
    bool   have_prev  = false;
    std::deque<double> seed_tr;
    void set(int p) { period = p; value = 0.0; primed = false; have_prev = false; seed_tr.clear(); }
    void push(double high, double low, double close) {
        double tr;
        if (!have_prev) { tr = high - low; }
        else {
            double a = high - low;
            double b = std::fabs(high - prev_close);
            double c = std::fabs(low  - prev_close);
            tr = std::max(a, std::max(b, c));
        }
        have_prev = true;
        prev_close = close;
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

// =============================================================================
// Types
// =============================================================================
struct Trade {
    int     id          = 0;
    int64_t entry_ts_ms = 0;
    int64_t exit_ts_ms  = 0;
    bool    is_long     = false;
    double  entry       = 0.0;
    double  exit        = 0.0;
    double  sl          = 0.0;
    double  tp          = 0.0;
    double  size        = 0.0;
    double  pnl_pts     = 0.0;
    double  pnl_usd     = 0.0;
    int     bars_held   = 0;
    std::string exit_reason;
};

struct Config {
    int    donchian_bars;
    double sl_mult;
    double tp_mult;
};

struct Result {
    Config cfg;
    int    n_trades = 0;
    int    n_wins   = 0;
    double total_pnl = 0.0;
    double avg_win   = 0.0;
    double avg_loss  = 0.0;
    double win_rate  = 0.0;
    double expectancy = 0.0;
    double max_dd    = 0.0;
    double profit_factor = 0.0;
    std::vector<Trade> trades;
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
// Load all ticks into H4 bars
// =============================================================================
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
            cur.bar.ts_ms_open  = anchor;
            cur.bar.ts_ms_close = anchor + dur_ms;
            cur.bar.open  = mid; cur.bar.high = mid; cur.bar.low = mid; cur.bar.close = mid;
            cur.bar.n     = 1;
            cur.bid_low = tk.bid; cur.bid_high = tk.bid;
            cur.ask_low = tk.ask; cur.ask_high = tk.ask;
            cur.bid_min_ts = cur.bid_max_ts = tk.ts_ms;
            cur.ask_min_ts = cur.ask_max_ts = tk.ts_ms;
            cur_anchor = anchor;
            have_cur = true;
            continue;
        }
        if (anchor != cur_anchor) {
            cur.bid_min_before_max = (cur.bid_min_ts < cur.bid_max_ts);
            cur.ask_max_before_min = (cur.ask_max_ts < cur.ask_min_ts);
            bars.push_back(cur);
            cur = H4BarWithTicks{};
            cur.bar.ts_ms_open  = anchor;
            cur.bar.ts_ms_close = anchor + dur_ms;
            cur.bar.open  = mid; cur.bar.high = mid; cur.bar.low = mid; cur.bar.close = mid;
            cur.bar.n     = 1;
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

    ticks_ok_out = ok;
    ticks_fail_out = fail;
    first_ts_out = first_ts;
    last_ts_out = last_ts;
    return bars;
}

// =============================================================================
// Core engine: run one config over a subset of bars [bar_start_idx, bar_end_idx)
// Note: we need at least donchian_bars of warmup WITHIN the window.
// The Donchian channel computation looks back donchian_bars from each bar.
// =============================================================================
static Result run_one(const std::vector<H4BarWithTicks>& bars,
                      const Config& cfg,
                      int bar_start_idx, int bar_end_idx)
{
    Result r;
    r.cfg = cfg;
    r.trades.reserve(512);

    ATR atr; atr.set(14);

    struct Pos {
        bool    active   = false;
        bool    is_long  = false;
        double  entry    = 0.0;
        double  sl       = 0.0;
        double  tp       = 0.0;
        double  size     = 0.0;
        double  h4_atr   = 0.0;
        int64_t entry_ts = 0;
        int     bars_held = 0;
    } pos;

    int trade_id = 0;

    for (int i = bar_start_idx; i < bar_end_idx; ++i) {
        const H4BarWithTicks& bt = bars[i];
        const Bar& b = bt.bar;

        atr.push(b.high, b.low, b.close);

        // EXIT check first
        if (pos.active) {
            pos.bars_held++;
            bool sl_hit = false, tp_hit = false;
            double sl_px = 0.0, tp_px = 0.0;
            if (pos.is_long) {
                if (bt.bid_low  <= pos.sl) { sl_hit = true; sl_px = pos.sl; }
                if (bt.bid_high >= pos.tp) { tp_hit = true; tp_px = pos.tp; }
            } else {
                if (bt.ask_high >= pos.sl) { sl_hit = true; sl_px = pos.sl; }
                if (bt.ask_low  <= pos.tp) { tp_hit = true; tp_px = pos.tp; }
            }
            if (sl_hit && tp_hit) {
                bool sl_first = pos.is_long ? bt.bid_min_before_max : bt.ask_max_before_min;
                if (sl_first) tp_hit = false;
                else          sl_hit = false;
            }
            if (sl_hit || tp_hit) {
                double exit_px = sl_hit ? sl_px : tp_px;
                double pnl_pts = (pos.is_long
                    ? (exit_px - pos.entry)
                    : (pos.entry - exit_px)) * pos.size;
                Trade t;
                t.id = ++trade_id;
                t.entry_ts_ms = pos.entry_ts;
                t.exit_ts_ms  = bt.bar.ts_ms_close;
                t.is_long     = pos.is_long;
                t.entry       = pos.entry;
                t.exit        = exit_px;
                t.sl          = pos.sl;
                t.tp          = pos.tp;
                t.size        = pos.size;
                t.bars_held   = pos.bars_held;
                t.pnl_pts     = (pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px));
                t.pnl_usd     = pnl_pts * 100.0;
                t.exit_reason = sl_hit ? "SL_HIT" : "TP_HIT";
                r.trades.push_back(t);
                pos = Pos{};
            }
        }

        if (pos.active) continue;
        if (!atr.primed) continue;
        if (i < bar_start_idx + cfg.donchian_bars) continue;

        if (is_weekend_gated(bt.bar.ts_ms_open)) continue;

        // Donchian over [i - donchian_bars .. i - 1]
        double ch_high = -1e18, ch_low = 1e18;
        for (int k = i - cfg.donchian_bars; k < i; ++k) {
            if (bars[k].bar.high > ch_high) ch_high = bars[k].bar.high;
            if (bars[k].bar.low  < ch_low)  ch_low  = bars[k].bar.low;
        }
        const bool bull_break = (b.close > ch_high);
        const bool bear_break = (b.close < ch_low);
        if (!bull_break && !bear_break) continue;

        const bool intend_long = bull_break;
        const double sl_pts = atr.value * cfg.sl_mult;
        const double tp_pts = atr.value * cfg.tp_mult;
        const double half_spread = 0.15;
        const double entry_px = intend_long ? (b.close + half_spread) : (b.close - half_spread);
        const double sl_px    = intend_long ? (entry_px - sl_pts) : (entry_px + sl_pts);
        const double tp_px    = intend_long ? (entry_px + tp_pts) : (entry_px - tp_pts);

        const double risk_dollars = 10.0;
        const double max_lot = 0.01;
        double size = risk_dollars / (sl_pts * 100.0);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(0.01, std::min(max_lot, size));

        pos.active   = true;
        pos.is_long  = intend_long;
        pos.entry    = entry_px;
        pos.sl       = sl_px;
        pos.tp       = tp_px;
        pos.size     = size;
        pos.h4_atr   = atr.value;
        pos.entry_ts = bt.bar.ts_ms_close;
        pos.bars_held = 0;
    }

    double cum = 0.0, peak = 0.0, max_dd = 0.0;
    double gw = 0.0, gl = 0.0;
    int nw = 0, nl = 0;
    for (const Trade& t : r.trades) {
        cum += t.pnl_usd;
        if (cum > peak) peak = cum;
        double dd = peak - cum;
        if (dd > max_dd) max_dd = dd;
        if (t.pnl_usd > 0.0) { ++nw; gw += t.pnl_usd; }
        else                 { ++nl; gl += -t.pnl_usd; }
    }
    r.n_trades = (int)r.trades.size();
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
// Find bar index where UTC timestamp first reaches target_ts_ms
// =============================================================================
static int find_bar_at_or_after(const std::vector<H4BarWithTicks>& bars, int64_t target_ts_ms) {
    for (size_t i = 0; i < bars.size(); ++i) {
        if (bars[i].bar.ts_ms_open >= target_ts_ms) return (int)i;
    }
    return (int)bars.size();
}

// =============================================================================
// Write walk-forward report
// =============================================================================
static void write_report(const char* path,
                         const std::vector<Result>& y1_all,
                         const std::vector<Result>& y2_all,
                         const std::vector<Result>& full_all,
                         int y1_best_idx, int y2_best_idx,
                         uint64_t ticks_ok, uint64_t ticks_fail,
                         int64_t first_ts, int64_t last_ts,
                         int64_t split_ts,
                         size_t n_bars_total, int split_bar_idx,
                         double runtime_s)
{
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return; }

    fprintf(f, "========================================================================\n");
    fprintf(f, "  htf_bt_walkforward -- train/test validation, H4 Donchian XAUUSD\n");
    fprintf(f, "========================================================================\n\n");
    fprintf(f, "Ticks OK:        %llu\n", (unsigned long long)ticks_ok);
    fprintf(f, "Ticks failed:    %llu\n", (unsigned long long)ticks_fail);
    fprintf(f, "Full date range: %s -> %s\n", fmt_ts(first_ts).c_str(), fmt_ts(last_ts).c_str());
    fprintf(f, "Split at:        %s\n", fmt_ts(split_ts).c_str());
    fprintf(f, "Total H4 bars:   %zu   (split at bar index %d)\n",
            n_bars_total, split_bar_idx);
    fprintf(f, "Year 1 bars:     %d\n", split_bar_idx);
    fprintf(f, "Year 2 bars:     %d\n", (int)(n_bars_total - split_bar_idx));
    fprintf(f, "Runtime:         %.1fs\n\n", runtime_s);

    // -----------------------------------------------------------
    // SECTION 1: YEAR 1 SWEEP
    // -----------------------------------------------------------
    fprintf(f, "========================================================================\n");
    fprintf(f, "  YEAR 1 IN-SAMPLE SWEEP (training)\n");
    fprintf(f, "========================================================================\n\n");
    fprintf(f, "%-3s %-5s %-5s | %-5s %-6s %-9s %-8s %-6s\n",
            "D", "SL", "TP", "n", "WR%", "PnL$", "DD", "PF");
    fprintf(f, "-------------------------------------------------------\n");
    for (const Result& r : y1_all) {
        fprintf(f, "%-3d %-5.1f %-5.1f | %-5d %-6.1f %+9.2f %-8.2f %-6.2f\n",
                r.cfg.donchian_bars, r.cfg.sl_mult, r.cfg.tp_mult,
                r.n_trades, r.win_rate, r.total_pnl, r.max_dd, r.profit_factor);
    }

    // -----------------------------------------------------------
    // SECTION 2: YEAR 2 OUT-OF-SAMPLE USING YEAR 1 BEST
    // -----------------------------------------------------------
    fprintf(f, "\n========================================================================\n");
    fprintf(f, "  YEAR 1 BEST CONFIG APPLIED TO YEAR 2 (OUT-OF-SAMPLE TEST)\n");
    fprintf(f, "========================================================================\n\n");
    const Result& y1_best = y1_all[y1_best_idx];
    // Find same config in y2_all
    int y2_idx_same = -1;
    for (size_t i = 0; i < y2_all.size(); ++i) {
        if (y2_all[i].cfg.donchian_bars == y1_best.cfg.donchian_bars &&
            std::fabs(y2_all[i].cfg.sl_mult - y1_best.cfg.sl_mult) < 1e-9 &&
            std::fabs(y2_all[i].cfg.tp_mult - y1_best.cfg.tp_mult) < 1e-9) {
            y2_idx_same = (int)i; break;
        }
    }
    const Result& y1b = y1_all[y1_best_idx];
    fprintf(f, "Best Year 1 config: D=%d SL=%.1f TP=%.1f\n",
            y1b.cfg.donchian_bars, y1b.cfg.sl_mult, y1b.cfg.tp_mult);
    fprintf(f, "\n                    Year 1 (train)     Year 2 (test)\n");
    fprintf(f, "  ----------------------------------------------------\n");
    const Result& y2s = y2_all[y2_idx_same];
    fprintf(f, "  Trades:           %-6d              %-6d\n",       y1b.n_trades, y2s.n_trades);
    fprintf(f, "  Win rate:         %-6.1f%%             %-6.1f%%\n", y1b.win_rate, y2s.win_rate);
    fprintf(f, "  Total PnL:        $%+-8.2f          $%+-8.2f\n",    y1b.total_pnl, y2s.total_pnl);
    fprintf(f, "  Avg win:          $%+-8.2f          $%+-8.2f\n",    y1b.avg_win,  y2s.avg_win);
    fprintf(f, "  Avg loss:         $-%-7.2f          $-%-7.2f\n",    y1b.avg_loss, y2s.avg_loss);
    fprintf(f, "  Max DD:           $-%-7.2f          $-%-7.2f\n",    y1b.max_dd,   y2s.max_dd);
    fprintf(f, "  Profit factor:    %-6.2f              %-6.2f\n",    y1b.profit_factor, y2s.profit_factor);
    fprintf(f, "  Expectancy/tr:    $%+-8.2f          $%+-8.2f\n",    y1b.expectancy, y2s.expectancy);

    // Verdict text
    fprintf(f, "\n  VERDICT:\n");
    if (y2s.profit_factor >= 1.2 && y2s.total_pnl > 0 && y2s.n_trades >= 20) {
        fprintf(f, "    EDGE HOLDS out-of-sample. PF=%.2f >= 1.2, PnL positive, n=%d.\n",
                y2s.profit_factor, y2s.n_trades);
    } else if (y2s.total_pnl > 0 && y2s.profit_factor > 1.0) {
        fprintf(f, "    EDGE MARGINAL. Year 2 positive but PF=%.2f below confidence threshold.\n",
                y2s.profit_factor);
    } else {
        fprintf(f, "    EDGE FAILED. Year 2 PnL=$%+.2f PF=%.2f -- overfit or regime-dependent.\n",
                y2s.total_pnl, y2s.profit_factor);
    }

    // Monthly breakdown for both years using same config
    auto write_monthly = [&](const std::vector<Trade>& trades, const char* label) {
        fprintf(f, "\n  %s monthly breakdown:\n", label);
        struct MB { int y = 0; int m = 0; int n = 0; double pnl = 0.0; };
        std::vector<MB> mb;
        auto find_ym = [&](int y, int m) -> MB& {
            for (MB& x : mb) if (x.y == y && x.m == m) return x;
            mb.push_back(MB{y, m, 0, 0.0});
            return mb.back();
        };
        for (const Trade& t : trades) {
            time_t s_ = (time_t)(t.exit_ts_ms / 1000LL);
            struct tm tmv; gmtime_r(&s_, &tmv);
            MB& x = find_ym(tmv.tm_year + 1900, tmv.tm_mon + 1);
            x.n += 1; x.pnl += t.pnl_usd;
        }
        std::sort(mb.begin(), mb.end(), [](const MB& a, const MB& b){
            if (a.y != b.y) return a.y < b.y;
            return a.m < b.m;
        });
        double cum = 0.0;
        for (const MB& x : mb) {
            cum += x.pnl;
            fprintf(f, "    %04d-%02d: n=%-3d pnl=$%+8.2f cum=$%+8.2f\n",
                    x.y, x.m, x.n, x.pnl, cum);
        }
    };
    write_monthly(y1b.trades, "Year 1");
    write_monthly(y2s.trades, "Year 2");

    // -----------------------------------------------------------
    // SECTION 3: TOP 5 YEAR 1 CONFIGS APPLIED TO YEAR 2
    // (ranking consistency check)
    // -----------------------------------------------------------
    fprintf(f, "\n========================================================================\n");
    fprintf(f, "  TOP-5 YEAR 1 CONFIGS APPLIED TO YEAR 2\n");
    fprintf(f, "========================================================================\n\n");
    // Build index list sorted by Y1 PnL
    std::vector<int> y1_rank;
    y1_rank.reserve(y1_all.size());
    for (int i = 0; i < (int)y1_all.size(); ++i) {
        if (y1_all[i].n_trades >= 30) y1_rank.push_back(i);
    }
    std::sort(y1_rank.begin(), y1_rank.end(),
              [&](int a, int b){ return y1_all[a].total_pnl > y1_all[b].total_pnl; });
    int top_n = std::min<int>(5, (int)y1_rank.size());
    fprintf(f, "%-5s %-5s %-5s | %-11s %-11s %-8s %-8s %-5s %-5s\n",
            "D", "SL", "TP", "Y1_PnL", "Y2_PnL", "Y1_PF", "Y2_PF", "Y1_n", "Y2_n");
    fprintf(f, "-----------------------------------------------------------------------\n");
    for (int k = 0; k < top_n; ++k) {
        int i = y1_rank[k];
        const Result& ry1 = y1_all[i];
        const Result& ry2 = y2_all[i];
        fprintf(f, "%-5d %-5.1f %-5.1f | $%+9.2f  $%+9.2f  %-8.2f %-8.2f %-5d %-5d\n",
                ry1.cfg.donchian_bars, ry1.cfg.sl_mult, ry1.cfg.tp_mult,
                ry1.total_pnl, ry2.total_pnl,
                ry1.profit_factor, ry2.profit_factor,
                ry1.n_trades, ry2.n_trades);
    }
    // Count how many of top-5 were positive in year 2
    int top5_y2_positive = 0;
    for (int k = 0; k < top_n; ++k) {
        if (y2_all[y1_rank[k]].total_pnl > 0) ++top5_y2_positive;
    }
    fprintf(f, "\n  Of top-5 Year 1 configs: %d/%d positive in Year 2.\n",
            top5_y2_positive, top_n);

    // -----------------------------------------------------------
    // SECTION 4: YEAR 2 BEST IN ISOLATION
    // (did we get lucky with Year 1 pick?)
    // -----------------------------------------------------------
    fprintf(f, "\n========================================================================\n");
    fprintf(f, "  YEAR 2 BEST CONFIG IN-ISOLATION (lucky-pick check)\n");
    fprintf(f, "========================================================================\n\n");
    const Result& y2_best = y2_all[y2_best_idx];
    // Find its rank in Year 1
    int y2b_y1_rank = -1;
    for (int k = 0; k < (int)y1_rank.size(); ++k) {
        if (y1_rank[k] == y2_best_idx) { y2b_y1_rank = k + 1; break; }
    }
    fprintf(f, "Year 2 best config:   D=%d SL=%.1f TP=%.1f\n",
            y2_best.cfg.donchian_bars, y2_best.cfg.sl_mult, y2_best.cfg.tp_mult);
    fprintf(f, "  Year 2 PnL:          $%+.2f\n", y2_best.total_pnl);
    fprintf(f, "  Year 2 PF:           %.2f\n",    y2_best.profit_factor);
    fprintf(f, "  Its Year 1 PnL was:  $%+.2f\n", y1_all[y2_best_idx].total_pnl);
    fprintf(f, "  Its Year 1 PF was:   %.2f\n",    y1_all[y2_best_idx].profit_factor);
    fprintf(f, "  Its Year 1 rank:     %s\n",
            y2b_y1_rank > 0 ? (std::to_string(y2b_y1_rank) + " of " + std::to_string((int)y1_rank.size())).c_str() : "n/a");
    if (y1_best_idx == y2_best_idx) {
        fprintf(f, "\n  The Year 1 best config IS ALSO the Year 2 best config.\n");
        fprintf(f, "  Strongest possible stability signal.\n");
    } else {
        fprintf(f, "\n  Year 1 best and Year 2 best are DIFFERENT configs.\n");
        fprintf(f, "  Expected -- if Year 1 best is top-5 in Year 2, edge is still robust.\n");
    }

    // -----------------------------------------------------------
    // SECTION 5: FULL 2YR SWEEP FOR REFERENCE
    // -----------------------------------------------------------
    fprintf(f, "\n========================================================================\n");
    fprintf(f, "  FULL 2YR SWEEP (reference, same as htf_bt_minimal)\n");
    fprintf(f, "========================================================================\n\n");
    fprintf(f, "%-3s %-5s %-5s | %-5s %-6s %-9s %-8s %-6s\n",
            "D", "SL", "TP", "n", "WR%", "PnL$", "DD", "PF");
    fprintf(f, "-------------------------------------------------------\n");
    for (const Result& r : full_all) {
        fprintf(f, "%-3d %-5.1f %-5.1f | %-5d %-6.1f %+9.2f %-8.2f %-6.2f\n",
                r.cfg.donchian_bars, r.cfg.sl_mult, r.cfg.tp_mult,
                r.n_trades, r.win_rate, r.total_pnl, r.max_dd, r.profit_factor);
    }

    fclose(f);
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <tick_csv>\n", argv[0]);
        return 1;
    }
    const char* csv_path = argv[1];
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
    printf("Ticks OK: %llu  failed: %llu\n",
           (unsigned long long)ticks_ok, (unsigned long long)ticks_fail);
    printf("Date range: %s -> %s\n", fmt_ts(first_ts).c_str(), fmt_ts(last_ts).c_str());
    printf("H4 bars built: %zu\n", bars.size());
    printf("Load time: %.1fs\n\n", load_s);
    fflush(stdout);

    // Split at midpoint by TIME (not by bar count). That is, midpoint timestamp.
    int64_t midpoint_ts = (first_ts + last_ts) / 2;
    int split_bar_idx = find_bar_at_or_after(bars, midpoint_ts);
    printf("=== PHASE 2: SPLIT ===\n");
    printf("Midpoint timestamp: %s\n", fmt_ts(midpoint_ts).c_str());
    printf("Split bar index:    %d (of %zu total)\n", split_bar_idx, bars.size());
    printf("Year 1: bars [0 .. %d)\n", split_bar_idx);
    printf("Year 2: bars [%d .. %zu)\n\n", split_bar_idx, bars.size());
    fflush(stdout);

    // Config grid
    std::vector<Config> cfgs;
    int    d_grid[] = {10, 15, 20};
    double sl_grid[] = {1.0, 1.5, 2.0};
    double tp_grid[] = {2.0, 3.0, 4.0};
    for (int d : d_grid)
        for (double sl : sl_grid)
            for (double tp : tp_grid)
                cfgs.push_back({d, sl, tp});

    printf("=== PHASE 3: YEAR 1 SWEEP ===\n");
    std::vector<Result> y1_all;
    for (const Config& c : cfgs) {
        Result r = run_one(bars, c, 0, split_bar_idx);
        y1_all.push_back(std::move(r));
    }
    // Year 1 best (requires n >= 30)
    int y1_best_idx = -1;
    for (int i = 0; i < (int)y1_all.size(); ++i) {
        if (y1_all[i].n_trades < 30) continue;
        if (y1_best_idx < 0 || y1_all[i].total_pnl > y1_all[y1_best_idx].total_pnl)
            y1_best_idx = i;
    }
    if (y1_best_idx < 0) {
        for (int i = 0; i < (int)y1_all.size(); ++i) {
            if (y1_best_idx < 0 || y1_all[i].total_pnl > y1_all[y1_best_idx].total_pnl)
                y1_best_idx = i;
        }
    }
    printf("  Year 1 best: D=%d SL=%.1f TP=%.1f  n=%d  PnL=$%+.2f  PF=%.2f\n\n",
           y1_all[y1_best_idx].cfg.donchian_bars,
           y1_all[y1_best_idx].cfg.sl_mult,
           y1_all[y1_best_idx].cfg.tp_mult,
           y1_all[y1_best_idx].n_trades,
           y1_all[y1_best_idx].total_pnl,
           y1_all[y1_best_idx].profit_factor);
    fflush(stdout);

    printf("=== PHASE 4: YEAR 2 SWEEP ===\n");
    std::vector<Result> y2_all;
    for (const Config& c : cfgs) {
        Result r = run_one(bars, c, split_bar_idx, (int)bars.size());
        y2_all.push_back(std::move(r));
    }
    // Year 2 best
    int y2_best_idx = -1;
    for (int i = 0; i < (int)y2_all.size(); ++i) {
        if (y2_all[i].n_trades < 20) continue;
        if (y2_best_idx < 0 || y2_all[i].total_pnl > y2_all[y2_best_idx].total_pnl)
            y2_best_idx = i;
    }
    if (y2_best_idx < 0) {
        for (int i = 0; i < (int)y2_all.size(); ++i) {
            if (y2_best_idx < 0 || y2_all[i].total_pnl > y2_all[y2_best_idx].total_pnl)
                y2_best_idx = i;
        }
    }
    printf("  Year 2 best: D=%d SL=%.1f TP=%.1f  n=%d  PnL=$%+.2f  PF=%.2f\n\n",
           y2_all[y2_best_idx].cfg.donchian_bars,
           y2_all[y2_best_idx].cfg.sl_mult,
           y2_all[y2_best_idx].cfg.tp_mult,
           y2_all[y2_best_idx].n_trades,
           y2_all[y2_best_idx].total_pnl,
           y2_all[y2_best_idx].profit_factor);
    fflush(stdout);

    printf("=== PHASE 5: FULL 2YR SWEEP (reference) ===\n");
    std::vector<Result> full_all;
    for (const Config& c : cfgs) {
        Result r = run_one(bars, c, 0, (int)bars.size());
        full_all.push_back(std::move(r));
    }
    fflush(stdout);

    // CRITICAL VERDICT: Year 1 best config performance in Year 2
    int y1best_y2_idx = y1_best_idx;
    const Result& verdict_y2 = y2_all[y1best_y2_idx];
    printf("\n=== WALK-FORWARD VERDICT ===\n");
    printf("Year 1 best config: D=%d SL=%.1f TP=%.1f\n",
           y1_all[y1_best_idx].cfg.donchian_bars,
           y1_all[y1_best_idx].cfg.sl_mult,
           y1_all[y1_best_idx].cfg.tp_mult);
    printf("  Year 1:  n=%-4d  PnL=$%+9.2f  PF=%.2f\n",
           y1_all[y1_best_idx].n_trades,
           y1_all[y1_best_idx].total_pnl,
           y1_all[y1_best_idx].profit_factor);
    printf("  Year 2:  n=%-4d  PnL=$%+9.2f  PF=%.2f\n",
           verdict_y2.n_trades, verdict_y2.total_pnl, verdict_y2.profit_factor);
    if (verdict_y2.profit_factor >= 1.2 && verdict_y2.total_pnl > 0) {
        printf("  VERDICT: EDGE HOLDS out-of-sample.\n");
    } else if (verdict_y2.total_pnl > 0 && verdict_y2.profit_factor > 1.0) {
        printf("  VERDICT: EDGE MARGINAL -- positive but weaker than in-sample.\n");
    } else {
        printf("  VERDICT: EDGE FAILED out-of-sample.\n");
    }
    fflush(stdout);

    auto t2 = std::chrono::steady_clock::now();
    double total_s = std::chrono::duration<double>(t2 - t0).count();

    write_report("htf_bt_walkforward_results.txt",
                 y1_all, y2_all, full_all,
                 y1_best_idx, y2_best_idx,
                 ticks_ok, ticks_fail,
                 first_ts, last_ts, midpoint_ts,
                 bars.size(), split_bar_idx, total_s);
    printf("\nWrote: htf_bt_walkforward_results.txt\n");
    printf("Total runtime: %.1fs\n", total_s);
    return 0;
}

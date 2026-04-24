// =============================================================================
// htf_bt_minimal.cpp -- Minimal H4 Donchian breakout sweep, XAUUSD 2yr
//
// Strategy:
//   - H4 bar close above Donchian-N high   -> LONG
//   - H4 bar close below Donchian-N low    -> SHORT
//   - SL = sl_mult * H4_ATR behind entry
//   - TP = tp_mult * H4_ATR ahead of entry
//   - No ADX, no EMA-sep, no RSI, no M15 ATR expansion
//   - Weekend entry gate only (no new entries Fri 20:00 UTC+)
//   - Fixed $10 risk per trade, 0.01 lot cap
//
// Sweep:
//   donchian_bars in {10, 15, 20}
//   sl_mult       in {1.0, 1.5, 2.0}
//   tp_mult       in {2.0, 3.0, 4.0}
//   => 27 configs
//
// Goal: establish whether a deliberately loose H4 breakout has base edge
// before adding filters. n-target: 50-200 trades per config over 2 years.
//
// Build:
//   clang++ -O3 -std=c++17 -o htf_bt_minimal htf_bt_minimal.cpp
// Run:
//   ./htf_bt_minimal /Users/jo/tick/2yr_XAUUSD_tick.csv
// Output:
//   htf_bt_minimal_results.txt         -- full sweep table + best config detail
//   htf_bt_minimal_best_trades.csv     -- trade log for best config
//   htf_bt_minimal_best_equity.csv     -- equity curve for best config
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
// Tick parsing (matches pdhl_bt_2y.cpp format exactly)
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
// Bar
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

// Pre-aggregate all ticks into H4 bars once, then iterate. Much faster than
// per-tick engine management when we only need bar-level data + intra-bar
// high/low for SL/TP intrabar checks.
struct H4BarWithTicks {
    Bar bar;
    // For intra-bar SL/TP evaluation we track OHLC sequence within the bar.
    // H4 contains ~4hr of ticks; we reduce to bid_high/low and ask_high/low
    // so SL/TP can be evaluated against realistic best/worst execution.
    double bid_low  = 1e18;
    double bid_high = 0.0;
    double ask_low  = 1e18;
    double ask_high = 0.0;
    // For chronological SL-before-TP determination within a bar, we track
    // the time-ordered bid_min_first flag: did bid reach its min BEFORE its max?
    // If entry is LONG: loss side is bid_min, win side is bid_max.
    // If entry is SHORT: loss side is ask_max, win side is ask_min.
    // Track: was min reached before max?
    bool   bid_min_before_max = false;
    bool   ask_max_before_min = false;
    int64_t bid_min_ts = 0, bid_max_ts = 0;
    int64_t ask_min_ts = 0, ask_max_ts = 0;
};

// =============================================================================
// Wilder ATR(14) on H4 bars
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
// Trade record
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

// =============================================================================
// Sweep config + result
// =============================================================================
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
    double max_dd   = 0.0;
    double profit_factor = 0.0;
    std::vector<Trade> trades;
};

// =============================================================================
// Timestamp helpers
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

// Weekend entry gate: no new entries Friday 20:00 UTC+
static bool is_weekend_gated(int64_t ts_ms) {
    const int64_t utc_sec  = ts_ms / 1000LL;
    const int     utc_dow  = static_cast<int>((utc_sec / 86400LL + 3) % 7);
    const int     utc_hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
    // Friday = dow 4; block from 20:00+. Saturday (dow=5), Sunday (dow=6) also blocked.
    if (utc_dow == 4 && utc_hour >= 20) return true;
    if (utc_dow == 5) return true;
    if (utc_dow == 6 && utc_hour < 22)  return true;  // gold reopens ~22:00 Sun UTC
    return false;
}

// =============================================================================
// Phase 1: pre-aggregate all ticks into H4 bars with intra-bar extremes
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
            cur.bar.open  = mid;
            cur.bar.high  = mid;
            cur.bar.low   = mid;
            cur.bar.close = mid;
            cur.bar.n     = 1;
            cur.bid_low  = tk.bid; cur.bid_high = tk.bid;
            cur.ask_low  = tk.ask; cur.ask_high = tk.ask;
            cur.bid_min_ts = cur.bid_max_ts = tk.ts_ms;
            cur.ask_min_ts = cur.ask_max_ts = tk.ts_ms;
            cur_anchor = anchor;
            have_cur = true;
            continue;
        }
        if (anchor != cur_anchor) {
            // H4 boundary crossed -- finalize and emit
            cur.bid_min_before_max = (cur.bid_min_ts < cur.bid_max_ts);
            cur.ask_max_before_min = (cur.ask_max_ts < cur.ask_min_ts);
            bars.push_back(cur);
            // start new bar
            cur = H4BarWithTicks{};
            cur.bar.ts_ms_open  = anchor;
            cur.bar.ts_ms_close = anchor + dur_ms;
            cur.bar.open  = mid;
            cur.bar.high  = mid;
            cur.bar.low   = mid;
            cur.bar.close = mid;
            cur.bar.n     = 1;
            cur.bid_low  = tk.bid; cur.bid_high = tk.bid;
            cur.ask_low  = tk.ask; cur.ask_high = tk.ask;
            cur.bid_min_ts = cur.bid_max_ts = tk.ts_ms;
            cur.ask_min_ts = cur.ask_max_ts = tk.ts_ms;
            cur_anchor = anchor;
            continue;
        }
        // update current bar
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
// Phase 2: run one config against pre-built H4 bars
// =============================================================================
static Result run_one(const std::vector<H4BarWithTicks>& bars, const Config& cfg)
{
    Result r;
    r.cfg = cfg;
    r.trades.reserve(512);

    ATR atr; atr.set(14);
    std::deque<double> highs, lows;

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

    for (size_t i = 0; i < bars.size(); ++i) {
        const H4BarWithTicks& bt = bars[i];
        const Bar& b = bt.bar;

        // Update ATR with this bar
        atr.push(b.high, b.low, b.close);

        // Update Donchian with this bar (rolling N, where N = cfg.donchian_bars)
        highs.push_back(b.high);
        lows .push_back(b.low);
        if ((int)highs.size() > cfg.donchian_bars) { highs.pop_front(); lows.pop_front(); }

        // ---------------------------------------------------------------
        // EXIT check for open position: did SL or TP hit during this bar?
        // Uses intra-bar bid_low/ask_high for long, bid_high/ask_low for short.
        // ---------------------------------------------------------------
        if (pos.active) {
            pos.bars_held++;
            bool sl_hit = false, tp_hit = false;
            double sl_px = 0.0, tp_px = 0.0;
            if (pos.is_long) {
                // Loss side: bid goes down to bid_low
                // Win side:  ask goes up to ask_high (but we exit at TP via bid typically;
                //   use bid_high since exit is closed at bid when selling a long)
                if (bt.bid_low  <= pos.sl) { sl_hit = true; sl_px = pos.sl; }
                if (bt.bid_high >= pos.tp) { tp_hit = true; tp_px = pos.tp; }
            } else {
                // SHORT: loss side is ask going up, win side is ask going down
                if (bt.ask_high >= pos.sl) { sl_hit = true; sl_px = pos.sl; }
                if (bt.ask_low  <= pos.tp) { tp_hit = true; tp_px = pos.tp; }
            }

            if (sl_hit && tp_hit) {
                // Both hit in the same bar -- use chronological order.
                // Long: SL is bid_min, TP is bid_max. SL first if bid_min_before_max.
                // Short: SL is ask_max, TP is ask_min. SL first if ask_max_before_min.
                bool sl_first = pos.is_long ? bt.bid_min_before_max : bt.ask_max_before_min;
                if (sl_first) { tp_hit = false; }
                else          { sl_hit = false; }
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

        // ---------------------------------------------------------------
        // ENTRY check on H4 bar close, only if no open position
        // ---------------------------------------------------------------
        if (pos.active) continue;
        if (!atr.primed) continue;
        if ((int)highs.size() < cfg.donchian_bars) continue;

        // Weekend entry gate (use bar-open ts for gate decision)
        if (is_weekend_gated(bt.bar.ts_ms_open)) continue;

        // Need the PRIOR window (exclude the bar we're evaluating) for Donchian.
        // The deque right now contains bars [i-N+1 .. i]. We want [i-N .. i-1].
        // Easiest: pop current bar off when computing.
        // But we already pushed. So recompute channel from [i-N .. i-1]:
        if (i + 1 < (size_t)cfg.donchian_bars) continue;
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
        // Entry at bar close price (proxied by mid; add half spread for realism)
        // Gold typical spread 0.2-0.5pts; use 0.3 as a reasonable static cost.
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

    // Compute summary stats
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
// Phase 3: write outputs
// =============================================================================
static void write_results(const char* path, const std::vector<Result>& all,
                          const Result& best,
                          uint64_t ticks_ok, uint64_t ticks_fail,
                          int64_t first_ts, int64_t last_ts,
                          size_t n_bars, double runtime_s)
{
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return; }

    fprintf(f, "========================================================================\n");
    fprintf(f, "  htf_bt_minimal -- H4 Donchian breakout sweep, XAUUSD 2yr tick\n");
    fprintf(f, "========================================================================\n\n");
    fprintf(f, "Ticks OK:        %llu\n", (unsigned long long)ticks_ok);
    fprintf(f, "Ticks failed:    %llu\n", (unsigned long long)ticks_fail);
    fprintf(f, "Date range:      %s -> %s\n", fmt_ts(first_ts).c_str(), fmt_ts(last_ts).c_str());
    fprintf(f, "H4 bars built:   %zu\n", n_bars);
    fprintf(f, "Runtime:         %.1fs\n\n", runtime_s);

    fprintf(f, "Sweep grid: donchian_bars x sl_mult x tp_mult\n");
    fprintf(f, "========================================================================\n\n");
    fprintf(f, "%-3s %-5s %-5s | %-5s %-6s %-9s %-9s %-9s %-8s %-6s\n",
            "D", "SL", "TP", "n", "WR%", "PnL$", "AvgWin", "AvgLoss", "DD", "PF");
    fprintf(f, "----------------------------------------------------------------------\n");
    for (const Result& r : all) {
        fprintf(f, "%-3d %-5.1f %-5.1f | %-5d %-6.1f %+9.2f %+9.2f %+9.2f %-8.2f %-6.2f\n",
                r.cfg.donchian_bars, r.cfg.sl_mult, r.cfg.tp_mult,
                r.n_trades, r.win_rate, r.total_pnl,
                r.avg_win, -r.avg_loss, r.max_dd, r.profit_factor);
    }

    fprintf(f, "\n========================================================================\n");
    fprintf(f, "  BEST CONFIG: D=%d SL=%.1f TP=%.1f\n",
            best.cfg.donchian_bars, best.cfg.sl_mult, best.cfg.tp_mult);
    fprintf(f, "========================================================================\n\n");
    fprintf(f, "Trades:          %d\n", best.n_trades);
    fprintf(f, "Wins:            %d (%.1f%%)\n", best.n_wins, best.win_rate);
    fprintf(f, "Total PnL:       $%+.2f\n", best.total_pnl);
    fprintf(f, "Avg win:         $%+.2f\n", best.avg_win);
    fprintf(f, "Avg loss:        $-%.2f\n", best.avg_loss);
    fprintf(f, "Expectancy:      $%+.2f/trade\n", best.expectancy);
    fprintf(f, "Max DD:          $-%.2f\n", best.max_dd);
    fprintf(f, "Profit factor:   %.2f\n", best.profit_factor);

    // Monthly breakdown for best config
    fprintf(f, "\nMonthly PnL (best config):\n");
    struct MB { int y = 0; int m = 0; int n = 0; double pnl = 0.0; };
    std::vector<MB> mb;
    auto find_ym = [&](int y, int m) -> MB& {
        for (MB& x : mb) if (x.y == y && x.m == m) return x;
        mb.push_back(MB{y, m, 0, 0.0});
        return mb.back();
    };
    for (const Trade& t : best.trades) {
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
        fprintf(f, "  %04d-%02d: n=%-3d pnl=$%+.2f cum=$%+.2f\n",
                x.y, x.m, x.n, x.pnl, cum);
    }

    // Exit reason breakdown for best config
    fprintf(f, "\nExit reasons (best config):\n");
    struct RB { std::string reason; int n = 0; double pnl = 0.0; };
    std::vector<RB> rb;
    auto find_or = [&](const std::string& s) -> RB& {
        for (RB& x : rb) if (x.reason == s) return x;
        rb.push_back(RB{s, 0, 0.0});
        return rb.back();
    };
    for (const Trade& t : best.trades) {
        RB& x = find_or(t.exit_reason);
        x.n += 1; x.pnl += t.pnl_usd;
    }
    std::sort(rb.begin(), rb.end(), [](const RB& a, const RB& b){ return a.n > b.n; });
    for (const RB& x : rb) {
        fprintf(f, "  %-16s n=%-4d pnl=$%+.2f\n", x.reason.c_str(), x.n, x.pnl);
    }

    fclose(f);
}

static void write_best_trades(const char* path, const std::vector<Trade>& trades) {
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return; }
    fprintf(f, "trade_id,entry_ts,exit_ts,side,entry,exit,sl,tp,size,bars_held,pnl_pts,pnl_usd,exit_reason\n");
    for (const Trade& t : trades) {
        fprintf(f, "%d,%s,%s,%s,%.3f,%.3f,%.3f,%.3f,%.4f,%d,%.3f,%.2f,%s\n",
                t.id, fmt_ts(t.entry_ts_ms).c_str(), fmt_ts(t.exit_ts_ms).c_str(),
                t.is_long ? "LONG" : "SHORT",
                t.entry, t.exit, t.sl, t.tp, t.size, t.bars_held,
                t.pnl_pts, t.pnl_usd, t.exit_reason.c_str());
    }
    fclose(f);
}

static void write_best_equity(const char* path, const std::vector<Trade>& trades) {
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return; }
    fprintf(f, "exit_ts,cumulative_pnl_usd\n");
    double cum = 0.0;
    for (const Trade& t : trades) {
        cum += t.pnl_usd;
        fprintf(f, "%s,%.2f\n", fmt_ts(t.exit_ts_ms).c_str(), cum);
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
    printf("Ticks OK: %llu  failed: %llu\n", (unsigned long long)ticks_ok,
           (unsigned long long)ticks_fail);
    printf("Date range: %s -> %s\n", fmt_ts(first_ts).c_str(), fmt_ts(last_ts).c_str());
    printf("H4 bars built: %zu\n", bars.size());
    printf("Load time: %.1fs\n\n", load_s);
    fflush(stdout);

    printf("=== PHASE 2: SWEEP ===\n");
    fflush(stdout);
    std::vector<Config> cfgs;
    int    d_grid[] = {10, 15, 20};
    double sl_grid[] = {1.0, 1.5, 2.0};
    double tp_grid[] = {2.0, 3.0, 4.0};
    for (int d : d_grid)
        for (double sl : sl_grid)
            for (double tp : tp_grid)
                cfgs.push_back({d, sl, tp});

    std::vector<Result> all;
    all.reserve(cfgs.size());
    int idx = 0;
    for (const Config& c : cfgs) {
        ++idx;
        Result r = run_one(bars, c);
        all.push_back(std::move(r));
        printf("  [%2d/%zu] D=%-2d SL=%.1f TP=%.1f  n=%-4d WR=%.1f%%  PnL=$%+.2f  DD=$-%.2f PF=%.2f\n",
               idx, cfgs.size(),
               all.back().cfg.donchian_bars,
               all.back().cfg.sl_mult,
               all.back().cfg.tp_mult,
               all.back().n_trades,
               all.back().win_rate,
               all.back().total_pnl,
               all.back().max_dd,
               all.back().profit_factor);
        fflush(stdout);
    }
    auto t2 = std::chrono::steady_clock::now();
    double sweep_s = std::chrono::duration<double>(t2 - t1).count();

    // Find best config by total PnL, requiring n >= 30 for statistical relevance.
    // If no config has n>=30, fall back to best PnL overall.
    const Result* best = nullptr;
    for (const Result& r : all) {
        if (r.n_trades < 30) continue;
        if (best == nullptr || r.total_pnl > best->total_pnl) best = &r;
    }
    if (best == nullptr) {
        for (const Result& r : all) {
            if (best == nullptr || r.total_pnl > best->total_pnl) best = &r;
        }
    }

    printf("\nBest config: D=%d SL=%.1f TP=%.1f  n=%d  PnL=$%+.2f  WR=%.1f%%  PF=%.2f\n",
           best->cfg.donchian_bars, best->cfg.sl_mult, best->cfg.tp_mult,
           best->n_trades, best->total_pnl, best->win_rate, best->profit_factor);
    printf("Sweep time: %.1fs\n\n", sweep_s);
    fflush(stdout);

    printf("=== PHASE 3: WRITE OUTPUTS ===\n");
    fflush(stdout);
    double total_s = std::chrono::duration<double>(t2 - t0).count();
    write_results       ("htf_bt_minimal_results.txt", all, *best,
                         ticks_ok, ticks_fail, first_ts, last_ts,
                         bars.size(), total_s);
    write_best_trades   ("htf_bt_minimal_best_trades.csv", best->trades);
    write_best_equity   ("htf_bt_minimal_best_equity.csv", best->trades);
    printf("Wrote:\n");
    printf("  htf_bt_minimal_results.txt         (sweep table + best config)\n");
    printf("  htf_bt_minimal_best_trades.csv     (best config trade log)\n");
    printf("  htf_bt_minimal_best_equity.csv     (best config equity curve)\n");
    printf("\nTotal runtime: %.1fs\n", total_s);

    return 0;
}

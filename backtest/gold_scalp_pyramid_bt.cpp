// =============================================================================
// gold_scalp_pyramid_bt.cpp -- M5 Gold Scalp + Pyramid + Aggressive Trail sweep
//
// Strategy: GoldScalpPyramid
//   - M5 bars on XAUUSD tick stream
//   - Entry: Donchian-N breakout filtered by EMA9/EMA21 alignment + momentum bar
//   - SL = ATR14 * sl_mult, TP = ATR14 * tp_mult
//   - Aggressive 4-phase trailing stop (BE lock -> profit lock -> tight trail -> very tight)
//   - Pyramiding up to 5 layers on trend continuation
//   - Session filter: 07-21 UTC (London+NY)
//   - Weekend gate (Fri 20:00+ through Sun 22:00)
//   - Cost model: 0.50pt total round-trip (spread + slip + commission)
//   - Max risk $50 per base layer, pyramid layers scale down
//
// Sweep grid:
//   lookback     {8, 12, 16}         -- Donchian channel bars
//   sl_mult      {0.8, 1.0, 1.5}     -- SL distance as ATR multiple
//   tp_mult      {1.5, 2.0, 3.0}     -- TP distance as ATR multiple
//   trail_tight  {0.30, 0.20, 0.12}  -- Tight trail distance as ATR fraction
//   pyramid_on   {false, true}       -- Enable pyramid adds
//   => 3 x 3 x 3 x 3 x 2 = 162 configs
//
// Supports both data formats:
//   Dukascopy:  timestamp_ms,askPrice,bidPrice  (header row)
//   Legacy:     YYYYMMDD,HH:MM:SS,bid,ask       (no header)
//
// Build:
//   clang++ -O3 -std=c++17 -o backtest/gold_scalp_pyramid_bt backtest/gold_scalp_pyramid_bt.cpp
// Run:
//   ./backtest/gold_scalp_pyramid_bt /Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv
// Output:
//   gold_scalp_pyramid_results.txt       -- full sweep table + best config detail
//   gold_scalp_pyramid_best_trades.csv   -- trade log for best config
//   gold_scalp_pyramid_best_equity.csv   -- equity curve for best config
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
// Tick
// =============================================================================
struct Tick {
    int64_t ts_ms;
    double  bid;
    double  ask;
    double  l2_imb = 0.5;  // 0..1, 0.5 = neutral; only populated for FMT_L2
};

// Format detection: set once after reading first line
enum DataFormat { FMT_UNKNOWN, FMT_LEGACY, FMT_DUKASCOPY, FMT_L2 };

// S38a-L2: parse L2 CSV format from VPS logs.
// ts_ms,mid,bid,ask,l2_imb,l2_bid_vol,l2_ask_vol,depth_bid_levels,depth_ask_levels,...
static bool parse_l2(const char* s, Tick& t) {
    if (!isdigit((unsigned char)s[0])) return false;
    char* end;
    int64_t ts = strtoll(s, &end, 10);
    if (end == s || *end != ',') return false;
    const char* p = end + 1;
    /* mid */ strtod(p, &end);
    if (end == p || *end != ',') return false;
    p = end + 1;
    double bid = strtod(p, &end);
    if (end == p || *end != ',') return false;
    p = end + 1;
    double ask = strtod(p, &end);
    if (end == p || *end != ',') return false;
    p = end + 1;
    double l2_imb = strtod(p, &end);
    if (end == p) return false;
    t.ts_ms = ts; t.bid = bid; t.ask = ask; t.l2_imb = l2_imb;
    return true;
}

static bool parse_legacy(const char* s, Tick& t) {
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

    int y = year, m = month;
    if (m <= 2) { y -= 1; }
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe/4 - yoe/100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    int64_t sec  = days * 86400 + H * 3600 + M * 60 + S;
    t.ts_ms = sec * 1000LL;
    t.bid = bid;
    t.ask = ask;
    return true;
}

static bool parse_dukascopy(const char* s, Tick& t) {
    if (!isdigit((unsigned char)s[0])) return false;
    char* end;
    int64_t ts = strtoll(s, &end, 10);
    if (end == s || *end != ',') return false;
    const char* p = end + 1;
    double ask_price = strtod(p, &end);
    if (end == p || *end != ',') return false;
    p = end + 1;
    double bid_price = strtod(p, &end);
    if (end == p) return false;
    t.ts_ms = ts;
    t.bid = bid_price;
    t.ask = ask_price;
    return true;
}

// =============================================================================
// M5 Bar with intra-bar extremes + pre-computed indicators
// =============================================================================
struct M5Bar {
    int64_t ts_open   = 0;
    int64_t ts_close  = 0;
    double  open      = 0.0;
    double  high      = 0.0;
    double  low       = 0.0;
    double  close     = 0.0;
    int     n         = 0;
    // Intra-bar extremes for realistic SL/TP/trail evaluation
    double  bid_low   = 1e18;
    double  bid_high  = 0.0;
    double  ask_low   = 1e18;
    double  ask_high  = 0.0;
    int64_t bid_min_ts = 0, bid_max_ts = 0;
    int64_t ask_min_ts = 0, ask_max_ts = 0;
    bool    bid_min_before_max = false;
    bool    ask_max_before_min = false;
    // Pre-computed indicators (filled after bar close)
    double  ema9      = 0.0;
    double  ema21     = 0.0;
    double  atr14     = 0.0;
    double  adx14     = 0.0;          // S38c: Wilder ADX(14) for chop filter
    bool    adx_primed = false;
    bool    atr_primed = false;
    // S38d: HTF + range expansion
    double  ema9_m15  = 0.0;          // M15 EMA9 (built from every 3rd M5 close)
    double  ema21_m15 = 0.0;          // M15 EMA21
    bool    htf_primed = false;
    double  range     = 0.0;          // bar high-low (cached for range-exp filter)
    // S38a-L2: L2 imbalance carried from ticks (last value in bar)
    double  l2_imb_close = 0.5;
    double  l2_imb_min   = 1.0;
    double  l2_imb_max   = 0.0;
    bool    has_l2       = false;
};

// =============================================================================
// EMA helper
// =============================================================================
struct EMA {
    int    period = 0;
    double value  = 0.0;
    double alpha  = 0.0;
    int    count  = 0;
    bool   primed = false;
    void init(int p) { period = p; alpha = 2.0 / (p + 1.0); value = 0.0; count = 0; primed = false; }
    void push(double v) {
        if (!primed) {
            value += v;
            ++count;
            if (count >= period) {
                value /= (double)period;
                primed = true;
            }
        } else {
            value = alpha * v + (1.0 - alpha) * value;
        }
    }
};

// =============================================================================
// Wilder ATR(14)
// =============================================================================
struct ATR {
    double value = 0.0;
    bool   primed = false;
    int    period = 14;
    double prev_close = 0.0;
    bool   have_prev = false;
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
// Wilder ADX(14) -- directional movement index
// =============================================================================
// ADX > 25 = trending. < 20 = chop. Asymmetric vs ER -- punishes lack of
// SUSTAINED directional pressure, not just net distance. Sustained pullbacks
// in trends still register as trending (high +DM dominance).
// =============================================================================
struct ADX {
    int    period     = 14;
    double tr_smooth  = 0.0;
    double pdm_smooth = 0.0;
    double mdm_smooth = 0.0;
    double adx_value  = 0.0;
    bool   primed_di  = false;
    bool   primed_adx = false;
    int    di_count   = 0;
    int    adx_count  = 0;
    double prev_high  = 0.0;
    double prev_low   = 0.0;
    double prev_close = 0.0;
    bool   have_prev  = false;
    double seed_tr    = 0.0;
    double seed_pdm   = 0.0;
    double seed_mdm   = 0.0;
    double seed_dx    = 0.0;

    void set(int p) {
        period = p;
        tr_smooth = pdm_smooth = mdm_smooth = adx_value = 0.0;
        primed_di = primed_adx = false;
        di_count = adx_count = 0;
        have_prev = false;
        seed_tr = seed_pdm = seed_mdm = seed_dx = 0.0;
    }

    void push(double high, double low, double close) {
        if (!have_prev) {
            prev_high = high; prev_low = low; prev_close = close;
            have_prev = true;
            return;
        }

        double up_move   = high - prev_high;
        double down_move = prev_low - low;
        double pdm = (up_move > down_move && up_move > 0.0) ? up_move : 0.0;
        double mdm = (down_move > up_move && down_move > 0.0) ? down_move : 0.0;
        double tr  = std::max(high - low,
                              std::max(std::fabs(high - prev_close),
                                       std::fabs(low - prev_close)));

        if (!primed_di) {
            seed_tr  += tr;
            seed_pdm += pdm;
            seed_mdm += mdm;
            ++di_count;
            if (di_count >= period) {
                tr_smooth  = seed_tr;
                pdm_smooth = seed_pdm;
                mdm_smooth = seed_mdm;
                primed_di  = true;
            }
        } else {
            // Wilder's smoothing
            tr_smooth  = tr_smooth  - (tr_smooth  / period) + tr;
            pdm_smooth = pdm_smooth - (pdm_smooth / period) + pdm;
            mdm_smooth = mdm_smooth - (mdm_smooth / period) + mdm;
        }

        prev_high = high; prev_low = low; prev_close = close;

        if (primed_di && tr_smooth > 1e-9) {
            double pdi = 100.0 * pdm_smooth / tr_smooth;
            double mdi = 100.0 * mdm_smooth / tr_smooth;
            double sum = pdi + mdi;
            if (sum > 1e-9) {
                double dx = 100.0 * std::fabs(pdi - mdi) / sum;
                if (!primed_adx) {
                    seed_dx += dx;
                    ++adx_count;
                    if (adx_count >= period) {
                        adx_value  = seed_dx / period;
                        primed_adx = true;
                    }
                } else {
                    adx_value = (adx_value * (period - 1) + dx) / period;
                }
            }
        }
    }
};

// =============================================================================
// Config + Result + Trade
// =============================================================================
struct Config {
    int    lookback;      // Donchian N bars
    double sl_mult;       // SL = ATR * sl_mult
    double tp_mult;       // TP = ATR * tp_mult
    double trail_tight;   // Tight trail = ATR * trail_tight behind MFE peak
    bool   pyramid_on;    // Enable pyramiding
};

struct Trade {
    int     id          = 0;
    int64_t entry_ts_ms = 0;
    int64_t exit_ts_ms  = 0;
    bool    is_long     = false;
    double  entry       = 0.0;
    double  exit_px     = 0.0;
    double  sl          = 0.0;
    double  tp          = 0.0;
    double  size        = 0.0;   // total size (all pyramid layers)
    double  pnl_pts     = 0.0;
    double  pnl_usd     = 0.0;
    double  mfe         = 0.0;
    int     bars_held   = 0;
    int     n_layers    = 0;     // how many pyramid layers entered
    std::string exit_reason;
};

struct Result {
    Config cfg;
    int    n_trades      = 0;
    int    n_wins        = 0;
    double total_pnl     = 0.0;
    double avg_win       = 0.0;
    double avg_loss      = 0.0;
    double win_rate      = 0.0;
    double expectancy    = 0.0;
    double max_dd        = 0.0;
    double profit_factor = 0.0;
    double avg_layers    = 0.0;  // average pyramid layers per trade
    int    stats_er_blocked = 0; // S38a: signals blocked by chop ER filter
    int    stats_l2_blocked = 0; // S38a-L2: signals blocked by L2 imb gate
    int    stats_l2_bars    = 0; // S38a-L2: bars with L2 data attached
    int    stats_adx_blocked = 0; // S38c: signals blocked by ADX gate
    int    stats_htf_blocked = 0; // S38d: signals blocked by HTF EMA align
    int    stats_rng_blocked = 0; // S38d: signals blocked by range expansion
    int    stats_tim_blocked = 0; // S38d: signals blocked by time-of-day
    std::vector<Trade> trades;
};

// =============================================================================
// Position with pyramid layers
// =============================================================================
static constexpr int MAX_LAYERS = 5;
static constexpr double COST_RT_PTS    = 0.60;   // S38a: realised BB cost @ 0.01 lot
static double           CHOP_ER_MIN    = 0.30;   // S38a: Kaufman ER threshold (CLI --er-min X)
static int              CHOP_ER_LOOKBACK = 10;   // S38a: bars for ER window (CLI --er-lb N)
static bool             g_er_enabled   = true;   // S38a: CLI --no-er disables
static bool             g_l2_gate      = false;  // S38a-L2: CLI --l2-gate enables L2 entry filter
static double           L2_LONG_MIN    = 0.58;   // l2_imb >= this for longs (CLI --l2-long X)
static double           L2_SHORT_MAX   = 0.42;   // l2_imb <= this for shorts (CLI --l2-short X)
static double           ADX_MIN        = 0.0;    // S38c: Wilder ADX gate (0 = off, CLI --adx-min N)
static double           ADX_MIN_OPEN   = 0.0;    // S38e: tighter ADX during open windows (07:55-08:15 + 13:25-13:45 UTC)
// S38d: extra chop filters
static bool             g_htf_align    = false;  // M15 EMA9 vs EMA21 alignment (CLI --htf)
static double           RANGE_EXP_MULT = 0.0;    // current bar range / N-bar avg (0 = off, CLI --range-exp X)
static int              RANGE_EXP_LB   = 10;     // N for avg-range (CLI --range-lb N)
static bool             g_time_block   = false;  // skip chop hours 11-13 + 21-23 UTC (CLI --time-block)
static bool             g_block_dead_hours = false;  // S38e: skip 17:00 + 21:00 UTC (data-driven, loss/breakeven hours)
static constexpr double HALF_SPREAD    = 0.25;   // half spread applied at entry/exit
static constexpr double USD_PER_PT_LOT = 100.0;  // XAUUSD: $100 per point per lot
static constexpr double RISK_DOLLARS   = 50.0;   // max risk per base layer
static constexpr double LOT_CAP        = 0.05;   // max lot per layer

struct Layer {
    bool   active  = false;
    double entry   = 0.0;
    double size    = 0.0;
};

struct Position {
    bool    active   = false;
    bool    is_long  = false;
    double  base_entry = 0.0;
    double  hard_sl  = 0.0;
    double  hard_tp  = 0.0;
    double  trail_sl = 0.0;     // current trailing SL (ratchets up for longs, down for shorts)
    double  mfe_peak = 0.0;     // max favorable excursion in points
    double  mfe_price = 0.0;    // price at MFE
    double  atr_at_entry = 0.0;
    int64_t entry_ts = 0;
    int     bars_held = 0;
    Layer   layers[MAX_LAYERS];
    int     n_layers = 0;
    int     next_pyramid_idx = 1; // next pyramid layer to add (0 = base, already placed)

    double total_size() const {
        double s = 0.0;
        for (int i = 0; i < n_layers; ++i) if (layers[i].active) s += layers[i].size;
        return s;
    }
    double weighted_entry() const {
        double sum_sv = 0.0, sum_s = 0.0;
        for (int i = 0; i < n_layers; ++i) {
            if (layers[i].active) {
                sum_sv += layers[i].entry * layers[i].size;
                sum_s  += layers[i].size;
            }
        }
        return sum_s > 0.0 ? sum_sv / sum_s : 0.0;
    }
    double compute_pnl(double exit_px) const {
        double pnl = 0.0;
        for (int i = 0; i < n_layers; ++i) {
            if (!layers[i].active) continue;
            double move = is_long ? (exit_px - layers[i].entry)
                                  : (layers[i].entry - exit_px);
            pnl += move * layers[i].size * USD_PER_PT_LOT;
        }
        return pnl;
    }
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

static bool is_weekend_gated(int64_t ts_ms) {
    const int64_t utc_sec  = ts_ms / 1000LL;
    const int     utc_dow  = static_cast<int>((utc_sec / 86400LL + 3) % 7);
    const int     utc_hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
    if (utc_dow == 4 && utc_hour >= 20) return true;  // Friday 20:00+
    if (utc_dow == 5) return true;                     // Saturday
    if (utc_dow == 6 && utc_hour < 22) return true;    // Sunday before 22:00
    return false;
}

// Session filter: only trade 07-21 UTC (London open through NY close)
static bool is_session_active(int64_t ts_ms) {
    const int64_t utc_sec  = ts_ms / 1000LL;
    const int     utc_hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
    return (utc_hour >= 7 && utc_hour < 21);
}

// =============================================================================
// Phase 1: Load ticks and build M5 bars with indicators
// =============================================================================
static std::vector<M5Bar> build_m5_bars(const char* csv_path,
                                         uint64_t& ticks_ok,
                                         uint64_t& ticks_fail,
                                         int64_t& first_ts,
                                         int64_t& last_ts)
{
    std::vector<M5Bar> bars;
    bars.reserve(400000);  // ~2 years of M5 = ~300K bars

    FILE* f = fopen(csv_path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", csv_path); return bars; }

    const int64_t BAR_MS = 300LL * 1000LL;  // 5 minutes
    int64_t cur_anchor = -1;
    M5Bar cur;
    bool have_cur = false;

    // Indicator state (computed incrementally as bars close)
    EMA ema9, ema21;
    ema9.init(9);
    ema21.init(21);
    ATR atr;
    atr.set(14);
    ADX adx;             // S38c: Wilder ADX(14)
    adx.set(14);
    // S38d: M15 EMA (sampled every 3rd M5 bar = 15min)
    EMA ema9_m15, ema21_m15;
    ema9_m15.init(9);
    ema21_m15.init(21);
    int m5_count_for_m15 = 0;
    double m15_open = 0.0, m15_high = 0.0, m15_low = 0.0, m15_close = 0.0;

    char line[256];
    uint64_t ok = 0, fail_count = 0;
    int64_t ft = 0, lt = 0;
    DataFormat fmt = FMT_UNKNOWN;

    while (fgets(line, sizeof(line), f)) {
        // Auto-detect format on first line
        if (fmt == FMT_UNKNOWN) {
            if (!isdigit((unsigned char)line[0])) {
                // Header line -- look for L2 marker
                if (strstr(line, "l2_imb")) {
                    fmt = FMT_L2;
                } else {
                    fmt = FMT_DUKASCOPY;
                }
                continue;
            }
            // Try legacy first (starts with 8 digits for date)
            if (strlen(line) >= 18 && line[8] == ',') {
                // Check if positions 9-10 look like HH (00-23)
                int h = (line[9]-'0')*10 + (line[10]-'0');
                if (h >= 0 && h <= 23 && line[11] == ':') {
                    fmt = FMT_LEGACY;
                } else {
                    fmt = FMT_DUKASCOPY;
                }
            } else {
                // Probably Dukascopy with epoch ms as first field
                fmt = FMT_DUKASCOPY;
            }
        }
        // Skip headers in mid-stream (e.g. concatenated daily L2 files)
        if (!isdigit((unsigned char)line[0])) continue;

        Tick tk;
        bool parsed = false;
        if (fmt == FMT_LEGACY) {
            parsed = parse_legacy(line, tk);
        } else if (fmt == FMT_L2) {
            parsed = parse_l2(line, tk);
        } else {
            parsed = parse_dukascopy(line, tk);
        }
        if (!parsed) { ++fail_count; continue; }
        ++ok;
        if (ft == 0) ft = tk.ts_ms;
        lt = tk.ts_ms;

        int64_t anchor = (tk.ts_ms / BAR_MS) * BAR_MS;
        double mid = (tk.bid + tk.ask) * 0.5;

        auto finalize_bar = [&]() {
            cur.bid_min_before_max = (cur.bid_min_ts < cur.bid_max_ts);
            cur.ask_max_before_min = (cur.ask_max_ts < cur.ask_min_ts);
            // Compute indicators
            ema9.push(cur.close);
            ema21.push(cur.close);
            atr.push(cur.high, cur.low, cur.close);
            adx.push(cur.high, cur.low, cur.close);   // S38c
            cur.ema9  = ema9.primed  ? ema9.value  : 0.0;
            cur.ema21 = ema21.primed ? ema21.value : 0.0;
            cur.atr14 = atr.value;
            cur.atr_primed = atr.primed;
            cur.adx14      = adx.adx_value;
            cur.adx_primed = adx.primed_adx;
            cur.range      = cur.high - cur.low;
            // S38d: M15 aggregation (every 3rd M5 bar = 15min)
            if (m5_count_for_m15 == 0) {
                m15_open = cur.open; m15_high = cur.high; m15_low = cur.low;
            } else {
                if (cur.high > m15_high) m15_high = cur.high;
                if (cur.low  < m15_low ) m15_low  = cur.low;
            }
            m15_close = cur.close;
            ++m5_count_for_m15;
            if (m5_count_for_m15 >= 3) {
                ema9_m15.push(m15_close);
                ema21_m15.push(m15_close);
                m5_count_for_m15 = 0;
            }
            cur.ema9_m15   = ema9_m15.primed  ? ema9_m15.value  : 0.0;
            cur.ema21_m15  = ema21_m15.primed ? ema21_m15.value : 0.0;
            cur.htf_primed = ema9_m15.primed && ema21_m15.primed;
            bars.push_back(cur);
        };

        if (!have_cur) {
            cur = M5Bar{};
            cur.ts_open  = anchor;
            cur.ts_close = anchor + BAR_MS;
            cur.open = mid; cur.high = mid; cur.low = mid; cur.close = mid;
            cur.n = 1;
            cur.bid_low = tk.bid; cur.bid_high = tk.bid;
            cur.ask_low = tk.ask; cur.ask_high = tk.ask;
            cur.bid_min_ts = cur.bid_max_ts = tk.ts_ms;
            cur.ask_min_ts = cur.ask_max_ts = tk.ts_ms;
            // S38a-L2: seed L2 bar fields from first tick
            if (fmt == FMT_L2) {
                cur.has_l2 = true;
                cur.l2_imb_close = tk.l2_imb;
                cur.l2_imb_min   = tk.l2_imb;
                cur.l2_imb_max   = tk.l2_imb;
            }
            cur_anchor = anchor;
            have_cur = true;
            continue;
        }

        if (anchor != cur_anchor) {
            finalize_bar();
            // Start new bar
            cur = M5Bar{};
            cur.ts_open  = anchor;
            cur.ts_close = anchor + BAR_MS;
            cur.open = mid; cur.high = mid; cur.low = mid; cur.close = mid;
            cur.n = 1;
            cur.bid_low = tk.bid; cur.bid_high = tk.bid;
            cur.ask_low = tk.ask; cur.ask_high = tk.ask;
            cur.bid_min_ts = cur.bid_max_ts = tk.ts_ms;
            cur.ask_min_ts = cur.ask_max_ts = tk.ts_ms;
            // S38a-L2: seed L2 bar fields from first tick
            if (fmt == FMT_L2) {
                cur.has_l2 = true;
                cur.l2_imb_close = tk.l2_imb;
                cur.l2_imb_min   = tk.l2_imb;
                cur.l2_imb_max   = tk.l2_imb;
            }
            cur_anchor = anchor;
            continue;
        }

        // Update current bar
        if (mid > cur.high) cur.high = mid;
        if (mid < cur.low)  cur.low  = mid;
        cur.close = mid;
        ++cur.n;
        if (tk.bid < cur.bid_low)  { cur.bid_low  = tk.bid; cur.bid_min_ts = tk.ts_ms; }
        if (tk.bid > cur.bid_high) { cur.bid_high = tk.bid; cur.bid_max_ts = tk.ts_ms; }
        if (tk.ask < cur.ask_low)  { cur.ask_low  = tk.ask; cur.ask_min_ts = tk.ts_ms; }
        if (tk.ask > cur.ask_high) { cur.ask_high = tk.ask; cur.ask_max_ts = tk.ts_ms; }
        // S38a-L2: carry L2 imbalance through bar
        if (fmt == FMT_L2) {
            cur.has_l2 = true;
            cur.l2_imb_close = tk.l2_imb;
            if (tk.l2_imb < cur.l2_imb_min) cur.l2_imb_min = tk.l2_imb;
            if (tk.l2_imb > cur.l2_imb_max) cur.l2_imb_max = tk.l2_imb;
        }
    }

    if (have_cur) {
        cur.bid_min_before_max = (cur.bid_min_ts < cur.bid_max_ts);
        cur.ask_max_before_min = (cur.ask_max_ts < cur.ask_min_ts);
        ema9.push(cur.close);
        ema21.push(cur.close);
        atr.push(cur.high, cur.low, cur.close);
        adx.push(cur.high, cur.low, cur.close);
        cur.ema9  = ema9.primed  ? ema9.value  : 0.0;
        cur.ema21 = ema21.primed ? ema21.value : 0.0;
        cur.atr14 = atr.value;
        cur.atr_primed = atr.primed;
        cur.adx14      = adx.adx_value;
        cur.adx_primed = adx.primed_adx;
        cur.range      = cur.high - cur.low;
        cur.ema9_m15   = ema9_m15.primed  ? ema9_m15.value  : 0.0;
        cur.ema21_m15  = ema21_m15.primed ? ema21_m15.value : 0.0;
        cur.htf_primed = ema9_m15.primed && ema21_m15.primed;
        bars.push_back(cur);
    }

    fclose(f);
    ticks_ok   = ok;
    ticks_fail = fail_count;
    first_ts   = ft;
    last_ts    = lt;
    return bars;
}

// =============================================================================
// Phase 2: Run one config against pre-built M5 bars
// =============================================================================
static Result run_one(const std::vector<M5Bar>& bars, const Config& cfg) {
    Result r;
    r.cfg = cfg;
    r.trades.reserve(2048);

    Position pos{};
    int trade_id = 0;
    int64_t cooldown_until = 0;
    double total_layers = 0.0;
    int stats_er_blocked = 0;  // S38a: local counter, copied to r at end
    int stats_l2_blocked = 0;  // S38a-L2: local L2 block counter
    int stats_l2_bars    = 0;  // S38a-L2: bars with L2 attached
    int stats_adx_blocked = 0; // S38c: local ADX block counter
    int stats_htf_blocked = 0; // S38d
    int stats_rng_blocked = 0;
    int stats_tim_blocked = 0;

    for (size_t i = 0; i < bars.size(); ++i) {
        const M5Bar& b = bars[i];

        // ---------------------------------------------------------------
        // MANAGE open position
        // ---------------------------------------------------------------
        if (pos.active) {
            pos.bars_held++;

            // --- Update MFE from intra-bar extremes ---
            double best_price, worst_price;
            int64_t best_ts, worst_ts;
            if (pos.is_long) {
                best_price  = b.bid_high;   // can sell at bid high
                worst_price = b.bid_low;    // worst exit at bid low
                best_ts     = b.bid_max_ts;
                worst_ts    = b.bid_min_ts;
            } else {
                best_price  = b.ask_low;    // can buy back at ask low
                worst_price = b.ask_high;   // worst exit at ask high
                best_ts     = b.ask_min_ts;
                worst_ts    = b.ask_max_ts;
            }

            // --- Chronological intra-bar simulation ---
            // Within an M5 bar, the favorable and adverse extremes happened
            // at known timestamps. We use this to evaluate events in the
            // correct order: if MFE peak happened FIRST, pyramid adds fire
            // at the peak before any retracement can trigger the trail.
            // If the adverse move happened first, we check exits before
            // updating MFE (the position might close before the peak).
            //
            // FIX 2026-05-18: previous version always checked exits before
            // pyramid adds, so pyramids never fired (the tight trail caught
            // the retracement on the same bar that crossed the threshold).

            const bool favorable_first = (best_ts <= worst_ts);

            // Helper lambda: update MFE + evaluate pyramid adds
            auto do_mfe_and_pyramid = [&]() {
                double cur_mfe_pts = pos.is_long
                    ? (best_price - pos.base_entry)
                    : (pos.base_entry - best_price);
                if (cur_mfe_pts > pos.mfe_peak) {
                    pos.mfe_peak  = cur_mfe_pts;
                    pos.mfe_price = best_price;
                }

                // Pyramid adds fire at MFE peak (before retracement)
                if (cfg.pyramid_on && pos.next_pyramid_idx < MAX_LAYERS) {
                    double sl_dist = pos.atr_at_entry * cfg.sl_mult;
                    static const double pyr_thresh[] = {0.0, 1.0, 1.5, 2.0, 3.0};
                    static const double pyr_size_mult[] = {1.0, 0.80, 0.65, 0.50, 0.40};

                    // Check ALL eligible layers (MFE might cross multiple thresholds in one bar)
                    while (pos.next_pyramid_idx < MAX_LAYERS) {
                        int idx = pos.next_pyramid_idx;
                        double threshold = pyr_thresh[idx] * sl_dist;
                        if (pos.mfe_peak < threshold) break;

                        double base_size = pos.layers[0].size;
                        double add_size  = base_size * pyr_size_mult[idx];
                        add_size = std::max(0.01, std::min(LOT_CAP, add_size));

                        pos.layers[idx].active = true;
                        pos.layers[idx].entry  = pos.is_long
                            ? (pos.base_entry + pos.mfe_peak - HALF_SPREAD)
                            : (pos.base_entry - pos.mfe_peak + HALF_SPREAD);
                        pos.layers[idx].size   = add_size;
                        pos.n_layers = idx + 1;
                        pos.next_pyramid_idx = idx + 1;
                    }
                }
            };

            // Helper lambda: compute trail + check exits, returns true if position closed
            auto do_trail_and_exits = [&]() -> bool {
                // --- Compute trail SL (4-phase aggressive trailing) ---
                double cost_pts = COST_RT_PTS;
                double new_trail = pos.hard_sl;

                // Phase 1: BE lock at MFE >= cost * 2.5
                if (pos.mfe_peak >= cost_pts * 2.5) {
                    double be_level = pos.is_long
                        ? (pos.base_entry + cost_pts)
                        : (pos.base_entry - cost_pts);
                    if (pos.is_long)  new_trail = std::max(new_trail, be_level);
                    else              new_trail = std::min(new_trail, be_level);
                }

                // Phase 2: Profit lock at 35% of MFE
                if (pos.mfe_peak >= pos.atr_at_entry * 0.4) {
                    double lock_pts = pos.mfe_peak * 0.35;
                    double lock_level = pos.is_long
                        ? (pos.base_entry + lock_pts)
                        : (pos.base_entry - lock_pts);
                    if (pos.is_long)  new_trail = std::max(new_trail, lock_level);
                    else              new_trail = std::min(new_trail, lock_level);
                }

                // Phase 3: Tight trail behind MFE peak
                if (pos.mfe_peak >= pos.atr_at_entry * 0.7) {
                    double trail_dist = pos.atr_at_entry * cfg.trail_tight;
                    double trail_level = pos.is_long
                        ? (pos.mfe_price - trail_dist)
                        : (pos.mfe_price + trail_dist);
                    if (pos.is_long)  new_trail = std::max(new_trail, trail_level);
                    else              new_trail = std::min(new_trail, trail_level);
                }

                // Phase 4: Very tight trail at MFE >= ATR * 1.2
                if (pos.mfe_peak >= pos.atr_at_entry * 1.2) {
                    double trail_dist = pos.atr_at_entry * cfg.trail_tight * 0.60;
                    double trail_level = pos.is_long
                        ? (pos.mfe_price - trail_dist)
                        : (pos.mfe_price + trail_dist);
                    if (pos.is_long)  new_trail = std::max(new_trail, trail_level);
                    else              new_trail = std::min(new_trail, trail_level);
                }

                // Ratchet: trail SL only moves in favorable direction
                if (pos.is_long)  pos.trail_sl = std::max(pos.trail_sl, new_trail);
                else              pos.trail_sl = std::min(pos.trail_sl, new_trail);

                // --- Check SL / TP / Trail hits ---
                bool sl_hit = false, tp_hit = false, trail_hit = false;
                double exit_px = 0.0;

                double eff_sl = pos.is_long
                    ? std::max(pos.hard_sl, pos.trail_sl)
                    : std::min(pos.hard_sl, pos.trail_sl);

                if (pos.is_long) {
                    if (worst_price <= eff_sl) {
                        if (pos.trail_sl > pos.hard_sl) trail_hit = true;
                        else sl_hit = true;
                        exit_px = eff_sl;
                    }
                    if (best_price >= pos.hard_tp) { tp_hit = true; exit_px = pos.hard_tp; }
                } else {
                    if (worst_price >= eff_sl) {
                        if (pos.trail_sl < pos.hard_sl) trail_hit = true;
                        else sl_hit = true;
                        exit_px = eff_sl;
                    }
                    if (best_price <= pos.hard_tp) { tp_hit = true; exit_px = pos.hard_tp; }
                }

                // Same-bar conflict: use chronological order
                if ((sl_hit || trail_hit) && tp_hit) {
                    bool stop_first = (worst_ts < best_ts);
                    if (stop_first) tp_hit = false;
                    else { sl_hit = false; trail_hit = false; }
                }

                // Time stop: max 12 bars (60 minutes)
                bool time_stop = (pos.bars_held >= 12);
                if (time_stop && !sl_hit && !trail_hit && !tp_hit) {
                    exit_px = b.close;
                }

                if (sl_hit || trail_hit || tp_hit || time_stop) {
                    Trade t;
                    t.id = ++trade_id;
                    t.entry_ts_ms = pos.entry_ts;
                    t.exit_ts_ms  = b.ts_close;
                    t.is_long     = pos.is_long;
                    t.entry       = pos.weighted_entry();
                    t.exit_px     = exit_px;
                    t.sl          = eff_sl;
                    t.tp          = pos.hard_tp;
                    t.size        = pos.total_size();
                    t.pnl_usd     = pos.compute_pnl(exit_px);
                    t.pnl_pts     = pos.is_long ? (exit_px - t.entry) : (t.entry - exit_px);
                    t.mfe         = pos.mfe_peak;
                    t.bars_held   = pos.bars_held;
                    t.n_layers    = pos.n_layers;
                    if (sl_hit)       t.exit_reason = "SL_HIT";
                    else if (trail_hit) t.exit_reason = "TRAIL_HIT";
                    else if (tp_hit)  t.exit_reason = "TP_HIT";
                    else              t.exit_reason = "TIME_STOP";
                    r.trades.push_back(t);
                    total_layers += pos.n_layers;
                    cooldown_until = b.ts_close + 60LL * 1000LL;
                    pos = Position{};
                    return true;
                }
                return false;
            };

            // --- Execute in chronological order ---
            if (favorable_first) {
                // Price moved favorably first: MFE peak + pyramid, THEN retracement
                do_mfe_and_pyramid();
                if (do_trail_and_exits()) continue;
            } else {
                // Adverse move happened first: check exits with PREVIOUS bar's trail
                // If position survives, update MFE + pyramid from the later favorable move
                // (Use previous trail -- don't update MFE yet since peak hadn't happened)
                if (do_trail_and_exits()) continue;
                do_mfe_and_pyramid();
                // After pyramid adds, re-ratchet trail for the new MFE (no exit re-check
                // since worst_price already passed without hitting)
            }

            continue;  // don't try new entry while position open
        }

        // ---------------------------------------------------------------
        // ENTRY: Donchian breakout on M5 close, EMA-filtered
        // ---------------------------------------------------------------
        if (!b.atr_primed) continue;
        if (b.ema9 == 0.0 || b.ema21 == 0.0) continue;
        if ((int)i < cfg.lookback) continue;
        if (is_weekend_gated(b.ts_open)) continue;
        if (!is_session_active(b.ts_open)) continue;
        if (b.ts_close < cooldown_until) continue;

        // ATR floor: don't trade dead markets
        if (b.atr14 < 1.50) continue;
        // ATR cap: don't trade extreme volatility (news spikes)
        if (b.atr14 > 15.0) continue;

        // S38c/S38e: ADX directional-strength gate, time-of-day-aware.
        // ADX < threshold = no sustained directional pressure = chop.
        // S38e: during London open (07:55-08:15) + NY open (13:25-13:45) UTC
        // -- known whipsaw windows -- use ADX_MIN_OPEN (tighter) if set.
        {
            double adx_thresh = ADX_MIN;
            if (ADX_MIN_OPEN > ADX_MIN) {
                time_t tt = b.ts_open / 1000;
                struct tm gm;
#ifdef _WIN32
                gmtime_s(&gm, &tt);
#else
                gmtime_r(&tt, &gm);
#endif
                int hm = gm.tm_hour * 100 + gm.tm_min;
                if ((hm >= 755 && hm < 815) || (hm >= 1325 && hm < 1345)) {
                    adx_thresh = ADX_MIN_OPEN;
                }
            }
            if (adx_thresh > 0.0 && b.adx_primed && b.adx14 < adx_thresh) {
                ++stats_adx_blocked;
                continue;
            }
        }

        // S38e: data-driven dead-hour block (17:00 + 21:00 UTC).
        // 2yr backtest: hour-21 PnL -$45 / 9 trades. hour-17 PnL -$36 / 361 trades.
        // Both lose or breakeven. Block them outright.
        if (g_block_dead_hours) {
            time_t tt = b.ts_open / 1000;
            struct tm gm;
#ifdef _WIN32
            gmtime_s(&gm, &tt);
#else
            gmtime_r(&tt, &gm);
#endif
            int h = gm.tm_hour;
            if (h == 17 || h == 21) {
                ++stats_tim_blocked;
                continue;
            }
        }

        // S38d: time-of-day chop block.
        // London lunch (11-13 UTC) + post-NY close (21-23 UTC) = known chop windows.
        if (g_time_block) {
            time_t tt = b.ts_open / 1000;
            struct tm gm;
#ifdef _WIN32
            gmtime_s(&gm, &tt);
#else
            gmtime_r(&tt, &gm);
#endif
            int h = gm.tm_hour;
            if ((h >= 11 && h < 13) || (h >= 21 && h < 23)) {
                ++stats_tim_blocked;
                continue;
            }
        }

        // S38d: range-expansion filter.
        // current bar range / avg(last N bars range) must exceed mult.
        // Chop bars are small; momentum bars are big.
        if (RANGE_EXP_MULT > 0.0 && (int)i >= RANGE_EXP_LB) {
            double sum_r = 0.0;
            for (int k = (int)i - RANGE_EXP_LB; k < (int)i; ++k) {
                sum_r += bars[k].range;
            }
            double avg_r = sum_r / RANGE_EXP_LB;
            if (avg_r > 1e-9 && b.range / avg_r < RANGE_EXP_MULT) {
                ++stats_rng_blocked;
                continue;
            }
        }

        // S38a: Kaufman Efficiency Ratio chop filter.
        // ER = |close[t] - close[t-N]| / sum(|close diffs|). < threshold = chop.
        if (g_er_enabled && (int)i >= CHOP_ER_LOOKBACK) {
            double net = std::fabs(bars[i].close - bars[i - CHOP_ER_LOOKBACK].close);
            double path = 0.0;
            for (int k = (int)i - CHOP_ER_LOOKBACK + 1; k <= (int)i; ++k) {
                path += std::fabs(bars[k].close - bars[k - 1].close);
            }
            double er = (path > 1e-9) ? (net / path) : 0.0;
            if (er < CHOP_ER_MIN) { ++stats_er_blocked; continue; }
        }

        // Compute Donchian channel over prior N bars (excluding current)
        double ch_high = -1e18, ch_low = 1e18;
        for (int k = (int)i - cfg.lookback; k < (int)i; ++k) {
            if (bars[k].high > ch_high) ch_high = bars[k].high;
            if (bars[k].low  < ch_low)  ch_low  = bars[k].low;
        }

        bool bull_break = (b.close > ch_high);
        bool bear_break = (b.close < ch_low);
        if (!bull_break && !bear_break) continue;

        bool intend_long = bull_break;

        // EMA trend alignment filter
        if (intend_long  && b.ema9 <= b.ema21) continue;
        if (!intend_long && b.ema9 >= b.ema21) continue;

        // S38d: HTF (M15) EMA alignment -- M5 momentum must match M15 direction.
        if (g_htf_align && b.htf_primed) {
            if (intend_long  && b.ema9_m15 <= b.ema21_m15) { ++stats_htf_blocked; continue; }
            if (!intend_long && b.ema9_m15 >= b.ema21_m15) { ++stats_htf_blocked; continue; }
        }

        // S38a-L2: L2 imbalance entry confirmation.
        // Only active when bar has L2 data AND --l2-gate flag set.
        // Long requires l2_imb_close >= 0.58 (bids stacked), short <= 0.42 (offers).
        if (g_l2_gate && b.has_l2) {
            ++stats_l2_bars;
            if (intend_long  && b.l2_imb_close < L2_LONG_MIN) { ++stats_l2_blocked; continue; }
            if (!intend_long && b.l2_imb_close > L2_SHORT_MAX) { ++stats_l2_blocked; continue; }
        }

        // Momentum bar filter: bar body > 40% of range, close in direction
        double body = std::fabs(b.close - b.open);
        double range = b.high - b.low;
        if (range < 0.01) continue;
        if (body / range < 0.40) continue;
        // Close must be in the correct half of the range
        if (intend_long  && b.close < (b.high + b.low) * 0.5) continue;
        if (!intend_long && b.close > (b.high + b.low) * 0.5) continue;

        // Compute SL/TP
        double sl_pts = b.atr14 * cfg.sl_mult;
        double tp_pts = b.atr14 * cfg.tp_mult;

        // Cost gate: TP must cover cost * 1.5
        double tp_usd_at_min_lot = tp_pts * USD_PER_PT_LOT * 0.01;
        double cost_usd_at_min_lot = COST_RT_PTS * USD_PER_PT_LOT * 0.01;
        if (tp_usd_at_min_lot < cost_usd_at_min_lot * 1.5) continue;

        // Spread check: require spread < 0.80 pts
        // Use bar midpoint spread proxy (avg ask - avg bid within bar)
        double spread_est = (b.ask_low + b.ask_high) * 0.5 - (b.bid_low + b.bid_high) * 0.5;
        if (spread_est > 0.80) continue;

        // Entry price: ask for long, bid for short (+ half spread for realism)
        double entry_px = intend_long
            ? (b.close + HALF_SPREAD)
            : (b.close - HALF_SPREAD);
        double sl_px = intend_long ? (entry_px - sl_pts) : (entry_px + sl_pts);
        double tp_px = intend_long ? (entry_px + tp_pts) : (entry_px - tp_pts);

        // Position sizing: $50 risk / SL distance / $100 per pt per lot
        double size = RISK_DOLLARS / (sl_pts * USD_PER_PT_LOT);
        size = std::floor(size / 0.01) * 0.01;
        size = std::max(0.01, std::min(LOT_CAP, size));

        // Open position
        pos = Position{};
        pos.active = true;
        pos.is_long = intend_long;
        pos.base_entry = entry_px;
        pos.hard_sl = sl_px;
        pos.hard_tp = tp_px;
        pos.trail_sl = sl_px;  // trail starts at hard SL
        pos.mfe_peak = 0.0;
        pos.mfe_price = entry_px;
        pos.atr_at_entry = b.atr14;
        pos.entry_ts = b.ts_close;
        pos.bars_held = 0;
        pos.layers[0] = {true, entry_px, size};
        pos.n_layers = 1;
        pos.next_pyramid_idx = 1;
    }

    // Force-close any open position at end of data
    if (pos.active && !bars.empty()) {
        const M5Bar& last = bars.back();
        double exit_px = last.close;
        Trade t;
        t.id = ++trade_id;
        t.entry_ts_ms = pos.entry_ts;
        t.exit_ts_ms  = last.ts_close;
        t.is_long     = pos.is_long;
        t.entry       = pos.weighted_entry();
        t.exit_px     = exit_px;
        t.sl          = pos.hard_sl;
        t.tp          = pos.hard_tp;
        t.size        = pos.total_size();
        t.pnl_usd    = pos.compute_pnl(exit_px);
        t.pnl_pts    = pos.is_long ? (exit_px - t.entry) : (t.entry - exit_px);
        t.mfe        = pos.mfe_peak;
        t.bars_held  = pos.bars_held;
        t.n_layers   = pos.n_layers;
        t.exit_reason = "DATA_END";
        r.trades.push_back(t);
        total_layers += pos.n_layers;
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
        else                   { ++nl; gl += -t.pnl_usd; }
    }
    r.n_trades = (int)r.trades.size();
    r.n_wins = nw;
    r.total_pnl = cum;
    r.max_dd = max_dd;
    if (nw > 0)        r.avg_win  = gw / nw;
    if (nl > 0)        r.avg_loss = gl / nl;
    if (r.n_trades > 0) {
        r.win_rate    = 100.0 * nw / r.n_trades;
        r.expectancy  = r.total_pnl / r.n_trades;
        r.avg_layers  = total_layers / r.n_trades;
    }
    if (gl > 0.0) r.profit_factor = gw / gl;
    r.stats_er_blocked  = stats_er_blocked;  // S38a: surface chop-filter blocks
    r.stats_l2_blocked  = stats_l2_blocked;
    r.stats_l2_bars     = stats_l2_bars;
    r.stats_adx_blocked = stats_adx_blocked; // S38c
    r.stats_htf_blocked = stats_htf_blocked; // S38d
    r.stats_rng_blocked = stats_rng_blocked;
    r.stats_tim_blocked = stats_tim_blocked;
    return r;
}

// =============================================================================
// Phase 3: Write outputs
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
    fprintf(f, "  GoldScalpPyramid -- M5 Donchian + EMA + Trail + Pyramid sweep, XAUUSD\n");
    fprintf(f, "========================================================================\n\n");
    fprintf(f, "Ticks OK:        %llu\n", (unsigned long long)ticks_ok);
    fprintf(f, "Ticks failed:    %llu\n", (unsigned long long)ticks_fail);
    fprintf(f, "Date range:      %s -> %s\n", fmt_ts(first_ts).c_str(), fmt_ts(last_ts).c_str());
    fprintf(f, "M5 bars built:   %zu\n", n_bars);
    fprintf(f, "Runtime:         %.1fs\n\n", runtime_s);
    fprintf(f, "Cost model:      %.2f pts RT (half_spread=%.2f)\n", COST_RT_PTS, HALF_SPREAD);
    fprintf(f, "Risk per trade:  $%.0f, lot cap %.2f per layer\n", RISK_DOLLARS, LOT_CAP);
    fprintf(f, "Session filter:  07-21 UTC\n");
    fprintf(f, "ATR floor/cap:   1.50 / 15.00\n");
    fprintf(f, "Max hold:        12 bars (60 min)\n\n");

    fprintf(f, "Sweep grid: lookback x sl_mult x tp_mult x trail_tight x pyramid\n");
    fprintf(f, "========================================================================\n\n");
    fprintf(f, "%-3s %-5s %-5s %-6s %-4s | %-5s %-6s %10s %-9s %-9s %-8s %-6s %-5s\n",
            "LB", "SL", "TP", "Trail", "Pyr", "n", "WR%", "PnL$", "AvgWin", "AvgLoss", "DD", "PF", "Lyrs");
    fprintf(f, "-------------------------------------------------------------------------\n");

    // Sort all results by PF descending for readability
    std::vector<const Result*> sorted;
    for (const Result& rr : all) sorted.push_back(&rr);
    std::sort(sorted.begin(), sorted.end(), [](const Result* a, const Result* b) {
        return a->profit_factor > b->profit_factor;
    });

    for (const Result* rr : sorted) {
        fprintf(f, "%-3d %-5.1f %-5.1f %-6.2f %-4s | %-5d %-6.1f %+10.2f %+9.2f %+9.2f %-8.2f %-6.2f %-5.1f\n",
                rr->cfg.lookback, rr->cfg.sl_mult, rr->cfg.tp_mult,
                rr->cfg.trail_tight, rr->cfg.pyramid_on ? "Y" : "N",
                rr->n_trades, rr->win_rate, rr->total_pnl,
                rr->avg_win, -rr->avg_loss, rr->max_dd,
                rr->profit_factor, rr->avg_layers);
    }

    fprintf(f, "\n========================================================================\n");
    fprintf(f, "  BEST CONFIG: LB=%d SL=%.1f TP=%.1f Trail=%.2f Pyr=%s\n",
            best.cfg.lookback, best.cfg.sl_mult, best.cfg.tp_mult,
            best.cfg.trail_tight, best.cfg.pyramid_on ? "Y" : "N");
    fprintf(f, "========================================================================\n\n");
    fprintf(f, "Trades:          %d\n", best.n_trades);
    fprintf(f, "Wins:            %d (%.1f%%)\n", best.n_wins, best.win_rate);
    fprintf(f, "Total PnL:       $%+.2f\n", best.total_pnl);
    fprintf(f, "Avg win:         $%+.2f\n", best.avg_win);
    fprintf(f, "Avg loss:        $-%.2f\n", best.avg_loss);
    fprintf(f, "Expectancy:      $%+.2f/trade\n", best.expectancy);
    fprintf(f, "Max DD:          $-%.2f\n", best.max_dd);
    fprintf(f, "Profit factor:   %.2f\n", best.profit_factor);
    fprintf(f, "Avg layers/trade:%.1f\n", best.avg_layers);
    fprintf(f, "ER blocked:      %d (chop filter %s)\n",
            best.stats_er_blocked, g_er_enabled ? "ON" : "OFF");
    fprintf(f, "L2 blocked:      %d / %d bars-with-L2 (L2 gate %s)\n",
            best.stats_l2_blocked, best.stats_l2_bars,
            g_l2_gate ? "ON" : "OFF");
    fprintf(f, "ADX blocked:     %d (ADX_MIN=%.1f)\n",
            best.stats_adx_blocked, ADX_MIN);
    fprintf(f, "HTF blocked:     %d (M15 align %s)\n",
            best.stats_htf_blocked, g_htf_align ? "ON" : "OFF");
    fprintf(f, "RNG blocked:     %d (range_exp_mult=%.2f lb=%d)\n",
            best.stats_rng_blocked, RANGE_EXP_MULT, RANGE_EXP_LB);
    fprintf(f, "TIME blocked:    %d (time-block %s)\n",
            best.stats_tim_blocked, g_time_block ? "ON" : "OFF");

    // Monthly breakdown
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
        fprintf(f, "  %04d-%02d: n=%-4d pnl=$%+9.2f cum=$%+9.2f\n",
                x.y, x.m, x.n, x.pnl, cum);
    }

    // Exit reason breakdown
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
        fprintf(f, "  %-16s n=%-5d pnl=$%+9.2f  avg=$%+.2f\n",
                x.reason.c_str(), x.n, x.pnl, x.n > 0 ? x.pnl / x.n : 0.0);
    }

    // Pyramid layer distribution
    fprintf(f, "\nPyramid layer distribution (best config):\n");
    int layer_counts[MAX_LAYERS + 1] = {};
    for (const Trade& t : best.trades) {
        if (t.n_layers >= 1 && t.n_layers <= MAX_LAYERS) layer_counts[t.n_layers]++;
    }
    for (int l = 1; l <= MAX_LAYERS; ++l) {
        if (layer_counts[l] > 0) {
            fprintf(f, "  %d layer(s): %d trades (%.1f%%)\n",
                    l, layer_counts[l], 100.0 * layer_counts[l] / best.n_trades);
        }
    }

    // Top-10 best and worst trades
    fprintf(f, "\nTop 10 wins (best config):\n");
    std::vector<const Trade*> by_pnl;
    for (const Trade& t : best.trades) by_pnl.push_back(&t);
    std::sort(by_pnl.begin(), by_pnl.end(), [](const Trade* a, const Trade* b){ return a->pnl_usd > b->pnl_usd; });
    int show = std::min(10, (int)by_pnl.size());
    for (int j = 0; j < show; ++j) {
        const Trade* t = by_pnl[j];
        fprintf(f, "  #%-4d %s %5s entry=%.2f exit=%.2f pnl=$%+.2f mfe=%.1f layers=%d %s\n",
                t->id, fmt_ts(t->entry_ts_ms).c_str(),
                t->is_long ? "LONG" : "SHORT",
                t->entry, t->exit_px, t->pnl_usd, t->mfe, t->n_layers,
                t->exit_reason.c_str());
    }
    fprintf(f, "\nTop 10 losses (best config):\n");
    for (int j = (int)by_pnl.size() - 1; j >= std::max(0, (int)by_pnl.size() - 10); --j) {
        const Trade* t = by_pnl[j];
        fprintf(f, "  #%-4d %s %5s entry=%.2f exit=%.2f pnl=$%+.2f mfe=%.1f layers=%d %s\n",
                t->id, fmt_ts(t->entry_ts_ms).c_str(),
                t->is_long ? "LONG" : "SHORT",
                t->entry, t->exit_px, t->pnl_usd, t->mfe, t->n_layers,
                t->exit_reason.c_str());
    }

    fclose(f);
}

static void write_best_trades(const char* path, const std::vector<Trade>& trades) {
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return; }
    fprintf(f, "trade_id,entry_ts,exit_ts,side,entry,exit,sl,tp,size,bars_held,pnl_pts,pnl_usd,mfe,n_layers,exit_reason\n");
    for (const Trade& t : trades) {
        fprintf(f, "%d,%s,%s,%s,%.3f,%.3f,%.3f,%.3f,%.4f,%d,%.3f,%.2f,%.3f,%d,%s\n",
                t.id, fmt_ts(t.entry_ts_ms).c_str(), fmt_ts(t.exit_ts_ms).c_str(),
                t.is_long ? "LONG" : "SHORT",
                t.entry, t.exit_px, t.sl, t.tp, t.size, t.bars_held,
                t.pnl_pts, t.pnl_usd, t.mfe, t.n_layers, t.exit_reason.c_str());
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
        fprintf(stderr, "usage: %s <tick_csv> [--no-er]\n", argv[0]);
        return 1;
    }
    const char* csv_path = argv[1];
    for (int a = 2; a < argc; ++a) {
        std::string arg = argv[a];
        if (arg == "--no-er")   g_er_enabled = false;
        if (arg == "--l2-gate") g_l2_gate    = true;
        if (arg == "--er-min"  && a + 1 < argc) CHOP_ER_MIN     = atof(argv[++a]);
        if (arg == "--er-lb"   && a + 1 < argc) CHOP_ER_LOOKBACK = atoi(argv[++a]);
        if (arg == "--l2-long" && a + 1 < argc) L2_LONG_MIN     = atof(argv[++a]);
        if (arg == "--l2-short"&& a + 1 < argc) L2_SHORT_MAX    = atof(argv[++a]);
        if (arg == "--adx-min" && a + 1 < argc) ADX_MIN         = atof(argv[++a]);
        if (arg == "--adx-min-open" && a + 1 < argc) ADX_MIN_OPEN = atof(argv[++a]);
        if (arg == "--block-dead-hours")             g_block_dead_hours = true;
        if (arg == "--htf")                     g_htf_align     = true;
        if (arg == "--range-exp" && a + 1 < argc) RANGE_EXP_MULT = atof(argv[++a]);
        if (arg == "--range-lb"  && a + 1 < argc) RANGE_EXP_LB   = atoi(argv[++a]);
        if (arg == "--time-block")              g_time_block    = true;
    }
    printf("S38a config: COST=%.2fpt CHOP_ER_MIN=%.2f ER_LB=%d ER=%s L2_GATE=%s ADX_MIN=%.1f\n",
           COST_RT_PTS, CHOP_ER_MIN, CHOP_ER_LOOKBACK,
           g_er_enabled ? "ON" : "OFF", g_l2_gate ? "ON" : "OFF", ADX_MIN);
    fflush(stdout);

    auto t0 = std::chrono::steady_clock::now();

    printf("=== PHASE 1: LOAD + BUILD M5 BARS ===\n");
    fflush(stdout);
    uint64_t ticks_ok = 0, ticks_fail = 0;
    int64_t first_ts = 0, last_ts = 0;
    std::vector<M5Bar> bars = build_m5_bars(csv_path, ticks_ok, ticks_fail, first_ts, last_ts);
    auto t1 = std::chrono::steady_clock::now();
    double load_s = std::chrono::duration<double>(t1 - t0).count();
    printf("Ticks OK: %llu  failed: %llu\n", (unsigned long long)ticks_ok,
           (unsigned long long)ticks_fail);
    printf("Date range: %s -> %s\n", fmt_ts(first_ts).c_str(), fmt_ts(last_ts).c_str());
    printf("M5 bars built: %zu\n", bars.size());
    printf("Load time: %.1fs\n\n", load_s);
    fflush(stdout);

    printf("=== PHASE 2: SWEEP (162 configs) ===\n");
    fflush(stdout);
    std::vector<Config> cfgs;
    int    lb_grid[]    = {8, 12, 16};
    double sl_grid[]    = {0.8, 1.0, 1.5};
    double tp_grid[]    = {1.5, 2.0, 3.0};
    double trail_grid[] = {0.30, 0.20, 0.12};
    bool   pyr_grid[]   = {false, true};

    for (int lb : lb_grid)
        for (double sl : sl_grid)
            for (double tp : tp_grid)
                for (double tr : trail_grid)
                    for (bool pyr : pyr_grid)
                        cfgs.push_back({lb, sl, tp, tr, pyr});

    std::vector<Result> all;
    all.reserve(cfgs.size());
    int idx = 0;
    for (const Config& c : cfgs) {
        ++idx;
        Result r = run_one(bars, c);
        all.push_back(std::move(r));
        const Result& last = all.back();
        printf("  [%3d/%zu] LB=%-2d SL=%.1f TP=%.1f Tr=%.2f Pyr=%s  n=%-5d WR=%.1f%%  PnL=$%+10.2f  DD=$-%.2f PF=%.2f Lyrs=%.1f\n",
               idx, cfgs.size(),
               last.cfg.lookback, last.cfg.sl_mult, last.cfg.tp_mult,
               last.cfg.trail_tight, last.cfg.pyramid_on ? "Y" : "N",
               last.n_trades, last.win_rate, last.total_pnl,
               last.max_dd, last.profit_factor, last.avg_layers);
        fflush(stdout);
    }
    auto t2 = std::chrono::steady_clock::now();
    double sweep_s = std::chrono::duration<double>(t2 - t1).count();

    // Select best: PF >= 1.20, n >= 100, then rank by total PnL
    const Result* best = nullptr;
    for (const Result& r : all) {
        if (r.n_trades < 100) continue;
        if (r.profit_factor < 1.20) continue;
        if (best == nullptr || r.total_pnl > best->total_pnl) best = &r;
    }
    // Fallback: PF >= 1.10, n >= 50
    if (!best) {
        for (const Result& r : all) {
            if (r.n_trades < 50) continue;
            if (r.profit_factor < 1.10) continue;
            if (best == nullptr || r.total_pnl > best->total_pnl) best = &r;
        }
    }
    // Fallback: best PnL overall
    if (!best) {
        for (const Result& r : all) {
            if (best == nullptr || r.total_pnl > best->total_pnl) best = &r;
        }
    }

    printf("\n===== BEST CONFIG =====\n");
    printf("LB=%d SL=%.1f TP=%.1f Trail=%.2f Pyr=%s\n",
           best->cfg.lookback, best->cfg.sl_mult, best->cfg.tp_mult,
           best->cfg.trail_tight, best->cfg.pyramid_on ? "Y" : "N");
    printf("n=%d  PnL=$%+.2f  WR=%.1f%%  PF=%.2f  DD=$-%.2f  Lyrs=%.1f\n",
           best->n_trades, best->total_pnl, best->win_rate,
           best->profit_factor, best->max_dd, best->avg_layers);
    printf("Sweep time: %.1fs\n\n", sweep_s);
    fflush(stdout);

    printf("=== PHASE 3: WRITE OUTPUTS ===\n");
    fflush(stdout);
    double total_s = std::chrono::duration<double>(t2 - t0).count();
    write_results      ("gold_scalp_pyramid_results.txt", all, *best,
                         ticks_ok, ticks_fail, first_ts, last_ts,
                         bars.size(), total_s);
    write_best_trades  ("gold_scalp_pyramid_best_trades.csv", best->trades);
    write_best_equity  ("gold_scalp_pyramid_best_equity.csv", best->trades);
    printf("Wrote:\n");
    printf("  gold_scalp_pyramid_results.txt       (sweep table + best config)\n");
    printf("  gold_scalp_pyramid_best_trades.csv   (best config trade log)\n");
    printf("  gold_scalp_pyramid_best_equity.csv   (best config equity curve)\n");
    printf("\nTotal runtime: %.1fs\n", total_s);

    return 0;
}

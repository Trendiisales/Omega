// =============================================================================
// minimal_sweep.cpp -- Unified minimal-baseline sweep harness, XAUUSD 2yr tick
//
// Purpose: Tier 1 of Omega Session 12 audit. Three filter-heavy shadow engines
// are suspected of suppressing edge that their core signal might produce when
// stripped to minimal baseline (same hypothesis that validated MinimalH4Breakout
// after H4RegimeEngine's 7 filters reduced it to 0 trades in 2yrs).
//
// Engines probed:
//   --engine=h1_pullback    EMA(20/50) trend + pullback-to-EMA20 touch on H1
//                           bars. Minimal: no ADX, no RSI, no ADX-rising, no
//                           EMA-separation gate, no extreme gate, no spread gate.
//                           Just trend (ema20 vs ema50) + pullback + weekend gate.
//
//   --engine=pullback_cont  Tick-based: after a MOVE_MIN move in LOOKBACK_S,
//                           wait for PB_FRAC retracement, enter on continuation.
//                           Minimal: session gate REMOVED (production is h07/h17/h23
//                           only). Sweep over all hours + weekend gate only.
//
//   --engine=rsi_turn       M1 bar RSI-extreme turn. Relax RSI bands and drop
//                           session/slot gating. Keep sustained-bars + turn-pts
//                           as the core filter, sweep them.
//
// This is a RESEARCH probe. For any engine showing baseline edge (PF >= 1.20,
// n >= 100), the full 4-phase validation protocol follows:
//   Phase 1: 2yr tick sweep  (this file)
//   Phase 2: walk-forward year1-train / year2-test (PF drop < 20% OOS)
//   Phase 3: cost stress under pessimistic spread+slip (PF > 1.20 retained)
//   Phase 4: live L2 replay on recent data
//
// Fixed cost model (Phase 1): half-spread 0.15pt static on entry; intra-bar
// SL/TP via bid_low/high and ask_low/high with chronological ordering when
// both hit same bar.
//
// Build:
//   clang++ -O3 -std=c++17 -o minimal_sweep minimal_sweep.cpp
// Run:
//   ./minimal_sweep --engine=h1_pullback /Users/jo/tick/2yr_XAUUSD_tick.csv
//   ./minimal_sweep --engine=pullback_cont /Users/jo/tick/2yr_XAUUSD_tick.csv
//   ./minimal_sweep --engine=rsi_turn /Users/jo/tick/2yr_XAUUSD_tick.csv
// Outputs (written to CWD, filename prefix = engine kind):
//   <eng>_minimal_results.txt
//   <eng>_minimal_best_trades.csv
//   <eng>_minimal_best_equity.csv
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
// Engine kind enum
// =============================================================================
enum class EngineKind {
    H1_PULLBACK,
    PULLBACK_CONT,
    RSI_TURN
};

static const char* engine_name(EngineKind k) {
    switch (k) {
        case EngineKind::H1_PULLBACK:   return "h1_pullback";
        case EngineKind::PULLBACK_CONT: return "pullback_cont";
        case EngineKind::RSI_TURN:      return "rsi_turn";
    }
    return "unknown";
}

// =============================================================================
// Tick parsing (matches h1_bt_minimal.cpp / pdhl_bt_2y.cpp format exactly)
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

// Weekend entry gate: no new entries Fri 20:00 UTC+ through Sun 22:00 UTC.
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
// Bar structure (used by H1 and M1 paths)
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

struct BarWithTicks {
    Bar bar;
    double bid_low  = 1e18;
    double bid_high = 0.0;
    double ask_low  = 1e18;
    double ask_high = 0.0;
    bool   bid_min_before_max = false;
    bool   ask_max_before_min = false;
    int64_t bid_min_ts = 0, bid_max_ts = 0;
    int64_t ask_min_ts = 0, ask_max_ts = 0;
};

// =============================================================================
// Wilder ATR (bar-based, used by H1 and M1 paths)
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
// Wilder RSI on bar closes (used by M1 path for rsi_turn)
// =============================================================================
struct RSI {
    int    period      = 14;
    bool   primed      = false;
    double avg_gain    = 0.0;
    double avg_loss    = 0.0;
    double prev_close  = 0.0;
    bool   have_prev   = false;
    double value       = 50.0;
    std::deque<double> seed_gains;
    std::deque<double> seed_losses;
    void set(int p) {
        period = p; primed = false;
        avg_gain = avg_loss = 0.0; have_prev = false; value = 50.0;
        seed_gains.clear(); seed_losses.clear();
    }
    void push(double close) {
        if (!have_prev) { prev_close = close; have_prev = true; return; }
        double change = close - prev_close;
        prev_close = close;
        double gain = change > 0.0 ? change : 0.0;
        double loss = change < 0.0 ? -change : 0.0;
        if (!primed) {
            seed_gains.push_back(gain);
            seed_losses.push_back(loss);
            if ((int)seed_gains.size() >= period) {
                double gsum = 0.0, lsum = 0.0;
                for (double v : seed_gains)  gsum += v;
                for (double v : seed_losses) lsum += v;
                avg_gain = gsum / period;
                avg_loss = lsum / period;
                primed = true;
            }
        } else {
            avg_gain = (avg_gain * (period - 1) + gain) / (double)period;
            avg_loss = (avg_loss * (period - 1) + loss) / (double)period;
        }
        if (!primed) { value = 50.0; return; }
        if (avg_loss == 0.0) { value = 100.0; return; }
        double rs = avg_gain / avg_loss;
        value = 100.0 - (100.0 / (1.0 + rs));
    }
};

// =============================================================================
// Trade record (shared across engines)
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
// Config & Result (polymorphic, driven by engine kind)
// =============================================================================
struct Config {
    EngineKind kind;
    // H1_PULLBACK params
    int    ema_fast      = 20;
    int    ema_slow      = 50;
    double pb_tol_atr    = 0.30;   // pullback zone tolerance as ATR multiple
    double sl_mult       = 1.0;    // SL in ATR
    double tp_mult       = 2.0;    // TP in ATR
    // PULLBACK_CONT params
    double move_min      = 20.0;   // pts
    double pb_frac       = 0.20;
    int    lookback_s    = 300;
    int    hold_s        = 300;
    double sl_pts        = 6.0;    // fixed pts
    int    hour_mask     = -1;     // -1 = all hours; 0..23 = one hour only (we sweep -1 first)
    // RSI_TURN params
    double rsi_low       = 20.0;
    double rsi_high      = 70.0;
    int    min_sustained = 3;
    double min_turn_pts  = 0.5;
    double rsi_sl_atr    = 0.50;
    double rsi_exit_long = 55.0;
    double rsi_exit_short= 45.0;
    int    max_hold_s    = 300;

    std::string label() const {
        char b[128];
        switch (kind) {
            case EngineKind::H1_PULLBACK:
                snprintf(b, sizeof(b), "EF=%d ES=%d pbTol=%.2f SL=%.1f TP=%.1f",
                         ema_fast, ema_slow, pb_tol_atr, sl_mult, tp_mult);
                break;
            case EngineKind::PULLBACK_CONT:
                snprintf(b, sizeof(b), "MV=%.0f PB=%.2f HOLD=%d SL=%.1f H=%d",
                         move_min, pb_frac, hold_s, sl_pts, hour_mask);
                break;
            case EngineKind::RSI_TURN:
                snprintf(b, sizeof(b), "LO=%.0f HI=%.0f SUS=%d TURN=%.1f SL=%.2f",
                         rsi_low, rsi_high, min_sustained, min_turn_pts, rsi_sl_atr);
                break;
        }
        return std::string(b);
    }
};

struct Result {
    Config cfg;
    int    n_trades   = 0;
    int    n_wins     = 0;
    double total_pnl  = 0.0;
    double avg_win    = 0.0;
    double avg_loss   = 0.0;
    double win_rate   = 0.0;
    double expectancy = 0.0;
    double max_dd     = 0.0;
    double profit_factor = 0.0;
    std::vector<Trade> trades;
};

// =============================================================================
// Bar-build helper (parametric duration): builds either H1 or M1 bars.
// =============================================================================
static std::vector<BarWithTicks> build_bars(const char* csv_path,
                                            int64_t dur_ms,
                                            uint64_t& ticks_ok_out,
                                            uint64_t& ticks_fail_out,
                                            int64_t& first_ts_out,
                                            int64_t& last_ts_out)
{
    std::vector<BarWithTicks> bars;
    FILE* f = fopen(csv_path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", csv_path); return bars; }

    int64_t cur_anchor = -1;
    BarWithTicks cur;
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
            cur = BarWithTicks{};
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
            cur.bid_min_before_max = (cur.bid_min_ts < cur.bid_max_ts);
            cur.ask_max_before_min = (cur.ask_max_ts < cur.ask_min_ts);
            bars.push_back(cur);
            cur = BarWithTicks{};
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
// Raw-tick loader (used by PULLBACK_CONT path). Streams from disk to vector.
// =============================================================================
static std::vector<Tick> load_ticks(const char* csv_path,
                                     uint64_t& ticks_ok_out,
                                     uint64_t& ticks_fail_out,
                                     int64_t& first_ts_out,
                                     int64_t& last_ts_out)
{
    std::vector<Tick> ticks;
    FILE* f = fopen(csv_path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", csv_path); return ticks; }
    ticks.reserve(120000000); // ~111M expected
    char line[256];
    uint64_t ok = 0, fail = 0;
    int64_t first_ts = 0, last_ts = 0;
    while (fgets(line, sizeof(line), f)) {
        Tick tk;
        if (!parse_line(line, tk)) { ++fail; continue; }
        ++ok;
        if (first_ts == 0) first_ts = tk.ts_ms;
        last_ts = tk.ts_ms;
        ticks.push_back(tk);
    }
    fclose(f);
    ticks_ok_out = ok; ticks_fail_out = fail;
    first_ts_out = first_ts; last_ts_out = last_ts;
    return ticks;
}

// =============================================================================
// Sizing helper (uniform $10 risk, 0.01 lot cap, 100 USD / pt / lot for gold)
// =============================================================================
static double compute_size(double sl_pts) {
    const double risk_dollars = 10.0;
    const double max_lot = 0.01;
    if (sl_pts <= 0.0) return 0.01;
    double size = risk_dollars / (sl_pts * 100.0);
    size = std::floor(size / 0.001) * 0.001;
    size = std::max(0.01, std::min(max_lot, size));
    return size;
}

// =============================================================================
// Stats computation (shared across engines)
// =============================================================================
static void compute_stats(Result& r) {
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
}

// =============================================================================
// ENGINE 1: H1_PULLBACK
//
// Logic (minimal):
//   - Build H1 bars + H1 ATR(14)
//   - Maintain EMA_fast and EMA_slow of bar closes
//   - Trend direction: EMA_fast > EMA_slow -> bull; EMA_fast < EMA_slow -> bear
//   - Pullback zone: price within pb_tol_atr * ATR of EMA_fast
//   - Entry: bar closes inside pullback zone AND bar's low touched EMA_fast
//     (bull trend -> LONG only) or bar's high touched EMA_fast (bear -> SHORT)
//   - SL = sl_mult * ATR behind entry; TP = tp_mult * ATR ahead
//   - Weekend gate
//   - No ADX, no RSI, no ADX-rising, no EMA-separation gate, no extreme gate.
// =============================================================================
static Result run_h1_pullback(const std::vector<BarWithTicks>& bars,
                              const Config& cfg)
{
    Result r;
    r.cfg = cfg;
    r.trades.reserve(256);

    ATR atr; atr.set(14);
    double ema_f = 0.0, ema_s = 0.0;
    bool   ema_primed = false;
    int    ema_seed_n = 0;
    const double af = 2.0 / (cfg.ema_fast + 1.0);
    const double as = 2.0 / (cfg.ema_slow + 1.0);

    struct Pos {
        bool    active   = false;
        bool    is_long  = false;
        double  entry    = 0.0;
        double  sl       = 0.0;
        double  tp       = 0.0;
        double  size     = 0.0;
        int64_t entry_ts = 0;
        int     bars_held = 0;
    } pos;

    int trade_id = 0;

    for (size_t i = 0; i < bars.size(); ++i) {
        const BarWithTicks& bt = bars[i];
        const Bar& b = bt.bar;

        atr.push(b.high, b.low, b.close);

        // EMA update (seed with simple avg for first N bars, then exponential)
        if (!ema_primed) {
            ema_f += b.close;
            ema_s += b.close;
            ++ema_seed_n;
            if (ema_seed_n >= cfg.ema_slow) {
                ema_f /= cfg.ema_slow;
                ema_s /= cfg.ema_slow;
                ema_primed = true;
            }
        } else {
            ema_f = af * b.close + (1.0 - af) * ema_f;
            ema_s = as * b.close + (1.0 - as) * ema_s;
        }

        // ---- EXIT check ----
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
                    : (pos.entry - exit_px));
                double pnl_usd = pnl_pts * pos.size * 100.0;
                Trade t;
                t.id = ++trade_id;
                t.entry_ts_ms = pos.entry_ts;
                t.exit_ts_ms  = bt.bar.ts_ms_close;
                t.is_long = pos.is_long;
                t.entry = pos.entry; t.exit = exit_px;
                t.sl = pos.sl; t.tp = pos.tp;
                t.size = pos.size;
                t.bars_held = pos.bars_held;
                t.pnl_pts = pnl_pts;
                t.pnl_usd = pnl_usd;
                t.exit_reason = sl_hit ? "SL_HIT" : "TP_HIT";
                r.trades.push_back(t);
                pos = Pos{};
            }
        }

        // ---- ENTRY check ----
        if (pos.active) continue;
        if (!atr.primed) continue;
        if (!ema_primed) continue;
        if (is_weekend_gated(bt.bar.ts_ms_open)) continue;

        const bool bull = ema_f > ema_s;
        const bool bear = ema_f < ema_s;
        if (!bull && !bear) continue;

        const double tol = cfg.pb_tol_atr * atr.value;

        // Pullback-touch condition.
        //   Bull: bar low dipped at/below ema_fast (touched support), and close > ema_fast.
        //   Bear: bar high rose at/above ema_fast (touched resistance), and close < ema_fast.
        // tol allows near-touches where price came within tol of ema_fast.
        bool touch_long  = bull &&
                           (b.low  <= ema_f + tol) &&
                           (b.close > ema_f) &&
                           (b.close > ema_s);
        bool touch_short = bear &&
                           (b.high >= ema_f - tol) &&
                           (b.close < ema_f) &&
                           (b.close < ema_s);

        if (!touch_long && !touch_short) continue;

        const bool intend_long = touch_long;
        const double sl_pts = atr.value * cfg.sl_mult;
        const double tp_pts = atr.value * cfg.tp_mult;
        const double half_spread = 0.15;
        const double entry_px = intend_long ? (b.close + half_spread) : (b.close - half_spread);
        const double sl_px    = intend_long ? (entry_px - sl_pts) : (entry_px + sl_pts);
        const double tp_px    = intend_long ? (entry_px + tp_pts) : (entry_px - tp_pts);
        const double size     = compute_size(sl_pts);

        pos.active   = true;
        pos.is_long  = intend_long;
        pos.entry    = entry_px;
        pos.sl       = sl_px;
        pos.tp       = tp_px;
        pos.size     = size;
        pos.entry_ts = bt.bar.ts_ms_close;
        pos.bars_held = 0;
    }

    compute_stats(r);
    return r;
}

// =============================================================================
// ENGINE 2: PULLBACK_CONT
//
// Logic (ported from include/PullbackContEngine.hpp but WITHOUT session gate).
//   - State machine: IDLE -> WATCHING -> IDLE
//   - IDLE: if |mid - mid[t-LOOKBACK_S]| >= MOVE_MIN, enter WATCHING
//   - WATCHING: wait for pullback of PB_FRAC * move; enter on touch
//   - Timeout after HOLD_S
//   - Counter-reversal abort: if price goes >50% against original move
//   - Fixed SL_PTS stop, no TP (managed by trail/timeout/counter-reversal)
//   - For the minimal sweep we use a simple fixed SL_PTS + MAX_HOLD_S exit
//     (the production engine's trail logic is filter-like; keep it minimal).
//     Exit: SL hit OR max-hold exceeded OR counter-reversal (reuse).
//   - Weekend gate
//   - hour_mask: -1 = all hours, 0..23 = that hour only. We sweep all-hours
//     first; if edge exists per-hour, user can add a follow-up sweep.
// =============================================================================
static Result run_pullback_cont(const std::vector<Tick>& ticks,
                                const Config& cfg)
{
    Result r;
    r.cfg = cfg;
    r.trades.reserve(2048);

    enum State { IDLE, WATCHING };
    State   st = IDLE;
    bool    long_dir = false;
    double  signal_mid = 0.0;
    double  move_pts   = 0.0;
    double  wanted_mid = 0.0;
    int64_t signal_ms  = 0;

    struct Pos {
        bool    active   = false;
        bool    is_long  = false;
        double  entry    = 0.0;
        double  sl       = 0.0;
        double  size     = 0.0;
        int64_t entry_ms = 0;
        int64_t expire_ms= 0;
    } pos;
    int trade_id = 0;

    // Rolling mid buffer for lookback
    std::deque<std::pair<int64_t,double>> m_buf;
    const int64_t lb_ms = (int64_t)cfg.lookback_s * 1000LL;

    for (size_t i = 0; i < ticks.size(); ++i) {
        const Tick& tk = ticks[i];
        const double mid = (tk.bid + tk.ask) * 0.5;
        const int64_t now = tk.ts_ms;

        // ---- Manage open position first ----
        if (pos.active) {
            bool exit = false;
            const char* reason = "";
            double exit_px = 0.0;
            double pnl_pts = 0.0;

            if (pos.is_long) {
                if (tk.bid <= pos.sl) {
                    exit = true; reason = "SL_HIT";
                    exit_px = pos.sl;
                    pnl_pts = exit_px - pos.entry;
                }
            } else {
                if (tk.ask >= pos.sl) {
                    exit = true; reason = "SL_HIT";
                    exit_px = pos.sl;
                    pnl_pts = pos.entry - exit_px;
                }
            }
            if (!exit && now >= pos.expire_ms) {
                exit = true; reason = "TIMEOUT";
                exit_px = pos.is_long ? tk.bid : tk.ask;
                pnl_pts = pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px);
            }
            if (exit) {
                double pnl_usd = pnl_pts * pos.size * 100.0;
                Trade t;
                t.id = ++trade_id;
                t.entry_ts_ms = pos.entry_ms;
                t.exit_ts_ms  = now;
                t.is_long = pos.is_long;
                t.entry = pos.entry; t.exit = exit_px;
                t.sl = pos.sl; t.tp = 0.0;
                t.size = pos.size;
                t.bars_held = 0;
                t.pnl_pts = pnl_pts;
                t.pnl_usd = pnl_usd;
                t.exit_reason = reason;
                r.trades.push_back(t);
                pos = Pos{};
            }
            // While in a trade, skip new signal eval
            continue;
        }

        // Maintain mid buffer
        m_buf.push_back({now, mid});
        while (!m_buf.empty() && now - m_buf.front().first > lb_ms + 5000LL)
            m_buf.pop_front();
        if (m_buf.size() < 10) continue;

        if (is_weekend_gated(now)) { st = IDLE; continue; }

        // Hour mask
        if (cfg.hour_mask >= 0) {
            int hour = (int)((now / 1000LL) % 86400LL / 3600LL);
            if (hour != cfg.hour_mask) { st = IDLE; continue; }
        }

        const double mid_back = m_buf.front().second;
        const double move     = mid - mid_back;

        if (st == IDLE) {
            if (std::fabs(move) >= cfg.move_min) {
                st          = WATCHING;
                long_dir    = (move > 0.0);
                signal_mid  = mid;
                move_pts    = std::fabs(move);
                signal_ms   = now;
                double pb_pts = move_pts * cfg.pb_frac;
                wanted_mid  = long_dir ? (signal_mid - pb_pts)
                                       : (signal_mid + pb_pts);
            }
            continue;
        }

        // WATCHING
        if (now - signal_ms > (int64_t)cfg.hold_s * 1000LL) {
            st = IDLE;
            continue;
        }
        const double vs_signal = long_dir ? (mid - signal_mid) : (signal_mid - mid);
        if (vs_signal < -(move_pts * 0.50)) {
            st = IDLE;
            continue;
        }
        const bool pb_hit = long_dir ? (mid <= wanted_mid) : (mid >= wanted_mid);
        if (pb_hit) {
            // Enter
            const double entry_px = long_dir ? tk.ask : tk.bid;
            const double sl_px    = long_dir ? (entry_px - cfg.sl_pts)
                                             : (entry_px + cfg.sl_pts);
            const double size     = compute_size(cfg.sl_pts);

            pos.active    = true;
            pos.is_long   = long_dir;
            pos.entry     = entry_px;
            pos.sl        = sl_px;
            pos.size      = size;
            pos.entry_ms  = now;
            pos.expire_ms = now + (int64_t)cfg.hold_s * 1000LL;
            st = IDLE;
        }
    }

    compute_stats(r);
    return r;
}

// =============================================================================
// ENGINE 3: RSI_TURN
//
// Logic (minimal port of RSIExtremeTurnEngine):
//   - Build M1 bars + M1 ATR(14) + M1 RSI(14)
//   - Track sustained bars in extreme zone
//   - Turn detection: RSI reverses by >= MIN_TURN_PTS after being sustained
//   - Entry on turn, SL = rsi_sl_atr * ATR, exit when RSI crosses back
//     through rsi_exit_long (55) / rsi_exit_short (45) or MAX_HOLD_S or SL
//   - No session gate, no DOM, no cost-gate (pure signal test)
// =============================================================================
static Result run_rsi_turn(const std::vector<BarWithTicks>& bars,
                           const Config& cfg)
{
    Result r;
    r.cfg = cfg;
    r.trades.reserve(2048);

    ATR atr; atr.set(14);
    RSI rsi; rsi.set(14);

    double rsi_prev = 50.0;
    bool   have_rsi_prev = false;
    int    sustained_os = 0;  // consecutive bars with RSI < rsi_low
    int    sustained_ob = 0;  // consecutive bars with RSI > rsi_high

    struct Pos {
        bool    active    = false;
        bool    is_long   = false;
        double  entry     = 0.0;
        double  sl        = 0.0;
        double  size      = 0.0;
        int64_t entry_ts  = 0;
        int     bars_held = 0;
    } pos;
    int trade_id = 0;

    for (size_t i = 0; i < bars.size(); ++i) {
        const BarWithTicks& bt = bars[i];
        const Bar& b = bt.bar;

        atr.push(b.high, b.low, b.close);
        rsi.push(b.close);

        const double rsi_now = rsi.value;

        // ---- Exit check for open position ----
        if (pos.active) {
            pos.bars_held++;
            bool sl_hit = false;
            bool rsi_exit = false;
            bool timeout = false;
            double sl_px_q = pos.sl;

            // SL intrabar
            if (pos.is_long) {
                if (bt.bid_low  <= sl_px_q) sl_hit = true;
            } else {
                if (bt.ask_high >= sl_px_q) sl_hit = true;
            }

            // RSI exit (evaluated on bar close only)
            if (!sl_hit && rsi.primed) {
                if (pos.is_long  && rsi_now >= cfg.rsi_exit_long)  rsi_exit = true;
                if (!pos.is_long && rsi_now <= cfg.rsi_exit_short) rsi_exit = true;
            }

            // Max-hold: each M1 bar = 60s, so max_hold bars = max_hold_s / 60
            const int max_bars = std::max(1, cfg.max_hold_s / 60);
            if (!sl_hit && !rsi_exit && pos.bars_held >= max_bars) timeout = true;

            if (sl_hit || rsi_exit || timeout) {
                double exit_px;
                const char* reason;
                if (sl_hit) {
                    exit_px = sl_px_q;
                    reason  = "SL_HIT";
                } else if (rsi_exit) {
                    // Close at bar close price (sell side for long, buy side for short).
                    // Approximate with mid + half_spread cost.
                    const double half_spread = 0.15;
                    exit_px = pos.is_long ? (b.close - half_spread)
                                          : (b.close + half_spread);
                    reason = "RSI_EXIT";
                } else {
                    const double half_spread = 0.15;
                    exit_px = pos.is_long ? (b.close - half_spread)
                                          : (b.close + half_spread);
                    reason = "TIMEOUT";
                }
                double pnl_pts = pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px);
                double pnl_usd = pnl_pts * pos.size * 100.0;
                Trade t;
                t.id = ++trade_id;
                t.entry_ts_ms = pos.entry_ts;
                t.exit_ts_ms  = bt.bar.ts_ms_close;
                t.is_long = pos.is_long;
                t.entry = pos.entry; t.exit = exit_px;
                t.sl = pos.sl; t.tp = 0.0;
                t.size = pos.size;
                t.bars_held = pos.bars_held;
                t.pnl_pts = pnl_pts;
                t.pnl_usd = pnl_usd;
                t.exit_reason = reason;
                r.trades.push_back(t);
                pos = Pos{};
            }
        }

        // ---- Sustained counters (update regardless of position) ----
        if (rsi.primed) {
            if (rsi_now < cfg.rsi_low)  ++sustained_os; else sustained_os = 0;
            if (rsi_now > cfg.rsi_high) ++sustained_ob; else sustained_ob = 0;
        }

        // ---- Entry ----
        if (!pos.active && rsi.primed && atr.primed && have_rsi_prev) {
            if (is_weekend_gated(bt.bar.ts_ms_open)) {
                rsi_prev = rsi_now;
                continue;
            }
            // LONG: was oversold for >=MIN_SUSTAINED_BARS then RSI turned up
            const bool long_turn = (sustained_os >= cfg.min_sustained) &&
                                   (rsi_now - rsi_prev >= cfg.min_turn_pts);
            // SHORT: was overbought then turned down
            const bool short_turn = (sustained_ob >= cfg.min_sustained) &&
                                    (rsi_prev - rsi_now >= cfg.min_turn_pts);

            if (long_turn || short_turn) {
                const bool intend_long = long_turn;
                const double sl_pts   = atr.value * cfg.rsi_sl_atr;
                const double half_spread = 0.15;
                const double entry_px = intend_long ? (b.close + half_spread)
                                                    : (b.close - half_spread);
                const double sl_px    = intend_long ? (entry_px - sl_pts)
                                                    : (entry_px + sl_pts);
                const double size     = compute_size(sl_pts);

                pos.active    = true;
                pos.is_long   = intend_long;
                pos.entry     = entry_px;
                pos.sl        = sl_px;
                pos.size      = size;
                pos.entry_ts  = bt.bar.ts_ms_close;
                pos.bars_held = 0;

                // Reset sustained counters after entry to avoid immediate re-trigger
                sustained_os = 0;
                sustained_ob = 0;
            }
        }

        rsi_prev = rsi_now;
        have_rsi_prev = true;
    }

    compute_stats(r);
    return r;
}

// =============================================================================
// Sweep grid builders (engine-specific)
// =============================================================================
static std::vector<Config> build_grid_h1_pullback() {
    std::vector<Config> cfgs;
    int    ef_grid[]       = {10, 20};
    int    es_grid[]       = {50, 100};
    double pb_tol_grid[]   = {0.20, 0.40};
    double sl_grid[]       = {1.0, 1.5, 2.0};
    double tp_grid[]       = {2.0, 3.0, 4.0};
    for (int ef : ef_grid)
        for (int es : es_grid) {
            if (ef >= es) continue;
            for (double pbt : pb_tol_grid)
                for (double sl : sl_grid)
                    for (double tp : tp_grid) {
                        Config c;
                        c.kind = EngineKind::H1_PULLBACK;
                        c.ema_fast = ef; c.ema_slow = es;
                        c.pb_tol_atr = pbt; c.sl_mult = sl; c.tp_mult = tp;
                        cfgs.push_back(c);
                    }
        }
    return cfgs; // 2 * 2 * 2 * 3 * 3 - (duplicates where ef>=es) = 4*2*3*3 = 72 minus pruning
}

static std::vector<Config> build_grid_pullback_cont() {
    std::vector<Config> cfgs;
    double mv_grid[]   = {15.0, 20.0, 30.0};
    double pb_grid[]   = {0.20, 0.33, 0.50};
    int    hold_grid[] = {300, 600};
    double sl_grid[]   = {4.0, 6.0, 8.0};
    // Hour mask: -1 (all hours) first. Per-hour sweep is optional follow-up.
    for (double mv : mv_grid)
        for (double pb : pb_grid)
            for (int h : hold_grid)
                for (double sl : sl_grid) {
                    Config c;
                    c.kind = EngineKind::PULLBACK_CONT;
                    c.move_min = mv; c.pb_frac = pb;
                    c.hold_s = h; c.sl_pts = sl;
                    c.hour_mask = -1;  // ALL HOURS
                    c.lookback_s = 300;
                    cfgs.push_back(c);
                }
    return cfgs; // 3*3*2*3 = 54 configs
}

static std::vector<Config> build_grid_rsi_turn() {
    std::vector<Config> cfgs;
    double lo_grid[]  = {20.0, 25.0, 30.0};
    double hi_grid[]  = {70.0, 75.0, 80.0};
    int    sus_grid[] = {2, 3, 4};
    double turn_grid[]= {0.3, 0.5, 1.0};
    double sl_grid[]  = {0.5, 1.0};
    for (double lo : lo_grid)
        for (double hi : hi_grid)
            for (int s : sus_grid)
                for (double t : turn_grid)
                    for (double sl : sl_grid) {
                        Config c;
                        c.kind = EngineKind::RSI_TURN;
                        c.rsi_low = lo; c.rsi_high = hi;
                        c.min_sustained = s; c.min_turn_pts = t;
                        c.rsi_sl_atr = sl;
                        c.rsi_exit_long = 55.0;
                        c.rsi_exit_short= 45.0;
                        c.max_hold_s = 300;
                        cfgs.push_back(c);
                    }
    return cfgs; // 3*3*3*3*2 = 162 configs
}

// =============================================================================
// Output writers
// =============================================================================
static void write_results(const std::string& path,
                          EngineKind kind,
                          const std::vector<Result>& all,
                          const Result& best,
                          uint64_t ticks_ok, uint64_t ticks_fail,
                          int64_t first_ts, int64_t last_ts,
                          size_t n_bars_or_ticks, double runtime_s)
{
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return; }

    fprintf(f, "========================================================================\n");
    fprintf(f, "  minimal_sweep -- engine=%s, XAUUSD 2yr tick\n", engine_name(kind));
    fprintf(f, "========================================================================\n\n");
    fprintf(f, "Ticks OK:        %llu\n", (unsigned long long)ticks_ok);
    fprintf(f, "Ticks failed:    %llu\n", (unsigned long long)ticks_fail);
    fprintf(f, "Date range:      %s -> %s\n", fmt_ts(first_ts).c_str(), fmt_ts(last_ts).c_str());
    if (kind == EngineKind::PULLBACK_CONT)
        fprintf(f, "Ticks loaded:    %zu\n", n_bars_or_ticks);
    else
        fprintf(f, "Bars built:      %zu\n", n_bars_or_ticks);
    fprintf(f, "Runtime:         %.1fs\n", runtime_s);
    fprintf(f, "Configs swept:   %zu\n\n", all.size());

    fprintf(f, "========================================================================\n");
    fprintf(f, "  FULL SWEEP TABLE\n");
    fprintf(f, "========================================================================\n");
    fprintf(f, "%-50s | %-5s %-6s %-10s %-10s %-10s %-10s %-6s\n",
            "config", "n", "WR%", "PnL$", "AvgWin$", "AvgLoss$", "MaxDD$", "PF");
    fprintf(f, "------------------------------------------------------------------------\n");
    for (const Result& rr : all) {
        fprintf(f, "%-50s | %-5d %-6.1f %+10.2f %+10.2f %+10.2f %-10.2f %-6.2f\n",
                rr.cfg.label().c_str(),
                rr.n_trades, rr.win_rate, rr.total_pnl,
                rr.avg_win, -rr.avg_loss, rr.max_dd, rr.profit_factor);
    }

    fprintf(f, "\n========================================================================\n");
    fprintf(f, "  BEST CONFIG: %s\n", best.cfg.label().c_str());
    fprintf(f, "========================================================================\n\n");
    fprintf(f, "Trades:          %d\n", best.n_trades);
    fprintf(f, "Wins:            %d (%.1f%%)\n", best.n_wins, best.win_rate);
    fprintf(f, "Total PnL:       $%+.2f\n", best.total_pnl);
    fprintf(f, "Avg win:         $%+.2f\n", best.avg_win);
    fprintf(f, "Avg loss:        $-%.2f\n", best.avg_loss);
    fprintf(f, "Expectancy:      $%+.2f/trade\n", best.expectancy);
    fprintf(f, "Max DD:          $-%.2f\n", best.max_dd);
    fprintf(f, "Profit factor:   %.2f\n", best.profit_factor);

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
        fprintf(f, "  %04d-%02d: n=%-3d pnl=$%+.2f cum=$%+.2f\n",
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
        fprintf(f, "  %-16s n=%-4d pnl=$%+.2f\n", x.reason.c_str(), x.n, x.pnl);
    }

    fclose(f);
}

static void write_best_trades(const std::string& path, const std::vector<Trade>& trades) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return; }
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

static void write_best_equity(const std::string& path, const std::vector<Trade>& trades) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return; }
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
static void usage(const char* prog) {
    fprintf(stderr,
        "usage: %s --engine=<kind> <tick_csv>\n"
        "  <kind> = h1_pullback | pullback_cont | rsi_turn\n",
        prog);
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    const char* engine_arg = nullptr;
    const char* csv_path   = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--engine=", 9) == 0) {
            engine_arg = argv[i] + 9;
        } else {
            csv_path = argv[i];
        }
    }
    if (!engine_arg || !csv_path) { usage(argv[0]); return 1; }

    EngineKind kind;
    if      (strcmp(engine_arg, "h1_pullback")   == 0) kind = EngineKind::H1_PULLBACK;
    else if (strcmp(engine_arg, "pullback_cont") == 0) kind = EngineKind::PULLBACK_CONT;
    else if (strcmp(engine_arg, "rsi_turn")      == 0) kind = EngineKind::RSI_TURN;
    else { fprintf(stderr, "unknown engine: %s\n", engine_arg); usage(argv[0]); return 1; }

    auto t0 = std::chrono::steady_clock::now();

    uint64_t ticks_ok = 0, ticks_fail = 0;
    int64_t first_ts = 0, last_ts = 0;
    size_t  n_items  = 0;

    std::vector<BarWithTicks> bars;
    std::vector<Tick>         ticks;

    if (kind == EngineKind::H1_PULLBACK) {
        printf("=== PHASE 1: LOAD + BUILD H1 BARS ===\n"); fflush(stdout);
        const int64_t H1_MS = 60LL * 60LL * 1000LL;
        bars = build_bars(csv_path, H1_MS, ticks_ok, ticks_fail, first_ts, last_ts);
        n_items = bars.size();
    } else if (kind == EngineKind::PULLBACK_CONT) {
        printf("=== PHASE 1: LOAD TICKS ===\n"); fflush(stdout);
        ticks = load_ticks(csv_path, ticks_ok, ticks_fail, first_ts, last_ts);
        n_items = ticks.size();
    } else {
        printf("=== PHASE 1: LOAD + BUILD M1 BARS ===\n"); fflush(stdout);
        const int64_t M1_MS = 60LL * 1000LL;
        bars = build_bars(csv_path, M1_MS, ticks_ok, ticks_fail, first_ts, last_ts);
        n_items = bars.size();
    }

    auto t1 = std::chrono::steady_clock::now();
    double load_s = std::chrono::duration<double>(t1 - t0).count();
    printf("Ticks OK: %llu  failed: %llu\n",
           (unsigned long long)ticks_ok, (unsigned long long)ticks_fail);
    printf("Date range: %s -> %s\n", fmt_ts(first_ts).c_str(), fmt_ts(last_ts).c_str());
    if (kind == EngineKind::PULLBACK_CONT)
        printf("Ticks loaded: %zu\n", n_items);
    else
        printf("Bars built: %zu\n", n_items);
    printf("Load time: %.1fs\n\n", load_s);
    fflush(stdout);

    std::vector<Config> cfgs;
    switch (kind) {
        case EngineKind::H1_PULLBACK:   cfgs = build_grid_h1_pullback();   break;
        case EngineKind::PULLBACK_CONT: cfgs = build_grid_pullback_cont(); break;
        case EngineKind::RSI_TURN:      cfgs = build_grid_rsi_turn();      break;
    }

    printf("=== PHASE 2: SWEEP (%zu configs) ===\n", cfgs.size());
    fflush(stdout);

    std::vector<Result> all;
    all.reserve(cfgs.size());
    int idx = 0;
    for (const Config& c : cfgs) {
        ++idx;
        Result rr;
        switch (kind) {
            case EngineKind::H1_PULLBACK:   rr = run_h1_pullback(bars, c);   break;
            case EngineKind::PULLBACK_CONT: rr = run_pullback_cont(ticks, c); break;
            case EngineKind::RSI_TURN:      rr = run_rsi_turn(bars, c);      break;
        }
        all.push_back(std::move(rr));
        printf("  [%3d/%zu] %-50s  n=%-5d WR=%.1f%%  PnL=$%+.2f  DD=$-%.2f PF=%.2f\n",
               idx, cfgs.size(),
               all.back().cfg.label().c_str(),
               all.back().n_trades,
               all.back().win_rate,
               all.back().total_pnl,
               all.back().max_dd,
               all.back().profit_factor);
        fflush(stdout);
    }
    auto t2 = std::chrono::steady_clock::now();
    double sweep_s = std::chrono::duration<double>(t2 - t1).count();

    // Best config: by total PnL, requiring n >= MIN_TRADES for statistical relevance.
    //   H1_PULLBACK: H1 is lower frequency, MIN=30
    //   PULLBACK_CONT: tick engine, MIN=100
    //   RSI_TURN: M1 engine, MIN=100
    const int min_trades = (kind == EngineKind::H1_PULLBACK) ? 30 : 100;

    const Result* best = nullptr;
    for (const Result& rr : all) {
        if (rr.n_trades < min_trades) continue;
        if (best == nullptr || rr.total_pnl > best->total_pnl) best = &rr;
    }
    if (best == nullptr) {
        for (const Result& rr : all) {
            if (best == nullptr || rr.total_pnl > best->total_pnl) best = &rr;
        }
    }

    printf("\nBest config: %s  n=%d  PnL=$%+.2f  WR=%.1f%%  PF=%.2f\n",
           best->cfg.label().c_str(),
           best->n_trades, best->total_pnl, best->win_rate, best->profit_factor);
    printf("Sweep time: %.1fs\n\n", sweep_s);
    fflush(stdout);

    printf("=== PHASE 3: WRITE OUTPUTS ===\n"); fflush(stdout);
    double total_s = std::chrono::duration<double>(t2 - t0).count();

    const std::string prefix = std::string(engine_name(kind)) + "_minimal";
    write_results(prefix + "_results.txt",
                  kind, all, *best,
                  ticks_ok, ticks_fail, first_ts, last_ts,
                  n_items, total_s);
    write_best_trades(prefix + "_best_trades.csv", best->trades);
    write_best_equity(prefix + "_best_equity.csv", best->trades);
    printf("Wrote:\n");
    printf("  %s_results.txt\n", prefix.c_str());
    printf("  %s_best_trades.csv\n", prefix.c_str());
    printf("  %s_best_equity.csv\n", prefix.c_str());
    printf("\nTotal runtime: %.1fs\n", total_s);

    return 0;
}

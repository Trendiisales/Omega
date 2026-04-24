// =============================================================================
// htf_bt.cpp -- HTFSwingEngines 2-year XAUUSD tick backtest
//
// Runs the EXACT live engine logic from include/HTFSwingEngines.hpp
// (H1SwingEngine + H4RegimeEngine) against
//   /Users/jo/tick/2yr_XAUUSD_tick.csv (111M tick rows, 2 years).
//
// Engine logic ported VERBATIM from HTFSwingEngines.hpp @ HEAD b0d1e321
// (gold defaults, shadow_mode=false for measurement, no live trading).
//
// Tick format: YYYYMMDD,HH:MM:SS,bid,ask,last,volume
//
// Builds M15 / H1 / H4 bars from ticks. Computes EMA9/21/50, ATR(14),
// RSI(14), ADX(14), Donchian(20) on each bar timeframe. Drives both
// engines on bar close; runs tick-level SL/TP/trail management every tick.
//
// Build:
//   clang++ -O3 -std=c++17 -o htf_bt htf_bt.cpp
// Run:
//   ./htf_bt /Users/jo/tick/2yr_XAUUSD_tick.csv
// Output files:
//   htf_bt_h1.txt              -- H1SwingEngine summary + stats
//   htf_bt_h4.txt              -- H4RegimeEngine summary + stats
//   htf_bt_h1_trades.csv       -- H1 trade log
//   htf_bt_h4_trades.csv       -- H4 trade log
//   htf_bt_h1_equity.csv       -- H1 equity curve (date, cumulative PnL)
//   htf_bt_h4_equity.csv       -- H4 equity curve
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
    int     year;
    int     month;
    int     day;
    int     utc_hour;
    int     utc_min;
    int     utc_sec;
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

    t.ts_ms    = sec * 1000LL;
    t.bid      = bid;
    t.ask      = ask;
    t.year     = year;
    t.month    = month;
    t.day      = day;
    t.utc_hour = H;
    t.utc_min  = M;
    t.utc_sec  = S;
    return true;
}

// =============================================================================
// Bar + indicators
// =============================================================================
struct Bar {
    int64_t ts_ms_open  = 0;   // bar open ts
    int64_t ts_ms_close = 0;   // bar close ts (open + duration)
    double  open   = 0.0;
    double  high   = 0.0;
    double  low    = 0.0;
    double  close  = 0.0;
    int     n      = 0;
};

// Exponential moving average (simple recursive form, alpha = 2/(N+1))
struct EMA {
    double value   = 0.0;
    bool   primed  = false;
    int    period  = 0;
    double alpha   = 0.0;
    void set(int p) { period = p; alpha = 2.0 / (double)(p + 1); value = 0.0; primed = false; }
    void push(double x) {
        if (!primed) { value = x; primed = true; }
        else         { value = alpha * x + (1.0 - alpha) * value; }
    }
};

// Wilder's ATR (true range smoothed Wilder-style, period 14)
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

// RSI(14) Wilder
struct RSI {
    double value = 50.0;
    bool   primed = false;
    int    period = 14;
    double prev_close = 0.0;
    bool   have_prev  = false;
    std::deque<double> gains;
    std::deque<double> losses;
    double avg_gain = 0.0;
    double avg_loss = 0.0;
    void set(int p) {
        period = p; value = 50.0; primed = false; have_prev = false;
        gains.clear(); losses.clear(); avg_gain = 0.0; avg_loss = 0.0;
    }
    void push(double close) {
        if (!have_prev) { prev_close = close; have_prev = true; return; }
        double change = close - prev_close;
        double g = change > 0.0 ? change  : 0.0;
        double l = change < 0.0 ? -change : 0.0;
        prev_close = close;
        if (!primed) {
            gains.push_back(g); losses.push_back(l);
            if ((int)gains.size() >= period) {
                double sg = 0.0, sl = 0.0;
                for (double v : gains)  sg += v;
                for (double v : losses) sl += v;
                avg_gain = sg / period;
                avg_loss = sl / period;
                primed = true;
                double rs = (avg_loss == 0.0) ? 1e9 : (avg_gain / avg_loss);
                value = 100.0 - (100.0 / (1.0 + rs));
            }
        } else {
            avg_gain = (avg_gain * (period - 1) + g) / (double)period;
            avg_loss = (avg_loss * (period - 1) + l) / (double)period;
            double rs = (avg_loss == 0.0) ? 1e9 : (avg_gain / avg_loss);
            value = 100.0 - (100.0 / (1.0 + rs));
        }
    }
};

// ADX(14) Wilder
struct ADX {
    double value = 0.0;
    bool   primed = false;
    int    period = 14;
    double prev_high = 0.0, prev_low = 0.0, prev_close = 0.0;
    bool   have_prev = false;
    int    seed_count = 0;
    double tr_sum = 0.0, pdm_sum = 0.0, ndm_sum = 0.0;
    double atr_smooth = 0.0, pdm_smooth = 0.0, ndm_smooth = 0.0;
    double dx_sum = 0.0;
    int    dx_count = 0;
    double prev_value = 0.0;
    void set(int p) {
        period = p; value = 0.0; primed = false; have_prev = false;
        seed_count = 0; tr_sum = pdm_sum = ndm_sum = 0.0;
        atr_smooth = pdm_smooth = ndm_smooth = 0.0;
        dx_sum = 0.0; dx_count = 0; prev_value = 0.0;
    }
    void push(double high, double low, double close) {
        if (!have_prev) {
            prev_high = high; prev_low = low; prev_close = close;
            have_prev = true; return;
        }
        double up    = high - prev_high;
        double dn    = prev_low - low;
        double pdm   = (up > dn && up > 0.0) ? up : 0.0;
        double ndm   = (dn > up && dn > 0.0) ? dn : 0.0;
        double a = high - low;
        double b = std::fabs(high - prev_close);
        double c = std::fabs(low  - prev_close);
        double tr = std::max(a, std::max(b, c));
        prev_high = high; prev_low = low; prev_close = close;

        if (seed_count < period) {
            tr_sum += tr; pdm_sum += pdm; ndm_sum += ndm;
            ++seed_count;
            if (seed_count == period) {
                atr_smooth = tr_sum;
                pdm_smooth = pdm_sum;
                ndm_smooth = ndm_sum;
            }
            return;
        }
        atr_smooth = atr_smooth - (atr_smooth / period) + tr;
        pdm_smooth = pdm_smooth - (pdm_smooth / period) + pdm;
        ndm_smooth = ndm_smooth - (ndm_smooth / period) + ndm;
        double pdi = (atr_smooth == 0.0) ? 0.0 : 100.0 * (pdm_smooth / atr_smooth);
        double ndi = (atr_smooth == 0.0) ? 0.0 : 100.0 * (ndm_smooth / atr_smooth);
        double sum = pdi + ndi;
        double dx  = (sum == 0.0) ? 0.0 : 100.0 * std::fabs(pdi - ndi) / sum;

        if (!primed) {
            dx_sum += dx; ++dx_count;
            if (dx_count >= period) {
                value = dx_sum / period;
                prev_value = value;
                primed = true;
            }
        } else {
            prev_value = value;
            value = (value * (period - 1) + dx) / (double)period;
        }
    }
};

// =============================================================================
// Engine logic ported VERBATIM from HTFSwingEngines.hpp (gold defaults)
// =============================================================================
struct H1Params {
    double risk_dollars         = 15.0;
    double max_lot              = 0.01;
    double sl_mult              = 1.0;
    double tp1_mult             = 2.0;
    double tp2_trail_arm_mult   = 1.5;
    double tp2_trail_dist_mult  = 0.7;
    double adx_min              = 27.0;
    double h4_adx_gate          = 20.0;
    double ema_min_sep          = 5.0;
    double pullback_atr_mult    = 0.30;
    double max_spread           = 1.0;
    double rsi_ob               = 72.0;
    double rsi_os               = 28.0;
    double daily_cap            = 60.0;
    int    timeout_h1_bars      = 14;
    int    cooldown_h1_bars     = 2;
    double crash_atr_threshold  = 60.0;
    int    session_min          = 1;
    int    session_max          = 6;
};

struct H4Params {
    double risk_dollars         = 10.0;
    double max_lot              = 0.01;
    double sl_struct_mult       = 0.5;
    double tp_mult              = 2.0;
    double trail_arm_mult       = 0.8;
    double trail_dist_mult      = 0.5;
    double adx_min              = 20.0;
    double adx_dead             = 15.0;
    int    channel_bars         = 20;
    double ema_min_sep          = 8.0;
    double max_spread           = 2.0;
    double rsi_ob               = 72.0;
    double rsi_os               = 28.0;
    double daily_cap            = 40.0;
    int    timeout_h4_bars      = 12;
    int    cooldown_h4_bars     = 3;
    bool   weekend_close_gate   = true;
};

// ──────────────────────────────────────────────────────────────────────────
// Trade record
// ──────────────────────────────────────────────────────────────────────────
struct Trade {
    int     id          = 0;
    int64_t entry_ts_ms = 0;
    int64_t exit_ts_ms  = 0;
    bool    is_long     = false;
    double  entry       = 0.0;
    double  exit        = 0.0;
    double  sl          = 0.0;
    double  size_full   = 0.0;    // for H1
    double  size        = 0.0;    // for H4 or size-closed-at
    double  mfe         = 0.0;
    double  pnl_pts     = 0.0;    // points * size (signed)
    double  pnl_usd     = 0.0;    // pnl_pts * 100 for gold
    std::string exit_reason;
};

// Session slot mapping (gold):
//   slot 1 = Asia early   (00-03 UTC)
//   slot 2 = Asia late    (03-07 UTC)
//   slot 3 = London open  (07-12 UTC)
//   slot 4 = NY overlap   (12-16 UTC)
//   slot 5 = NY late      (16-20 UTC)
//   slot 6 = Asia close / pre-Asia (20-24 UTC)
//   slot 0 = weekend / dead
static int session_slot_gold(int64_t ts_ms) {
    int64_t sec = ts_ms / 1000LL;
    int dow = (int)((sec / 86400LL + 3) % 7); // Mon=0..Sun=6
    if (dow == 5) return 0;                   // Saturday
    if (dow == 6) return 0;                   // Sunday (gold closed most of Sunday UTC)
    int hour = (int)((sec % 86400LL) / 3600LL);
    if (hour < 3)  return 1;
    if (hour < 7)  return 2;
    if (hour < 12) return 3;
    if (hour < 16) return 4;
    if (hour < 20) return 5;
    return 6;
}

// H4 trend state derived from H4 EMA stack.
// +1 = bullish (ema9>ema21>ema50), -1 = bearish (ema9<ema21<ema50), 0 = mixed
static int trend_state_from_ema(double ema9, double ema21, double ema50) {
    if (ema9 == 0.0 || ema21 == 0.0 || ema50 == 0.0) return 0;
    if (ema9 > ema21 && ema21 > ema50) return +1;
    if (ema9 < ema21 && ema21 < ema50) return -1;
    return 0;
}

// =============================================================================
// H1 Swing Engine (ported verbatim)
// =============================================================================
struct H1Engine {
    H1Params p;
    std::string symbol = "XAUUSD";

    struct OpenPos {
        bool    active           = false;
        bool    is_long          = false;
        double  entry            = 0.0;
        double  sl               = 0.0;
        double  tp1              = 0.0;
        double  tp2_trail_sl     = 0.0;
        bool    tp2_trail_active = false;
        bool    partial_done     = false;
        double  size_full        = 0.0;
        double  size_remaining   = 0.0;
        double  mfe              = 0.0;
        double  h1_atr           = 0.0;
        int64_t entry_ts_ms      = 0;
        int     h1_bars_held     = 0;
    } pos_;

    double  daily_pnl_          = 0.0;
    int64_t daily_reset_day_    = 0;
    int     h1_bar_count_       = 0;
    int     cooldown_until_bar_ = 0;
    double  prev_h1_ema9_       = 0.0;
    double  prev_h1_ema50_      = 0.0;
    int     m_trade_id_         = 0;

    // counters for telemetry
    uint64_t rej_daily_cap = 0, rej_session = 0, rej_cooldown = 0, rej_h4trend = 0,
             rej_h4adx = 0, rej_h1adx = 0, rej_warmup = 0, rej_stack = 0, rej_ema_sep = 0,
             rej_pullback = 0, rej_rsi = 0, rej_spread = 0, bars_seen = 0, entries = 0;

    std::vector<Trade> trades;

    bool has_open_position() const { return pos_.active; }

    void on_h1_bar(
        double mid, double bid, double ask,
        double h1_ema9, double h1_ema21, double h1_ema50,
        double h1_atr,  double h1_rsi,
        double h1_adx,  bool   h1_adx_rising,
        int    h1_trend_state,
        int    h4_trend_state,
        double h4_adx,
        double h4_atr,
        int    session_slot,
        int64_t now_ms)
    {
        ++bars_seen;
        ++h1_bar_count_;
        _daily_reset(now_ms);

        if (pos_.active) {
            pos_.h1_bars_held++;
            _manage_bar(mid, bid, ask, h1_ema9, h1_ema50, h1_atr, h1_adx, now_ms);
            prev_h1_ema9_  = h1_ema9;
            prev_h1_ema50_ = h1_ema50;
            return;
        }
        prev_h1_ema9_  = h1_ema9;
        prev_h1_ema50_ = h1_ema50;

        if (daily_pnl_ < -p.daily_cap)                 { ++rej_daily_cap; return; }
        if (session_slot < p.session_min || session_slot > p.session_max) { ++rej_session; return; }
        if (h1_bar_count_ < cooldown_until_bar_)        { ++rej_cooldown; return; }
        if (h4_trend_state == 0)                         { ++rej_h4trend; return; }
        if (h4_adx < p.h4_adx_gate)                      { ++rej_h4adx;  return; }
        if (h1_adx < p.adx_min || !h1_adx_rising)        { ++rej_h1adx;  return; }
        if (h1_ema9 <= 0.0 || h1_ema50 <= 0.0 || h1_atr <= 0.0) { ++rej_warmup; return; }

        const bool intend_long  = (h4_trend_state == +1);
        const bool intend_short = (h4_trend_state == -1);

        const bool bull_stack = (h1_ema9 > h1_ema21 && h1_ema21 > h1_ema50);
        const bool bear_stack = (h1_ema9 < h1_ema21 && h1_ema21 < h1_ema50);
        if (intend_long  && !bull_stack) { ++rej_stack; return; }
        if (intend_short && !bear_stack) { ++rej_stack; return; }

        if (std::fabs(h1_ema9 - h1_ema50) < p.ema_min_sep) { ++rej_ema_sep; return; }

        const double band = h1_atr * p.pullback_atr_mult;
        if (std::fabs(mid - h1_ema21) > band) { ++rej_pullback; return; }
        if (intend_long  && mid < h1_ema21 - band * 0.4) { ++rej_pullback; return; }
        if (intend_short && mid > h1_ema21 + band * 0.4) { ++rej_pullback; return; }

        if (intend_long  && h1_rsi > p.rsi_ob) { ++rej_rsi; return; }
        if (intend_short && h1_rsi < p.rsi_os) { ++rej_rsi; return; }

        if ((ask - bid) > p.max_spread) { ++rej_spread; return; }

        const double effective_risk = (h4_atr > p.crash_atr_threshold)
            ? (p.risk_dollars * 0.5) : p.risk_dollars;

        const double sl_pts  = h1_atr * p.sl_mult;
        const double tp1_pts = h1_atr * p.tp1_mult;
        const double entry_px = intend_long ? ask : bid;
        const double sl_px    = intend_long ? (entry_px - sl_pts) : (entry_px + sl_pts);
        const double tp1_px   = intend_long ? (entry_px + tp1_pts) : (entry_px - tp1_pts);

        double size = effective_risk / (sl_pts * 100.0);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(0.01, std::min(p.max_lot, size));

        pos_.active           = true;
        pos_.is_long          = intend_long;
        pos_.entry            = entry_px;
        pos_.sl               = sl_px;
        pos_.tp1              = tp1_px;
        pos_.tp2_trail_sl     = sl_px;
        pos_.tp2_trail_active = false;
        pos_.partial_done     = false;
        pos_.size_full        = size;
        pos_.size_remaining   = size;
        pos_.mfe              = 0.0;
        pos_.h1_atr           = h1_atr;
        pos_.entry_ts_ms      = now_ms;
        pos_.h1_bars_held     = 0;
        ++m_trade_id_;
        ++entries;
    }

    void on_tick(double bid, double ask, int64_t now_ms) {
        if (!pos_.active) return;
        const double mid = (bid + ask) * 0.5;
        const double move = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
        if (move > pos_.mfe) pos_.mfe = move;

        // Partial TP1
        if (!pos_.partial_done) {
            const bool tp1_hit = pos_.is_long ? (bid >= pos_.tp1) : (ask <= pos_.tp1);
            if (tp1_hit) {
                const double px  = pos_.tp1;
                const double pnl = (pos_.is_long ? (px - pos_.entry) : (pos_.entry - px))
                                   * (pos_.size_full * 0.5);
                daily_pnl_ += pnl * 100.0;
                // Record half-close as separate trade for clarity
                Trade t;
                t.id = m_trade_id_;
                t.entry_ts_ms = pos_.entry_ts_ms;
                t.exit_ts_ms  = now_ms;
                t.is_long     = pos_.is_long;
                t.entry       = pos_.entry;
                t.exit        = px;
                t.sl          = pos_.sl;
                t.size_full   = pos_.size_full;
                t.size        = pos_.size_full * 0.5;
                t.mfe         = pos_.mfe;
                t.pnl_pts     = (pos_.is_long ? (px - pos_.entry) : (pos_.entry - px));
                t.pnl_usd     = pnl * 100.0;
                t.exit_reason = "PARTIAL_TP1";
                trades.push_back(t);
                pos_.partial_done     = true;
                pos_.size_remaining   = pos_.size_full * 0.5;
                if (pos_.is_long && pos_.sl < pos_.entry)
                    pos_.sl = pos_.entry;
                else if (!pos_.is_long && pos_.sl > pos_.entry)
                    pos_.sl = pos_.entry;
                return;
            }
        }

        const double arm_pts  = pos_.h1_atr * p.tp2_trail_arm_mult;
        const double dist_pts = pos_.h1_atr * p.tp2_trail_dist_mult;
        if (pos_.mfe >= arm_pts) {
            const double new_trail = pos_.is_long ? (mid - dist_pts) : (mid + dist_pts);
            if (!pos_.tp2_trail_active) {
                if (pos_.is_long ? (new_trail > pos_.sl) : (new_trail < pos_.sl)) {
                    pos_.tp2_trail_sl     = new_trail;
                    pos_.tp2_trail_active = true;
                }
            } else {
                if (pos_.is_long ? (new_trail > pos_.tp2_trail_sl)
                                 : (new_trail < pos_.tp2_trail_sl))
                    pos_.tp2_trail_sl = new_trail;
            }
        }

        const double eff_sl = pos_.tp2_trail_active ? pos_.tp2_trail_sl : pos_.sl;
        if (pos_.is_long ? (bid <= eff_sl) : (ask >= eff_sl)) {
            _close(pos_.is_long ? bid : ask,
                   pos_.tp2_trail_active ? "TRAIL_SL" : "SL_HIT",
                   now_ms);
        }
    }

private:
    void _daily_reset(int64_t now_ms) {
        const int64_t day = (now_ms / 1000LL) / 86400LL;
        if (day != daily_reset_day_) { daily_pnl_ = 0.0; daily_reset_day_ = day; }
    }
    void _manage_bar(double mid, double bid, double ask,
                     double h1_ema9, double h1_ema50,
                     double h1_atr, double h1_adx, int64_t now_ms)
    {
        (void)mid; (void)h1_atr;
        if (pos_.h1_bars_held >= p.timeout_h1_bars) {
            _close(pos_.is_long ? bid : ask, "H1_TIMEOUT", now_ms);
            return;
        }
        if (prev_h1_ema9_ > 0.0 && h1_ema9 > 0.0) {
            const bool was_bull = (prev_h1_ema9_ > prev_h1_ema50_);
            const bool now_bull = (h1_ema9 > h1_ema50);
            if (pos_.is_long && was_bull && !now_bull) {
                _close(bid, "EMA_CROSS", now_ms); return;
            }
            if (!pos_.is_long && !was_bull && now_bull) {
                _close(ask, "EMA_CROSS", now_ms); return;
            }
        }
        if (h1_adx > 0.0 && h1_adx < 15.0) {
            _close(pos_.is_long ? bid : ask, "ADX_COLLAPSE", now_ms);
            return;
        }
    }
    void _close(double exit_px, const char* reason, int64_t now_ms) {
        const double pnl_pts = (pos_.is_long
            ? (exit_px - pos_.entry)
            : (pos_.entry - exit_px)) * pos_.size_remaining;
        daily_pnl_ += pnl_pts * 100.0;
        Trade t;
        t.id = m_trade_id_;
        t.entry_ts_ms = pos_.entry_ts_ms;
        t.exit_ts_ms  = now_ms;
        t.is_long     = pos_.is_long;
        t.entry       = pos_.entry;
        t.exit        = exit_px;
        t.sl          = pos_.sl;
        t.size_full   = pos_.size_full;
        t.size        = pos_.size_remaining;
        t.mfe         = pos_.mfe;
        t.pnl_pts     = (pos_.is_long ? (exit_px - pos_.entry) : (pos_.entry - exit_px));
        t.pnl_usd     = pnl_pts * 100.0;
        t.exit_reason = reason;
        trades.push_back(t);
        cooldown_until_bar_ = h1_bar_count_ + p.cooldown_h1_bars;
        pos_ = OpenPos{};
    }
};

// =============================================================================
// H4 Regime Engine (ported verbatim)
// =============================================================================
struct H4Engine {
    H4Params p;
    std::string symbol = "XAUUSD";

    struct OpenPos {
        bool    active        = false;
        bool    is_long       = false;
        double  entry         = 0.0;
        double  sl            = 0.0;
        double  channel_level = 0.0;
        double  tp            = 0.0;
        double  size          = 0.0;
        double  mfe           = 0.0;
        double  h4_atr        = 0.0;
        bool    trail_active  = false;
        double  trail_sl      = 0.0;
        int64_t entry_ts_ms   = 0;
        int     h4_bars_held  = 0;
    } pos_;

    double  daily_pnl_          = 0.0;
    int64_t daily_reset_day_    = 0;
    int     h4_bar_count_       = 0;
    int     cooldown_until_bar_ = 0;
    std::deque<double> h4_highs_;
    std::deque<double> h4_lows_;
    double channel_high_ = 0.0;
    double channel_low_  = 1e9;
    int    m_trade_id_   = 0;

    uint64_t rej_daily_cap = 0, rej_cooldown = 0, rej_channel_warmup = 0, rej_weekend = 0,
             rej_adx = 0, rej_warmup = 0, rej_no_break = 0, rej_ema_sep = 0,
             rej_rsi = 0, rej_m15_atr = 0, rej_spread = 0, bars_seen = 0, entries = 0;

    std::vector<Trade> trades;

    bool has_open_position() const { return pos_.active; }

    void on_h4_bar(
        double h4_bar_high, double h4_bar_low, double h4_bar_close,
        double mid, double bid, double ask,
        double h4_ema9, double h4_ema50,
        double h4_atr, double h4_rsi, double h4_adx,
        bool   m15_atr_expanding,
        int64_t now_ms)
    {
        ++bars_seen;
        ++h4_bar_count_;
        _daily_reset(now_ms);

        h4_highs_.push_back(h4_bar_high);
        h4_lows_ .push_back(h4_bar_low);
        if ((int)h4_highs_.size() > p.channel_bars) {
            h4_highs_.pop_front();
            h4_lows_ .pop_front();
        }
        if ((int)h4_highs_.size() >= p.channel_bars) {
            channel_high_ = *std::max_element(h4_highs_.begin(), h4_highs_.end());
            channel_low_  = *std::min_element(h4_lows_ .begin(), h4_lows_ .end());
        }

        if (pos_.active) {
            pos_.h4_bars_held++;
            _manage_bar(bid, ask, h4_adx, now_ms);
            return;
        }

        if (daily_pnl_ < -p.daily_cap)                    { ++rej_daily_cap; return; }
        if (h4_bar_count_ < cooldown_until_bar_)          { ++rej_cooldown;  return; }
        if ((int)h4_highs_.size() < p.channel_bars)       { ++rej_channel_warmup; return; }
        if (channel_high_ <= 0.0 || channel_low_ >= 1e8)  { ++rej_channel_warmup; return; }

        if (p.weekend_close_gate) {
            const int64_t utc_sec  = now_ms / 1000LL;
            const int     utc_dow  = static_cast<int>((utc_sec / 86400LL + 3) % 7);
            const int     utc_hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
            if (utc_dow == 4 && utc_hour >= 20) { ++rej_weekend; return; }
        }

        if (h4_adx < p.adx_min)                           { ++rej_adx;    return; }
        if (h4_ema9 <= 0.0 || h4_ema50 <= 0.0 || h4_atr <= 0.0) { ++rej_warmup; return; }

        const bool bull_break = (h4_bar_close > channel_high_);
        const bool bear_break = (h4_bar_close < channel_low_);
        if (!bull_break && !bear_break) { ++rej_no_break; return; }

        const bool intend_long = bull_break;

        const double ema_sep = h4_ema9 - h4_ema50;
        if (intend_long  && ema_sep < p.ema_min_sep)  { ++rej_ema_sep; return; }
        if (!intend_long && ema_sep > -p.ema_min_sep) { ++rej_ema_sep; return; }

        if (intend_long  && h4_rsi > p.rsi_ob) { ++rej_rsi; return; }
        if (!intend_long && h4_rsi < p.rsi_os) { ++rej_rsi; return; }

        if (!m15_atr_expanding) { ++rej_m15_atr; return; }

        if ((ask - bid) > p.max_spread) { ++rej_spread; return; }

        const double struct_level = intend_long ? channel_high_ : channel_low_;
        const double sl_pts  = h4_atr * p.sl_struct_mult;
        const double tp_pts  = h4_atr * p.tp_mult;
        const double entry_px = intend_long ? ask : bid;
        const double sl_px   = intend_long
            ? (struct_level - sl_pts) : (struct_level + sl_pts);
        const double tp_px   = intend_long
            ? (entry_px + tp_pts) : (entry_px - tp_pts);

        double size = p.risk_dollars / (sl_pts * 100.0);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(0.01, std::min(p.max_lot, size));

        pos_.active        = true;
        pos_.is_long       = intend_long;
        pos_.entry         = entry_px;
        pos_.sl            = sl_px;
        pos_.channel_level = struct_level;
        pos_.tp            = tp_px;
        pos_.size          = size;
        pos_.mfe           = 0.0;
        pos_.h4_atr        = h4_atr;
        pos_.trail_active  = false;
        pos_.trail_sl      = sl_px;
        pos_.entry_ts_ms   = now_ms;
        pos_.h4_bars_held  = 0;
        ++m_trade_id_;
        ++entries;
    }

    void on_tick(double bid, double ask, int64_t now_ms) {
        if (!pos_.active) return;
        const double mid = (bid + ask) * 0.5;
        const double move = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
        if (move > pos_.mfe) pos_.mfe = move;

        const double arm_pts  = pos_.h4_atr * p.trail_arm_mult;
        const double dist_pts = pos_.h4_atr * p.trail_dist_mult;
        if (pos_.mfe >= arm_pts) {
            const double nt = pos_.is_long ? (mid - dist_pts) : (mid + dist_pts);
            if (!pos_.trail_active) {
                if (pos_.is_long ? (nt > pos_.sl) : (nt < pos_.sl)) {
                    pos_.trail_sl    = nt;
                    pos_.trail_active = true;
                }
            } else {
                if (pos_.is_long ? (nt > pos_.trail_sl) : (nt < pos_.trail_sl))
                    pos_.trail_sl = nt;
            }
        }
        const double eff_sl = pos_.trail_active ? pos_.trail_sl : pos_.sl;
        if (pos_.is_long ? (bid <= eff_sl) : (ask >= eff_sl)) {
            _close(pos_.is_long ? bid : ask,
                   pos_.trail_active ? "TRAIL_SL" : "SL_HIT",
                   now_ms);
        } else if (pos_.is_long ? (bid >= pos_.tp) : (ask <= pos_.tp)) {
            _close(pos_.is_long ? bid : ask, "TP_HIT", now_ms);
        }
    }

    void check_weekend_close(double bid, double ask, int64_t now_ms) {
        if (!pos_.active || !p.weekend_close_gate) return;
        const int64_t utc_sec  = now_ms / 1000LL;
        const int     utc_dow  = static_cast<int>((utc_sec / 86400LL + 3) % 7);
        const int     utc_hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
        if (utc_dow != 4 || utc_hour < 20) return;
        const double mid  = (bid + ask) * 0.5;
        const double move = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
        if (move > 0.0) {
            _close(pos_.is_long ? bid : ask, "WEEKEND_CLOSE", now_ms);
        }
    }

private:
    void _daily_reset(int64_t now_ms) {
        const int64_t day = (now_ms / 1000LL) / 86400LL;
        if (day != daily_reset_day_) { daily_pnl_ = 0.0; daily_reset_day_ = day; }
    }
    void _manage_bar(double bid, double ask, double h4_adx, int64_t now_ms) {
        if (pos_.h4_bars_held >= p.timeout_h4_bars) {
            _close(pos_.is_long ? bid : ask, "H4_TIMEOUT", now_ms); return;
        }
        if (h4_adx > 0.0 && h4_adx < p.adx_dead) {
            _close(pos_.is_long ? bid : ask, "ADX_COLLAPSE", now_ms); return;
        }
    }
    void _close(double exit_px, const char* reason, int64_t now_ms) {
        const double pnl_pts = (pos_.is_long
            ? (exit_px - pos_.entry)
            : (pos_.entry - exit_px)) * pos_.size;
        daily_pnl_ += pnl_pts * 100.0;
        Trade t;
        t.id = m_trade_id_;
        t.entry_ts_ms = pos_.entry_ts_ms;
        t.exit_ts_ms  = now_ms;
        t.is_long     = pos_.is_long;
        t.entry       = pos_.entry;
        t.exit        = exit_px;
        t.sl          = pos_.sl;
        t.size_full   = pos_.size;
        t.size        = pos_.size;
        t.mfe         = pos_.mfe;
        t.pnl_pts     = (pos_.is_long ? (exit_px - pos_.entry) : (pos_.entry - exit_px));
        t.pnl_usd     = pnl_pts * 100.0;
        t.exit_reason = reason;
        trades.push_back(t);
        cooldown_until_bar_ = h4_bar_count_ + p.cooldown_h4_bars;
        pos_ = OpenPos{};
    }
};

// =============================================================================
// Bar builder
// =============================================================================
class BarBuilder {
public:
    int64_t duration_ms = 0;
    Bar current;
    bool has_current = false;

    BarBuilder() = default;
    explicit BarBuilder(int64_t dur_ms) : duration_ms(dur_ms) {}

    // Returns true + completed-bar in `out` if a bar just closed on this tick.
    // `anchor_ms` = aligned bar-open timestamp (floor of now_ms / duration_ms * duration_ms)
    bool on_tick(int64_t now_ms, double mid, Bar& out) {
        int64_t anchor = (now_ms / duration_ms) * duration_ms;
        if (!has_current) {
            current = Bar{};
            current.ts_ms_open  = anchor;
            current.ts_ms_close = anchor + duration_ms;
            current.open  = mid;
            current.high  = mid;
            current.low   = mid;
            current.close = mid;
            current.n     = 1;
            has_current = true;
            return false;
        }
        if (anchor != current.ts_ms_open) {
            // bar boundary crossed -- emit previous bar
            out = current;
            current = Bar{};
            current.ts_ms_open  = anchor;
            current.ts_ms_close = anchor + duration_ms;
            current.open  = mid;
            current.high  = mid;
            current.low   = mid;
            current.close = mid;
            current.n     = 1;
            return true;
        }
        // update current
        if (mid > current.high) current.high = mid;
        if (mid < current.low)  current.low  = mid;
        current.close = mid;
        ++current.n;
        return false;
    }
};

// =============================================================================
// Main
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

static void write_trade_log(const char* path, const std::vector<Trade>& trades) {
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot open %s for write\n", path); return; }
    fprintf(f, "trade_id,entry_ts,exit_ts,side,entry,exit,sl,size,mfe,pnl_pts,pnl_usd,exit_reason\n");
    for (const Trade& t : trades) {
        fprintf(f, "%d,%s,%s,%s,%.3f,%.3f,%.3f,%.4f,%.3f,%.3f,%.2f,%s\n",
                t.id,
                fmt_ts(t.entry_ts_ms).c_str(),
                fmt_ts(t.exit_ts_ms).c_str(),
                t.is_long ? "LONG" : "SHORT",
                t.entry, t.exit, t.sl, t.size, t.mfe,
                t.pnl_pts, t.pnl_usd, t.exit_reason.c_str());
    }
    fclose(f);
}

static void write_equity_curve(const char* path, const std::vector<Trade>& trades) {
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot open %s for write\n", path); return; }
    fprintf(f, "exit_ts,cumulative_pnl_usd\n");
    double cum = 0.0;
    for (const Trade& t : trades) {
        cum += t.pnl_usd;
        fprintf(f, "%s,%.2f\n", fmt_ts(t.exit_ts_ms).c_str(), cum);
    }
    fclose(f);
}

struct Stats {
    int    n_trades = 0;
    int    n_wins   = 0;
    int    n_losses = 0;
    double total_pnl = 0.0;
    double gross_win = 0.0;
    double gross_loss = 0.0;
    double avg_win = 0.0;
    double avg_loss = 0.0;
    double max_dd   = 0.0;
    double max_equity = 0.0;
    double expectancy = 0.0;
    double profit_factor = 0.0;
    double win_rate = 0.0;
};

static Stats compute_stats(const std::vector<Trade>& trades) {
    Stats s{};
    double cum = 0.0, peak = 0.0;
    for (const Trade& t : trades) {
        ++s.n_trades;
        s.total_pnl += t.pnl_usd;
        if (t.pnl_usd > 0.0) { ++s.n_wins; s.gross_win += t.pnl_usd; }
        else                 { ++s.n_losses; s.gross_loss += -t.pnl_usd; }
        cum += t.pnl_usd;
        if (cum > peak) peak = cum;
        double dd = peak - cum;
        if (dd > s.max_dd) s.max_dd = dd;
        if (cum > s.max_equity) s.max_equity = cum;
    }
    if (s.n_wins > 0)    s.avg_win  = s.gross_win  / s.n_wins;
    if (s.n_losses > 0)  s.avg_loss = s.gross_loss / s.n_losses;
    if (s.n_trades > 0)  s.win_rate = 100.0 * s.n_wins / s.n_trades;
    if (s.n_trades > 0)  s.expectancy = s.total_pnl / s.n_trades;
    if (s.gross_loss > 0.0) s.profit_factor = s.gross_win / s.gross_loss;
    return s;
}

static void write_summary(const char* path, const char* engine_name,
                          const Stats& s, const std::vector<Trade>& trades,
                          uint64_t bars_seen, uint64_t entries_attempted)
{
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot open %s for write\n", path); return; }
    fprintf(f, "================================================================\n");
    fprintf(f, "  %s -- 2yr XAUUSD backtest summary\n", engine_name);
    fprintf(f, "================================================================\n\n");
    fprintf(f, "Bars processed:       %llu\n", (unsigned long long)bars_seen);
    fprintf(f, "Entry events:         %llu\n", (unsigned long long)entries_attempted);
    fprintf(f, "Trades (records):     %d\n", s.n_trades);
    fprintf(f, "  Wins:               %d\n", s.n_wins);
    fprintf(f, "  Losses:             %d\n", s.n_losses);
    fprintf(f, "Win rate:             %.1f%%\n", s.win_rate);
    fprintf(f, "Total PnL (USD):      $%+.2f\n", s.total_pnl);
    fprintf(f, "Gross win:            $%+.2f\n", s.gross_win);
    fprintf(f, "Gross loss:           $-%.2f\n", s.gross_loss);
    fprintf(f, "Avg win:              $%+.2f\n", s.avg_win);
    fprintf(f, "Avg loss:             $-%.2f\n", s.avg_loss);
    fprintf(f, "Expectancy/trade:     $%+.2f\n", s.expectancy);
    fprintf(f, "Profit factor:        %.2f\n", s.profit_factor);
    fprintf(f, "Max drawdown:         $-%.2f\n", s.max_dd);

    // Exit reason breakdown
    fprintf(f, "\nExit reasons:\n");
    struct RB { std::string reason; int n = 0; double pnl = 0.0; };
    std::vector<RB> rb;
    auto find_or = [&](const std::string& r) -> RB& {
        for (RB& x : rb) if (x.reason == r) return x;
        rb.push_back(RB{r, 0, 0.0});
        return rb.back();
    };
    for (const Trade& t : trades) {
        RB& x = find_or(t.exit_reason);
        x.n += 1;
        x.pnl += t.pnl_usd;
    }
    std::sort(rb.begin(), rb.end(), [](const RB& a, const RB& b){ return a.n > b.n; });
    for (const RB& x : rb) {
        fprintf(f, "  %-16s n=%-5d pnl=$%+.2f\n", x.reason.c_str(), x.n, x.pnl);
    }

    // Year / month breakdown
    fprintf(f, "\nMonthly PnL:\n");
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
        fprintf(f, "  %04d-%02d: trades=%-4d pnl=$%+.2f cum=$%+.2f\n",
                x.y, x.m, x.n, x.pnl, cum);
    }
    fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <tick_csv>\n", argv[0]);
        return 1;
    }
    const char* csv_path = argv[1];

    FILE* f = fopen(csv_path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", csv_path); return 1; }

    auto t0 = std::chrono::steady_clock::now();

    // Timeframe bar builders
    BarBuilder bb_m15( 15 * 60 * 1000);
    BarBuilder bb_h1 ( 60 * 60 * 1000);
    BarBuilder bb_h4 (240 * 60 * 1000);

    // M15 indicators (for H4 m15_atr_expanding confirmation)
    ATR m15_atr; m15_atr.set(14);
    double prev_m15_atr_values[3] = {0.0, 0.0, 0.0};
    int m15_atr_hist_n = 0;

    // H1 indicators
    EMA h1_ema9, h1_ema21, h1_ema50; h1_ema9.set(9); h1_ema21.set(21); h1_ema50.set(50);
    ATR h1_atr; h1_atr.set(14);
    RSI h1_rsi; h1_rsi.set(14);
    ADX h1_adx; h1_adx.set(14);
    double prev_h1_adx = 0.0;

    // H4 indicators
    EMA h4_ema9, h4_ema21, h4_ema50; h4_ema9.set(9); h4_ema21.set(21); h4_ema50.set(50);
    ATR h4_atr; h4_atr.set(14);
    RSI h4_rsi; h4_rsi.set(14);
    ADX h4_adx; h4_adx.set(14);

    // Engines
    H1Engine h1e;
    H4Engine h4e;

    // Parsing loop
    char line[256];
    uint64_t ticks_ok = 0, ticks_fail = 0;
    int64_t first_ts = 0, last_ts = 0;

    while (fgets(line, sizeof(line), f)) {
        Tick tk;
        if (!parse_line(line, tk)) { ++ticks_fail; continue; }
        ++ticks_ok;
        if (first_ts == 0) first_ts = tk.ts_ms;
        last_ts = tk.ts_ms;

        double mid = (tk.bid + tk.ask) * 0.5;

        // Drive engine tick-level management FIRST (before bar close/new bar)
        if (h1e.has_open_position()) h1e.on_tick(tk.bid, tk.ask, tk.ts_ms);
        if (h4e.has_open_position()) {
            h4e.on_tick(tk.bid, tk.ask, tk.ts_ms);
            h4e.check_weekend_close(tk.bid, tk.ask, tk.ts_ms);
        }

        // M15 bar close -- updates m15 ATR for H4 m15_atr_expanding signal
        Bar m15_closed;
        if (bb_m15.on_tick(tk.ts_ms, mid, m15_closed)) {
            m15_atr.push(m15_closed.high, m15_closed.low, m15_closed.close);
            if (m15_atr.primed) {
                prev_m15_atr_values[2] = prev_m15_atr_values[1];
                prev_m15_atr_values[1] = prev_m15_atr_values[0];
                prev_m15_atr_values[0] = m15_atr.value;
                if (m15_atr_hist_n < 3) ++m15_atr_hist_n;
            }
        }

        // H1 bar close
        Bar h1_closed;
        if (bb_h1.on_tick(tk.ts_ms, mid, h1_closed)) {
            h1_ema9.push (h1_closed.close);
            h1_ema21.push(h1_closed.close);
            h1_ema50.push(h1_closed.close);
            h1_atr.push  (h1_closed.high, h1_closed.low, h1_closed.close);
            h1_rsi.push  (h1_closed.close);
            h1_adx.push  (h1_closed.high, h1_closed.low, h1_closed.close);

            bool primed = h1_ema9.primed && h1_ema21.primed && h1_ema50.primed
                        && h1_atr.primed && h1_rsi.primed   && h1_adx.primed;
            bool adx_rising = (h1_adx.value > prev_h1_adx);
            prev_h1_adx = h1_adx.value;

            if (primed) {
                int h1_trend  = trend_state_from_ema(h1_ema9.value, h1_ema21.value, h1_ema50.value);
                int h4_trend  = trend_state_from_ema(h4_ema9.value, h4_ema21.value, h4_ema50.value);
                int slot      = session_slot_gold(tk.ts_ms);
                h1e.on_h1_bar(
                    mid, tk.bid, tk.ask,
                    h1_ema9.value, h1_ema21.value, h1_ema50.value,
                    h1_atr.value, h1_rsi.value,
                    h1_adx.value, adx_rising,
                    h1_trend, h4_trend,
                    h4_adx.primed ? h4_adx.value : 0.0,
                    h4_atr.primed ? h4_atr.value : 0.0,
                    slot, tk.ts_ms);
            }
        }

        // H4 bar close
        Bar h4_closed;
        if (bb_h4.on_tick(tk.ts_ms, mid, h4_closed)) {
            h4_ema9.push (h4_closed.close);
            h4_ema21.push(h4_closed.close);
            h4_ema50.push(h4_closed.close);
            h4_atr.push  (h4_closed.high, h4_closed.low, h4_closed.close);
            h4_rsi.push  (h4_closed.close);
            h4_adx.push  (h4_closed.high, h4_closed.low, h4_closed.close);

            bool primed = h4_ema9.primed && h4_ema21.primed && h4_ema50.primed
                        && h4_atr.primed && h4_rsi.primed   && h4_adx.primed;
            bool m15_atr_expanding = (m15_atr_hist_n >= 3) &&
                (prev_m15_atr_values[0] > prev_m15_atr_values[1]) &&
                (prev_m15_atr_values[1] > prev_m15_atr_values[2]);

            if (primed) {
                h4e.on_h4_bar(
                    h4_closed.high, h4_closed.low, h4_closed.close,
                    mid, tk.bid, tk.ask,
                    h4_ema9.value, h4_ema50.value,
                    h4_atr.value, h4_rsi.value, h4_adx.value,
                    m15_atr_expanding,
                    tk.ts_ms);
            }
        }

        if (ticks_ok % 20000000ULL == 0ULL) {
            printf("[PROGRESS] %llu ticks  date=%s  h1_trades=%zu  h4_trades=%zu\n",
                   (unsigned long long)ticks_ok,
                   fmt_ts(tk.ts_ms).c_str(),
                   h1e.trades.size(), h4e.trades.size());
            fflush(stdout);
        }
    }
    fclose(f);

    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    printf("\n==== LOAD COMPLETE ====\n");
    printf("Ticks OK:     %llu\n", (unsigned long long)ticks_ok);
    printf("Ticks failed: %llu\n", (unsigned long long)ticks_fail);
    printf("Date range:   %s -> %s\n", fmt_ts(first_ts).c_str(), fmt_ts(last_ts).c_str());
    printf("Runtime:      %.1fs\n\n", secs);

    printf("==== H1SwingEngine ====\n");
    printf("Bars seen:       %llu\n", (unsigned long long)h1e.bars_seen);
    printf("Entries:         %llu\n", (unsigned long long)h1e.entries);
    printf("Trade records:   %zu\n", h1e.trades.size());
    printf("Rejections: daily_cap=%llu session=%llu cooldown=%llu h4trend=%llu h4adx=%llu "
           "h1adx=%llu warmup=%llu stack=%llu ema_sep=%llu pullback=%llu rsi=%llu spread=%llu\n",
           (unsigned long long)h1e.rej_daily_cap, (unsigned long long)h1e.rej_session,
           (unsigned long long)h1e.rej_cooldown, (unsigned long long)h1e.rej_h4trend,
           (unsigned long long)h1e.rej_h4adx,    (unsigned long long)h1e.rej_h1adx,
           (unsigned long long)h1e.rej_warmup,   (unsigned long long)h1e.rej_stack,
           (unsigned long long)h1e.rej_ema_sep,  (unsigned long long)h1e.rej_pullback,
           (unsigned long long)h1e.rej_rsi,      (unsigned long long)h1e.rej_spread);
    Stats h1s = compute_stats(h1e.trades);
    printf("PnL: $%+.2f   WR: %.1f%%   n=%d   DD: $-%.2f   PF: %.2f\n",
           h1s.total_pnl, h1s.win_rate, h1s.n_trades, h1s.max_dd, h1s.profit_factor);

    printf("\n==== H4RegimeEngine ====\n");
    printf("Bars seen:       %llu\n", (unsigned long long)h4e.bars_seen);
    printf("Entries:         %llu\n", (unsigned long long)h4e.entries);
    printf("Trade records:   %zu\n", h4e.trades.size());
    printf("Rejections: daily_cap=%llu cooldown=%llu ch_warmup=%llu weekend=%llu adx=%llu "
           "warmup=%llu no_break=%llu ema_sep=%llu rsi=%llu m15_atr=%llu spread=%llu\n",
           (unsigned long long)h4e.rej_daily_cap, (unsigned long long)h4e.rej_cooldown,
           (unsigned long long)h4e.rej_channel_warmup, (unsigned long long)h4e.rej_weekend,
           (unsigned long long)h4e.rej_adx, (unsigned long long)h4e.rej_warmup,
           (unsigned long long)h4e.rej_no_break, (unsigned long long)h4e.rej_ema_sep,
           (unsigned long long)h4e.rej_rsi, (unsigned long long)h4e.rej_m15_atr,
           (unsigned long long)h4e.rej_spread);
    Stats h4s = compute_stats(h4e.trades);
    printf("PnL: $%+.2f   WR: %.1f%%   n=%d   DD: $-%.2f   PF: %.2f\n",
           h4s.total_pnl, h4s.win_rate, h4s.n_trades, h4s.max_dd, h4s.profit_factor);

    // Write output files
    write_trade_log   ("htf_bt_h1_trades.csv", h1e.trades);
    write_trade_log   ("htf_bt_h4_trades.csv", h4e.trades);
    write_equity_curve("htf_bt_h1_equity.csv", h1e.trades);
    write_equity_curve("htf_bt_h4_equity.csv", h4e.trades);
    write_summary     ("htf_bt_h1.txt", "H1SwingEngine", h1s, h1e.trades,
                       h1e.bars_seen, h1e.entries);
    write_summary     ("htf_bt_h4.txt", "H4RegimeEngine", h4s, h4e.trades,
                       h4e.bars_seen, h4e.entries);

    printf("\nWrote:\n");
    printf("  htf_bt_h1.txt              (H1 summary)\n");
    printf("  htf_bt_h4.txt              (H4 summary)\n");
    printf("  htf_bt_h1_trades.csv       (H1 trade log)\n");
    printf("  htf_bt_h4_trades.csv       (H4 trade log)\n");
    printf("  htf_bt_h1_equity.csv       (H1 equity curve)\n");
    printf("  htf_bt_h4_equity.csv       (H4 equity curve)\n");
    return 0;
}

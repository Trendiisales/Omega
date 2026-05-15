// =============================================================================
// IndexNoiseBandMomentumBacktest.cpp
// =============================================================================
// Standalone backtest harness for NoiseBandMomentumEngine on index tick data.
// Replicates ATR-band breakout from session open with VWAP alignment filter,
// in-flight BE_RATCHET / LOSS_CUT / VWAP_STOP management, and CrossPosition
// base manage (BE lock, mid-lock, trail, TP extend, timeout).
//
// No external dependencies -- compile with:
//   clang++ -std=c++17 -O3 -o backtest/idx_nbm_bt backtest/IndexNoiseBandMomentumBacktest.cpp
//
// Usage:
//   ./backtest/idx_nbm_bt --instrument SP ~/Tick/SPXUSD/...csv
//   ./backtest/idx_nbm_bt --instrument NQ ~/Tick/Nas/...csv
//
// Multi-config sweep: ATR_MULT x TP_PCT x SL_PCT x LOSS_CUT_PCT = 54 combos.
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <numeric>
#include <array>
#include <limits>
#include <functional>

// =============================================================================
// TICK FORMAT AUTO-DETECTION
// =============================================================================
enum class TickFmt { UNKNOWN, HISTDATA, DUKA_BID_ASK, DUKA_ASK_BID, JFOREX };

struct RawTick {
    int64_t ts_ms = 0;
    double  bid   = 0.0;
    double  ask   = 0.0;
};

// timegm portability
static int64_t portable_timegm(struct tm* t) {
#if defined(__APPLE__) || defined(__linux__)
    return static_cast<int64_t>(timegm(t));
#else
    return static_cast<int64_t>(_mkgmtime(t));
#endif
}

static TickFmt detect_format(const std::string& line) {
    if (line.empty()) return TickFmt::UNKNOWN;

    // JFOREX: starts with "Time" header or contains EET
    // Actual data lines start with DD.MM.YYYY or similar date
    if (line.find("Time") != std::string::npos && line.find("EET") != std::string::npos)
        return TickFmt::JFOREX;

    // Check for JFOREX data line: DD.MM.YYYY HH:MM:SS.mmm
    if (line.size() > 23 && line[2] == '.' && line[5] == '.' && line[10] == ' ' &&
        line[13] == ':' && line[16] == ':' && line[19] == '.')
        return TickFmt::JFOREX;

    // HISTDATA: YYYYMMDD HHMMSSmmm,bid,ask,0
    if (line.size() > 25 && line[8] == ' ' && std::isdigit((unsigned char)line[0]) &&
        std::isdigit((unsigned char)line[9])) {
        // Count commas
        int commas = 0;
        for (char c : line) if (c == ',') commas++;
        if (commas == 3) return TickFmt::HISTDATA;
    }

    // Numeric-leading with commas -> DUKA variants
    if (std::isdigit((unsigned char)line[0])) {
        int commas = 0;
        for (char c : line) if (c == ',') commas++;
        if (commas == 2) {
            // timestamp_ms,val1,val2 -- need to detect bid/ask vs ask/bid
            // Heuristic: if first price > second price -> first=ask, second=bid (DUKA_ASK_BID)
            // if first price < second price -> first=bid, second=ask (DUKA_BID_ASK)
            // We'll guess DUKA_BID_ASK by default and the caller can override
            return TickFmt::DUKA_BID_ASK;
        }
    }

    return TickFmt::UNKNOWN;
}

static bool parse_histdata(const char* line, RawTick& out) {
    if (std::strlen(line) < 25) return false;
    int Y, M, D, h, m, s, ms;
    if (std::sscanf(line, "%4d%2d%2d %2d%2d%2d%3d", &Y, &M, &D, &h, &m, &s, &ms) != 7)
        return false;
    const char* p = std::strchr(line, ',');
    if (!p) return false;
    double v1 = std::strtod(p + 1, nullptr);
    p = std::strchr(p + 1, ',');
    if (!p) return false;
    double v2 = std::strtod(p + 1, nullptr);
    if (v1 <= 0.0 || v2 <= 0.0) return false;

    // HistData timestamps are EST (UTC-5)
    struct tm t{};
    t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D;
    t.tm_hour = h; t.tm_min = m; t.tm_sec = s;
    int64_t utc_s = portable_timegm(&t) + 5 * 3600; // EST -> UTC
    out.ts_ms = utc_s * 1000LL + ms;
    out.bid   = v1;
    out.ask   = v2;
    if (out.bid > out.ask) std::swap(out.bid, out.ask);
    return true;
}

static bool parse_duka_bid_ask(const char* line, RawTick& out) {
    char* e = nullptr;
    int64_t ts = std::strtoll(line, &e, 10);
    if (*e != ',') return false;
    double v1 = std::strtod(e + 1, &e);
    if (*e != ',') return false;
    double v2 = std::strtod(e + 1, nullptr);
    if (v1 <= 0.0 || v2 <= 0.0) return false;
    out.ts_ms = ts;
    out.bid   = v1;
    out.ask   = v2;
    if (out.bid > out.ask) std::swap(out.bid, out.ask);
    return true;
}

static bool parse_duka_ask_bid(const char* line, RawTick& out) {
    char* e = nullptr;
    int64_t ts = std::strtoll(line, &e, 10);
    if (*e != ',') return false;
    double v1 = std::strtod(e + 1, &e); // ask
    if (*e != ',') return false;
    double v2 = std::strtod(e + 1, nullptr); // bid
    if (v1 <= 0.0 || v2 <= 0.0) return false;
    out.ts_ms = ts;
    out.ask   = v1;
    out.bid   = v2;
    if (out.bid > out.ask) std::swap(out.bid, out.ask);
    return true;
}

static bool parse_jforex(const char* line, RawTick& out) {
    // DD.MM.YYYY HH:MM:SS.mmm,Ask,Bid,...
    int D, M, Y, h, m, s, ms;
    if (std::sscanf(line, "%2d.%2d.%4d %2d:%2d:%2d.%3d", &D, &M, &Y, &h, &m, &s, &ms) != 7)
        return false;
    // Find first comma after timestamp
    const char* p = std::strchr(line, ',');
    if (!p) return false;
    double ask_val = std::strtod(p + 1, nullptr);
    p = std::strchr(p + 1, ',');
    if (!p) return false;
    double bid_val = std::strtod(p + 1, nullptr);
    if (ask_val <= 0.0 || bid_val <= 0.0) return false;

    // JForex is EET (UTC+2)
    struct tm t{};
    t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D;
    t.tm_hour = h; t.tm_min = m; t.tm_sec = s;
    int64_t utc_s = portable_timegm(&t) - 2 * 3600; // EET -> UTC
    out.ts_ms = utc_s * 1000LL + ms;
    out.bid   = bid_val;
    out.ask   = ask_val;
    if (out.bid > out.ask) std::swap(out.bid, out.ask);
    return true;
}

using ParseFn = bool(*)(const char*, RawTick&);

static ParseFn get_parser(TickFmt fmt) {
    switch (fmt) {
        case TickFmt::HISTDATA:     return parse_histdata;
        case TickFmt::DUKA_BID_ASK: return parse_duka_bid_ask;
        case TickFmt::DUKA_ASK_BID: return parse_duka_ask_bid;
        case TickFmt::JFOREX:       return parse_jforex;
        default: return nullptr;
    }
}

static const char* fmt_name(TickFmt f) {
    switch (f) {
        case TickFmt::HISTDATA:     return "HISTDATA";
        case TickFmt::DUKA_BID_ASK: return "DUKA_BID_ASK";
        case TickFmt::DUKA_ASK_BID: return "DUKA_ASK_BID";
        case TickFmt::JFOREX:       return "JFOREX";
        default: return "UNKNOWN";
    }
}

// =============================================================================
// WEEKEND GATE
// =============================================================================
static bool is_weekend(int64_t ts_ms) noexcept {
    const int64_t sec = ts_ms / 1000;
    const int dow = ((sec / 86400) + 4) % 7; // 0=Sun, 6=Sat
    if (dow == 0 || dow == 6) return true;
    if (dow == 5) {
        const int hour = static_cast<int>((sec % 86400) / 3600);
        if (hour >= 21) return true;
    }
    return false;
}

// =============================================================================
// INSTRUMENT CONFIG
// =============================================================================
struct InstrumentConfig {
    const char* name;
    const char* symbol;
    double lot_size;
    double pnl_per_pt;
    int    session_open_h;
    int    session_open_m;
    int    session_close_h;
    int    session_close_m;
    double price_lo;
    double price_hi;
};

static InstrumentConfig g_configs[] = {
    { "SP", "US500.F",  0.01, 0.50, 13, 30, 21, 30, 3000.0, 8000.0 },
    { "NQ", "USTEC.F",  0.01, 0.20, 13, 30, 21, 30, 10000.0, 25000.0 },
};

static const InstrumentConfig* find_config(const char* inst) {
    for (auto& c : g_configs) {
        if (std::strcmp(c.name, inst) == 0) return &c;
    }
    return nullptr;
}

// =============================================================================
// SWEEP CONFIG
// =============================================================================
struct SweepConfig {
    double atr_mult;
    double tp_pct;
    double sl_pct;
    double loss_cut_pct;

    // Fixed params (not swept)
    int    lookback_ticks  = 300;
    double vwap_stop_mult  = 0.5;
    int    max_hold_sec    = 2700;
    int    cooldown_sec    = 600;
    double max_spread_pct  = 0.05;
    double min_band_pct    = 0.08;
    double max_band_pct    = 2.00;
    int    warmup_ticks    = 120;
    double be_arm_pct      = 0.04;
    double be_buffer_pct   = 0.015;
};

// =============================================================================
// PERFORMANCE METRICS
// =============================================================================
struct Metrics {
    int    trades       = 0;
    int    wins         = 0;
    int    longs        = 0;
    int    shorts       = 0;
    double gross_win    = 0.0;
    double gross_loss   = 0.0;
    double total_pnl    = 0.0;
    double peak_eq      = 0.0;
    double max_dd       = 0.0;
    std::vector<double> pnl_vec;

    // Exit counts
    int exit_tp     = 0;
    int exit_sl     = 0;
    int exit_lc     = 0;  // LOSS_CUT
    int exit_be     = 0;  // BE_CUT
    int exit_vwap   = 0;  // VWAP_STOP
    int exit_timeout= 0;

    // Per-hour PF
    std::array<double, 24> hour_win{};
    std::array<double, 24> hour_loss{};
    std::array<int, 24>    hour_n{};

    // Long/short split
    double long_pnl  = 0.0;
    double short_pnl = 0.0;
    int    long_wins = 0, long_losses = 0;
    int    short_wins= 0, short_losses= 0;

    void record(double pnl, bool is_long, int entry_hour, const char* exit_reason) {
        trades++;
        total_pnl += pnl;
        pnl_vec.push_back(pnl);
        if (pnl > 0) { wins++; gross_win += pnl; }
        else         { gross_loss += std::fabs(pnl); }
        if (total_pnl > peak_eq) peak_eq = total_pnl;
        double dd = peak_eq - total_pnl;
        if (dd > max_dd) max_dd = dd;

        if (is_long) {
            longs++;
            long_pnl += pnl;
            if (pnl > 0) long_wins++; else long_losses++;
        } else {
            shorts++;
            short_pnl += pnl;
            if (pnl > 0) short_wins++; else short_losses++;
        }

        int h = entry_hour % 24;
        hour_n[h]++;
        if (pnl > 0) hour_win[h] += pnl;
        else          hour_loss[h] += std::fabs(pnl);

        if (std::strcmp(exit_reason, "TP_HIT") == 0)       exit_tp++;
        else if (std::strcmp(exit_reason, "SL_HIT") == 0)  exit_sl++;
        else if (std::strcmp(exit_reason, "LOSS_CUT") == 0) exit_lc++;
        else if (std::strcmp(exit_reason, "BE_CUT") == 0)   exit_be++;
        else if (std::strcmp(exit_reason, "VWAP_STOP") == 0)exit_vwap++;
        else if (std::strcmp(exit_reason, "TIMEOUT") == 0)  exit_timeout++;
    }

    double pf() const { return gross_loss > 0 ? gross_win / gross_loss : (gross_win > 0 ? 999.0 : 0.0); }
    double wr() const { return trades > 0 ? 100.0 * wins / trades : 0.0; }
    double avg() const { return trades > 0 ? total_pnl / trades : 0.0; }
};

// =============================================================================
// NoiseBandMomentum STRATEGY (self-contained replica)
// =============================================================================
struct NbmState {
    // Position
    bool   pos_active   = false;
    bool   pos_is_long  = true;
    double pos_entry    = 0.0;
    double pos_tp       = 0.0;
    double pos_sl       = 0.0;
    double pos_mfe      = 0.0;
    double pos_mae      = 0.0;
    double pos_spread   = 0.0;
    int64_t pos_entry_ts= 0;
    bool   be_locked    = false;
    bool   tp_extended  = false;
    double init_tp_dist = 0.0;
    bool   be_ratchet_armed = false;

    // Daily state
    double session_open = 0.0;
    int    last_yday    = -1;
    int    tick_count   = 0;

    // ATR from 1-minute bars (not tick-level)
    static constexpr int ATR_BUF_SZ = 600;
    double atr_buf[ATR_BUF_SZ] = {};
    int    atr_head  = 0;
    int    atr_count = 0;
    double prev_mid  = 0.0;

    // 1-minute bar aggregation for ATR
    int64_t bar_open_ms  = 0;
    double  bar_high     = 0.0;
    double  bar_low      = 99999999.0;
    double  bar_close    = 0.0;
    double  prev_bar_close = 0.0;
    bool    bar_active   = false;

    // VWAP
    double vwap_cum_pv  = 0.0;
    double vwap_cum_vol = 0.0;
    double vwap         = 0.0;

    // Entry context
    double vwap_at_entry   = 0.0;
    double band_half_entry = 0.0;
    bool   is_long_entry   = true;
    int64_t entry_time     = 0;
    int64_t cooldown_until = 0;
    int    entry_hour      = 0;

    void reset_daily(double mid) {
        session_open   = mid;
        tick_count     = 0;
        // Don't reset ATR buffer on daily reset — ATR carries across days
        prev_mid       = 0.0;
        vwap_cum_pv    = 0.0;
        vwap_cum_vol   = 0.0;
        vwap           = mid;
        vwap_at_entry  = 0.0;
        band_half_entry= 0.0;
        // Reset bar state for new day
        bar_active     = false;
    }

    // Feed tick into 1-minute bar aggregation; push ATR on bar close
    void update_atr(double mid, int64_t ts_ms, int lookback) {
        prev_mid = mid;
        if (!bar_active) {
            bar_open_ms = ts_ms;
            bar_high = mid;
            bar_low  = mid;
            bar_close = mid;
            bar_active = true;
            return;
        }
        // Update current bar
        if (mid > bar_high) bar_high = mid;
        if (mid < bar_low)  bar_low  = mid;
        bar_close = mid;
        // Check if 1-minute bar is complete
        if (ts_ms - bar_open_ms >= 60000) {
            // Compute true range
            double tr = bar_high - bar_low;
            if (prev_bar_close > 0.0) {
                tr = std::max({tr,
                               std::fabs(bar_high - prev_bar_close),
                               std::fabs(bar_low  - prev_bar_close)});
            }
            atr_buf[atr_head % ATR_BUF_SZ] = tr;
            atr_head++;
            if (atr_count < lookback) atr_count++;
            prev_bar_close = bar_close;
            // Start new bar
            bar_open_ms = ts_ms;
            bar_high = mid;
            bar_low  = mid;
            bar_close = mid;
        }
    }

    void update_vwap(double mid) {
        vwap_cum_pv  += mid;
        vwap_cum_vol += 1.0;
        if (vwap_cum_vol > 0.0) vwap = vwap_cum_pv / vwap_cum_vol;
    }

    double rolling_atr(int lookback) const {
        if (atr_count < 2) return 0.0;
        double sum = 0.0;
        int n = std::min(atr_count, lookback);
        for (int i = 0; i < n; i++)
            sum += atr_buf[i % ATR_BUF_SZ];
        return sum / n;
    }

    void reset_position() {
        pos_active   = false;
        pos_mfe      = 0.0;
        pos_mae      = 0.0;
        be_locked    = false;
        tp_extended  = false;
        init_tp_dist = 0.0;
        be_ratchet_armed = false;
    }
};

// Callback for trade close
struct TradeResult {
    bool   is_long;
    double entry;
    double exit_px;
    double pnl;
    double mfe;
    double mae;
    int64_t entry_ts;
    int64_t exit_ts;
    int    entry_hour;
    const char* exit_reason;
};

// Run one config over the tick stream, return IS and OOS metrics
static void run_config(
    const std::vector<RawTick>& ticks,
    const SweepConfig& cfg,
    const InstrumentConfig& inst,
    int64_t oos_split_ts,
    Metrics& is_met,
    Metrics& oos_met,
    Metrics& all_met)
{
    NbmState st;

    const int open_mins  = inst.session_open_h  * 60 + inst.session_open_m;
    const int close_mins = inst.session_close_h * 60 + inst.session_close_m;

    auto close_trade = [&](double exit_px, const char* reason, int64_t now_s) {
        double pnl_pts = st.pos_is_long ? (exit_px - st.pos_entry) : (st.pos_entry - exit_px);
        double pnl_usd = pnl_pts * inst.pnl_per_pt;

        all_met.record(pnl_usd, st.pos_is_long, st.entry_hour, reason);
        if (st.pos_entry_ts < oos_split_ts)
            is_met.record(pnl_usd, st.pos_is_long, st.entry_hour, reason);
        else
            oos_met.record(pnl_usd, st.pos_is_long, st.entry_hour, reason);

        st.cooldown_until = now_s + cfg.cooldown_sec;
        st.reset_position();
    };

    for (size_t i = 0; i < ticks.size(); i++) {
        const auto& tk = ticks[i];
        if (is_weekend(tk.ts_ms)) continue;

        const double bid = tk.bid;
        const double ask = tk.ask;
        const double mid = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const int64_t now_s = tk.ts_ms / 1000;

        // Price sanity
        if (mid < inst.price_lo || mid > inst.price_hi) continue;

        // UTC time decomposition
        int64_t day_sec = now_s % 86400;
        int hour = (int)(day_sec / 3600);
        int minute = (int)((day_sec % 3600) / 60);
        int mins = hour * 60 + minute;
        int yday = (int)((now_s / 86400 + 4) / 1); // approximate; use proper
        // Proper yday computation
        time_t tt = (time_t)now_s;
        struct tm utc_tm;
#if defined(__APPLE__) || defined(__linux__)
        gmtime_r(&tt, &utc_tm);
#else
        gmtime_s(&utc_tm, &tt);
#endif
        yday = utc_tm.tm_yday + utc_tm.tm_year * 366; // unique day id

        // Daily reset
        if (yday != st.last_yday) {
            // Force close any open position at day boundary
            if (st.pos_active) {
                close_trade(mid, "TIMEOUT", now_s);
            }
            st.reset_daily(mid);
            st.last_yday = yday;
        }

        // ---- MANAGE OPEN POSITION ----
        if (st.pos_active) {
            st.update_vwap(mid);

            const double move = st.pos_is_long ? (mid - st.pos_entry) : (st.pos_entry - mid);
            if (move > st.pos_mfe) st.pos_mfe = move;
            if (-move > st.pos_mae) st.pos_mae = -move;

            // Phase 1: BE RATCHET
            if (cfg.be_arm_pct > 0.0 && cfg.be_buffer_pct >= 0.0) {
                double arm_pts    = st.pos_entry * cfg.be_arm_pct / 100.0;
                double buffer_pts = st.pos_entry * cfg.be_buffer_pct / 100.0;
                if (st.pos_mfe >= arm_pts && move <= buffer_pts) {
                    close_trade(mid, "BE_CUT", now_s);
                    continue;
                }
            }

            // Phase 2: LOSS_CUT
            if (cfg.loss_cut_pct > 0.0) {
                double adverse = -move;
                double loss_cut_dist = st.pos_entry * cfg.loss_cut_pct / 100.0;
                if (adverse >= loss_cut_dist) {
                    close_trade(mid, "LOSS_CUT", now_s);
                    continue;
                }
            }

            // Phase 3: VWAP STOP (after 60s hold)
            int64_t age = now_s - st.entry_time;
            if (age > 60 && st.vwap > 0.0 && st.band_half_entry > 0.0) {
                double tol = st.band_half_entry * cfg.vwap_stop_mult;
                bool vwap_stop = st.is_long_entry
                    ? (mid < st.vwap - tol)
                    : (mid > st.vwap + tol);
                if (vwap_stop) {
                    close_trade(mid, "VWAP_STOP", now_s);
                    continue;
                }
            }

            // Phase 4: Winner exemption for timeout
            int eff_max_hold = (move > 0.0)
                ? std::numeric_limits<int>::max()
                : cfg.max_hold_sec;

            // Phase 5: CrossPosition base manage (BE lock + trail + TP/SL + timeout)
            double tp_dist = st.tp_extended ? st.init_tp_dist : std::fabs(st.pos_tp - st.pos_entry);
            if (!st.tp_extended) st.init_tp_dist = tp_dist;

            if (tp_dist > 0.0) {
                double be_threshold    = tp_dist * 0.40;
                double mid_threshold   = tp_dist * 0.50;
                double trail_threshold = tp_dist * 0.60;
                double trail_dist      = tp_dist * 0.20;

                // BE lock at 40%
                if (move >= be_threshold && !st.be_locked) {
                    st.be_locked = true;
                    double be_sl = st.pos_is_long
                        ? (st.pos_entry + st.pos_spread)
                        : (st.pos_entry - st.pos_spread);
                    if (st.pos_is_long  && be_sl > st.pos_sl) st.pos_sl = be_sl;
                    if (!st.pos_is_long && be_sl < st.pos_sl) st.pos_sl = be_sl;
                }

                // Mid-lock at 50%
                if (move >= mid_threshold && st.be_locked) {
                    double mid_lock = st.pos_is_long
                        ? (st.pos_entry + tp_dist * 0.25)
                        : (st.pos_entry - tp_dist * 0.25);
                    if (st.pos_is_long  && mid_lock > st.pos_sl) st.pos_sl = mid_lock;
                    if (!st.pos_is_long && mid_lock < st.pos_sl) st.pos_sl = mid_lock;
                }

                // Trail at 60%
                if (move >= trail_threshold) {
                    double trail_sl = st.pos_is_long
                        ? (st.pos_entry + st.pos_mfe - trail_dist)
                        : (st.pos_entry - st.pos_mfe + trail_dist);
                    if (st.pos_is_long  && trail_sl > st.pos_sl) st.pos_sl = trail_sl;
                    if (!st.pos_is_long && trail_sl < st.pos_sl) st.pos_sl = trail_sl;
                }
            }

            // TP hit with extension
            bool tp_hit = st.pos_is_long ? (bid >= st.pos_tp) : (ask <= st.pos_tp);
            if (tp_hit && !st.tp_extended && tp_dist > 0.0) {
                st.tp_extended = true;
                st.pos_tp = st.pos_is_long ? (st.pos_tp + tp_dist) : (st.pos_tp - tp_dist);
                double tight_sl = st.pos_is_long
                    ? (st.pos_entry + st.pos_mfe - tp_dist * 0.15)
                    : (st.pos_entry - st.pos_mfe + tp_dist * 0.15);
                if (st.pos_is_long  && tight_sl > st.pos_sl) st.pos_sl = tight_sl;
                if (!st.pos_is_long && tight_sl < st.pos_sl) st.pos_sl = tight_sl;
                continue; // ride continuation
            }

            // Check final TP (after extension)
            tp_hit = st.pos_is_long ? (bid >= st.pos_tp) : (ask <= st.pos_tp);
            bool sl_hit = st.pos_is_long ? (bid <= st.pos_sl) : (ask >= st.pos_sl);
            bool timed_out = (age >= eff_max_hold) && (move <= 0.0);

            if (tp_hit) {
                close_trade(st.pos_tp, "TP_HIT", now_s);
            } else if (sl_hit) {
                close_trade(st.pos_sl, "SL_HIT", now_s);
            } else if (timed_out) {
                close_trade(mid, "TIMEOUT", now_s);
            }
            continue;
        }

        // ---- ENTRY LOGIC ----

        // Session filter
        if (mins < open_mins || mins >= close_mins) continue;

        // Update ATR and VWAP
        st.update_atr(mid, tk.ts_ms, cfg.lookback_ticks);
        st.update_vwap(mid);
        st.tick_count++;

        // Warmup
        if (st.tick_count < cfg.warmup_ticks) continue;

        // Spread filter
        if (mid > 0.0 && spread / mid * 100.0 > cfg.max_spread_pct) continue;

        // Cooldown
        if (now_s < st.cooldown_until) continue;

        // Session open must be set
        if (st.session_open <= 0.0) continue;

        // Compute ATR bands
        double atr = st.rolling_atr(cfg.lookback_ticks);
        if (atr <= 0.0) continue;
        double band_half = atr * cfg.atr_mult;
        double band_pct  = (mid > 0.0) ? (band_half / mid * 100.0) : 0.0;

        if (band_pct < cfg.min_band_pct || band_pct > cfg.max_band_pct) continue;

        double upper_band = st.session_open + band_half;
        double lower_band = st.session_open - band_half;

        bool go_long  = (mid > upper_band);
        bool go_short = (mid < lower_band);
        if (!go_long && !go_short) continue;

        // VWAP alignment filter (after warmup+60 ticks)
        if (st.vwap > 0.0 && st.tick_count > cfg.warmup_ticks + 60) {
            double vwap_vs_open = st.vwap - st.session_open;
            if (go_long  && vwap_vs_open < -band_half * 0.3) continue;
            if (go_short && vwap_vs_open >  band_half * 0.3) continue;
        }

        // Open position
        double entry_px = go_long ? ask : bid;
        double tp_dist_px = entry_px * cfg.tp_pct / 100.0;
        double sl_dist_px = entry_px * cfg.sl_pct / 100.0;
        double tp = go_long ? entry_px + tp_dist_px : entry_px - tp_dist_px;
        double sl = go_long ? entry_px - sl_dist_px : entry_px + sl_dist_px;

        st.pos_active    = true;
        st.pos_is_long   = go_long;
        st.pos_entry     = entry_px;
        st.pos_tp        = tp;
        st.pos_sl        = sl;
        st.pos_mfe       = 0.0;
        st.pos_mae       = 0.0;
        st.pos_spread    = spread;
        st.pos_entry_ts  = tk.ts_ms;
        st.be_locked     = false;
        st.tp_extended   = false;
        st.init_tp_dist  = 0.0;
        st.vwap_at_entry = st.vwap > 0.0 ? st.vwap : mid;
        st.band_half_entry = band_half;
        st.is_long_entry = go_long;
        st.entry_time    = now_s;
        st.entry_hour    = hour;
        st.cooldown_until= now_s + cfg.cooldown_sec;
    }

    // Force close any open position at end
    if (st.pos_active && !ticks.empty()) {
        double mid = (ticks.back().bid + ticks.back().ask) * 0.5;
        close_trade(mid, "TIMEOUT", ticks.back().ts_ms / 1000);
    }
}

// =============================================================================
// REPORT PRINTER
// =============================================================================
static void print_one_liner(int idx, const SweepConfig& cfg, const Metrics& is_m, const Metrics& oos_m) {
    std::printf("  #%02d ATR=%.1f TP=%.2f SL=%.2f LC=%.2f | "
                "IS: t=%d PF=%.2f WR=%.0f%% $%.2f | "
                "OOS: t=%d PF=%.2f WR=%.0f%% $%.2f\n",
                idx, cfg.atr_mult, cfg.tp_pct, cfg.sl_pct, cfg.loss_cut_pct,
                is_m.trades, is_m.pf(), is_m.wr(), is_m.total_pnl,
                oos_m.trades, oos_m.pf(), oos_m.wr(), oos_m.total_pnl);
}

static void print_full_report(const SweepConfig& cfg, const Metrics& all,
                               const Metrics& is_m, const Metrics& oos_m,
                               const InstrumentConfig& inst) {
    std::printf("\n================================================================\n");
    std::printf("  BEST OOS CONFIG -- %s NoiseBandMomentum\n", inst.name);
    std::printf("================================================================\n");
    std::printf("  ATR_MULT=%.1f  TP_PCT=%.2f  SL_PCT=%.2f  LOSS_CUT_PCT=%.2f\n",
                cfg.atr_mult, cfg.tp_pct, cfg.sl_pct, cfg.loss_cut_pct);
    std::printf("  BE_ARM=%.3f  BE_BUF=%.3f  VWAP_STOP_MULT=%.1f  MAX_HOLD=%ds\n",
                cfg.be_arm_pct, cfg.be_buffer_pct, cfg.vwap_stop_mult, cfg.max_hold_sec);
    std::printf("----------------------------------------------------------------\n");

    // Overall
    std::printf("  OVERALL : trades=%d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  avg=$%.4f  maxDD=$%.2f\n",
                all.trades, all.wr(), all.pf(), all.total_pnl, all.avg(), all.max_dd);
    std::printf("  LONG    : trades=%d  WR=%.1f%%  PnL=$%.2f\n",
                all.longs, all.longs > 0 ? 100.0 * all.long_wins / all.longs : 0.0, all.long_pnl);
    std::printf("  SHORT   : trades=%d  WR=%.1f%%  PnL=$%.2f\n",
                all.shorts, all.shorts > 0 ? 100.0 * all.short_wins / all.shorts : 0.0, all.short_pnl);

    // Exit breakdown
    std::printf("  Exit: TP=%d  SL=%d  LOSS_CUT=%d  BE_CUT=%d  VWAP_STOP=%d  TIMEOUT=%d\n",
                all.exit_tp, all.exit_sl, all.exit_lc, all.exit_be, all.exit_vwap, all.exit_timeout);

    // Per-hour PF
    std::printf("  Per-hour PF: ");
    for (int h = 0; h < 24; h++) {
        if (all.hour_n[h] > 0) {
            double hpf = all.hour_loss[h] > 0 ? all.hour_win[h] / all.hour_loss[h] : 999.0;
            std::printf("%02d=%.2f(%d) ", h, hpf, all.hour_n[h]);
        }
    }
    std::printf("\n");

    // IS/OOS
    std::printf("  IS     : trades=%d  PF=%.2f  WR=%.1f%%  PnL=$%.2f  maxDD=$%.2f\n",
                is_m.trades, is_m.pf(), is_m.wr(), is_m.total_pnl, is_m.max_dd);
    std::printf("  OOS    : trades=%d  PF=%.2f  WR=%.1f%%  PnL=$%.2f  maxDD=$%.2f\n",
                oos_m.trades, oos_m.pf(), oos_m.wr(), oos_m.total_pnl, oos_m.max_dd);

    // Decay check
    if (is_m.pf() > 0.0 && oos_m.pf() > 0.0) {
        double decay = 1.0 - oos_m.pf() / is_m.pf();
        std::printf("  Decay  : %.1f%% (OOS PF / IS PF)\n", decay * 100.0);
    }

    // Verdict
    bool pass = oos_m.pf() >= 1.20 && oos_m.trades >= 20;
    std::printf("  VERDICT: %s (OOS PF=%.2f, OOS trades=%d)\n",
                pass ? "PASS" : "FAIL", oos_m.pf(), oos_m.trades);
    std::printf("================================================================\n\n");
}

// =============================================================================
// MAIN
// =============================================================================
int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::fprintf(stderr,
            "Usage: %s --instrument <SP|NQ> <tick_csv> [tick_csv2 ...]\n\n"
            "Tick formats auto-detected: HISTDATA, DUKA_BID_ASK, DUKA_ASK_BID, JFOREX\n"
            "Sweep: ATR_MULT x TP_PCT x SL_PCT x LOSS_CUT_PCT = 54 configs\n",
            argv[0]);
        return 1;
    }

    // Parse args
    const char* inst_name = nullptr;
    std::vector<std::string> csv_paths;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--instrument") == 0 && i + 1 < argc) {
            inst_name = argv[++i];
        } else {
            csv_paths.push_back(argv[i]);
        }
    }

    if (!inst_name) {
        std::fprintf(stderr, "[ERROR] --instrument required (SP or NQ)\n");
        return 1;
    }

    const InstrumentConfig* inst = find_config(inst_name);
    if (!inst) {
        std::fprintf(stderr, "[ERROR] Unknown instrument: %s (use SP or NQ)\n", inst_name);
        return 1;
    }

    if (csv_paths.empty()) {
        std::fprintf(stderr, "[ERROR] No CSV files provided\n");
        return 1;
    }

    // ---- LOAD TICKS ----
    std::printf("================================================================\n");
    std::printf("  IndexNoiseBandMomentum Backtest -- %s (%s)\n", inst->name, inst->symbol);
    std::printf("  Files: %zu\n", csv_paths.size());
    std::printf("================================================================\n");

    std::vector<RawTick> all_ticks;
    all_ticks.reserve(50'000'000); // pre-alloc for large datasets

    for (auto& path : csv_paths) {
        std::ifstream f(path);
        if (!f.is_open()) {
            std::fprintf(stderr, "[WARN] Cannot open: %s\n", path.c_str());
            continue;
        }

        // Detect format from first few lines
        TickFmt fmt = TickFmt::UNKNOWN;
        std::string line;
        std::vector<std::string> header_lines;

        // Read up to 5 lines to find format
        while (header_lines.size() < 5 && std::getline(f, line)) {
            if (line.empty()) continue;
            // Strip trailing \r
            if (!line.empty() && line.back() == '\r') line.pop_back();
            header_lines.push_back(line);

            TickFmt detected = detect_format(line);
            if (detected != TickFmt::UNKNOWN) {
                fmt = detected;
                break;
            }
        }

        if (fmt == TickFmt::UNKNOWN) {
            // Try DUKA_ASK_BID as fallback for 2-comma numeric lines
            for (auto& hl : header_lines) {
                int commas = 0;
                for (char c : hl) if (c == ',') commas++;
                if (commas == 2 && std::isdigit((unsigned char)hl[0])) {
                    fmt = TickFmt::DUKA_ASK_BID;
                    break;
                }
            }
        }

        if (fmt == TickFmt::UNKNOWN) {
            std::fprintf(stderr, "[WARN] Cannot detect format for: %s\n", path.c_str());
            continue;
        }

        ParseFn parser = get_parser(fmt);
        if (!parser) continue;

        std::printf("  Loading %s (format: %s)...\n", path.c_str(), fmt_name(fmt));

        // Parse buffered header lines first
        size_t pre = all_ticks.size();
        RawTick tk;
        for (auto& hl : header_lines) {
            if (parser(hl.c_str(), tk)) {
                double mid = (tk.bid + tk.ask) * 0.5;
                if (mid >= inst->price_lo && mid <= inst->price_hi)
                    all_ticks.push_back(tk);
            }
        }

        // Parse rest of file
        size_t line_count = header_lines.size();
        while (std::getline(f, line)) {
            line_count++;
            if (line.empty()) continue;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (parser(line.c_str(), tk)) {
                double mid = (tk.bid + tk.ask) * 0.5;
                if (mid >= inst->price_lo && mid <= inst->price_hi)
                    all_ticks.push_back(tk);
            }
            if (line_count % 10'000'000 == 0) {
                std::printf("    ... %zuM lines read, %zuM ticks loaded\n",
                            line_count / 1'000'000, all_ticks.size() / 1'000'000);
            }
        }

        std::printf("    %zu ticks from this file (%zu total)\n",
                    all_ticks.size() - pre, all_ticks.size());
    }

    if (all_ticks.empty()) {
        std::fprintf(stderr, "[ERROR] No valid ticks loaded\n");
        return 1;
    }

    // Sort by timestamp
    std::sort(all_ticks.begin(), all_ticks.end(),
              [](const RawTick& a, const RawTick& b) { return a.ts_ms < b.ts_ms; });

    // Compute IS/OOS split (60/40)
    int64_t first_ts = all_ticks.front().ts_ms;
    int64_t last_ts  = all_ticks.back().ts_ms;
    int64_t range_ms = last_ts - first_ts;
    int64_t oos_split_ts = first_ts + (int64_t)(range_ms * 0.60);

    // Count IS/OOS ticks
    size_t is_ticks = 0;
    for (auto& t : all_ticks) {
        if (t.ts_ms < oos_split_ts) is_ticks++;
    }

    {
        time_t t1 = (time_t)(first_ts / 1000);
        time_t t2 = (time_t)(last_ts / 1000);
        time_t ts = (time_t)(oos_split_ts / 1000);
        struct tm tm1, tm2, tms;
#if defined(__APPLE__) || defined(__linux__)
        gmtime_r(&t1, &tm1); gmtime_r(&t2, &tm2); gmtime_r(&ts, &tms);
#else
        gmtime_s(&tm1, &t1); gmtime_s(&tm2, &t2); gmtime_s(&tms, &ts);
#endif
        char buf1[32], buf2[32], bufs[32];
        std::strftime(buf1, sizeof(buf1), "%Y-%m-%d", &tm1);
        std::strftime(buf2, sizeof(buf2), "%Y-%m-%d", &tm2);
        std::strftime(bufs, sizeof(bufs), "%Y-%m-%d", &tms);

        std::printf("  Total ticks: %zu  |  Range: %s to %s\n", all_ticks.size(), buf1, buf2);
        std::printf("  IS/OOS split: %s  |  IS=%zuM  OOS=%zuM\n",
                    bufs, is_ticks / 1'000'000, (all_ticks.size() - is_ticks) / 1'000'000);
    }

    // ---- BUILD SWEEP CONFIGS ----
    double atr_mults[]    = { 1.0, 1.5, 2.0 };
    double tp_pcts[]      = { 0.40, 0.60, 0.80 };
    double sl_pcts[]      = { 0.15, 0.25, 0.35 };
    double loss_cut_pcts[]= { 0.0, 0.06 };

    std::vector<SweepConfig> configs;
    for (double am : atr_mults)
        for (double tp : tp_pcts)
            for (double sl : sl_pcts)
                for (double lc : loss_cut_pcts) {
                    SweepConfig c;
                    c.atr_mult     = am;
                    c.tp_pct       = tp;
                    c.sl_pct       = sl;
                    c.loss_cut_pct = lc;
                    configs.push_back(c);
                }

    std::printf("\n  Sweep: %zu configs  (ATR_MULT x TP_PCT x SL_PCT x LOSS_CUT_PCT)\n\n",
                configs.size());

    // ---- RUN SWEEP ----
    struct SweepResult {
        int idx;
        SweepConfig cfg;
        Metrics is_met, oos_met, all_met;
    };
    std::vector<SweepResult> results(configs.size());

    for (size_t i = 0; i < configs.size(); i++) {
        results[i].idx = (int)i;
        results[i].cfg = configs[i];
        run_config(all_ticks, configs[i], *inst, oos_split_ts,
                   results[i].is_met, results[i].oos_met, results[i].all_met);
        print_one_liner((int)i, configs[i], results[i].is_met, results[i].oos_met);

        if ((i + 1) % 10 == 0) {
            std::printf("  ... %zu/%zu configs done\n", i + 1, configs.size());
        }
    }

    // ---- FIND BEST OOS ----
    int best_idx = -1;
    double best_oos_pf = 0.0;
    for (size_t i = 0; i < results.size(); i++) {
        auto& r = results[i];
        if (r.oos_met.trades >= 5 && r.oos_met.pf() > best_oos_pf) {
            best_oos_pf = r.oos_met.pf();
            best_idx = (int)i;
        }
    }

    if (best_idx >= 0) {
        auto& r = results[best_idx];
        print_full_report(r.cfg, r.all_met, r.is_met, r.oos_met, *inst);
    } else {
        std::printf("\n  [WARN] No config had >= 5 OOS trades.\n");
    }

    // ---- SUMMARY TABLE ----
    std::printf("\n================================================================\n");
    std::printf("  SWEEP SUMMARY (sorted by OOS PF, trades >= 5)\n");
    std::printf("================================================================\n");
    std::printf("  %-4s %-5s %-5s %-5s %-5s | %-5s %-5s %-7s | %-5s %-5s %-7s | VERDICT\n",
                "#", "ATR", "TP", "SL", "LC", "IS_PF", "IS_WR", "IS_PnL",
                "OOS_PF", "OOS_WR", "OOS_PnL");
    std::printf("  %-4s %-5s %-5s %-5s %-5s | %-5s %-5s %-7s | %-5s %-5s %-7s | -------\n",
                "----", "-----", "-----", "-----", "-----",
                "-----", "-----", "-------", "-----", "-----", "-------");

    // Sort by OOS PF descending
    std::vector<int> order(results.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return results[a].oos_met.pf() > results[b].oos_met.pf();
    });

    for (int idx : order) {
        auto& r = results[idx];
        if (r.oos_met.trades < 5 && r.is_met.trades < 5) continue;
        bool pass = r.oos_met.pf() >= 1.20 && r.oos_met.trades >= 20;
        std::printf("  %3d  %4.1f  %4.2f  %4.2f  %4.2f | %5.2f %4.0f%% %7.2f | %5.2f %4.0f%% %7.2f | %s\n",
                    idx, r.cfg.atr_mult, r.cfg.tp_pct, r.cfg.sl_pct, r.cfg.loss_cut_pct,
                    r.is_met.pf(), r.is_met.wr(), r.is_met.total_pnl,
                    r.oos_met.pf(), r.oos_met.wr(), r.oos_met.total_pnl,
                    pass ? "PASS" : "FAIL");
    }

    std::printf("================================================================\n");
    std::printf("  Done. %zu ticks processed, %zu configs swept.\n",
                all_ticks.size(), configs.size());

    return 0;
}

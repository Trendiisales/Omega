// =============================================================================
// cbe_walk_forward.cpp -- CompressionBreakoutEngine REENTER_COMP gate sweep
//
// PURPOSE:
//   Replays XAUUSD L2 tick CSVs through a faithful reproduction of CBE's entry
//   and management logic, while varying the REENTER_COMP exit-gate parameters
//   via #ifdef CBE_SWEEP_OVERRIDE_* guards. Emits one CSV row per closed trade.
//
// INVOKED BY:
//   run_cbe_sweep.ps1 -- passes CLI args + define flags per cell.
//
// NOT PRODUCTION:
//   This file is backtest-only. It duplicates CBE logic rather than including
//   the live engine so sweep overrides don't risk touching production state.
//   Source of truth remains include/CompressionBreakoutEngine.hpp.
//
// CSV INPUT FORMATS (auto-detected by header):
//   v1 (pre 2026-04-22): ts_ms,bid,ask,l2_imb,l2_bid_vol,l2_ask_vol,
//                        depth_bid_levels,depth_ask_levels,depth_events_total,
//                        watchdog_dead,vol_ratio,regime,vpin,has_pos,
//                        micro_edge,ewm_drift
//   v2 (from 2026-04-22): ts_ms,mid,bid,ask,l2_imb,l2_bid_vol,l2_ask_vol,
//                         depth_bid_levels,depth_ask_levels,depth_events_total,
//                         watchdog_dead,vol_ratio,regime,vpin,has_pos,
//                         micro_edge,ewm_drift
//
// CLI:
//   cbe_walk_forward <out_csv> <input_csv_1> [input_csv_2 ...]
//   Inputs must be in chronological order. Engine state is NOT reset between
//   files (daily_pnl resets via _daily_reset based on ts_ms date rollover).
//
// BUILD (MSVC):
//   cl /std:c++17 /O2 /EHsc /W4 /WX cbe_walk_forward.cpp
//     /DCBE_SWEEP_REENTER_TOL=0.25 /DCBE_SWEEP_REENTER_NEEDS_BE=1
//     /DCBE_SWEEP_MIN_HOLD_MS=15000
//
// BUILD (g++ for local dev):
//   g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic cbe_walk_forward.cpp
//     -DCBE_SWEEP_REENTER_TOL=0.25 -DCBE_SWEEP_REENTER_NEEDS_BE=1
//     -DCBE_SWEEP_MIN_HOLD_MS=15000 -o cbe_walk_forward
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// =============================================================================
// Sweep overrides -- defaults match CompressionBreakoutEngine.hpp production
// =============================================================================

// Offset from compression range edge (pts). Production = 0.10.
// Larger = exit gate triggers only on deeper retracement.
#ifndef CBE_SWEEP_REENTER_TOL
#define CBE_SWEEP_REENTER_TOL 0.10
#endif

// 1 = require !pos.be_done (production). 0 = require pos.mfe < 0.0 (dead gate).
// With value 0 the gate is effectively disabled because mfe is monotonically
// non-decreasing (never below 0).
#ifndef CBE_SWEEP_REENTER_NEEDS_BE
#define CBE_SWEEP_REENTER_NEEDS_BE 1
#endif

// Minimum hold before REENTER_COMP can fire (ms). Production = 5000.
#ifndef CBE_SWEEP_MIN_HOLD_MS
#define CBE_SWEEP_MIN_HOLD_MS 5000
#endif

// If defined, disables REENTER_COMP gate entirely (baseline cell).
// #define CBE_SWEEP_REENTER_DISABLED

// =============================================================================
// Production CBE constants -- mirror include/CompressionBreakoutEngine.hpp
// =============================================================================
static constexpr int     CBE_COMP_BARS            = 3;
static constexpr double  CBE_COMP_RANGE_MULT      = 1.50;
static constexpr int     CBE_COMP_MIN_BARS        = 3;
static constexpr double  CBE_BREAK_FRAC           = 0.30;
static constexpr double  CBE_TP_RR                = 1.5;
static constexpr double  CBE_MAX_SL_ATR_MULT      = 4.0;
static constexpr double  CBE_TRAIL_ARM_FRAC       = 0.50;
static constexpr double  CBE_TRAIL_DIST_FRAC      = 0.40;
static constexpr double  CBE_BE_FRAC              = 0.40;
static constexpr int64_t CBE_TIMEOUT_MS           = 180000;
static constexpr int64_t CBE_COOLDOWN_MS          = 30000;
static constexpr double  CBE_RSI_BLOCK_OB         = 72.0;
static constexpr double  CBE_RSI_BLOCK_OS         = 22.0;
static constexpr double  CBE_COMMISSION_RT        = 0.20;
static constexpr double  CBE_RISK_DOLLARS         = 30.0;
static constexpr double  CBE_MIN_LOT              = 0.01;
static constexpr double  CBE_MAX_LOT              = 0.01;
static constexpr int64_t CBE_STARTUP_LOCKOUT_MS   = 90000;
static constexpr bool    CBE_BLOCK_LONDON_NY_LONG = true;

// =============================================================================
// Tick struct
// =============================================================================
struct Tick {
    int64_t ts_ms   = 0;
    double  bid     = 0.0;
    double  ask     = 0.0;
    double  ewm_drift = 0.0;
};

// =============================================================================
// M1 bar folder -- produces OHLC on UTC-minute boundaries
// =============================================================================
struct M1Bar {
    int64_t close_ms = 0;
    double  open = 0.0, high = 0.0, low = 0.0, close = 0.0;
    bool    valid = false;
};

struct BarFolder {
    int64_t cur_min   = -1;   // UTC minute index (ts_ms / 60000)
    double  o = 0, h = 0, l = 0, c = 0;
    bool    any = false;

    // Returns true if the incoming tick closed a bar; the closed bar is
    // written to *out_bar.
    bool add(int64_t ts_ms, double mid, M1Bar* out_bar) {
        const int64_t m = ts_ms / 60000;
        if (cur_min < 0) {
            cur_min = m;
            o = h = l = c = mid;
            any = true;
            return false;
        }
        if (m != cur_min) {
            // Close previous bar
            out_bar->close_ms = cur_min * 60000 + 59999;
            out_bar->open  = o;
            out_bar->high  = h;
            out_bar->low   = l;
            out_bar->close = c;
            out_bar->valid = true;
            // Start new bar
            cur_min = m;
            o = h = l = c = mid;
            return true;
        }
        if (mid > h) h = mid;
        if (mid < l) l = mid;
        c = mid;
        return false;
    }
};

// =============================================================================
// ATR14 / RSI14 -- M1 incremental
//
// ATR14: Wilder's smoothing. TR = max(high-low, |high-prev_close|, |low-prev_close|).
// RSI14: Wilder's smoothing on avg gains / avg losses of M1 closes.
// =============================================================================
struct AtrRsi {
    // ATR
    double atr14 = 0.0;
    double prev_close = 0.0;
    int    bar_count = 0;
    double tr_sum = 0.0;

    // RSI
    double rsi14 = 50.0;
    double avg_gain = 0.0;
    double avg_loss = 0.0;

    void on_bar_close(const M1Bar& b) {
        // TR
        const double tr = (bar_count == 0)
            ? (b.high - b.low)
            : std::max({ b.high - b.low,
                         std::fabs(b.high - prev_close),
                         std::fabs(b.low  - prev_close) });

        // ATR14 seed over first 14 bars (simple average), then Wilder
        if (bar_count < 14) {
            tr_sum += tr;
            if (bar_count == 13) atr14 = tr_sum / 14.0;
        } else {
            atr14 = (atr14 * 13.0 + tr) / 14.0;
        }

        // RSI14
        if (bar_count > 0) {
            const double delta = b.close - prev_close;
            const double gain = (delta > 0) ? delta : 0.0;
            const double loss = (delta < 0) ? -delta : 0.0;
            if (bar_count <= 14) {
                avg_gain += gain;
                avg_loss += loss;
                if (bar_count == 14) {
                    avg_gain /= 14.0;
                    avg_loss /= 14.0;
                }
            } else {
                avg_gain = (avg_gain * 13.0 + gain) / 14.0;
                avg_loss = (avg_loss * 13.0 + loss) / 14.0;
            }
            if (avg_loss < 1e-9) {
                rsi14 = 100.0;
            } else {
                const double rs = avg_gain / avg_loss;
                rsi14 = 100.0 - 100.0 / (1.0 + rs);
            }
        }

        prev_close = b.close;
        ++bar_count;
    }
};

// =============================================================================
// Session slot -- matches the mapping used by CBE comments (verified against
// observed session_slot=2 at 11:31 UTC in production log).
//   0         = dead zone (21-22 UTC rollover / weekend)
//   1-2       = London (07-12 UTC)    [slot 1 = 07-09, slot 2 = 10-12]
//   3-5       = NY     (13-20 UTC)    [slot 3 = 13-15, slot 4 = 16-18, slot 5 = 19-20]
//   6         = Asia   (22-06 UTC)
//
// NOTE: This is a best-effort reconstruction from CBE's comment block. The
// production session_slot is computed elsewhere in Omega; if this differs from
// live slot allocation, the entry gate (slots 1-5) may include or exclude
// different hours. The sweep's interpretation gate requires results to hold on
// BOTH train and test splits, so moderate miscalibration of slot boundaries
// will not produce a spurious pass.
// =============================================================================
static int session_slot_from_ts(int64_t ts_ms) {
    const int64_t utc_sec = ts_ms / 1000;
    const int hour = (int)((utc_sec / 3600) % 24);
    if (hour >= 7  && hour <= 9)  return 1;
    if (hour >= 10 && hour <= 12) return 2;
    if (hour >= 13 && hour <= 15) return 3;
    if (hour >= 16 && hour <= 18) return 4;
    if (hour >= 19 && hour <= 20) return 5;
    if (hour == 21) return 0;                      // dead zone
    return 6;                                      // Asia (22-06)
}

// =============================================================================
// CSV parser -- auto-detects v1 vs v2 format by header
// =============================================================================
struct CsvParser {
    std::ifstream in;
    int idx_ts = -1, idx_bid = -1, idx_ask = -1, idx_drift = -1;
    std::string path;

    bool open(const std::string& p) {
        path = p;
        in.open(p);
        if (!in.is_open()) return false;
        std::string header;
        if (!std::getline(in, header)) return false;
        std::vector<std::string> cols;
        std::stringstream ss(header);
        std::string tok;
        while (std::getline(ss, tok, ',')) cols.push_back(tok);
        for (size_t i = 0; i < cols.size(); ++i) {
            // Trim any trailing \r
            while (!cols[i].empty() && (cols[i].back() == '\r' || cols[i].back() == '\n'))
                cols[i].pop_back();
            if (cols[i] == "ts_ms")     idx_ts    = (int)i;
            if (cols[i] == "bid")       idx_bid   = (int)i;
            if (cols[i] == "ask")       idx_ask   = (int)i;
            if (cols[i] == "ewm_drift") idx_drift = (int)i;
        }
        if (idx_ts < 0 || idx_bid < 0 || idx_ask < 0 || idx_drift < 0) {
            std::cerr << "[ERROR] Missing required column in " << p
                      << " (ts_ms=" << idx_ts << " bid=" << idx_bid
                      << " ask=" << idx_ask << " drift=" << idx_drift << ")\n";
            return false;
        }
        return true;
    }

    bool next(Tick* out) {
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> cols;
            cols.reserve(20);
            std::stringstream ss(line);
            std::string tok;
            while (std::getline(ss, tok, ',')) cols.push_back(tok);
            const int max_idx = std::max({idx_ts, idx_bid, idx_ask, idx_drift});
            if ((int)cols.size() <= max_idx) continue;   // malformed row, skip
            try {
                out->ts_ms     = std::stoll(cols[idx_ts]);
                out->bid       = std::stod(cols[idx_bid]);
                out->ask       = std::stod(cols[idx_ask]);
                out->ewm_drift = std::stod(cols[idx_drift]);
            } catch (...) {
                continue;   // non-numeric row, skip
            }
            if (out->bid <= 0.0 || out->ask <= 0.0) continue;
            if (out->ask < out->bid) continue;   // bad tick
            return true;
        }
        return false;
    }
};

// =============================================================================
// Harness trade record -- lightweight subset of omega::TradeRecord
// =============================================================================
struct HarnessTrade {
    int      id = 0;
    int64_t  entry_ts_ms = 0;
    int64_t  exit_ts_ms  = 0;
    bool     is_long = false;
    double   entry = 0, exit_px = 0, sl = 0, tp = 0;
    double   comp_hi = 0, comp_lo = 0;
    double   size = 0, mfe = 0;
    double   pnl_pts = 0, pnl_usd = 0;
    double   spread_at_entry = 0;
    double   atr_at_entry = 0;
    double   rsi_at_entry = 0;
    int64_t  held_ms = 0;
    int      session_slot = 0;
    std::string exit_reason;
};

// =============================================================================
// CBE replay engine -- faithful to CompressionBreakoutEngine.hpp
// =============================================================================
struct CbeHarness {
    // Open position
    struct OpenPos {
        bool    active = false;
        bool    is_long = false;
        double  entry = 0, sl = 0, tp = 0, size = 0, mfe = 0;
        double  comp_range_hi = 0, comp_range_lo = 0;
        double  atr_at_entry = 0;
        double  rsi_at_entry = 0;
        bool    be_done = false;
        bool    trail_armed = false;
        double  trail_sl = 0;
        int64_t entry_ts_ms = 0;
        int     trade_id = 0;
        double  spread_at_entry = 0;
        int     session_slot = 0;
    } pos;

    // Bar state
    std::deque<double> bar_hi, bar_lo, bar_cl;
    int    consec_comp = 0;
    bool   armed = false;
    bool   break_detected = false;
    double comp_hi = 0, comp_lo = 0;
    int64_t comp_armed_ms = 0;
    double atr_cur = 0, rsi_cur = 50.0;
    int64_t bars_total = 0;

    // Session / PnL
    double  daily_pnl = 0;
    int64_t daily_day = 0;
    int64_t last_exit_ms = 0;
    int     consec_losses = 0;
    int     total_trades = 0;
    int     total_wins = 0;
    int     trade_id = 0;
    int64_t startup_ms = 0;

    // Output trades accumulator (pointer, owned by caller)
    std::vector<HarnessTrade>* trades = nullptr;

    bool has_open_position() const { return pos.active; }

    void on_bar(const M1Bar& b, double atr14, double rsi14, int64_t now_ms) {
        atr_cur = (atr14 > 0.5) ? atr14 : atr_cur;
        rsi_cur = rsi14;

        bar_hi.push_back(b.high);
        bar_lo.push_back(b.low);
        bar_cl.push_back(b.close);
        if ((int)bar_hi.size() > CBE_COMP_BARS) {
            bar_hi.pop_front();
            bar_lo.pop_front();
            bar_cl.pop_front();
        }
        ++bars_total;

        if ((int)bar_hi.size() < 2) return;

        const double whi = *std::max_element(bar_hi.begin(), bar_hi.end());
        const double wlo = *std::min_element(bar_lo.begin(), bar_lo.end());
        const double wrange = whi - wlo;
        const double comp_thresh = atr_cur * CBE_COMP_RANGE_MULT;
        const bool in_comp = (atr_cur > 0.0) && (wrange < comp_thresh);

        if (in_comp) {
            ++consec_comp;
            if (consec_comp >= CBE_COMP_MIN_BARS) {
                armed = true;
                comp_hi = whi;
                comp_lo = wlo;
                comp_armed_ms = now_ms;
            }
        } else {
            consec_comp = 0;
            if (!break_detected) armed = false;
        }
    }

    void on_tick(double bid, double ask, double ewm_drift,
                 int session_slot, int64_t now_ms) {
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        daily_reset(now_ms);

        if (startup_ms == 0) startup_ms = now_ms;
        if (now_ms - startup_ms < CBE_STARTUP_LOCKOUT_MS) return;

        if (pos.active) { manage(bid, ask, mid, now_ms); return; }

        // Entry guards
        if (!armed) return;
        if (atr_cur <= 0.0) return;
        if (now_ms - last_exit_ms < CBE_COOLDOWN_MS) return;
        if (spread > atr_cur * 0.30) return;

        // Consec-loss kill (CBE-10)
        if (consec_losses >= 3) {
            const int64_t since = now_ms - last_exit_ms;
            if (since < 1800000LL) return;
            consec_losses = 0;
        }

        // Daily loss cap
        if (daily_pnl <= -150.0) return;

        // Session gate
        if (session_slot < 1 || session_slot > 5) return;

        // Break detection
        const double break_margin = atr_cur * CBE_BREAK_FRAC;
        const bool long_break  = (bid >= comp_hi + break_margin);
        const bool short_break = (ask <= comp_lo - break_margin);
        if (!long_break && !short_break) return;

        const bool is_long = long_break;

        // EWM drift gate
        if (is_long  && ewm_drift <= 0.0) return;
        if (!is_long && ewm_drift >= 0.0) return;

        // CVD divergence -- SKIPPED IN SWEEP (both divs treated as false).
        // Decision documented in session log: including CVD would require
        // synthesizing from L2 imbalance slope, introducing a second unknown.
        // Sweep isolates REENTER_COMP gate by holding all other gates constant.

        // RSI exhaustion
        if (is_long  && rsi_cur > CBE_RSI_BLOCK_OB) return;
        if (!is_long && rsi_cur < CBE_RSI_BLOCK_OS) return;

        // London/NY LONG block
        if (CBE_BLOCK_LONDON_NY_LONG && is_long &&
            session_slot >= 1 && session_slot <= 5) return;

        // SL geometry
        const double sl_buffer = 0.50;
        const double sl_dist = is_long
            ? (ask - (comp_lo - sl_buffer))
            : ((comp_hi + sl_buffer) - bid);

        if (sl_dist > atr_cur * CBE_MAX_SL_ATR_MULT) return;
        if (sl_dist <= 0.0) return;

        // Cost coverage
        const double tp_pts = sl_dist * CBE_TP_RR;
        const double cost   = spread + CBE_COMMISSION_RT;
        if (tp_pts <= cost) return;

        enter(is_long, bid, ask, spread, sl_dist, tp_pts, session_slot, now_ms);
    }

    void enter(bool is_long, double bid, double ask, double spread,
               double sl_dist, double tp_pts, int session_slot,
               int64_t now_ms) {
        const double entry_px = is_long ? ask : bid;
        const double sl_px    = is_long ? (entry_px - sl_dist) : (entry_px + sl_dist);
        const double tp_px    = is_long ? (entry_px + tp_pts)  : (entry_px - tp_pts);

        const double sl_safe = std::max(0.10, sl_dist);
        double size = CBE_RISK_DOLLARS / (sl_safe * 100.0);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(CBE_MIN_LOT, std::min(CBE_MAX_LOT, size));

        ++trade_id;

        pos.active         = true;
        pos.is_long        = is_long;
        pos.entry          = entry_px;
        pos.sl             = sl_px;
        pos.tp             = tp_px;
        pos.size           = size;
        pos.mfe            = 0.0;
        pos.comp_range_hi  = comp_hi;
        pos.comp_range_lo  = comp_lo;
        pos.atr_at_entry   = atr_cur;
        pos.rsi_at_entry   = rsi_cur;
        pos.be_done        = false;
        pos.trail_armed    = false;
        pos.trail_sl       = sl_px;
        pos.entry_ts_ms    = now_ms;
        pos.trade_id       = trade_id;
        pos.spread_at_entry = spread;
        pos.session_slot   = session_slot;

        armed = false;
        break_detected = true;
        consec_comp = 0;
        comp_hi = 0;
        comp_lo = 0;
        comp_armed_ms = 0;
        bar_hi.clear();
        bar_lo.clear();
        bar_cl.clear();
    }

    void manage(double bid, double ask, double mid, int64_t now_ms) {
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;

        const double eff_price = pos.is_long ? bid : ask;
        const double tp_dist   = std::fabs(pos.tp - pos.entry);

        // BE
        if (!pos.be_done && tp_dist > 0.0 && pos.mfe >= tp_dist * CBE_BE_FRAC) {
            pos.sl = pos.entry;
            pos.trail_sl = pos.entry;
            pos.be_done = true;
        }

        // Trail
        if (!pos.trail_armed && tp_dist > 0.0 && pos.mfe >= tp_dist * CBE_TRAIL_ARM_FRAC) {
            pos.trail_armed = true;
        }
        if (pos.trail_armed) {
            const double trail_dist = pos.atr_at_entry * CBE_TRAIL_DIST_FRAC;
            const double new_trail  = pos.is_long ? (mid - trail_dist) : (mid + trail_dist);
            if (pos.is_long  && new_trail > pos.trail_sl) pos.trail_sl = new_trail;
            if (!pos.is_long && new_trail < pos.trail_sl) pos.trail_sl = new_trail;
            if (pos.is_long  && pos.trail_sl > pos.sl) pos.sl = pos.trail_sl;
            if (!pos.is_long && pos.trail_sl < pos.sl) pos.sl = pos.trail_sl;
        }

        // REENTER_COMP gate -- parameterized for sweep
#ifndef CBE_SWEEP_REENTER_DISABLED
        const double tol = (double)(CBE_SWEEP_REENTER_TOL);
        const bool reenter_comp = pos.is_long
            ? (bid < pos.comp_range_hi - tol)
            : (ask > pos.comp_range_lo + tol);

        bool be_gate_ok;
#if CBE_SWEEP_REENTER_NEEDS_BE == 1
        be_gate_ok = !pos.be_done;
#else
        be_gate_ok = (pos.mfe < 0.0);   // equivalent to "never fires"
#endif

        if (reenter_comp && be_gate_ok &&
            (now_ms - pos.entry_ts_ms) > (int64_t)(CBE_SWEEP_MIN_HOLD_MS)) {
            close(eff_price, "REENTER_COMP", now_ms);
            return;
        }
#endif

        // TP
        if (pos.is_long ? (bid >= pos.tp) : (ask <= pos.tp)) {
            close(eff_price, "TP_HIT", now_ms);
            return;
        }
        // SL
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (sl_hit) {
            const char* reason = pos.trail_armed ? "TRAIL_HIT"
                               : pos.be_done     ? "BE_HIT"
                               :                   "SL_HIT";
            close(eff_price, reason, now_ms);
            return;
        }
        // Timeout
        if (now_ms - pos.entry_ts_ms > CBE_TIMEOUT_MS) {
            close(eff_price, "TIMEOUT", now_ms);
            return;
        }
    }

    void close(double exit_px, const char* reason, int64_t now_ms) {
        const double pnl_pts = pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px);
        const double pnl_usd = pnl_pts * pos.size * 100.0;

        daily_pnl    += pnl_usd;
        last_exit_ms  = now_ms;
        ++total_trades;
        const bool win = (pnl_usd > 0);
        if (win) { ++total_wins; consec_losses = 0; }
        else     { ++consec_losses; }

        if (trades) {
            HarnessTrade t;
            t.id              = pos.trade_id;
            t.entry_ts_ms     = pos.entry_ts_ms;
            t.exit_ts_ms      = now_ms;
            t.is_long         = pos.is_long;
            t.entry           = pos.entry;
            t.exit_px         = exit_px;
            t.sl              = pos.sl;
            t.tp              = pos.tp;
            t.comp_hi         = pos.comp_range_hi;
            t.comp_lo         = pos.comp_range_lo;
            t.size            = pos.size;
            t.mfe             = pos.mfe;
            t.pnl_pts         = pnl_pts;
            t.pnl_usd         = pnl_usd;
            t.spread_at_entry = pos.spread_at_entry;
            t.atr_at_entry    = pos.atr_at_entry;
            t.rsi_at_entry    = pos.rsi_at_entry;
            t.held_ms         = now_ms - pos.entry_ts_ms;
            t.session_slot    = pos.session_slot;
            t.exit_reason     = reason;
            trades->push_back(t);
        }

        break_detected = false;
        pos = OpenPos{};
    }

    void daily_reset(int64_t now_ms) {
        const int64_t day = (now_ms / 1000LL) / 86400LL;
        if (day != daily_day) {
            daily_pnl = 0.0;
            daily_day = day;
        }
    }
};

// =============================================================================
// Main
// =============================================================================
int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <out_csv> <input_csv_1> [input_csv_2 ...]\n";
        return 2;
    }

    const std::string out_path = argv[1];

    std::vector<HarnessTrade> trades;
    trades.reserve(1000);

    CbeHarness cbe;
    cbe.trades = &trades;

    BarFolder folder;
    AtrRsi atr_rsi;

    // Process each file in order
    for (int i = 2; i < argc; ++i) {
        CsvParser p;
        if (!p.open(argv[i])) {
            std::cerr << "[ERROR] Cannot open " << argv[i] << "\n";
            return 3;
        }
        Tick t;
        int64_t count = 0;
        while (p.next(&t)) {
            const double mid = (t.bid + t.ask) * 0.5;

            // Fold into bars; on bar close, update ATR/RSI and feed CBE bar.
            M1Bar closed_bar;
            closed_bar.valid = false;
            if (folder.add(t.ts_ms, mid, &closed_bar) && closed_bar.valid) {
                atr_rsi.on_bar_close(closed_bar);
                cbe.on_bar(closed_bar, atr_rsi.atr14, atr_rsi.rsi14,
                           closed_bar.close_ms);
            }

            // Tick-level processing
            const int slot = session_slot_from_ts(t.ts_ms);
            cbe.on_tick(t.bid, t.ask, t.ewm_drift, slot, t.ts_ms);
            ++count;
        }
        std::cerr << "[INFO] " << argv[i] << ": " << count << " ticks, "
                  << trades.size() << " cumulative trades\n";
    }

    // Write trade CSV
    std::ofstream out(out_path);
    if (!out.is_open()) {
        std::cerr << "[ERROR] Cannot write " << out_path << "\n";
        return 4;
    }
    out << "id,entry_ts_ms,exit_ts_ms,side,entry,exit,sl,tp,comp_hi,comp_lo,"
        << "size,mfe,pnl_pts,pnl_usd,spread_at_entry,atr_at_entry,rsi_at_entry,"
        << "held_ms,session_slot,exit_reason\n";
    for (const auto& t : trades) {
        out << t.id << ","
            << t.entry_ts_ms << ","
            << t.exit_ts_ms << ","
            << (t.is_long ? "LONG" : "SHORT") << ","
            << std::fixed << std::setprecision(3) << t.entry << ","
            << t.exit_px << ","
            << t.sl << ","
            << t.tp << ","
            << t.comp_hi << ","
            << t.comp_lo << ","
            << std::setprecision(4) << t.size << ","
            << std::setprecision(3) << t.mfe << ","
            << t.pnl_pts << ","
            << std::setprecision(2) << t.pnl_usd << ","
            << std::setprecision(3) << t.spread_at_entry << ","
            << t.atr_at_entry << ","
            << std::setprecision(1) << t.rsi_at_entry << ","
            << t.held_ms << ","
            << t.session_slot << ","
            << t.exit_reason << "\n";
    }
    out.close();

    std::cerr << "[DONE] " << trades.size() << " trades -> " << out_path << "\n";

    // Quick summary to stderr
    int wins = 0;
    double net = 0;
    for (const auto& t : trades) {
        if (t.pnl_usd > 0) ++wins;
        net += t.pnl_usd;
    }
    const double wr = trades.empty() ? 0.0 : (100.0 * wins / (double)trades.size());
    std::cerr << "[SUMMARY] T=" << trades.size()
              << " W=" << wins
              << " WR=" << std::fixed << std::setprecision(1) << wr << "%"
              << " net=$" << std::setprecision(2) << net << "\n";

    return 0;
}

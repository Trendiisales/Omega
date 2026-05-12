// =============================================================================
// midscalper_crtp_sweep.cpp -- CRTP-style sweep harness for GoldMidScalperEngine
// =============================================================================
//
// 2026-05-12 (Claude / Jo): Standalone sweep harness mirroring the
//   microscalper_crtp_sweep.cpp pattern. Sweeps 5 cadence parameters across a
//   faithful port of the live GoldMidScalperEngine logic.
//
//   The live engine in include/GoldMidScalperEngine.hpp is NOT modified.
//   This TU defines a parallel variant whose body is a faithful port of the
//   live engine's tick logic. NOTE: the per-combo CRTP fold-expression dispatch
//   used by microscalper_crtp_sweep.cpp was tested here but exhausted the
//   sandbox build memory (3.8GB) when instantiating 490 engine types with
//   compile-time params. To preserve the harness's intent (sweep many configs
//   over the same tape stream) without blowing up the compiler, the swept
//   params are held in a per-instance runtime Traits struct, and the engine
//   body compiles ONCE. Equivalent semantics, much smaller compile cost.
//
//   The live MidScalperGoldTRAIL engine just produced a +$2.37 net scalp in
//   17 seconds on XAUUSD. Operator wants hundreds of trades/day but the
//   current cooldown stack (COOLDOWN_S=180, SAME_LEVEL post-win=600,
//   post-SL=900) gates that hard. This sweep measures how relaxing those
//   gates (plus MIN_ENTRY_TICKS and MAX_SPREAD) trades off frequency vs PF.
//
// SWEPT PARAMS (5)
//   p0  COOLDOWN_S                   -- post-close cooldown gate (live 180s)
//   p1  SAME_LEVEL_POST_WIN_BLOCK_S  -- post-win same-level block (live 600s)
//   p2  SAME_LEVEL_POST_SL_BLOCK_S   -- post-SL  same-level block (live 900s)
//   p3  MIN_ENTRY_TICKS              -- warmup tick gate (live 30)
//   p4  MAX_SPREAD                   -- entry-gate spread cap (live 2.5pt)
//
// GRIDS (7 values each, centred on the live value at index 3)
//   COOLDOWN_S                  -> {30,60,90,180,270,360,540}
//   SAME_LEVEL_POST_WIN_BLOCK_S -> {60,180,300,600,900,1200,1800}
//   SAME_LEVEL_POST_SL_BLOCK_S  -> {120,300,600,900,1200,1800,2700}
//   MIN_ENTRY_TICKS             -> {10,15,20,30,45,60,90}
//   MAX_SPREAD                  -> {1.0,1.5,2.0,2.5,3.0,3.5,5.0}
//
// HELD FIXED (cross-config invariants ported from the live engine)
//   STRUCTURE_LOOKBACK=300, MIN_BREAK_TICKS=5, MIN_RANGE=8.0, MAX_RANGE=20.0,
//   SL_FRAC=0.6, SL_BUFFER=1.0, TP_RR=4.0, TRAIL_FRAC=0.25,
//   MIN_TRAIL_ARM_PTS=5.0, MIN_TRAIL_ARM_SECS=15, MFE_TRAIL_FRAC=0.55,
//   BE_TRIGGER_PTS=3.0, BE_OFFSET_PTS=2.5, SAME_LEVEL_BLOCK_PTS=8.0,
//   MAX_FILL_SPREAD = 2*MAX_SPREAD, PENDING_TIMEOUT_S=120, SESSION 06-22 UTC,
//   EXPANSION_HISTORY_LEN=20, EXPANSION_MIN_HISTORY=5, EXPANSION_MULT=1.10,
//   USD_PER_PT=100, LIVE_LOT=0.01.
//
//   DOM-based lot sizing is simplified to fixed lot=0.01 (sweep is about
//   CADENCE not lot sizing). L2 imbalance is used as a tiebreaker when a
//   wide tick straddles both bracket edges in a single bar.
//
// PAIRWISE 2-FACTOR COVERAGE
//   5 choose 2 = 10 pairs * 49 (=7*7) combos = 490 combos total.
//
// USAGE
//   ./midscalper_sweep data/l2_ticks_XAUUSD_*.csv \
//       [--warmup N] [--out PATH] [--top N] [--verbose]
//
// BUILD
//   clang++ -std=c++17 -O3 -DNDEBUG -fbracket-depth=1024 -I include \
//           backtest/midscalper_crtp_sweep.cpp -o /tmp/midscalper_sweep
//   g++ -std=c++17 -O3 -DNDEBUG -I include \
//       backtest/midscalper_crtp_sweep.cpp -o /tmp/midscalper_sweep
// =============================================================================

#define OMEGA_BACKTEST 1

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "../include/OmegaTradeLedger.hpp"

namespace omega::midscalper_sweep {

// -----------------------------------------------------------------------------
// Pairwise 2-factor sweep grid: 5 params * 7 values pairwise.
// -----------------------------------------------------------------------------
inline constexpr int N_PARAMS  = 5;
inline constexpr int N_PAIRS   = 10;        // 5 choose 2
inline constexpr int PAIR_GRID = 7;
inline constexpr int N_COMBOS  = N_PAIRS * PAIR_GRID * PAIR_GRID;  // 490

inline constexpr std::pair<int,int> PAIR_TABLE[10] = {
    {0,1}, {0,2}, {0,3}, {0,4},
    {1,2}, {1,3}, {1,4},
    {2,3}, {2,4},
    {3,4},
};
constexpr int combo_pair(int I) { return I / 49; }
constexpr int combo_ix_a(int I) { return (I % 49) / 7; }
constexpr int combo_ix_b(int I) { return (I % 49) % 7; }

inline constexpr double COOLDOWN_GRID[7]        = {  30.0,  60.0,  90.0, 180.0, 270.0, 360.0, 540.0 };
inline constexpr double POST_WIN_GRID[7]        = {  60.0, 180.0, 300.0, 600.0, 900.0,1200.0,1800.0 };
inline constexpr double POST_SL_GRID[7]         = { 120.0, 300.0, 600.0, 900.0,1200.0,1800.0,2700.0 };
inline constexpr double MIN_ENTRY_TICKS_GRID[7] = {  10.0,  15.0,  20.0,  30.0,  45.0,  60.0,  90.0 };
inline constexpr double MAX_SPREAD_GRID[7]      = {   1.0,   1.5,   2.0,   2.5,   3.0,   3.5,   5.0 };

inline constexpr int LIVE_IX = 3;  // live value sits at index 3 in every grid

constexpr double grid_value(int p, int ix) {
    if (p == 0) return COOLDOWN_GRID[ix];
    if (p == 1) return POST_WIN_GRID[ix];
    if (p == 2) return POST_SL_GRID[ix];
    if (p == 3) return MIN_ENTRY_TICKS_GRID[ix];
    return MAX_SPREAD_GRID[ix];
}

inline double value_for_combo(int I, int p) {
    const int pi = combo_pair(I);
    const auto& pp = PAIR_TABLE[pi];
    if (pp.first  == p) return grid_value(p, combo_ix_a(I));
    if (pp.second == p) return grid_value(p, combo_ix_b(I));
    return grid_value(p, LIVE_IX);
}

// -----------------------------------------------------------------------------
// Traits -- per-instance runtime values for the 5 swept params. (The original
// design held these compile-time via Traits<I>; flipped to runtime after
// instantiating 490 engine types exhausted the 3.8GB sandbox memory at
// compile. Behaviour identical.)
// -----------------------------------------------------------------------------
struct MidScalperTraits {
    int    COOLDOWN_S;
    int    SAME_LEVEL_POST_WIN_BLOCK_S;
    int    SAME_LEVEL_POST_SL_BLOCK_S;
    int    MIN_ENTRY_TICKS;
    double MAX_SPREAD;
};

inline MidScalperTraits traits_for_combo(int I) {
    MidScalperTraits t{};
    t.COOLDOWN_S                  = static_cast<int>(value_for_combo(I, 0));
    t.SAME_LEVEL_POST_WIN_BLOCK_S = static_cast<int>(value_for_combo(I, 1));
    t.SAME_LEVEL_POST_SL_BLOCK_S  = static_cast<int>(value_for_combo(I, 2));
    t.MIN_ENTRY_TICKS             = static_cast<int>(value_for_combo(I, 3));
    t.MAX_SPREAD                  = value_for_combo(I, 4);
    return t;
}

// -----------------------------------------------------------------------------
// MidScalperEngine -- faithful port of GoldMidScalperEngine::on_tick.
//   - Range-structure detection (STRUCTURE_LOOKBACK=300 mid window)
//   - Phase state machine IDLE -> ARMED -> PENDING -> LIVE -> COOLDOWN
//   - Inside-ticks confirmation (MIN_BREAK_TICKS=5)
//   - ATR-expansion gate (S47 T4a; cold-start guard S58)
//   - DOM lot sizing simplified to fixed lot=0.01 (sweep is about cadence)
//   - Fill simulation: ask>=bracket_high => LONG, bid<=bracket_low => SHORT
//   - manage(): BE lock, MFE-trail with MIN_TRAIL_ARM guards, SL/TP/BE/TRAIL classify
//   - same-level blocks (post-SL on entry, post-WIN on exit)
//   - Session 06-22 UTC gate
//   - L2 imbalance as tiebreaker: when both fills would simultaneously trigger
//     in a single wide tick, L2 imbalance picks the side.
// -----------------------------------------------------------------------------
class MidScalperEngine {
public:
    // Held-fixed constants (mirror live engine) -----------------------------
    static constexpr int    STRUCTURE_LOOKBACK    = 300;
    static constexpr int    MIN_BREAK_TICKS       = 5;
    static constexpr double MIN_RANGE             = 8.0;
    static constexpr double MAX_RANGE             = 20.0;
    static constexpr double SL_FRAC               = 0.6;
    static constexpr double SL_BUFFER             = 1.0;
    static constexpr double TP_RR                 = 4.0;
    static constexpr double TRAIL_FRAC            = 0.25;
    static constexpr double MIN_TRAIL_ARM_PTS     = 5.0;
    static constexpr int    MIN_TRAIL_ARM_SECS    = 15;
    static constexpr double MFE_TRAIL_FRAC        = 0.55;
    static constexpr double BE_TRIGGER_PTS        = 3.0;
    static constexpr double BE_OFFSET_PTS         = 2.5;
    static constexpr double SAME_LEVEL_BLOCK_PTS  = 8.0;
    static constexpr int    PENDING_TIMEOUT_S     = 120;
    static constexpr int    SESSION_START_HOUR_UTC = 6;
    static constexpr int    SESSION_END_HOUR_UTC   = 22;
    static constexpr int    EXPANSION_HISTORY_LEN = 20;
    static constexpr int    EXPANSION_MIN_HISTORY = 5;
    static constexpr double EXPANSION_MULT        = 1.10;
    static constexpr double USD_PER_PT            = 100.0;
    static constexpr double LIVE_LOT              = 0.01;

    enum class Phase { IDLE, ARMED, PENDING, LIVE, COOLDOWN };

    MidScalperTraits T{};
    Phase phase = Phase::IDLE;

    struct LivePos {
        bool    active   = false;
        bool    is_long  = false;
        double  entry    = 0.0;
        double  tp       = 0.0;
        double  sl       = 0.0;
        double  size     = 0.01;
        double  mfe      = 0.0;
        double  mae      = 0.0;
        double  spread_at_entry = 0.0;
        int64_t entry_ts = 0;
        bool    be_locked = false;
    } pos;

    double bracket_high = 0.0;
    double bracket_low  = 0.0;
    double range        = 0.0;

    // Shared per-tick window stats (computed ONCE by the sweep driver across
    // all 490 engines; the rolling mid window is independent of any swept
    // parameter, so duplicating the max/min work per-engine was the bottleneck
    // that pushed a single-tape smoke run past 45s wallclock).
    template <typename Sink>
    void on_tick(double bid, double ask, int64_t now_ms,
                 double l2_imbalance,
                 int window_count, double w_hi_shared, double w_lo_shared,
                 Sink& sink) noexcept
    {
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const int64_t now_s = now_ms / 1000;

        ++m_ticks_received;

        // -- COOLDOWN -------------------------------------------------------
        if (phase == Phase::COOLDOWN) {
            if (now_s - m_cooldown_start >= T.COOLDOWN_S) phase = Phase::IDLE;
            else return;
        }

        // -- LIVE: manage position ------------------------------------------
        if (phase == Phase::LIVE) {
            _manage(bid, ask, mid, now_s, sink);
            return;
        }

        // -- PENDING: wait for fill -----------------------------------------
        if (phase == Phase::PENDING) {
            const bool would_fill_long  = (ask >= bracket_high);
            const bool would_fill_short = (bid <= bracket_low);
            const double max_fill_spread = 2.0 * T.MAX_SPREAD;

            if ((would_fill_long || would_fill_short) && spread > max_fill_spread) {
                phase = Phase::IDLE;
                bracket_high = bracket_low = range = 0.0;
                return;
            }
            if (would_fill_long && would_fill_short) {
                // L2 imbalance tiebreaker.
                const bool prefer_long = (l2_imbalance >= 0.5);
                if (prefer_long) _confirm_fill(true,  bracket_high, LIVE_LOT, spread, now_s);
                else             _confirm_fill(false, bracket_low,  LIVE_LOT, spread, now_s);
                return;
            }
            if (would_fill_long)  { _confirm_fill(true,  bracket_high, LIVE_LOT, spread, now_s); return; }
            if (would_fill_short) { _confirm_fill(false, bracket_low,  LIVE_LOT, spread, now_s); return; }

            if (now_s - m_armed_ts > PENDING_TIMEOUT_S) {
                phase = Phase::IDLE;
                bracket_high = bracket_low = range = 0.0;
            }
            return;
        }

        // -- IDLE / ARMED entry gates ---------------------------------------
        if (m_ticks_received < T.MIN_ENTRY_TICKS) return;
        if (window_count < STRUCTURE_LOOKBACK) return;
        if (spread > T.MAX_SPREAD) return;

        // Session window 06-22 UTC (wraparound-aware as in live).
        {
            const std::time_t t = static_cast<std::time_t>(now_s);
            std::tm utc{};
#ifdef _WIN32
            gmtime_s(&utc, &t);
#else
            gmtime_r(&t, &utc);
#endif
            const int h = utc.tm_hour;
            const bool in_window =
                (SESSION_END_HOUR_UTC > SESSION_START_HOUR_UTC)
                    ? (h >= SESSION_START_HOUR_UTC && h <  SESSION_END_HOUR_UTC)
                    : (h >= SESSION_START_HOUR_UTC || h <  SESSION_END_HOUR_UTC);
            if (!in_window) return;
        }

        const double w_hi = w_hi_shared;
        const double w_lo = w_lo_shared;
        range = w_hi - w_lo;

        // -- IDLE -> ARMED --------------------------------------------------
        if (phase == Phase::IDLE) {
            if (m_sl_price > 0.0 && now_s < m_sl_cooldown_ts) {
                if (std::fabs(w_hi - m_sl_price) < SAME_LEVEL_BLOCK_PTS ||
                    std::fabs(w_lo - m_sl_price) < SAME_LEVEL_BLOCK_PTS) {
                    return;
                }
            }
            if (m_win_exit_price > 0.0 && now_s < m_win_exit_block_ts) {
                if (std::fabs(w_hi - m_win_exit_price) < SAME_LEVEL_BLOCK_PTS ||
                    std::fabs(w_lo - m_win_exit_price) < SAME_LEVEL_BLOCK_PTS) {
                    return;
                }
            }
            if (range >= MIN_RANGE && range <= MAX_RANGE) {
                phase          = Phase::ARMED;
                bracket_high   = w_hi;
                bracket_low    = w_lo;
                m_inside_ticks = 0;
                m_armed_ts     = now_s;
            }
            return;
        }

        // -- ARMED ---------------------------------------------------------
        if (phase == Phase::ARMED) {
            bracket_high = std::max(bracket_high, w_hi);
            bracket_low  = std::min(bracket_low,  w_lo);
            range        = bracket_high - bracket_low;
            if (range > MAX_RANGE) { phase = Phase::IDLE; bracket_high = bracket_low = range = 0.0; return; }
            if (range < MIN_RANGE)  { phase = Phase::IDLE; return; }

            if (mid >= bracket_low && mid <= bracket_high) {
                ++m_inside_ticks;
            } else {
                m_inside_ticks = 0;
                phase = Phase::IDLE;
                bracket_high = bracket_low = range = 0.0;
                return;
            }
            if (m_inside_ticks < MIN_BREAK_TICKS) return;

            const double sl_dist = range * SL_FRAC + SL_BUFFER;
            const double tp_dist = sl_dist * TP_RR;
            const double min_tp  = spread * 2.0 + 0.12;
            if (tp_dist < min_tp) {
                phase = Phase::IDLE;
                return;
            }

            // ATR-expansion gate.
            m_range_history.push_back(range);
            if ((int)m_range_history.size() > EXPANSION_HISTORY_LEN)
                m_range_history.pop_front();

            // S58 cold-start guard.
            if ((int)m_range_history.size() < EXPANSION_MIN_HISTORY) {
                phase = Phase::IDLE;
                bracket_high = bracket_low = 0.0;
                return;
            }
            {
                std::vector<double> sorted(m_range_history.begin(), m_range_history.end());
                std::sort(sorted.begin(), sorted.end());
                const size_t n = sorted.size();
                const double median = (n % 2 == 1)
                    ? sorted[n / 2]
                    : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
                const double threshold = median * EXPANSION_MULT;
                if (range < threshold) {
                    phase = Phase::IDLE;
                    bracket_high = bracket_low = 0.0;
                    return;
                }
            }

            phase = Phase::PENDING;
            m_armed_ts = now_s;
        }
    }

private:
    void _confirm_fill(bool is_long, double fill_px, double fill_lot,
                       double spread_at_fill, int64_t now_s) noexcept
    {
        const double sl_dist = range * SL_FRAC + SL_BUFFER;
        const double tp_dist = sl_dist * TP_RR;
        pos.active          = true;
        pos.is_long         = is_long;
        pos.entry           = fill_px;
        pos.sl              = is_long ? (fill_px - sl_dist) : (fill_px + sl_dist);
        pos.tp              = is_long ? (fill_px + tp_dist)  : (fill_px - tp_dist);
        pos.size            = fill_lot;
        pos.mfe             = 0.0;
        pos.mae             = 0.0;
        pos.spread_at_entry = spread_at_fill;
        pos.entry_ts        = now_s;
        pos.be_locked       = false;
        phase               = Phase::LIVE;
    }

    template <typename Sink>
    void _manage(double bid, double ask, double mid,
                 int64_t now_s, Sink& sink) noexcept
    {
        if (!pos.active) return;
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;
        if (move < pos.mae) pos.mae = move;

        const int64_t held_s = now_s - pos.entry_ts;

        if (move > 0 && !pos.be_locked && pos.mfe >= BE_TRIGGER_PTS) {
            const double effective_offset = (move >= BE_OFFSET_PTS) ? BE_OFFSET_PTS : 0.0;
            const double be_target = pos.is_long
                ? (pos.entry + effective_offset)
                : (pos.entry - effective_offset);
            if (pos.is_long  && be_target > pos.sl) pos.sl = be_target;
            if (!pos.is_long && be_target < pos.sl) pos.sl = be_target;
            pos.be_locked = true;
        }

        const bool arm_mfe_ok  = (MIN_TRAIL_ARM_PTS  <= 0.0) || (pos.mfe >= MIN_TRAIL_ARM_PTS);
        const bool arm_hold_ok = (MIN_TRAIL_ARM_SECS <= 0 ) || (held_s  >= MIN_TRAIL_ARM_SECS);
        if (move > 0 && arm_mfe_ok && arm_hold_ok) {
            const double mfe_trail   = pos.mfe * MFE_TRAIL_FRAC;
            const double range_trail = range * TRAIL_FRAC;
            const double trail_dist  = (mfe_trail > 0.0) ? std::min(range_trail, mfe_trail) : range_trail;
            const double trail_sl = pos.is_long ? (pos.entry + pos.mfe - trail_dist)
                                                : (pos.entry - pos.mfe + trail_dist);
            if (pos.is_long  && trail_sl > pos.sl) pos.sl = trail_sl;
            if (!pos.is_long && trail_sl < pos.sl) pos.sl = trail_sl;
        }

        const bool tp_hit = pos.is_long ? (ask >= pos.tp) : (bid <= pos.tp);
        if (tp_hit) { _close(pos.tp, "TP_HIT", now_s, sink); return; }

        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (sl_hit) {
            const double exit_px = pos.is_long ? bid : ask;
            const bool sl_at_be        = (pos.sl <= pos.entry + 0.01)
                                      && (pos.sl >= pos.entry - 0.01);
            const bool trail_in_profit = pos.is_long
                ? (pos.sl > pos.entry + 0.01)
                : (pos.sl < pos.entry - 0.01);
            const char* reason;
            if      (sl_at_be)        reason = "BE_HIT";
            else if (trail_in_profit) reason = "TRAIL_HIT";
            else                      reason = "SL_HIT";
            _close(exit_px, reason, now_s, sink);
        }
    }

    template <typename Sink>
    void _close(double exit_px, const char* reason,
                int64_t now_s, Sink& sink) noexcept
    {
        if (!pos.active) return;
        const bool   is_long_  = pos.is_long;
        const double entry_    = pos.entry;
        const double sl_       = pos.sl;
        const double tp_       = pos.tp;
        const double size_     = pos.size;
        const double mfe_      = pos.mfe;
        const double mae_      = pos.mae;
        const double spread_at_entry_ = pos.spread_at_entry;
        const int64_t entry_ts_ = pos.entry_ts;

        const double pnl = (is_long_ ? (exit_px - entry_) : (entry_ - exit_px)) * size_ * USD_PER_PT;

        if (std::string(reason) == "SL_HIT") {
            m_sl_cooldown_ts = now_s + T.SAME_LEVEL_POST_SL_BLOCK_S;
            m_sl_price       = entry_;
        }
        if (std::string(reason) == "TRAIL_HIT" || std::string(reason) == "TP_HIT") {
            m_win_exit_price    = exit_px;
            m_win_exit_block_ts = now_s + T.SAME_LEVEL_POST_WIN_BLOCK_S;
        }

        omega::TradeRecord tr;
        tr.id            = ++m_trade_id;
        tr.symbol        = "XAUUSD";
        tr.side          = is_long_ ? "LONG" : "SHORT";
        tr.engine        = "MidScalperGoldSweep";
        tr.regime        = "MID_COMPRESSION";
        tr.entryPrice    = entry_;
        tr.exitPrice     = exit_px;
        tr.tp            = tp_;
        tr.sl            = sl_;
        tr.size          = size_;
        tr.pnl           = pnl;
        tr.net_pnl       = pnl;
        tr.mfe           = mfe_ * size_ * USD_PER_PT;
        tr.mae           = mae_ * size_ * USD_PER_PT;
        tr.entryTs       = entry_ts_;
        tr.exitTs        = now_s;
        tr.exitReason    = reason;
        tr.spreadAtEntry = spread_at_entry_;
        tr.bracket_hi    = bracket_high;
        tr.bracket_lo    = bracket_low;
        tr.shadow        = true;

        pos = LivePos{};
        phase = Phase::COOLDOWN;
        m_cooldown_start = now_s;
        bracket_high = bracket_low = range = 0.0;

        sink(tr);
    }

    int     m_ticks_received = 0;
    int     m_inside_ticks   = 0;
    int64_t m_armed_ts       = 0;
    int64_t m_cooldown_start = 0;
    int64_t m_sl_cooldown_ts = 0;
    double  m_sl_price       = 0.0;
    double  m_win_exit_price = 0.0;
    int64_t m_win_exit_block_ts = 0;
    int     m_trade_id       = 0;
    std::deque<double> m_range_history;
};

// -----------------------------------------------------------------------------
// Per-combo result aggregator + sink.
// -----------------------------------------------------------------------------
struct ComboResult {
    int    combo_id    = 0;
    double p[5]        = {};
    int    n_trades    = 0;
    int    n_wins      = 0;
    double gross_pnl   = 0.0;   // $
    double net_pnl     = 0.0;   // $
    double sum_hold_s  = 0.0;
    double gross_w     = 0.0;
    double gross_l     = 0.0;
    double sum_mfe     = 0.0;
    double sum_mae     = 0.0;
    double equity      = 0.0;
    double peak_equity = 0.0;
    double max_dd      = 0.0;
    int64_t first_ts_s = 0;
    int64_t last_ts_s  = 0;
    int n_tp        = 0;
    int n_be        = 0;
    int n_trail     = 0;
    int n_sl        = 0;
};

struct ComboSink {
    ComboResult* out = nullptr;
    int64_t      cur_idx = 0;
    int64_t      warmup_ticks = 0;

    void operator()(const omega::TradeRecord& tr) noexcept {
        if (!out) return;
        if (cur_idx < warmup_ticks) return;

        // Cost model: round-trip cost ~= spread at entry * size * USD_PER_PT.
        // Live MidScalper observed gross-to-net delta ~$2.50/trade on 0.01 lots.
        const double cost = std::max(tr.spreadAtEntry, 0.20) * tr.size * 100.0;
        const double net  = tr.pnl - cost;

        out->n_trades   += 1;
        out->gross_pnl  += tr.pnl;
        out->net_pnl    += net;
        out->sum_hold_s += static_cast<double>(tr.exitTs - tr.entryTs);
        out->sum_mfe    += tr.mfe;
        out->sum_mae    += tr.mae;
        if (net > 0) { out->n_wins += 1; out->gross_w += net; }
        else         { out->gross_l += -net; }

        out->equity += net;
        if (out->equity > out->peak_equity) out->peak_equity = out->equity;
        const double dd = out->peak_equity - out->equity;
        if (dd > out->max_dd) out->max_dd = dd;

        if (out->first_ts_s == 0) out->first_ts_s = tr.entryTs;
        out->last_ts_s = tr.exitTs;

        if      (tr.exitReason == "TP_HIT")    ++out->n_tp;
        else if (tr.exitReason == "BE_HIT")    ++out->n_be;
        else if (tr.exitReason == "TRAIL_HIT") ++out->n_trail;
        else if (tr.exitReason == "SL_HIT")    ++out->n_sl;
    }
};

} // namespace omega::midscalper_sweep

// =============================================================================
// Tick row + L2 CSV reader
// =============================================================================
struct TickRow {
    int64_t ts_ms;
    double  bid;
    double  ask;
    double  l2_imb;
};

static std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else if (c == '\r' || c == '\n') {}
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

static int find_col(const std::vector<std::string>& cols, const char* name) {
    for (std::size_t i = 0; i < cols.size(); ++i) {
        if (cols[i] == name) return static_cast<int>(i);
    }
    return -1;
}

static bool load_l2_csv(const char* path,
                        std::vector<TickRow>& rows,
                        bool verbose) noexcept
{
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[err] cannot open %s\n", path);
        return false;
    }
    std::string line;
    if (!std::getline(f, line)) {
        std::fprintf(stderr, "[err] empty file %s\n", path);
        return false;
    }
    auto header = split_csv(line);
    const int c_ts  = find_col(header, "ts_ms");
    const int c_bid = find_col(header, "bid");
    const int c_ask = find_col(header, "ask");
    const int c_imb = find_col(header, "l2_imb");
    if (c_ts < 0 || c_bid < 0 || c_ask < 0) {
        std::fprintf(stderr, "[err] missing required columns in %s\n", path);
        return false;
    }

    const std::size_t before = rows.size();
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        auto fld = split_csv(line);
        if (static_cast<int>(fld.size()) <= std::max({c_ts, c_bid, c_ask})) continue;
        TickRow r{};
        try {
            r.ts_ms = static_cast<int64_t>(std::stoll(fld[c_ts]));
            r.bid   = std::stod(fld[c_bid]);
            r.ask   = std::stod(fld[c_ask]);
            r.l2_imb = (c_imb >= 0 && c_imb < (int)fld.size()) ? std::stod(fld[c_imb]) : 0.5;
        } catch (...) { continue; }
        rows.push_back(r);
    }
    if (verbose) {
        std::printf("[bt] loaded %zu rows from %s\n",
                    rows.size() - before, path);
        std::fflush(stdout);
    }
    return true;
}

// =============================================================================
// Sweep runner
// =============================================================================
namespace ms = omega::midscalper_sweep;

static void run_sweep(const std::vector<TickRow>& ticks,
                      int64_t warmup_ticks,
                      std::vector<ms::ComboResult>& results,
                      bool verbose)
{
    auto engines_p = std::make_unique<std::array<ms::MidScalperEngine, ms::N_COMBOS>>();
    auto& engines  = *engines_p;
    auto sinks_p   = std::make_unique<std::array<ms::ComboSink, ms::N_COMBOS>>();
    auto& sinks    = *sinks_p;

    results.assign(ms::N_COMBOS, ms::ComboResult{});
    for (int i = 0; i < ms::N_COMBOS; ++i) {
        engines[i].T = ms::traits_for_combo(i);
        results[i].combo_id = i;
        results[i].p[0] = ms::value_for_combo(i, 0);
        results[i].p[1] = ms::value_for_combo(i, 1);
        results[i].p[2] = ms::value_for_combo(i, 2);
        results[i].p[3] = ms::value_for_combo(i, 3);
        results[i].p[4] = ms::value_for_combo(i, 4);
        sinks[i].out          = &results[i];
        sinks[i].cur_idx      = 0;
        sinks[i].warmup_ticks = warmup_ticks;
    }

    const int64_t N = static_cast<int64_t>(ticks.size());
    int64_t progress_step = N / 20;
    if (progress_step < 1) progress_step = 1;
    const auto t0 = std::chrono::steady_clock::now();

    // Shared rolling-window state. The mid window is identical for all 490
    // engines (depends only on the tick stream, not the swept params), so we
    // compute hi/lo once per tick and pass the values into on_tick. Without
    // this hoist, 600-element max/min ran 490 times per tick -> ~155M ops
    // per tape; with it, ~1 set of max/min per tick.
    // Capacity = STRUCTURE_LOOKBACK*2 = 600 mids (matches live engine retain).
    constexpr int WIN_CAP = ms::MidScalperEngine::STRUCTURE_LOOKBACK * 2;
    std::deque<double> window;

    for (int64_t k = 0; k < N; ++k) {
        const TickRow& r = ticks[k];
        const double mid = (r.bid + r.ask) * 0.5;
        if (r.bid > 0.0 && r.ask > 0.0) {
            window.push_back(mid);
            if ((int)window.size() > WIN_CAP) window.pop_front();
        }
        double w_hi = 0.0, w_lo = 0.0;
        const int win_count = (int)window.size();
        if (win_count >= ms::MidScalperEngine::STRUCTURE_LOOKBACK) {
            // Only compute when we have a full window -- engines that don't
            // have a full window early-return before consulting w_hi/w_lo.
            w_hi = *std::max_element(window.begin(), window.end());
            w_lo = *std::min_element(window.begin(), window.end());
        }

        for (int i = 0; i < ms::N_COMBOS; ++i) {
            sinks[i].cur_idx = k;
            engines[i].on_tick(r.bid, r.ask, r.ts_ms, r.l2_imb,
                               win_count, w_hi, w_lo,
                               sinks[i]);
        }
        if (verbose && (k % progress_step == 0)) {
            const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            std::printf("  sweep %5.1f%%  (%" PRId64 "/%" PRId64 ")  elapsed=%lldms\n",
                        100.0 * static_cast<double>(k) / static_cast<double>(N),
                        k, N, static_cast<long long>(dt));
            std::fflush(stdout);
        }
    }
}

// =============================================================================
// Output
// =============================================================================
static void write_results_csv(const char* path,
                              std::vector<ms::ComboResult>& results)
{
    std::ofstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[err] cannot write %s\n", path);
        return;
    }
    f << "rank,combo_id,COOLDOWN_S,SAME_LEVEL_POST_WIN_BLOCK_S,"
         "SAME_LEVEL_POST_SL_BLOCK_S,MIN_ENTRY_TICKS,MAX_SPREAD,"
         "n_trades,n_wins,win_rate,trades_per_day,gross_pnl,net_pnl,"
         "avg_hold_s,profit_factor,avg_mfe,avg_mae,max_dd,"
         "n_TP,n_BE,n_TRAIL,n_SL\n";
    for (std::size_t rk = 0; rk < results.size(); ++rk) {
        const auto& r = results[rk];
        const double wr = (r.n_trades > 0) ? (double)r.n_wins / r.n_trades : 0.0;
        const double ah = (r.n_trades > 0) ? r.sum_hold_s / r.n_trades : 0.0;
        const double pf = (r.gross_l > 0.0) ? (r.gross_w / r.gross_l) : 0.0;
        const double am = (r.n_trades > 0) ? r.sum_mfe / r.n_trades : 0.0;
        const double aa = (r.n_trades > 0) ? r.sum_mae / r.n_trades : 0.0;
        const double dur_s = (r.last_ts_s > r.first_ts_s)
            ? static_cast<double>(r.last_ts_s - r.first_ts_s) : 0.0;
        const double tpd = (dur_s > 0.0) ? r.n_trades * 86400.0 / dur_s : 0.0;
        f << std::fixed << std::setprecision(4);
        f << (rk + 1) << "," << r.combo_id << ","
          << (int)r.p[0] << "," << (int)r.p[1] << "," << (int)r.p[2] << ","
          << (int)r.p[3] << "," << r.p[4] << ","
          << r.n_trades << "," << r.n_wins << "," << wr << "," << tpd << ","
          << r.gross_pnl << "," << r.net_pnl << "," << ah << "," << pf << ","
          << am << "," << aa << "," << r.max_dd << ","
          << r.n_tp << "," << r.n_be << "," << r.n_trail << "," << r.n_sl << "\n";
    }
}

static void print_top(const std::vector<ms::ComboResult>& results, int top) {
    std::printf("\n=== TOP %d CONFIGS (by net_pnl * PF) ===\n", top);
    std::printf("%4s %5s %5s %5s %5s %5s  %6s %6s %7s %10s %10s %6s %8s %9s\n",
                "rk", "CD", "PW", "PSL", "MET", "MSP",
                "N", "WR%", "T/day", "gross", "net", "PF", "maxDD", "score");
    for (int rk = 0; rk < top && rk < (int)results.size(); ++rk) {
        const auto& r = results[rk];
        const double wr = (r.n_trades > 0) ? 100.0 * r.n_wins / r.n_trades : 0.0;
        const double pf = (r.gross_l > 0.0) ? (r.gross_w / r.gross_l) : 0.0;
        const double dur_s = (r.last_ts_s > r.first_ts_s)
            ? static_cast<double>(r.last_ts_s - r.first_ts_s) : 0.0;
        const double tpd = (dur_s > 0.0) ? r.n_trades * 86400.0 / dur_s : 0.0;
        const double score = r.net_pnl * pf;
        std::printf("%4d %5d %5d %5d %5d %5.1f  %6d %5.1f%% %7.1f %10.2f %10.2f %6.2f %8.2f %9.2f\n",
                    rk + 1,
                    (int)r.p[0], (int)r.p[1], (int)r.p[2], (int)r.p[3], r.p[4],
                    r.n_trades, wr, tpd, r.gross_pnl, r.net_pnl, pf, r.max_dd, score);
    }
}

// =============================================================================
// main
// =============================================================================
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <l2_ticks_XAUUSD.csv> [<more.csv>...] "
            "[--warmup N] [--out PATH] [--top N] [--verbose]\n", argv[0]);
        return 2;
    }

    std::vector<const char*> csv_paths;
    int64_t warmup = 1000;
    const char* out_path = "backtest/sweep_midscalper_XAUUSD.csv";
    int top = 20;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--warmup") == 0 && i + 1 < argc) {
            warmup = std::atoll(argv[++i]);
        } else if (std::strcmp(a, "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (std::strcmp(a, "--top") == 0 && i + 1 < argc) {
            top = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--verbose") == 0) {
            verbose = true;
        } else if (a[0] != '-') {
            csv_paths.push_back(a);
        }
    }
    if (csv_paths.empty()) {
        std::fprintf(stderr, "[err] no CSV inputs supplied\n");
        return 2;
    }

    std::printf("[bt] MidScalper sweep -- XAUUSD, %d combos\n", ms::N_COMBOS);
    std::printf("[bt] swept params (live -> grid):\n"
                "       COOLDOWN_S                  180 -> {30,60,90,180,270,360,540}\n"
                "       SAME_LEVEL_POST_WIN_BLOCK_S 600 -> {60,180,300,600,900,1200,1800}\n"
                "       SAME_LEVEL_POST_SL_BLOCK_S  900 -> {120,300,600,900,1200,1800,2700}\n"
                "       MIN_ENTRY_TICKS              30 -> {10,15,20,30,45,60,90}\n"
                "       MAX_SPREAD                  2.5 -> {1.0,1.5,2.0,2.5,3.0,3.5,5.0}\n");

    std::vector<TickRow> ticks;
    ticks.reserve(4'000'000);
    for (const char* p : csv_paths) {
        if (!load_l2_csv(p, ticks, true)) return 1;
    }
    if (ticks.empty()) {
        std::fprintf(stderr, "[err] no tick rows loaded\n");
        return 1;
    }
    std::printf("[bt] total ticks: %zu  warmup: %lld  combos: %d\n",
                ticks.size(), static_cast<long long>(warmup), ms::N_COMBOS);

    std::vector<ms::ComboResult> results;
    run_sweep(ticks, warmup, results, verbose);

    std::sort(results.begin(), results.end(),
              [](const ms::ComboResult& a, const ms::ComboResult& b) {
                  const double pf_a = (a.gross_l > 0.0) ? a.gross_w / a.gross_l : 0.0;
                  const double pf_b = (b.gross_l > 0.0) ? b.gross_w / b.gross_l : 0.0;
                  const double sa = a.net_pnl * pf_a;
                  const double sb = b.net_pnl * pf_b;
                  if (sa != sb) return sa > sb;
                  return a.net_pnl > b.net_pnl;
              });

    write_results_csv(out_path, results);
    std::printf("[bt] wrote %s\n", out_path);

    print_top(results, top);
    return 0;
}

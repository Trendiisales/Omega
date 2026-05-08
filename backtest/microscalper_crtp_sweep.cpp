// =============================================================================
// microscalper_crtp_sweep.cpp -- CRTP sweep harness for GoldMicroScalper
// =============================================================================
//
// 2026-05-08 S19 (Claude / Jo): Standalone CRTP sweep mirroring the established
//   SweepableEnginesCRTP.hpp + OmegaSweepHarnessCRTP.cpp pattern in a single
//   self-contained TU. 490 combos via the same 7-value geometric grid (0.5x..
//   2.0x), pairwise 2-factor across 5 swept params.
//
//   The live engine in include/GoldMicroScalperEngine.hpp is NOT modified.
//   This TU defines a parallel CRTP variant whose body is a faithful port of
//   the live engine's tick logic, with the 5 swept params hoisted into a
//   Traits<I> typedef so combos compile to distinct types and the optimiser
//   gets full constant propagation.
//
// SWEPT PARAMS (5)
//   p0  ENTRY_Z              -- z-score threshold on 20-tick window
//   p1  TP_DIST_PTS          -- take-profit distance from entry
//   p2  SL_DIST_PTS          -- initial stop-loss distance
//   p3  BE_TRIGGER_PTS       -- MFE that arms the BE lock
//   p4  TRAIL_DIST_PTS       -- post-BE trail distance below MFE
//
// HELD FIXED (live-engine defaults)
//   ENTRY_LOOKBACK=20, BE_OFFSET_PTS=0.3, REVERSAL_LOOKBACK=5,
//   REVERSAL_DELTA_PTS=0.30, L2_FLIP_THRESH=0.20, L2_IMB_LONG_MIN=0.55,
//   L2_IMB_SHORT_MAX=0.45, MAX_SPREAD=1.0, MAX_HOLD_SEC=60, COOLDOWN_S=5,
//   SESSION_START_HOUR=6, SESSION_END_HOUR=22, MIN_ENTRY_TICKS=30,
//   USD_PER_PT=100.0, LIVE_LOT=0.01.
//
// USAGE
//   ./microscalper_crtp_sweep <l2_ticks.csv> [<more.csv>...] [opts]
//     --warmup <n>      ticks to skip before recording (default 1000)
//     --out <path>      output CSV (default backtest/sweep_microscalper.csv)
//     --top <N>         leaderboard rows printed to stdout (default 20)
//     --verbose         per-tape progress reporting
//
// CSV SCHEMA (matches existing L2 captures)
//   Required columns by name in header:
//     ts_ms, bid, ask, l2_imb, l2_bid_vol, l2_ask_vol,
//     depth_bid_levels, depth_ask_levels, watchdog_dead
//   Extra columns (vol_ratio, regime, vpin, has_pos, micro_edge, ewm_drift)
//   are ignored. Header may be in any order; column indices are resolved by
//   name on the first line.
//
// BUILD
//   clang++ -std=c++17 -O3 -DNDEBUG -fbracket-depth=1024 \
//           -I include \
//           backtest/microscalper_crtp_sweep.cpp \
//           -o backtest/microscalper_crtp_sweep
//
//   -fbracket-depth=1024 is REQUIRED on Apple/clang: the 490-element fold
//   over std::index_sequence<N_COMBOS> exceeds clang's default 256 nesting
//   limit. Same flag is set by CMakeLists.txt for the cohort harnesses
//   (OmegaSweepHarness + OmegaSweepHarnessCRTP) -- see CMakeLists.txt:551
//   and :612.
//
//   No project-wide deps. Only OmegaTradeLedger.hpp is pulled in for the
//   TradeRecord shape -- handy because that's the same record the live
//   engine emits, so the sink layer is identical between sweep and live.
// =============================================================================

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "../include/OmegaTradeLedger.hpp"

namespace omega::microscalper_sweep {

// -----------------------------------------------------------------------------
// Pairwise 2-factor sweep grid: 7 values geometric 0.5x..2.0x, 10 pairs of
// the 5 swept params (5 choose 2), 7*7 = 49 combos per pair, 10*49 = 490.
// Identical numerical layout to SweepableEnginesCRTP.hpp.
// -----------------------------------------------------------------------------
inline constexpr double GRID_MULT[7] = {
    0.5,
    0.6299605249474366,
    0.7937005259840998,
    1.0,
    1.2599210498948732,
    1.5874010519681994,
    2.0,
};
inline constexpr int N_PAIRS  = 10;
inline constexpr int PAIR_GRID = 7;
inline constexpr int N_COMBOS = N_PAIRS * PAIR_GRID * PAIR_GRID;  // 490

constexpr std::pair<int,int> pair_indices(int k) {
    constexpr std::pair<int,int> P[10] = {
        {0,1}, {0,2}, {0,3}, {0,4},
        {1,2}, {1,3}, {1,4},
        {2,3}, {2,4},
        {3,4},
    };
    return P[k];
}
constexpr int combo_pair (int I) { return I / 49; }
constexpr int combo_ix_a (int I) { return (I % 49) / 7; }
constexpr int combo_ix_b (int I) { return (I % 49) % 7; }

constexpr double mult_for_param(int I, int p) {
    const int pi = combo_pair(I);
    const auto pp = pair_indices(pi);
    if (pp.first  == p) return GRID_MULT[combo_ix_a(I)];
    if (pp.second == p) return GRID_MULT[combo_ix_b(I)];
    return 1.0;
}

// -----------------------------------------------------------------------------
// Live-engine defaults expressed as a traits BaseParams class. Traits<I>
// scales the 5 swept ones by the per-combo multiplier; everything else is
// inherited at face value.
// -----------------------------------------------------------------------------
struct MicroScalperBaseParams {
    // 2026-05-08 S19 follow-up: ENTRY_Z anchor lowered 1.5 -> 0.75 after the
    // first full-tape sweep saturated the LOW grid boundary (top-9 configs all
    // at the swept minimum 0.75). The 7-value geometric grid (0.5x..2.0x) now
    // covers ENTRY_Z = 0.375 .. 1.5 instead of 0.75 .. 3.0, so the secondary
    // sweep can find the true low-Z optimum. Other anchors unchanged: their
    // top-config values were mid-grid (TP=1.0 / SL=2.38 / BE=0.5 / TR=0.5) so
    // their grids cover the right range already. Live engine default in
    // GoldMicroScalperEngine.hpp::ENTRY_Z stays at 1.5 -- this anchor is sweep
    // -only.
    static constexpr double ENTRY_Z              = 0.75;
    static constexpr double TP_DIST_PTS          = 1.0;
    static constexpr double SL_DIST_PTS          = 1.5;
    static constexpr double BE_TRIGGER_PTS       = 0.5;
    static constexpr double TRAIL_DIST_PTS       = 0.5;
};

template <std::size_t I>
struct MicroScalperTraits {
    static constexpr double ENTRY_Z =
        MicroScalperBaseParams::ENTRY_Z         * mult_for_param(static_cast<int>(I), 0);
    static constexpr double TP_DIST_PTS =
        MicroScalperBaseParams::TP_DIST_PTS     * mult_for_param(static_cast<int>(I), 1);
    static constexpr double SL_DIST_PTS =
        MicroScalperBaseParams::SL_DIST_PTS     * mult_for_param(static_cast<int>(I), 2);
    static constexpr double BE_TRIGGER_PTS =
        MicroScalperBaseParams::BE_TRIGGER_PTS  * mult_for_param(static_cast<int>(I), 3);
    static constexpr double TRAIL_DIST_PTS =
        MicroScalperBaseParams::TRAIL_DIST_PTS  * mult_for_param(static_cast<int>(I), 4);
};

// -----------------------------------------------------------------------------
// MicroScalperBase<Derived> -- engine logic. Reads swept params via
//   static_cast<Derived const*>(this)->traits_t::PARAM. All non-swept
//   constants are static constexpr in the base.
// -----------------------------------------------------------------------------
template <typename Derived>
class MicroScalperBase {
public:
    // Held-fixed constants ----------------------------------------------------
    static constexpr int    ENTRY_LOOKBACK      = 20;
    static constexpr double L2_IMB_LONG_MIN     = 0.55;
    static constexpr double L2_IMB_SHORT_MAX    = 0.45;
    static constexpr double MAX_SPREAD          = 1.0;
    static constexpr double BE_OFFSET_PTS       = 0.3;
    static constexpr int    REVERSAL_LOOKBACK   = 5;
    static constexpr double REVERSAL_DELTA_PTS  = 0.30;
    static constexpr double L2_FLIP_THRESH      = 0.20;
    static constexpr int    MAX_HOLD_SEC        = 60;
    static constexpr int    COOLDOWN_S          = 5;
    static constexpr int    SESSION_START_HOUR  = 6;
    static constexpr int    SESSION_END_HOUR    = 22;
    static constexpr int    MIN_ENTRY_TICKS     = 30;
    static constexpr double USD_PER_PT          = 100.0;
    static constexpr double LIVE_LOT            = 0.01;

    enum class Phase { IDLE, LIVE, COOLDOWN };
    Phase phase = Phase::IDLE;

    struct LivePos {
        bool    active           = false;
        bool    is_long          = false;
        double  entry            = 0.0;
        double  tp               = 0.0;
        double  sl               = 0.0;
        double  size             = 0.0;
        double  mfe              = 0.0;
        double  mae              = 0.0;
        double  spread_at_entry  = 0.0;
        int64_t entry_ts         = 0;
        bool    be_locked        = false;
        bool    l2_real_at_entry = false;
        double  z_at_entry       = 0.0;
    } pos;

    bool has_open_position() const noexcept { return pos.active; }

    template <typename Sink>
    void on_tick(double bid, double ask, int64_t now_ms,
                 Sink& sink,
                 double l2_imbalance = 0.5,
                 double book_slope   = 0.0,
                 bool   vacuum_ask   = false,
                 bool   vacuum_bid   = false,
                 bool   l2_real      = false) noexcept
    {
        using T = typename Derived::traits_t;

        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const int64_t now_s = now_ms / 1000;

        m_last_book_slope = book_slope;
        m_last_l2_real    = l2_real;
        m_last_l2_imb     = l2_imbalance;

        ++m_ticks_received;
        _push_window(mid);
        _push_micro(mid);

        if (phase == Phase::COOLDOWN) {
            if (now_s - m_cooldown_start >= COOLDOWN_S) {
                phase = Phase::IDLE;
                m_cooldown_dir = 0;
            }
        }

        if (phase == Phase::LIVE) {
            _manage(bid, ask, mid, now_s, sink);
            return;
        }

        if (m_ticks_received < MIN_ENTRY_TICKS) return;
        if (m_window_count < ENTRY_LOOKBACK) return;
        if (spread > MAX_SPREAD) return;

        // Session window 06-22 UTC (wraparound-aware identical to live).
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
                (SESSION_END_HOUR > SESSION_START_HOUR)
                    ? (h >= SESSION_START_HOUR && h <  SESSION_END_HOUR)
                    : (h >= SESSION_START_HOUR || h <  SESSION_END_HOUR);
            if (!in_window) return;
        }

        double mean = 0.0, sd = 0.0;
        if (!_rolling_stats(mean, sd)) return;
        if (sd < 0.05) return;
        const double z = (mid - mean) / sd;

        const bool block_long  = (phase == Phase::COOLDOWN && m_cooldown_dir == +1);
        const bool block_short = (phase == Phase::COOLDOWN && m_cooldown_dir == -1);
        const bool z_long      = (z <= -T::ENTRY_Z);
        const bool z_short     = (z >=  T::ENTRY_Z);

        bool l2_ok_long  = true;
        bool l2_ok_short = true;
        if (l2_real) {
            l2_ok_long  = (l2_imbalance >= L2_IMB_LONG_MIN)  || vacuum_ask;
            l2_ok_short = (l2_imbalance <= L2_IMB_SHORT_MAX) || vacuum_bid;
            if (book_slope <= -L2_FLIP_THRESH) l2_ok_long  = false;
            if (book_slope >=  L2_FLIP_THRESH) l2_ok_short = false;
        }

        const bool fire_long  = z_long  && l2_ok_long  && !block_long;
        const bool fire_short = z_short && l2_ok_short && !block_short;

        if (fire_long == fire_short) return;  // both false or contradictory

        const bool   is_long = fire_long;
        const double fill_px = is_long ? ask : bid;
        const double sl_px = is_long
            ? (fill_px - T::SL_DIST_PTS) : (fill_px + T::SL_DIST_PTS);
        const double tp_px = is_long
            ? (fill_px + T::TP_DIST_PTS) : (fill_px - T::TP_DIST_PTS);

        pos = LivePos{};
        pos.active           = true;
        pos.is_long          = is_long;
        pos.entry            = fill_px;
        pos.sl               = sl_px;
        pos.tp               = tp_px;
        pos.size             = LIVE_LOT;
        pos.spread_at_entry  = spread;
        pos.entry_ts         = now_s;
        pos.l2_real_at_entry = l2_real;
        pos.z_at_entry       = z;
        phase = Phase::LIVE;
    }

private:
    // Fixed-size ring buffers ------------------------------------------------
    // ENTRY_LOOKBACK doubles for z-score; REVERSAL_LOOKBACK doubles for
    // micro net-delta. Powers of two-aligned for fast modulo.
    static constexpr int WINDOW_CAP = 32;   // >= ENTRY_LOOKBACK(20)
    static constexpr int MICRO_CAP  = 8;    // >= REVERSAL_LOOKBACK(5)

    std::array<double, WINDOW_CAP> m_window{};
    int                            m_window_head  = 0;
    int                            m_window_count = 0;

    std::array<double, MICRO_CAP> m_micro{};
    int                           m_micro_head  = 0;
    int                           m_micro_count = 0;

    int     m_ticks_received = 0;
    int64_t m_cooldown_start = 0;
    int     m_cooldown_dir   = 0;

    double  m_last_book_slope = 0.0;
    double  m_last_l2_imb     = 0.5;
    bool    m_last_l2_real    = false;
    int     m_trade_id        = 0;

    void _push_window(double v) noexcept {
        m_window[m_window_head] = v;
        m_window_head = (m_window_head + 1) % WINDOW_CAP;
        if (m_window_count < WINDOW_CAP) ++m_window_count;
    }
    void _push_micro(double v) noexcept {
        m_micro[m_micro_head] = v;
        m_micro_head = (m_micro_head + 1) % MICRO_CAP;
        if (m_micro_count < MICRO_CAP) ++m_micro_count;
    }
    double _window_at(int i) const noexcept {
        // i=0 is oldest of the active count; i=count-1 is newest
        const int start = (m_window_head + WINDOW_CAP - m_window_count) % WINDOW_CAP;
        return m_window[(start + i) % WINDOW_CAP];
    }
    double _micro_at(int i) const noexcept {
        const int start = (m_micro_head + MICRO_CAP - m_micro_count) % MICRO_CAP;
        return m_micro[(start + i) % MICRO_CAP];
    }

    bool _rolling_stats(double& mean_out, double& sd_out) const noexcept {
        if (m_window_count < ENTRY_LOOKBACK) return false;
        const int start = m_window_count - ENTRY_LOOKBACK;
        double sum = 0.0;
        for (int i = 0; i < ENTRY_LOOKBACK; ++i) sum += _window_at(start + i);
        const double mean = sum / static_cast<double>(ENTRY_LOOKBACK);
        double var = 0.0;
        for (int i = 0; i < ENTRY_LOOKBACK; ++i) {
            const double d = _window_at(start + i) - mean;
            var += d * d;
        }
        mean_out = mean;
        sd_out   = std::sqrt(var / static_cast<double>(ENTRY_LOOKBACK));
        return true;
    }

    bool _detect_reversal() const noexcept {
        if (!pos.active) return false;
        if (m_micro_count >= REVERSAL_LOOKBACK) {
            const double last  = _micro_at(m_micro_count - 1);
            const double prior = _micro_at(m_micro_count - REVERSAL_LOOKBACK);
            const double delta = last - prior;
            if (pos.is_long  && delta <= -REVERSAL_DELTA_PTS) return true;
            if (!pos.is_long && delta >=  REVERSAL_DELTA_PTS) return true;
        }
        if (pos.l2_real_at_entry && m_last_l2_real) {
            if (pos.is_long  && m_last_book_slope <= -L2_FLIP_THRESH) return true;
            if (!pos.is_long && m_last_book_slope >=  L2_FLIP_THRESH) return true;
        }
        return false;
    }

    template <typename Sink>
    void _manage(double bid, double ask, double mid,
                 int64_t now_s, Sink& sink) noexcept
    {
        using T = typename Derived::traits_t;
        if (!pos.active) return;

        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;
        if (move < pos.mae) pos.mae = move;

        if (!pos.be_locked && pos.mfe >= T::BE_TRIGGER_PTS) {
            const double eff_off = (pos.mfe >= BE_OFFSET_PTS) ? BE_OFFSET_PTS : 0.0;
            const double be_target = pos.is_long
                ? (pos.entry + eff_off) : (pos.entry - eff_off);
            if (pos.is_long  && be_target > pos.sl) pos.sl = be_target;
            if (!pos.is_long && be_target < pos.sl) pos.sl = be_target;
            pos.be_locked = true;
        }

        if (pos.be_locked) {
            const double trail_sl = pos.is_long
                ? (pos.entry + pos.mfe - T::TRAIL_DIST_PTS)
                : (pos.entry - pos.mfe + T::TRAIL_DIST_PTS);
            if (pos.is_long  && trail_sl > pos.sl) pos.sl = trail_sl;
            if (!pos.is_long && trail_sl < pos.sl) pos.sl = trail_sl;
        }

        if (pos.be_locked && _detect_reversal()) {
            const double exit_px = pos.is_long ? bid : ask;
            _close(exit_px, "REVERSAL_EXIT", now_s, sink);
            return;
        }

        const bool tp_hit = pos.is_long ? (ask >= pos.tp) : (bid <= pos.tp);
        if (tp_hit) {
            _close(pos.tp, "TP_HIT", now_s, sink);
            return;
        }

        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (sl_hit) {
            const double exit_px = pos.is_long ? bid : ask;
            const bool sl_at_be = std::fabs(pos.sl - pos.entry) <= 0.05;
            const bool trail_in_prof = pos.is_long
                ? (pos.sl > pos.entry + 0.05)
                : (pos.sl < pos.entry - 0.05);
            const char* reason;
            if      (sl_at_be)      reason = "BE_HIT";
            else if (trail_in_prof) reason = "TRAIL_HIT";
            else                    reason = "SL_HIT";
            _close(exit_px, reason, now_s, sink);
            return;
        }

        if ((now_s - pos.entry_ts) >= MAX_HOLD_SEC) {
            _close(mid, "MAX_HOLD_EXIT", now_s, sink);
            return;
        }
    }

    template <typename Sink>
    void _close(double exit_px, const char* reason,
                int64_t now_s, Sink& sink) noexcept
    {
        if (!pos.active) return;

        const double pnl =
            (pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px)) * pos.size;

        omega::TradeRecord tr;
        tr.id            = ++m_trade_id;
        tr.symbol        = "XAUUSD";
        tr.side          = pos.is_long ? "LONG" : "SHORT";
        tr.engine        = "MicroScalperGold";
        tr.regime        = "MICRO_TICK";
        tr.entryPrice    = pos.entry;
        tr.exitPrice     = exit_px;
        tr.tp            = pos.tp;
        tr.sl            = pos.sl;
        tr.size          = pos.size;
        tr.pnl           = pnl;
        tr.net_pnl       = tr.pnl;
        tr.mfe           = pos.mfe * pos.size;
        tr.mae           = pos.mae * pos.size;
        tr.entryTs       = pos.entry_ts;
        tr.exitTs        = now_s;
        tr.exitReason    = reason;
        tr.spreadAtEntry = pos.spread_at_entry;
        tr.shadow        = true;

        m_cooldown_start = now_s;
        m_cooldown_dir   = pos.is_long ? +1 : -1;
        pos = LivePos{};
        phase = Phase::COOLDOWN;

        sink(tr);
    }
};

template <typename Traits>
class MicroScalper_CRTP : public MicroScalperBase<MicroScalper_CRTP<Traits>> {
public:
    using traits_t = Traits;
};

// -----------------------------------------------------------------------------
// Tuple builder + fold-expression dispatcher. One CRTP instance per combo.
// -----------------------------------------------------------------------------
template <std::size_t I>
using MS_AT = MicroScalper_CRTP<MicroScalperTraits<I>>;

template <std::size_t... I>
auto make_ms_tuple_impl(std::index_sequence<I...>) {
    return std::tuple<MS_AT<I>...>{};
}
inline auto make_ms_tuple() {
    return make_ms_tuple_impl(std::make_index_sequence<N_COMBOS>{});
}

template <std::size_t... I, typename Tup, typename SinkArr>
inline void ms_run_tick_impl(Tup& tup, SinkArr& sinks,
                             double bid, double ask, int64_t ts_ms,
                             double l2_imb, double slope,
                             bool vac_a, bool vac_b, bool l2_real,
                             std::index_sequence<I...>) noexcept
{
    (std::get<I>(tup).on_tick(bid, ask, ts_ms, sinks[I],
                              l2_imb, slope, vac_a, vac_b, l2_real), ...);
}
template <typename Tup, typename SinkArr>
inline void ms_run_tick(Tup& tup, SinkArr& sinks,
                        double bid, double ask, int64_t ts_ms,
                        double l2_imb, double slope,
                        bool vac_a, bool vac_b, bool l2_real) noexcept
{
    ms_run_tick_impl(tup, sinks, bid, ask, ts_ms,
                     l2_imb, slope, vac_a, vac_b, l2_real,
                     std::make_index_sequence<N_COMBOS>{});
}

static void params_for_combo(int I, double out[5]) noexcept {
    out[0] = MicroScalperBaseParams::ENTRY_Z         * mult_for_param(I, 0);
    out[1] = MicroScalperBaseParams::TP_DIST_PTS     * mult_for_param(I, 1);
    out[2] = MicroScalperBaseParams::SL_DIST_PTS     * mult_for_param(I, 2);
    out[3] = MicroScalperBaseParams::BE_TRIGGER_PTS  * mult_for_param(I, 3);
    out[4] = MicroScalperBaseParams::TRAIL_DIST_PTS  * mult_for_param(I, 4);
}

// -----------------------------------------------------------------------------
// Per-combo result aggregator + sink.
// -----------------------------------------------------------------------------
struct ComboResult {
    int    combo_id    = 0;
    double p[5]        = {};
    int    n_trades    = 0;
    int    n_wins      = 0;
    double gross_pnl   = 0.0;   // pts * lot units
    double net_pnl     = 0.0;   // gross - exit-side spread cost (lot * spread)
    double sum_hold_s  = 0.0;
    double gross_w     = 0.0;
    double gross_l     = 0.0;
    double sum_mfe     = 0.0;
    double sum_mae     = 0.0;
    int    cut_winners = 0;     // REVERSAL_EXIT or MAX_HOLD where MFE > 0

    int n_tp        = 0;
    int n_be        = 0;
    int n_trail     = 0;
    int n_sl        = 0;
    int n_reversal  = 0;
    int n_maxhold   = 0;
};

struct ComboSink {
    ComboResult* out = nullptr;
    int64_t      cur_idx = 0;
    int64_t      warmup_ticks = 0;

    void operator()(const omega::TradeRecord& tr) noexcept {
        if (!out) return;
        if (cur_idx < warmup_ticks) return;

        const double net = (tr.exitReason == "TP_HIT")
            ? tr.pnl
            : (tr.pnl - tr.spreadAtEntry * tr.size);

        out->n_trades   += 1;
        out->gross_pnl  += tr.pnl;
        out->net_pnl    += net;
        out->sum_hold_s += static_cast<double>(tr.exitTs - tr.entryTs);
        out->sum_mfe    += tr.mfe;
        out->sum_mae    += tr.mae;
        if (net > 0) { out->n_wins += 1; out->gross_w += net; }
        else         { out->gross_l += -net; }

        if      (tr.exitReason == "TP_HIT")        ++out->n_tp;
        else if (tr.exitReason == "BE_HIT")        ++out->n_be;
        else if (tr.exitReason == "TRAIL_HIT")     ++out->n_trail;
        else if (tr.exitReason == "SL_HIT")        ++out->n_sl;
        else if (tr.exitReason == "REVERSAL_EXIT") ++out->n_reversal;
        else if (tr.exitReason == "MAX_HOLD_EXIT") ++out->n_maxhold;

        if ((tr.exitReason == "REVERSAL_EXIT" || tr.exitReason == "MAX_HOLD_EXIT")
            && tr.mfe > 0)
            ++out->cut_winners;
    }
};

} // namespace omega::microscalper_sweep

// =============================================================================
// Tick row + L2 CSV reader
// =============================================================================
struct TickRow {
    int64_t ts_ms;
    double  bid;
    double  ask;
    double  l2_imb;
    double  l2_bid_vol;
    double  l2_ask_vol;
    int     depth_bid_levels;
    int     depth_ask_levels;
    int     watchdog_dead;

    double book_slope() const noexcept {
        // Approximation: count-based imbalance mapped to -1..+1.
        return 2.0 * (l2_imb - 0.5);
    }
    bool vacuum_ask() const noexcept {
        return depth_ask_levels < 2 && depth_bid_levels >= 3;
    }
    bool vacuum_bid() const noexcept {
        return depth_bid_levels < 2 && depth_ask_levels >= 3;
    }
    bool l2_real() const noexcept {
        return l2_bid_vol > 0.0 && l2_ask_vol > 0.0
            && watchdog_dead == 0
            && depth_bid_levels >= 1 && depth_ask_levels >= 1;
    }
};

static int find_col(const std::vector<std::string>& cols, const char* name) {
    for (std::size_t i = 0; i < cols.size(); ++i) {
        if (cols[i] == name) return static_cast<int>(i);
    }
    return -1;
}

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
    const int c_bv  = find_col(header, "l2_bid_vol");
    const int c_av  = find_col(header, "l2_ask_vol");
    const int c_dbl = find_col(header, "depth_bid_levels");
    const int c_dal = find_col(header, "depth_ask_levels");
    const int c_wd  = find_col(header, "watchdog_dead");
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
            r.l2_imb           = (c_imb >= 0 && c_imb < (int)fld.size()) ? std::stod(fld[c_imb]) : 0.5;
            r.l2_bid_vol       = (c_bv  >= 0 && c_bv  < (int)fld.size()) ? std::stod(fld[c_bv])  : 0.0;
            r.l2_ask_vol       = (c_av  >= 0 && c_av  < (int)fld.size()) ? std::stod(fld[c_av])  : 0.0;
            r.depth_bid_levels = (c_dbl >= 0 && c_dbl < (int)fld.size()) ? std::stoi(fld[c_dbl]) : 0;
            r.depth_ask_levels = (c_dal >= 0 && c_dal < (int)fld.size()) ? std::stoi(fld[c_dal]) : 0;
            r.watchdog_dead    = (c_wd  >= 0 && c_wd  < (int)fld.size()) ? std::stoi(fld[c_wd])  : 0;
        } catch (...) {
            continue;
        }
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
namespace ms = omega::microscalper_sweep;

static void run_sweep(const std::vector<TickRow>& ticks,
                      int64_t warmup_ticks,
                      std::vector<ms::ComboResult>& results,
                      bool verbose)
{
    using TupleT = decltype(ms::make_ms_tuple());
    auto engines_p = std::make_unique<TupleT>();
    auto& engines  = *engines_p;
    auto sinks_p   = std::make_unique<std::array<ms::ComboSink, ms::N_COMBOS>>();
    auto& sinks    = *sinks_p;
    results.assign(ms::N_COMBOS, ms::ComboResult{});

    const int64_t N = static_cast<int64_t>(ticks.size());

    for (int i = 0; i < ms::N_COMBOS; ++i) {
        results[i].combo_id = i;
        ms::params_for_combo(i, results[i].p);
        sinks[i].out          = &results[i];
        sinks[i].cur_idx      = 0;
        sinks[i].warmup_ticks = warmup_ticks;
    }

    int64_t progress_step = N / 20;
    if (progress_step < 1) progress_step = 1;
    const auto t0 = std::chrono::steady_clock::now();

    for (int64_t k = 0; k < N; ++k) {
        const TickRow& r = ticks[k];

        for (int i = 0; i < ms::N_COMBOS; ++i) sinks[i].cur_idx = k;

        ms::ms_run_tick(engines, sinks,
                        r.bid, r.ask, r.ts_ms,
                        r.l2_imb, r.book_slope(),
                        r.vacuum_ask(), r.vacuum_bid(), r.l2_real());

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
    f << "rank,combo_id,ENTRY_Z,TP_DIST_PTS,SL_DIST_PTS,BE_TRIGGER_PTS,TRAIL_DIST_PTS,"
         "n_trades,n_wins,win_rate,gross_pnl,net_pnl,avg_hold_s,profit_factor,"
         "avg_mfe,avg_mae,cut_winners_pct,"
         "n_TP,n_BE,n_TRAIL,n_SL,n_REVERSAL,n_MAXHOLD\n";
    for (std::size_t rk = 0; rk < results.size(); ++rk) {
        const auto& r = results[rk];
        const double wr = (r.n_trades > 0) ? (double)r.n_wins / r.n_trades : 0.0;
        const double ah = (r.n_trades > 0) ? r.sum_hold_s / r.n_trades : 0.0;
        const double pf = (r.gross_l > 0.0) ? (r.gross_w / r.gross_l) : 0.0;
        const double am = (r.n_trades > 0) ? r.sum_mfe / r.n_trades : 0.0;
        const double aa = (r.n_trades > 0) ? r.sum_mae / r.n_trades : 0.0;
        const double cw = (r.n_trades > 0) ? (double)r.cut_winners / r.n_trades : 0.0;
        f << std::fixed << std::setprecision(4);
        f << (rk + 1) << "," << r.combo_id << ","
          << r.p[0] << "," << r.p[1] << "," << r.p[2] << "," << r.p[3] << "," << r.p[4] << ","
          << r.n_trades << "," << r.n_wins << "," << wr << ","
          << r.gross_pnl << "," << r.net_pnl << "," << ah << "," << pf << ","
          << am << "," << aa << "," << cw << ","
          << r.n_tp << "," << r.n_be << "," << r.n_trail << "," << r.n_sl << ","
          << r.n_reversal << "," << r.n_maxhold << "\n";
    }
}

static void print_top(const std::vector<ms::ComboResult>& results, int top) {
    std::printf("\n=== TOP %d CONFIGS (by net_pnl, secondary profit_factor) ===\n", top);
    std::printf("%4s %5s %5s %5s %5s %5s  %5s %6s %8s %8s %6s %5s\n",
                "rk", "Z", "TP", "SL", "BE", "TR",
                "N", "WR%", "gross", "net", "PF", "hold");
    for (int rk = 0; rk < top && rk < (int)results.size(); ++rk) {
        const auto& r = results[rk];
        const double wr = (r.n_trades > 0) ? 100.0 * r.n_wins / r.n_trades : 0.0;
        const double ah = (r.n_trades > 0) ? r.sum_hold_s / r.n_trades : 0.0;
        const double pf = (r.gross_l > 0.0) ? (r.gross_w / r.gross_l) : 0.0;
        std::printf("%4d %5.2f %5.2f %5.2f %5.2f %5.2f  %5d %5.1f%% %8.3f %8.3f %6.2f %4.1fs\n",
                    rk + 1,
                    r.p[0], r.p[1], r.p[2], r.p[3], r.p[4],
                    r.n_trades, wr, r.gross_pnl, r.net_pnl, pf, ah);
    }
}

static void print_exit_breakdown(const ms::ComboResult& r) {
    std::printf("\n  exit breakdown for top combo:\n");
    std::printf("    TP_HIT        %d\n", r.n_tp);
    std::printf("    BE_HIT        %d\n", r.n_be);
    std::printf("    TRAIL_HIT     %d\n", r.n_trail);
    std::printf("    SL_HIT        %d\n", r.n_sl);
    std::printf("    REVERSAL_EXIT %d\n", r.n_reversal);
    std::printf("    MAX_HOLD_EXIT %d\n", r.n_maxhold);
    if (r.n_trades > 0) {
        std::printf("    cut_winners%%  %.1f%%  (REVERSAL/MAX_HOLD with MFE>0)\n",
                    100.0 * (double)r.cut_winners / r.n_trades);
    }
}

// =============================================================================
// main
// =============================================================================
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <l2_ticks.csv> [<more.csv>...] [--warmup N] [--out PATH] "
            "[--top N] [--verbose]\n", argv[0]);
        return 2;
    }

    std::vector<const char*> csv_paths;
    int64_t warmup = 1000;
    const char* out_path = "backtest/sweep_microscalper.csv";
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

    std::vector<TickRow> ticks;
    ticks.reserve(400000);
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
                  if (a.net_pnl != b.net_pnl) return a.net_pnl > b.net_pnl;
                  const double pf_a = (a.gross_l > 0.0) ? a.gross_w / a.gross_l : 0.0;
                  const double pf_b = (b.gross_l > 0.0) ? b.gross_w / b.gross_l : 0.0;
                  return pf_a > pf_b;
              });

    write_results_csv(out_path, results);
    std::printf("[bt] wrote %s\n", out_path);

    print_top(results, top);
    if (!results.empty()) print_exit_breakdown(results[0]);

    return 0;
}

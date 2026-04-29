// ============================================================================
// SpreadRegimeGate.hpp -- v2  (2026-04-29 LATE rebuild)
// ----------------------------------------------------------------------------
// Drop-in replacement for v1.  Existing call sites in HBG / CFE / MCE keep
// working unchanged:
//
//     omega::SpreadRegimeGate m_spread_gate;            // member
//     m_spread_gate.on_tick(now_ms, ask - bid);          // every tick
//     if (!m_spread_gate.can_fire()) return;             // before new entry
//
// What v2 adds over v1
// --------------------
// 1. ADAPTIVE THRESHOLD instead of v1's fixed 0.50pt.  The threshold floats
//    with the regime: T_eff = clamp(p75_of_7day_hourly_medians, ABS_FLOOR,
//    ABS_CEIL) * macro_mult.  Floor 0.40pt (below this, every engine is in
//    its break-even zone empirically); ceiling 0.70pt (above this, every
//    engine bleeds in the recost analysis -- gate must close).
//
// 2. HYSTERESIS to prevent thrash near the threshold.  Two thresholds are
//    derived from T_eff: T_close = T_eff and T_open = T_eff - HYST_BAND
//    (0.05pt).  The internal state machine stays CLOSED until the rolling
//    median falls back below T_open.  Combined with the existing 1h
//    smoothing this removes the need for a separate dwell-tick filter.
//
// 3. SPIKE CHECK on a 60-second short window.  Even when the long (1h)
//    regime is OK, if the *current* spread is more than SPIKE_MULT (2.5x)
//    larger than the 60s median, the gate refuses the fire.  This guards
//    against single-bar wide quotes during news prints / liquidity holes.
//
// 4. MACRO-REGIME HOOK.  Optional -- engines that don't call
//    set_macro_regime() see identical behaviour to "NEUTRAL" (1.0x), which
//    is approximately v1-equivalent threshold-wise (modulo the adaptive
//    base).  Engines that wire it in get RISK_OFF +10%, RISK_ON -5%.
//
// Behavioural drop-in compatibility with v1
// -----------------------------------------
// * on_tick(int64_t,double) and can_fire() preserve v1 signatures.
// * MIN_SAMPLES warmup behaviour preserved (returns true while cold).
// * current_median() and sample_count() preserve v1 names + semantics.
// * The constants WINDOW_MS, MAX_MEDIAN_SPREAD, MIN_SAMPLES are kept as
//   public constexpr values so any external code that referenced them
//   still compiles -- though MAX_MEDIAN_SPREAD is now a *legacy* default
//   (the live threshold comes from the adaptive computation).
//
// Performance
// -----------
// One std::multiset for each of the long-window and short-window samples
// (O(log N) insert/erase, cached median).  Typical N at peak gold density
// (2026-Q1): long ~14k, short ~250.  Adaptive p75 is computed from a
// rolling deque of 168 hourly snapshots and re-sorted only when the deque
// changes (once per UTC hour boundary).  All accessor reads are O(1) via
// caching.
//
// Thread safety
// -------------
// Not internally thread-safe.  Each engine instance owns one gate.  Engines
// assume single-threaded on_tick which matches.
// ============================================================================
#pragma once

#include <cstdint>
#include <set>
#include <deque>
#include <vector>
#include <utility>
#include <iterator>
#include <algorithm>
#include <string>
#include <cmath>

namespace omega {

class SpreadRegimeGate {
public:
    // ------------------------------------------------------------------
    // State of the hysteresis machine.
    //   OPEN   -- gate is letting fires through (long median is acceptable)
    //   CLOSED -- gate is blocking fires (long median above T_close); will
    //             remain closed until long median falls below T_open.
    // ------------------------------------------------------------------
    enum class State : int { OPEN = 0, CLOSED = 1 };

    // ------------------------------------------------------------------
    // Tunables -- single source of truth.  Change here and rebuild.
    // ------------------------------------------------------------------

    // v1-name constants kept for source-compat.  WINDOW_MS still defines
    // the long window.  MAX_MEDIAN_SPREAD is now a *legacy* default; the
    // live threshold is adaptive (see effective_close_threshold()).
    static constexpr int64_t WINDOW_MS         = 3600LL * 1000LL;  // 1h long
    static constexpr double  MAX_MEDIAN_SPREAD = 0.5;              // pt (legacy default)
    static constexpr int     MIN_SAMPLES       = 60;               // warmup

    // Long window (regime detector)
    static constexpr int64_t LONG_WINDOW_MS    = WINDOW_MS;        // alias

    // Short window (spike detector)
    static constexpr int64_t SHORT_WINDOW_MS   = 60LL * 1000LL;    // 60s
    static constexpr int     MIN_SAMPLES_SHORT = 5;                // need >=5 samples for spike

    // Adaptive threshold envelope (empirical from spread_knee.py recost analysis)
    static constexpr double  ABS_FLOOR         = 0.40;             // pt
    static constexpr double  ABS_CEIL          = 0.70;             // pt
    static constexpr double  HYST_BAND         = 0.05;             // pt deadband

    // Spike detector
    static constexpr double  SPIKE_MULT        = 2.5;              // current/short_med > this blocks

    // Hourly p75 history -- 7 days rolling
    static constexpr int64_t HOURLY_BUCKET_MS   = 3600LL * 1000LL;
    static constexpr int     HOURLY_HISTORY_HOURS = 24 * 7;        // 168

    // Macro regime modulation factors
    static constexpr double  MACRO_MULT_RISK_OFF = 1.10;           // widen +10%
    static constexpr double  MACRO_MULT_RISK_ON  = 0.95;           // tighten -5%
    static constexpr double  MACRO_MULT_NEUTRAL  = 1.00;

    // ===========================================================
    // v1 drop-in API -- engines call these unchanged
    // ===========================================================

    // Record current spread.  Call once per tick at the top of on_tick(),
    // even on management-only ticks, so the rolling windows stay fresh.
    void on_tick(int64_t now_ms, double spread_pt) noexcept {
        if (spread_pt < 0.0) spread_pt = 0.0;          // defensive

        // ---- LONG WINDOW (1h) ----
        m_long_window.emplace_back(now_ms, spread_pt);
        m_long_sorted.insert(spread_pt);
        m_long_dirty = true;
        const int64_t long_cutoff = now_ms - LONG_WINDOW_MS;
        while (!m_long_window.empty() && m_long_window.front().first < long_cutoff) {
            const double old_v = m_long_window.front().second;
            auto it = m_long_sorted.find(old_v);
            if (it != m_long_sorted.end()) m_long_sorted.erase(it);
            m_long_window.pop_front();
            m_long_dirty = true;
        }

        // ---- SHORT WINDOW (60s) ----
        m_short_window.emplace_back(now_ms, spread_pt);
        m_short_sorted.insert(spread_pt);
        m_short_dirty = true;
        const int64_t short_cutoff = now_ms - SHORT_WINDOW_MS;
        while (!m_short_window.empty() && m_short_window.front().first < short_cutoff) {
            const double old_v = m_short_window.front().second;
            auto it = m_short_sorted.find(old_v);
            if (it != m_short_sorted.end()) m_short_sorted.erase(it);
            m_short_window.pop_front();
            m_short_dirty = true;
        }

        // Latest spread (used by zero-arg can_fire() for spike check)
        m_last_spread_pt = spread_pt;
        m_last_now_ms    = now_ms;

        // ---- HOURLY BUCKET ROLL ----
        // The first tick anchors the bucket; on every UTC-hour boundary
        // we snapshot the long-window median and push to history.
        const int64_t this_bucket = now_ms - (now_ms % HOURLY_BUCKET_MS);
        if (m_current_bucket_ms == 0) {
            m_current_bucket_ms = this_bucket;
        } else if (this_bucket != m_current_bucket_ms) {
            // close the previous bucket -- record the long median we
            // had at the moment of rollover.
            const double snapshot = compute_median(m_long_sorted);
            m_hourly_history.push_back(snapshot);
            while ((int)m_hourly_history.size() > HOURLY_HISTORY_HOURS) {
                m_hourly_history.pop_front();
            }
            m_current_bucket_ms = this_bucket;
            m_hourly_dirty      = true;
        }
    }

    // Permission to fire (zero-arg form).  Uses the most recent spread
    // captured in on_tick() for the spike check.  Engines that already
    // call on_tick() once at the top of their tick handler can use this
    // form unchanged from v1.
    bool can_fire() const noexcept {
        return can_fire(m_last_spread_pt);
    }

    // Permission to fire with explicit current spread for the spike check.
    // Use when the engine already has the live spread in a local variable
    // and wants to avoid relying on the gate's last-on_tick value (e.g.
    // if there are multiple potential entry checks per tick).
    bool can_fire(double current_spread) const noexcept {
        // Warmup -- don't gate on too few samples
        if ((int)m_long_sorted.size() < MIN_SAMPLES) return true;

        const double long_med = long_median();
        const double T_close  = effective_close_threshold();
        const double T_open   = effective_open_threshold();

        // Hysteresis state machine
        if (m_state == State::OPEN) {
            if (long_med > T_close) m_state = State::CLOSED;
        } else {
            if (long_med < T_open)  m_state = State::OPEN;
        }

        // Spike override -- always blocks regardless of state
        if (current_spread > 0.0 && (int)m_short_sorted.size() >= MIN_SAMPLES_SHORT) {
            const double short_med = short_median();
            if (short_med > 0.0 && current_spread > SPIKE_MULT * short_med) {
                return false;
            }
        }

        return m_state == State::OPEN;
    }

    // ---- v1 diagnostics ----
    double current_median() const noexcept {
        if (m_long_sorted.empty()) return 0.0;
        return long_median();
    }
    int sample_count() const noexcept { return (int)m_long_sorted.size(); }

    // ===========================================================
    // v2 NEW API
    // ===========================================================

    // Set the macro regime modulation.  Recognised values:
    //   "RISK_OFF"  -> threshold widens by MACRO_MULT_RISK_OFF (+10%)
    //   "RISK_ON"   -> threshold tightens by MACRO_MULT_RISK_ON (-5%)
    //   anything else (incl. "NEUTRAL", "", unknown) -> 1.0x
    //
    // Engines that never call this see neutral behaviour, which mirrors
    // v1's drop-in semantics from a macro-modulation standpoint.
    void set_macro_regime(const std::string& regime) noexcept {
        if      (regime == "RISK_OFF") m_macro_mult = MACRO_MULT_RISK_OFF;
        else if (regime == "RISK_ON")  m_macro_mult = MACRO_MULT_RISK_ON;
        else                           m_macro_mult = MACRO_MULT_NEUTRAL;
    }

    // Diagnostic accessors -- safe to call any time.
    double current_long_median()         const noexcept { return long_median(); }
    double current_short_median()        const noexcept { return short_median(); }
    double current_macro_mult()          const noexcept { return m_macro_mult; }
    double effective_close_threshold()   const noexcept {
        const double base = clamp_to_envelope(p75_of_hourly_history());
        return base * m_macro_mult;
    }
    double effective_open_threshold()    const noexcept {
        const double t_close = effective_close_threshold();
        return t_close - HYST_BAND;
    }
    State  state()                       const noexcept { return m_state; }
    int    long_sample_count()           const noexcept { return (int)m_long_sorted.size(); }
    int    short_sample_count()          const noexcept { return (int)m_short_sorted.size(); }
    int    hourly_history_size()         const noexcept { return (int)m_hourly_history.size(); }

private:
    // ----- helpers -----
    static double compute_median(const std::multiset<double>& s) noexcept {
        const std::size_t n = s.size();
        if (n == 0) return 0.0;
        auto it = s.begin();
        std::advance(it, n / 2);
        if (n % 2 == 1) return *it;
        auto prev_it = std::prev(it);
        return 0.5 * (*prev_it + *it);
    }

    static double clamp_to_envelope(double v) noexcept {
        if (v < ABS_FLOOR) return ABS_FLOOR;
        if (v > ABS_CEIL)  return ABS_CEIL;
        return v;
    }

    double long_median() const noexcept {
        if (m_long_dirty) {
            m_long_cached = compute_median(m_long_sorted);
            m_long_dirty  = false;
        }
        return m_long_cached;
    }

    double short_median() const noexcept {
        if (m_short_dirty) {
            m_short_cached = compute_median(m_short_sorted);
            m_short_dirty  = false;
        }
        return m_short_cached;
    }

    // p75 of the hourly history.  When the history is empty (cold start
    // for the first hour after init) we fall back to the current long
    // median so the threshold never collapses to zero -- but the empty-
    // history path does NOT cache, because the long median can move on
    // every tick.  Only the populated-history result is cached, and that
    // cache is invalidated when m_hourly_history changes (bucket roll).
    double p75_of_hourly_history() const noexcept {
        if (m_hourly_history.empty()) {
            // No cache -- recompute from current long median each call.
            return m_long_sorted.empty() ? ABS_FLOOR : long_median();
        }
        if (!m_hourly_dirty) return m_p75_cached;

        std::vector<double> tmp(m_hourly_history.begin(), m_hourly_history.end());
        std::sort(tmp.begin(), tmp.end());
        const std::size_t n = tmp.size();
        // p75 -- conservative integer index for small N: (n*3)/4
        std::size_t i75 = (n * 3) / 4;
        if (i75 >= n) i75 = n - 1;
        m_p75_cached  = tmp[i75];
        m_hourly_dirty = false;
        return m_p75_cached;
    }

    // Long-window state (1h)
    std::deque<std::pair<int64_t, double>> m_long_window;
    std::multiset<double>                  m_long_sorted;
    mutable double                         m_long_cached  = 0.0;
    mutable bool                           m_long_dirty   = false;

    // Short-window state (60s)
    std::deque<std::pair<int64_t, double>> m_short_window;
    std::multiset<double>                  m_short_sorted;
    mutable double                         m_short_cached = 0.0;
    mutable bool                           m_short_dirty  = false;

    // Hourly history for adaptive p75 (rolling 7 days, max 168 entries)
    std::deque<double>                     m_hourly_history;
    mutable double                         m_p75_cached   = 0.0;
    mutable bool                           m_hourly_dirty = true;
    int64_t                                m_current_bucket_ms = 0;

    // Latest tick (used by zero-arg can_fire spike check)
    double                                 m_last_spread_pt = 0.0;
    int64_t                                m_last_now_ms    = 0;

    // Macro modulation
    double                                 m_macro_mult     = MACRO_MULT_NEUTRAL;

    // Hysteresis state -- mutable because can_fire() is logically const but
    // updates the state machine as a side-effect of the question.
    mutable State                          m_state          = State::OPEN;
};

} // namespace omega

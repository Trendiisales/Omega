// ============================================================================
// SpreadRegimeGate.hpp -- 2026-04-29 PM, Option 1 of regime-aware costing plan
//                                          (audit-fixes-18)
//
// Purpose
//   Single point-of-truth gate for "is the bid-ask spread currently wide
//   enough that no engine should be opening positions?"  Embedded as a
//   member of each on-tick engine (HBG, CFE, MCE).  Asked twice per tick
//   per engine: once to record the current spread, once to ask permission
//   to enter a new position.  Position management (SL/TP/TRAIL) for an
//   already-open trade is NOT gated -- only new entries are.
//
//   Driven by SESSION_HANDOFF_2026-04-29_pm.md sec "Three options to test"
//   Option 1.  XAUUSD median bid-ask spread roughly doubled from 0.34pt
//   (2024) to 0.92pt (2026-02 peak).  All three engines were calibrated
//   to the older spread regime and bleed under the new one.  This gate
//   trades a small amount of opportunity loss in widening regimes for a
//   large reduction in adverse fills under abnormal spreads.
//
// Behaviour
//   Window:    1 hour rolling (WINDOW_MS)
//   Threshold: median spread > MAX_MEDIAN_SPREAD (0.5pt) -> can_fire() = false
//   Warmup:    fewer than MIN_SAMPLES (60) ticks in window -> can_fire() = true
//              (we do not gate on insufficient data; engines keep their
//              normal behaviour during the first ~minute of any session)
//
// Integration (HBG / CFE / MCE)
//   Add #include "SpreadRegimeGate.hpp" near top of engine header
//   Add member:        omega::SpreadRegimeGate m_spread_gate;
//   At top of on_tick: m_spread_gate.on_tick(now_ms, ask - bid);
//   Before new fire:   if (!m_spread_gate.can_fire()) return;
//
//   Engines must keep updating the gate even on management-only ticks so
//   the window stays fresh; only the can_fire() check is moved onto the
//   entry path.
//
// Performance
//   std::multiset for the spread sample window: O(log N) insert, O(log N)
//   erase, O(N) median lookup.  Median is cached and only recomputed when
//   the window changes by an insertion or eviction since the last lookup,
//   so steady-state median queries are O(1).  N ~= 14k at peak (2026-Q1
//   tick density on XAUUSD).
//
// Thread safety
//   Not internally thread-safe.  Each engine instance owns one gate.  The
//   engines themselves assume single-threaded on_tick -- this matches.
// ============================================================================
#pragma once

#include <cstdint>
#include <set>
#include <deque>
#include <utility>
#include <iterator>

namespace omega {

class SpreadRegimeGate {
public:
    // Tunables -- single source of truth.  Change here and rebuild.
    static constexpr int64_t WINDOW_MS         = 3600LL * 1000LL;  // 1h
    static constexpr double  MAX_MEDIAN_SPREAD = 0.5;              // pt
    static constexpr int     MIN_SAMPLES       = 60;               // warmup

    // Record current spread.  Call once per tick at the top of on_tick().
    // Always call -- even on ticks where the engine is managing an existing
    // position rather than evaluating an entry -- so the rolling window
    // reflects every tick the engine sees.
    void on_tick(int64_t now_ms, double spread_pt) noexcept {
        if (spread_pt < 0.0) spread_pt = 0.0;          // defensive
        m_window.emplace_back(now_ms, spread_pt);
        m_sorted.insert(spread_pt);
        m_dirty = true;

        // Evict samples outside the trailing window.
        const int64_t cutoff = now_ms - WINDOW_MS;
        while (!m_window.empty() && m_window.front().first < cutoff) {
            const double old_v = m_window.front().second;
            auto it = m_sorted.find(old_v);
            if (it != m_sorted.end()) m_sorted.erase(it);
            m_window.pop_front();
            m_dirty = true;
        }
    }

    // Permission to enter a new position.  Call before the engine's fire
    // logic; if false, skip the fire (hold flat).  Always returns true
    // during warmup so we don't over-gate on cold starts.
    bool can_fire() const noexcept {
        if ((int)m_sorted.size() < MIN_SAMPLES) return true;
        return median() <= MAX_MEDIAN_SPREAD;
    }

    // Diagnostic accessor; useful for engine TRACE prints.  Returns 0.0
    // if the window is empty (which shouldn't happen post-warmup).
    double current_median() const noexcept {
        if (m_sorted.empty()) return 0.0;
        return median();
    }

    int sample_count() const noexcept { return (int)m_sorted.size(); }

private:
    double median() const noexcept {
        if (m_dirty) {
            const std::size_t n = m_sorted.size();
            if (n == 0) {
                m_cached_median = 0.0;
            } else {
                auto it = m_sorted.begin();
                std::advance(it, n / 2);
                if (n % 2 == 1) {
                    m_cached_median = *it;
                } else {
                    auto prev_it = std::prev(it);
                    m_cached_median = 0.5 * (*prev_it + *it);
                }
            }
            m_dirty = false;
        }
        return m_cached_median;
    }

    std::deque<std::pair<int64_t, double>> m_window;
    std::multiset<double>                  m_sorted;
    mutable double                         m_cached_median = 0.0;
    mutable bool                           m_dirty         = false;
};

} // namespace omega

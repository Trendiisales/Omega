// =============================================================================
// test_extractor_no_lookahead.cpp
//
// Synthetic-tick test for the extractor. Verifies:
//   1. NEGATIVE: features at bar t do not see bar t+1.
//   2. POSITIVE: forward returns at bar t DO see bar t+1.
//   3. Bar boundaries are minute-aligned.
//   4. Trailing rings are updated after the row is emitted (not before).
//
// Compile (Mac/Linux):
//   c++ -std=c++20 -O2 -Iextractor tests/test_extractor_no_lookahead.cpp \
//       -o test_extractor_no_lookahead
// =============================================================================

#include "PanelSchema.hpp"
#include "BarState.hpp"
#include "ForwardTracker.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

using namespace edgefinder;

// Capture sink for ForwardTracker — collects all emitted rows.
struct Capture {
    std::vector<PanelRow> rows;
};

static void capture_emit(const PanelRow& r, void* ud) {
    auto* c = static_cast<Capture*>(ud);
    c->rows.push_back(r);
}

// Drive the extractor with a list of (ts_ms, bid, ask) ticks; return rows.
static std::vector<PanelRow> run_synthetic(const std::vector<std::tuple<int64_t,double,double>>& ticks) {
    Capture        cap;
    BarState       bar;
    ForwardTracker fwd(capture_emit, &cap);

    for (auto& [ts_ms, bid, ask] : ticks) {
        PanelRow closed;
        auto cr = bar.on_tick(bid, ask, ts_ms, closed);
        if (cr.emitted) fwd.on_bar_close(closed, cr.close_mid);
        fwd.on_tick(bid, ask, ts_ms);
    }
    PanelRow last;
    bar.finalise(last);
    if (bar.bars_emitted() > 0) {
        fwd.on_bar_close(last, last.close);
    }
    fwd.flush_remaining();
    return std::move(cap.rows);
}

#define ASSERT_TRUE(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); std::exit(1); } \
} while (0)

#define ASSERT_NEAR(a, b, eps, msg) do { \
    double _aa = (a), _bb = (b); \
    if (std::fabs(_aa - _bb) > (eps)) { \
        std::fprintf(stderr, "FAIL: %s  (%g vs %g, eps=%g)  (%s:%d)\n", \
                     (msg), _aa, _bb, (double)(eps), __FILE__, __LINE__); \
        std::exit(1); \
    } \
} while (0)

// Build a tick stream:
//   Day starts at 2025-06-01 00:00:00 UTC (ts_ms = 1748736000000).
//   100 minutes of constant price 2000.0 (1 tick/sec).
//   Then minute 100 has a single tick at 2050.0 (the "future spike").
// Bar 99's close is the last bar of the constant phase; it MUST NOT see the spike.
//
// Spread is fixed 0.20 throughout: bid = mid - 0.10, ask = mid + 0.10.
static std::vector<std::tuple<int64_t,double,double>> make_synthetic_stream() {
    std::vector<std::tuple<int64_t,double,double>> v;
    const int64_t start_ms = 1748736000000LL;  // 2025-06-01 00:00:00 UTC
    // 100 minutes at constant price.
    for (int min = 0; min < 100; ++min) {
        for (int sec = 0; sec < 60; ++sec) {
            const int64_t ts = start_ms + (int64_t)min * 60000 + (int64_t)sec * 1000;
            v.emplace_back(ts, 1999.90, 2000.10);   // mid=2000
        }
    }
    // Minute 100 starts with a spike — single tick at mid=2050.
    v.emplace_back(start_ms + 100*60000, 2049.90, 2050.10);
    // 59 more ticks at 2050 to flesh out the spike bar
    for (int sec = 1; sec < 60; ++sec) {
        v.emplace_back(start_ms + 100*60000 + sec*1000, 2049.90, 2050.10);
    }
    // Add 240 more flat minutes at 2050 so the long-horizon forward returns
    // for bar 99 can be computed (the longest horizon is 240m).
    for (int min = 101; min < 100 + 1 + 240 + 1; ++min) {
        for (int sec = 0; sec < 60; ++sec) {
            const int64_t ts = start_ms + (int64_t)min * 60000 + (int64_t)sec * 1000;
            v.emplace_back(ts, 2049.90, 2050.10);
        }
    }
    return v;
}

int main() {
    auto ticks = make_synthetic_stream();
    auto rows  = run_synthetic(ticks);

    std::fprintf(stdout, "test: produced %zu rows\n", rows.size());

    // Sanity: we should have at least 100 bars (the flat phase).
    ASSERT_TRUE(rows.size() >= 100, "not enough rows produced");

    // ---- TEST 1: bar 99 close should be exactly 2000.0 (no spike leakage) ----
    // Find bar with ts_close at start_ms + 100*60000 (== bar 99 close)
    const int64_t start_ms = 1748736000000LL;
    int idx_b99 = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].ts_close_ms == start_ms + 100LL*60000LL) { idx_b99 = (int)i; break; }
    }
    ASSERT_TRUE(idx_b99 >= 0, "could not find bar 99");
    const auto& b99 = rows[idx_b99];
    ASSERT_NEAR(b99.close, 2000.0, 1e-9, "bar 99 close leaked the spike");
    ASSERT_NEAR(b99.high,  2000.0, 1e-9, "bar 99 high leaked the spike");
    ASSERT_NEAR(b99.low,   2000.0, 1e-9, "bar 99 low leaked the spike");

    // ---- TEST 2: trailing features at bar 99 reflect prior flat closes ----
    // EMA9, EMA50, RSI all should equal exactly 2000.0 (or close to RSI=50 with
    // zero deltas; in our case all prior deltas are zero so RSI is undefined.
    // The Wilder convention with both gain==0 and loss==0 returns 100.0 in our
    // implementation; but for OUR test purposes we accept that the EMAs are
    // exactly 2000.0).
    ASSERT_TRUE(!std::isnan(b99.ema_9),  "ema_9 should be warmed at bar 99");
    ASSERT_TRUE(!std::isnan(b99.ema_50), "ema_50 should be warmed at bar 99");
    ASSERT_NEAR(b99.ema_9,  2000.0, 1e-9, "ema_9 leaked the spike");
    ASSERT_NEAR(b99.ema_50, 2000.0, 1e-9, "ema_50 leaked the spike");
    ASSERT_TRUE(!std::isnan(b99.atr_14), "atr_14 should be warmed at bar 99");
    // ATR is non-zero only because of the *bar range* (high-low) within each bar.
    // Each bar in the flat phase had high=low=2000 (single mid), so true range
    // is 0.0. ATR should equal exactly 0.0 (no leakage of the spike's TR).
    ASSERT_NEAR(b99.atr_14, 0.0, 1e-9, "atr_14 leaked the spike's true range");

    // ---- TEST 3: range_20bar at bar 99 should be hi=lo=2000 ----
    ASSERT_TRUE(!std::isnan(b99.range_20bar_hi), "range_20bar_hi should be set");
    ASSERT_NEAR(b99.range_20bar_hi, 2000.0, 1e-9, "range_20bar_hi leaked the spike");
    ASSERT_NEAR(b99.range_20bar_lo, 2000.0, 1e-9, "range_20bar_lo leaked the spike");

    // ---- TEST 4: vwap_session at bar 99 should be 2000.0 ----
    ASSERT_NEAR(b99.vwap_session, 2000.0, 1e-9, "vwap_session leaked the spike");

    // ---- TEST 5: forward-return cells at bar 99 SHOULD show the spike ----
    // Bar 99 close is at minute 100; tick at minute 100 sec 0 is the spike.
    // The 1-min forward return should be ≈ +50 pts (mid 2050 - mid 2000).
    ASSERT_TRUE(!std::isnan(b99.fwd_ret_1m_pts), "fwd_ret_1m must be filled");
    ASSERT_NEAR(b99.fwd_ret_1m_pts, 50.0, 0.5, "fwd_ret_1m_pts wrong magnitude");
    ASSERT_NEAR(b99.fwd_ret_5m_pts, 50.0, 0.5, "fwd_ret_5m_pts wrong magnitude");
    ASSERT_NEAR(b99.fwd_ret_60m_pts,50.0, 0.5, "fwd_ret_60m_pts wrong magnitude");

    // ---- TEST 6: first_touch at bar 99 should be +1 across all horizons ----
    ASSERT_TRUE(b99.first_touch_5m  == +1, "first_touch_5m should be +1 (price went up first)");
    ASSERT_TRUE(b99.first_touch_15m == +1, "first_touch_15m should be +1");
    ASSERT_TRUE(b99.first_touch_60m == +1, "first_touch_60m should be +1");

    // ---- TEST 7: bracket outcomes at bar 99 ----
    // Bracket 0 = (5m, 10sl, 20tp). Spike of +50 means TP hit -> outcome=+1, pnl=+20.
    ASSERT_TRUE(b99.fwd_bracket_outcome[0] == +1,
                "bracket[0] should be TP-hit (long won)");
    ASSERT_NEAR(b99.fwd_bracket_pts[0], +20.0, 1e-9, "bracket[0] pnl wrong");
    ASSERT_TRUE(b99.fwd_bracket_outcome[1] == +1, "bracket[1] should be TP-hit");
    ASSERT_NEAR(b99.fwd_bracket_pts[1], +50.0, 1e-9, "bracket[1] pnl wrong (15m,20sl,50tp)");

    // ---- TEST 8: bar 100 (the spike bar) close should be 2050.0 ----
    int idx_b100 = idx_b99 + 1;
    if (idx_b100 < (int)rows.size() &&
        rows[idx_b100].ts_close_ms == start_ms + 101LL*60000LL) {
        ASSERT_NEAR(rows[idx_b100].close, 2050.0, 1e-9, "bar 100 close wrong");
        // bar 100's TRAILING ema_9 should still be 2000 (spike not yet incorporated).
        ASSERT_NEAR(rows[idx_b100].ema_9, 2000.0, 1e-9,
                    "bar 100 ema_9 leaked self-bar (should reflect bars 0..99 only)");
    }

    // ---- TEST 9: timestamp alignment ----
    for (size_t i = 0; i < rows.size(); ++i) {
        ASSERT_TRUE(rows[i].ts_close_ms % 60000LL == 0,
                    "bar close timestamp not minute-aligned");
        if (i > 0) {
            ASSERT_TRUE(rows[i].ts_close_ms > rows[i-1].ts_close_ms,
                        "bar close timestamps not strictly increasing");
        }
    }

    std::printf("PASS: all leakage / forward-fill assertions OK (%zu rows tested)\n",
                rows.size());
    return 0;
}

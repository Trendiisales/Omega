#pragma once
// =============================================================================
// PanelSchema.hpp -- canonical row layout for the OmegaEdgeFinder panel.
//
// The extractor writes one PanelRow per closed bar to a flat binary file. The
// Python analytics layer reads the same file via numpy.fromfile with a dtype
// that mirrors this struct exactly. Every column appears once: changing the
// schema in C++ requires the matching change in analytics/load.py.
//
// LAYOUT RULES (do not violate without bumping PANEL_SCHEMA_VERSION below):
//   * No virtual functions / no inheritance / no padding-sensitive members.
//   * Every field is a fixed-width primitive: int32_t / int64_t / double / float / uint8_t.
//   * Booleans are uint8_t (0/1).
//   * The struct is packed (#pragma pack(push, 1)) so the on-disk layout is
//     byte-identical to the in-memory layout. NumPy will read it byte-for-byte.
//   * NaNs are emitted as std::numeric_limits<double>::quiet_NaN() for any
//     feature that is not yet warmed up. Analytics layer treats NaN as missing.
//
// FILE FORMAT:
//   Header: 64 bytes, "EDGEFINDER_PANEL_V<NN>\n" + ascii row-count + padding.
//   Body:   N * sizeof(PanelRow) bytes, raw struct dump.
//
// The header lets the Python loader sanity-check schema version + row count
// without parsing the body. If the file's V tag mismatches PANEL_SCHEMA_VERSION,
// load.py refuses to open it -- forcing a re-extract instead of silent corruption.
// =============================================================================

#include <cstdint>
#include <cstddef>

namespace edgefinder {

constexpr int PANEL_SCHEMA_VERSION = 1;
constexpr int PANEL_HEADER_BYTES   = 64;

// Number of bracket-return scenarios. Indexed [0..N_BRACKETS).
// Layout fixed in Brackets.hpp: { (h_min, sl_pts, tp_pts) } per slot.
constexpr int N_BRACKETS = 6;

#pragma pack(push, 1)
struct PanelRow {
    // ---- timestamps ----
    int64_t  ts_close_ms;          // bar-close epoch ms (UTC)
    int32_t  utc_hour;             // [0..23]
    int32_t  utc_minute_of_day;    // [0..1439]
    int32_t  dow;                  // [0..6] Mon=0
    int32_t  dom;                  // [1..31]
    int32_t  yday;                 // [0..365]
    uint8_t  session;              // 0=ASIAN 1=LONDON 2=OVERLAP 3=NY_AM 4=NY_PM
    uint8_t  _pad_session[3];      // align next int32

    // ---- bar-internal (this bar's accumulated tick stats) ----
    double   open;
    double   high;
    double   low;
    double   close;
    double   bar_range_pts;        // high - low
    double   bar_body_pts;         // |close - open|
    double   bar_upper_wick_pts;
    double   bar_lower_wick_pts;
    int32_t  bar_direction;        // sign(close - open): -1, 0, +1
    int32_t  tick_count;
    double   spread_median_pts;
    double   spread_max_pts;

    // ---- trailing technicals (computed from bars 1..t-1) ----
    double   ema_9;
    double   ema_21;
    double   ema_50;
    double   ema_200;
    double   ema_9_minus_50;       // momentum proxy
    double   ema_50_slope;         // 5-bar linear-reg slope
    double   rsi_14;               // Wilder
    double   atr_14;               // Wilder true-range ATR
    double   atr_50;
    double   range_20bar_hi;
    double   range_20bar_lo;
    double   range_20bar_position; // (close - lo) / (hi - lo) when hi > lo
    double   bb_upper;             // 20-bar ±2σ
    double   bb_lower;
    double   bb_position;
    double   vol_60bar_stddev;     // realised vol of close-to-close returns
    double   vol_5bar_stddev;
    double   vol_5_vs_60_ratio;

    // ---- session / structural ----
    double   session_open_price;
    double   session_open_dist_pts;
    double   session_high;
    double   session_low;
    double   session_range_pts;
    double   session_position;     // [0,1]
    double   vwap_session;
    double   vwap_dist_pts;
    double   vwap_z;               // vwap_dist / vol_60
    double   pdh;                  // prior-day high (locked at 00:00 UTC)
    double   pdl;
    uint8_t  above_pdh;
    uint8_t  below_pdl;
    uint8_t  _pad_pdhl[2];
    double   dist_to_pdh_pts;
    double   dist_to_pdl_pts;
    double   asian_hi;             // 00:00..06:59 UTC running high
    double   asian_lo;
    double   asian_range_pts;      // hi - lo, 0 if not yet built
    uint8_t  asian_built;          // 1 once the 00..06:59 window has closed
    uint8_t  above_asian_hi;
    uint8_t  below_asian_lo;
    uint8_t  _pad_asian[1];

    // ---- recent-move / pattern ----
    double   ret_1bar_pts;
    double   ret_5bar_pts;
    double   ret_15bar_pts;
    double   ret_60bar_pts;
    int32_t  consecutive_up_bars;
    int32_t  consecutive_down_bars;
    uint8_t  nr4;
    uint8_t  nr7;
    uint8_t  inside_bar;
    uint8_t  outside_bar;
    double   gap_from_prev_close;

    // ---- transitions (sparse boolean events) ----
    uint8_t  cross_above_pdh;
    uint8_t  cross_below_pdl;
    uint8_t  cross_above_asian_hi;
    uint8_t  cross_below_asian_lo;
    uint8_t  cross_above_vwap;
    uint8_t  cross_below_vwap;
    uint8_t  ema_9_50_bull_cross;
    uint8_t  ema_9_50_bear_cross;
    uint8_t  enter_bb_upper;
    uint8_t  enter_bb_lower;
    uint8_t  _pad_xs[6];           // align to 8

    // ---- forward returns (filled when future ticks arrive; NaN until then) ----
    double   fwd_ret_1m_pts;
    double   fwd_ret_5m_pts;
    double   fwd_ret_15m_pts;
    double   fwd_ret_60m_pts;
    double   fwd_ret_240m_pts;

    // First-touch direction (+1 if +X pts hit before -X pts within window, -1 reverse, 0 neither)
    int32_t  first_touch_5m;       // ±10 pts
    int32_t  first_touch_15m;      // ±20 pts
    int32_t  first_touch_60m;      // ±50 pts

    // Bracketed returns: for each scenario, returns realised PnL in points
    // (+tp_pts if TP hit first, -sl_pts if SL hit first, MtM at horizon if neither)
    double   fwd_bracket_pts[N_BRACKETS];
    int32_t  fwd_bracket_outcome[N_BRACKETS]; // +1 TP, -1 SL, 0 MtM

    // ---- quality flags ----
    uint8_t  warmed_up;            // 1 once all trailing windows are full
    uint8_t  fwd_complete;         // 1 once all fwd returns are filled
    uint8_t  _pad_tail[6];
};
#pragma pack(pop)

static_assert(sizeof(PanelRow) % 8 == 0,
              "PanelRow must be 8-byte aligned (NumPy struct dtype expects this).");

// Forward-return horizons in MILLISECONDS. Indexed parallel to the columns above.
constexpr int64_t FWD_RET_HORIZONS_MS[5] = {
    60LL    * 1000LL,   // 1m
    5LL*60  * 1000LL,   // 5m
    15LL*60 * 1000LL,   // 15m
    60LL*60 * 1000LL,   // 60m
    240LL*60* 1000LL    // 240m
};
constexpr int N_FWD_RET = 5;

// First-touch ±X points and horizon (parallel arrays).
constexpr double  FIRST_TOUCH_PTS[3]    = { 10.0, 20.0, 50.0 };
constexpr int64_t FIRST_TOUCH_HORIZON_MS[3] = {
    5LL*60  * 1000LL,   // 5m
    15LL*60 * 1000LL,   // 15m
    60LL*60 * 1000LL    // 60m
};
constexpr int N_FIRST_TOUCH = 3;

// Bracket scenarios: { horizon_ms, sl_pts, tp_pts } per slot.
struct BracketSpec { int64_t horizon_ms; double sl_pts; double tp_pts; };
constexpr BracketSpec BRACKETS[N_BRACKETS] = {
    {  5LL*60  * 1000LL,  10.0,  20.0 },  // scalp 5m / 10sl / 20tp
    { 15LL*60  * 1000LL,  20.0,  50.0 },  // short 15m / 20sl / 50tp
    { 15LL*60  * 1000LL,  30.0,  60.0 },  // medium 15m / 30sl / 60tp
    { 60LL*60  * 1000LL,  50.0, 100.0 },  // swing 60m / 50sl / 100tp
    { 60LL*60  * 1000LL, 100.0, 200.0 },  // wide 60m / 100sl / 200tp
    {240LL*60  * 1000LL, 100.0, 300.0 },  // session 240m / 100sl / 300tp
};

// Session enum -- matches order used elsewhere in Omega.
enum class Session : uint8_t {
    ASIAN   = 0,
    LONDON  = 1,
    OVERLAP = 2,
    NY_AM   = 3,
    NY_PM   = 4
};

// Classify a UTC minute-of-day to a session.
//   ASIAN:   0-6:59       (000..419)
//   LONDON:  7:00-10:29   (420..629)
//   OVERLAP: 10:30-12:59  (630..779)
//   NY_AM:   13:00-16:59  (780..1019)
//   NY_PM:   17:00-23:59  (1020..1439)
inline Session classify_session_minute(int mins) noexcept {
    if (mins <  420)  return Session::ASIAN;
    if (mins <  630)  return Session::LONDON;
    if (mins <  780)  return Session::OVERLAP;
    if (mins < 1020)  return Session::NY_AM;
    return Session::NY_PM;
}

} // namespace edgefinder

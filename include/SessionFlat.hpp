#pragma once
// =============================================================================
// SessionFlat.hpp — UTC weekend-window helper for the up-jump ladder mimic books
// (S-2026-07-11). Header-only, self-contained, PURE INTEGER UTC epoch math
// (no gmtime / localtime / tz state) -> thread-safe + reentrant + deterministic.
//
// is_weekend(ts) == true across the FX close window Fri 21:00 UTC -> Sun 22:00 UTC.
// This is a BYTE-FOR-BYTE mirror of backtest/weekend_risk_layers_bt.py's
// is_weekend()/utc_dow()/utc_sod() so the live weekend-gap risk caps (Layer 2 =
// no new weekend arms; Layer 3 = weekend carry-fraction) behave EXACTLY as the
// findings doc measured them (WEEKEND_RISK_LAYERS_FINDINGS.md).
//
// USAGE (matches the harness): test the H1 bar CLOSE instant, i.e. is_weekend(ts+3600)
// for an H1 bar stamped at its OPEN ts. H1 bars stamp at open and fire lazily on the
// close, so the close instant is ts+3600 (the same instant the python harness tested).
//
// Reference: 1970-01-01 (epoch 0) was a THURSDAY; ((days % 7) + 4) % 7 maps epoch
// days -> {0=Sun .. 6=Sat}. Guards keep it correct for the (never-in-practice)
// negative-epoch case, matching the python floor-division fixups.
// =============================================================================
#include <cstdint>

namespace omega {

// UTC day-of-week for an epoch-SECONDS timestamp: 0=Sun, 1=Mon, ... 6=Sat.
inline int utc_dow(int64_t ts) noexcept {
    int64_t d = ((ts / 86400) % 7 + 4) % 7;
    return static_cast<int>(d < 0 ? d + 7 : d);
}

// UTC seconds-of-day in [0, 86400) for an epoch-SECONDS timestamp.
inline int64_t utc_sod(int64_t ts) noexcept {
    int64_t s = ts % 86400;
    return s < 0 ? s + 86400 : s;
}

// Weekend window = Fri 21:00 UTC -> Sun 22:00 UTC (inclusive of Saturday).
// EXACT mirror of weekend_risk_layers_bt.py is_weekend().
inline bool is_weekend(int64_t ts) noexcept {
    const int     dow = utc_dow(ts);
    const int64_t sod = utc_sod(ts);
    if (dow == 6) return true;                  // Saturday: all day
    if (dow == 5) return sod >= 21 * 3600;      // Friday >= 21:00 UTC
    if (dow == 0) return sod <  22 * 3600;      // Sunday  <  22:00 UTC
    return false;
}

} // namespace omega

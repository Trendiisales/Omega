#pragma once
// =============================================================================
// CivilTime.hpp -- pure-integer UTC date arithmetic, no libc, no locks.
//
// Reused approach from SweepableEngines.hpp. We do NOT call gmtime_r in the
// extractor hot loop because:
//   * On macOS, gmtime_r internally takes an os_unfair_lock via notify_check_tz
//     -- catastrophic when called once per tick (154M times).
//   * The extractor loop processes ~250k ticks/sec; we cannot afford a syscall.
//
// Algorithm: Howard Hinnant's civil_from_days (public domain). Computes
// (year, month, day) and day-of-year from days-since-1970-01-01.
// Verified exact for years 0001..9999.
// =============================================================================

#include <cstdint>

namespace edgefinder {

struct CivilTime {
    int     hour;       // [0, 23]
    int     minute;     // [0, 59]
    int     second;     // [0, 59]
    int     yday;       // [0, 365]
    int     dom;        // [1, 31]
    int     dow;        // [0, 6] Mon=0..Sun=6
    int     month;      // [1, 12]
    int     year;
    int     mins_of_day;// hour*60 + minute, [0, 1439]
};

inline CivilTime civil_from_epoch_ms(int64_t epoch_ms) noexcept {
    const int64_t s   = epoch_ms / 1000LL;
    const int64_t day = s / 86400LL;
    const int64_t sod = s - day * 86400LL;        // seconds-of-day [0, 86399]

    CivilTime t{};
    t.hour        = static_cast<int>(sod / 3600LL);
    t.minute      = static_cast<int>((sod / 60LL) % 60LL);
    t.second      = static_cast<int>(sod % 60LL);
    t.mins_of_day = t.hour * 60 + t.minute;

    // 1970-01-01 was a Thursday (dow=3 with Mon=0)
    // Generalised: dow = ((day + 3) % 7 + 7) % 7  (handles negative)
    int64_t d_mod = ((day % 7) + 7) % 7;
    t.dow = static_cast<int>((d_mod + 3) % 7);

    // civil-from-days
    const int64_t z   = day + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int            y   = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);  // Mar1-based
    const unsigned mp  = (5*doy + 2) / 153;
    const unsigned mo  = mp < 10 ? mp + 3 : mp - 9;
    if (mo <= 2) ++y;
    const unsigned d   = doy - (153*mp + 2)/5 + 1;

    t.year  = y;
    t.month = static_cast<int>(mo);
    t.dom   = static_cast<int>(d);

    static constexpr int days_before_month[12] =
        {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const bool leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
    t.yday = days_before_month[mo - 1] + static_cast<int>(d) - 1
           + ((leap && mo > 2) ? 1 : 0);
    return t;
}

// Returns the epoch-ms at the start of the UTC day containing epoch_ms.
inline int64_t day_start_ms(int64_t epoch_ms) noexcept {
    const int64_t s   = epoch_ms / 1000LL;
    const int64_t day = s / 86400LL;
    return day * 86400LL * 1000LL;
}

// Returns the epoch-ms at the start of the UTC minute containing epoch_ms.
inline int64_t minute_start_ms(int64_t epoch_ms) noexcept {
    return (epoch_ms / 60000LL) * 60000LL;
}

} // namespace edgefinder

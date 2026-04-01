// mac_compat.hpp -- POSIX/Mac compatibility shims for Windows-only functions
// Force-included via -include flag in build_mac.sh
// DO NOT include in Windows builds.
#pragma once

#ifndef _WIN32

#include <cstring>
#include <cstdio>
#include <ctime>

// strncpy_s: Windows secure version -- map to strncpy on POSIX
// strncpy_s(dest, src, count) where count = dest buffer size - 1
inline void strncpy_s(char* dest, const char* src, size_t count) {
    if (!dest || count == 0) return;
    strncpy(dest, src ? src : "", count);
    dest[count] = '\0';
}

// gmtime_s: Windows has gmtime_s(result, time), POSIX has gmtime_r(time, result)
// Already handled in GoldEngineStack.hpp via #ifdef _WIN32, but add here as safety
#ifndef gmtime_s
#define gmtime_s(result, time) gmtime_r(time, result)
#endif

#endif // _WIN32

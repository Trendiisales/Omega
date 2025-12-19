#pragma once
// CHIMERA HFT - Fast Numeric Parsers
// REPLACES: atof(), atoi(), stod(), strtod(), strtol()
// 
// These are locale-free, allocation-free, branch-minimal parsers
// optimized for FIX protocol numeric fields.
//
// Usage:
//   FixFieldView v;
//   if (msg.getView(44, v)) {
//       double price = fast_parse_double(v.ptr, v.len);
//   }

#include <cstdint>

namespace Chimera {

// Fast integer parser - NO ALLOCATION, NO LOCALE
// Handles: positive, negative, leading zeros
// Returns 0 on empty/invalid input
inline int fast_parse_int(const char* p, uint32_t n) noexcept {
    if (!p || n == 0) return 0;
    
    int v = 0;
    bool neg = false;
    uint32_t i = 0;
    
    // Handle sign
    if (p[0] == '-') {
        neg = true;
        i = 1;
    } else if (p[0] == '+') {
        i = 1;
    }
    
    // Parse digits
    for (; i < n; ++i) {
        char c = p[i];
        if (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
        }
        // Skip non-digits (whitespace, etc)
    }
    
    return neg ? -v : v;
}

// Fast int64 parser for large sequence numbers, timestamps
inline int64_t fast_parse_int64(const char* p, uint32_t n) noexcept {
    if (!p || n == 0) return 0;
    
    int64_t v = 0;
    bool neg = false;
    uint32_t i = 0;
    
    if (p[0] == '-') {
        neg = true;
        i = 1;
    } else if (p[0] == '+') {
        i = 1;
    }
    
    for (; i < n; ++i) {
        char c = p[i];
        if (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
        }
    }
    
    return neg ? -v : v;
}

// Fast double parser - NO ALLOCATION, NO LOCALE
// Handles: positive, negative, decimal point, scientific notation (basic)
// Precision: sufficient for FIX price/qty fields (8 decimal places)
inline double fast_parse_double(const char* p, uint32_t n) noexcept {
    if (!p || n == 0) return 0.0;
    
    double v = 0.0;
    double frac = 0.1;
    bool neg = false;
    bool seen_dot = false;
    uint32_t i = 0;
    
    // Handle sign
    if (p[0] == '-') {
        neg = true;
        i = 1;
    } else if (p[0] == '+') {
        i = 1;
    }
    
    // Parse mantissa
    for (; i < n; ++i) {
        char c = p[i];
        if (c == '.') {
            seen_dot = true;
        } else if (c >= '0' && c <= '9') {
            if (!seen_dot) {
                v = v * 10.0 + (c - '0');
            } else {
                v += frac * (c - '0');
                frac *= 0.1;
            }
        } else if (c == 'e' || c == 'E') {
            // Scientific notation: parse exponent
            ++i;
            int exp = fast_parse_int(p + i, n - i);
            // Apply exponent
            if (exp > 0) {
                for (int e = 0; e < exp; ++e) v *= 10.0;
            } else if (exp < 0) {
                for (int e = 0; e > exp; --e) v *= 0.1;
            }
            break;
        }
    }
    
    return neg ? -v : v;
}

// Fast unsigned int parser (for sequence numbers, etc)
inline uint32_t fast_parse_uint(const char* p, uint32_t n) noexcept {
    if (!p || n == 0) return 0;
    
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; ++i) {
        char c = p[i];
        if (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
        }
    }
    return v;
}

// Fast uint64 parser
inline uint64_t fast_parse_uint64(const char* p, uint32_t n) noexcept {
    if (!p || n == 0) return 0;
    
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; ++i) {
        char c = p[i];
        if (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
        }
    }
    return v;
}

// Fast boolean parser (FIX uses Y/N, true/false, 1/0)
inline bool fast_parse_bool(const char* p, uint32_t n) noexcept {
    if (!p || n == 0) return false;
    char c = p[0];
    return (c == 'Y' || c == 'y' || c == '1' || c == 'T' || c == 't');
}

} // namespace Chimera

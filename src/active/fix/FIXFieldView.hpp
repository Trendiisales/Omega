#pragma once
// CHIMERA HFT - Zero-Copy FIX Field View
// NO STRING ALLOCATIONS - HOT PATH SAFE
// 
// Usage:
//   FixFieldView v;
//   if (msg.getView(44, v)) {
//       double px = fast_parse_double(v.ptr, v.len);
//   }

#include <cstdint>

namespace Chimera {

struct FixFieldView {
    const char* ptr;   // Points directly into FIX buffer - NO COPY
    uint32_t    len;   // Length of field value
    
    // Convenience: check if view is valid
    bool valid() const noexcept { return ptr != nullptr && len > 0; }
    
    // Convenience: compare against known string (hot path safe)
    bool equals(const char* s, uint32_t slen) const noexcept {
        if (len != slen) return false;
        for (uint32_t i = 0; i < len; ++i) {
            if (ptr[i] != s[i]) return false;
        }
        return true;
    }
    
    // Single char compare (for msgType checks like "D", "8", "0")
    bool equals(char c) const noexcept {
        return len == 1 && ptr[0] == c;
    }
    
    // Two char compare (for msgType checks like "AE", "35")
    bool equals(char c1, char c2) const noexcept {
        return len == 2 && ptr[0] == c1 && ptr[1] == c2;
    }
};

} // namespace Chimera

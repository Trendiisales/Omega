#pragma once
// Backward compatibility - use strategy/Decision.hpp for new code
#include "../strategy/Decision.hpp"

namespace Omega {

// Legacy Decision structure for compatibility
struct LegacyDecision {
    bool valid = false;
    std::string side;
    double qty = 0.0;
    double price = 0.0;
    double score = 0.0;
    int64_t ts = 0;
    
    // Convert from new Decision
    static LegacyDecision from(const Decision& d) {
        LegacyDecision ld;
        ld.valid = d.valid;
        ld.side = d.sideStr();
        ld.qty = d.qty;
        ld.price = d.price;
        ld.score = d.score;
        ld.ts = d.ts;
        return ld;
    }
};

}

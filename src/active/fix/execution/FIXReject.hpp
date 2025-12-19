#pragma once
// =============================================================================
// FIXReject.hpp - FIX Reject Handler v2.0 (Zero-Copy)
// =============================================================================
// HOT PATH SAFE: Uses FixFieldView, no std::string allocations
// =============================================================================

#include <functional>
#include <cstdint>
#include "../FIXMessage.hpp"
#include "../FIXFieldView.hpp"

namespace Chimera {

// Hot-path safe reject info - NO STRING ALLOCATIONS
struct FIXRejectInfo {
    int32_t  refSeqNum;          // Tag 45: RefSeqNum
    int32_t  rejectCode;         // Tag 371: SessionRejectReason
    char     refID[32];          // Tag 45 as string (fixed buffer)
    uint32_t refID_len;          // Actual length
    char     text[128];          // Tag 58: Text (fixed buffer, truncated)
    uint32_t text_len;           // Actual length
    
    FIXRejectInfo() : refSeqNum(0), rejectCode(0), refID_len(0), text_len(0) {
        refID[0] = '\0';
        text[0] = '\0';
    }
};

class FIXReject {
public:
    FIXReject();

    // Parse reject message - HOT PATH SAFE (no allocations)
    bool parse(const FIXMessage& msg, FIXRejectInfo& out) noexcept;

    // Set callback for reject notifications
    void setCallback(std::function<void(const FIXRejectInfo&)> cb);

private:
    std::function<void(const FIXRejectInfo&)> callback_;
};

}


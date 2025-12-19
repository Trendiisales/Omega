#pragma once
// CHIMERA HFT - Preallocated Lock-Free FIX Resend Buffer
//
// This ring buffer stores outgoing FIX messages for potential resend
// without any heap allocation or mutex on the hot path.
//
// Features:
// - Fixed-size preallocated storage (no malloc on send)
// - Lock-free store/fetch via atomic sequence tracking
// - Deterministic replay under disconnect storms
// - HFT-safe: no mutex, no allocation
//
// Usage:
//   // On send (hot path - allocation free)
//   resend.store(seqNum, raw_fix_buf, raw_fix_len);
//
//   // On ResendRequest (cold path)
//   FIXStoredMsg m;
//   if (resend.fetch(reqSeq, m)) {
//       send_raw(m.data, m.len);
//   }

#include <cstdint>
#include <atomic>
#include <cstring>

namespace Chimera {

// Single stored FIX message
// Aligned to cache line to prevent false sharing
struct alignas(64) FIXStoredMsg {
    uint32_t seq;                   // Sequence number
    uint32_t len;                   // Message length
    char     data[512];             // Message data (tune to max FIX msg size)
    
    FIXStoredMsg() : seq(0), len(0) {
        data[0] = '\0';
    }
};

// Lock-free ring buffer for FIX message resend
class FIXResendRing {
public:
    // Ring buffer capacity - must be power of 2 for fast modulo
    static constexpr uint32_t CAP = 4096;
    static constexpr uint32_t MASK = CAP - 1;
    
    FIXResendRing() : head_(0), tail_(0) {
        // Ring is preallocated at construction
    }
    
    // Store message for potential resend
    // HOT PATH: No allocation, no mutex
    void store(uint32_t seq, const char* msg, uint32_t len) noexcept {
        uint32_t idx = seq & MASK;
        FIXStoredMsg& s = ring_[idx];
        
        // Truncate if message too large
        uint32_t copy_len = len > sizeof(s.data) ? sizeof(s.data) : len;
        
        // Store message
        s.seq = seq;
        s.len = copy_len;
        std::memcpy(s.data, msg, copy_len);
        
        // Update head (release semantics for visibility)
        head_.store(seq, std::memory_order_release);
    }
    
    // Fetch message by sequence number for resend
    // Returns true if message found, false if overwritten or not found
    bool fetch(uint32_t seq, FIXStoredMsg& out) const noexcept {
        uint32_t idx = seq & MASK;
        const FIXStoredMsg& s = ring_[idx];
        
        // Check if this slot still contains the requested sequence
        if (s.seq != seq) {
            return false;  // Overwritten or never stored
        }
        
        // Copy out
        out.seq = s.seq;
        out.len = s.len;
        std::memcpy(out.data, s.data, s.len);
        return true;
    }
    
    // Fetch range for ResendRequest (begin to end inclusive)
    // Returns count of messages successfully retrieved
    // Caller provides output array
    uint32_t fetchRange(uint32_t begin, uint32_t end, 
                        FIXStoredMsg* out, uint32_t out_cap) const noexcept {
        if (begin > end || out_cap == 0) return 0;
        
        uint32_t count = 0;
        for (uint32_t seq = begin; seq <= end && count < out_cap; ++seq) {
            if (fetch(seq, out[count])) {
                ++count;
            }
            // Note: gaps in sequence may occur if messages were overwritten
        }
        return count;
    }
    
    // Get current head sequence (most recent stored)
    uint32_t head() const noexcept {
        return head_.load(std::memory_order_acquire);
    }
    
    // Check if sequence is still available (not overwritten)
    bool available(uint32_t seq) const noexcept {
        uint32_t h = head_.load(std::memory_order_acquire);
        
        // Sequence is available if:
        // 1. It's been stored (seq <= head)
        // 2. It hasn't been overwritten (head - seq < CAP)
        if (seq > h) return false;
        if (h - seq >= CAP) return false;
        
        // Verify slot still has correct sequence
        uint32_t idx = seq & MASK;
        return ring_[idx].seq == seq;
    }
    
    // Get range of available sequences
    void getAvailableRange(uint32_t& begin, uint32_t& end) const noexcept {
        uint32_t h = head_.load(std::memory_order_acquire);
        end = h;
        begin = (h >= CAP) ? (h - CAP + 1) : 1;
    }
    
    // Statistics
    uint32_t capacity() const noexcept { return CAP; }

private:
    // Preallocated ring buffer - no dynamic allocation
    alignas(64) FIXStoredMsg ring_[CAP];
    
    // Sequence tracking
    alignas(64) std::atomic<uint32_t> head_;
    alignas(64) std::atomic<uint32_t> tail_;
};

} // namespace Chimera

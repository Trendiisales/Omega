#pragma once
// CHIMERA HFT - FIX Message with Zero-Copy Field Access
//
// HOT PATH: Use getView() + fast_parse_*() - NO ALLOCATIONS
// COLD PATH: Use get() for convenience (allocates strings)
//
// Example hot path usage:
//   FixFieldView v;
//   if (msg.getView(44, v)) {
//       double price = fast_parse_double(v.ptr, v.len);
//   }

#include <string>
#include <unordered_map>
#include <cstdint>
#include "FIXFieldView.hpp"
#include "FIXFastParse.hpp"

namespace Chimera {

class FIXMessage {
public:
    FIXMessage();
    void clear();

    // =========================================================
    // COLD PATH API (allocates strings - OK for config/setup)
    // =========================================================
    void set(int tag, const std::string& v);
    void setInt(int tag, int v);
    
    std::string get(int tag) const;           // Returns copy - COLD PATH ONLY
    int getInt(int tag) const;                // Uses atoi - COLD PATH ONLY
    
    std::string encode() const;
    bool decode(const std::string& raw);
    
    // Legacy field map access (COLD PATH)
    std::unordered_map<int, std::string> fields;

    // =========================================================
    // HOT PATH API (zero-copy - use for onMessage/onTick/onExec)
    // =========================================================
    
    // Zero-copy field access - returns view into buffer
    // Returns true if field exists, false otherwise
    bool getView(int tag, FixFieldView& out) const noexcept;
    
    // Hot-path numeric accessors using fast parsers
    int getIntFast(int tag) const noexcept;
    int64_t getInt64Fast(int tag) const noexcept;
    double getDoubleFast(int tag) const noexcept;
    uint32_t getUIntFast(int tag) const noexcept;
    uint64_t getUInt64Fast(int tag) const noexcept;
    bool getBoolFast(int tag) const noexcept;
    
    // Check if field exists (hot path safe)
    bool hasField(int tag) const noexcept;
    
    // MsgType shortcuts (hot path safe)
    bool isMsgType(char c) const noexcept;            // Single char: '0', '8', 'D'
    bool isMsgType(char c1, char c2) const noexcept;  // Two char: "AE"
    
    // Parse into zero-copy index (call once, then use getView)
    // This is the HFT-grade parser - builds index without string copies
    bool parseZeroCopy(const char* data, uint32_t len);
    
    // Access raw buffer (for resend, logging)
    const char* buffer() const noexcept { return buf_; }
    uint32_t bufferLen() const noexcept { return buf_len_; }
    
    // Called by parser to index fields
    void indexField(int tag, uint32_t offset, uint32_t length) noexcept;
    
    // Set buffer pointer (for zero-copy mode)
    void setBuffer(const char* data, uint32_t len) noexcept;

private:
    // Zero-copy buffer reference
    const char* buf_;
    uint32_t    buf_len_;
    
    // Field index: tag -> (offset << 32 | length)
    // Built during parse, enables O(1) zero-copy lookup
    std::unordered_map<int, uint64_t> index_;
    
    // Static helpers
    static std::string computeBodyLength(const std::string&);
    static std::string computeChecksum(const std::string&);
};

} // namespace Chimera

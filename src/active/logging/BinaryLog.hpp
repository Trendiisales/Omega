// =============================================================================
// BinaryLog.hpp - Binary mmap Logger v1.0 (HFT-Hardened)
// =============================================================================
// DESIGN:
//   - mmap-based
//   - append-only
//   - no locks
//   - no streams
//   - no formatting
//   - fixed binary record
//   - offline decode only
// =============================================================================
#pragma once
#include <cstdint>
#include <cstddef>

namespace Chimera {
namespace Log {

// Log record types
enum class RecordType : uint32_t {
    TICK        = 1,
    ORDER       = 2,
    FILL        = 3,
    CANCEL      = 4,
    REJECT      = 5,
    LATENCY     = 6,
    REGIME      = 7,
    RISK        = 8,
    HEARTBEAT   = 9,
    ERROR       = 10
};

// Fixed-size binary log record (64 bytes, cache-line aligned)
struct alignas(64) LogRecord {
    uint64_t ts_ns;           // Timestamp in nanoseconds
    uint32_t type;            // RecordType
    uint32_t size;            // Payload size used
    char payload[48];         // Fixed payload buffer
};

static_assert(sizeof(LogRecord) == 64, "LogRecord must be 64 bytes");

// Initialize binary log file
bool init(const char* path) noexcept;

// Write a record (lock-free)
void write(const LogRecord& r) noexcept;

// Shutdown and cleanup
void shutdown() noexcept;

// Get current file offset
size_t get_offset() noexcept;

// Check if log is initialized
bool is_initialized() noexcept;

// Helper: get current timestamp in nanoseconds
uint64_t now_ns() noexcept;

// Helper: create a record with timestamp
inline LogRecord make_record(RecordType type) noexcept {
    LogRecord r{};
    r.ts_ns = now_ns();
    r.type = static_cast<uint32_t>(type);
    r.size = 0;
    return r;
}

} // namespace Log
} // namespace Chimera

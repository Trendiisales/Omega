// =============================================================================
// BinaryLog.cpp - Binary mmap Logger Implementation (Cross-Platform)
// =============================================================================
#include "logging/BinaryLog.hpp"
#include <atomic>
#include <cstring>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Chimera {
namespace Log {

static constexpr size_t FILE_SIZE = 64 * 1024 * 1024; // 64MB

#ifdef _WIN32
static HANDLE hFile = INVALID_HANDLE_VALUE;
static HANDLE hMapping = NULL;
static char* base = nullptr;
#else
static int fd = -1;
static char* base = nullptr;
#endif

static std::atomic<size_t> offset{0};
static std::atomic<bool> initialized{false};

bool init(const char* path) noexcept {
    if (initialized.load(std::memory_order_acquire)) {
        return true; // Already initialized
    }
    
#ifdef _WIN32
    // Windows: CreateFile + CreateFileMapping + MapViewOfFile
    hFile = CreateFileA(
        path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    // Set file size
    LARGE_INTEGER fileSize;
    fileSize.QuadPart = FILE_SIZE;
    if (!SetFilePointerEx(hFile, fileSize, NULL, FILE_BEGIN)) {
        CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;
        return false;
    }
    if (!SetEndOfFile(hFile)) {
        CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;
        return false;
    }
    
    hMapping = CreateFileMappingA(
        hFile,
        NULL,
        PAGE_READWRITE,
        0,
        FILE_SIZE,
        NULL
    );
    
    if (hMapping == NULL) {
        CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;
        return false;
    }
    
    base = static_cast<char*>(MapViewOfFile(
        hMapping,
        FILE_MAP_WRITE,
        0, 0, FILE_SIZE
    ));
    
    if (base == nullptr) {
        CloseHandle(hMapping);
        CloseHandle(hFile);
        hMapping = NULL;
        hFile = INVALID_HANDLE_VALUE;
        return false;
    }
#else
    // Unix: open + ftruncate + mmap
    fd = ::open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return false;
    
    if (ftruncate(fd, FILE_SIZE) != 0) {
        ::close(fd);
        fd = -1;
        return false;
    }

    base = static_cast<char*>(
        mmap(nullptr, FILE_SIZE, PROT_WRITE, MAP_SHARED, fd, 0));

    if (base == MAP_FAILED) {
        ::close(fd);
        fd = -1;
        base = nullptr;
        return false;
    }
#endif
    
    initialized.store(true, std::memory_order_release);
    return true;
}

void write(const LogRecord& r) noexcept {
    if (!initialized.load(std::memory_order_acquire)) return;
    
    size_t pos = offset.fetch_add(sizeof(LogRecord), std::memory_order_relaxed);
    
    // Write if within bounds (no wrap - append only)
    if (pos + sizeof(LogRecord) <= FILE_SIZE) {
        std::memcpy(base + pos, &r, sizeof(LogRecord));
    }
}

void shutdown() noexcept {
    if (!initialized.load(std::memory_order_acquire)) return;
    
#ifdef _WIN32
    if (base != nullptr) {
        FlushViewOfFile(base, FILE_SIZE);
        UnmapViewOfFile(base);
        base = nullptr;
    }
    
    if (hMapping != NULL) {
        CloseHandle(hMapping);
        hMapping = NULL;
    }
    
    if (hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;
    }
#else
    if (base && base != MAP_FAILED) {
        msync(base, FILE_SIZE, MS_SYNC);
        munmap(base, FILE_SIZE);
        base = nullptr;
    }
    
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
#endif
    
    initialized.store(false, std::memory_order_release);
}

size_t get_offset() noexcept {
    return offset.load(std::memory_order_acquire);
}

bool is_initialized() noexcept {
    return initialized.load(std::memory_order_acquire);
}

uint64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace Log
} // namespace Chimera

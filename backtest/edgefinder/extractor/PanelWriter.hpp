#pragma once
// =============================================================================
// PanelWriter.hpp -- streams PanelRows to a flat binary file.
//
// File layout:
//   bytes [0..63]   ASCII header: "EDGEFINDER_PANEL_V01 ROWS=NNNNNNNNNN\n" + pad
//                   The ROWS field is patched in finish() once the final count
//                   is known. Up to 10 digits => max ~10B rows (we have <1M).
//   bytes [64..]    raw PanelRow structs (sizeof(PanelRow) bytes each).
//
// Python loader (analytics/load.py) reads the header, validates the version,
// then numpy.fromfile(path, dtype=PANEL_DTYPE, offset=64).
// =============================================================================

#include "PanelSchema.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

namespace edgefinder {

class PanelWriter {
public:
    PanelWriter() = default;
    ~PanelWriter() { close(); }

    bool open(const std::string& path) noexcept {
        path_ = path;
        f_ = std::fopen(path.c_str(), "wb");
        if (!f_) return false;
        // Reserve the 64-byte header (we'll rewrite it on finish()).
        char hdr[PANEL_HEADER_BYTES];
        std::memset(hdr, ' ', sizeof(hdr));
        std::snprintf(hdr, sizeof(hdr),
                      "EDGEFINDER_PANEL_V%02d ROWS=          ",
                      PANEL_SCHEMA_VERSION);
        // The snprintf wrote a NUL; replace with space to keep the header
        // pure-ASCII and exactly PANEL_HEADER_BYTES wide. Last byte = \n.
        for (int i = 0; i < PANEL_HEADER_BYTES; ++i) {
            if (hdr[i] == '\0') hdr[i] = ' ';
        }
        hdr[PANEL_HEADER_BYTES - 1] = '\n';
        if (std::fwrite(hdr, 1, sizeof(hdr), f_) != sizeof(hdr)) return false;
        rows_ = 0;
        return true;
    }

    bool write_row(const PanelRow& r) noexcept {
        if (!f_) return false;
        if (std::fwrite(&r, 1, sizeof(PanelRow), f_) != sizeof(PanelRow)) return false;
        ++rows_;
        return true;
    }

    bool finish() noexcept {
        if (!f_) return false;
        // Rewrite header with actual row count.
        if (std::fseek(f_, 0, SEEK_SET) != 0) { close(); return false; }
        char hdr[PANEL_HEADER_BYTES];
        std::memset(hdr, ' ', sizeof(hdr));
        std::snprintf(hdr, sizeof(hdr),
                      "EDGEFINDER_PANEL_V%02d ROWS=%-10lld",
                      PANEL_SCHEMA_VERSION, static_cast<long long>(rows_));
        for (int i = 0; i < PANEL_HEADER_BYTES; ++i) {
            if (hdr[i] == '\0') hdr[i] = ' ';
        }
        hdr[PANEL_HEADER_BYTES - 1] = '\n';
        if (std::fwrite(hdr, 1, sizeof(hdr), f_) != sizeof(hdr)) { close(); return false; }
        close();
        return true;
    }

    int64_t rows_written() const noexcept { return rows_; }

private:
    void close() noexcept {
        if (f_) { std::fclose(f_); f_ = nullptr; }
    }

    std::string path_;
    std::FILE*  f_    = nullptr;
    int64_t     rows_ = 0;
};

} // namespace edgefinder

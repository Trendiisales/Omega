#pragma once
// =============================================================================
// LogXauusdFvgCsv.hpp -- side-channel CSV writer for the XAUUSD FVG engine.
// =============================================================================
//
// 2026-05-02 SESSION (Claude / Jo):
//   Per docs/DESIGN_XAUUSD_FVG_ENGINE.md  §6 (Quarterly re-validation logging),
//   every closed XAUUSD-FVG trade is appended to a side-channel CSV with the
//   columns required by the v3 walk-forward re-feed at month 3 / 6 / 9 etc.
//
//   Why a side-channel and not extra fields on omega::TradeRecord:
//     - omega::TradeRecord is shared with the cohort. Adding `score_at_entry`,
//       `gap_height`, `fvg_age_bars`, `bars_held`, `direction` to it would
//       force every other engine to either populate them or accept noise.
//     - The S59 sister engine's pattern is the same: ledger via
//       handle_closed_trade(tr); engine-internal extras via a dedicated log.
//
//   Output path:
//     logs/live_xauusd_fvg.csv  (created on first append; header written once).
//
//   Columns are intentionally aligned with the trades_top.csv schema produced
//   by scripts/fvg_pnl_backtest_v3.py so post-hoc walk-forward reconciliation
//   and the synthetic-trace verifier can diff field-by-field. The two extras
//   (engine_id, shadow) are appended at the end so a column-name diff against
//   the v3 CSV is a strict superset.
//
//   Wired in include/engine_init.hpp via:
//     g_xauusd_fvg.on_close_cb = [](const omega::TradeRecord& tr) {
//         handle_closed_trade(tr);
//         omega::xauusd_fvg::log_xauusd_fvg_csv(tr, g_xauusd_fvg.last_extras());
//     };
//
// SAFETY:
//   - Header-only, single-mutex appender. No background thread, no flush
//     batching -- writes are synchronous on the close-callback caller (which
//     fires inside _close() under the engine's own m_close_mtx, so the
//     critical section is already serialised; this mutex only guards the
//     file-handle lifetime).
//   - Open errors are logged once and silenced thereafter so a missing
//     log directory cannot crash the engine.
//   - Format: ISO-8601 UTC timestamps with offset suffix "+00:00" so a
//     direct string comparison with the v3 trades_top.csv entry_time /
//     exit_time columns works in the verifier.
// =============================================================================

#include <cstdio>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <iostream>
#include "OmegaTradeLedger.hpp"
#include "XauusdFvgEngine.hpp"

namespace omega {
namespace xauusd_fvg {

// Default landing path -- relative to the working directory of the running
// binary (Omega.exe / OmegaBacktest / verify_xauusd_fvg). Override via
// set_log_path() before the first append if a different location is needed.
inline std::string& log_path_ref() noexcept
{
    static std::string p = "logs/live_xauusd_fvg.csv";
    return p;
}

inline void set_log_path(const std::string& p) noexcept
{
    log_path_ref() = p;
}

inline std::mutex& log_mtx() noexcept
{
    static std::mutex m;
    return m;
}

inline FILE*& log_file_ref() noexcept
{
    static FILE* f = nullptr;
    return f;
}

inline bool& log_open_failed_ref() noexcept
{
    static bool failed = false;
    return failed;
}

// Translate the engine's session code (A/L/N/O) to the v3 CSV's lowercase
// label (asian/london/ny/off). Required so a verifier diff against
// trades_top.csv's `session` column matches without further mapping.
inline const char* session_label(char c) noexcept
{
    switch (c) {
        case 'A': return "asian";
        case 'L': return "london";
        case 'N': return "ny";
        case 'O': return "off";
        default:  return "unknown";
    }
}

// Translate engine direction code (B/S) to the v3 CSV's lowercase label
// (long/short). Matches scripts/fvg_pnl_backtest_v3.py:simulate_trade().
inline const char* direction_label(char c) noexcept
{
    return (c == 'B') ? "long" : (c == 'S') ? "short" : "unknown";
}

// Translate engine exit reason ("TP_HIT" / "SL_HIT" / "TIME_STOP" /
// "FORCE_CLOSE") to the v3 CSV's lowercase label ("tp" / "sl" / "time_stop").
// FORCE_CLOSE has no v3 equivalent; emitted as "force_close" so it surfaces
// in the diff rather than silently masquerading as something else.
inline std::string exit_reason_label(const std::string& r) noexcept
{
    if (r == "TP_HIT")     return "tp";
    if (r == "SL_HIT")     return "sl";
    if (r == "TIME_STOP")  return "time_stop";
    if (r == "FORCE_CLOSE") return "force_close";
    return r;
}

// Format a unix-seconds timestamp as ISO-8601 UTC with explicit "+00:00" tail
// so the textual representation matches the trades_top.csv `entry_time` /
// `exit_time` columns produced by pandas.
inline std::string format_iso_utc(int64_t ts_s) noexcept
{
    std::time_t t = static_cast<std::time_t>(ts_s);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf),
                  "%04d-%02d-%02d %02d:%02d:%02d+00:00",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

inline void ensure_open_locked() noexcept
{
    if (log_file_ref() != nullptr) return;
    if (log_open_failed_ref())     return;
    const std::string& path = log_path_ref();
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) {
        log_open_failed_ref() = true;
        std::cout << "[XAU-FVG-LOG] open FAILED for '" << path
                  << "' -- side-channel CSV will not be written this session.\n";
        std::cout.flush();
        return;
    }
    // Determine whether the file is empty by seeking to end and checking the
    // position. If empty, write the header line. Done once per session.
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    if (sz == 0) {
        std::fputs(
            "trade_id,direction,entry_ts,entry_time,entry_price,sl,tp,"
            "score_at_entry,atr_at_entry,gap_height,session,fvg_age_bars,"
            "spread_at_entry,exit_ts,exit_time,exit_price,exit_reason,"
            "size,pnl,bars_held,engine,shadow\n",
            f);
        std::fflush(f);
    }
    log_file_ref() = f;
}

// Append one row from a TradeRecord + the engine's last_extras() snapshot.
// Caller is the on_close_cb wired in engine_init.hpp; the snapshot is
// guaranteed-fresh because XauusdFvgEngine::_close() updates m_last_extras
// BEFORE invoking on_close.
inline void log_xauusd_fvg_csv(const omega::TradeRecord& tr,
                               const omega::XauusdFvgEngine::LastClosedExtras& ex) noexcept
{
    std::lock_guard<std::mutex> lk(log_mtx());
    ensure_open_locked();
    FILE* f = log_file_ref();
    if (!f) return;

    const std::string entry_time_iso = format_iso_utc(tr.entryTs);
    const std::string exit_time_iso  = format_iso_utc(tr.exitTs);
    const std::string reason_label   = exit_reason_label(tr.exitReason);

    char line[1024];
    const int n = std::snprintf(line, sizeof(line),
        "%d,%s,%lld,%s,%.6f,%.6f,%.6f,"
        "%.6f,%.6f,%.6f,%s,%d,"
        "%.6f,%lld,%s,%.6f,%s,"
        "%.4f,%.6f,%d,%s,%d\n",
        tr.id,
        direction_label(ex.direction),
        static_cast<long long>(tr.entryTs),
        entry_time_iso.c_str(),
        tr.entryPrice,
        tr.sl,
        tr.tp,
        ex.score_at_entry,
        ex.atr_at_entry,
        ex.gap_height,
        session_label(ex.session),
        ex.fvg_age_bars,
        tr.spreadAtEntry,
        static_cast<long long>(tr.exitTs),
        exit_time_iso.c_str(),
        tr.exitPrice,
        reason_label.c_str(),
        tr.size,
        tr.pnl,
        ex.bars_held,
        tr.engine.c_str(),
        tr.shadow ? 1 : 0);
    if (n > 0 && static_cast<size_t>(n) < sizeof(line)) {
        std::fwrite(line, 1, static_cast<size_t>(n), f);
        std::fflush(f);
    }
}

// Convenience overload that pulls last_extras() from a live engine reference.
// Slightly nicer at the call site:
//     omega::xauusd_fvg::log_xauusd_fvg_csv(tr, g_xauusd_fvg);
inline void log_xauusd_fvg_csv(const omega::TradeRecord& tr,
                               const omega::XauusdFvgEngine& eng) noexcept
{
    log_xauusd_fvg_csv(tr, eng.last_extras());
}

} // namespace xauusd_fvg
} // namespace omega

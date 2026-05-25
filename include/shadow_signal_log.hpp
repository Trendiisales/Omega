#pragma once
// ============================================================================
// shadow_signal_log.hpp — signal-level audit for SymbolSupervisor decisions.
//
// History: omega_config.ini has shipped enable_shadow_signal_audit and
// shadow_signal_csv keys since 2026-04-xx but NO C++ code ever read them.
// On 2026-05-25 the operator noticed omega_shadow_signals.csv had never
// been created — a full day of supervisor decisions silently lost.
// This file is the writer the original config keys assumed would exist.
//
// Pattern: header-only, static-in-header (single TU = src/main.cpp), same
// approach as g_shadow_csv in omega_runtime.hpp + write_shadow_csv in
// logging.hpp. Lives in its own header (not omega_runtime.hpp) so the
// supervisor — included earlier than omega_runtime.hpp in main.cpp — can
// reference the writer without an include-order ratchet.
//
// Guarded by g_cfg.enable_shadow_signal_audit. The opener lives in
// omega_main.hpp and treats an open-failure as FATAL (same as g_shadow_csv).
//
// Row schema:
//   ts_unix,symbol,regime,confidence,bracket_score,breakout_score,
//   top_score,threshold,winner,allow,cooldown,reason
// ============================================================================

#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <string>

// NOTE: declared at GLOBAL scope (matching g_shadow_csv in omega_runtime.hpp),
// NOT inside namespace omega. omega_main.hpp and other callers reference
// these unqualified.

static std::ofstream g_shadow_signal_csv;
static std::mutex    g_shadow_signal_csv_mtx;

// Caller passes the already-computed top_score / threshold (avoids
// recomputing inside the writer and keeps the supervisor's view canonical).
// Caller is expected to gate on "is this a change worth recording" — the
// supervisor already does that for stdout logging; we mirror.
static inline void write_shadow_signal_row(
    const char* symbol,
    const char* regime_name_,
    double       confidence,
    double       bracket_score,
    double       breakout_score,
    double       top_score,
    double       threshold,
    const char*  winner,
    bool         allow,
    bool         cooldown,
    const char*  reason) noexcept
{
    if (!g_shadow_signal_csv.is_open()) return;
    const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(g_shadow_signal_csv_mtx);
    g_shadow_signal_csv
        << now_s << ',' << (symbol ? symbol : "") << ','
        << (regime_name_ ? regime_name_ : "") << ','
        << std::fixed << std::setprecision(3) << confidence << ','
        << bracket_score << ',' << breakout_score << ','
        << top_score << ',' << threshold << ','
        << (winner ? winner : "") << ',' << (allow ? 1 : 0) << ','
        << (cooldown ? 1 : 0) << ','
        << (reason ? reason : "") << '\n';
    g_shadow_signal_csv.flush();
}

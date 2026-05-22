#pragma once
// SeedGuard.hpp -- enforces the no-cold-engines rule at boot.
//
// Standing rule (CLAUDE.md "Engine Warm-Seed Mandate"): every D1/H4/H1 engine
// must hot-seed from a bundled CSV at boot. On 2026-05-22 the boot logs had
// zero [SEED] lines for the entire trading day because nothing in the binary
// enforced the rule -- the only check was a manual operator step. This header
// closes that gap.
//
//   resolve_seed_path(rel)
//     Tries the given path; if missing, tries <exe-dir>/<rel>. Returns the
//     first path that exists, else the input unchanged so the downstream
//     open() fails with a useful error string. Fixes the Windows-service-CWD
//     vs source-tree-CWD mismatch that is the most common cause of cold boot.
//
//   seed_die(engine_name, path)
//     [[noreturn]] -- prints a bright [SEED-FATAL] banner identifying the
//     engine and the path it could not seed from, plus CWD + exe-dir for
//     diagnosis, then std::abort()s the process. Watchdog sees a clean
//     exit-on-boot and the operator sees the banner immediately. Better
//     than silent cold-warm.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
  #include <windows.h>
#endif

namespace omega {

inline std::string seed_exe_dir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf).parent_path().string();
#else
    return {};
#endif
}

inline std::string resolve_seed_path(const std::string& rel) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(rel, ec)) return rel;
    const std::string exe_dir = seed_exe_dir();
    if (!exe_dir.empty()) {
        const std::string a = exe_dir + "/" + rel;
        if (fs::exists(a, ec)) return a;
    }
    return rel;
}

// Anchor process CWD to the directory that contains the running binary.
// Called once at the very top of init_engines(). Most Windows services
// inherit CWD = C:\Windows\System32, which makes every "phase1/..." or
// "data/..." relative path open() against the wrong root. Pinning CWD to
// the exe dir removes that whole class of bug for every existing path
// that init_engines() already uses (logs/, data/, phase1/, etc.).
//
// Safe to call multiple times; if exe path is unavailable, leaves CWD
// untouched and returns false.
inline bool anchor_cwd_to_exe_dir() {
    const std::string dir = seed_exe_dir();
    if (dir.empty()) return false;
    std::error_code ec;
    std::filesystem::current_path(dir, ec);
    if (ec) {
        std::fprintf(stderr,
            "[OMEGA-INIT] WARN failed to set CWD to %s: %s\n",
            dir.c_str(), ec.message().c_str());
        std::fflush(stderr);
        return false;
    }
    std::fprintf(stderr, "[OMEGA-INIT] CWD anchored to exe dir: %s\n", dir.c_str());
    std::fflush(stderr);
    return true;
}

[[noreturn]] inline void seed_die(const char* engine_name,
                                  const std::string& path) {
    std::error_code ec;
    const std::string cwd = std::filesystem::current_path(ec).string();
    std::fprintf(stderr,
        "\n"
        "================================================================\n"
        "[SEED-FATAL] %s could not warm-seed from\n"
        "             %s\n"
        "             CWD     = %s\n"
        "             exe_dir = %s\n"
        "             Engine would cold-warm for days. Rule violated.\n"
        "             See CLAUDE.md > Engine Warm-Seed Mandate.\n"
        "             ABORTING boot.\n"
        "================================================================\n",
        engine_name, path.c_str(),
        cwd.c_str(), seed_exe_dir().c_str());
    std::fflush(stderr);
    std::fprintf(stdout,
        "[SEED-FATAL] %s seed missing (%s) -- ABORTING boot\n",
        engine_name, path.c_str());
    std::fflush(stdout);
    std::abort();
}

// Wrapper for the `warmup_from_csv(warmup_csv_path)` pattern used by many
// engines (TsmomEngine, TrendRiderEngine, CellEngine, XauTrendFollow*,
// EmaPullbackEngine, XauThreeBar30mEngine, XauusdFvgEngine, etc.).
// Resolves the CSV path against the exe dir, calls the engine's warmup,
// and aborts the process if zero bars loaded.
//
// Skips silently when the engine itself is disabled — that's an explicit
// opt-out (e.g. tombstoned strategy) and not a rule violation.
template<typename Engine>
int warmup_or_die(Engine& eng, const char* engine_name) {
    if (!eng.enabled) {
        std::printf("[SEED] %s: skipped (engine disabled)\n", engine_name);
        std::fflush(stdout);
        return 0;
    }
    if (eng.warmup_csv_path.empty()) {
        std::fprintf(stderr,
            "[SEED-FATAL] %s has empty warmup_csv_path -- "
            "rule violation, every enabled engine must declare a CSV\n",
            engine_name);
        seed_die(engine_name, "<empty>");
    }
    const std::string actual = resolve_seed_path(eng.warmup_csv_path);
    eng.warmup_csv_path = actual;
    const int n = eng.warmup_from_csv(actual);
    if (n <= 0) seed_die(engine_name, actual);
    return n;
}

}  // namespace omega

cmake_minimum_required(VERSION 3.20)
project(Omega VERSION 1.0.0 LANGUAGES CXX)

add_definitions(-D_WINSOCKAPI_ -DWIN32_LEAN_AND_MEAN)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# vcpkg toolchain resolution (hardened, Step-6 hot-fix 2026-05-01e).
#
# The build needs the vcpkg toolchain to find libcurl (Step 5/7) and any
# other vcpkg-installed deps. We try in order:
#   1. Caller-supplied -DCMAKE_TOOLCHAIN_FILE on the cmake command line.
#   2. VCPKG_ROOT env var, if set.
#   3. Hardcoded common Windows install paths as a fallback.
# This makes the build robust against shells that forgot to export
# VCPKG_ROOT (which has bitten the QUICK_RESTART deploy at least twice
# now -- once at the Step-5 deploy, then again at the Step-6 deploy
# when a stale empty CMAKE_TOOLCHAIN_FILE in CMakeCache.txt short-
# circuited the original `if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)` check).
#
# On macOS / Linux, none of these branches fire and find_package falls
# back to the system libcurl, which is what we want.
#
# Hardening notes:
#   - Use `if(NOT CMAKE_TOOLCHAIN_FILE)` (truthy check) instead of
#     `if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)`. A previously-failed
#     configure can leave CMAKE_TOOLCHAIN_FILE defined-but-empty in
#     CMakeCache.txt; the original DEFINED check then refuses to retry
#     the autodetect and the build dies with "Could NOT find CURL"
#     even though the autodetect would have succeeded.
#   - Always emit an explicit status line so the next failure is
#     diagnosable from the log even if our autodetect path didn't fire.
#   - Verify the vcpkg-installed curl is actually present after the
#     toolchain is loaded; if not, fail loud with a message that names
#     the exact `vcpkg install` command to run.
if(NOT CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain file (from VCPKG_ROOT)" FORCE)
        message(STATUS "vcpkg:      toolchain via VCPKG_ROOT=$ENV{VCPKG_ROOT}")
    else()
        # Fallback: probe common Windows vcpkg install paths. Order matters
        # -- C:/vcpkg/vcpkg/... is the actual layout on the Omega VPS;
        # C:/vcpkg/... is the more common single-level layout.
        set(_omega_vcpkg_candidates
            "C:/vcpkg/vcpkg/scripts/buildsystems/vcpkg.cmake"
            "C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
            "C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake"
            "C:/tools/vcpkg/scripts/buildsystems/vcpkg.cmake"
            "C:/Users/Administrator/vcpkg/scripts/buildsystems/vcpkg.cmake"
        )
        foreach(_omega_vcpkg_try ${_omega_vcpkg_candidates})
            if(EXISTS "${_omega_vcpkg_try}")
                set(CMAKE_TOOLCHAIN_FILE "${_omega_vcpkg_try}"
                    CACHE STRING "vcpkg toolchain file (autodetected fallback)" FORCE)
                message(STATUS "vcpkg:      toolchain autodetected at ${_omega_vcpkg_try}")
                break()
            endif()
        endforeach()
        unset(_omega_vcpkg_try)
        unset(_omega_vcpkg_candidates)
    endif()
endif()
if(NOT CMAKE_TOOLCHAIN_FILE)
    if(WIN32)
        message(STATUS "vcpkg:      NOT FOUND (no VCPKG_ROOT, no candidate path on disk).")
        message(STATUS "vcpkg:      Install vcpkg + curl with:")
        message(STATUS "vcpkg:        git clone https://github.com/microsoft/vcpkg.git C:/vcpkg/vcpkg")
        message(STATUS "vcpkg:        C:/vcpkg/vcpkg/bootstrap-vcpkg.bat")
        message(STATUS "vcpkg:        C:/vcpkg/vcpkg/vcpkg.exe install curl:x64-windows")
    else()
        message(STATUS "vcpkg:      not used on this host -- relying on system libcurl")
    endif()
else()
    message(STATUS "vcpkg:      using ${CMAKE_TOOLCHAIN_FILE}")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
    # NOTE: /O2 intentionally NOT included in this global add_compile_options.
    # CMake's default CMAKE_CXX_FLAGS_RELEASE already contains "/O2 /Ob2 /DNDEBUG"
    # so Release builds receive /O2 via the per-config flag set. Forcing /O2
    # globally injects it into Debug / RelWithDebInfo builds too, which then
    # conflicts with /RTC1 (Debug runtime checks) and produces MSVC error
    # D8016: "'/O2' and '/RTC1' command-line options are incompatible".
    # If you ever need /O2 on a non-Release config, add it via the per-config
    # flag set, not here.
    add_compile_options(/W4 /WX /wd4100 /wd4505 /wd4324 /std:c++20)
    add_definitions(-D_WIN32_WINNT=0x0601 -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)

    # Defensive guard: strip /RTC1 from every configuration EXCEPT Debug.
    # Some toolchains, CMakePresets, IDE generators, or vcpkg ports
    # occasionally inject /RTC1 into non-Debug configs. This regex
    # normalises the state so /RTC1 cannot coexist with any /O level
    # outside Debug, where it belongs. Re-runs harmlessly if absent.
    foreach(_omega_flag_var
            CMAKE_C_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_RELEASE
            CMAKE_C_FLAGS_RELWITHDEBINFO
            CMAKE_CXX_FLAGS_RELWITHDEBINFO
            CMAKE_C_FLAGS_MINSIZEREL
            CMAKE_CXX_FLAGS_MINSIZEREL)
        if(DEFINED ${_omega_flag_var})
            string(REGEX REPLACE "[ ]*/RTC[1csu]+" ""
                   ${_omega_flag_var} "${${_omega_flag_var}}")
        endif()
    endforeach()
    unset(_omega_flag_var)
else()
    add_compile_options(-Wall -Wextra -O3 -std=c++20)
endif()

find_package(OpenSSL REQUIRED)

# libcurl -- required by src/api/MarketDataProxy.cpp (Step 5/7 of the Omega
# Terminal build). Used to call free market-data providers (Yahoo Finance +
# FRED) from the engine binary so no API tokens ship in the React bundle.
# Backtest targets do NOT link curl -- they live in their own add_executable()
# blocks below and the MarketDataProxy translation unit is gated under
# `#ifndef OMEGA_BACKTEST` so even an accidental include compiles to nothing
# in those builds.
#
# Install:
#   macOS:   brew install curl   (system libcurl already works too)
#   Linux:   apt-get install libcurl4-openssl-dev
#   Windows: vcpkg install curl[ssl]:x64-windows
#
# Step-6 hot-fix carried into Step 7: try find_package(CURL) softly first,
# then on Windows fall back to a manual probe of the vcpkg-installed tree
# before erroring out. This rescues the common case of the toolchain being
# loaded but `vcpkg install curl:x64-windows` having never been run on the
# host (or having been wiped by a vcpkg upgrade). When everything fails we
# emit a precise install command in the error message so the operator can
# copy/paste it into PowerShell.
find_package(CURL QUIET)
if(NOT CURL_FOUND AND WIN32)
    # Probe vcpkg's installed/x64-windows tree directly. The toolchain file
    # adds the vcpkg installed/<triplet>/include and lib paths to the
    # search list, but only if the package is actually installed. If
    # CURL_FOUND is still false here it almost always means the package
    # is missing; try to recover by pointing FindCURL at the vcpkg tree
    # explicitly under the candidate paths from the toolchain probe.
    set(_omega_curl_roots
        "C:/vcpkg/vcpkg/installed/x64-windows"
        "C:/vcpkg/installed/x64-windows"
        "C:/dev/vcpkg/installed/x64-windows"
        "C:/tools/vcpkg/installed/x64-windows"
        "C:/Users/Administrator/vcpkg/installed/x64-windows"
    )
    foreach(_omega_curl_root ${_omega_curl_roots})
        if(EXISTS "${_omega_curl_root}/include/curl/curl.h"
           AND EXISTS "${_omega_curl_root}/lib/libcurl.lib")
            set(CURL_INCLUDE_DIR "${_omega_curl_root}/include"
                CACHE PATH "libcurl include dir (manual probe)" FORCE)
            set(CURL_LIBRARY     "${_omega_curl_root}/lib/libcurl.lib"
                CACHE FILEPATH "libcurl import lib (manual probe)" FORCE)
            message(STATUS "libcurl:    manual probe found at ${_omega_curl_root}")
            break()
        endif()
    endforeach()
    unset(_omega_curl_root)
    unset(_omega_curl_roots)
    find_package(CURL QUIET)
endif()
if(NOT CURL_FOUND)
    if(WIN32)
        message(FATAL_ERROR
            "libcurl is required but was not found. On the Omega VPS, run:\n"
            "    C:/vcpkg/vcpkg/vcpkg.exe install curl:x64-windows\n"
            "Then re-run cmake. (If your vcpkg lives elsewhere, adjust the path.) "
            "If `vcpkg install` reports 'curl is already installed', delete the "
            "build/ directory and re-run; a stale CMakeCache.txt can hold a bad "
            "CURL_INCLUDE_DIR=NOTFOUND from a previous failed configure.")
    else()
        message(FATAL_ERROR
            "libcurl is required but was not found. Install with:\n"
            "    macOS:  brew install curl\n"
            "    Linux:  apt-get install libcurl4-openssl-dev")
    endif()
endif()

include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/gui
    ${OPENSSL_INCLUDE_DIR}
)

set(SOURCES
    src/main.cpp
    src/SymbolConfig.cpp
    src/gui/OmegaTelemetryServer.cpp
    src/api/OmegaApiServer.cpp        # Step 2 Omega Terminal: HTTP :7781 read-API
    src/api/MarketDataProxy.cpp       # Step 5/7 Omega Terminal: market-data proxy (Yahoo Finance + FRED)
    src/api/WatchScheduler.cpp        # Step 6 Omega Terminal: nightly INTEL screener (S&P 500 + NDX)
    src/gold_coordinator.cpp
)

add_executable(Omega ${SOURCES})

# Precompiled Header -- cuts recompile time 40-60% on MSVC /O2.
# omega_pch.hpp contains all stable Windows/OpenSSL/STL includes.
# Omega .hpp engine headers are NOT in the PCH -- they change often and would
# invalidate it constantly. Only rebuild PCH when omega_pch.hpp itself changes.
# target_precompile_headers(Omega PRIVATE include/omega_pch.hpp)  # disabled: LNK1181 cmake_pch.obj path mismatch on clean MSVC builds

target_include_directories(Omega PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/gui
    ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(Omega
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    ws2_32
)

# Build version stamp.
# Git hash + build time are injected as compiler /D defines -- no generated
# header, no sentinel, no forced recompile of main.cpp every build.
#
# HOW IT WORKS:
#   1. UpdateGitHash.cmake runs at cmake CONFIGURE time (not build time).
#      It writes a small .cmake file with set(OMEGA_GIT_HASH ...) etc.
#   2. We include() that file to get the variables into CMake scope.
#   3. target_compile_definitions() passes them as /DOMEGA_GIT_HASH="abc123"
#   4. main.cpp uses #ifndef guards with fallback to "unknown".
#
# RESULT: main.cpp only recompiles when it or its .hpp deps actually change.
# PCH saves 40-60% on every real recompile. Clean build still shows correct hash.
#
# To get a fresh hash without a full rebuild: cmake --fresh -B build (reconfigures).
# DEPLOY_OMEGA.ps1 already does this on every deploy via cmake -B build.

find_package(Git QUIET)

set(VERSION_CMAKE_FILE "${CMAKE_BINARY_DIR}/omega_version.cmake")

execute_process(
    COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DOUTPUT_FILE=${VERSION_CMAKE_FILE}
            -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
            -DOMEGA_FORCE_GIT_HASH=${OMEGA_FORCE_GIT_HASH}
            -P ${CMAKE_SOURCE_DIR}/cmake/UpdateGitHash.cmake
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# Read the computed variables into CMake scope
if(EXISTS "${VERSION_CMAKE_FILE}")
    include("${VERSION_CMAKE_FILE}")
else()
    set(OMEGA_GIT_HASH   "unknown")
    set(OMEGA_GIT_DATE   "unknown")
    set(OMEGA_BUILD_TIME "unknown")
endif()

# Pass as compiler defines -- no header file needed, no forced recompile
target_compile_definitions(Omega PRIVATE
    OMEGA_GIT_HASH="${OMEGA_GIT_HASH}"
    OMEGA_GIT_DATE="${OMEGA_GIT_DATE}"
    OMEGA_BUILD_TIME="${OMEGA_BUILD_TIME}"
)

# Engine headers as OBJECT_DEPENDS -- main.cpp recompiles when headers change.
# The sentinel (old approach) has been removed -- it caused a full recompile
# on every build regardless of whether anything changed, making PCH useless.
file(GLOB OMEGA_HEADERS "${CMAKE_SOURCE_DIR}/include/*.hpp")
set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/main.cpp"
    APPEND PROPERTY OBJECT_DEPENDS
        ${OMEGA_HEADERS}
)

target_include_directories(Omega PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Copy config files to build output directory.
# symbols.ini and omega_config.ini must live next to Omega.exe at runtime.
# This POST_BUILD step copies them automatically on every build so the binary
# always has the latest config without manual copying.
add_custom_command(TARGET Omega POST_BUILD
    # Canonical source is config/symbols.ini -- sync root copy then deploy both
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/config/symbols.ini"
        "${CMAKE_SOURCE_DIR}/symbols.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/config/symbols.ini"
        "$<TARGET_FILE_DIR:Omega>/symbols.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/omega_config.ini"
        "$<TARGET_FILE_DIR:Omega>/omega_config.ini"
    COMMENT "Copying config files to build output directory"
)

# Copy vcpkg runtime DLLs next to Omega.exe (Step-6 hot-fix 2026-05-01e).
# When find_package(CURL) succeeds via the vcpkg toolchain we link against
# libcurl.lib (the import library), but at *runtime* Windows needs the
# matching DLL set: libcurl-x64.dll plus its transitive deps (zlib1.dll,
# libssl-3-x64.dll, libcrypto-3-x64.dll, nghttp2.dll, libssh2.dll, ...).
#
# vcpkg's classic-mode toolchain does NOT auto-deploy these by default for
# x64-windows binaries (autoapplocal is off-by-default outside of x86). So
# we copy the entire vcpkg `installed/<triplet>/bin/` directory (debug or
# release per CONFIG) next to $<TARGET_FILE_DIR:Omega> on every build. This
# is the same shape the symbols.ini POST_BUILD step uses.
#
# Probe order:
#   1. ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin   (toolchain set)
#   2. C:/vcpkg/vcpkg/installed/x64-windows/bin             (Omega VPS layout)
#   3. C:/vcpkg/installed/x64-windows/bin                   (single-level)
#   4. C:/dev/vcpkg/installed/x64-windows/bin
#   5. C:/tools/vcpkg/installed/x64-windows/bin
#   6. C:/Users/Administrator/vcpkg/installed/x64-windows/bin
#
# The first one that exists wins. If none are found and we're on Windows,
# emit a STATUS warning -- the .exe will still link but will fail to load
# at runtime with "code execution cannot proceed because LIBCURL.dll was
# not found", which matches the symptom that took Step 6 down post-build.
if(WIN32)
    set(_omega_vcpkg_bin "")
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        set(_omega_try "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
        if(EXISTS "${_omega_try}")
            set(_omega_vcpkg_bin "${_omega_try}")
        endif()
    endif()
    if(NOT _omega_vcpkg_bin)
        set(_omega_vcpkg_bin_candidates
            "C:/vcpkg/vcpkg/installed/x64-windows/bin"
            "C:/vcpkg/installed/x64-windows/bin"
            "C:/dev/vcpkg/installed/x64-windows/bin"
            "C:/tools/vcpkg/installed/x64-windows/bin"
            "C:/Users/Administrator/vcpkg/installed/x64-windows/bin"
        )
        foreach(_omega_try ${_omega_vcpkg_bin_candidates})
            if(EXISTS "${_omega_try}")
                set(_omega_vcpkg_bin "${_omega_try}")
                break()
            endif()
        endforeach()
        unset(_omega_vcpkg_bin_candidates)
    endif()
    unset(_omega_try)

    if(_omega_vcpkg_bin)
        message(STATUS "vcpkg DLLs: copy from ${_omega_vcpkg_bin}")
        add_custom_command(TARGET Omega POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${_omega_vcpkg_bin}"
                "$<TARGET_FILE_DIR:Omega>"
            COMMENT "Copying vcpkg runtime DLLs (libcurl + transitive deps) next to Omega.exe"
        )
    else()
        message(STATUS "vcpkg DLLs: NOT FOUND -- Omega.exe will link but will fail to launch.")
        message(STATUS "vcpkg DLLs: install curl with `C:/vcpkg/vcpkg/vcpkg.exe install curl:x64-windows`")
        message(STATUS "vcpkg DLLs: then re-run cmake -B build (delete build/ first if needed).")
    endif()
    unset(_omega_vcpkg_bin)
endif()

# Push current log to git after every successful build.
# LOG PUSH REMOVED FROM POST_BUILD -- permanent fix for stale-hash problem.
# Previously push_log.ps1 ran here after every build, pushing logs/latest.log
# as a new git commit. This moved HEAD forward to a log-only commit DURING
# the deploy, so the stamp ended up with the log-push hash not the source hash.
# Even with walk-commit source-hash detection, the timing created an
# unavoidable race between "build finishes" and "stamp is written".
#
# Fix: log pushing is now manual only. The log is still written to disk.
# Push manually when needed: powershell -File C:\Omega\push_log.ps1 -RepoRoot C:\Omega
# Or trigger it from DEPLOY_OMEGA.ps1 AFTER the stamp is written (see step [6/9]).

# =============================================================================
# OmegaBacktest -- native C++ backtester (no FIX / OpenSSL / WS deps)
# 500K-2M ticks/sec vs ~21K in Python.  Build: cmake --build build --target OmegaBacktest
# =============================================================================
add_executable(OmegaBacktest
    backtest/OmegaBacktest.cpp
    src/SymbolConfig.cpp
)
target_include_directories(OmegaBacktest PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/backtest
)
if(MSVC)
    target_compile_options(OmegaBacktest PRIVATE
        /O2 /std:c++20 /W3
        /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
        /FI "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(OmegaBacktest PRIVATE
        _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS OMEGA_BACKTEST
    )
    target_link_libraries(OmegaBacktest PRIVATE ws2_32)
else()
    target_compile_options(OmegaBacktest PRIVATE
        -O3 -std=c++20 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-reorder
        -include "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(OmegaBacktest PRIVATE OMEGA_BACKTEST)
    target_link_libraries(OmegaBacktest PRIVATE pthread)
endif()
add_custom_command(TARGET OmegaBacktest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/config/symbols.ini"
        "$<TARGET_FILE_DIR:OmegaBacktest>/symbols.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/config/omega_config.ini"
        "$<TARGET_FILE_DIR:OmegaBacktest>/omega_config.ini"
    COMMENT "Copying config -> OmegaBacktest"
)

# =============================================================================
# IndexBacktest -- NAS100 cross-engine backtest exercising the index engines
# (IndexFlow + IndexHybridBracket + IndexMacroCrash) and the Bug #3 cross-
# engine gate from include/tick_indices.hpp / include/globals.hpp.
#
# Reads HistData NSXUSD tick CSVs directly. Self-contained.
#
# Build:  cmake --build build --target IndexBacktest --config Release
# Run:    ./build/IndexBacktest <ticks.csv> [--out f.csv] [--gap N] [--no-gate]
# =============================================================================
add_executable(IndexBacktest
    backtest/IndexBacktest.cpp
)
target_include_directories(IndexBacktest PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/backtest
)
if(MSVC)
    target_compile_options(IndexBacktest PRIVATE
        /O2 /std:c++20 /W3
        /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
    )
    target_compile_definitions(IndexBacktest PRIVATE
        _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS OMEGA_BACKTEST
    )
else()
    target_compile_options(IndexBacktest PRIVATE
        -O2 -std=c++20 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-reorder
    )
    target_compile_definitions(IndexBacktest PRIVATE OMEGA_BACKTEST)
endif()

# =============================================================================
# TsmomCellBacktest -- Phase 2a dual-engine harness for the CellEngine refactor.
# Drives V1 (TsmomPortfolio in TsmomEngine.hpp) and V2 (CellPortfolio<TsmomStrategy>
# in CellEngine.hpp + TsmomStrategy.hpp) over the same H1 OHLC corpus and emits
# two CSV ledgers we can byte-diff. With --max-pos 1 the ledgers MUST match.
#
# Self-contained: no OpenSSL, no FIX, no time-shim (the cell engines use the
# now_ms passed in by the harness, never std::time / std::chrono::*).
#
# Build:
#   cmake --build build --target TsmomCellBacktest --config Release
# Run (Mac/Linux):
#   ./build/TsmomCellBacktest
# Run (Windows):
#   .\build\Release\TsmomCellBacktest.exe
# =============================================================================
add_executable(TsmomCellBacktest
    backtest/TsmomCellBacktest.cpp
)
target_include_directories(TsmomCellBacktest PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)
if(MSVC)
    target_compile_options(TsmomCellBacktest PRIVATE
        /O2 /std:c++20 /W3
        /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
    )
    target_compile_definitions(TsmomCellBacktest PRIVATE
        _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(TsmomCellBacktest PRIVATE
        -O3 -std=c++20 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function
    )
    target_link_libraries(TsmomCellBacktest PRIVATE pthread)
endif()

# =============================================================================
# OmegaSweepHarness -- parameter sweep harness (S51 X3)
#
# Runs a pairwise 2-factor parameter grid: for each engine, vary 2 of 5 params
# on a 7x7 geometric grid (0.5x..2.0x). C(5,2)=10 pairs * 49 = 490 combos per
# engine; 4 engines (HBG, AsianRange, VWAPStretch, EMACross). DXY is wired
# but skipped at runtime (no DXY tick stream).
#
# Standalone target: does not link OpenSSL, does not affect Omega.exe or
# OmegaBacktest. Multithreaded via std::thread (no OpenMP).
#
# OFF BY DEFAULT.
#   The X3 design instantiates std::tuple<HBG_T<0..489>> and equivalents for
#   the other 3 engines (~1,960 template instantiations). MSVC's compiler heap
#   OOMs on this load (observed C1060 on VPS 2026-04-28 after 511s). Clang on
#   Mac handles it fine with -fbracket-depth=1024. The harness is therefore
#   gated behind OMEGA_BUILD_SWEEP=ON so the VPS production build (Omega.exe
#   only via --target Omega) does not pull the harness in by default.
#
# Build (Mac / Clang only):
#   cmake .. -DOMEGA_BUILD_SWEEP=ON -DCMAKE_BUILD_TYPE=Release
#   cmake --build . --target OmegaSweepHarness --config Release
# =============================================================================
option(OMEGA_BUILD_SWEEP "Build OmegaSweepHarness (Mac/Clang dev only; OOMs MSVC)" OFF)
option(OMEGA_SWEEP_DIAG "Enable diagnostic counters in OmegaSweepHarness (D7/HBG-DIAG; off for normal sweep runs)" OFF)
if(OMEGA_BUILD_SWEEP)
    add_executable(OmegaSweepHarness
        backtest/OmegaSweepHarness.cpp
    )
    target_include_directories(OmegaSweepHarness PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/backtest
    )
    if(MSVC)
        target_compile_options(OmegaSweepHarness PRIVATE
            /O2 /std:c++20 /W3
            /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
            /FI "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
        )
        target_compile_definitions(OmegaSweepHarness PRIVATE
            _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
            _CRT_SECURE_NO_WARNINGS OMEGA_BACKTEST
        )
        target_link_libraries(OmegaSweepHarness PRIVATE ws2_32)
    else()
        target_compile_options(OmegaSweepHarness PRIVATE
            -O3 -std=c++20 -Wall -Wextra
            -Wno-unused-parameter -Wno-unused-function -Wno-reorder
            -include "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
        )
        # S51 X3: Clang's default fbracket-depth=256 is exceeded by the
        # 490-element fold expression in the sweep harness's tuple dispatch.
        # Raise to 1024 so 490 (with headroom) parses. GCC does not have or need
        # this flag, so it is gated on Clang. Mac builds use Clang by default.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
            target_compile_options(OmegaSweepHarness PRIVATE -fbracket-depth=1024)
        endif()
        target_compile_definitions(OmegaSweepHarness PRIVATE OMEGA_BACKTEST)
        target_link_libraries(OmegaSweepHarness PRIVATE pthread)
    endif()
    if(OMEGA_SWEEP_DIAG)
        target_compile_definitions(OmegaSweepHarness PRIVATE OMEGA_SWEEP_DIAG)
        message(STATUS "OmegaSweepHarness:      diag enabled (OMEGA_SWEEP_DIAG=ON)")
    endif()
endif()

# =============================================================================
# OmegaSweepHarnessCRTP -- CRTP-architecture parameter sweep harness
#                         (S51 1A.1.b post-G4, 2026-04-29)
#
# Same pairwise 2-factor 490-combo sweep as OmegaSweepHarness, but driving
# CRTP-based engines (omega::sweep_crtp::*) defined in
# include/SweepableEnginesCRTP.hpp. The engine logic lives in shared
# *Base<Derived> classes; per-combo specialisation is supplied by Traits<I>
# derived classes via the canonical CRTP downcast.
#
# Independent target: shares no TU with OmegaSweepHarness. Either may be
# built without affecting the other. Output schema is identical so existing
# downstream analysis scripts work without modification.
#
# Build (Mac / Clang only -- same OOM caveat as OmegaSweepHarness on MSVC):
#   cmake .. -DOMEGA_BUILD_SWEEP_CRTP=ON -DCMAKE_BUILD_TYPE=Release
#   cmake --build . --target OmegaSweepHarnessCRTP --config Release
# =============================================================================
option(OMEGA_BUILD_SWEEP_CRTP "Build OmegaSweepHarnessCRTP (Mac/Clang dev only; OOMs MSVC)" OFF)
if(OMEGA_BUILD_SWEEP_CRTP)
    add_executable(OmegaSweepHarnessCRTP
        backtest/OmegaSweepHarnessCRTP.cpp
    )
    target_include_directories(OmegaSweepHarnessCRTP PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/backtest
    )
    if(MSVC)
        target_compile_options(OmegaSweepHarnessCRTP PRIVATE
            /O2 /std:c++20 /W3
            /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
            /FI "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
        )
        target_compile_definitions(OmegaSweepHarnessCRTP PRIVATE
            _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
            _CRT_SECURE_NO_WARNINGS OMEGA_BACKTEST
        )
        target_link_libraries(OmegaSweepHarnessCRTP PRIVATE ws2_32)
    else()
        target_compile_options(OmegaSweepHarnessCRTP PRIVATE
            -O3 -std=c++20 -Wall -Wextra
            -Wno-unused-parameter -Wno-unused-function -Wno-reorder
            -include "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
        )
        # Same Clang fold-expression bracket-depth lift as OmegaSweepHarness:
        # 490-element fold over std::index_sequence<490> exceeds the default
        # fbracket-depth=256.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
            target_compile_options(OmegaSweepHarnessCRTP PRIVATE -fbracket-depth=1024)
        endif()
        target_compile_definitions(OmegaSweepHarnessCRTP PRIVATE OMEGA_BACKTEST)
        target_link_libraries(OmegaSweepHarnessCRTP PRIVATE pthread)
    endif()
endif()

# =============================================================================
# OmegaEdgeFinder -- edge-discovery framework (S51 X3.4)
#
# Two standalone targets:
#   OmegaEdgeFinderExtract  -- C++ tick -> bar-close panel extractor (one-pass,
#                              ~10 min on the 5GB XAUUSD CSV). Writes a flat
#                              binary panel (~150 MB) consumed by Python
#                              analytics. Header-only-library based; no .cpp
#                              files except the entrypoint.
#   OmegaEdgeFinderTest     -- leakage / forward-fill unit test. Feeds a
#                              synthetic tick stream with a known +50 future
#                              spike and asserts that bar t's features do NOT
#                              reflect bar t+1, while forward-return cells DO.
#
# Both are self-contained: no OpenSSL, no shim, no Omega.exe / sweep state.
# Build: cmake --build build --target OmegaEdgeFinderExtract --config Release
#        cmake --build build --target OmegaEdgeFinderTest    --config Release
# Run:   ./OmegaEdgeFinderTest
#        ./OmegaEdgeFinderExtract --in <ticks.csv> --out <panel.bin> --verbose
# =============================================================================
add_executable(OmegaEdgeFinderExtract
    backtest/edgefinder/extractor/EventExtractor.cpp
)
target_include_directories(OmegaEdgeFinderExtract PRIVATE
    ${CMAKE_SOURCE_DIR}/backtest/edgefinder/extractor
)
if(MSVC)
    target_compile_options(OmegaEdgeFinderExtract PRIVATE
        /O2 /std:c++20 /W3
        /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
    )
    target_compile_definitions(OmegaEdgeFinderExtract PRIVATE
        _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(OmegaEdgeFinderExtract PRIVATE
        -O3 -std=c++20 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-misleading-indentation
    )
    target_link_libraries(OmegaEdgeFinderExtract PRIVATE pthread)
endif()

add_executable(OmegaEdgeFinderTest
    backtest/edgefinder/tests/test_extractor_no_lookahead.cpp
)
target_include_directories(OmegaEdgeFinderTest PRIVATE
    ${CMAKE_SOURCE_DIR}/backtest/edgefinder/extractor
)
if(MSVC)
    target_compile_options(OmegaEdgeFinderTest PRIVATE
        /O2 /std:c++20 /W3
        /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
    )
    target_compile_definitions(OmegaEdgeFinderTest PRIVATE
        _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(OmegaEdgeFinderTest PRIVATE
        -O2 -std=c++20 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-comment
    )
endif()

# =============================================================================
# verify_xauusd_fvg -- synthetic-trace verifier for XauusdFvgEngine.
#                     2026-05-02 with the §7 integration of the FVG engine.
#
# Replays bars_XAUUSD_15min_*.csv (pre-flattened from the original .pkl by
# scripts/dump_bars_to_csv.py) through omega::XauusdFvgEngine and asserts
# row-for-row match against trades_top.csv produced by
# scripts/fvg_pnl_backtest_v3.py.
#
# Self-contained: no OpenSSL / no libcurl / no FIX / no time-shim. Compiles
# with -DOMEGA_BACKTEST so the engine's g_macroDetector / g_news_blackout
# call sites compile out cleanly.
#
# Build:  cmake --build build --target verify_xauusd_fvg --config Release
# Run:    ./build/verify_xauusd_fvg <bars.csv> <trades_top.csv>
# =============================================================================
add_executable(verify_xauusd_fvg
    backtest/verify_xauusd_fvg.cpp
)
target_include_directories(verify_xauusd_fvg PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)
if(MSVC)
    target_compile_options(verify_xauusd_fvg PRIVATE
        /O2 /std:c++20 /W3
        /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
    )
    target_compile_definitions(verify_xauusd_fvg PRIVATE
        _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS OMEGA_BACKTEST
    )
else()
    target_compile_options(verify_xauusd_fvg PRIVATE
        -O2 -std=c++20 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-reorder
    )
    target_compile_definitions(verify_xauusd_fvg PRIVATE OMEGA_BACKTEST)
endif()

message(STATUS "========================================")
message(STATUS "OMEGA -- Commodities & Indices System")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "OpenSSL:    ${OPENSSL_VERSION}")
message(STATUS "libcurl:    ${CURL_VERSION_STRING} (Step 5/7 market-data proxy)")
message(STATUS "Git hash:   regenerated on every build")
message(STATUS "OmegaBacktest:          standalone (no OpenSSL / no libcurl)")
message(STATUS "TsmomCellBacktest:      standalone (no OpenSSL) [Phase 2a CellEngine refactor]")
if(OMEGA_BUILD_SWEEP)
    message(STATUS "OmegaSweepHarness:      standalone (no OpenSSL) [OMEGA_BUILD_SWEEP=ON]")
else()
    message(STATUS "OmegaSweepHarness:      SKIPPED (OMEGA_BUILD_SWEEP=OFF; default)")
endif()
if(OMEGA_BUILD_SWEEP_CRTP)
    message(STATUS "OmegaSweepHarnessCRTP:  standalone (no OpenSSL) [OMEGA_BUILD_SWEEP_CRTP=ON]")
else()
    message(STATUS "OmegaSweepHarnessCRTP:  SKIPPED (OMEGA_BUILD_SWEEP_CRTP=OFF; default)")
endif()
message(STATUS "OmegaEdgeFinderExtract: standalone (no OpenSSL) [S51 X3.4]")
message(STATUS "OmegaEdgeFinderTest:    standalone (no OpenSSL) [S51 X3.4]")
message(STATUS "verify_xauusd_fvg:      standalone (no OpenSSL) [XauusdFvg synthetic-trace verifier]")
message(STATUS "========================================")

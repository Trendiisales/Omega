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

# IBKR execution migration (2026-06-16): link the vendored TWS API client into
# Omega.exe so the native C++ IbkrExecutionEngine can route orders to IBKR.
# OMEGA_WITH_IBKR (defined below, Omega target only) gates every IBKR code path
# (IbkrExecGlobal.hpp / order_exec routing / engine_init) -- other targets
# (OmegaBacktest, GapShortEngine, ...) do NOT define it and are unaffected.
# FIX/OmegaFIX/BlackBull path stays intact; IBKR routing is runtime-gated too.
file(GLOB TWS_SOURCES_OMEGA ${CMAKE_SOURCE_DIR}/third_party/twsapi/client/*.cpp)
# src/ibkr_exec.cpp is the ONLY Omega TU that includes the TWS API headers
# (isolates the TWS C <stdlib.h> from the main TU -> no std::atoll pollution).
# bid64_integer.cpp provides the Intel-RDFP __bid64_* symbols that TWS Decimal.cpp
# needs (same stub GapShortEngine links). STUB today -> replace with real
# IntelRDFPMathLib2U before any LIVE order SIZING (paper fine). See ibkr/README.
set(IBKR_OMEGA_SOURCES
    ${CMAKE_SOURCE_DIR}/src/ibkr_exec.cpp
    # src/bigcap_momo_ibkr.cpp -- the in-process BigCapMomo engine (its own IBKR
    # scanner/data thread). Same TWS-isolation rule as ibkr_exec.cpp: only this
    # TU sees the TWS headers; the main TU includes only the thin BigCapMomoIbkr.hpp.
    ${CMAKE_SOURCE_DIR}/src/bigcap_momo_ibkr.cpp
    # src/nq_momo_ibkr.cpp -- the in-process NQ/MNQ futures momentum engine (S-2026-06-25,
    # the one validated new edge). Own IBKR 5m-bar data thread; same TWS-isolation rule.
    # Main TU includes only the thin NqMomoIbkr.hpp.
    ${CMAKE_SOURCE_DIR}/src/nq_momo_ibkr.cpp
    ${CMAKE_SOURCE_DIR}/ibkr/bid64_integer.cpp)
if(MSVC)
    # Vendored TWS API + the TU that includes its headers emit warnings; Omega
    # builds /W4 /WX (warnings = errors) -> C2220. Suppress on the TWS sources +
    # ibkr_exec.cpp ONLY (mirrors GapShortEngine /W0). Omega's own code keeps /W4 /WX.
    set_source_files_properties(${TWS_SOURCES_OMEGA} ${IBKR_OMEGA_SOURCES}
                                PROPERTIES COMPILE_OPTIONS "/W0;/WX-")
endif()
add_executable(Omega ${SOURCES} ${IBKR_OMEGA_SOURCES} ${TWS_SOURCES_OMEGA})

# Precompiled Header -- cuts recompile time 40-60% on MSVC /O2 (the main.cpp monolith
# re-parses all of Windows/OpenSSL/STL on EVERY engine-header change without it; that is
# the bulk of deploy time + it grows as engines are added). omega_pch.hpp holds only the
# stable Windows/OpenSSL/STL includes -- NO Omega engine .hpp (those change often).
# S-2026-06-24 RE-ENABLED behind OMEGA_USE_PCH (default ON). The prior LNK1181
# (cmake_pch.obj path mismatch) came from toggling PCH on a STALE CMakeCache -> the first
# PCH build MUST be a clean reconfigure: run `OMEGA.ps1 deploy -Clean` ONCE to bed it in,
# then incrementals are 40-60% faster. If it still LNK1181s, disable with -DOMEGA_USE_PCH=OFF
# (the running binary is untouched by a failed build -- zero live risk).
option(OMEGA_USE_PCH "Precompiled header for the Omega target (40-60% faster MSVC recompile)" ON)
if(OMEGA_USE_PCH)
    target_precompile_headers(Omega PRIVATE include/omega_pch.hpp)
    message(STATUS "Omega PCH: ON (omega_pch.hpp) -- first build after toggling must be -Clean")
else()
    message(STATUS "Omega PCH: OFF (-DOMEGA_USE_PCH=OFF)")
endif()

target_include_directories(Omega PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/gui
    ${OPENSSL_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/twsapi/client
    ${CMAKE_SOURCE_DIR}/ibkr   # RiskManager.hpp for src/bigcap_momo_ibkr.cpp
)

target_link_libraries(Omega
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    ws2_32
)

# IBKR execution path gate (Omega target only). TWS headers also want these
# winsock defines; harmless if Omega already sets them (same values).
target_compile_definitions(Omega PRIVATE OMEGA_WITH_IBKR)

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

# S-2026-06-20 DEPLOY-SPEED: do NOT include() omega_version.cmake.
# The OMEGA_GIT_HASH/_DATE/_BUILD_TIME CMake-scope vars it set are DEAD --
# compile-defines were removed 2026-06-11 (the git hash now reaches the binary
# ONLY via include/version_generated.hpp, generated below copy-if-different).
# include()-ing it made omega_version.cmake a TRACKED configure dependency, and
# the build-time omega_version target rewrites that file every build -> CMake
# reconfigured mid-build on EVERY deploy -> MSBuild rebuilt all 9 Omega-own TUs
# even when nothing changed (~90s/deploy wasted; only main.cpp actually depends
# on the hash, via the header). No include() = no tracked dep = no reconfigure.
# The configure-time execute_process above still seeds version_generated.hpp for
# a clean first build; the build-time target keeps it fresh thereafter.

# S-2026-06-11: the git hash is delivered via include/version_generated.hpp
# (generated above at configure for the first build, and regenerated at BUILD
# time by the omega_version target below). Compile-defines were REMOVED — they
# only refreshed at configure time, so incremental `cmake --build` shipped a
# stale badge while running new code (chronic deploy-verification breakage).
# main.cpp #include's the header (single source of truth); build time = __DATE__.
# NOTE: deliberately does NOT pass -DOMEGA_FORCE_GIT_HASH. That override is for
# OMEGA.ps1's explicit deploy stamp only; passing the CMake-scope variable here
# re-applied a STALE forced hash cached from the last official deploy (the badge
# showed 7f8a044a while HEAD was f1f6c3b). At build time, in the real checkout,
# `git rev-parse HEAD` IS the truth — let UpdateGitHash read it directly.
add_custom_target(omega_version ALL
    COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DOUTPUT_FILE=${VERSION_CMAKE_FILE}
            -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
            -P ${CMAKE_SOURCE_DIR}/cmake/UpdateGitHash.cmake
    BYPRODUCTS ${CMAKE_SOURCE_DIR}/include/version_generated.hpp
    COMMENT "[Omega] Regenerating git-hash header (build-time, copy-if-different)"
    VERBATIM
)
add_dependencies(Omega omega_version)

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
# BatchEngineRunner -- generalized engine-zoo batch BT: array of (engine x config)
# cells driven in ONE tick pass, per-cell PF/net split both-halves. Models on
# OmegaBacktest (no FIX/curl/winsock). Build: cmake --build build --target BatchEngineRunner
# =============================================================================
add_executable(BatchEngineRunner
    backtest/batch_engine_runner.cpp
    src/SymbolConfig.cpp
)
target_include_directories(BatchEngineRunner PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/backtest
)
if(MSVC)
    target_compile_options(BatchEngineRunner PRIVATE
        /O2 /std:c++20 /W3
        /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
        /FI "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(BatchEngineRunner PRIVATE
        _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS OMEGA_BACKTEST
    )
    target_link_libraries(BatchEngineRunner PRIVATE ws2_32)
else()
    target_compile_options(BatchEngineRunner PRIVATE
        -O3 -std=c++20 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-reorder
        -include "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(BatchEngineRunner PRIVATE OMEGA_BACKTEST)
    target_link_libraries(BatchEngineRunner PRIVATE pthread)
endif()

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
# VWAPReversionBacktest -- dedicated harness for VWAPReversionEngine
# Built to validate the S37-H-followup in-flight cut fields (LOSS_CUT_PCT /
# BE_ARM_PCT / BE_BUFFER_PCT) against historical per-symbol tick data.
#
# Build:  cmake --build build --target VWAPReversionBacktest --config Release
# Run:    ./build/VWAPReversionBacktest <ticks.csv> --symbol <SYM> [options]
# Symbols: US500.F | USTEC.F | GER40 | EURUSD
# Modes:   --mode baseline | tuned (default tuned)
# See backtest/VWAPReversionBacktest.cpp header for full usage.
# =============================================================================
add_executable(VWAPReversionBacktest
    backtest/VWAPReversionBacktest.cpp
)
target_include_directories(VWAPReversionBacktest PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/backtest
)
if(MSVC)
    target_compile_options(VWAPReversionBacktest PRIVATE
        /O2 /std:c++20 /W3
        /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
        /FI "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(VWAPReversionBacktest PRIVATE
        _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS OMEGA_BACKTEST
    )
else()
    target_compile_options(VWAPReversionBacktest PRIVATE
        -O3 -std=c++20 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-reorder
        -include "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(VWAPReversionBacktest PRIVATE OMEGA_BACKTEST)
endif()

# =============================================================================
# LatencyEdgeBacktest -- harness for LatencyEdgeEngines (Gold)
# Drives GoldSpreadDislocation + GoldEventCompression directly under
# OmegaTimeShim so session/event-schedule gates resolve against tape time.
#
# Build: cmake --build build --target LatencyEdgeBacktest --config Release
# Run:   ./build/LatencyEdgeBacktest <ticks.csv> [options]
# =============================================================================
add_executable(LatencyEdgeBacktest
    backtest/LatencyEdgeBacktest.cpp
)
target_include_directories(LatencyEdgeBacktest PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/backtest
)
if(MSVC)
    target_compile_options(LatencyEdgeBacktest PRIVATE
        /O2 /std:c++20 /W3
        /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
        /FI "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(LatencyEdgeBacktest PRIVATE
        _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS OMEGA_BACKTEST
    )
else()
    target_compile_options(LatencyEdgeBacktest PRIVATE
        -O3 -std=c++20 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-reorder
        -include "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(LatencyEdgeBacktest PRIVATE OMEGA_BACKTEST)
endif()

# =============================================================================
# UstecTrendFollow5mBacktest -- dedicated harness for UstecTrendFollow5mEngine
# Built to answer the promotion-gate question documented at
# include/engine_init.hpp:964-967: "S63 + S37 widened SL/TP profile net-positive
# on USTEC fresh tape". Mirrors VWAPReversionBacktest pattern: instantiates the
# real omega::UstecTrendFollow5mEngine, drives tick + 5m-bar from CSV, exposes
# the S63 trio (LOSS_CUT_PCT / BE_ARM_PCT / BE_BUFFER_PCT) at the CLI; S34-B
# guards + cell sl/tp stay pinned at engine header defaults per part-T phased
# decision (handoff SESSION_HANDOFF_2026-05-14h.md).
#
# Build:  cmake --build build --target UstecTrendFollow5mBacktest --config Release
# Run:    ./build/UstecTrendFollow5mBacktest <ticks.csv> [options]
# See backtest/UstecTrendFollow5mBacktest.cpp header for full usage.
# =============================================================================
add_executable(UstecTrendFollow5mBacktest
    backtest/UstecTrendFollow5mBacktest.cpp
)
target_include_directories(UstecTrendFollow5mBacktest PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/backtest
)
if(MSVC)
    target_compile_options(UstecTrendFollow5mBacktest PRIVATE
        /O2 /std:c++20 /W3
        /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
        /FI "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(UstecTrendFollow5mBacktest PRIVATE
        _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS OMEGA_BACKTEST
    )
else()
    target_compile_options(UstecTrendFollow5mBacktest PRIVATE
        -O3 -std=c++20 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-reorder
        -include "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(UstecTrendFollow5mBacktest PRIVATE OMEGA_BACKTEST)
endif()

# =============================================================================
# XauTrendFollowBacktest -- dedicated harness for the XAU trend-follow trio
# (XauTrendFollow2hEngine / 4hEngine / D1Engine). Added 2026-05-14 (part W).
#
# Skeleton state: --engine 2h is fully wired; --engine 4h and --engine d1
# instantiate the engine + accept the CLI but do not yet feed bars (see
# header comment 'NEXT-SESSION TODO' items 1-2 inside the .cpp). Build is
# green either way.
#
# Mirrors UstecTrendFollow5mBacktest pattern: instantiates the real
# omega::XauTrendFollow*Engine, drives tick + bar from CSV, exposes the
# S63 trio (LOSS_CUT_PCT / BE_ARM_PCT / BE_BUFFER_PCT) at the CLI for the
# planned Phase 3 walk-forward sweep. Engine cell sl/tp stay pinned at
# header defaults per part-T phased decision.
#
# Build:  cmake --build build --target XauTrendFollowBacktest --config Release
# Run:    ./build/XauTrendFollowBacktest <ticks.csv> [options]
# See backtest/XauTrendFollowBacktest.cpp header for full usage.
# =============================================================================
add_executable(XauTrendFollowBacktest
    backtest/XauTrendFollowBacktest.cpp
)
target_include_directories(XauTrendFollowBacktest PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/backtest
)
if(MSVC)
    target_compile_options(XauTrendFollowBacktest PRIVATE
        /O2 /std:c++20 /W3
        /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
        /FI "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(XauTrendFollowBacktest PRIVATE
        _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS OMEGA_BACKTEST
    )
else()
    target_compile_options(XauTrendFollowBacktest PRIVATE
        -O3 -std=c++20 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-reorder
        -include "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(XauTrendFollowBacktest PRIVATE OMEGA_BACKTEST)
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
# EmaPullbackBacktest -- dedicated harness for the EmaPullback portfolio.
# Created 2026-05-14 (part-X follow-up to S76 EmaPullback S63 activation;
# validates the (LOSS_CUT_PCT, BE_ARM_PCT, BE_BUFFER_PCT) tuple against the
# engine's H1 OHLC validation corpus via a 27-cell sweep). Mirrors
# TsmomCellBacktest's standalone pattern -- instantiates omega::EpbPortfolio
# directly, drives it over phase1/signal_discovery/tsmom_warmup_H1.csv, and
# emits a per-trade ledger + per-config summary report. No production code
# touched. Self-contained: no OpenSSL, no FIX, no time-shim.
#
# Build:
#   cmake --build build --target EmaPullbackBacktest --config Release
# Run (Mac/Linux):
#   ./build/EmaPullbackBacktest               # single-run, S63 OFF baseline
#   ./build/EmaPullbackBacktest --loss-cut 1.0 --be-arm 0.40 --be-buffer 0.05
#                                              # single-run, current S76 values
#   ./build/EmaPullbackBacktest --sweep        # 27-cell 3x3x3 grid
# =============================================================================
add_executable(EmaPullbackBacktest
    backtest/EmaPullbackBacktest.cpp
)
target_include_directories(EmaPullbackBacktest PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)
if(MSVC)
    target_compile_options(EmaPullbackBacktest PRIVATE
        /O2 /std:c++20 /W3
        /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
    )
    target_compile_definitions(EmaPullbackBacktest PRIVATE
        _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(EmaPullbackBacktest PRIVATE
        -O3 -std=c++20 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function
    )
    target_link_libraries(EmaPullbackBacktest PRIVATE pthread)
endif()

# =============================================================================
# S37-Z 2026-05-28 -- bulk add_executable for backtest binaries that have
# source files in backtest/ but no cmake target. Future audit sessions can
# build any of these via `cmake --build build --target <name>`.
#
# Each target is built standalone (no OpenSSL / libcurl / Windows-only
# headers). If a source file requires globals/types from main.cpp it will
# fail to compile -- the build error tells us which need a different setup.
# =============================================================================
set(EXTRA_BT_TARGETS
    GoldUltimateBacktest
    Ger40UltimateBacktest
    Spx500UltimateBacktest
    Nas100UltimateBacktest
    IndexFlowBacktest
    IndexBracketBacktest
    IndexOpeningRangeBacktest
    IndexVwapReversionBacktest
    IndexNoiseBandMomentumBacktest
    XauEmaPullbackBacktest
    XauFvgBacktest
    GoldTrendEnsembleBacktest
    TsmomStaggerBacktest
)
foreach(BT_TGT IN LISTS EXTRA_BT_TARGETS)
    if(EXISTS "${CMAKE_SOURCE_DIR}/backtest/${BT_TGT}.cpp")
        add_executable(${BT_TGT} EXCLUDE_FROM_ALL
            backtest/${BT_TGT}.cpp
        )
        target_include_directories(${BT_TGT} PRIVATE
            ${CMAKE_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/include
        )
        if(MSVC)
            target_compile_options(${BT_TGT} PRIVATE
                /O2 /std:c++20 /W3
                /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
            )
            target_compile_definitions(${BT_TGT} PRIVATE
                _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
                _CRT_SECURE_NO_WARNINGS OMEGA_BACKTEST
            )
        else()
            target_compile_options(${BT_TGT} PRIVATE
                -O3 -std=c++20 -Wall -Wextra
                -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
                -Wno-unused-but-set-variable -Wno-reorder
            )
            target_compile_definitions(${BT_TGT} PRIVATE OMEGA_BACKTEST)
            target_link_libraries(${BT_TGT} PRIVATE pthread)
        endif()
    endif()
endforeach()

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

# =============================================================================
# gold_bracket_counterfactual -- one-shot harness asking "what would
#                                 g_bracket_gold have made on a given tape?"
#                                 Built 2026-05-28 for S37-X observability fix.
# =============================================================================
add_executable(gold_bracket_counterfactual
    backtest/gold_bracket_counterfactual.cpp
)
target_include_directories(gold_bracket_counterfactual PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)
if(MSVC)
    target_compile_options(gold_bracket_counterfactual PRIVATE
        /O2 /std:c++20 /W3
        /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
    )
    target_compile_definitions(gold_bracket_counterfactual PRIVATE
        _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS OMEGA_BACKTEST
    )
else()
    target_compile_options(gold_bracket_counterfactual PRIVATE
        -O3 -std=c++20 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
        -Wno-unused-but-set-variable -Wno-reorder
    )
    target_compile_definitions(gold_bracket_counterfactual PRIVATE OMEGA_BACKTEST)
    target_link_libraries(gold_bracket_counterfactual PRIVATE pthread)
endif()

# =============================================================================
# bracket_gold_2yr_audit -- 2-year XAUUSD backtest for g_bracket_gold.
#                           Built 2026-05-28 (S37-Y) after operator override
#                           re-enabled g_disable_bracket_gold. Re-validates
#                           the 2026-04-30 4-week audit with per-session
#                           breakdown (London/NY/Asia/Late_NY).
#
# Build: cmake --build build --target bracket_gold_2yr_audit -j
# Run:   ./build/bracket_gold_2yr_audit /Users/jo/Tick/2yr_XAUUSD_tick_fresh.csv
# =============================================================================
add_executable(bracket_gold_2yr_audit
    backtest/bracket_gold_2yr_audit.cpp
)
target_include_directories(bracket_gold_2yr_audit PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/backtest
)
if(MSVC)
    target_compile_options(bracket_gold_2yr_audit PRIVATE
        /O2 /std:c++20 /W3
        /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
        /FI "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(bracket_gold_2yr_audit PRIVATE
        _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS OMEGA_BACKTEST
    )
else()
    target_compile_options(bracket_gold_2yr_audit PRIVATE
        -O3 -std=c++20 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
        -Wno-unused-but-set-variable -Wno-reorder
        -include "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(bracket_gold_2yr_audit PRIVATE OMEGA_BACKTEST)
    target_link_libraries(bracket_gold_2yr_audit PRIVATE pthread)
endif()

# =============================================================================
# bracket_gold_sweep -- multi-threaded parameter sweep harness.
#                       Built 2026-05-28 after the 2yr_audit returned all
#                       sessions negative. Loads tape into RAM once, replays
#                       in parallel against many engine configurations.
#
# Build: cmake --build build --target bracket_gold_sweep -j
# Run:   ./build/bracket_gold_sweep /tmp/xau_last12mo.csv 8
# =============================================================================
add_executable(bracket_gold_sweep
    backtest/bracket_gold_sweep.cpp
)
target_include_directories(bracket_gold_sweep PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/backtest
)
if(MSVC)
    target_compile_options(bracket_gold_sweep PRIVATE
        /O2 /std:c++20 /W3
        /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
        /FI "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(bracket_gold_sweep PRIVATE
        _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS OMEGA_BACKTEST
    )
else()
    target_compile_options(bracket_gold_sweep PRIVATE
        -O3 -std=c++20 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
        -Wno-unused-but-set-variable -Wno-reorder
        -include "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(bracket_gold_sweep PRIVATE OMEGA_BACKTEST)
    target_link_libraries(bracket_gold_sweep PRIVATE pthread)
endif()

# =============================================================================
# disabled_engine_audit -- multi-engine CRTP audit harness for
#                          re-validating audit-disabled XAUUSD engines.
# Built 2026-05-28 (S37-Z / task #9). std::tuple of CRTP-driver wrappers
# around production engine classes; single tape replay feeds all drivers
# via fold expression.
#
# Build: cmake --build build --target disabled_engine_audit -j
# Run:   ./build/disabled_engine_audit /Users/jo/Tick/2yr_XAUUSD_tick_fresh.csv
# =============================================================================
add_executable(disabled_engine_audit
    backtest/disabled_engine_audit.cpp
)
target_include_directories(disabled_engine_audit PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/backtest
)
if(MSVC)
    target_compile_options(disabled_engine_audit PRIVATE
        /O2 /std:c++20 /W3
        /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
        /FI "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(disabled_engine_audit PRIVATE
        _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS OMEGA_BACKTEST
    )
else()
    target_compile_options(disabled_engine_audit PRIVATE
        -O3 -std=c++20 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
        -Wno-unused-but-set-variable -Wno-reorder
        -include "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(disabled_engine_audit PRIVATE OMEGA_BACKTEST)
    target_link_libraries(disabled_engine_audit PRIVATE pthread)
endif()

# =============================================================================
# goldstack_subs_audit -- 10 GoldStack sub-engine CRTP audit harness.
# Built 2026-05-29 (S37-Z task#18) to re-validate the doubled abs-pt
# thresholds in include/GoldEngineStack.hpp after the $2400->$4700 price
# drift. Synthesises GoldSnapshot from raw ticks (no GoldFeatures wall-clock
# dependency) and intercepts libc time() at link scope so all 21
# std::time(nullptr) sites in the engine stack see tape time.
#
# Build: cmake --build build --target goldstack_subs_audit -j
# Run:   ./build/goldstack_subs_audit /Users/jo/Tick/2yr_XAUUSD_tick_fresh.csv \
#                                     /tmp/goldstack_subs_audit.txt
# =============================================================================
add_executable(goldstack_subs_audit
    backtest/goldstack_subs_audit.cpp
)
target_include_directories(goldstack_subs_audit PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/backtest
)
if(MSVC)
    target_compile_options(goldstack_subs_audit PRIVATE
        /O2 /std:c++20 /W3
        /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
        /FI "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(goldstack_subs_audit PRIVATE
        _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS OMEGA_BACKTEST
    )
else()
    target_compile_options(goldstack_subs_audit PRIVATE
        -O3 -std=c++20 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
        -Wno-unused-but-set-variable -Wno-reorder
        -include "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(goldstack_subs_audit PRIVATE OMEGA_BACKTEST)
    target_link_libraries(goldstack_subs_audit PRIVATE pthread)
endif()

# goldstack_subs_audit_native -- same harness, native combined-corpus parser
# (timestamp_ms,askPrice,bidPrice) + GSA_COST_STRESS_PTS env cost stress.
# Used 2026-06-17 gold_stack faithful re-validation.
add_executable(goldstack_subs_audit_native
    backtest/goldstack_subs_audit_native.cpp
)
target_include_directories(goldstack_subs_audit_native PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/backtest
)
if(MSVC)
    target_compile_options(goldstack_subs_audit_native PRIVATE
        /O2 /std:c++20 /W3
        /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
        /FI "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(goldstack_subs_audit_native PRIVATE
        _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS OMEGA_BACKTEST
    )
else()
    target_compile_options(goldstack_subs_audit_native PRIVATE
        -O3 -std=c++20 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
        -Wno-unused-but-set-variable -Wno-reorder
        -include "${CMAKE_SOURCE_DIR}/backtest/OmegaTimeShim.hpp"
    )
    target_compile_definitions(goldstack_subs_audit_native PRIVATE OMEGA_BACKTEST)
    target_link_libraries(goldstack_subs_audit_native PRIVATE pthread)
endif()

# =============================================================================
# xau_d1_zoo_audit -- standalone harness for the Xau H4/D1 engine cohort.
#                     Used by S37 Phase H trail audit (2026-05-27b) to test
#                     gated stage_trail on PullbackContH4/D1, TurtleD1,
#                     SwingBreakD1, DojiRejD1, OutsideBarD1, StopRunD1.
#
# Build:  cmake --build build --target xau_d1_zoo_audit
# Run:    ./build/xau_d1_zoo_audit              # baseline
#         STAGE_TRAIL=1 ./build/xau_d1_zoo_audit  # all engines w/ trail on
# =============================================================================
add_executable(xau_d1_zoo_audit
    backtest/xau_d1_zoo_audit.cpp
)
target_include_directories(xau_d1_zoo_audit PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)
if(MSVC)
    target_compile_options(xau_d1_zoo_audit PRIVATE
        /O2 /std:c++20 /W3
        /wd4005 /wd4100 /wd4505 /wd4324 /wd4244
    )
    target_compile_definitions(xau_d1_zoo_audit PRIVATE
        _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS OMEGA_BACKTEST
    )
else()
    target_compile_options(xau_d1_zoo_audit PRIVATE
        -O3 -std=c++20 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
        -Wno-unused-but-set-variable -Wno-reorder
    )
    target_compile_definitions(xau_d1_zoo_audit PRIVATE OMEGA_BACKTEST)
    target_link_libraries(xau_d1_zoo_audit PRIVATE pthread)
endif()

message(STATUS "========================================")
message(STATUS "OMEGA -- Commodities & Indices System")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "OpenSSL:    ${OPENSSL_VERSION}")
message(STATUS "libcurl:    ${CURL_VERSION_STRING} (Step 5/7 market-data proxy)")
message(STATUS "Git hash:   regenerated on every build")
message(STATUS "OmegaBacktest:          standalone (no OpenSSL / no libcurl)")
message(STATUS "TsmomCellBacktest:      standalone (no OpenSSL) [Phase 2a CellEngine refactor]")
message(STATUS "EmaPullbackBacktest:    standalone (no OpenSSL) [part-X S63 sweep harness]")
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

# =============================================================================
# GapShortEngine -- IBKR gap-short (Evan Shunk). Isolated C++ subsystem, links the
# vendored TWS API client + winsock. NOT part of Omega.exe / no FIX / no BlackBull.
# Build (does NOT affect Omega): cmake --build build --target GapShortEngine
# =============================================================================
file(GLOB TWS_SOURCES ${CMAKE_SOURCE_DIR}/third_party/twsapi/client/*.cpp)
add_executable(GapShortEngine
    ibkr/GapShortEngine.cpp
    ibkr/bid64_integer.cpp
    ${TWS_SOURCES}
)
target_include_directories(GapShortEngine PRIVATE ${CMAKE_SOURCE_DIR}/third_party/twsapi/client)
if(MSVC)
    target_compile_options(GapShortEngine PRIVATE /std:c++17 /EHsc /W0)
    target_compile_definitions(GapShortEngine PRIVATE _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(GapShortEngine PRIVATE ws2_32)
else()
    target_compile_options(GapShortEngine PRIVATE -std=c++17 -w)
    target_link_libraries(GapShortEngine PRIVATE pthread)
endif()
message(STATUS "GapShortEngine:         IBKR gap-short C++ subsystem (paper)")

# =============================================================================
# BigCapMomoEngine -- CULLED 2026-06-24 (S-2026-06-24). Build target REMOVED.
# WHY: this standalone exe is the ORPHAN impl of the BigCapMomo thesis. It carries
# ONLY the regime gate + old wide-trail + hard 4h MAXHOLD time-cut. It LACKS every
# post-2026-06-16 protection the two LIVE impls have:
#   - NO gain-protect exit (ATR-trail + BE-ratchet + ride-in-profit) -> the exit-
#     giveback bug (omega-bigcap-exit-givesback): cuts winners flat on the clock.
#   - NO breadth>=2 chop/bear gate.
#   - NO single-entry-per-name-per-day (the IONQ re-entry-at-HOD -$58 loser).
# i.e. if launched it trades the EXACT broken way the operator flagged. It is in NO
# scheduled task, but a buildable exe is a latent footgun. Superseded by the two
# protected impls inside Omega.exe: the bridge g_bigcap_momo (PumpScalpManager) and
# the in-process BigCapMomoIbkr (src/bigcap_momo_ibkr.cpp). Source kept + tombstoned
# + hard-aborts in main(); to ever revive, PORT the protections first.
# (was: add_executable(BigCapMomoEngine ibkr/BigCapMomoEngine.cpp ibkr/bid64_integer.cpp ${TWS_SOURCES}) + deps)
message(STATUS "BigCapMomoEngine:       CULLED 2026-06-24 (unprotected orphan; use in-process BigCapMomoIbkr)")
# =============================================================================

# GapShortDaily -- production daily-loop (enter->monitor->cover, kill switch, locate gate)
add_executable(GapShortDaily ibkr/GapShortDaily.cpp ibkr/bid64_integer.cpp ${TWS_SOURCES})
target_include_directories(GapShortDaily PRIVATE ${CMAKE_SOURCE_DIR}/third_party/twsapi/client)
if(MSVC)
    target_compile_options(GapShortDaily PRIVATE /std:c++17 /EHsc /W0)
    target_compile_definitions(GapShortDaily PRIVATE _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(GapShortDaily PRIVATE ws2_32)
else()
    target_compile_options(GapShortDaily PRIVATE -std=c++17 -w)
    target_link_libraries(GapShortDaily PRIVATE pthread)
endif()
message(STATUS "GapShortDaily:          gap-short production daily-loop")

# CapitulationEngine -- Lance capitulation V-reversal (LONG the bounce), same risk layer
add_executable(CapitulationEngine ibkr/CapitulationEngine.cpp ibkr/bid64_integer.cpp ${TWS_SOURCES})
target_include_directories(CapitulationEngine PRIVATE ${CMAKE_SOURCE_DIR}/third_party/twsapi/client)
if(MSVC)
    target_compile_options(CapitulationEngine PRIVATE /std:c++17 /EHsc /W0)
    target_compile_definitions(CapitulationEngine PRIVATE _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(CapitulationEngine PRIVATE ws2_32)
else()
    target_compile_options(CapitulationEngine PRIVATE -std=c++17 -w)
    target_link_libraries(CapitulationEngine PRIVATE pthread)
endif()
message(STATUS "CapitulationEngine:     Lance capitulation V-reversal (LONG)")

# CapitulationDaily -- production daily-loop (reprotect/trail + entry, kill switch, GTC swing)
add_executable(CapitulationDaily ibkr/CapitulationDaily.cpp ibkr/bid64_integer.cpp ${TWS_SOURCES})
target_include_directories(CapitulationDaily PRIVATE ${CMAKE_SOURCE_DIR}/third_party/twsapi/client)
if(MSVC)
    target_compile_options(CapitulationDaily PRIVATE /std:c++17 /EHsc /W0)
    target_compile_definitions(CapitulationDaily PRIVATE _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(CapitulationDaily PRIVATE ws2_32)
else()
    target_compile_options(CapitulationDaily PRIVATE -std=c++17 -w)
    target_link_libraries(CapitulationDaily PRIVATE pthread)
endif()
message(STATUS "CapitulationDaily:      capitulation production daily-loop (swing)")

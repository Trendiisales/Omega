cmake_minimum_required(VERSION 3.20)
project(Omega VERSION 1.0.0 LANGUAGES CXX)

add_definitions(-D_WINSOCKAPI_ -DWIN32_LEAN_AND_MEAN)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# vcpkg toolchain resolution.
#
# The build needs the vcpkg toolchain to find libcurl (Step 5) and any
# other vcpkg-installed deps. We try in order:
#   1. Caller-supplied -DCMAKE_TOOLCHAIN_FILE on the cmake command line.
#   2. VCPKG_ROOT env var, if set.
#   3. Hardcoded common Windows install paths as a fallback.
# This makes the build robust against shells that forgot to export
# VCPKG_ROOT (which has bitten the QUICK_RESTART deploy at least once
# -- the recovery path correctly kept the previous binary running, but
# the configure aborted with "Could NOT find CURL").
#
# On macOS / Linux, none of these branches fire and find_package falls
# back to the system libcurl, which is what we want.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain file (from VCPKG_ROOT)")
        message(STATUS "vcpkg:      toolchain via VCPKG_ROOT=$ENV{VCPKG_ROOT}")
    else()
        # Fallback: probe common Windows vcpkg install paths. Order matters
        # -- C:/vcpkg/vcpkg/... is the actual layout on the Omega VPS;
        # C:/vcpkg/... is the more common single-level layout.
        set(_omega_vcpkg_candidates
            "C:/vcpkg/vcpkg/scripts/buildsystems/vcpkg.cmake"
            "C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
            "C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake"
        )
        foreach(_omega_vcpkg_try ${_omega_vcpkg_candidates})
            if(EXISTS "${_omega_vcpkg_try}")
                set(CMAKE_TOOLCHAIN_FILE "${_omega_vcpkg_try}"
                    CACHE STRING "vcpkg toolchain file (autodetected fallback)")
                message(STATUS "vcpkg:      toolchain autodetected at ${_omega_vcpkg_try}")
                break()
            endif()
        endforeach()
        unset(_omega_vcpkg_try)
        unset(_omega_vcpkg_candidates)
    endif()
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

# ?? libcurl -- required by src/api/OpenBbProxy.cpp (Step 5 of the Omega
# Terminal build). Used to call OpenBB Hub from the engine binary so the
# OpenBB API token never ships in the React bundle. Backtest targets do
# NOT link curl -- they live in their own add_executable() blocks below
# and the OpenBbProxy translation unit is gated under
# `#ifndef OMEGA_BACKTEST` so even an accidental include compiles to
# nothing in those builds.
#
# Install:
#   macOS:   brew install curl   (system libcurl already works too)
#   Linux:   apt-get install libcurl4-openssl-dev
#   Windows: vcpkg install curl[ssl]:x64-windows
find_package(CURL REQUIRED)

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
    src/api/OpenBbProxy.cpp           # Step 5 Omega Terminal: OpenBB Hub HTTPS proxy
    src/gold_coordinator.cpp
)

add_executable(Omega ${SOURCES})

# ?? Precompiled Header -- cuts recompile time 40-60% on MSVC /O2 ????????????
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

# ?? Build version stamp ??????????????????????????????????????????????????????
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

# ?? Copy config files to build output directory ???????????????????????????????
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

# ?? Push current log to git after every successful build ?????????????????????
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
    COMMENT "Copying config ? OmegaBacktest"
)

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

message(STATUS "========================================")
message(STATUS "OMEGA -- Commodities & Indices System")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "OpenSSL:    ${OPENSSL_VERSION}")
message(STATUS "libcurl:    ${CURL_VERSION_STRING} (Step 5 OpenBB proxy)")
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
message(STATUS "========================================")

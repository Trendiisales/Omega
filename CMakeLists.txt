cmake_minimum_required(VERSION 3.20)
project(Omega VERSION 1.0.0 LANGUAGES CXX)

add_definitions(-D_WINSOCKAPI_ -DWIN32_LEAN_AND_MEAN)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain file")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
    add_compile_options(/W4 /WX /wd4100 /wd4505 /wd4324 /O2 /std:c++20)
    add_definitions(-D_WIN32_WINNT=0x0601 -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -O3 -std=c++20)
endif()

find_package(OpenSSL REQUIRED)

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
)

add_executable(Omega ${SOURCES})

# Precompiled Header -- cuts recompile time 40-60% on MSVC /O2
# omega_pch.hpp contains all stable Windows/OpenSSL/STL includes.
target_precompile_headers(Omega PRIVATE include/omega_pch.hpp)

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
# CandleFlowL2Bt -- standalone L2 backtest, no Omega deps
# Build: cmake --build build --target CandleFlowL2Bt
# Run:   .\Release\CandleFlowL2Bt.exe C:\Omega\logs\l2_ticks_2026-04-10.csv
# =============================================================================
add_executable(CandleFlowL2Bt
    backtest/candle_flow_l2_bt.cpp
)
target_include_directories(CandleFlowL2Bt PRIVATE ${CMAKE_SOURCE_DIR}/backtest)
if(MSVC)
    target_compile_options(CandleFlowL2Bt PRIVATE /O2 /std:c++17 /W3 /wd4100 /wd4505)
    target_compile_definitions(CandleFlowL2Bt PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(CandleFlowL2Bt PRIVATE -O2 -std=c++17 -Wall)
endif()

message(STATUS "========================================")
message(STATUS "OMEGA -- Commodities & Indices System")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "OpenSSL:    ${OPENSSL_VERSION}")
message(STATUS "Git hash:   regenerated on every build")
message(STATUS "OmegaBacktest: standalone (no OpenSSL)")
message(STATUS "========================================")

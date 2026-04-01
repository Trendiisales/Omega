# UpdateGitHash.cmake -- runs at BUILD TIME via add_custom_target(ALL)
# Writes version_generated.hpp + version_sentinel.h every build.
# The sentinel's changing timestamp forces main.cpp to recompile via
# set_property(SOURCE main.cpp APPEND PROPERTY OBJECT_DEPENDS sentinel).

find_package(Git QUIET)

if(GIT_FOUND AND EXISTS "${SOURCE_DIR}/.git")
    # SOURCE HASH FIX: use the last commit that touched real source files.
    # git rev-parse HEAD / origin/main returns the log-push commit when the
    # deploy script pushes logs after building -- that commit has no code changes.
    # The GUI then shows the log-push hash instead of the fix commit, making it
    # impossible to verify what code is running (root cause of 7-week stale binary).
    # Fix: find last commit that touched src/ include/ CMakeLists.txt or *.ini
    execute_process(
        COMMAND ${GIT_EXECUTABLE} log --oneline -1
            -- src include CMakeLists.txt omega_config.ini symbols.ini
               DEPLOY_OMEGA.ps1 OmegaWatchdog.ps1
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE GIT_SRC_LINE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    # Extract short hash from "abcdef1 commit message" format
    string(REGEX REPLACE "^([a-f0-9]+) .*" "\\1" GIT_HASH "${GIT_SRC_LINE}")
    if("${GIT_HASH}" STREQUAL "" OR "${GIT_HASH}" STREQUAL "${GIT_SRC_LINE}")
        # Fallback to HEAD short hash if log parse failed
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY "${SOURCE_DIR}"
            OUTPUT_VARIABLE GIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
    execute_process(
        COMMAND ${GIT_EXECUTABLE} log -1 --format=%ci HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE GIT_DATE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
else()
    set(GIT_HASH "unknown")
    set(GIT_DATE "unknown")
endif()

string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%d %H:%M UTC")

# Write the real header included by main.cpp
file(WRITE "${OUTPUT_FILE}"
"// AUTO-GENERATED -- do not edit. Regenerated on every build by cmake/UpdateGitHash.cmake
#pragma once
#define OMEGA_GIT_HASH   \"${GIT_HASH}\"
#define OMEGA_GIT_DATE   \"${GIT_DATE}\"
#define OMEGA_BUILD_TIME \"${BUILD_TIMESTAMP}\"
")

# Write the sentinel -- its timestamp changes every build, forcing main.cpp recompile
file(WRITE "${SENTINEL_FILE}"
"// Sentinel -- timestamp updated every build to force main.cpp recompile.
// Do not include this file directly.
// Built: ${BUILD_TIMESTAMP}
")

message(STATUS "[Omega] Git hash: ${GIT_HASH} | Built: ${BUILD_TIMESTAMP}")

# UpdateGitHash.cmake — runs at BUILD time (not configure time) via add_custom_command
# Writes include/version_generated.hpp with fresh git hash + timestamp every build.

find_package(Git QUIET)

if(GIT_FOUND AND EXISTS "${SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
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

file(WRITE "${OUTPUT_FILE}"
"// AUTO-GENERATED — do not edit. Regenerated on every build by cmake/UpdateGitHash.cmake
#pragma once
#define OMEGA_GIT_HASH   \"${GIT_HASH}\"
#define OMEGA_GIT_DATE   \"${GIT_DATE}\"
#define OMEGA_BUILD_TIME \"${BUILD_TIMESTAMP}\"
")

message(STATUS "[Omega] Git hash: ${GIT_HASH} | Built: ${BUILD_TIMESTAMP}")

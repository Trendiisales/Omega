# UpdateGitHash.cmake -- runs at configure time via execute_process()
# Computes the git hash and build timestamp, writes them to a CMake cache
# variable file that CMakeLists.txt reads to pass as /D defines.
#
# No sentinel, no forced recompile. main.cpp only recompiles when its
# actual source or headers change -- PCH savings are now real.
#
# The hash is still correct: cmake --build re-runs configure if CMakeLists.txt
# changes, and DEPLOY_OMEGA.ps1 always does a clean configure before building.

find_package(Git QUIET)

if(GIT_FOUND AND EXISTS "${SOURCE_DIR}/.git")
    # SOURCE HASH FIX: use the last commit that touched real source files.
    # Skip log-push commits (only touch logs/) -- same logic as DEPLOY_OMEGA.ps1.
    set(GIT_HASH "")
    set(_SEARCH_LIMIT 20)
    set(_IDX 0)
    while(NOT GIT_HASH AND _IDX LESS _SEARCH_LIMIT)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} log --oneline -1 --skip=${_IDX}
            WORKING_DIRECTORY "${SOURCE_DIR}"
            OUTPUT_VARIABLE _COMMIT_LINE
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        string(REGEX REPLACE "^([a-f0-9]+) .*" "\\1" _SHORT_HASH "${_COMMIT_LINE}")
        if(NOT "${_SHORT_HASH}" STREQUAL "" AND NOT "${_SHORT_HASH}" STREQUAL "${_COMMIT_LINE}")
            execute_process(
                COMMAND ${GIT_EXECUTABLE} show --name-only --format= ${_SHORT_HASH}
                WORKING_DIRECTORY "${SOURCE_DIR}"
                OUTPUT_VARIABLE _FILES_CHANGED
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
            string(REPLACE "\n" ";" _FILES_LIST "${_FILES_CHANGED}")
            foreach(_F ${_FILES_LIST})
                if(_F AND NOT "${_F}" MATCHES "^logs/")
                    set(GIT_HASH "${_SHORT_HASH}")
                    break()
                endif()
            endforeach()
        endif()
        math(EXPR _IDX "${_IDX} + 1")
    endwhile()
    if("${GIT_HASH}" STREQUAL "")
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

# Write defines to a cmake include file -- CMakeLists.txt reads this and
# passes them as target_compile_definitions() so no header file is needed.
file(WRITE "${OUTPUT_FILE}"
"set(OMEGA_GIT_HASH   \"${GIT_HASH}\")
set(OMEGA_GIT_DATE   \"${GIT_DATE}\")
set(OMEGA_BUILD_TIME \"${BUILD_TIMESTAMP}\")
")

message(STATUS "[Omega] Git hash: ${GIT_HASH} | Built: ${BUILD_TIMESTAMP}")

# UpdateGitHash.cmake -- runs at configure time
# Uses HEAD directly -- one hash, always matches what was pulled and built.

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
"set(OMEGA_GIT_HASH   \"${GIT_HASH}\")
set(OMEGA_GIT_DATE   \"${GIT_DATE}\")
set(OMEGA_BUILD_TIME \"${BUILD_TIMESTAMP}\")
")

message(STATUS "[Omega] Git hash: ${GIT_HASH} | Built: ${BUILD_TIMESTAMP}")

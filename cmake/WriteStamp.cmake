# WriteStamp.cmake
# Called as POST_BUILD step to write omega_build.stamp
# Avoids MSBuild/cmake quoting issues with inline PowerShell backticks
# Called with: cmake -DSOURCE_DIR=... -DGIT_HASH=... -P WriteStamp.cmake

# Try to get git hash via cmake
find_program(GIT_EXE git)
if(GIT_EXE)
    execute_process(
        COMMAND ${GIT_EXE} -C "${SOURCE_DIR}" rev-parse --short HEAD
        OUTPUT_VARIABLE GIT_SHORT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

# Fall back to compile-time hash if git not available
if(NOT GIT_SHORT OR GIT_SHORT STREQUAL "")
    set(GIT_SHORT "${GIT_HASH}")
endif()

# Get current UTC time
string(TIMESTAMP BUILD_TIME UTC)

# Write stamp file
set(STAMP_CONTENT "GIT_HASH_SHORT=${GIT_SHORT}\nBUILD_TIME=${BUILD_TIME}")
file(WRITE "${SOURCE_DIR}/omega_build.stamp" "GIT_HASH_SHORT=${GIT_SHORT}\nBUILD_TIME=${BUILD_TIME}\n")

message(STATUS "[Omega] Stamp written: ${GIT_SHORT} @ ${BUILD_TIME}")

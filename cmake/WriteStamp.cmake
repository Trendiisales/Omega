# WriteStamp.cmake
# Called as POST_BUILD step -- writes omega_build.stamp AND build_ok.sentinel.
# QUICK_RESTART checks build_ok.sentinel before syncing the binary.
# If this script fails, sentinel is not written, QUICK_RESTART refuses to sync.
# Called with: cmake -DSOURCE_DIR=... -DGIT_HASH=... -P WriteStamp.cmake

# Get git hash
find_program(GIT_EXE git)
if(GIT_EXE)
    execute_process(
        COMMAND ${GIT_EXE} -C "${SOURCE_DIR}" rev-parse --short HEAD
        OUTPUT_VARIABLE GIT_SHORT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

if(NOT GIT_SHORT OR GIT_SHORT STREQUAL "")
    set(GIT_SHORT "${GIT_HASH}")
endif()

# Get UTC timestamp
string(TIMESTAMP BUILD_TIME UTC)

# Write stamp
file(WRITE "${SOURCE_DIR}/omega_build.stamp"
    "GIT_HASH_SHORT=${GIT_SHORT}\nBUILD_TIME=${BUILD_TIME}\n")

# Write sentinel -- QUICK_RESTART checks this before syncing binary.
# Sentinel contains the exe path so QUICK_RESTART can verify the right binary.
file(WRITE "${SOURCE_DIR}/build_ok.sentinel"
    "BUILD_OK=1\nGIT_HASH=${GIT_SHORT}\nBUILD_TIME=${BUILD_TIME}\nEXE=${SOURCE_DIR}/build/Release/Omega.exe\n")

message(STATUS "[Omega] Build stamp written: ${GIT_SHORT} @ ${BUILD_TIME}")
message(STATUS "[Omega] Sentinel written: build_ok.sentinel")

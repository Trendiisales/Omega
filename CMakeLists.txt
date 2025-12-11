cmake_minimum_required(VERSION 3.13)
project(OmegaEngine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# HFT OPTIMIZATIONS FOR LINUX RT KERNEL
# =============================================================================

# Detect Linux RT kernel
execute_process(
    COMMAND uname -r
    OUTPUT_VARIABLE KERNEL_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(FIND "${KERNEL_VERSION}" "rt" HAS_RT_KERNEL)

if(HAS_RT_KERNEL GREATER -1)
    message(STATUS "Linux RT kernel detected: ${KERNEL_VERSION}")
    add_definitions(-DHAS_RT_KERNEL=1)
else()
    message(STATUS "Standard kernel: ${KERNEL_VERSION} (RT kernel recommended for HFT)")
    add_definitions(-DHAS_RT_KERNEL=0)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Debug flags
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra")

# =============================================================================
# HFT RELEASE FLAGS - MAXIMUM OPTIMIZATION
# =============================================================================
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -march=native -mtune=native")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -flto -ffast-math")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -funroll-loops -fprefetch-loop-arrays")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fomit-frame-pointer")

# Link-time optimization
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_SOURCE_DIR}/src/third_party)

file(GLOB_RECURSE OMEGA_SOURCES ${CMAKE_SOURCE_DIR}/src/*.cpp)
list(FILTER OMEGA_SOURCES EXCLUDE REGEX ".*test.*\\.cpp$")

add_executable(omega ${OMEGA_SOURCES})

if(WIN32)
    target_link_libraries(omega ws2_32)
    target_compile_definitions(omega PRIVATE _WIN32_WINNT=0x0601)
else()
    target_link_libraries(omega pthread)
    # Linux-specific: Link with rt for real-time clock
    target_link_libraries(omega rt)
endif()

set_target_properties(omega PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

message(STATUS "")
message(STATUS "=== OMEGA HFT BUILD CONFIGURATION ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Kernel: ${KERNEL_VERSION}")
message(STATUS "CXX Flags: ${CMAKE_CXX_FLAGS_RELEASE}")
message(STATUS "No OpenSSL required - using built-in crypto")
message(STATUS "=====================================")

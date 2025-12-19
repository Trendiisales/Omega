cmake_minimum_required(VERSION 3.16)
project(chimera_hft LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(ACTIVE_SRC_DIR ${CMAKE_SOURCE_DIR}/src/active)

# =============================================================================
# ACTIVE TRANSLATION UNITS ONLY
# =============================================================================
# These are the ONLY .cpp files that get compiled.
# Everything else is header-only or archived.
# Total: 2 .cpp files, 47 .hpp files
# =============================================================================
set(ACTIVE_CPP
    ${ACTIVE_SRC_DIR}/main_dual.cpp
    ${ACTIVE_SRC_DIR}/BinaryLog.cpp
)

# Verify all required files exist
foreach(src ${ACTIVE_CPP})
    if(NOT EXISTS ${src})
        message(FATAL_ERROR "Required active source missing: ${src}")
    endif()
endforeach()

add_executable(chimera ${ACTIVE_CPP})

target_include_directories(chimera PRIVATE
    ${ACTIVE_SRC_DIR}
)

target_compile_options(chimera PRIVATE
    -O3
    -march=native
    -mtune=native
)

if(APPLE)
    target_compile_definitions(chimera PRIVATE _DARWIN_C_SOURCE)
endif()

if(WIN32)
    target_link_libraries(chimera PRIVATE ws2_32 crypt32 advapi32 user32 gdi32 bcrypt)
    target_compile_definitions(chimera PRIVATE _WIN32)
endif()

# OpenSSL
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    target_link_libraries(chimera PRIVATE OpenSSL::SSL OpenSSL::Crypto)
else()
    message(STATUS "OpenSSL not found via find_package, assuming manual linking")
endif()

message(STATUS "==============================================")
message(STATUS "CHIMERA HFT v1.5.1 - Active-Only Build")
message(STATUS "==============================================")
message(STATUS "Active .cpp files: 2")
message(STATUS "Active .hpp files: 47")
message(STATUS "Archived files: 280")
message(STATUS "==============================================")

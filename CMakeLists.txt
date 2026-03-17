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
    src/gui/OmegaTelemetryServer.cpp
)

add_executable(Omega ${SOURCES})

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

# ── Build version stamp ──────────────────────────────────────────────────────
# Regenerates include/version_generated.hpp on EVERY build (not just cmake ..)
# so the binary always shows the correct git hash after git pull + rebuild.
# Uses a build-time custom command — execute_process runs at configure time only
# and would bake a stale hash forever until cmake .. is re-run.

set(VERSION_HEADER "${CMAKE_SOURCE_DIR}/include/version_generated.hpp")

find_package(Git QUIET)

add_custom_command(
    OUTPUT  "${VERSION_HEADER}"
    COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DOUTPUT_FILE=${VERSION_HEADER}
            -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
            -P ${CMAKE_SOURCE_DIR}/cmake/UpdateGitHash.cmake
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Regenerating git hash header..."
    # DEPFILE not used — we want this to run unconditionally every build
)

# Dummy target that always runs, forcing the custom command every build
add_custom_target(omega_version_header ALL
    DEPENDS "${VERSION_HEADER}"
)

# Ensure version header is generated before main.cpp compiles
add_dependencies(Omega omega_version_header)

# Include the generated header directory
target_include_directories(Omega PRIVATE ${CMAKE_SOURCE_DIR}/include)

message(STATUS "========================================")
message(STATUS "OMEGA — Commodities & Indices System")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "OpenSSL:    ${OPENSSL_VERSION}")
message(STATUS "Git hash:   regenerated on every build")
message(STATUS "========================================")

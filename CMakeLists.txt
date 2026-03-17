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
# Rewrites include/version_generated.hpp on EVERY build and forces main.cpp
# to recompile so the banner always shows the current commit hash.
#
# Why this approach:
#   - add_custom_command(OUTPUT ...) only reruns when the output is MISSING or
#     stale vs listed deps — since version_generated.hpp has no deps it is
#     always "up to date" and CMake skips it entirely.
#   - Even if the file is rewritten, MSVC skips recompiling main.cpp unless the
#     file's mtime changes.
#   - Solution: ALL custom_target (always runs) writes a BYPRODUCTS sentinel
#     file with a fresh timestamp every build. main.cpp lists that sentinel as
#     a source dep via set_property, forcing recompile whenever it changes.

find_package(Git QUIET)

set(VERSION_HEADER   "${CMAKE_SOURCE_DIR}/include/version_generated.hpp")
set(VERSION_SENTINEL "${CMAKE_BINARY_DIR}/version_sentinel.h")

# Step 1 — custom target marked ALL: runs UpdateGitHash.cmake unconditionally
# every build. Writes both the real header and a sentinel with current timestamp.
add_custom_target(omega_version_header ALL
    COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DOUTPUT_FILE=${VERSION_HEADER}
            -DSENTINEL_FILE=${VERSION_SENTINEL}
            -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
            -P ${CMAKE_SOURCE_DIR}/cmake/UpdateGitHash.cmake
    BYPRODUCTS "${VERSION_HEADER}" "${VERSION_SENTINEL}"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Regenerating git hash header..."
)

# Step 2 — make Omega depend on the target so it runs before linking
add_dependencies(Omega omega_version_header)

# Step 3 — add the sentinel as a source file dep on main.cpp so MSVC sees a
# changed input and recompiles it every time the sentinel timestamp updates.
set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/main.cpp"
    APPEND PROPERTY OBJECT_DEPENDS "${VERSION_SENTINEL}"
)

target_include_directories(Omega PRIVATE ${CMAKE_SOURCE_DIR}/include)

message(STATUS "========================================")
message(STATUS "OMEGA — Commodities & Indices System")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "OpenSSL:    ${OPENSSL_VERSION}")
message(STATUS "Git hash:   regenerated on every build")
message(STATUS "========================================")

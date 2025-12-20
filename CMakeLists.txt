cmake_minimum_required(VERSION 3.16)
project(chimera LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(chimera
    # ===== ENTRY =====
    src/active/main_dual.cpp

    # ===== CORE =====
    src/active/BinaryLog.cpp

    # ===== FIX (THIN CLIENT) =====
    src/active/fix/client/CTraderFIXClient.cpp

    # ===== NETWORK =====
    src/active/net/SSLWebSocketClient.cpp
)

target_include_directories(chimera PRIVATE
    src
    src/active
)

find_package(Threads REQUIRED)
target_link_libraries(chimera PRIVATE Threads::Threads)

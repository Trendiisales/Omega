# omega/CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
project(omega LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -O2)
endif()

# ---- Core library: pure, CSV-driven, no external deps ----------------------
add_library(omega_core
  src/CsvLoader.cpp
  src/Indicators.cpp
  src/Strategy.cpp
  src/Backtester.cpp
  src/Scanner.cpp
  src/IbkrClient.cpp        # compiles as a stub unless OMEGA_WITH_IBKR is set
)
target_include_directories(omega_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---- CLI tools -------------------------------------------------------------
add_executable(omega_backtest src/main_backtest.cpp)
target_link_libraries(omega_backtest PRIVATE omega_core)

add_executable(omega_scan src/main_scan.cpp)
target_link_libraries(omega_scan PRIVATE omega_core)

# ---- Optional IBKR data fetcher --------------------------------------------
# Enable with:  cmake -DOMEGA_WITH_IBKR=ON -DTWSAPI_DIR=/path/to/twsapi ..
option(OMEGA_WITH_IBKR "Build the IBKR TWS API data fetcher" OFF)
if(OMEGA_WITH_IBKR)
  if(NOT TWSAPI_DIR)
    message(FATAL_ERROR "Set -DTWSAPI_DIR=/path/to/twsapi (TWS API C++ source).")
  endif()
  set(TWS_CLIENT_DIR ${TWSAPI_DIR}/source/cppclient/client)
  file(GLOB TWS_SOURCES ${TWS_CLIENT_DIR}/*.cpp)

  target_compile_definitions(omega_core PUBLIC OMEGA_WITH_IBKR)
  target_include_directories(omega_core PUBLIC ${TWS_CLIENT_DIR})
  target_sources(omega_core PRIVATE ${TWS_SOURCES})
  find_package(Threads REQUIRED)
  target_link_libraries(omega_core PUBLIC Threads::Threads)

  add_executable(omega_ibkr_fetch src/main_ibkr_fetch.cpp)
  target_link_libraries(omega_ibkr_fetch PRIVATE omega_core)
endif()

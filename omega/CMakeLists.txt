cmake_minimum_required(VERSION 3.16)
project(omega LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4 /wd4996)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -O2)
endif()

add_library(omega_core
  src/CsvLoader.cpp
  src/Indicators.cpp
  src/Strategy.cpp
  src/Backtester.cpp
  src/Scanner.cpp
  src/IbkrClient.cpp
)
target_include_directories(omega_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(omega_backtest src/main_backtest.cpp)
target_link_libraries(omega_backtest PRIVATE omega_core)

add_executable(omega_scan src/main_scan.cpp)
target_link_libraries(omega_scan PRIVATE omega_core)

option(OMEGA_WITH_IBKR "Build the IBKR TWS API data fetcher" OFF)
if(OMEGA_WITH_IBKR)
  if(NOT TWSAPI_DIR)
    message(FATAL_ERROR "Set -DTWSAPI_DIR=/path/to/twsapi (TWS API C++ source).")
  endif()

  set(TWS_CLIENT_DIR ${TWSAPI_DIR}/source/cppclient/client)
  if(NOT EXISTS ${TWS_CLIENT_DIR}/EWrapper.h)
    set(TWS_CLIENT_DIR ${TWSAPI_DIR}/client)
  endif()
  if(NOT EXISTS ${TWS_CLIENT_DIR}/EWrapper.h)
    message(FATAL_ERROR "Could not find EWrapper.h under ${TWSAPI_DIR}")
  endif()
  message(STATUS "Omega: TWS client dir = ${TWS_CLIENT_DIR}")

  file(GLOB TWS_SOURCES ${TWS_CLIENT_DIR}/*.cpp)

  target_compile_definitions(omega_core PUBLIC OMEGA_WITH_IBKR)
  target_include_directories(omega_core PUBLIC ${TWS_CLIENT_DIR})
  target_sources(omega_core PRIVATE ${TWS_SOURCES})

  if(WIN32)
    target_link_libraries(omega_core PUBLIC ws2_32)
  else()
    find_package(Threads REQUIRED)
    target_link_libraries(omega_core PUBLIC Threads::Threads)
  endif()

  add_executable(omega_ibkr_fetch src/main_ibkr_fetch.cpp)
  target_link_libraries(omega_ibkr_fetch PRIVATE omega_core)
endif()

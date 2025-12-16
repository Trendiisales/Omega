cmake_minimum_required(VERSION 3.16)
project(chimera VERSION 4.0.0 LANGUAGES CXX)

# =============================================================================
# Build Options
# =============================================================================
option(CHIMERA_LIVE "Enable live execution (OFF = shadow/dry-run mode)" OFF)
option(BINANCE_USE_TESTNET "Use Binance testnet instead of production" OFF)

# =============================================================================
# C++ Standard
# =============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# =============================================================================
# Find OpenSSL (required for WebSocket and FIX feeds)
# =============================================================================
find_package(OpenSSL REQUIRED)
if(OPENSSL_FOUND)
    message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")
    message(STATUS "  Include: ${OPENSSL_INCLUDE_DIR}")
    message(STATUS "  Libraries: ${OPENSSL_LIBRARIES}")
else()
    message(FATAL_ERROR "OpenSSL not found - required for SSL/TLS connections")
endif()

# =============================================================================
# Compiler Flags
# =============================================================================
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")
endif()

# =============================================================================
# Build mode definitions
# =============================================================================
if(CHIMERA_LIVE)
    add_definitions(-DCHIMERA_LIVE)
    message(STATUS "*** LIVE MODE ENABLED - ORDERS WILL BE SENT ***")
else()
    message(STATUS "Shadow mode (dry-run) - orders will NOT be sent")
endif()

if(BINANCE_USE_TESTNET)
    add_definitions(-DBINANCE_USE_TESTNET=1)
    message(STATUS "Binance: TESTNET")
else()
    add_definitions(-DBINANCE_USE_TESTNET=0)
    message(STATUS "Binance: PRODUCTION")
endif()

# =============================================================================
# Include directories
# =============================================================================
include_directories(${CMAKE_SOURCE_DIR})
include_directories(${OPENSSL_INCLUDE_DIR})

# =============================================================================
# Source files (explicit - no globs)
# =============================================================================
set(SOURCES
    main.cpp
    core/Logger.cpp
)

# =============================================================================
# Headers (for IDE support)
# =============================================================================
set(HEADERS
    # Core
    core/SPSCQueue.hpp
    core/Logger.hpp
    core/LatencyStats.hpp
    core/ThreadPin.hpp
    core/MonotonicClock.hpp

    # Market
    market/CanonicalTick.hpp
    market/MarketTypes.hpp
    market/TickValidator.hpp

    # Engine
    engine/EngineConfig.hpp
    engine/EngineIngress.hpp
    engine/EngineCore.hpp
    engine/Intent.hpp
    engine/IntentQueue.hpp
    engine/StrategyRunner.hpp
    engine/ExecutionRouter.hpp
    engine/EngineHealth.hpp
    engine/QueueMetrics.hpp
    engine/BurstDetector.hpp
    engine/EngineSupervisor.hpp
    engine/ExecutionHealth.hpp
    engine/IExecutionRouter.hpp
    engine/NullExecutionRouter.hpp

    # Strategy
    strategy/StrategyBaseCRTP.hpp
    strategy/momentum/MomentumConfig.hpp
    strategy/momentum/MomentumStrategy.hpp
    strategy/reversion/MeanReversionConfig.hpp
    strategy/reversion/MeanReversionStrategy.hpp
    strategy/breakout/BreakoutConfig.hpp
    strategy/breakout/BreakoutStrategy.hpp
    strategy/volatility/VolatilityShockConfig.hpp
    strategy/volatility/VolatilityShockStrategy.hpp
    strategy/orderflow/OrderFlowImbalanceConfig.hpp
    strategy/orderflow/OrderFlowImbalanceStrategy.hpp
    strategy/liquidity/LiquidityFadeConfig.hpp
    strategy/liquidity/LiquidityFadeStrategy.hpp
    strategy/trend/TrendContinuationConfig.hpp
    strategy/trend/TrendContinuationStrategy.hpp
    strategy/regime/RegimeWrapper.hpp

    # Feed - Real WebSocket and FIX
    feed/binance/TradeFeed.hpp
    feed/binance/BookFeed.hpp
    feed/binance/BinanceTradeNormalizer.hpp
    feed/binance/BinanceWebSocket.hpp
    feed/fix/FixSession.hpp
    feed/fix/FixDecoder.hpp

    # Data
    data/TickSource.hpp
    data/LiveTickSource.hpp
    data/BacktestTickSource.hpp

    # API
    api/MetricsExporter.hpp
    api/MetricsServer.hpp

    # Monitor
    monitor/AlertEngine.hpp

    # Tests
    tests/BurstTickGenerator.hpp
)

# =============================================================================
# Main Executable
# =============================================================================
add_executable(chimera ${SOURCES} ${HEADERS})
target_link_libraries(chimera PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# =============================================================================
# Stress Test Executable
# =============================================================================
add_executable(stress_test 
    tests/StressHarness.cpp
    core/Logger.cpp
)

# =============================================================================
# Platform-specific linking
# =============================================================================
if(UNIX AND NOT APPLE)
    target_link_libraries(chimera PRIVATE pthread)
    target_link_libraries(stress_test PRIVATE pthread)
endif()

if(APPLE)
    # macOS thread support is in system libs
endif()

if(WIN32)
    target_link_libraries(chimera PRIVATE ws2_32)
    target_link_libraries(stress_test PRIVATE ws2_32)
endif()

# =============================================================================
# Print configuration
# =============================================================================
message(STATUS "")
message(STATUS "=== Chimera HFT Build Configuration ===")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Live mode: ${CHIMERA_LIVE}")
message(STATUS "Binance testnet: ${BINANCE_USE_TESTNET}")
message(STATUS "")
message(STATUS "Targets:")
message(STATUS "  chimera     - Main HFT engine with real feeds")
message(STATUS "  stress_test - Stress test harness")
message(STATUS "")
message(STATUS "Build commands:")
message(STATUS "  Shadow + Testnet: cmake -DBINANCE_USE_TESTNET=ON ..")
message(STATUS "  Shadow + Live:    cmake -DBINANCE_USE_TESTNET=OFF ..")
message(STATUS "  Live + Production: cmake -DBINANCE_USE_TESTNET=OFF -DCHIMERA_LIVE=ON ..")
message(STATUS "")

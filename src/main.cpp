// ==============================================================================
// OMEGA -- Commodities & Indices Trading System
// Strategy: Compression Breakout (CRTP engine, zero virtual dispatch)
// Broker: BlackBull Markets -- identical FIX stack to ChimeraMetals
// Primary: MES ? MNQ ? MCL  |  Confirmation: ES NQ CL VIX DX ZN YM RTY
// GUI: HTTP :7779 / WebSocket :7780
// ==============================================================================

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mstcpip.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#pragma comment(lib, "ws2_32.lib")

#include <iostream>
#include <atomic>
#include <string>
#include <string_view>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <direct.h>   // _mkdir on Windows
#include <chrono>
#include <memory>
#include <mutex>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <cmath>
#include <csignal>
#include <functional>
#include <cstdint>
#include <cstring>

// ?? Omega headers (flat -- all files in same directory on VPS) ????????????????
#include "OmegaFIX.hpp"          // IMMUTABLE -- FIX infrastructure, do not modify
#include "OmegaTelemetryWriter.hpp"
#include "OmegaTradeLedger.hpp"
#include "SymbolConfig.hpp"
#include "SymbolSupervisor.hpp"
#include "OmegaCostGuard.hpp"    // ExecutionCostGuard -- must precede templated lambdas (MSVC)

// ?? Build version -- injected as compiler /D defines by CMake ???????????????
// Passed via target_compile_definitions() -- no generated header, no forced recompile.
// Falls back to "unknown" if build system doesn't provide them.
#ifndef OMEGA_GIT_HASH
#  define OMEGA_GIT_HASH   "unknown"
#endif
#ifndef OMEGA_BUILD_TIME
#  define OMEGA_BUILD_TIME "unknown"
#endif
#ifndef OMEGA_GIT_DATE
#  define OMEGA_GIT_DATE   "unknown"
#endif
static constexpr const char* OMEGA_VERSION = OMEGA_GIT_HASH;
static constexpr const char* OMEGA_BUILT   = OMEGA_BUILD_TIME;
static constexpr const char* OMEGA_COMMIT  = OMEGA_GIT_DATE;
#include "BreakoutEngine.hpp"
#include "SymbolEngines.hpp"      // SpEngine, NqEngine, OilEngine, MacroContext (includes BreakoutEngine.hpp)
#include "MacroRegimeDetector.hpp"
#include "OmegaTelemetryServer.hpp"
#include "GoldEngineStack.hpp"    // Multi-engine gold stack (ported from ChimeraMetals)
#include "LatencyEdgeEngines.hpp"
#include "CrossAssetEngines.hpp" // Co-location speed advantage engines (LeadLag, SpreadDisloc, EventComp)
#include "CTraderDepthClient.hpp" // cTrader Open API v2 -- full order book depth feed

// ?? Adaptive intelligence layer (gap-close vs best systems) ??????????????????
#include "OmegaAdaptiveRisk.hpp"   // Kelly sizing, rolling Sharpe, DD throttle, corr heat
#include "OmegaNewsBlackout.hpp"   // Economic calendar blackout (NFP, FOMC, CPI, EIA)
#include "OmegaPartialExit.hpp"    // Split TP -- close 50% at 1R, trail remainder
#include "OmegaEdges.hpp"          // 7 institutional edges: CVD, TOD, spread-Z, round#, PDH/PDL, FX-fix, fill quality
#include "OmegaRegimeAdaptor.hpp"  // Regime-adaptive engine weights + vol regime
#include "OmegaHotReload.hpp"      // Live config reload -- no reboot needed for param changes
#include "OmegaCorrelationMatrix.hpp" // EWM rolling corr matrix + vol-parity sizing
#include "OmegaVPIN.hpp"              // Tick-classified VPIN toxicity gate (GoldFlow pre-entry)
#include "OmegaMonteCarlo.hpp"        // Bootstrap P&L resample + BH/FDR correction (offline tool)
#include "OmegaVolTargeter.hpp"       // EWMA vol targeting + ADX momentum regime classifier
#include "OmegaSignalScorer.hpp"      // Composite signal scoring (replaces soft gate chain)
#include "OmegaCrowdingGuard.hpp"     // Directional crowding tracker + score penalty (RenTec #4)
#include "OmegaWalkForward.hpp"       // Rolling live walk-forward OOS validation (RenTec #6)
#include "OmegaParamGate.hpp"         // Adaptive parameter gate: dynamic score threshold (RenTec #7)
#include "omega_types.hpp"
#include "globals.hpp"
#include "omega_runtime.hpp"
#include "on_tick.hpp"
#include "trade_loop.hpp"
#include "omega_main.hpp"

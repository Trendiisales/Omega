// ==============================================================================
// OMEGA -- Commodities & Indices Trading System
// Strategy: Compression Breakout (CRTP engine, zero virtual dispatch)
// Broker: BlackBull Markets -- identical FIX stack to ChimeraMetals
// Primary: MES ? MNQ ? MCL  |  Confirmation: ES NQ CL VIX DX ZN YM RTY
// GUI: HTTP :7779 / WebSocket :7780  (OmegaTelemetryServer; legacy GUI client)
// API: HTTP :7781                    (OmegaApiServer; omega-terminal Step 2)
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
#ifndef OMEGA_BACKTEST
#include "api/OmegaApiServer.hpp"   // Step 2 read-API for omega-terminal (resolves via -I src)
#endif
#include "GoldEngineStack.hpp"    // Multi-engine gold stack (ported from ChimeraMetals)
// LatencyEdgeEngines.hpp removed S13 Finding B 2026-04-24 — engine culled.
#include "CrossAssetEngines.hpp" // 9 cross-asset engines: EsNqDiv, OilFade, BrentWti, FxCascade, CarryUnwind, ORB, VWAPRev, TrendPB, NBM (SilverTurtle removed at Batch 5V)
#include "IndexFlowEngine.hpp"   // L2 flow + EWM drift engines for equity indices (IndexFlowEngine, IndexMacroCrashEngine)
#include "CTraderDepthClient.hpp" // cTrader Open API v2 -- full order book depth feed
#include "RealDomReceiver.hpp"   // OmegaDomStreamer cBot receiver -- real XAUUSD DOM sizes on port 8765

// ?? Adaptive intelligence layer (gap-close vs best systems) ??????????????????
#include "OmegaAdaptiveRisk.hpp"   // Kelly sizing, rolling Sharpe, DD throttle, corr heat
#include "OmegaNewsBlackout.hpp"   // Economic calendar blackout (NFP, FOMC, CPI, EIA)
#include "OmegaPartialExit.hpp"    // Split TP -- close 50% at 1R, trail remainder
#include "OmegaEdges.hpp"          // 7 institutional edges: CVD, TOD, spread-Z, round#, PDH/PDL, FX-fix, fill quality
#include "OmegaRegimeAdaptor.hpp"  // Regime-adaptive engine weights + vol regime
#include "OmegaHotReload.hpp"      // Live config reload -- no reboot needed for param changes
#include "OmegaCorrelationMatrix.hpp" // EWM rolling corr matrix + vol-parity sizing
#include "OmegaVPIN.hpp"              // Tick-classified VPIN toxicity gate
#include "OmegaMonteCarlo.hpp"        // Bootstrap P&L resample + BH/FDR correction (offline tool)
#include "OmegaVolTargeter.hpp"       // EWMA vol targeting + ADX momentum regime classifier
#include "OmegaSignalScorer.hpp"      // Composite signal scoring (replaces soft gate chain)
#include "OmegaCrowdingGuard.hpp"     // Directional crowding tracker + score penalty (RenTec #4)
#include "OmegaWalkForward.hpp"       // Rolling live walk-forward OOS validation (RenTec #6)
#include "OmegaParamGate.hpp"         // Adaptive parameter gate: dynamic score threshold (RenTec #7)
#include "HTFSwingEngines.hpp"        // H1 ADX+EMA pullback engine + H4 Donchian breakout engine (XAUUSD)
#include "omega_types.hpp"
#include "globals.hpp"
#include "omega_runtime.hpp"
#include "on_tick.hpp"
// 2026-05-01 race fix: single-writer engine dispatch worker. MUST be included
// after on_tick.hpp and after order_exec.hpp (transitively pulled in by
// on_tick.hpp) so that handle_execution_report() and on_tick() are visible to
// engine_dispatch.hpp's worker_loop_ at parse time. trade_loop.hpp below this
// includes quote_loop.hpp which references engine_dispatch_post_tick;
// fix_dispatch.hpp (included by on_tick.hpp at L2149, BEFORE this point)
// forward-declares engine_dispatch_post_tick at its top to compile cleanly.
#include "engine_dispatch.hpp"
#include "trade_loop.hpp"
#include "engine_init.hpp"
#include "omega_main.hpp"

// ── Step 2 Omega Terminal: OmegaApiServer auto-start ────────────────────────
// The new HTTP read-API for the omega-terminal React UI runs on
// 127.0.0.1:7781 alongside the existing OmegaTelemetryServer (HTTP :7779 + WS
// :7780). Started here at static-init time so the accept loop is up before
// init_engines() registers any engines; pre-registration requests return [].
//
// OmegaApiServer::run() does its own WSAStartup, so it is independent of the
// FIX layer's socket subsystem init. The dtor calls stop() which closesocket()s
// the listen FD and joins the accept thread, so process exit is clean.
//
// Excluded from backtest builds via #ifndef OMEGA_BACKTEST -- the harness has
// no JSON consumer and the extra port would conflict with parallel runs.
#ifndef OMEGA_BACKTEST
static omega::OmegaApiServer g_omega_api_server;
namespace {
    struct OmegaApiServerAutoStart {
        OmegaApiServerAutoStart()  { g_omega_api_server.start(7781); }
        ~OmegaApiServerAutoStart() { g_omega_api_server.stop();      }
    };
    static OmegaApiServerAutoStart g_omega_api_server_autostart;
}
#endif

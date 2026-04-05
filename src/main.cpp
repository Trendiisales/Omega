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

// ?????????????????????????????????????????????????????????????????????????????
// Singleton
// ?????????????????????????????????????????????????????????????????????????????
static HANDLE g_singleton_mutex = NULL;

// ?????????????????????????????????????????????????????????????????????????????
// Config
// ?????????????????????????????????????????????????????????????????????????????
struct OmegaConfig {
    // FIX -- identical to ChimeraMetals
    std::string host       = "live-uk-eqx-02.p.c-trader.com";
    int         port       = 5211;
    std::string sender     = "live.blackbull.8077780";
    std::string target     = "cServer";
    std::string username   = "8077780";
    std::string password   = "Bowen6feb";
    int         heartbeat  = 30;
    int         connection_warmup_sec = 30;  // seconds after reconnect before new entries allowed

    std::string mode       = "SHADOW";

    // Breakout params
    double vol_thresh_pct        = 0.050;
    double tp_pct                = 0.400;
    double sl_pct                = 2.000;
    int    compression_lookback  = 50;
    int    baseline_lookback     = 200;
    double compression_threshold = 0.80;
    int    max_hold_sec          = 1800;  // raised from 1500 -- spec MIN_HOLD values imply longer trades
    int    min_entry_gap_sec     = 30;
    double max_spread_pct        = 0.05;
    double max_latency_ms        = 120.0;  // raised from 60ms -- RTTp95 from remote VPS is ~68ms, 60ms was vetoing all LIVE entries
    double momentum_thresh_pct   = 0.05;   // momentum gate threshold
    double min_breakout_pct      = 0.25;   // min breakout size from comp edge
    int    max_trades_per_min    = 2;       // rate limiter
    int    max_trades_per_cycle  = 1;       // ranking window: max candidates to promote per cycle

    // Risk
    bool   reload_trades_on_startup = true;  // false = ignore today's trade CSV on startup (clean PnL slate)
    double daily_loss_limit  = 200.0;
    double daily_profit_target = 0.0;   // 0=disabled. Stop new entries once daily P&L >= this.
                                        // Recommended: set to 1.5? daily_loss_limit once live.
    double max_loss_per_trade_usd = 0.0; // 0=disabled. Hard dollar cap per trade regardless of sizing.
                                          // Enforced in enter_directional and gold stack path.
    double max_portfolio_sl_risk_usd = 0.0; // 0=disabled. Max total simultaneous open SL risk.
                                             // Blocks new entries when sum of (SL_pts * lot * tick_val)
                                             // across all open positions exceeds this threshold.
                                             // Prevents correlated crash (gold+silver+oil all SHORT).
                                             // Recommended: 2-3x daily_loss_limit (e.g. 500 if limit=200).
    double session_watermark_pct = 0.0;  // 0=disabled. Stop if drawdown from intra-day peak
                                          // exceeds this % of daily_profit_target (or daily_loss_limit).
                                          // e.g. 0.50 = stop if 50% of peak is given back.
    double hourly_loss_limit = 0.0;      // 0=disabled. Block new entries if rolling 2h loss > this.
    int    max_consec_losses = 3;
    int    loss_pause_sec    = 300;
    int    max_open_positions = 4;     // allow up to 4 concurrent positions across different symbols
    bool   independent_symbols = true;  // true: risk/position gating is per-symbol (recommended)
    int    auto_disable_after_trades  = 10;
    bool   shadow_ustec_pilot_only    = false;  // false: multi-symbol SHADOW research; true: restrict to GOLD + USTEC pilot
    bool   shadow_research_mode       = false;  // false: paper/live-like behavior; true: discovery mode with relaxed filters
    double ustec_pilot_size           = 0.35;   // reduced NQ pilot size vs default 1.0
    int    ustec_pilot_min_gap_sec    = 60;     // more opportunities than default 90s shadow NQ gap
    bool   ustec_pilot_require_session = true;  // keep USTEC pilot out of session-closed tape
    bool   ustec_pilot_require_latency = true;  // enforce latency gate in shadow for USTEC pilot
    bool   ustec_pilot_block_risk_off  = true;  // skip USTEC pilot in RISK_OFF regime
    bool   enable_extended_symbols    = true;   // subscribe + trade additional opportunity symbols

    // ?? Risk-based position sizing ??????????????????????????????????????????
    // risk_per_trade_usd: the maximum dollar loss per trade (SL + spread cost).
    // Size is computed dynamically at entry: size = risk$ / (sl_abs + spread) / tick_mult
    // Set to 0.0 to use legacy fixed ENTRY_SIZE / sig.size (backward-compatible default).
    // Start conservative (e.g. 50.0) and increase as shadow P&L validates the strategy.
    double risk_per_trade_usd = 0.0;  // 0 = disabled, use fixed sizes
    double account_equity     = 10000.0; // account size for edge-based position sizing
    // Per-symbol hard cap on lot size -- safety net regardless of computed size.
    // Prevents a misconfigured risk_per_trade from opening an oversized position.
    double max_lot_gold    = 0.50;   // XAUUSD max lots per trade
    double max_lot_indices = 0.20;   // SP/NQ/DJ30/NAS100/EU indices max lots
    double max_lot_oil     = 0.50;   // USOIL.F / UKBRENT max lots
    double max_lot_silver  = 0.20;   // XAGUSD max lots
    double max_lot_fx      = 5.00;   // EURUSD max lots

    // Per-symbol minimum lot size floor.
    // compute_size() clamps DOWN to max_lot and UP to min_lot.
    // Overrides risk calculation if risk would produce a smaller size.
    // Set to 0.0 to use only the global 0.01 hard floor.
    // Broker minimums (BlackBull cTrader confirmed):
    // GOLD/XAGUSD/OIL/FX/indices = 0.01 lots, NAS100 = 0.10 lots (hardcoded in compute_size)
    double min_lot_gold    = 0.01;
    double min_lot_indices = 0.01;
    double min_lot_oil     = 0.01;
    double min_lot_silver  = 0.01;
    double min_lot_fx      = 0.01;

    // GoldFlow compression vol floor -- hot-reloadable via omega_config.ini
    // vol_range must exceed this (pts) for GoldFlow to enter in COMPRESSION regime.
    // 0.6 = valid coil; raise to 2.0 to require stronger compression signal.
    // Lowered 0.80->0.60: vol_range was reading 0.66-0.79 during London open, sitting
    // just under the 0.80 threshold and blocking all entries. 0.60 still filters
    // genuinely dead tape (vol_range < 0.3) while allowing real low-vol setups.
    // Asia-specific: gf_compression_vol_floor_asia applies 22:00-07:00 UTC only.
    double gf_compression_vol_floor      = 3.0;   // raised 0.60->3.0: bad trades at vol_range=0.83/2.00 SL'd <60s MFE=0. Good trades vol_range>=4.35
    double gf_compression_vol_floor_asia = 0.40;  // lowered 0.50->0.40: Asia tape is thinner, valid coils are smaller

    int    ext_ger30_id               = 0;
    int    ext_uk100_id               = 0;
    int    ext_estx50_id              = 0;
    int    ext_xagusd_id              = 0;
    int    ext_eurusd_id              = 0;
    int    ext_ukbrent_id             = 0;
    int    ext_gbpusd_id              = 0;

    // Session UTC
    int session_start_utc = 7;
    int session_end_utc   = 21;
    bool session_asia     = true;  // enable Asia/Tokyo window 22:00-05:00 UTC

    // SP (US500) -- spec: TP_MULT=1.7, SL_MULT=1.0, MAX_SPREAD=2.5pts@6000=0.042%, MIN_HOLD=22s
    double sp_tp_pct              = 0.595;  // TP_MULT=1.7 ? SL=0.35% ? 0.595%
    double sp_sl_pct              = 0.350;
    double sp_vol_thresh_pct      = 0.040;
    int    sp_min_gap_sec         = 60;
    double sp_momentum_thresh_pct = 0.006;
    double sp_min_breakout_pct    = 0.030;
    double sp_max_spread_pct      = 0.042;  // 2.5pts @ ~6000
    double sp_compression_threshold = 0.85;
    double sp_vix_panic           = 40.0;
    double sp_div_threshold       = 0.0060;

    // NQ (USTEC) -- spec: TP_MULT=1.6, SL_MULT=1.0, MAX_SPREAD=3.0pts@19000=0.016%
    double nq_tp_pct              = 0.640;  // TP_MULT=1.6 ? SL=0.40%
    double nq_sl_pct              = 0.400;
    double nq_vol_thresh_pct      = 0.050;
    int    nq_min_gap_sec         = 60;
    double nq_momentum_thresh_pct = 0.005;
    double nq_min_breakout_pct    = 0.040;
    double nq_max_spread_pct      = 0.060;  // 0.060% of ~$24600 = ~$14.76 max. OLD: 0.016% = $3.94 -- blocked most ticks (actual $8-13)
    double nq_compression_threshold = 0.85;
    double nq_vix_panic           = 40.0;
    double nq_div_threshold       = 0.0060;

    // Us30 (DJ30) -- supervisor spread gate: 0.080% of ~$46000 = ~$37 max
    // OLD: 0.008% = $3.68 max -- blocked EVERY tick (actual spread $25-65). Wrong era.
    double us30_momentum_thresh_pct   = 0.006;
    double us30_min_breakout_pct      = 0.040;
    double us30_max_spread_pct        = 0.080;  // 0.080% of ~$46000 = ~$37
    double us30_compression_threshold = 0.85;
    double us30_vix_panic             = 40.0;
    double us30_div_threshold         = 0.0060;

    // Nas100 (NAS100 cash) -- supervisor spread gate: 0.060% of ~$24600 = ~$14.76 max
    // OLD: 0.016% = $3.94 max -- blocked most ticks (actual spread $8-13). Wrong era.
    double nas100_momentum_thresh_pct   = 0.005;
    double nas100_min_breakout_pct      = 0.040;
    double nas100_max_spread_pct        = 0.060;  // 0.060% of ~$24600 = ~$14.76
    double nas100_compression_threshold = 0.85;
    double nas100_vix_panic             = 40.0;
    double nas100_div_threshold         = 0.0060;

    // Oil (USOIL) -- spec: TP_MULT=1.8, SL_MULT=1.2, MAX_SPREAD=1.5pts@97=1.55%
    double oil_tp_pct                 = 1.440;  // TP_MULT=1.8 ? SL=0.8% (wider for oil)
    double oil_sl_pct                 = 0.800;  // widened: oil needs room (SL_MULT=1.2)
    double oil_vol_thresh_pct         = 0.080;
    int    oil_min_gap_sec            = 90;
    int    oil_max_hold_sec           = 1800;
    double oil_momentum_thresh_pct    = 0.050;
    double oil_min_breakout_pct       = 0.060;
    double oil_max_spread_pct         = 0.120;  // 1.5pts @ ~97 ? 1.55% -- keep current
    double oil_compression_threshold  = 0.80;
    double oil_vix_panic              = 50.0;

    // Silver (XAGUSD)
    double silver_tp_pct               = 0.800;
    double silver_sl_pct               = 0.400;
    double silver_vol_thresh_pct       = 0.060;
    int    silver_min_gap_sec          = 180;
    double silver_momentum_thresh_pct  = 0.020;
    double silver_min_breakout_pct     = 0.050;
    double silver_max_spread_pct       = 0.120;  // 0.120% of ~$68 = ~$0.082 max. OLD: 0.080% = $0.054 -- too tight for volatile silver ($0.05-$0.13)
    double silver_compression_threshold = 0.85;

    // Brent (UKBRENT)
    double brent_tp_pct                = 1.500;
    double brent_sl_pct                = 0.500;
    double brent_vol_thresh_pct        = 0.080;
    int    brent_min_gap_sec           = 90;
    double brent_momentum_thresh_pct   = 0.050;
    double brent_min_breakout_pct      = 0.060;
    double brent_max_spread_pct        = 0.120;
    double brent_compression_threshold = 0.80;

    // EU Indices (GER40, UK100, ESTX50) -- shared params
    double eu_index_momentum_thresh_pct   = 0.005;
    double eu_index_min_breakout_pct      = 0.030;
    double eu_index_compression_threshold = 0.85;
    double eu_index_max_spread_pct        = 0.080;  // 0.080% -- ESTX50@5600=$4.48, GER40@22500=$18, UK100@9900=$7.92
    // OLD: 0.042% -- ESTX50=$2.35 max, actual $1.50-8 (wide spreads blocked all EU index supervisor decisions)

    // FX (EURUSD) -- spec: TP_MULT=1.5, MAX_SPREAD=0.0002@1.15=0.017%
    double fx_tp_pct               = 0.060;  // TP_MULT=1.5 ? SL=0.040%
    double fx_sl_pct               = 0.040;
    double fx_vol_thresh_pct       = 0.010;
    int    fx_min_gap_sec          = 45;
    double fx_momentum_thresh_pct  = 0.015;
    double fx_min_breakout_pct     = 0.080;
    double fx_max_spread_pct       = 0.017;  // 0.0002 @ ~1.15 -- tightened from 0.010 (was too loose)
    double fx_compression_threshold = 0.85;

    // GBPUSD -- spec: TP_MULT=1.5, SL_MULT=1.0, MAX_SPREAD=0.0003@1.27=0.024%
    double gbpusd_tp_pct               = 0.057;  // TP_MULT=1.5 ? SL=0.038%
    double gbpusd_sl_pct               = 0.038;
    double gbpusd_vol_thresh_pct       = 0.012;
    int    gbpusd_min_gap_sec          = 40;
    double gbpusd_momentum_thresh_pct  = 0.018;
    double gbpusd_min_breakout_pct     = 0.085;
    double gbpusd_max_spread_pct       = 0.020;  // 2pip @ ~1.34 -- actual spread 1.5-2.5pip
    double gbpusd_compression_threshold = 0.85;
    double max_lot_gbpusd              = 5.00;
    double min_lot_gbpusd              = 0.01;

    // Asia FX (AUDUSD, NZDUSD, USDJPY) -- shared session gate flag
    bool   asia_fx_asia_only = false;  // false: trade Asia FX in all sessions (asia_fx_asia_only=false in config)
    // AUDUSD
    double audusd_tp_pct               = 0.070;
    double audusd_sl_pct               = 0.035;
    double audusd_vol_thresh_pct       = 0.010;
    int    audusd_min_gap_sec          = 45;
    double audusd_momentum_thresh_pct  = 0.015;
    double audusd_min_breakout_pct     = 0.070;
    double audusd_max_spread_pct       = 0.030;  // 80% of max viable spread at $50 risk RR2.0
    double audusd_compression_threshold = 0.82;
    // NZDUSD
    double nzdusd_tp_pct               = 0.075;
    double nzdusd_sl_pct               = 0.038;
    double nzdusd_vol_thresh_pct       = 0.010;
    int    nzdusd_min_gap_sec          = 45;
    double nzdusd_momentum_thresh_pct  = 0.015;
    double nzdusd_min_breakout_pct     = 0.075;
    double nzdusd_max_spread_pct       = 0.035;  // 80% of max viable spread at $50 risk RR2.0
    double nzdusd_compression_threshold = 0.82;
    // USDJPY
    double usdjpy_tp_pct               = 0.090;
    double usdjpy_sl_pct               = 0.045;
    double usdjpy_vol_thresh_pct       = 0.012;
    int    usdjpy_min_gap_sec          = 45;
    double usdjpy_momentum_thresh_pct  = 0.018;
    double usdjpy_min_breakout_pct     = 0.090;
    double usdjpy_max_spread_pct       = 0.013;  // 80% of max viable spread at $50 risk RR2.0
    double usdjpy_compression_threshold = 0.80;
    // Asia FX lot limits
    double max_lot_audusd = 5.00;
    double max_lot_nzdusd = 5.00;
    double max_lot_usdjpy = 5.00;
    double min_lot_audusd = 0.01;
    double min_lot_nzdusd = 0.01;
    double min_lot_usdjpy = 0.01;
    // Extended symbol IDs for Asia FX
    int    ext_audusd_id = 0;
    int    ext_nzdusd_id = 0;
    int    ext_usdjpy_id = 0;

    // Bracket engines (Gold + Silver)
    int    bracket_gold_lookback      = 40;
    double bracket_gold_tp_pct        = 0.25;
    double bracket_gold_sl_pct        = 0.12;
    double bracket_gold_min_range_pct = 0.04;
    double bracket_gold_max_spread_pct = 0.06;
    int    bracket_gold_min_gap_sec   = 90;
    int    bracket_gold_cooldown_sl_sec = 120;
    int    bracket_gold_max_hold_sec  = 1800;
    int    bracket_xag_lookback       = 40;
    double bracket_xag_tp_pct         = 0.20;
    double bracket_xag_sl_pct         = 0.10;
    double bracket_xag_min_range_pct  = 0.035;
    double bracket_xag_max_spread_pct = 0.08;
    int    bracket_xag_min_gap_sec    = 90;
    int    bracket_xag_cooldown_sl_sec = 120;
    int    bracket_xag_max_hold_sec   = 1800;

    // GUI
    int         gui_port   = 7779;
    int         ws_port    = 7780;
    int         trade_port = 5212;   // FIX trade connection (orders)
    std::string shadow_csv = "omega_shadow.csv";
    std::string log_file   = "";   // if set, tee all stdout+stderr here

    // GoldEngineStack config -- passed to g_gold_stack.configure()
    omega::gold::GoldStackCfg gs_cfg;

    // ?? cTrader Open API v2 -- depth feed ????????????????????????????????????
    // Provides real multi-level L2 book data (ProtoOADepthEvent) in parallel
    // with the FIX connection. BlackBull FIX only supports 264=1 (top-of-book);
    // cTrader Open API has no depth limitation.
    // Obtain tokens: openapi.ctrader.com/apps/20304/playground (Account info scope)
    // ctid_trader_account_id: found via GetAccountListByAccessToken (8077780 = 44735058)
    std::string ctrader_access_token        = "";
    std::string ctrader_refresh_token       = "";
    int64_t     ctrader_ctid_account_id     = 0;
    bool        ctrader_depth_enabled       = false;  // set true when token is configured

    // LatencyEdgeStack config -- passed to g_latency_edge.configure()
    omega::latency::LatencyEdgeCfg le_cfg;
};

static OmegaConfig         g_cfg;
static std::atomic<bool>   g_running(true);
static std::atomic<bool>   g_emergency_close(false);  // set by GUI button ? closes all positions immediately

// ?? cTrader Open API depth client -- parallel to FIX, read-only L2 feed ???????
static CTraderDepthClient  g_ctrader_depth;
// OHLC bar state -- populated by cTrader trendbar API (M1+M5 history + live bars)
// Read lock-free by GoldFlow and GoldStack via atomic accessors.
static SymBarState         g_bars_gold;   // XAUUSD M1/M5 bars + indicators
static SymBarState         g_bars_sp;     // US500.F M1/M5 bars + indicators
static SymBarState         g_bars_nq;     // USTEC.F M1/M5 bars + indicators
static SymBarState         g_bars_ger;    // GER40   M1/M5 bars + indicators
static OmegaVolTargeter    g_vol_targeter;   // EWMA vol targeting + momentum regime
static OmegaSignalScorer   g_signal_scorer;   // Composite signal scoring (13-point system)
static OmegaCrowdingGuard  g_crowding_guard;  // Directional crowding tracker (RenTec #4)
static OmegaWalkForward    g_walk_forward;    // Rolling walk-forward OOS validation (RenTec #6)
static OmegaParamGate      g_param_gate;      // Adaptive entry score threshold (RenTec #7)
static std::atomic<bool>   g_quote_logout_received(false);  // server sent Logout to quote session
static std::atomic<bool>   g_shutdown_done(false);  // set by main() after all cleanup -- unblocks ctrl handler
static std::atomic<bool>   g_trade_thread_done(false);  // set by trade_loop() just before it returns
static const int64_t       g_start_time = static_cast<int64_t>(std::time(nullptr)); // process start for uptime

// ?????????????????????????????????????????????????????????????????????????????
// Globals
// ?????????????????????????????????????????????????????????????????????????????
static OmegaTelemetryWriter      g_telemetry;  // NOTE: always g_telemetry -- never g_telem
omega::OmegaTradeLedger          g_omegaLedger;      // extern in TelemetryServer.cpp
static omega::MacroRegimeDetector g_macroDetector;
static omega::HTFBiasFilter       g_htf_filter;       // higher-timeframe bias (daily+intraday)

// ?? Adaptive intelligence layer ???????????????????????????????????????????????
static omega::risk::AdaptiveRiskManager   g_adaptive_risk;   // Kelly, Sharpe, DD throttle, corr heat
static omega::risk::PortfolioVaR          g_portfolio_var;   // correlation-adjusted portfolio VaR gate
static omega::news::NewsBlackout          g_news_blackout;   // NFP/FOMC/CPI/EIA/ECB event blackouts
static omega::news::LiveCalendarFetcher   g_live_calendar;   // Forex Factory live calendar (HTTPS)
static omega::edges::EdgeContext          g_edges;           // 7 institutional edges
static omega::partial::PartialExitManager g_partial_exit;    // split TP: 50% at 1R, trail remainder
static omega::regime::RegimeAdaptor       g_regime_adaptor;  // regime-adaptive engine weights + vol
static omega::corr::CorrelationMatrix     g_corr_matrix;     // EWM rolling corr + vol-parity sizing
static omega::vpin::VPINTracker           g_vpin;            // tick-classified VPIN toxicity gate

// CRTP breakout engines -- typed per symbol (instrument-specific params + regime gating)
#include "globals.hpp"
static void set_ctrader_tick_ms(const std::string& sym, int64_t ms) noexcept {
    auto* p = get_ctrader_tick_ms_ptr(sym);
    if (p) p->store(ms, std::memory_order_relaxed);
}
static bool ctrader_depth_is_live(const std::string& sym) noexcept {
    auto* p = get_ctrader_tick_ms_ptr(sym);
    if (!p) return false;
    const int64_t last = p->load(std::memory_order_relaxed);
    if (last == 0) return false;
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return (now_ms - last) < 500;  // fresh if cTrader event arrived within 500ms
}

// RTT
static double              g_rtt_last = 0.0, g_rtt_p50 = 0.0, g_rtt_p95 = 0.0;
// Live USDJPY mid -- updated every tick, used by tick_value_multiplier().
// Avoids the static 667 approximation (100000/150) drifting ?8% as rate moves.
// Initialised to 150.0 so the function is safe before the first USDJPY tick arrives.
static std::atomic<double> g_usdjpy_mid{150.0};
static std::deque<double>  g_rtts;
static int64_t             g_rtt_pending_ts = 0;
static std::string         g_rtt_pending_id;

// Governor counters
static int     g_gov_spread  = 0;
static int     g_gov_lat     = 0;
static int     g_gov_pnl     = 0;
// Last lot size computed by enter_directional -- written on success so callers
// can patch pos_.size for accurate shadow P&L simulation.
static double  g_last_directional_lot = 0.01;
static int     g_gov_pos     = 0;
static int     g_gov_consec  = 0;

struct SymbolRiskState {
    double daily_pnl = 0.0;
    int    consec_losses = 0;
    int64_t pause_until = 0;
};
struct ShadowQualityState {
    int     fast_loss_streak  = 0;
    int64_t pause_until       = 0;
    int     engine_consec_sl  = 0;   // consecutive SL_HIT per engine key
    bool    engine_culled     = false; // culled until session restart
};
static std::mutex g_sym_risk_mtx;
static std::unordered_map<std::string, SymbolRiskState> g_sym_risk;

// ?? Hourly P&L ring buffer -- rolling 2-hour loss throttle ????????????????????
// Records net_pnl of each closed trade with its close timestamp.
// On each symbol_gate call we sum trades from last 2h and block if > hourly_loss_limit.
struct HourlyPnlRecord { int64_t ts_sec; double net_pnl; };
static std::mutex                    g_hourly_pnl_mtx;
static std::deque<HourlyPnlRecord>   g_hourly_pnl_records;
static constexpr int64_t HOURLY_WINDOW_SEC = 7200; // 2-hour rolling window
static std::unordered_map<std::string, ShadowQualityState> g_shadow_quality;

// Latency governor -- blocks trades when FIX RTT exceeds configured hard cap
struct Governor {
    bool checkLatency(double latency_ms, double cfg_max_ms) const noexcept {
        if (cfg_max_ms > 0.0) return latency_ms <= cfg_max_ms;
        return latency_ms <= 2000.0;  // fallback: only block dead connections
    }
};
static Governor g_governor;
static int     g_last_ledger_utc_day = -1;

// ?? Stale quote watchdog ?????????????????????????????????????????????????????
// Tracks the last time any tick was received per symbol.
// If last_tick_age > STALE_QUOTE_SEC while positions are open ? widen SLs and
// alert. A frozen feed with open positions is the most dangerous state.
static std::mutex                              g_last_tick_mtx;
static std::unordered_map<std::string,int64_t> g_last_tick_ts;   // symbol ? unix ms of last tick
static std::unordered_map<std::string,double>  g_last_tick_bid;  // symbol ? last bid price
static std::unordered_map<std::string,int>     g_frozen_count;   // symbol ? consecutive identical ticks
static constexpr int64_t STALE_QUOTE_SEC  = 30;   // 30s without tick = genuinely stale feed
static constexpr int     FROZEN_TICK_MAX  = 20;   // 20 consecutive identical bids = frozen feed
                                                   // At ~1 tick/10s: 20 ticks = 3.3 min of freeze
                                                   // Price-freeze detection catches brokers that
                                                   // repeat last-price with updated timestamps.

// Record a tick receipt (called from on_tick per symbol)
// Also tracks consecutive identical bids for frozen-feed detection.
static inline void stale_watchdog_ping(const std::string& sym, double bid = 0.0) {
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(g_last_tick_mtx);
    g_last_tick_ts[sym] = now_ms;
    if (bid > 0.0) {
        auto it = g_last_tick_bid.find(sym);
        if (it != g_last_tick_bid.end() && std::fabs(it->second - bid) < 0.001) {
            g_frozen_count[sym]++;
        } else {
            g_frozen_count[sym] = 0;
            g_last_tick_bid[sym] = bid;
        }
    }
}

// Returns true if symbol has received a tick within STALE_QUOTE_SEC
static inline bool stale_watchdog_ok(const std::string& sym) {
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(g_last_tick_mtx);
    auto it = g_last_tick_ts.find(sym);
    if (it == g_last_tick_ts.end()) return false;  // never received -- treat as stale
    return (now_ms - it->second) < STALE_QUOTE_SEC * 1000;
}

// ?? Weekend gap sizing ???????????????????????????????????????????????????????
// Friday 21:00 UTC through Sunday 22:00 UTC -- markets closed, gap risk high.
// GOLD can gap 1.5-2% on Sunday open. Size is halved, SL widened.
// Returns a multiplier [0.5, 1.0] to apply to computed lot size.
// Always 1.0 during normal sessions.
static inline double weekend_gap_size_scale() {
    const int64_t now_sec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    time_t t = static_cast<time_t>(now_sec);
    struct tm ti{};
#ifdef _WIN32
    gmtime_s(&ti, &t);
#else
    gmtime_r(&t, &ti);
#endif
    // tm_wday: 0=Sun 1=Mon 2=Tue 3=Wed 4=Thu 5=Fri 6=Sat
    const int wday = ti.tm_wday;
    const int hour = ti.tm_hour;
    // Friday >= 21:00 UTC or Saturday (all day) or Sunday < 22:00 UTC
    const bool in_gap_window =
        (wday == 5 && hour >= 21) ||   // Friday night
        (wday == 6) ||                 // All day Saturday
        (wday == 0 && hour < 22);      // Sunday before open
    if (in_gap_window) {
        static int64_t s_gap_log = 0;
        if (now_sec - s_gap_log > 3600) {  // log once per hour
            s_gap_log = now_sec;
            std::printf("[WEEKEND-GAP] Gap window active -- sizing 0.5x\n");
        }
        return 0.50;
    }
    return 1.00;
}

// Trade connection (port 5212) -- separate SSL from quote (port 5211)
static SSL*               g_trade_ssl  = nullptr;
static int                g_trade_sock = -1;
static std::atomic<bool>  g_trade_ready{false};
static std::mutex         g_trade_mtx;
static int                g_trade_seq  = 1;
static std::atomic<bool>  g_quote_ready{false};
static std::atomic<int64_t> g_connected_since{0};  // unix seconds of last successful logon
static std::atomic<bool>  g_ext_md_refresh_needed{false};
static std::atomic<bool>  g_md_subscribed{false};   // true once OMEGA-MD-ALL is active on this session

// Live unrealised P&L -- updated every tick, read by GUI and risk gates.
// Stored as int64 (cents) to allow lock-free atomic read/write.
// GUI daily_pnl = closed_pnl + g_open_unrealised_pnl_cents / 100.0
static std::atomic<int64_t> g_open_unrealised_cents{0};

// Portfolio open SL risk tracker -- sum of max_dollar_loss across all open positions.
// Incremented on entry (sl_pts * lot * tick_value), decremented on close.
// Stored in cents (int64) for lock-free atomic access on hot path.
// Checked in symbol_gate when max_portfolio_sl_risk_usd > 0.
static std::atomic<int64_t> g_open_sl_risk_cents{0};

inline void portfolio_sl_risk_add(double sl_pts, double lot, double tick_value) {
    if (sl_pts <= 0.0 || lot <= 0.0 || tick_value <= 0.0) return;
    const int64_t cents = static_cast<int64_t>((sl_pts * lot * tick_value) * 100.0);
    g_open_sl_risk_cents.fetch_add(cents, std::memory_order_relaxed);
}
inline void portfolio_sl_risk_sub(double sl_pts, double lot, double tick_value) {
    if (sl_pts <= 0.0 || lot <= 0.0 || tick_value <= 0.0) return;
    const int64_t cents = static_cast<int64_t>((sl_pts * lot * tick_value) * 100.0);
    // Never go below zero -- defensive against double-decrements
    const int64_t prev = g_open_sl_risk_cents.fetch_sub(cents, std::memory_order_relaxed);
    if (prev < cents) g_open_sl_risk_cents.store(0, std::memory_order_relaxed);
}

// Shadow CSV
static std::ofstream g_shadow_csv;
static std::ofstream g_trade_close_csv;
static std::ofstream g_trade_open_csv;   // entry-time log -- one row per position opened
static std::mutex    g_trade_close_csv_mtx;

struct PerfStats {
    int live_trades = 0;
    int live_wins = 0;
    int live_losses = 0;
    double live_pnl = 0.0;
    int shadow_trades = 0;
    int shadow_wins = 0;
    int shadow_losses = 0;
    double shadow_pnl = 0.0;
    bool disabled = false;
};
static std::mutex g_perf_mtx;
static std::unordered_map<std::string, PerfStats> g_perf;
static bool g_disable_gold_stack = false;

static std::string perf_key_from_trade(const omega::TradeRecord& tr) {
    // Route gold trades to the specific engine that generated them,
    // not a single "GOLD_STACK" bucket. This prevents auto-disable
    // from misfiring when losses are from flow/bracket, not the stack.
    if (tr.symbol == "XAUUSD") {
        if (tr.engine.find("BRACKET") != std::string::npos)   return "XAUUSD_BRACKET";
        if (tr.engine.find("L2_FLOW") != std::string::npos ||
            tr.engine.find("GOLD_FLOW") != std::string::npos) return "XAUUSD_FLOW";
        if (tr.engine.find("LEAD_LAG") != std::string::npos)  return "XAUUSD_LATENCY";
        return "GOLD_STACK";  // CompressionBreakout / Impulse / SessionMom / VWAPSnap / SweepPro
    }
    return tr.symbol;
}

static std::string build_trade_close_csv_row(const omega::TradeRecord& tr);
static void write_trade_open_log(const std::string& symbol, const std::string& engine,
                                  const std::string& side, double entry_px, double tp,
                                  double sl, double size, double spread_at_entry,
                                  const std::string& regime, const std::string& reason);
static void print_perf_stats() {
    std::lock_guard<std::mutex> lk(g_perf_mtx);
    if (g_perf.empty()) return;
    for (const auto& kv : g_perf) {
        const auto& k = kv.first;
        const auto& s = kv.second;
        std::cout << "[OMEGA-PERF] " << k
                  << " liveT=" << s.live_trades
                  << " livePnL=" << std::fixed << std::setprecision(2) << s.live_pnl
                  << " WR=" << std::fixed << std::setprecision(1)
                  << (s.live_trades > 0 ? (100.0 * s.live_wins / s.live_trades) : 0.0)
                  << "% shadowT=" << s.shadow_trades
                  << " shadowPnL=" << std::fixed << std::setprecision(2) << s.shadow_pnl
                  << " disabled=" << (s.disabled ? 1 : 0) << "\n";
        std::cout.unsetf(std::ios::fixed);
        std::cout << std::setprecision(6); // restore default precision
    }
}

// ?????????????????????????????????????????????????????????????????????????????
// RollingTeeBuffer -- mirrors stdout to a daily rolling log file
// Rotates at UTC midnight. Keeps LOG_KEEP_DAYS files, deletes older ones.
// File naming: logs/omega_YYYY-MM-DD.log
// Thread-safe: mtx_ serialises all streambuf operations.
// Without this mutex the FIX thread and cTrader depth thread both write to
// std::cout concurrently, corrupting at_line_start_ and the ofstream state
// -> undefined behaviour -> crash ~30s into startup during the book burst.
// ?????????????????????????????????????????????????????????????????????????????
class RollingTeeBuffer : public std::streambuf {
public:
    static constexpr int LOG_KEEP_DAYS = 10;

    explicit RollingTeeBuffer(std::streambuf* orig, const std::string& log_dir)
        : orig_(orig), log_dir_(log_dir)
    {
        open_today();
    }

    int overflow(int c) override {
        if (c == EOF) return !EOF;
        std::lock_guard<std::mutex> lk(mtx_);
        check_rotate();
        orig_->sputc(static_cast<char>(c));
        if (file_buf_) {
            if (at_line_start_ && c != '\n') {
                write_ts_prefix();
                at_line_start_ = false;
            }
            file_buf_->sputc(static_cast<char>(c));
            if (c == '\n') {
                file_.flush();
                at_line_start_ = true;
            }
        }
        return c;
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::lock_guard<std::mutex> lk(mtx_);
        check_rotate();
        orig_->sputn(s, n);
        if (file_buf_) {
            const char* p   = s;
            const char* end = s + n;
            while (p < end) {
                if (at_line_start_) {
                    write_ts_prefix();
                    at_line_start_ = false;
                }
                const char* nl = static_cast<const char*>(
                    std::memchr(p, '\n', static_cast<size_t>(end - p)));
                if (nl) {
                    file_buf_->sputn(p, (nl - p) + 1);
                    at_line_start_ = true;
                    p = nl + 1;
                } else {
                    file_buf_->sputn(p, end - p);
                    break;
                }
            }
            file_.flush();
        }
        return n;
    }

    std::string current_path() const { std::lock_guard<std::mutex> lk(mtx_); return current_path_; }
    bool is_open() const { std::lock_guard<std::mutex> lk(mtx_); return file_.is_open(); }

    void force_rotate_check() {
        std::lock_guard<std::mutex> lk(mtx_);
        const std::string before = current_path_;
        check_rotate();
        // If file rotated, write a header so we know log is alive
        if (current_path_ != before && file_buf_) {
            const std::string hdr = "[OMEGA-LOG] Daily rotation -- new log: " + current_path_ + "\n";
            file_buf_->sputn(hdr.c_str(), (std::streamsize)hdr.size());
            file_.flush();
        }
    }

    void flush_and_close() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (file_.is_open()) { file_.flush(); file_.close(); }
        file_buf_ = nullptr;
    }

private:
    mutable std::mutex mtx_;  // serialises all writes -- FIX + cTrader threads both use std::cout
    std::streambuf* orig_;
    std::string     log_dir_;
    std::ofstream   file_;
    std::streambuf* file_buf_ = nullptr;
    std::string     current_path_;
    int             current_day_ = -1;
    bool            at_line_start_ = true;  // true when next char starts a new line

    void write_ts_prefix() {
        // UTC HH:MM:SS prefix injected into file only (not console)
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch()) % 1000;
        struct tm ti{};
        gmtime_s(&ti, &t);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d ",
                      ti.tm_hour, ti.tm_min, ti.tm_sec);
        file_buf_->sputn(buf, 9);
    }

    static std::string utc_date_str() {
        auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        struct tm ti{};
        gmtime_s(&ti, &t);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                      ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
        return buf;
    }

    static int utc_day_of_year() {
        auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        struct tm ti{};
        gmtime_s(&ti, &t);
        return ti.tm_yday;
    }

    void open_today() {
        if (file_.is_open()) { file_.flush(); file_.close(); file_buf_ = nullptr; }
        // Use filesystem::create_directories -- _mkdir returns -1 if dir exists
        // which was silently ignored, but more importantly create_directories
        // handles nested paths and Unicode correctly on Windows.
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(fs::path(log_dir_), ec);
        current_path_ = log_dir_ + "/omega_" + utc_date_str() + ".log";
        file_.open(current_path_, std::ios::app);
        file_buf_    = file_.is_open() ? file_.rdbuf() : nullptr;
        current_day_      = utc_day_of_year();
        current_date_str_ = utc_date_str();
        if (!file_.is_open()) {
            // Print direct to orig_ (console) -- tee not usable if file failed
            const std::string msg = "[OMEGA-LOG-FAIL] Cannot open log: " + current_path_ + "\n";
            if (orig_) orig_->sputn(msg.c_str(), (std::streamsize)msg.size());
        }
        purge_old_logs();
    }

    void check_rotate() {
        // Compare full date string not just day-of-year -- day-of-year wraps
        // at year boundary and can miss the Jan 1 rotation.
        if (utc_date_str() != current_date_str_)
            open_today();
    }

    std::string current_date_str_; // set in open_today via utc_date_str()

    void purge_old_logs() {
        // Enumerate logs/omega_*.log:
        //   - Files older than LOG_ARCHIVE_AFTER_DAYS: compress to logs/archive/omega_YYYY-MM-DD.zip
        //     and delete the original. Keeps full history without eating disk.
        //   - Files older than LOG_KEEP_DAYS total: delete even the zip (hard cap).
        // This means: 2 days of uncompressed logs (fast grep), 8 more days as zips.
        // Archive dir is created automatically.
        static constexpr int LOG_ARCHIVE_AFTER_DAYS = 2;

        // Ensure archive directory exists
        const std::string archive_dir = log_dir_ + "/archive";
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::create_directories(fs::path(archive_dir), ec);
        }

        WIN32_FIND_DATAA fd{};
        std::string pattern = log_dir_ + "/omega_*.log";
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;

        // Collect all matching filenames
        std::vector<std::string> files;
        do {
            // Skip today's file — never archive the live log
            const std::string fname = log_dir_ + "/" + fd.cFileName;
            if (fname != current_path_)
                files.push_back(fname);
        } while (FindNextFileA(h, &fd));
        FindClose(h);

        // Sort ascending -- oldest first
        std::sort(files.begin(), files.end());

        // Archive files older than LOG_ARCHIVE_AFTER_DAYS using PowerShell Compress-Archive
        // Files beyond LOG_KEEP_DAYS total are hard-deleted (including their zips).
        const int total = static_cast<int>(files.size());
        for (int i = 0; i < total; ++i) {
            const std::string& fpath = files[i];
            // Extract date from filename: omega_YYYY-MM-DD.log
            const std::string fname  = fpath.substr(fpath.find_last_of("/\\") + 1);
            const std::string zip_path = archive_dir + "/" + fname.substr(0, fname.size() - 4) + ".zip";

            const bool should_archive = (total - i) > LOG_ARCHIVE_AFTER_DAYS;
            const bool should_delete  = (total - i) > LOG_KEEP_DAYS;

            if (should_delete) {
                // Hard cap exceeded — delete log and zip
                DeleteFileA(fpath.c_str());
                DeleteFileA(zip_path.c_str());
                continue;
            }

            if (should_archive) {
                // Check if zip already exists — don't re-compress
                WIN32_FIND_DATAA zfd{};
                const HANDLE zh = FindFirstFileA(zip_path.c_str(), &zfd);
                const bool zip_exists = (zh != INVALID_HANDLE_VALUE);
                if (zh != INVALID_HANDLE_VALUE) FindClose(zh);

                if (!zip_exists) {
                    // Use PowerShell Compress-Archive (available Windows 5+)
                    // Run hidden, fire-and-forget — don't block the tick loop
                    const std::string cmd = "powershell -WindowStyle Hidden -Command "
                        "\"Compress-Archive -Path '" + fpath + "' "
                        "-DestinationPath '" + zip_path + "' -Force\" & exit";
                    // Build the zip check path as a named std::string BEFORE the lambda.
                    // The old code built a temporary string expression inside the lambda
                    // and immediately called .c_str() on it -- the temporary was destroyed
                    // before FindFirstFileA read the pointer (dangling pointer UB).
                    // Capturing the fully-formed std::string by value guarantees lifetime.
                    const std::string zip_check_path =
                        (fpath.substr(0, fpath.rfind('/') + 1) + "archive/" +
                         fpath.substr(fpath.rfind('/') + 1, fpath.rfind('.') - fpath.rfind('/') - 1) + ".zip");
                    std::thread([cmd, fpath, zip_check_path]() {
                        // Small delay so Omega doesn't hammer disk on startup
                        std::this_thread::sleep_for(std::chrono::seconds(5));
                        std::system(cmd.c_str());
                        // Delete original after successful zip
                        WIN32_FIND_DATAA cfd{};
                        const HANDLE ch = FindFirstFileA(zip_check_path.c_str(), &cfd);
                        if (ch != INVALID_HANDLE_VALUE) {
                            FindClose(ch);
                            DeleteFileA(fpath.c_str());
                        }
                    }).detach();
                } else {
                    // Zip already exists — safe to delete original
                    DeleteFileA(fpath.c_str());
                }
            }
        }
    }
};

static std::string utc_date_for_ts(int64_t ts);

class RollingCsvLogger {
public:
    static constexpr int LOG_KEEP_DAYS = 10;

    RollingCsvLogger(std::string dir, std::string stem, std::string header)
        : dir_(std::move(dir)), stem_(std::move(stem)), header_(std::move(header)) {}

    void append_row(int64_t ts_utc, const std::string& row) {
        std::lock_guard<std::mutex> lk(mtx_);
        open_for_ts(ts_utc);
        if (!file_.is_open()) return;
        file_ << row << '\n';
        file_.flush();
    }

    std::string current_path() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return current_path_;
    }

    void close() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
        current_path_.clear();
        current_date_.clear();
    }

private:
    mutable std::mutex mtx_;
    std::string dir_;
    std::string stem_;
    std::string header_;
    std::ofstream file_;
    std::string current_path_;
    std::string current_date_;

    void open_for_ts(int64_t ts_utc) {
        const std::string date = utc_date_for_ts(ts_utc);
        if (date == current_date_ && file_.is_open()) return;

        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(fs::path(dir_), ec);

        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }

        current_date_ = date;
        current_path_ = dir_ + "/" + stem_ + "_" + date + ".csv";
        file_.open(current_path_, std::ios::app);
        if (!file_.is_open()) return;

        file_.seekp(0, std::ios::end);
        if (file_.tellp() == std::streampos(0))
            file_ << header_ << '\n';
        purge_old_logs();
    }

    void purge_old_logs() {
        namespace fs = std::filesystem;
        std::error_code ec;
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(fs::path(dir_), ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            const auto name = entry.path().filename().string();
            if (name.rfind(stem_ + "_", 0) != 0) continue;
            if (entry.path().extension() != ".csv") continue;
            files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        while (static_cast<int>(files.size()) > LOG_KEEP_DAYS) {
            fs::remove(files.front(), ec);
            files.erase(files.begin());
        }
    }
};

static RollingTeeBuffer* g_tee_buf   = nullptr;
static std::streambuf*   g_orig_cout = nullptr;
static std::unique_ptr<RollingCsvLogger> g_daily_trade_close_log;
static std::unique_ptr<RollingCsvLogger> g_daily_gold_trade_close_log;
static std::unique_ptr<RollingCsvLogger> g_daily_shadow_trade_log;
static std::unique_ptr<RollingCsvLogger> g_daily_trade_open_log;   // entry-time rolling log

// FIX recv buffer -- owned by extract_messages() as a static local

// ?????????????????????????????????????????????????????????????????????????????
// Helpers
// ?????????????????????????????????????????????????????????????????????????????

static int64_t nowSec() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string timestamp() {
    const auto tp = std::chrono::system_clock::now();
    const auto t  = std::chrono::system_clock::to_time_t(tp);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        tp.time_since_epoch()) % 1000;
    struct tm ti; gmtime_s(&ti, &t);
    std::ostringstream o;
    o << std::put_time(&ti, "%Y%m%d-%H:%M:%S")
      << '.' << std::setw(3) << std::setfill('0') << ms.count();
    return o.str();
}

static void utc_tm_from_ts(int64_t ts, struct tm& ti) noexcept {
    const auto t = static_cast<time_t>(ts);
    gmtime_s(&ti, &t);
}

static std::string utc_iso8601(int64_t ts) {
    struct tm ti{};
    utc_tm_from_ts(ts, ti);
    std::ostringstream o;
    o << std::put_time(&ti, "%Y-%m-%dT%H:%M:%SZ");
    return o.str();
}

static std::string utc_date_for_ts(int64_t ts) {
    struct tm ti{};
    utc_tm_from_ts(ts, ti);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
    return buf;
}

static const char* utc_weekday_name(int64_t ts) noexcept {
    static const char* DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    struct tm ti{};
    utc_tm_from_ts(ts, ti);
    const int idx = (ti.tm_wday >= 0 && ti.tm_wday < 7) ? ti.tm_wday : 0;
    return DAYS[idx];
}

static std::string csv_quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (const char c : s) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

static std::string log_root_dir() {
    if (!g_cfg.log_file.empty()) {
        const size_t slash = g_cfg.log_file.find_last_of("/\\");
        if (slash != std::string::npos) return g_cfg.log_file.substr(0, slash);
    }
    // Always absolute path -- exe runs from C:\Omega\build\Release\
    // Relative "logs" fallback silently lost all logs to build dir. Removed.
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string abs = "C:\\Omega\\logs";
    fs::create_directories(fs::path(abs), ec);
    if (ec) {
        fprintf(stderr, "[OMEGA-FATAL] Cannot create log dir %s: %s\n",
                abs.c_str(), ec.message().c_str());
        fflush(stderr);
    }
    return abs;  // always absolute path -- no relative fallback
}

static std::string resolve_audit_log_path(const std::string& configured_path,
                                          const std::string& default_relative_path) {
    namespace fs = std::filesystem;
    if (configured_path.empty()) return log_root_dir() + "/" + default_relative_path;
    const fs::path p(configured_path);
    if (p.is_absolute() || p.has_parent_path()) return configured_path;
    const fs::path base = fs::path(log_root_dir()) / "shadow" / p;
    return base.generic_string();
}

static void ensure_parent_dir(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(path);
    if (!p.parent_path().empty()) fs::create_directories(p.parent_path(), ec);
}


// ?????????????????????????????????????????????????????????????????????????????
// Tick value normalisation -- converts raw price-point PnL to USD equivalent.
// BlackBull CFD lot sizes (standard 1-lot). Adjust if trading mini/micro lots.
// Used by: daily_loss_limit check, shadow PnL accumulation, GUI display.
// ?????????????????????????????????????????????????????????????????????????????
#include "sizing.hpp"
#include "fix_builders.hpp"
// Does nothing in SHADOW mode. Returns clOrdId on success, empty on failure/shadow.
#include "order_exec.hpp"
// ?????????????????????????????????????????????????????????????????????????????
#include "logging.hpp"
// ?????????????????????????????????????????????????????????????????????????????
#include "engine_config.hpp"
#include "config.hpp"
#include "trade_lifecycle.hpp"

static void on_tick(const std::string& sym, double bid, double ask) {
    // ?? Tick spike filter ???????????????????????????????????????????????
    // Reject ticks where mid moves > 5x slow ATR in a single step.
    // Broker bad ticks (gold at 46420 instead of 4642) distort ATR, VWAP,
    // and can trigger entries on garbage data. Hard reject -- drops tick
    // entirely before any engine state is touched.
    // Threshold: 5 * atr_slow. Gold ATR_SLOW~10pts -> threshold=50pts.
    // A real $50 gold move in a single tick is physically impossible.
    // Warmup: skip filter until atr_slow is populated (~100 ticks/symbol).
    {
        static std::mutex                              s_spike_mtx;
        static std::unordered_map<std::string, double> s_prev_mid;

        if (bid <= 0.0 || ask <= 0.0 || bid >= ask) return;  // bad quote

        const double mid = (bid + ask) * 0.5;
        double prev = 0.0;
        {
            std::lock_guard<std::mutex> lk(s_spike_mtx);
            auto it = s_prev_mid.find(sym);
            if (it != s_prev_mid.end()) prev = it->second;
            s_prev_mid[sym] = mid;
        }

        if (prev > 0.0) {
            const double atr_s = g_adaptive_risk.vol_scaler.atr_slow(sym);
            if (atr_s > 0.0) {
                const double move      = std::fabs(mid - prev);
                // Per-symbol minimum threshold floor -- prevents near-zero atr_slow
                // (during warmup or quiet tape) from rejecting valid ticks.
                // Root cause: atr_slow for XAUUSD was 0.11pts -> threshold=0.54pts
                // -> every normal gold tick-to-tick move rejected -> bars/RSI starved.
                // Gold moves 0.5-2pts per tick normally. Floor = 5pts (50x normal tick).
                // A real bad-tick on gold (e.g. 46420 instead of 4642) moves 800pts.
                // FX floor = 0.002 (20 pips). Index floor = 5pts. Oil floor = 0.5pts.
                const double min_threshold =
                    (sym == "XAUUSD" || sym == "XAGUSD") ? 5.0   :  // gold/silver: 5pt floor
                    (sym == "US500.F" || sym == "USTEC.F" || sym == "DJ30.F" ||
                     sym == "NAS100"  || sym == "GER40"   || sym == "UK100"  ||
                     sym == "ESTX50")                     ? 5.0   :  // indices: 5pt floor
                    (sym == "USOIL.F" || sym == "BRENT")  ? 0.5   :  // oil: 0.5pt floor
                                                            0.002;   // FX: 20 pip floor
                const double threshold = std::max(5.0 * atr_s, min_threshold);
                if (move > threshold) {
                    printf("[TICK-SPIKE] %s REJECTED mid=%.5f prev=%.5f move=%.5f threshold=%.5f (5xATR_SLOW)\n",
                           sym.c_str(), mid, prev, move, threshold);
                    fflush(stdout);
                    return;  // drop tick -- no engine state updated
                }
            }
        }
    }

    { std::lock_guard<std::mutex> lk(g_book_mtx); g_bids[sym] = bid; g_asks[sym] = ask; }
    stale_watchdog_ping(sym, bid);  // record tick + price for frozen-feed detection

    // ?? Edge system updates (every tick, every symbol) ????????????????????????
    {
        const double mid = (bid + ask) * 0.5;
        g_edges.cvd.update(sym, bid, ask);
        g_edges.spread_gate.update(sym, ask - bid);
        g_edges.prev_day.update(sym, mid, nowSec());
        // Edge 8: Volume profile -- track time-at-price every tick
        g_edges.vol_profile.update(sym, mid);
        // Edge 9: Order flow absorption -- detect institutional fading of moves
        // Use the per-symbol L2 imbalance from MacroContext (updated just above this)
        {
            double l2_imb = 0.5;  // neutral fallback
            if      (sym == "XAUUSD")  l2_imb = g_macro_ctx.gold_l2_imbalance;
            else if (sym == "US500.F") l2_imb = g_macro_ctx.sp_l2_imbalance;
            else if (sym == "USTEC.F") l2_imb = g_macro_ctx.nq_l2_imbalance;
            else if (sym == "XAGUSD")  l2_imb = g_macro_ctx.xag_l2_imbalance;
            else if (sym == "USOIL.F") l2_imb = g_macro_ctx.cl_l2_imbalance;
            else if (sym == "BRENT")   l2_imb = g_macro_ctx.brent_l2_imbalance;
            else if (sym == "EURUSD")  l2_imb = g_macro_ctx.eur_l2_imbalance;
            else if (sym == "GBPUSD")  l2_imb = g_macro_ctx.gbp_l2_imbalance;
            else if (sym == "AUDUSD")  l2_imb = g_macro_ctx.aud_l2_imbalance;
            else if (sym == "NZDUSD")  l2_imb = g_macro_ctx.nzd_l2_imbalance;
            else if (sym == "USDJPY")  l2_imb = g_macro_ctx.jpy_l2_imbalance;
            else if (sym == "GER40")   l2_imb = g_macro_ctx.ger40_l2_imbalance;
            else if (sym == "UK100")   l2_imb = g_macro_ctx.uk100_l2_imbalance;
            else if (sym == "ESTX50")  l2_imb = g_macro_ctx.estx50_l2_imbalance;
            else if (sym == "DJ30.F")  l2_imb = g_macro_ctx.us30_l2_imbalance;
            else if (sym == "NAS100")  l2_imb = g_macro_ctx.nas_l2_imbalance;
            g_edges.absorption.update(sym, mid, l2_imb);
        }
        // Edge 10: VPIN -- volume-synchronised informed flow toxicity
        // Detects institutional flow that doesn't show as L2 wall/vacuum.
        // Bucket-based: classifies each 50-tick window as buy/sell imbalance.
        g_edges.vpin.update(sym, mid);
        // HTF bias filter -- tracks daily + intraday momentum per symbol.
        // Used in lot sizing: 0.5? when trade opposes both TF trends.
        g_htf_filter.update(sym, mid);
    }  // end edge system updates

    // Seed vol history on first tick after reconnect -- avoids warmup dead zone.
    // seed() is a no-op if already warmed up.
    {
        const double mid = (bid + ask) * 0.5;
        if      (sym == "US500.F") g_eng_sp.seed(mid);
        else if (sym == "USTEC.F") g_eng_nq.seed(mid);
        else if (sym == "USOIL.F") g_eng_cl.seed(mid);
        else if (sym == "DJ30.F")  g_eng_us30.seed(mid);
        else if (sym == "NAS100")  g_eng_nas100.seed(mid);
        else if (sym == "GER40")   g_eng_ger30.seed(mid);
        else if (sym == "UK100")   g_eng_uk100.seed(mid);
        else if (sym == "ESTX50")  g_eng_estx50.seed(mid);
        else if (sym == "XAGUSD")  g_eng_xag.seed(mid);
        else if (sym == "EURUSD")  g_eng_eurusd.seed(mid);
        else if (sym == "GBPUSD")  g_eng_gbpusd.seed(mid);
        else if (sym == "AUDUSD")  g_eng_audusd.seed(mid);
        else if (sym == "NZDUSD")  g_eng_nzdusd.seed(mid);
        else if (sym == "USDJPY")  g_eng_usdjpy.seed(mid);
        else if (sym == "BRENT")   g_eng_brent.seed(mid);
        else if (sym == "XAUUSD") {
            // Pass VIX level so seed ATR scales to actual volatility regime.
            // VIX 27 day ? seed_atr=18pts ? SL=18pts -- survives real moves.
            // VIX 15 day ? seed_atr=5pts  ? SL=5pts  -- appropriate for quiet tape.
            g_gold_flow.seed(mid, g_macro_ctx.vix);
        }
    }

    // Rate-limit tick logging -- max 1 line per symbol per 30s to keep logs readable.
    // Previously logged every tick: thousands of lines/minute drowning signal output.
    {
        static std::mutex s_tick_log_mtx;
        static std::unordered_map<std::string, int64_t> s_last_tick_log;
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lk(s_tick_log_mtx);
        auto& last = s_last_tick_log[sym];
        if (now_ms - last >= 30000) {
            last = now_ms;
            std::cout << "[TICK] " << sym << " "
                      << std::fixed << std::setprecision(2) << bid << "/"
                      << std::fixed << std::setprecision(2) << ask << "\n";
            std::cout.unsetf(std::ios::fixed);
            std::cout << std::setprecision(6);
            std::cout.flush();
        }
    }

    maybe_reset_daily_ledger();

    const double mid = (bid + ask) * 0.5;
    if (sym == "VIX.F")   g_macroDetector.updateVIX(mid);
    if (sym == "DX.F")    g_macroDetector.updateDXY(mid);  // Dollar Index futures -- DXY momentum
    if (sym == "US500.F") g_macroDetector.updateES(mid);   // use traded futures, not cash ES
    if (sym == "USTEC.F") g_macroDetector.updateNQ(mid);   // use traded futures, not cash NAS100

    g_telemetry.UpdatePrice(sym.c_str(), bid, ask);

    const std::string regime = g_macroDetector.regime();
    g_telemetry.UpdateMacroRegime(
        g_macroDetector.vixLevel(), regime.c_str(), g_macroDetector.esNqDivergence());

    // Update shared MacroContext -- read by SP/NQ shouldTrade() overrides
    g_macro_ctx.regime     = regime;
    g_macro_ctx.vix        = g_macroDetector.vixLevel();
    g_macro_ctx.es_nq_div  = g_macroDetector.esNqDivergence();
    g_macro_ctx.sp_open    = g_eng_sp.pos.active;
    g_macro_ctx.nq_open    = g_eng_nq.pos.active;
    g_macro_ctx.oil_open   = g_eng_cl.pos.active;

    // ?? Feed adaptive intelligence layer ?????????????????????????????????????
    // Regime adaptor: track macro regime changes + per-symbol vol regime
    g_regime_adaptor.update(regime, g_macroDetector.vixLevel(), nowSec());
    // Vol update: use half-spread as proxy for tick vol (avoids needing prev_mid)
    // update_vol needs actual mid price (for bucket high/low range), not half-spread.
    // Passing half-spread was causing vol regime to track spread width, not price vol.
    g_regime_adaptor.update_vol(sym, (bid + ask) * 0.5, nowSec());
    g_adaptive_risk.update_vol(sym, (ask - bid) * 0.5);

    // Pending limit order cancel fallback -- runs every tick
    check_pending_limits();

    // Correlation cluster counts -- updated every tick for heat guard
    // Count open positions per cluster for CorrelationHeatGuard
    {
        const int us_eq = static_cast<int>(g_eng_sp.pos.active)
                        + static_cast<int>(g_eng_nq.pos.active)
                        + static_cast<int>(g_eng_us30.pos.active)
                        + static_cast<int>(g_eng_nas100.pos.active)
                        + static_cast<int>(g_bracket_sp.pos.active)
                        + static_cast<int>(g_bracket_nq.pos.active)
                        + static_cast<int>(g_bracket_us30.pos.active)
                        + static_cast<int>(g_bracket_nas100.pos.active)
                        + static_cast<int>(g_ca_esnq.has_open_position())
                        + static_cast<int>(g_vwap_rev_sp.has_open_position())
                        + static_cast<int>(g_vwap_rev_nq.has_open_position())
                        + static_cast<int>(g_orb_us.has_open_position())
                        + static_cast<int>(g_nbm_sp.has_open_position())
                        + static_cast<int>(g_nbm_nq.has_open_position())
                        + static_cast<int>(g_nbm_nas.has_open_position())
                        + static_cast<int>(g_nbm_us30.has_open_position());
                        // NOTE: g_nbm_gold_london and g_nbm_oil_london are NOT US equity --
                        // they were incorrectly counted here, inflating us_eq heat and
                        // blocking legitimate US equity entries when gold/oil NBM was open.
                        // g_nbm_gold_london is counted in `metals` below.
                        // g_nbm_oil_london  is counted in `oil`    below.
        const int eu_eq = static_cast<int>(g_eng_ger30.pos.active)
                        + static_cast<int>(g_eng_uk100.pos.active)
                        + static_cast<int>(g_eng_estx50.pos.active)
                        + static_cast<int>(g_bracket_ger30.pos.active)
                        + static_cast<int>(g_bracket_uk100.pos.active)
                        + static_cast<int>(g_bracket_estx50.pos.active)
                        + static_cast<int>(g_vwap_rev_ger40.has_open_position())
                        + static_cast<int>(g_orb_ger30.has_open_position())
                        + static_cast<int>(g_orb_uk100.has_open_position())
                        + static_cast<int>(g_orb_estx50.has_open_position());
        const int oil   = static_cast<int>(g_eng_cl.pos.active)
                        + static_cast<int>(g_eng_brent.pos.active)
                        + static_cast<int>(g_bracket_brent.pos.active)
                        + static_cast<int>(g_ca_eia_fade.has_open_position())
                        + static_cast<int>(g_ca_brent_wti.has_open_position())
                        + static_cast<int>(g_nbm_oil_london.has_open_position()); // was incorrectly in us_eq
        const int metals = static_cast<int>(g_gold_stack.has_open_position())
                         + static_cast<int>(g_bracket_gold.pos.active)
                         + static_cast<int>(g_gold_flow.has_open_position())
                         + static_cast<int>(g_gold_flow_reload.has_open_position())
                         + static_cast<int>(g_eng_xag.pos.active)
                         + static_cast<int>(g_bracket_xag.pos.active)
                         + static_cast<int>(g_orb_silver.has_open_position())
                         + static_cast<int>(g_nbm_gold_london.has_open_position()); // was incorrectly in us_eq
        const int jpy   = static_cast<int>(g_eng_usdjpy.pos.active)
                        + static_cast<int>(g_eng_audusd.pos.active)
                        + static_cast<int>(g_eng_nzdusd.pos.active)
                        + static_cast<int>(g_ca_carry_unwind.has_open_position());
        const int eur_gbp = static_cast<int>(g_eng_eurusd.pos.active)
                          + static_cast<int>(g_eng_gbpusd.pos.active)
                          + static_cast<int>(g_bracket_eurusd.pos.active)
                          + static_cast<int>(g_bracket_gbpusd.pos.active)
                          + static_cast<int>(g_vwap_rev_eurusd.has_open_position())
                          + static_cast<int>(g_ca_fx_cascade.has_open_gbpusd());
        g_adaptive_risk.update_cluster_counts(us_eq, eu_eq, oil, metals, jpy, eur_gbp);

        // Portfolio VaR: update dollar-risk estimates per cluster.
        // Dollar risk proxy = cluster_open_count ? risk_per_trade_usd (configured).
        // Uses risk_per_trade_usd if set, else falls back to daily_loss_limit / 4.
        // This gives a conservative estimate of simultaneous loss if all positions
        // move adversely together -- the correlation-adjustment (beta) in PortfolioVaR
        // then weights by DXY sensitivity to catch correlated drawdowns.
        {
            const double rpt = (g_cfg.risk_per_trade_usd > 0.0)
                ? g_cfg.risk_per_trade_usd
                : g_cfg.daily_loss_limit / 4.0;
            g_portfolio_var.update("US_EQUITY", us_eq    * rpt);
            g_portfolio_var.update("EU_EQUITY", eu_eq    * rpt);
            g_portfolio_var.update("OIL",       oil      * rpt);
            g_portfolio_var.update("METALS",    metals   * rpt);
            g_portfolio_var.update("JPY_RISK",  jpy      * rpt);
            g_portfolio_var.update("EUR_GBP",   eur_gbp  * rpt);
        }
    }

    // ?? Correlation matrix -- feed current symbol mid price each tick ????????
    // Each symbol passes through the macro-tick dispatch once per tick.
    // XAUUSD is also fed in the gold tick handler (higher frequency is fine --
    // duplicate feeds are idempotent: EWM converges regardless of update rate).
    if (bid > 0.0 && ask > 0.0)
        g_corr_matrix.on_price(sym, (bid + ask) * 0.5);

    // Session slot -- updated every tick
    {
        const auto t_now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        struct tm ti_now = {};
#ifdef _WIN32
        gmtime_s(&ti_now, &t_now);
#else
        gmtime_r(&t_now, &ti_now);
#endif
        const int h = ti_now.tm_hour;
        if      (h >= 7  && h < 9)  g_macro_ctx.session_slot = 1; // London open
        else if (h >= 9  && h < 12) g_macro_ctx.session_slot = 2; // London core
        else if (h >= 12 && h < 14) g_macro_ctx.session_slot = 3; // Overlap
        else if (h >= 14 && h < 17) g_macro_ctx.session_slot = 4; // NY open
        else if (h >= 17 && h < 22) g_macro_ctx.session_slot = 5; // NY late
        else if (h >= 22 || h < 5)  g_macro_ctx.session_slot = 6; // Asia
        else                         g_macro_ctx.session_slot = 1; // 05-07 UTC: was dead zone, now London pre-open
    }

    // Cross-symbol compression state -- engine is in COMPRESSION or BREAKOUT_WATCH
    // NAS100 and USTEC.F are the same underlying -- either one counts as "nq compressing"
    g_macro_ctx.sp_compressing    = (g_eng_sp.phase     == omega::Phase::COMPRESSION
                                  || g_eng_sp.phase     == omega::Phase::BREAKOUT_WATCH);
    g_macro_ctx.nq_compressing    = (g_eng_nq.phase     == omega::Phase::COMPRESSION
                                  || g_eng_nq.phase     == omega::Phase::BREAKOUT_WATCH
                                  || g_eng_nas100.phase == omega::Phase::COMPRESSION
                                  || g_eng_nas100.phase == omega::Phase::BREAKOUT_WATCH);
    g_macro_ctx.us30_compressing  = (g_eng_us30.phase   == omega::Phase::COMPRESSION
                                  || g_eng_us30.phase   == omega::Phase::BREAKOUT_WATCH);
    g_macro_ctx.ger30_compressing = (g_eng_ger30.phase  == omega::Phase::COMPRESSION
                                  || g_eng_ger30.phase  == omega::Phase::BREAKOUT_WATCH);
    g_macro_ctx.uk100_compressing = (g_eng_uk100.phase  == omega::Phase::COMPRESSION
                                  || g_eng_uk100.phase  == omega::Phase::BREAKOUT_WATCH);

    // ?? L2 imbalance: lock-free atomic reads with freshness guard ????????????????
    // fresh() returns 0.5 (neutral) if no book update arrived in last 5s.
    // This prevents stale/default-initialised imbalance from influencing engine decisions.
    // memory_order_acquire on last_update_ms ensures imbalance/has_data are visible.
    {
        const int64_t l2_now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto rd = [&](const AtomicL2& al) -> double {
            return al.fresh(l2_now_ms) ? al.imbalance.load(std::memory_order_relaxed) : 0.5;
        };
        g_macro_ctx.gold_l2_imbalance   = rd(g_l2_gold);
        g_macro_ctx.sp_l2_imbalance     = rd(g_l2_sp);
        g_macro_ctx.nq_l2_imbalance     = rd(g_l2_nq);
        g_macro_ctx.cl_l2_imbalance     = rd(g_l2_cl);
        g_macro_ctx.xag_l2_imbalance    = rd(g_l2_xag);
        g_macro_ctx.eur_l2_imbalance    = rd(g_l2_eur);
        g_macro_ctx.gbp_l2_imbalance    = rd(g_l2_gbp);
        g_macro_ctx.aud_l2_imbalance    = rd(g_l2_aud);
        g_macro_ctx.nzd_l2_imbalance    = rd(g_l2_nzd);
        g_macro_ctx.jpy_l2_imbalance    = rd(g_l2_jpy);
        g_macro_ctx.ger40_l2_imbalance  = rd(g_l2_ger40);
        g_macro_ctx.uk100_l2_imbalance  = rd(g_l2_uk100);
        g_macro_ctx.estx50_l2_imbalance = rd(g_l2_estx50);
        g_macro_ctx.brent_l2_imbalance  = rd(g_l2_brent);
        g_macro_ctx.nas_l2_imbalance    = rd(g_l2_nas);
        g_macro_ctx.us30_l2_imbalance   = rd(g_l2_us30);
        g_macro_ctx.gold_l2_real        = g_l2_gold.has_data.load(std::memory_order_relaxed)
                                          && g_l2_gold.fresh(l2_now_ms);
    }
    // Microprice bias -- still from cTrader atomics (FIX doesn't compute this)
    g_macro_ctx.gold_microprice_bias = g_l2_gold.microprice_bias.load(std::memory_order_relaxed);
    g_macro_ctx.sp_microprice_bias   = g_l2_sp.microprice_bias.load(std::memory_order_relaxed);
    g_macro_ctx.xag_microprice_bias  = g_l2_xag.microprice_bias.load(std::memory_order_relaxed);
    g_macro_ctx.cl_microprice_bias   = g_l2_cl.microprice_bias.load(std::memory_order_relaxed);
    g_macro_ctx.eur_microprice_bias  = g_l2_eur.microprice_bias.load(std::memory_order_relaxed);
    g_macro_ctx.gbp_microprice_bias  = g_l2_gbp.microprice_bias.load(std::memory_order_relaxed);
    g_macro_ctx.aud_microprice_bias  = g_l2_aud.microprice_bias.load(std::memory_order_relaxed);
    g_macro_ctx.nzd_microprice_bias  = g_l2_nzd.microprice_bias.load(std::memory_order_relaxed);
    g_macro_ctx.jpy_microprice_bias  = g_l2_jpy.microprice_bias.load(std::memory_order_relaxed);
    g_macro_ctx.ger40_microprice_bias= g_l2_ger40.microprice_bias.load(std::memory_order_relaxed);

    // ?? L2 quality flags -- lock-free reads ????????????????????????????????
    g_macro_ctx.ctrader_l2_live = (g_ctrader_depth.depth_events_total.load() > 0);
    // gold_l2_real now set from g_l2_books in the mutex block above

    // ?? Midnight log rotation -- every tick check, guaranteed rotation ???????????
    // force_rotate_check runs every 60s in diagnostic loop but if stdout is
    // quiet it may not fire. Check every tick so rotation is never missed.
    if (g_tee_buf) {
        static int64_t s_last_rotate_check = 0;
        const int64_t now_rot = nowSec();
        if (now_rot - s_last_rotate_check >= 5) {  // check every 5s max
            s_last_rotate_check = now_rot;
            g_tee_buf->force_rotate_check();
        }
    }

    // ?? Periodic bar indicator auto-save every 10 minutes ????????????????
    // Prevents cold-start on crash/kill: indicators saved to .dat every 10min
    // so a restart within 12h loads them instantly (m1_ready=true immediately).
    // Previously only saved at midnight + shutdown -- a crash between saves
    // meant cold start, m1_ready=false, GoldFlow blocked for 15min+ every restart.
    {
        static int64_t s_last_bar_save = 0;
        const int64_t now_bs = nowSec();
        if (now_bs - s_last_bar_save >= 60) {  // every 60s -- ensures warm restart even after short session
            s_last_bar_save = now_bs;
            if (g_bars_gold.m1.ind.m1_ready.load(std::memory_order_relaxed)) {
                const std::string base = log_root_dir();
                g_bars_gold.m1 .save_indicators(base + "/bars_gold_m1.dat");
                g_bars_gold.m5 .save_indicators(base + "/bars_gold_m5.dat");
                g_bars_gold.m15.save_indicators(base + "/bars_gold_m15.dat");
                g_bars_gold.h4 .save_indicators(base + "/bars_gold_h4.dat");
                printf("[BAR-SAVE] Periodic save -- bars_gold_m1/m5/m15/h4.dat updated\n");
                fflush(stdout);
            }
        }
    }

    // ?? L2 size-dead watchdog -- warn at 5s, force-restart at 10s ??????????????
    // cTrader sends depth events (prices arrive) but size=0 on all levels.
    // has_data()=false ? GoldFlow blind, no L2 signal. Unacceptable.
    // 5s dead = warn every tick. 10s dead = force stop/start depth feed.
    {
        // XAUUSD gets zero cTrader depth events on this account -- broker doesn't send them.
        // L2 imbalance comes from the FIX feed instead (g_l2_books via atomic writes).
        // No restart logic needed -- just log once if truly dead after 60s.
        const bool gold_size_dead = g_macro_ctx.ctrader_l2_live
                                    && !g_macro_ctx.gold_l2_real;
        if (gold_size_dead) {
            static int64_t s_warn_once = 0;
            const int64_t now_wd = nowSec();
            if (s_warn_once == 0) s_warn_once = now_wd;
            if (now_wd - s_warn_once >= 60) {
                static bool s_warned = false;
                if (!s_warned) {
                    printf("[L2-INFO] XAUUSD cTrader depth events=0 -- using FIX L2 (normal for this account)\n");
                    fflush(stdout);
                    s_warned = true;
                }
            }
        }
    }

    // Log L2 status once per minute
    {
        static int64_t s_l2_log = 0;
        const int64_t now_l2 = nowSec();
        if (now_l2 - s_l2_log >= 60) {
            s_l2_log = now_l2;
            printf("[L2-STATUS] ctrader_live=%d events=%llu gold_real=%d gold_imb=%.3f "
                   "gold_mp=%.3f sp_real=%d cl_real=%d\n",
                   (int)g_macro_ctx.ctrader_l2_live,
                   (unsigned long long)g_ctrader_depth.depth_events_total.load(),
                   (int)g_macro_ctx.gold_l2_real,
                   g_macro_ctx.gold_l2_imbalance,
                   g_macro_ctx.gold_microprice_bias,
                   (int)g_macro_ctx.sp_l2_real,
                   (int)g_macro_ctx.cl_l2_real);
            fflush(stdout);
        }
    }

    // ?? Cold path: snapshot all needed books under ONE lock ?????????????????
    // Previously called getBook() per-symbol with a lock per call -- 12 locks/tick.
    // Now: one lock, copy all books, release, process outside -- zero contention window.
    // Only walls/vacuums/book_slope/GUI push need the full book; microprice_bias
    // already comes from atomics above.
    struct ColdSnap { L2Book book; bool valid = false; };
    std::unordered_map<std::string, ColdSnap> cold_snap;
    {
        static constexpr const char* COLD_SYMS[] = {
            "XAUUSD","US500.F","XAGUSD","USOIL.F","EURUSD","GBPUSD",
            "AUDUSD","NZDUSD","USDJPY","GER40","UK100","ESTX50","BRENT"
        };
        std::lock_guard<std::mutex> lk(g_l2_mtx);
        for (const char* cold_sym : COLD_SYMS) {
            auto it = g_l2_books.find(cold_sym);
            // Use bid_count OR ask_count > 0 -- cTrader sends incremental one-sided
            // updates so ask_count may be 0 when bids arrive first and vice versa.
            // has_data() requires BOTH sides which causes empty book on startup.
            if (it != g_l2_books.end() &&
                (it->second.bid_count > 0 || it->second.ask_count > 0))
                cold_snap[cold_sym] = {it->second, true};
        }
    }
    auto getBook = [&](const std::string& s) -> const L2Book* {
        auto it = cold_snap.find(s);
        return (it != cold_snap.end() && it->second.valid) ? &it->second.book : nullptr;
    };
        // Push L2 book levels to telemetry for GUI depth panel.
        // Always push exactly 5 levels -- pad with price=0/size=0 if book has fewer.
        // This prevents the JS depth panel from showing/hiding rows as cTrader sends
        // partial incremental updates (bid_count fluctuates 1-5 between ticks).
        // Zero-size levels render as invisible rows (zero-width bar, empty size) -- stable.
        auto pushL2 = [&](const char* sym, const L2Book* b) {
            if (!b) return;
            double bp[5]{}, bs[5]{}, ap[5]{}, as_[5]{};
            const int nb = std::min(b->bid_count, 5);
            const int na = std::min(b->ask_count, 5);
            for (int i=0;i<nb;++i){bp[i]=b->bids[i].price;bs[i]=b->bids[i].size;}
            for (int i=0;i<na;++i){ap[i]=b->asks[i].price;as_[i]=b->asks[i].size;}
            g_telemetry.UpdateL2Book(sym, bp, bs, nb, ap, as_, na);
        };
        if (const L2Book* b = getBook("XAUUSD")) {
            g_macro_ctx.gold_book_slope      = b->book_slope();
            g_macro_ctx.gold_vacuum_ask      = b->liquidity_vacuum_ask();
            g_macro_ctx.gold_vacuum_bid      = b->liquidity_vacuum_bid();
            g_macro_ctx.gold_wall_above      = b->wall_above(g_macro_ctx.gold_mid_price);
            g_macro_ctx.gold_wall_below      = b->wall_below(g_macro_ctx.gold_mid_price);
            pushL2("XAUUSD", b);
        }
        if (const L2Book* b = getBook("US500.F")) {
            g_macro_ctx.sp_book_slope        = b->book_slope();
            g_macro_ctx.sp_vacuum_ask        = b->liquidity_vacuum_ask();
            g_macro_ctx.sp_vacuum_bid        = b->liquidity_vacuum_bid();
            g_macro_ctx.sp_wall_above        = b->wall_above(b->bid_count > 0 ? b->bids[0].price : 0.0);
            g_macro_ctx.sp_wall_below        = b->wall_below(b->ask_count > 0 ? b->asks[0].price : 0.0);
            pushL2("US500.F", b);
        }
        if (const L2Book* b = getBook("XAGUSD"))  { pushL2("XAGUSD", b); }
        if (const L2Book* b = getBook("USOIL.F")) {
            g_macro_ctx.cl_vacuum_ask        = b->liquidity_vacuum_ask();
            g_macro_ctx.cl_vacuum_bid        = b->liquidity_vacuum_bid();
        }
        // ?? FX pairs -- Priority 6 backlog now complete ????????????????????????????????
        // These previously used L2 imbalance < 0.30 as a vacuum proxy.
        // Now populated from real book data for full L2 scoring parity with GOLD/SP.
        if (const L2Book* b = getBook("EURUSD")) {
            g_macro_ctx.eur_vacuum_ask      = b->liquidity_vacuum_ask();
            g_macro_ctx.eur_vacuum_bid      = b->liquidity_vacuum_bid();
            g_macro_ctx.eur_wall_above      = b->wall_above(g_macro_ctx.eur_mid_price);
            g_macro_ctx.eur_wall_below      = b->wall_below(g_macro_ctx.eur_mid_price);
            pushL2("EURUSD", b);
        }
        if (const L2Book* b = getBook("GBPUSD")) {
            g_macro_ctx.gbp_vacuum_ask      = b->liquidity_vacuum_ask();
            g_macro_ctx.gbp_vacuum_bid      = b->liquidity_vacuum_bid();
            g_macro_ctx.gbp_wall_above      = b->wall_above(g_macro_ctx.gbp_mid_price);
            g_macro_ctx.gbp_wall_below      = b->wall_below(g_macro_ctx.gbp_mid_price);
        }
        if (const L2Book* b = getBook("AUDUSD")) {
            g_macro_ctx.aud_vacuum_ask      = b->liquidity_vacuum_ask();
            g_macro_ctx.aud_vacuum_bid      = b->liquidity_vacuum_bid();
        }
        if (const L2Book* b = getBook("NZDUSD")) {
            g_macro_ctx.nzd_vacuum_ask      = b->liquidity_vacuum_ask();
            g_macro_ctx.nzd_vacuum_bid      = b->liquidity_vacuum_bid();
        }
        if (const L2Book* b = getBook("USDJPY")) {
            g_macro_ctx.jpy_vacuum_ask      = b->liquidity_vacuum_ask();
            g_macro_ctx.jpy_vacuum_bid      = b->liquidity_vacuum_bid();
        }
        // EU equity vacuum (for bracket L2 gate)
        if (const L2Book* b = getBook("GER40")) {
            g_macro_ctx.ger40_vacuum_ask      = b->liquidity_vacuum_ask();
            g_macro_ctx.ger40_vacuum_bid      = b->liquidity_vacuum_bid();
            g_macro_ctx.ger40_wall_above      = b->wall_above(
                b->bid_count > 0 ? b->bids[0].price : 0.0);
            g_macro_ctx.ger40_wall_below      = b->wall_below(
                b->ask_count > 0 ? b->asks[0].price : 0.0);
        }
        if (const L2Book* b = getBook("UK100")) {
            g_macro_ctx.uk100_vacuum_ask = b->liquidity_vacuum_ask();
            g_macro_ctx.uk100_vacuum_bid = b->liquidity_vacuum_bid();
        }
        if (const L2Book* b = getBook("ESTX50")) {
            g_macro_ctx.estx50_vacuum_ask = b->liquidity_vacuum_ask();
            g_macro_ctx.estx50_vacuum_bid = b->liquidity_vacuum_bid();
        }

    // ?? CVD direction ? MacroContext ??????????????????????????????????????????
    // Push CVD direction and divergence flags into MacroContext so all engines
    // can use them as entry confirmation without accessing g_edges directly.
    {
        auto upd_cvd = [&](int& dir, bool& bull, bool& bear, const char* s) {
            const omega::edges::CVDState cs = g_edges.cvd.get(s);
            dir  = cs.direction();
            bull = cs.bullish_divergence();
            bear = cs.bearish_divergence();
        };
        bool dummy_b = false, dummy_b2 = false;
        upd_cvd(g_macro_ctx.gold_cvd_dir,   g_macro_ctx.gold_cvd_bull_div, g_macro_ctx.gold_cvd_bear_div, "XAUUSD");
        upd_cvd(g_macro_ctx.sp_cvd_dir,     g_macro_ctx.sp_cvd_bull_div,   g_macro_ctx.sp_cvd_bear_div,   "US500.F");
        upd_cvd(g_macro_ctx.nq_cvd_dir,     dummy_b,  dummy_b2,  "USTEC.F");
        upd_cvd(g_macro_ctx.eurusd_cvd_dir, dummy_b,  dummy_b2,  "EURUSD");
        upd_cvd(g_macro_ctx.usdjpy_cvd_dir, dummy_b,  dummy_b2,  "USDJPY");
        upd_cvd(g_macro_ctx.xagusd_cvd_dir, dummy_b,  dummy_b2,  "XAGUSD");
    }

    // Push L2 imbalance snapshot to telemetry
    g_telemetry.UpdateL2(
        g_macro_ctx.sp_l2_imbalance,  g_macro_ctx.nq_l2_imbalance,
        g_macro_ctx.us30_l2_imbalance,g_macro_ctx.nas_l2_imbalance,
        g_macro_ctx.cl_l2_imbalance,  g_macro_ctx.brent_l2_imbalance,
        g_macro_ctx.gold_l2_imbalance,g_macro_ctx.xag_l2_imbalance,
        g_macro_ctx.ger40_l2_imbalance,g_macro_ctx.uk100_l2_imbalance,
        g_macro_ctx.estx50_l2_imbalance,
        g_macro_ctx.eur_l2_imbalance, g_macro_ctx.gbp_l2_imbalance,
        g_macro_ctx.aud_l2_imbalance, g_macro_ctx.nzd_l2_imbalance,
        g_macro_ctx.jpy_l2_imbalance,
        [&]() -> int {
            // L2 active: TCP connected AND depth events received within last 30s.
            // Without the age check, a silent feed stall (connection alive, broker
            // stopped sending quotes -- common in thin Asian session) shows the badge
            // green while all imbalance values are frozen stale data.
            if (!g_ctrader_depth.depth_active.load()) return 0;
            const int64_t last_ev = g_ctrader_depth.last_depth_event_ms.load();
            if (last_ev == 0) return 0;  // connected but no events yet
            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            return ((now_ms - last_ev) < 30000) ? 1 : 0;  // stale after 30s
        }());

    const bool tradeable = session_tradeable();
    g_telemetry.UpdateSession(tradeable ? "ACTIVE" : "CLOSED", tradeable ? 1 : 0);

    // Base gate flags -- passed into dispatch, checked before entry (not before warmup)
    // Use p95 RTT (not last) -- a single spike in g_rtt_last was permanently blocking
    // entries until the next 5s ping. p95 over 200 samples is stable and representative.
    const double rtt_check = (g_rtt_p95 > 0.0) ? g_rtt_p95 : g_rtt_last;
    const bool lat_ok = (rtt_check <= 0.0 || g_governor.checkLatency(rtt_check, g_cfg.max_latency_ms));
    if (!lat_ok) ++g_gov_lat;
    // Spread governor counter -- incremented only when bracket_spread_blocked fires
    // (i.e. a real signal was ready but spread was too wide). Not per-tick noise.
    // See bracket_spread_blocked lambda below which calls ++g_gov_spread directly.

    // ?? Open unrealised P&L accumulator ?????????????????????????????????????
    // Sums floating P&L across ALL open positions each tick.
    // Used by symbol_gate to enforce daily_loss_limit on combined
    // closed + unrealised loss -- prevents limit breach before any close fires.
    // open_unrealised_pnl -- converted to static function above on_tick

    // ?? Push unrealised P&L to global atomic every 250ms ????????????????????
    // This feeds the GUI daily_pnl display (closed + floating) and persists
    // across the tick so handle_closed_trade can read it without recomputing.
    {
        static thread_local int64_t s_last_unr_push = 0;
        const int64_t now_ms_unr = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (now_ms_unr - s_last_unr_push >= 250) {
            s_last_unr_push = now_ms_unr;
            const double unr = open_unrealised_pnl();
            g_open_unrealised_cents.store(static_cast<int64_t>(unr * 100.0));
            // Push combined (closed + unrealised) to GUI so it shows live floating P&L
            const double closed = g_omegaLedger.dailyPnl();
            g_telemetry.UpdateStats(
                closed + unr,
                g_omegaLedger.grossDailyPnl() + unr,
                g_omegaLedger.maxDD(),
                g_omegaLedger.total(), g_omegaLedger.wins(), g_omegaLedger.losses(),
                g_omegaLedger.winRate(), g_omegaLedger.avgWin(), g_omegaLedger.avgLoss(), 0, 0,
                closed, unr);

            // ?? Per-trade live P&L update ?????????????????????????????????????
            // Rebuilds live_trades[] from scratch every 250ms.
            // Each open position contributes one entry with current floating P&L.
            g_telemetry.ClearLiveTrades();
            // Per-trade live P&L -- push_live_trade(sym, eng, is_long, entry, tp, sl, size, ts)
            // ?? Gold engines ????????????????????????????????????????????????
            if (g_gold_flow.pos.active) {
                // TP for GoldFlow = next stage trigger price (there is no fixed TP --
                // the engine trails. The next stage price is the meaningful target).
                // Stage triggers: 1x/2x/8x/15x ATR profit from entry.
                static constexpr double GF_STAGE_MULTS[] = {
                    GFE_BE_ATR_MULT,     // stage 0 ? 1: 1x ATR
                    GFE_STAGE2_ATR_MULT, // stage 1 ? 2: 2x ATR
                    GFE_STAGE3_ATR_MULT, // stage 2 ? 3: 8x ATR
                    GFE_STAGE4_ATR_MULT, // stage 3 ? 4: 15x ATR
                };
                const int   cur_stage = g_gold_flow.pos.trail_stage;
                const double atr_e    = g_gold_flow.pos.atr_at_entry;
                double gf_tp = 0.0;
                if (atr_e > 0.0 && cur_stage < 4) {
                    const double next_mult = GF_STAGE_MULTS[cur_stage];
                    gf_tp = g_gold_flow.pos.is_long
                        ? g_gold_flow.pos.entry + atr_e * next_mult
                        : g_gold_flow.pos.entry - atr_e * next_mult;
                }
                push_live_trade("XAUUSD", "GoldFlow",
                    g_gold_flow.pos.is_long, g_gold_flow.pos.entry,
                    gf_tp, g_gold_flow.pos.sl,
                    g_gold_flow.pos.size, g_gold_flow.pos.entry_ts);
            }
            if (g_gold_stack.has_open_position())
                push_live_trade("XAUUSD", g_gold_stack.live_engine(),
                    g_gold_stack.live_is_long(), g_gold_stack.live_entry(),
                    g_gold_stack.live_tp(),       g_gold_stack.live_sl(),
                    g_gold_stack.live_size(),     (int64_t)std::time(nullptr));
            if (g_bracket_gold.pos.active)
                push_live_trade("XAUUSD","Bracket",
                    g_bracket_gold.pos.is_long, g_bracket_gold.pos.entry,
                    g_bracket_gold.pos.tp,      g_bracket_gold.pos.sl,
                    g_bracket_gold.pos.size,    g_bracket_gold.pos.entry_ts);
            if (g_trend_pb_gold.has_open_position())
                push_live_trade("XAUUSD","TrendPB",
                    g_trend_pb_gold.open_is_long(), g_trend_pb_gold.open_entry(),
                    0.0, g_trend_pb_gold.open_sl(),
                    g_trend_pb_gold.open_size(), (int64_t)std::time(nullptr));
            if (g_nbm_gold_london.has_open_position())
                push_live_trade("XAUUSD","NBM-London",
                    g_nbm_gold_london.open_is_long(), g_nbm_gold_london.open_entry(),
                    0.0, 0.0, g_nbm_gold_london.open_size(), (int64_t)std::time(nullptr));
            // ?? US indices ??????????????????????????????????????????????????
            if (g_eng_sp.pos.active)
                push_live_trade("US500.F","BE", g_eng_sp.pos.is_long,
                    g_eng_sp.pos.entry, g_eng_sp.pos.tp, g_eng_sp.pos.sl,
                    g_eng_sp.pos.size, g_eng_sp.pos.entry_ts);
            if (g_eng_nq.pos.active)
                push_live_trade("USTEC.F","BE", g_eng_nq.pos.is_long,
                    g_eng_nq.pos.entry, g_eng_nq.pos.tp, g_eng_nq.pos.sl,
                    g_eng_nq.pos.size, g_eng_nq.pos.entry_ts);
            if (g_eng_us30.pos.active)
                push_live_trade("DJ30.F","BE", g_eng_us30.pos.is_long,
                    g_eng_us30.pos.entry, g_eng_us30.pos.tp, g_eng_us30.pos.sl,
                    g_eng_us30.pos.size, g_eng_us30.pos.entry_ts);
            if (g_eng_nas100.pos.active)
                push_live_trade("NAS100","BE", g_eng_nas100.pos.is_long,
                    g_eng_nas100.pos.entry, g_eng_nas100.pos.tp, g_eng_nas100.pos.sl,
                    g_eng_nas100.pos.size, g_eng_nas100.pos.entry_ts);
            if (g_bracket_sp.pos.active)
                push_live_trade("US500.F","Bracket", g_bracket_sp.pos.is_long,
                    g_bracket_sp.pos.entry, g_bracket_sp.pos.tp, g_bracket_sp.pos.sl,
                    g_bracket_sp.pos.size, g_bracket_sp.pos.entry_ts);
            if (g_bracket_nq.pos.active)
                push_live_trade("USTEC.F","Bracket", g_bracket_nq.pos.is_long,
                    g_bracket_nq.pos.entry, g_bracket_nq.pos.tp, g_bracket_nq.pos.sl,
                    g_bracket_nq.pos.size, g_bracket_nq.pos.entry_ts);
            if (g_bracket_us30.pos.active)
                push_live_trade("DJ30.F","Bracket", g_bracket_us30.pos.is_long,
                    g_bracket_us30.pos.entry, g_bracket_us30.pos.tp, g_bracket_us30.pos.sl,
                    g_bracket_us30.pos.size, g_bracket_us30.pos.entry_ts);
            if (g_bracket_nas100.pos.active)
                push_live_trade("NAS100","Bracket", g_bracket_nas100.pos.is_long,
                    g_bracket_nas100.pos.entry, g_bracket_nas100.pos.tp, g_bracket_nas100.pos.sl,
                    g_bracket_nas100.pos.size, g_bracket_nas100.pos.entry_ts);
            if (g_nbm_sp.has_open_position())
                push_live_trade("US500.F","NBM", g_nbm_sp.open_is_long(),
                    g_nbm_sp.open_entry(), 0.0, 0.0, g_nbm_sp.open_size(), (int64_t)std::time(nullptr));
            if (g_nbm_nq.has_open_position())
                push_live_trade("USTEC.F","NBM", g_nbm_nq.open_is_long(),
                    g_nbm_nq.open_entry(), 0.0, 0.0, g_nbm_nq.open_size(), (int64_t)std::time(nullptr));
            if (g_nbm_nas.has_open_position())
                push_live_trade("NAS100","NBM", g_nbm_nas.open_is_long(),
                    g_nbm_nas.open_entry(), 0.0, 0.0, g_nbm_nas.open_size(), (int64_t)std::time(nullptr));
            if (g_nbm_us30.has_open_position())
                push_live_trade("DJ30.F","NBM", g_nbm_us30.open_is_long(),
                    g_nbm_us30.open_entry(), 0.0, 0.0, g_nbm_us30.open_size(), (int64_t)std::time(nullptr));
            // ?? EU indices ??????????????????????????????????????????????????
            if (g_eng_ger30.pos.active)
                push_live_trade("GER40","BE", g_eng_ger30.pos.is_long,
                    g_eng_ger30.pos.entry, g_eng_ger30.pos.tp, g_eng_ger30.pos.sl,
                    g_eng_ger30.pos.size, g_eng_ger30.pos.entry_ts);
            if (g_eng_uk100.pos.active)
                push_live_trade("UK100","BE", g_eng_uk100.pos.is_long,
                    g_eng_uk100.pos.entry, g_eng_uk100.pos.tp, g_eng_uk100.pos.sl,
                    g_eng_uk100.pos.size, g_eng_uk100.pos.entry_ts);
            if (g_eng_estx50.pos.active)
                push_live_trade("ESTX50","BE", g_eng_estx50.pos.is_long,
                    g_eng_estx50.pos.entry, g_eng_estx50.pos.tp, g_eng_estx50.pos.sl,
                    g_eng_estx50.pos.size, g_eng_estx50.pos.entry_ts);
            if (g_bracket_ger30.pos.active)
                push_live_trade("GER40","Bracket", g_bracket_ger30.pos.is_long,
                    g_bracket_ger30.pos.entry, g_bracket_ger30.pos.tp, g_bracket_ger30.pos.sl,
                    g_bracket_ger30.pos.size, g_bracket_ger30.pos.entry_ts);
            if (g_bracket_uk100.pos.active)
                push_live_trade("UK100","Bracket", g_bracket_uk100.pos.is_long,
                    g_bracket_uk100.pos.entry, g_bracket_uk100.pos.tp, g_bracket_uk100.pos.sl,
                    g_bracket_uk100.pos.size, g_bracket_uk100.pos.entry_ts);
            if (g_bracket_estx50.pos.active)
                push_live_trade("ESTX50","Bracket", g_bracket_estx50.pos.is_long,
                    g_bracket_estx50.pos.entry, g_bracket_estx50.pos.tp, g_bracket_estx50.pos.sl,
                    g_bracket_estx50.pos.size, g_bracket_estx50.pos.entry_ts);
            if (g_trend_pb_ger40.has_open_position())
                push_live_trade("GER40","TrendPB", g_trend_pb_ger40.open_is_long(),
                    g_trend_pb_ger40.open_entry(), 0.0, g_trend_pb_ger40.open_sl(),
                    g_trend_pb_ger40.open_size(), (int64_t)std::time(nullptr));
            // ?? Oil/commodities ?????????????????????????????????????????????
            if (g_eng_cl.pos.active)
                push_live_trade("USOIL.F","BE", g_eng_cl.pos.is_long,
                    g_eng_cl.pos.entry, g_eng_cl.pos.tp, g_eng_cl.pos.sl,
                    g_eng_cl.pos.size, g_eng_cl.pos.entry_ts);
            if (g_eng_brent.pos.active)
                push_live_trade("BRENT","BE", g_eng_brent.pos.is_long,
                    g_eng_brent.pos.entry, g_eng_brent.pos.tp, g_eng_brent.pos.sl,
                    g_eng_brent.pos.size, g_eng_brent.pos.entry_ts);
            if (g_bracket_brent.pos.active)
                push_live_trade("BRENT","Bracket", g_bracket_brent.pos.is_long,
                    g_bracket_brent.pos.entry, g_bracket_brent.pos.tp, g_bracket_brent.pos.sl,
                    g_bracket_brent.pos.size, g_bracket_brent.pos.entry_ts);
            if (g_eng_xag.pos.active)
                push_live_trade("XAGUSD","BE", g_eng_xag.pos.is_long,
                    g_eng_xag.pos.entry, g_eng_xag.pos.tp, g_eng_xag.pos.sl,
                    g_eng_xag.pos.size, g_eng_xag.pos.entry_ts);
            if (g_nbm_oil_london.has_open_position())
                push_live_trade("USOIL.F","NBM-London", g_nbm_oil_london.open_is_long(),
                    g_nbm_oil_london.open_entry(), 0.0, 0.0, g_nbm_oil_london.open_size(), (int64_t)std::time(nullptr));
            // ?? FX ??????????????????????????????????????????????????????????
            if (g_eng_eurusd.pos.active)
                push_live_trade("EURUSD","BE", g_eng_eurusd.pos.is_long,
                    g_eng_eurusd.pos.entry, g_eng_eurusd.pos.tp, g_eng_eurusd.pos.sl,
                    g_eng_eurusd.pos.size, g_eng_eurusd.pos.entry_ts);
            if (g_eng_gbpusd.pos.active)
                push_live_trade("GBPUSD","BE", g_eng_gbpusd.pos.is_long,
                    g_eng_gbpusd.pos.entry, g_eng_gbpusd.pos.tp, g_eng_gbpusd.pos.sl,
                    g_eng_gbpusd.pos.size, g_eng_gbpusd.pos.entry_ts);
            if (g_eng_audusd.pos.active)
                push_live_trade("AUDUSD","BE", g_eng_audusd.pos.is_long,
                    g_eng_audusd.pos.entry, g_eng_audusd.pos.tp, g_eng_audusd.pos.sl,
                    g_eng_audusd.pos.size, g_eng_audusd.pos.entry_ts);
            if (g_eng_nzdusd.pos.active)
                push_live_trade("NZDUSD","BE", g_eng_nzdusd.pos.is_long,
                    g_eng_nzdusd.pos.entry, g_eng_nzdusd.pos.tp, g_eng_nzdusd.pos.sl,
                    g_eng_nzdusd.pos.size, g_eng_nzdusd.pos.entry_ts);
            if (g_eng_usdjpy.pos.active)
                push_live_trade("USDJPY","BE", g_eng_usdjpy.pos.is_long,
                    g_eng_usdjpy.pos.entry, g_eng_usdjpy.pos.tp, g_eng_usdjpy.pos.sl,
                    g_eng_usdjpy.pos.size, g_eng_usdjpy.pos.entry_ts);
            if (g_bracket_eurusd.pos.active)
                push_live_trade("EURUSD","Bracket", g_bracket_eurusd.pos.is_long,
                    g_bracket_eurusd.pos.entry, g_bracket_eurusd.pos.tp, g_bracket_eurusd.pos.sl,
                    g_bracket_eurusd.pos.size, g_bracket_eurusd.pos.entry_ts);
            if (g_bracket_gbpusd.pos.active)
                push_live_trade("GBPUSD","Bracket", g_bracket_gbpusd.pos.is_long,
                    g_bracket_gbpusd.pos.entry, g_bracket_gbpusd.pos.tp, g_bracket_gbpusd.pos.sl,
                    g_bracket_gbpusd.pos.size, g_bracket_gbpusd.pos.entry_ts);
            if (g_bracket_audusd.pos.active)
                push_live_trade("AUDUSD","Bracket", g_bracket_audusd.pos.is_long,
                    g_bracket_audusd.pos.entry, g_bracket_audusd.pos.tp, g_bracket_audusd.pos.sl,
                    g_bracket_audusd.pos.size, g_bracket_audusd.pos.entry_ts);
            if (g_bracket_nzdusd.pos.active)
                push_live_trade("NZDUSD","Bracket", g_bracket_nzdusd.pos.is_long,
                    g_bracket_nzdusd.pos.entry, g_bracket_nzdusd.pos.tp, g_bracket_nzdusd.pos.sl,
                    g_bracket_nzdusd.pos.size, g_bracket_nzdusd.pos.entry_ts);
            if (g_bracket_usdjpy.pos.active)
                push_live_trade("USDJPY","Bracket", g_bracket_usdjpy.pos.is_long,
                    g_bracket_usdjpy.pos.entry, g_bracket_usdjpy.pos.tp, g_bracket_usdjpy.pos.sl,
                    g_bracket_usdjpy.pos.size, g_bracket_usdjpy.pos.entry_ts);
            if (g_ca_fx_cascade.has_open_position())
                push_live_trade("GBPUSD","FxCascade", g_ca_fx_cascade.open_is_long(),
                    g_ca_fx_cascade.open_entry(), 0.0, 0.0, g_ca_fx_cascade.open_size(), (int64_t)std::time(nullptr));
            if (g_ca_carry_unwind.has_open_position())
                push_live_trade("USDJPY","CarryUnw", g_ca_carry_unwind.open_is_long(),
                    g_ca_carry_unwind.open_entry(), 0.0, 0.0, g_ca_carry_unwind.open_size(), (int64_t)std::time(nullptr));
        }
    }

    // symbol_risk_blocked -- converted to static function (see above on_tick)

    // symbol_gate -- converted to static function (see above on_tick)

    // on_close -- called by BreakoutEngine (CRTP) when a position closes.
    // BreakoutEngine positions are closed by the BROKER via SL/TP orders submitted
    // at entry. The broker sends an ExecutionReport fill -- on_close is called from
    // the fill handler. No market close order needed here; broker already closed it.
    //
    // Cross-asset engines (EsNqDiv, EIAFade, BrentWTI, FxCascade, CarryUnwind, ORB)
    // manage TP/SL in software -- they enter with a market order but place no broker
    // TP/SL orders. When they close, a market close order IS needed.
    // Those engines receive their own dedicated callback below (ca_on_close).
    auto on_close = [&](const omega::TradeRecord& tr) {
        handle_closed_trade(tr);
    };

    // ca_on_close -- for cross-asset and ORB engines. These manage TP/SL in software
    // with no broker-side orders, so closing requires an explicit market order.
    // ca_on_close -- converted to static function above on_tick
    // bracket_on_close -- used exclusively by g_bracket_gold and g_bracket_xag.
    // When the engine closes a position (TP/SL/timeout/force), it calls this with
    // the filled TradeRecord. We:
    //   1. Record/ledger the trade (same as on_close)
    //   2. Send a closing market order (opposite side, same size) in LIVE mode.
    //
    // The bracket engines manage TP/SL/trailing purely in software -- we do NOT
    // submit bracket orders to BlackBull. When the engine decides to exit, we
    // fire a market order here to close the position at the broker.
    //
    // tr.side is the ENTRY side ("LONG"/"SHORT"); to close we flip it.
    // tr.size is the lot size originally submitted at entry.
    // bracket_on_close -- converted to static function above on_tick
    // ?? Global ranking: collect candidates across all symbols ????????????????
    // on_tick fires once per symbol per tick -- not all symbols in one call.
    // We use a static buffer with a time window: collect all candidates that
    // fire within RANKING_WINDOW_MS of each other, then pick the best one.
    // Any candidate older than the window is flushed before adding new ones.
    // ?? Global ranking state ??????????????????????????????????????????????????
    static omega::RankingConfig g_ranking_cfg;
    g_ranking_cfg.max_trades_per_cycle = g_cfg.max_trades_per_cycle;
    static std::vector<omega::TradeCandidate> g_cycle_candidates;
    static int64_t g_cycle_window_start_ms = 0;
    constexpr int64_t RANKING_WINDOW_MS = 500;

    // ?? Supervisor helper -- run supervisor for a given symbol/engine/supervisor ?
    // Returns the decision. Also always ticks the engine for position management
    // regardless of whether new entries are allowed.
    // sup_decision -- converted to static function above on_tick
    // ?? cost_ok() -- mandatory gate for all direct send_live_order calls ?????????
    // Defined BEFORE dispatch/dispatch_bracket (generic lambdas) so MSVC can
    // resolve it at template definition time, not just instantiation time.
    auto cost_ok = [&](const char* csym, double sl_abs, double lot) -> bool {
        const double tp_dist = sl_abs * 1.5;  // conservative TP estimate at 1.5R
        if (!ExecutionCostGuard::is_viable(csym, ask - bid, tp_dist, lot)) {
            g_telemetry.IncrCostBlocked();
            std::cout << "[COST-BLOCKED] " << csym
                      << " spread=" << std::fixed << std::setprecision(5) << (ask - bid)
                      << " tp_dist=" << tp_dist
                      << " lot=" << lot << "\n";
            std::cout.flush();
            return false;
        }
        return true;
    };

    // ?? dispatch -- breakout engine + supervisor gated ?????????????????????????
    // Calls supervisor, gates new entries on allow_breakout, always ticks for
    // position management. Feeds valid signals into global ranking buffer.
    // Accepts optional pre-computed sdec to avoid double-calling sup.update()
    // when dispatch and dispatch_bracket are both called for the same symbol.
    auto dispatch = [&](auto& eng, omega::SymbolSupervisor& sup, bool base_can_enter,
                        const omega::SupervisorDecision* precomputed_sdec = nullptr) {
        // Supervisor always runs every tick -- for regime classification and telemetry.
        // If a pre-computed decision is provided, reuse it -- don't call update() again.
        const auto sdec = precomputed_sdec ? *precomputed_sdec
                                           : sup_decision(sup, eng, base_can_enter, sym, bid, ask);

        // ?? can_enter construction ????????????????????????????????????????????
        // FLAT:            supervisor must allow + base gates must pass
        // COMPRESSION:     supervisor must allow + base gates must pass
        //                  (BreakoutEngine Leak2 fix aborts to FLAT if can_enter=false)
        // BREAKOUT_WATCH:  ARMED -- bypass supervisor, only base gates apply.
        //                  Once armed at breakout-watch stage a supervisor flip must
        //                  not kill it mid-execution. Final re-check below is last defence.
        const bool eng_armed = (eng.phase == omega::Phase::BREAKOUT_WATCH);
        const bool can_enter = eng_armed
            ? base_can_enter
            : (base_can_enter && sdec.allow_breakout);

        if (eng_armed && !sdec.allow_breakout && base_can_enter) {
            static int64_t s_last_armed_log = 0;
            const int64_t now_log = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (now_log - s_last_armed_log >= 5) {
                s_last_armed_log = now_log;
                std::cout << "[ARMED-BYPASS] " << eng.symbol
                          << " supervisor allow=0 ignored -- engine in BREAKOUT_WATCH\n";
                std::cout.flush();
            }
        }
        // Session-slot scaling on MIN_BREAKOUT_PCT -- only when idle (FLAT), not mid-setup
        const bool eng_mid_cycle = (eng.phase == omega::Phase::COMPRESSION
                                 || eng.phase == omega::Phase::BREAKOUT_WATCH);
        if (!eng_mid_cycle) {
            static std::unordered_map<std::string, double> s_base_breakout;
            const std::string sym_key = eng.symbol;
            auto it = s_base_breakout.find(sym_key);
            if (it == s_base_breakout.end()) {
                s_base_breakout[sym_key] = eng.MIN_BREAKOUT_PCT;
                it = s_base_breakout.find(sym_key);
            }
            const double base = it->second;
            const double mult = omega::session_breakout_mult(g_macro_ctx.session_slot);
            eng.MIN_BREAKOUT_PCT = base * std::max(0.70, std::min(1.30, mult));
        }
        // Pass supervisor regime name (EXPANSION_BREAKOUT/QUIET_COMPRESSION etc) not macro
        // regime (RISK_ON/NEUTRAL). The pyramid logic in BreakoutEngine checks for
        // EXPANSION_BREAKOUT/TREND_CONTINUATION -- those are supervisor regime names.
        const char* eng_regime = omega::regime_name(sdec.regime);
        // Adaptive TP: compress target in LOW/CRUSH vol so it actually fills.
        // CRUSH=0.70x, LOW=0.85x, NORMAL=1.00x, HIGH=1.15x -- set each tick.
        eng.EDGE_CFG.tp_vol_mult = static_cast<double>(g_regime_adaptor.tp_vol_mult(sym));
        const auto sig = eng.update(bid, ask, rtt_check, eng_regime, on_close, can_enter);
        g_telemetry.UpdateEngineState(sym.c_str(),
            static_cast<int>(eng.phase), eng.comp_high, eng.comp_low,
            eng.recent_vol_pct, eng.base_vol_pct, eng.signal_count);

        // ?? Pyramid add-on dispatch ???????????????????????????????????????????
        // engine sets pyramid_pending=true when trail1 arm + expansion regime met.
        // We send the add-on order here and clear the flag so it fires exactly once.
        if (eng.pos.pyramid_pending && eng.pos.pyramid_entry > 0.0) {
            eng.pos.pyramid_pending = false;
            const double pyr_sl_abs_raw = std::fabs(eng.pos.pyramid_entry - eng.pos.pyramid_sl);
            const double pyr_sl_abs = std::min(pyr_sl_abs_raw, 3.0);  // cap pyramid SL at $3
            // Fix: use half of the actual patched base lot (eng.pos.size) not a fresh
            // compute_size() which recalculates from eng.ENTRY_SIZE and can mismatch
            // the lot that was actually sent to the broker for the base trade.
            const double pyr_lot    = std::min(std::max(0.01, eng.pos.size * 0.5), 0.20);
            std::cout << "\033[1;36m[PYRAMID] " << sym
                      << " " << (eng.pos.is_long ? "LONG" : "SHORT")
                      << " add-on entry=" << eng.pos.pyramid_entry
                      << " tp=" << eng.pos.pyramid_tp
                      << " sl=" << eng.pos.pyramid_sl
                      << " size=" << pyr_lot << "\033[0m\n";
            std::cout.flush();
            // ?? Pyramid L2 gate ???????????????????????????????????????????
            // Don't add-on into absorption (institutional fading) or a wall
            // directly above/below the pyramid entry toward TP.
            bool pyr_l2_ok = true;
            {
                const bool absorbing = g_edges.absorption.is_absorbing(
                    sym, eng.pos.is_long);
                bool wall_in_dir = false;
                const std::string_view psv(sym);
                if      (psv == "XAUUSD")  wall_in_dir = eng.pos.is_long
                    ? g_macro_ctx.gold_wall_above : g_macro_ctx.gold_wall_below;
                else if (psv == "US500.F" || psv == "USTEC.F" ||
                         psv == "DJ30.F"  || psv == "NAS100")
                    wall_in_dir = eng.pos.is_long
                        ? g_macro_ctx.sp_wall_above : g_macro_ctx.sp_wall_below;
                else if (psv == "EURUSD") wall_in_dir = eng.pos.is_long
                    ? g_macro_ctx.eur_wall_above : g_macro_ctx.eur_wall_below;
                else if (psv == "GBPUSD") wall_in_dir = eng.pos.is_long
                    ? g_macro_ctx.gbp_wall_above : g_macro_ctx.gbp_wall_below;
                if (absorbing || wall_in_dir) {
                    pyr_l2_ok = false;
                    printf("[PYRAMID-L2-BLOCK] %s %s absorb=%d wall=%d -- pyramid suppressed\n",
                           sym.c_str(), eng.pos.is_long?"LONG":"SHORT",
                           absorbing?1:0, wall_in_dir?1:0);
                }
            }
            // Also block pyramid during session transition noise windows
            {
                struct tm ti_pyr{}; const auto t_pyr = std::chrono::system_clock::to_time_t(
                    std::chrono::system_clock::now());
                gmtime_s(&ti_pyr, &t_pyr);
                const int mins_pyr = ti_pyr.tm_hour * 60 + ti_pyr.tm_min;
                const bool in_transition = (mins_pyr >= 1320 && mins_pyr < 1330)
                                        || (mins_pyr >= 0    && mins_pyr < 15);
                if (in_transition) pyr_l2_ok = false;
            }
            if (pyr_l2_ok && cost_ok(sym.c_str(), pyr_sl_abs, pyr_lot))
                send_live_order(sym, eng.pos.is_long, pyr_lot, eng.pos.pyramid_entry);
        }

        if (!sig.valid) return;

        const double sl_abs_raw = sig.entry * eng.SL_PCT / 100.0;
        // ATR-normalised SL floor: prevent oversized lots when comp_range is tiny.
        // Never size from an SL smaller than half the slow ATR baseline.
        const double sl_abs = g_adaptive_risk.vol_scaler.atr_sl_floor(sym, sl_abs_raw);
        if (sl_abs > sl_abs_raw) {
            static thread_local int64_t s_atr_log = 0;
            if (nowSec() - s_atr_log > 30) {
                s_atr_log = nowSec();
                printf("[ATR-SL-FLOOR] %s sl_raw=%.5f ? sl_floor=%.5f (ATR slow=%.5f)\n",
                       sym.c_str(), sl_abs_raw, sl_abs,
                       g_adaptive_risk.vol_scaler.atr_slow(sym));
            }
        }
        // Compute lot_size but do NOT write back to eng.ENTRY_SIZE yet.
        // eng.ENTRY_SIZE must only be updated if the trade actually executes.
        const double lot_size_base = compute_size(sym, sl_abs, ask - bid, eng.ENTRY_SIZE);
        // Vol-regime size scale from RegimeAdaptor -- CRUSH=1.10, HIGH=0.75, NORMAL=1.0
        // Previously computed but never applied -- was dead code
        const double vol_mult = static_cast<double>(
            g_regime_adaptor.vol_size_scale(sym));
        // ?? Adaptive risk adjustment: Kelly + DD throttle + vol regime ????????
        // Pull current daily loss for this symbol from SymbolRiskState
        double sym_daily_loss = 0.0;
        int    sym_consec     = 0;
        {
            std::lock_guard<std::mutex> lk(g_sym_risk_mtx);
            auto it = g_sym_risk.find(sym);
            if (it != g_sym_risk.end()) {
                sym_daily_loss = std::max(0.0, -it->second.daily_pnl); // positive = loss
                sym_consec     = it->second.consec_losses;
            }
        }
        double lot_size = g_adaptive_risk.adjusted_lot(
            sym, lot_size_base * vol_mult, sym_daily_loss, g_cfg.daily_loss_limit, sym_consec);

        // ?? TOD-weighted lot scaling ??????????????????????????????????????
        // Scale down on marginal time-of-day buckets (WR < 55%) instead of
        // binary block/allow. Reduces size 10-40% on borderline windows.
        {
            const double tod_mult = g_edges.tod.size_scale(sym, "ALL", nowSec());
            if (tod_mult < 1.0) {
                printf("[TOD-SCALE] %s lot %.4f ? %.4f (tod_mult=%.2f)\n",
                       sym.c_str(), lot_size, lot_size * tod_mult, tod_mult);
                lot_size *= tod_mult;
                lot_size = std::max(0.01, std::floor(lot_size * 100.0 + 0.5) / 100.0);
            }
        }

        // ?? HTF bias size scale ???????????????????????????????????????????
        // 1.0? when daily+intraday both agree with direction (Jane Street 2/2 rule).
        // 0.5? when both TFs oppose direction -- trade is counter-trend on all TFs.
        // 0.75? when TFs are mixed/neutral -- modest size reduction for uncertainty.
        {
            const double htf_mult = g_htf_filter.size_scale(sym, sig.is_long);
            if (htf_mult < 1.0) {
                static thread_local int64_t s_htf_log = 0;
                if (nowSec() - s_htf_log > 30) {
                    s_htf_log = nowSec();
                    printf("[HTF-BIAS] %s %s bias=%s ? lot %.4f ? %.2f\n",
                           sym.c_str(), sig.is_long ? "LONG" : "SHORT",
                           g_htf_filter.bias_name(sym),
                           lot_size, htf_mult);
                }
                lot_size *= htf_mult;
                lot_size = std::max(0.01, std::floor(lot_size * 100.0 + 0.5) / 100.0);
            }
        }

        omega::TradeCandidate cand = omega::build_candidate(
            omega::EdgeResult{
                sig.net_edge > 0 ? sig.net_edge : 0.0,
                0.0, sig.net_edge,
                sig.tp, sig.sl, static_cast<double>(lot_size),
                sig.breakout_strength, sig.momentum_score, sig.vol_score,
                true
            },
            sig.is_long, sig.entry, sym.c_str());

        const int64_t now_ms = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (now_ms - g_cycle_window_start_ms > RANKING_WINDOW_MS) {
            g_cycle_candidates.clear();
            g_cycle_window_start_ms = now_ms;
        }

        // Per-symbol duplicate guard: if this symbol already has a candidate in the
        // current window, replace it only if the new score is better -- never double-enter.
        {
            auto existing = std::find_if(g_cycle_candidates.begin(), g_cycle_candidates.end(),
                [&sym](const omega::TradeCandidate& c) {
                    return std::string(c.symbol) == sym;
                });
            if (existing != g_cycle_candidates.end()) {
                if (cand.score > existing->score) {
                    *existing = cand;  // replace with higher-scoring candidate
                }
                // Either way, don't add a second entry for same symbol
            } else {
                g_cycle_candidates.push_back(cand);
            }
        }

        auto selected = omega::select_best_trades(g_cycle_candidates, g_ranking_cfg);
        if (selected.empty()) {
            std::cout << "[RANKED OUT] " << sym << " no valid candidates\n";
            std::cout.flush();
            g_cycle_candidates.clear();  // don't let invalid candidates persist into next window
            return;
        }
        const omega::TradeCandidate& best = selected[0];
        if (std::string(best.symbol) != sym) {
            std::cout << "[RANKED OUT] " << sym
                      << " outranked by " << best.symbol
                      << " score=" << best.score << "\n";
            std::cout.flush();
            // Do NOT clear g_cycle_candidates -- the winner stays live for execution
            // on its own dispatch call this same tick. But remove this symbol's
            // losing candidate so it cannot re-compete on the next tick.
            g_cycle_candidates.erase(
                std::remove_if(g_cycle_candidates.begin(), g_cycle_candidates.end(),
                    [&sym](const omega::TradeCandidate& c) {
                        return std::string(c.symbol) == sym;
                    }),
                g_cycle_candidates.end());
            return;
        }

        // Final supervisor re-check before execution.
        // eng.phase is FLAT at this point (set inside update() when signal fires).
        // If supervisor flipped to NO_TRADE between arming and breakout, block here.
        if (!sdec.allow_breakout) {
            std::cout << "[ENG-" << sym << "] BLOCKED: supervisor_recheck allow=0 at execution\n";
            std::cout.flush();
            g_cycle_candidates.clear();
            return;
        }

        // Cost guard: block if spread+commission+slippage > expected gross ? 1.5
        {
            const double tp_dist = std::fabs(sig.tp - sig.entry);
            if (!ExecutionCostGuard::is_viable(sym.c_str(), ask - bid, tp_dist, lot_size)) {
                g_telemetry.IncrCostBlocked();
                g_cycle_candidates.clear();
                return;
            }
        }

        // All gates passed -- commit sizing and execute.
        // eng.ENTRY_SIZE and telemetry written here only, after all gates cleared.
        eng.ENTRY_SIZE  = lot_size;
        // CRITICAL: patch pos.size with the correct risk-based lot size.
        // pos.size was set to edge.size inside compute_edge_and_execution which uses
        // account_equity*0.002/sl_dist -- an internal formula that bypasses risk_per_trade_usd
        // and the per-symbol lot caps. Without this patch, tr.pnl = move * edge.size
        // (potentially 5-10 lots) instead of move * lot_size (0.01-0.10 lots).
        eng.pos.size    = lot_size;
        g_telemetry.UpdateLastSignal(sym.c_str(),
            sig.is_long ? "LONG" : "SHORT", sig.entry, sig.reason,
            omega::regime_name(sdec.regime), regime.c_str(), "BREAKOUT",
            sig.tp, sig.sl);
        std::cout << "\033[1;" << (sig.is_long ? "32" : "31") << "m"
                  << "[OMEGA] " << sym << " " << (sig.is_long ? "LONG" : "SHORT")
                  << " entry=" << sig.entry << " tp=" << sig.tp << " sl=" << sig.sl
                  << " size=" << lot_size << " score=" << best.score
                  << " sup_regime=" << omega::regime_name(sdec.regime)
                  << " regime=" << regime << "\033[0m\n";
        // ?? Arm partial exit (split TP: 50% at 1R, trail remainder) ??????????
        g_partial_exit.arm(sym, sig.is_long, sig.entry, sig.tp, sig.sl, lot_size,
                           g_adaptive_risk.vol_scaler.atr_fast(sym));
        send_live_order(sym, sig.is_long, lot_size, sig.entry);
        g_cycle_candidates.clear();
    };

    // ?? dispatch_bracket -- bracket engine + supervisor gated ?????????????????
    // Runs supervisor (shared with breakout for the symbol), gates arming on
    // allow_bracket. Position management always runs regardless.
    auto dispatch_bracket = [&](auto& bracket_eng,
                                 omega::SymbolSupervisor& sup,
                                 auto& ref_eng,         // breakout eng for vol/phase data
                                 bool base_can_enter,
                                 double vwap_val,
                                 int& trades_this_min,
                                 int64_t& min_start,
                                 double l2_imb = 0.5,
                                 const omega::SupervisorDecision* precomputed_sdec = nullptr) {
        int fb = 0;
        { std::lock_guard<std::mutex> lk(g_false_break_mtx);
          auto it = g_false_break_counts.find(sym); if (it != g_false_break_counts.end()) fb = it->second; }
        const double bkt_momentum = (ref_eng.base_vol_pct > 0.0)
            ? ((ref_eng.recent_vol_pct - ref_eng.base_vol_pct) / ref_eng.base_vol_pct * 100.0)
            : 0.0;
        // in_compression: true when breakout engine is in COMPRESSION/WATCH,
        // OR when the bracket engine itself has ARMED (it detected compression independently).
        // Without this, bracket ARMED state is invisible to the supervisor -- it scores
        // the symbol as HIGH_RISK/no_dominant and blocks the bracket it should allow.
        const bool bracket_detected_compression =
            (bracket_eng.phase == omega::BracketPhase::ARMED ||
             bracket_eng.phase == omega::BracketPhase::PENDING);
        const bool in_compression_for_sup =
            (ref_eng.phase == omega::Phase::COMPRESSION) || bracket_detected_compression;
        // Reuse pre-computed decision if provided -- avoids double-calling sup.update()
        // which causes allow=1/allow=0 flicker when dispatch + dispatch_bracket both
        // run for the same symbol on the same tick.
        const auto sdec = precomputed_sdec ? *precomputed_sdec
                                           : sup.update(
                                               bid, ask,
                                               ref_eng.recent_vol_pct, ref_eng.base_vol_pct,
                                               bkt_momentum,
                                               ref_eng.comp_high, ref_eng.comp_low,
                                               in_compression_for_sup,
                                               fb);

        // Frequency limit
        const int64_t now_ms = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (now_ms - min_start >= 60000) { min_start = now_ms; trades_this_min = 0; }
        const bool freq_ok = (trades_this_min < 2);

        const bool bracket_open    = bracket_eng.has_open_position();
        const bool bracket_armed   = (bracket_eng.phase == omega::BracketPhase::ARMED);
        const bool bracket_pending = (bracket_eng.phase == omega::BracketPhase::PENDING);

        // ?? Direct spread guard -- belt-and-suspenders beyond supervisor ???????
        // Supervisor spread gate was broken (max_spread_pct defaulted to 0.10=10%,
        // never overridden). Now fixed via apply_supervisor, but also guard here
        // as a second layer. Only blocks NEW arming (IDLE phase) -- never cancels
        // an already ARMED/PENDING/LIVE position.
        // London open guard (07:00-07:15 UTC): spreads blow out on all instruments
        // at session open. Same pattern as gold's in_london_open_noise.
        const bool bracket_spread_blocked = [&]() -> bool {
            if (bracket_open || bracket_armed || bracket_pending) return false; // managing existing
            const double mid_price = (bid + ask) * 0.5;
            if (mid_price <= 0.0) return false;
            const double spread_pct = (ask - bid) / mid_price * 100.0;
            // Use >= with a small epsilon to handle floating-point equality at limit
            if (spread_pct >= sup.cfg.max_spread_pct * 1.001) {
                static thread_local int64_t s_last_log = 0;
                if (nowSec() - s_last_log > 30) {
                    s_last_log = nowSec();
                    printf("[BRACKET-SPREAD-BLOCK] %s spread_pct=%.3f%% >= max=%.3f%%\n",
                           sym.c_str(), spread_pct, sup.cfg.max_spread_pct);
                }
                ++g_gov_spread;
                return true;
            }
            return false;
        }();
        // London open guard: 07:00-07:15 UTC -- violent liquidity sweeps on all instruments
        const bool bracket_london_noise = [&]() -> bool {
            if (bracket_open || bracket_armed || bracket_pending) return false;
            const auto t_bn = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now());
            struct tm ti_bn{}; gmtime_s(&ti_bn, &t_bn);
            const int mins_utc = ti_bn.tm_hour * 60 + ti_bn.tm_min;
            return (mins_utc >= 420 && mins_utc < 435); // 07:00-07:15 UTC
        }();

        // ?? Trend bias: L2-aware counter-trend suppression + pyramiding ???????
        // Update L2 adjustment on the trend state every tick (throttled internally to 5s)
        BracketTrendState& trend = g_bracket_trend[sym];
        trend.update_l2(l2_imb, now_ms);
        const bool trend_blocked   = trend.counter_trend_blocked(now_ms);
        const bool pyramid_ok      = trend.pyramid_allowed(l2_imb, now_ms);

        // Pyramiding: when trend bias active + L2 strongly confirms + position already open
        // in trend direction ? allow a second bracket arm (base_can_enter bypasses !bracket_open).
        // The second arm uses PYRAMID_SIZE_MULT of normal size.
        const bool is_pyramiding   = pyramid_ok && bracket_open &&
                                     ((trend.bias == 1  && bracket_eng.pos.is_long) ||
                                      (trend.bias == -1 && !bracket_eng.pos.is_long));

        const bool can_arm         = (base_can_enter && sdec.allow_bracket && freq_ok
                                      && (!bracket_open || is_pyramiding)
                                      && !trend_blocked
                                      && !bracket_spread_blocked
                                      && !bracket_london_noise)
                                   // ?? Supervisor chop_detected price-break override ???????
                                   // Same logic as gold: if ARMED and price breaks the bracket
                                   // level by >= 2pt, bypass supervisor allow_bracket=0.
                                   // Hard gates (base_can_enter, freq_ok, no open pos) still apply.
                                   || ([&]() -> bool {
                                       if (!bracket_armed) return false;
                                       if (!base_can_enter || !freq_ok) return false;
                                       if (bracket_open && !is_pyramiding) return false;
                                       if (trend_blocked || bracket_spread_blocked || bracket_london_noise) return false;
                                       const double bhi = bracket_eng.bracket_high;
                                       const double blo = bracket_eng.bracket_low;
                                       const bool brk_hi_hit = (bhi > 0.0) && (ask >= bhi + 2.0);
                                       const bool brk_lo_hit = (blo > 0.0) && (bid <= blo - 2.0);
                                       if (!brk_hi_hit && !brk_lo_hit) return false;
                                       static thread_local int64_t s_disp_ovr_log = 0;
                                       const int64_t now_do = static_cast<int64_t>(std::time(nullptr));
                                       if (now_do - s_disp_ovr_log >= 5) {
                                           s_disp_ovr_log = now_do;
                                           printf("[BRK-CHOP-OVERRIDE] %s supervisor chop bypassed -- "
                                                  "price broke %s by %.1fpt\n",
                                                  sym.c_str(),
                                                  brk_hi_hit ? "brk_hi" : "brk_lo",
                                                  brk_hi_hit ? (ask - bhi) : (blo - bid));
                                           fflush(stdout);
                                       }
                                       return true;
                                   }());

        // Gate logic by phase:
        //   IDLE    ? can_arm: supervisor + session + freq + trend bias all required
        //   ARMED   ? true: structure qualified, timer must run uninterrupted
        //   PENDING ? true: orders at broker, only timeout should cancel
        //   LIVE    ? can_manage: allow force-close on session/risk gate
        const bool can_manage      = (bracket_armed || bracket_pending) ? true
                                                     : (base_can_enter && sdec.allow_bracket);

        bracket_eng.on_tick(bid, ask, now_ms,
            (bracket_open || bracket_armed) ? can_manage : can_arm,
            regime.c_str(), bracket_on_close, vwap_val, l2_imb);

        // Update bracket state in telemetry snapshot every tick so GUI shows live levels
        g_telemetry.UpdateBracketState(sym.c_str(),
            static_cast<int>(bracket_eng.phase),
            bracket_eng.bracket_high,
            bracket_eng.bracket_low);

        const auto bsigs = bracket_eng.get_signals();
        if (bsigs.valid) {
            const double raw_sl_dist = std::fabs(bsigs.long_entry - bsigs.long_sl);
            // Pyramid: cap SL and lot to limit add-on risk
            const double eff_sl_dist = is_pyramiding
                ? std::min(raw_sl_dist, raw_sl_dist * 0.5)  // 50% of natural SL for pyramids
                : raw_sl_dist;
            const double base_lot = compute_size(sym, eff_sl_dist, ask - bid,
                bracket_eng.ENTRY_SIZE);
            const double raw_lot = is_pyramiding ? (base_lot * PYRAMID_SIZE_MULT) : base_lot;
            // Cap pyramid lot at 50% of normal max lot for this symbol
            const double max_pyr_lot = is_pyramiding ? 0.10 : 1.0;  // indices: max 0.10 lots pyramid
            const double lot = std::min(raw_lot, max_pyr_lot);
            // Cost guard: ensure spread+cost is covered by bracket TP distance
            {
                const double tp_dist = raw_sl_dist *
                    (bracket_eng.RR > 0.0 ? bracket_eng.RR : 1.5);
                if (!ExecutionCostGuard::is_viable(sym.c_str(), ask - bid, tp_dist, lot)) {
                    g_telemetry.IncrCostBlocked();
                    return;
                }
            }

            // ?? Bracket R:R floor ?????????????????????????????????????????
            // RR config is the minimum acceptable R:R for bracket signals.
            // Enforce 1.5 as a hard floor regardless of config.
            {
                const double bkt_rr = bracket_eng.RR > 0.0 ? bracket_eng.RR : 1.5;
                if (bkt_rr < 1.5) {
                    printf("[RR-FLOOR] %s BRACKET blocked: R:R=%.2f < 1.5 floor\n",
                           sym.c_str(), bkt_rr);
                    return;
                }
            }

            // ?? Bracket L2 microstructure gate ????????????????????????????
            // Brackets bypass enter_directional so they previously skipped the
            // full L2 scoring layer. Apply entry_score_l2 to both directions;
            // if BOTH legs score <= -3, the setup is structurally blocked.
            // If only one leg is blocked, proceed with the other at half size.
            {
                double bkt_microprice = 0.0, bkt_l2_imb = l2_imb;
                bool bkt_vac_ask = false, bkt_vac_bid = false;
                bool bkt_wall_above = false, bkt_wall_below = false;
                const std::string_view sv_bkt(sym);
                if (sv_bkt == "XAUUSD") {
                    bkt_microprice  = g_macro_ctx.gold_microprice_bias;
                    bkt_vac_ask     = g_macro_ctx.gold_vacuum_ask;
                    bkt_vac_bid     = g_macro_ctx.gold_vacuum_bid;
                    bkt_wall_above  = g_macro_ctx.gold_wall_above;
                    bkt_wall_below  = g_macro_ctx.gold_wall_below;
                } else if (sv_bkt == "US500.F" || sv_bkt == "USTEC.F" ||
                           sv_bkt == "DJ30.F"  || sv_bkt == "NAS100") {
                    bkt_microprice  = g_macro_ctx.sp_microprice_bias;
                    bkt_vac_ask     = g_macro_ctx.sp_vacuum_ask;
                    bkt_vac_bid     = g_macro_ctx.sp_vacuum_bid;
                    bkt_wall_above  = g_macro_ctx.sp_wall_above;
                    bkt_wall_below  = g_macro_ctx.sp_wall_below;
                } else if (sv_bkt == "USOIL.F" || sv_bkt == "BRENT") {
                    bkt_microprice  = g_macro_ctx.cl_microprice_bias;
                    bkt_vac_ask     = g_macro_ctx.cl_vacuum_ask;
                    bkt_vac_bid     = g_macro_ctx.cl_vacuum_bid;
                } else if (sv_bkt == "XAGUSD") {
                    bkt_microprice  = g_macro_ctx.xag_microprice_bias;
                    bkt_vac_ask     = bkt_l2_imb < 0.30;
                    bkt_vac_bid     = bkt_l2_imb > 0.70;
                } else if (sv_bkt == "EURUSD") {
                    bkt_microprice = g_macro_ctx.eur_microprice_bias;
                    bkt_vac_ask    = g_macro_ctx.eur_vacuum_ask;
                    bkt_vac_bid    = g_macro_ctx.eur_vacuum_bid;
                    bkt_wall_above = g_macro_ctx.eur_wall_above;
                    bkt_wall_below = g_macro_ctx.eur_wall_below;
                } else if (sv_bkt == "GBPUSD") {
                    bkt_microprice = g_macro_ctx.gbp_microprice_bias;
                    bkt_vac_ask    = g_macro_ctx.gbp_vacuum_ask;
                    bkt_vac_bid    = g_macro_ctx.gbp_vacuum_bid;
                    bkt_wall_above = g_macro_ctx.gbp_wall_above;
                    bkt_wall_below = g_macro_ctx.gbp_wall_below;
                } else if (sv_bkt == "AUDUSD") {
                    bkt_vac_ask = g_macro_ctx.aud_vacuum_ask;
                    bkt_vac_bid = g_macro_ctx.aud_vacuum_bid;
                } else if (sv_bkt == "NZDUSD") {
                    bkt_vac_ask = g_macro_ctx.nzd_vacuum_ask;
                    bkt_vac_bid = g_macro_ctx.nzd_vacuum_bid;
                } else if (sv_bkt == "USDJPY") {
                    bkt_vac_ask = g_macro_ctx.jpy_vacuum_ask;
                    bkt_vac_bid = g_macro_ctx.jpy_vacuum_bid;
                } else if (sv_bkt == "GER40") {
                    bkt_microprice = g_macro_ctx.ger40_microprice_bias;
                    bkt_vac_ask    = g_macro_ctx.ger40_vacuum_ask;
                    bkt_vac_bid    = g_macro_ctx.ger40_vacuum_bid;
                    bkt_wall_above = g_macro_ctx.ger40_wall_above;
                    bkt_wall_below = g_macro_ctx.ger40_wall_below;
                } else if (sv_bkt == "UK100") {
                    bkt_vac_ask = g_macro_ctx.uk100_vacuum_ask;
                    bkt_vac_bid = g_macro_ctx.uk100_vacuum_bid;
                } else if (sv_bkt == "ESTX50") {
                    bkt_vac_ask = g_macro_ctx.estx50_vacuum_ask;
                    bkt_vac_bid = g_macro_ctx.estx50_vacuum_bid;
                } else {
                    // Fallback: L2 imbalance proxy for any uncovered symbol
                    bkt_vac_ask = bkt_l2_imb < 0.30;
                    bkt_vac_bid = bkt_l2_imb > 0.70;
                }
                const int score_long  = g_edges.entry_score_l2(
                    sym, bsigs.long_entry,  true,  bsigs.long_tp,
                    nowSec(), bkt_microprice, bkt_l2_imb, bkt_vac_ask, bkt_wall_above);
                const int score_short = g_edges.entry_score_l2(
                    sym, bsigs.short_entry, false, bsigs.short_tp,
                    nowSec(), bkt_microprice, bkt_l2_imb, bkt_vac_bid, bkt_wall_below);
                if (score_long <= -3 && score_short <= -3) {
                    printf("[EDGE-BLOCK-BKT] %s BRACKET both legs blocked: L=%d S=%d\n",
                           sym.c_str(), score_long, score_short);
                    return;
                }
            }

            // ?? Cross-engine dedup (inline -- lambda not yet in scope here) ?
            {
                std::lock_guard<std::mutex> _lk(g_dedup_mtx);
                auto _it = g_last_cross_entry.find(sym);
                if (_it != g_last_cross_entry.end() &&
                    (nowSec() - _it->second) < CROSS_ENG_DEDUP_SEC) {
                    printf("[CROSS-DEDUP] %s BRACKET blocked -- another engine entered %.0fs ago\n",
                           sym.c_str(), static_cast<double>(nowSec() - _it->second));
                    return;
                }
                g_last_cross_entry[sym] = nowSec();
            }

            // Encode bracket levels + trend state in reason for GUI
            char bracket_reason[80];
            snprintf(bracket_reason, sizeof(bracket_reason), "HI:%.2f LO:%.2f bias:%d l2:%.2f",
                     bsigs.long_entry, bsigs.short_entry, trend.bias, l2_imb);
            g_telemetry.UpdateLastSignal(sym.c_str(), "BRACKET", bsigs.long_entry, bracket_reason,
                omega::regime_name(sdec.regime), regime.c_str(), "BRACKET",
                bsigs.long_tp, bsigs.long_sl);
            std::cout << "\033[1;33m[BRACKET] " << sym
                      << " sup_regime=" << omega::regime_name(sdec.regime)
                      << " bracket_score=" << sdec.bracket_score
                      << " winner=" << sdec.winner
                      << " bias=" << trend.bias
                      << " l2=" << std::fixed << std::setprecision(2) << l2_imb
                      << (is_pyramiding ? " PYRAMID" : "")
                      << "\033[0m\n";

            // L2-aware order sizing:
            //   When trend bias active: send trend-direction leg at full size,
            //   counter-trend leg at half size (hedge leg, expect cancellation).
            //   When no bias: both legs at full size (standard bracket).
            std::string long_id, short_id;
            if (trend.bias != 0) {
                const bool long_is_trend  = (trend.bias == 1);
                const double trend_lot    = lot;
                const double counter_lot  = lot * 0.5;
                long_id  = send_live_order(sym, true,  long_is_trend  ? trend_lot : counter_lot, bsigs.long_entry);
                short_id = send_live_order(sym, false, !long_is_trend ? trend_lot : counter_lot, bsigs.short_entry);
                printf("[BRACKET-L2] %s bias=%d trend_lot=%.4f counter_lot=%.4f l2=%.3f\n",
                       sym.c_str(), trend.bias, trend_lot, counter_lot, l2_imb);
            } else {
                long_id  = send_live_order(sym, true,  lot, bsigs.long_entry);
                short_id = send_live_order(sym, false, lot, bsigs.short_entry);
            }
            bracket_eng.pending_long_clOrdId  = long_id;
            bracket_eng.pending_short_clOrdId = short_id;
            // Tag pyramid arms so bracket_on_close can enforce SL cooldown
            if (is_pyramiding) {
                g_pyramid_clordids.insert(sym);
            }
            ++trades_this_min;
        }
    };

    // ?? cost_ok() -- mandatory gate for ALL direct send_live_order calls ???????
    // Every engine signal that bypasses dispatch()/dispatch_bracket() must call
    // this before executing. Blocks the trade and increments the cost counter
    // if spread + commission + slippage exceeds expected gross ? EDGE_MULTIPLIER.
    // Params: symbol, sl_abs (SL distance in price points), lot.
    // Uses the same ExecutionCostGuard that dispatch() and dispatch_bracket() use,
    // with RR=1.5 as the TP estimate (conservative: actual RR is often 2.0+).
    // ?? Cross-engine deduplication ????????????????????????????????????????????
    // Per-symbol 30s lockout across all engine types -- statics live at file scope.
    // cross_engine_dedup_ok -- converted to static function above on_tick
    // Stamp dedup timestamp -- called only after all gates pass and trade executes
    // cross_engine_dedup_stamp -- converted to static function above on_tick
    // ?? enter_directional: unified entry helper for all cross-asset engines ???
    // Replaces the repeated pattern: compute_size ? cost_ok ? send_live_order
    // Also applies adaptive risk (adjusted_lot) and arms partial exit.
    // sig: must have .valid, .is_long, .entry, .sl, .tp (or computed tp_dist)
    // fallback_lot: default size when risk sizing disabled (0.01 for most CA engines)
    // sym_override: use this symbol for sizing/corr if different from outer sym
    // enter_directional -- converted to static function above on_tick
    // ?? Partial exit tick check ???????????????????????????????????????????????
    // Runs every tick for every symbol. No-op when no partial state is active.
    // When TP1 is hit: sends a market close for the first half and moves SL to BE.
    // When TP2/trailing SL is hit: sends final close for the remainder.
    // Applies in all modes -- shadow simulates the close without sending a real order.
    // XAUUSD + GFE open: GoldFlowEngine manages its own partial internally via
    // manage_position() ? PARTIAL_1R callback. Skip here to prevent duplicate orders.
    const bool gfe_owns_partial = (sym == "XAUUSD" && g_gold_flow.has_open_position());
    if (g_partial_exit.active(sym) && !gfe_owns_partial) {
        double pe_price = 0.0, pe_lot = 0.0;
        using PE = omega::partial::CloseAction;
        const PE act = g_partial_exit.tick(sym, mid, bid, ask, pe_price, pe_lot);
        if (act == PE::PARTIAL || act == PE::FULL) {
            const bool close_is_long = !g_partial_exit.entry_is_long(sym);
            // In LIVE mode send the actual order; in SHADOW simulate the fill at current mid.
            if (g_cfg.mode == "LIVE") {
                send_live_order(sym, close_is_long, pe_lot, pe_price);
            }
            // Log in both modes -- shadow records the simulated partial as if it were real.
            std::printf("[PARTIAL-EXIT]%s %s %s %.2f lots @ %.5f  entry_long=%d\n",
                        g_cfg.mode == "LIVE" ? "" : "[SHADOW]",
                        sym.c_str(),
                        act == PE::PARTIAL ? "TP1-HIT" : "TP2/TRAIL-HIT",
                        pe_lot, pe_price,
                        g_partial_exit.entry_is_long(sym) ? 1 : 0);
            if (act == PE::FULL) {
                g_partial_exit.reset(sym);
            }
        }
    }

    // ?? ACTIVE SYMBOLS GATE ???????????????????????????????????????????????????
    // SIM-VALIDATED: XAUUSD and USOIL.F have proven compression edge.
    // US indices (US500.F, USTEC.F, NAS100, DJ30.F) now routed for
    // NoiseBandMomentumEngine -- Zarattini/Maroy research (Sharpe 3.0-5.9).
    // All other symbols remain hard-blocked until re-validated.
    {
        // All symbols active -- bars now built from FIX ticks directly, no broker bar API needed
        const bool is_active_sym = (sym == "XAUUSD"  || sym == "USOIL.F"  ||
                                    sym == "US500.F" || sym == "USTEC.F"  ||
                                    sym == "NAS100"  || sym == "DJ30.F");
        // XAGUSD hard-blocked: SilverTurtleTick real-tick backtest result:
        // Sharpe=-16.23, MaxDD=$18,381, 0 positive months across 24 months.
        // Root cause: 65% timeout rate, TP=$0.30 requires 49x the actual
        // avg 45-min move. Silver reverts too fast for Turtle architecture.
        // All 12 silver strategies were audited -- none viable. DROP silver.
        if (!is_active_sym) return;
    }

    // ?? Routing -- every symbol goes through supervisor ????????????????????????
    // ── Symbol dispatch ────────────────────────────────────────────────────────
    if      (sym == "US500.F")                          on_tick_us500(sym, bid, ask, tradeable, lat_ok, regime);
    else if (sym == "USTEC.F")                          on_tick_ustec(sym, bid, ask, tradeable, lat_ok, regime);
    else if (sym == "USOIL.F")                          on_tick_oil(sym, bid, ask, tradeable, lat_ok, regime, dispatch);
    else if (sym == "DJ30.F")                           on_tick_dj30(sym, bid, ask, tradeable, lat_ok, regime);
    else if (sym == "GER40")                            on_tick_ger40(sym, bid, ask, tradeable, lat_ok, regime);
    else if (sym == "UK100")                            on_tick_uk100(sym, bid, ask, tradeable, lat_ok, regime);
    else if (sym == "ESTX50")                           on_tick_estx50(sym, bid, ask, tradeable, lat_ok, regime);
    else if (sym == "XAGUSD")                           on_tick_silver(sym, bid, ask, tradeable, lat_ok, regime);
    else if (sym == "EURUSD")                           on_tick_eurusd(sym, bid, ask, tradeable, lat_ok, regime, dispatch);
    else if (sym == "GBPUSD")                           on_tick_gbpusd(sym, bid, ask, tradeable, lat_ok, regime, dispatch);
    else if (sym == "AUDUSD" || sym == "NZDUSD" || sym == "USDJPY")
                                                        on_tick_audusd(sym, bid, ask, tradeable, lat_ok, regime, dispatch);
    else if (sym == "BRENT")                            on_tick_brent(sym, bid, ask, tradeable, lat_ok, regime, dispatch);
    else if (sym == "NAS100")                           on_tick_nas100(sym, bid, ask, tradeable, lat_ok, regime);
    else if (sym == "XAUUSD")                           on_tick_gold(sym, bid, ask, tradeable, lat_ok, regime, rtt_check);
    else {
        // Confirmation-only symbol (VIX, ES, NAS100, DX etc) -- no engine dispatch
        g_telemetry.UpdateGovernor(g_gov_spread, g_gov_lat, g_gov_pnl, g_gov_pos, g_gov_consec);
        return;
    }

    g_telemetry.UpdateGovernor(g_gov_spread, g_gov_lat, g_gov_pnl, g_gov_pos, g_gov_consec);

    // ?? SL cooldown telemetry -- collect active cooldowns across all engines ??
    {
        const int64_t now_s = static_cast<int64_t>(std::time(nullptr));
        std::vector<std::pair<std::string,int>> cooldowns;
        auto chk = [&](auto& eng, const char* name) {
            const int64_t rem = eng.sl_cooldown_until() - now_s;
            if (rem > 0) cooldowns.push_back({name, static_cast<int>(rem)});
        };
        chk(g_eng_sp,     "US500.F");
        chk(g_eng_nq,     "USTEC.F");
        chk(g_eng_cl,     "USOIL.F");
        chk(g_eng_us30,   "DJ30.F");
        chk(g_eng_nas100, "NAS100");
        chk(g_eng_ger30,  "GER40");
        chk(g_eng_uk100,  "UK100");
        chk(g_eng_estx50, "ESTX50");
        chk(g_eng_xag,    "XAGUSD");
        chk(g_eng_eurusd, "EURUSD");
        chk(g_eng_gbpusd, "GBPUSD");
        chk(g_eng_audusd, "AUDUSD");
        chk(g_eng_nzdusd, "NZDUSD");
        chk(g_eng_usdjpy, "USDJPY");
        chk(g_eng_brent,  "BRENT");
        g_telemetry.UpdateSLCooldown(cooldowns);
    }

    // ?? Asia gate + config snapshot ??????????????????????????????????????????
    {
        const auto t_asia = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        struct tm ti_asia; gmtime_s(&ti_asia, &t_asia);
        const int h2 = ti_asia.tm_hour;
        const int asia_open = (!g_cfg.asia_fx_asia_only || (h2 >= 22 || h2 < 7)) ? 1 : 0;
        g_telemetry.UpdateAsiaCfg(asia_open, g_cfg.max_trades_per_cycle, g_cfg.max_open_positions);
        // Push L2 quality flags so GUI shows cTrader status and gold_l2_real indicator
        if (g_telemetry.snap()) {
            g_telemetry.snap()->ctrader_l2_live = g_macro_ctx.ctrader_l2_live ? 1 : 0;
            g_telemetry.snap()->gold_l2_real     = g_macro_ctx.gold_l2_real     ? 1 : 0;
        }
    }

    // ?? Cross-asset engine live state snapshot ???????????????????????????????
    // Written every tick so the GUI always has fresh active/position data.
    // ref_price carries the most meaningful context value per engine type:
    //   ORB      ? range midpoint (0 until range is built)
    //   VWAP_REV ? VWAP proxy used for this instrument
    //   TREND_PB ? EMA50 level (dynamic SL reference)
    //   FX_CASCADE ? 0 (armed state not visible here; signal fires fast)
    {
        // ORB instances
        const double orb_us_mid    = (g_orb_us.range_high()    + g_orb_us.range_low())    > 0.0 ? (g_orb_us.range_high()    + g_orb_us.range_low())    * 0.5 : 0.0;
        const double orb_ger_mid   = (g_orb_ger30.range_high() + g_orb_ger30.range_low())  > 0.0 ? (g_orb_ger30.range_high() + g_orb_ger30.range_low())  * 0.5 : 0.0;
        const double orb_uk_mid    = (g_orb_uk100.range_high()  + g_orb_uk100.range_low())  > 0.0 ? (g_orb_uk100.range_high()  + g_orb_uk100.range_low())  * 0.5 : 0.0;
        const double orb_estx_mid  = (g_orb_estx50.range_high() + g_orb_estx50.range_low()) > 0.0 ? (g_orb_estx50.range_high() + g_orb_estx50.range_low()) * 0.5 : 0.0;
        const double orb_xag_mid   = (g_orb_silver.range_high() + g_orb_silver.range_low()) > 0.0 ? (g_orb_silver.range_high() + g_orb_silver.range_low()) * 0.5 : 0.0;

        auto ca = [&](const char* nm, const char* sym, bool act, bool lng,
                      double ent, double tp, double sl, double ref, int sigs) {
            g_telemetry.UpdateCrossAsset(nm, sym, act?1:0, lng?1:0, ent, tp, sl, ref, sigs);
        };

        // ORB
        ca("ORB_US",    "US500.F", g_orb_us.has_open_position(),    false, 0,0,0, orb_us_mid,   0);
        ca("ORB_GER40", "GER40",   g_orb_ger30.has_open_position(),  false, 0,0,0, orb_ger_mid,  0);
        ca("ORB_UK100", "UK100",   g_orb_uk100.has_open_position(),  false, 0,0,0, orb_uk_mid,   0);
        ca("ORB_ESTX50","ESTX50",  g_orb_estx50.has_open_position(), false, 0,0,0, orb_estx_mid, 0);
        ca("ORB_XAG",   "XAGUSD",  g_orb_silver.has_open_position(), false, 0,0,0, orb_xag_mid,  0);

        // VWAP Reversion -- capture VWAP proxy per instrument
        // SP/NQ share the US ORB range mid; GER40 uses Xetra ORB; EUR uses daily open static
        ca("VWAP_SP",   "US500.F", g_vwap_rev_sp.has_open_position(),     false, 0,0,0, orb_us_mid,  0);
        ca("VWAP_NQ",   "USTEC.F", g_vwap_rev_nq.has_open_position(),     false, 0,0,0, orb_us_mid,  0);
        ca("VWAP_GER40","GER40",   g_vwap_rev_ger40.has_open_position(),  false, 0,0,0, orb_ger_mid, 0);
        ca("VWAP_EUR",  "EURUSD",  g_vwap_rev_eurusd.has_open_position(), false, 0,0,0, 0.0,         0);

        // Trend Pullback -- ref_price = EMA50 (dynamic SL)
        ca("TRENDPB_GOLD","XAUUSD", g_trend_pb_gold.has_open_position(),  false, 0,0,0, g_trend_pb_gold.ema50(),  0);
        ca("TRENDPB_GER", "GER40",  g_trend_pb_ger40.has_open_position(), false, 0,0,0, g_trend_pb_ger40.ema50(), 0);

        // FX Cascade -- one entry covers all three legs; armed state not stored here
        ca("FXCASC_GBP", "GBPUSD", g_ca_fx_cascade.has_open_position(), false, 0,0,0, 0.0, 0);

        // Other cross-asset (EsNq, OilFade, BrentWTI, CarryUnwind)
        ca("ESNQ_DIV",   "US500.F", g_ca_esnq.has_open_position(),        false, 0,0,0, 0.0, 0);
        ca("CARRY_UNW",  "USDJPY",  g_ca_carry_unwind.has_open_position(), false, 0,0,0, 0.0, 0);
    }

    // ?? Real-time cluster dollar exposure ?????????????????????????????????????????
    // Computed every tick from phase==IN_TRADE (3) and open ca_engine positions.
    // Dollar notional = lot_size * tick_value_multiplier(symbol) * direction (1=long, -1=short).
    // For breakout/bracket engines, lot_size is not directly in the snapshot, so we
    // use a fixed 1.0 sentinel scaled by tick_value_multiplier to give relative cluster weight.
    // Exact lot sizing is in the trade ledger; this is for real-time directional exposure.
    {
        // Helper: net dollar exposure for one open position slot
        // phase==3 (IN_TRADE for breakout, LIVE for bracket) = position open
        // For engines that have ca_engine active flags, use those (more precise).
        auto eng_exposure = [](int phase, double lot, double tick_mult, int is_long_hint=1) -> double {
            if (phase != 3) return 0.0;
            return lot * tick_mult * (is_long_hint >= 0 ? 1.0 : -1.0);
        };

        double exp_us = 0.0, exp_eu = 0.0, exp_oil = 0.0;
        double exp_metals = 0.0, exp_jpy = 0.0, exp_egbp = 0.0;

        const auto* sn = g_telemetry.snap();
        if (sn) {
            // Use cross-asset engine live states (have direction) for all cluster symbols
            for (int i = 0; i < sn->ca_engine_count; ++i) {
                const auto& e = sn->ca_engines[i];
                if (!e.active) continue;
                const double tm = tick_value_multiplier(e.symbol);
                // We use 1.0 lot as a relative unit; actual lot unknown from snapshot
                const double dir = e.is_long ? 1.0 : -1.0;
                const double notional = tm * dir;
                const std::string ca_sym(e.symbol);
                using CL = omega::risk::CorrCluster;
                switch (omega::risk::symbol_to_cluster(ca_sym)) {
                    case CL::US_EQUITY: exp_us    += notional; break;
                    case CL::EU_EQUITY: exp_eu    += notional; break;
                    case CL::OIL:       exp_oil   += notional; break;
                    case CL::METALS:    exp_metals+= notional; break;
                    case CL::JPY_RISK:  exp_jpy   += notional; break;
                    case CL::EUR_GBP:   exp_egbp  += notional; break;
                    default: break;
                }
            }
            // Supplement with breakout/bracket phase==3 for symbols not covered by ca_engines
            // (breakout engines don't store direction in snapshot; use +1 as unsigned exposure)
            auto add_bkt = [&](int phase, const char* sym) {
                if (phase != 3) return;
                const double tm = tick_value_multiplier(sym);
                const std::string s(sym);
                using CL = omega::risk::CorrCluster;
                switch (omega::risk::symbol_to_cluster(s)) {
                    case CL::US_EQUITY: exp_us    += tm; break;
                    case CL::EU_EQUITY: exp_eu    += tm; break;
                    case CL::OIL:       exp_oil   += tm; break;
                    case CL::METALS:    exp_metals+= tm; break;
                    case CL::JPY_RISK:  exp_jpy   += tm; break;
                    case CL::EUR_GBP:   exp_egbp  += tm; break;
                    default: break;
                }
            };
            add_bkt(sn->sp_phase,     "US500.F");
            add_bkt(sn->nq_phase,     "USTEC.F");
            add_bkt(sn->cl_phase,     "USOIL.F");
            add_bkt(sn->xau_phase,    "XAUUSD");
            add_bkt(sn->brent_phase,  "BRENT");
            add_bkt(sn->xag_phase,    "XAGUSD");
            add_bkt(sn->eurusd_phase, "EURUSD");
            add_bkt(sn->gbpusd_phase, "GBPUSD");
            add_bkt(sn->audusd_phase, "AUDUSD");
            add_bkt(sn->nzdusd_phase, "NZDUSD");
            add_bkt(sn->usdjpy_phase, "USDJPY");
            add_bkt(sn->bkt_sp.phase,    "US500.F");
            add_bkt(sn->bkt_nq.phase,    "USTEC.F");
            add_bkt(sn->bkt_us30.phase,  "DJ30.F");
            add_bkt(sn->bkt_nas.phase,   "NAS100");
            add_bkt(sn->bkt_ger.phase,   "GER40");
            add_bkt(sn->bkt_uk.phase,    "UK100");
            add_bkt(sn->bkt_estx.phase,  "ESTX50");
            add_bkt(sn->bkt_xag.phase,   "XAGUSD");
            add_bkt(sn->bkt_gold.phase,  "XAUUSD");
            add_bkt(sn->bkt_eur.phase,   "EURUSD");
            add_bkt(sn->bkt_gbp.phase,   "GBPUSD");
            add_bkt(sn->bkt_brent.phase, "BRENT");
        }
        g_telemetry.UpdateExposure(exp_us, exp_eu, exp_oil, exp_metals, exp_jpy, exp_egbp);
    }

    if (g_telemetry.snap()) g_telemetry.snap()->uptime_sec =
        static_cast<int64_t>(std::time(nullptr)) - g_start_time;
}  // ? on_tick
// ?????????????????????????????????????????????????????????????????????????????
static std::vector<std::string> extract_messages(const char* data, int n, bool reset = false) {
    static std::string recv_buf;  // local static -- no global sharing
    if (reset) { recv_buf.clear(); return {}; }
    recv_buf.append(data, static_cast<size_t>(n));
    std::vector<std::string> msgs;
    while (true) {
        const size_t bs = recv_buf.find("8=FIX");
        if (bs == std::string::npos) { recv_buf.clear(); break; }
        if (bs > 0u) recv_buf = recv_buf.substr(bs);
        const size_t bl_pos = recv_buf.find("\x01" "9=");
        if (bl_pos == std::string::npos) break;
        const size_t bl_start = bl_pos + 3u;
        const size_t bl_end   = recv_buf.find('\x01', bl_start);
        if (bl_end == std::string::npos) break;
        int body_len = 0;
        try { body_len = std::stoi(recv_buf.substr(bl_start, bl_end - bl_start)); }
        catch (...) { recv_buf = recv_buf.substr(bl_end); continue; } // malformed -- skip
        const size_t hdr_end  = bl_end + 1u;
        const size_t msg_end  = hdr_end + static_cast<size_t>(body_len) + 7u;
        if (msg_end > recv_buf.size()) break;
        msgs.push_back(recv_buf.substr(0u, msg_end));
        recv_buf = recv_buf.substr(msg_end);
    }
    return msgs;
}

// ?????????????????????????????????????????????????????????????????????????????
// FIX dispatch
// ?????????????????????????????????????????????????????????????????????????????
#include "fix_dispatch.hpp"
static void trade_loop() {
    int backoff_ms = 1000;
    const int max_backoff = 30000;

    while (g_running.load()) {
        std::cout << "[OMEGA-TRADE] Connecting " << g_cfg.host << ":" << g_cfg.trade_port << "\n";

        int sock = -1;
        SSL* ssl = connect_ssl(g_cfg.host, g_cfg.trade_port, sock);
        if (!ssl) {
            std::cerr << "[OMEGA-TRADE] Connect failed -- retry " << backoff_ms << "ms\n";
            for (int i = 0; i < backoff_ms / 10 && g_running.load(); ++i) Sleep(10);
            backoff_ms = std::min(backoff_ms * 2, max_backoff);
            continue;
        }

        backoff_ms = 1000;
        g_trade_seq = 1;
        g_trade_ready.store(false);  // clear before logon -- previous session may have left it true

        // Send trade logon
        const std::string logon = fix_build_logon(g_trade_seq++, "TRADE");
        SSL_write(ssl, logon.c_str(), static_cast<int>(logon.size()));
        std::cout << "[OMEGA-TRADE] Logon sent\n";

        // Store globally for order submission
        {
            std::lock_guard<std::mutex> lk(g_trade_mtx);
            g_trade_ssl  = ssl;
            g_trade_sock = sock;
        }

        // Read loop -- heartbeats + logon ACK only on trade session
        std::string trade_recv_buf;
        auto last_ping      = std::chrono::steady_clock::now();
        auto logon_sent_at  = std::chrono::steady_clock::now();


        while (g_running.load()) {
            const auto now = std::chrono::steady_clock::now();

            // Logon timeout: if no LOGON ACCEPTED within 10s, drop and reconnect
            if (!g_trade_ready.load() &&
                std::chrono::duration_cast<std::chrono::seconds>(now - logon_sent_at).count() >= 10) {
                std::cerr << "[OMEGA-TRADE] Logon timeout (10s) -- reconnecting\n";
                break;
            }

            // Proactive 13min reconnect REMOVED.
            // Was added to handle "32 drops at 15min intervals" -- those drops
            // were caused by d7a0a16's L2 auto-restart, not a broker timeout.
            // With L2 restart removed, FIX session stays up indefinitely.

            // Heartbeat every 30s
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_ping).count() >= g_cfg.heartbeat) {
                last_ping = now;
                const std::string hb = build_heartbeat(g_trade_seq++, "TRADE");
                std::lock_guard<std::mutex> lk(g_trade_mtx);
                if (SSL_write(ssl, hb.c_str(), static_cast<int>(hb.size())) <= 0) break;
            }

            char buf[4096];
            const int n = SSL_read(ssl, buf, static_cast<int>(sizeof(buf)) - 1);
            if (n <= 0) {
                const int err = SSL_get_error(ssl, n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    Sleep(1); continue;
                }
                // SO_RCVTIMEO timeout fires as SSL_ERROR_SYSCALL + WSAETIMEDOUT --
                // not a real disconnect, just no data in 200ms window
                if (err == SSL_ERROR_SYSCALL && WSAGetLastError() == WSAETIMEDOUT) {
                    continue;
                }
                std::cerr << "[OMEGA-TRADE] SSL error " << err << " -- reconnecting\n";
                break;
            }
            trade_recv_buf.append(buf, static_cast<size_t>(n));

            // Parse messages from trade session
            while (true) {
                const size_t bs = trade_recv_buf.find("8=FIX");
                if (bs == std::string::npos) { trade_recv_buf.clear(); break; }
                if (bs > 0) trade_recv_buf = trade_recv_buf.substr(bs);
                const size_t bl_pos = trade_recv_buf.find("\x01" "9=");
                if (bl_pos == std::string::npos) break;
                const size_t bl_start = bl_pos + 3u;
                const size_t bl_end   = trade_recv_buf.find('\x01', bl_start);
                if (bl_end == std::string::npos) break;
                int body_len = 0;
                try { body_len = std::stoi(trade_recv_buf.substr(bl_start, bl_end - bl_start)); }
                catch (...) { trade_recv_buf = trade_recv_buf.substr(bl_end); continue; }
                const size_t hdr_end = bl_end + 1u;
                const size_t msg_end = hdr_end + static_cast<size_t>(body_len) + 7u;
                if (msg_end > trade_recv_buf.size()) break;
                const std::string tmsg = trade_recv_buf.substr(0u, msg_end);
                trade_recv_buf = trade_recv_buf.substr(msg_end);

                const std::string ttype = extract_tag(tmsg, "35");
                if (ttype == "A") {
                    g_trade_ready.store(true);
                    std::cout << "[OMEGA-TRADE] LOGON ACCEPTED\n";
                    const std::string req_id = "omega-sec-" + std::to_string(nowSec());
                    const std::string sec_req = fix_build_security_list_request(g_trade_seq++, req_id);
                    SSL_write(ssl, sec_req.c_str(), static_cast<int>(sec_req.size()));
                    std::cout << "[OMEGA-TRADE] SecurityListRequest sent req_id=" << req_id << "\n";
                } else if (ttype == "8") {
                    // ExecutionReport -- order ACK / fill / reject
                    handle_execution_report(tmsg);
                } else if (ttype == "y") {
                    const auto entries = parse_security_list_entries(tmsg);
                    if (!entries.empty()) {
                        const bool ext_changed = apply_security_list_symbol_map(entries);
                        const std::string req_id = extract_tag(tmsg, "320");
                        std::cout << "[OMEGA-TRADE] SecurityList received req_id="
                                  << (req_id.empty() ? "?" : req_id)
                                  << " entries=" << entries.size();
                        if (ext_changed) std::cout << " (ext IDs updated -- will re-subscribe)";
                        std::cout << "\n";
                        std::cout.flush();
                        if (ext_changed) g_ext_md_refresh_needed.store(true);
                    }
                } else if (ttype == "5") {
                    std::cout << "[OMEGA-TRADE] Logout received\n";
                    break;
                } else if (ttype == "3" || ttype == "j") {
                    std::string r = tmsg.substr(0, 300);
                    for (char& c : r) if (c == '\x01') c = '|';
                    std::cerr << "[OMEGA-TRADE] REJECT type=" << ttype
                              << " text=" << extract_tag(tmsg, "58") << "\n";
                }
                // Heartbeats (type=0) and TestRequests (type=1) silently absorbed
            }
        }

        // Tear down: SO_SNDTIMEO (500ms, set in connect_ssl) ensures SSL_write
        // returns within 500ms max even if the server is slow to ACK.
        // DO NOT set FIONBIO here -- non-blocking mode causes SSL_write to return
        // WANT_WRITE immediately (0 bytes sent) which silently drops the logout.
        // SO_SNDTIMEO is the correct mechanism; FIONBIO defeats it.
        if (g_trade_ready.load()) {
            const std::string tlo = fix_build_logout(g_trade_seq++, "TRADE");
            SSL_write(ssl, tlo.c_str(), static_cast<int>(tlo.size()));
            std::cout << "[OMEGA-TRADE] Logout sent\n";
        }
        g_trade_ready.store(false);
        {
            std::lock_guard<std::mutex> lk(g_trade_mtx);
            g_trade_ssl  = nullptr;
            g_trade_sock = -1;
        }
        Sleep(150);  // let kernel flush logout to wire before closing
        // Close the socket FIRST -- SSL_free then finds a dead socket and
        // returns immediately rather than attempting any I/O.
        if (sock >= 0) closesocket(static_cast<SOCKET>(sock));
        SSL_free(ssl);

        // Reconnect after any disconnect
        std::cerr << "[OMEGA-TRADE] Disconnected -- reconnecting\n";
        for (int i = 0; i < 200 && g_running.load(); ++i) Sleep(10);
    }
    g_trade_thread_done.store(true);  // signal main() that trade_loop has fully exited
}

#include "quote_loop.hpp"
int main(int argc, char* argv[])
{
    g_singleton_mutex = CreateMutexA(NULL, TRUE, "Global\\Omega_Breakout_System");
    if (!g_singleton_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cout << "[OMEGA] ALREADY RUNNING -- another Omega instance holds the mutex. Exiting.\n";
        std::cerr << "[OMEGA] ALREADY RUNNING -- another Omega instance holds the mutex. Exiting.\n";
        std::cout.flush(); std::cerr.flush();
        if (g_singleton_mutex) { CloseHandle(g_singleton_mutex); g_singleton_mutex = nullptr; }
        Sleep(2000);  // keep window open long enough to read
        return 1;
    }

    SetConsoleOutputCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0; GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    std::cout << "\033[1;36m"
              << "=======================================================\n"
              << "  OMEGA  |  Commodities & Indices  |  Breakout System  \n"
              << "=======================================================\n"
              << "  Build:   " << OMEGA_VERSION << "  (" << OMEGA_BUILT << ")\n"
              << "  Commit:  " << OMEGA_COMMIT  << "\n"
              << "=======================================================\n"
              << "\033[0m";
    // Also print to stderr so it's visible even if stdout is redirected
    std::fprintf(stderr, "[OMEGA] version=%s built=%s\n", OMEGA_VERSION, OMEGA_BUILT);

    std::signal(SIGINT,  sig_handler);
    std::signal(SIGTERM, sig_handler);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);  // handles X button, CTRL_CLOSE, shutdown

    // Resolve config path: explicit arg > cwd\omega_config.ini > config\omega_config.ini
    std::string cfg_path = "omega_config.ini";
    if (argc > 1) {
        cfg_path = argv[1];
    } else {
        std::ifstream test_cwd("omega_config.ini");
        if (!test_cwd.is_open()) cfg_path = "config\\omega_config.ini";
    }
    load_config(cfg_path);
    sanitize_config();
    apply_shadow_research_profile();
    // Per-symbol typed overloads -- each applies instrument-specific params + macro context ptr
    apply_engine_config(g_eng_sp);   // [sp] section: tp=0.60%, sl=0.35%, vol=0.04%, regime-gated
    apply_engine_config(g_eng_nq);   // [nq] section: tp=0.70%, sl=0.40%, vol=0.05%, regime-gated
    apply_engine_config(g_eng_cl);   // [oil] section: tp=1.20%, sl=0.60%, vol=0.08%, inventory-blocked
    apply_engine_config(g_eng_us30); // typed Us30Engine: macro-gated like SP/NQ
    apply_engine_config(g_eng_nas100); // typed Nas100Engine: macro-gated, independent from USTEC.F
    apply_generic_index_config(g_eng_ger30);
    apply_generic_index_config(g_eng_uk100);
    apply_generic_index_config(g_eng_estx50);
    apply_generic_silver_config(g_eng_xag);
    // Bracket engines -- configure() with tuned production params.
    // buffer, lookback, RR, cooldown_ms, MIN_RANGE, CONFIRM_MOVE, confirm_timeout_ms, min_hold_ms
    g_bracket_gold.configure(
        0.8,    // buffer: place orders 0.8pts outside the range
        20,     // DATA-CALIBRATED lookback: 20 ticks. Grid on 718k bars: LB=20 R=$1.5-$12 RR=4x
                //   yields $3,817/2yr vs LB=40 $2,952/2yr (+$433/yr). Confirmed Jan2024-Jan2026.
        4.0,    // DATA-CALIBRATED RR: 4.0x SL. Best on 2yr tick data ($38k profit).
                //   TP = 4x the structure range. Median range $2.67 ? TP ~$10.68
        90000,  // cooldown_ms: 90s
        1.5,    // DATA-CALIBRATED MIN_RANGE: $1.5. Allows small but real compressions.
                //   Brute-force showed $1.5-$12 range captures $38k vs $5 min misses most signals.
        0.05,   // CONFIRM_MOVE static fallback
        4000,   // confirm_timeout_ms
        12000,  // min_hold_ms
        0.0,    // VWAP_MIN_DIST: removed.
        30000,  // MIN_STRUCTURE_MS
        25000,  // FAILURE_WINDOW_MS
        20,     // ATR_PERIOD
        0.15,   // ATR_CONFIRM_K
        2.0,    // ATR_RANGE_K
        0.8,    // SLIPPAGE_BUFFER
        1.5     // EDGE_MULTIPLIER
    );
    // XAGUSD (~$65): daily range $3.5, typical compression $0.30, spread $0.08
    // Silver amplifies gold -- same cascade logic, same cooldown, trail rides $3-5 weekly moves.
    // Trail at 3R: comp=$0.30, trail_dist=$0.075 -- very tight, holds through volatile moves.
    g_bracket_xag.configure(
        0.04,   // buffer: spread*0.5 = $0.04 outside range
        30,     // lookback: 30-tick structural window
        3.0,    // RR: 3.0 matches gold. On $0.30 compression: trail arms at $0.90 in.
                //   On a $3.50 weekly move: 10R+ captured via trail.
        30000,  // cooldown_ms: 30s -- silver cascades same as gold, re-arm fast.
        0.40,   // MIN_RANGE: $0.40 minimum raw structural range.
                //   At $64, $0.40 = 0.63% -- real compression, not tick noise.
                //   Old value was $0.15 which fired on $0.18 structures that
                //   were swept in seconds. Raw range check in arm_both_sides
                //   now enforces this against the spread-padded dist.
        0.06,   // CONFIRM_MOVE
        4000,   // confirm_timeout_ms
        8000,   // min_hold_ms
        0.0,    // VWAP_MIN_DIST: removed -- silver near VWAP pre-breakout by definition.
        20000,  // MIN_STRUCTURE_MS: 20s
        12000,  // FAILURE_WINDOW_MS: 12s -- silver sweeps slightly faster than gold.
        20,     // ATR_PERIOD
        0.17,   // ATR_CONFIRM_K
        1.4,    // ATR_RANGE_K
        0.08,   // SLIPPAGE_BUFFER: $0.08 matches typical silver spread
        1.5     // EDGE_MULTIPLIER
    );
    // Wire shadow fill simulation -- price-triggered in PENDING, not immediate at arm
    g_bracket_gold.shadow_mode = (g_cfg.mode != "LIVE");
    g_bracket_xag.shadow_mode  = (g_cfg.mode != "LIVE");
    // PENDING_TIMEOUT_SEC: gold/silver compress for minutes before breaking -- 60s was expiring before the move
    g_bracket_gold.PENDING_TIMEOUT_SEC = 600;  // 10 min: gold compression can last well beyond 5 min
    g_bracket_xag.PENDING_TIMEOUT_SEC  = 300;  // 5 min: silver moves faster than gold
    // MIN_BREAK_TICKS: sweep guard -- price must stay inside the bracket for N consecutive
    // ticks before orders are sent. Catches London open liquidity sweeps (07:00:34 SHORT
    // -$7.97): bracket range $7.80 was exactly one sweep wide, SHORT filled in 1 tick
    // then price snapped back $7.80 to SL in 16s. 3 ticks ~= 0.3-0.6s -- long enough to
    // distinguish a single-tick spike from genuine compression holding at the boundary.
    // Silver also benefits: London open sweep pattern identical, slightly faster ticks.
    g_bracket_gold.MIN_BREAK_TICKS = 3;
    g_bracket_xag.MIN_BREAK_TICKS  = 3;
    // ATR-based dynamic minimum range: eff_min_range = max(recent_noise * ATR_RANGE_K, MIN_RANGE)
    // Prevents brackets arming when the market noise floor exceeds the bracket width --
    // the SL then sits inside normal noise and gets swept without a real move.
    //
    // ATR_PERIOD=20 ticks: at London open ~5-15 ticks/sec = 1-4s of recent price action.
    // This captures the current noise floor, not historical vol. Updates every tick.
    //
    // Gold: London noise $8-20. MIN_RANGE=6. ATR_RANGE_K=1.5 ? when noise=$12, eff_min=18.
    //   A $10 bracket in $12 noise is invalid -- SL would be swept immediately.
    //   A $20 bracket in $12 noise is valid -- genuine compression above noise.
    // Silver: proportionally similar (~$0.10-0.40 noise). ATR_RANGE_K=1.5.
    // FX (EURUSD etc.): noise ~0.0003-0.0008. ATR_RANGE_K=1.8 (tighter price, more sensitive).
    // Gold bracket: ATR_RANGE_K=0 disables ATR floor -- use MIN_RANGE=1.0pt directly.
    // With VIX at 24 gold compresses to 1-3pt ranges which ATR_RANGE_K=1.5 rejects (needs 15pt).
    // The bracket window itself defines the range -- 1pt minimum just filters single-tick noise.
    g_bracket_gold.ATR_PERIOD  = 20;  g_bracket_gold.ATR_RANGE_K  = 0.0;
    g_bracket_gold.MIN_RANGE   = 1.0;
    g_bracket_xag.ATR_PERIOD   = 20;  g_bracket_xag.ATR_RANGE_K   = 1.5;
    g_bracket_eurusd.ATR_PERIOD = 20; g_bracket_eurusd.ATR_RANGE_K = 1.8;
    g_bracket_gbpusd.ATR_PERIOD = 20; g_bracket_gbpusd.ATR_RANGE_K = 1.8;
    g_bracket_audusd.ATR_PERIOD = 20; g_bracket_audusd.ATR_RANGE_K = 1.8;
    g_bracket_nzdusd.ATR_PERIOD = 20; g_bracket_nzdusd.ATR_RANGE_K = 1.8;
    g_bracket_usdjpy.ATR_PERIOD = 20; g_bracket_usdjpy.ATR_RANGE_K = 1.8;
    // Indices: leave ATR disabled -- noise floor more stable, fixed MIN_RANGE sufficient
    // MAX_RANGE: prevents bracketing full trending session moves instead of real compression
    // Gold at $4400: 0.4% = $17.6 max range. Tight compression is $8-16. Day range is $40-120.
    g_bracket_gold.MAX_RANGE   = 12.0;   // DATA-CALIBRATED: $12 max. Ranges >$12 are trending, not bracketing.
    // Silver at $68: 0.4% = $0.27 max range. Compression = $0.15-0.25. Day range = $1-3.
    g_bracket_xag.MAX_RANGE    = 0.30;   // ~0.44% of silver ~$68
    // Configure opening range engines
    g_orb_us.OPEN_HOUR    = 13; g_orb_us.OPEN_MIN    = 30;  // NY open 13:30 UTC
    g_orb_ger30.OPEN_HOUR = 8;  g_orb_ger30.OPEN_MIN = 0;   // Xetra open 08:00 UTC
    g_orb_silver.OPEN_HOUR= 13; g_orb_silver.OPEN_MIN= 30;  // COMEX open 13:30 UTC
    // New ORB instruments: LSE and Euronext with tighter 15-min range windows
    g_orb_uk100.OPEN_HOUR  = 8;  g_orb_uk100.OPEN_MIN  = 0;   // LSE open 08:00 UTC
    g_orb_uk100.RANGE_WINDOW_MIN = 15;  // 15-min range (LSE moves fast at open)
    g_orb_uk100.TP_PCT  = 0.12;  g_orb_uk100.SL_PCT  = 0.07;  // UK100 TP/SL calibrated to GBP volatility
    g_orb_estx50.OPEN_HOUR = 9;  g_orb_estx50.OPEN_MIN = 0;   // Euronext open 09:00 UTC
    g_orb_estx50.RANGE_WINDOW_MIN = 15; // 15-min range
    g_orb_estx50.TP_PCT = 0.10;  g_orb_estx50.SL_PCT = 0.06;  // ESTX50 TP/SL similar to GER40
    // VWAPReversionEngine params -- per-instrument tuning
    // Indices: 0.20% extension threshold, 180s cooldown (fast mean-reversion)
    g_vwap_rev_sp.EXTENSION_THRESH_PCT    = 0.20; g_vwap_rev_sp.COOLDOWN_SEC    = 180;
    g_vwap_rev_nq.EXTENSION_THRESH_PCT    = 0.20; g_vwap_rev_nq.COOLDOWN_SEC    = 180;
    g_vwap_rev_ger40.EXTENSION_THRESH_PCT = 0.20; g_vwap_rev_ger40.COOLDOWN_SEC = 180;
    // EURUSD: 0.12% extension threshold (FX moves more precisely, smaller range)
    g_vwap_rev_eurusd.EXTENSION_THRESH_PCT = 0.12; g_vwap_rev_eurusd.COOLDOWN_SEC = 120;
    // ?? NBM London session engines (07:00-13:30 UTC) ????????????????????????????
    // Covers the gap before NY open. Gold and oil are liquid from London open.
    // Uses same ATR/band logic as NY engines but anchored to London open price.
    g_nbm_gold_london.SESSION_OPEN_UTC  =  7;  g_nbm_gold_london.SESSION_OPEN_MIN  =  0;
    g_nbm_gold_london.SESSION_CLOSE_UTC = 13;  g_nbm_gold_london.SESSION_CLOSE_MIN = 30;
    g_nbm_gold_london.MAX_SPREAD_PCT    = 0.02;  // gold spread tighter than indices
    g_nbm_gold_london.WARMUP_TICKS      = 120;
    g_nbm_gold_london.COOLDOWN_SEC      = 600;

    g_nbm_oil_london.SESSION_OPEN_UTC  =  7;  g_nbm_oil_london.SESSION_OPEN_MIN  =  0;
    g_nbm_oil_london.SESSION_CLOSE_UTC = 13;  g_nbm_oil_london.SESSION_CLOSE_MIN = 30;
    g_nbm_oil_london.MAX_SPREAD_PCT    = 0.05;
    g_nbm_oil_london.WARMUP_TICKS      = 120;
    g_nbm_oil_london.COOLDOWN_SEC      = 600;

    // TrendPullbackEngine params -- per-instrument tuning
    //
    // GOLD (M15 bar EMAs -- seeded from g_bars_gold.m15):
    //   EMAs come from OHLCBarEngine on 50 M15 bars:
    //     EMA9  half-life = 3.1 bars = 47 min
    //     EMA21 half-life = 7.3 bars = 109 min
    //     EMA50 half-life = 17.3 bars = 260 min = 4.3 hours
    //
    //   PULLBACK_BAND_PCT: the critical setting.
    //   Old value (0.08%) = ±3.7pts at $4700. With M15 EMA50 representing
    //   4.3h weighted average, price is typically 10-30pts from EMA50 during
    //   an active trend. 0.08% = never fires. Must be wide enough to detect
    //   genuine pullbacks TO the M15 EMA50 level.
    //   0.50% = ±23.5pts at $4700. Fires when price is within ~24pts of
    //   the M15 EMA50 -- correct for pullback-to-slow-average strategy.
    //   Upper bound: 1.0% = ±47pts -- too loose, fires mid-trend constantly.
    //
    //   COOLDOWN_SEC: M15 trade = swing trade, minimum 1 M15 bar (15min)
    //   between signals. 900s (15min) = 1 full M15 bar -- prevents rapid
    //   re-entry on the same pullback level.
    //
    //   TRAIL_ARM/DIST: ATR from M15 bars = 4-8pts typical. 2x arm = 8-16pts
    //   before trailing. 1x dist = 4-8pt trail. Correct for swing scale.
    //   Leave at class defaults (2.0x arm, 1.0x dist, 1.0x BE).
    //
    //   BE_ATR_MULT: lock BE at 1x M15 ATR (~5pts). Unchanged -- good.
    g_trend_pb_gold.PULLBACK_BAND_PCT  = 0.50;  // M15: ±23.5pts at $4700. Old 0.08% (±3.7pts) never fired.
    g_trend_pb_gold.COOLDOWN_SEC       = 60;    // 60s cooldown -- reduced from 900s (15min was insane, missed 100pt moves)
    g_trend_pb_gold.MIN_EMA_SEP        = 5.0;   // gold: 5pt EMA9-EMA50 separation = real trend
    g_trend_pb_gold.H4_GATE_ENABLED    = true;  // gate M15 entries on H4 trend direction
    g_trend_pb_gold.ATR_SL_MULT        = 1.2;   // SL floor = 1.2x M15 ATR (adaptive, not fixed 8pt)
    // Improvement 1: vol regime sizing
    g_trend_pb_gold.VOL_SCALE_HIGH_MULT = 1.5;
    g_trend_pb_gold.VOL_SCALE_LOW_MULT  = 0.7;
    g_trend_pb_gold.VOL_SCALE_CUT       = 0.60;
    g_trend_pb_gold.VOL_SCALE_BOOST     = 1.20;
    // Improvement 2: daily loss cap -- stop gold TrendPB after $150 loss in a day
    g_trend_pb_gold.DAILY_LOSS_CAP      = 150.0;
    // Improvement 4: time-of-day weighting
    g_trend_pb_gold.TOD_WEIGHT_ENABLED  = true;
    // Improvement 5: CVD gate
    g_trend_pb_gold.CVD_GATE_ENABLED    = true;
    // Improvement 7: news SL widening
    g_trend_pb_gold.NEWS_WARN_SECS      = 900;   // 15min before event
    g_trend_pb_gold.NEWS_SL_MULT        = 1.5;
    // Widen pullback band: 0.15% -> 0.50% (±23pts at 4620)
    // Default 0.15% = ±6.9pts. On a $20 trending move price is 20pts from EMA50
    // and never enters the band -- engine silent on all clean trends.
    // 0.50% allows entry when price is trending away from EMA50 but still directional.
    g_trend_pb_gold.PULLBACK_BAND_PCT   = 0.50;
    // Improvement 8: pyramid on second pullback
    g_trend_pb_gold.PYRAMID_ENABLED     = true;
    g_trend_pb_gold.PYRAMID_SIZE_MULT   = 0.5;
    g_trend_pb_gold.PYRAMID_MAX_ADDS    = 1;
    // Trail/BE params: class defaults are correct for M15 ATR scale (4-8pts)
    // TRAIL_ARM_ATR_MULT=2.0, TRAIL_DIST_ATR_MULT=1.0, BE_ATR_MULT=1.0 -- no change needed
    // GER40: tighter band (index moves more cleanly around EMAs)
    g_trend_pb_ger40.PULLBACK_BAND_PCT = 0.05;  // 0.05% of GER40 = ~11pts at 22500
    g_trend_pb_ger40.COOLDOWN_SEC     = 120;
    g_trend_pb_ger40.MIN_EMA_SEP      = 15.0;
    // NQ/SP TrendPullback: daily loss cap + tighter controls
    // Without DAILY_LOSS_CAP, NQ TrendPullback fired 7 consecutive losing entries
    // during the Apr 2 tariff crash (NQ dropped ~1000pts). Each SL hit was $12-13,
    // but the direction block (2 consec SL hits = 10min pause) was the only guard.
    // Daily loss cap stops the engine entirely after a bad sequence.
    g_trend_pb_nq.MIN_EMA_SEP         = 25.0;
    g_trend_pb_nq.DAILY_LOSS_CAP      = 80.0;   // $80 daily cap: ~6 SL hits at $12 each
    g_trend_pb_sp.MIN_EMA_SEP         = 15.0;
    g_trend_pb_sp.DAILY_LOSS_CAP      = 80.0;   // same cap for SP
    g_trend_pb_ger40.DAILY_LOSS_CAP   = 80.0;   // GER40 too -- no cap was previously set
    // Load warm EMA state -- skips EMA_WARMUP_TICKS cold period on restart
    g_trend_pb_gold.load_state(log_root_dir()  + "/trend_pb_gold.dat");
    g_trend_pb_ger40.load_state(log_root_dir() + "/trend_pb_ger40.dat");
    g_trend_pb_nq.load_state(log_root_dir()    + "/trend_pb_nq.dat");
    g_trend_pb_sp.load_state(log_root_dir()    + "/trend_pb_sp.dat");

    // ?? Nuke stale ctrader_bar_failed.txt on every startup ??????????????????
    // Old binaries wrote M5/M15 periods (5/7) and BOM-prefixed keys into this
    // file, permanently blacklisting the tick-data fallback requests that seed
    // M15 bars on cold start. Effect: bars NEVER seed, GoldFlow blocked all day.
    // Fix: delete the file and rewrite it clean from the pre-seeded in-memory set.
    // The in-memory set is pre-seeded in ctrader setup (XAUUSD:1 and live subs).
    // Any valid entries that were on disk will be re-discovered naturally --
    // GetTrendbarsReq is already blocked in-memory. The only thing we lose is
    // session-specific crash history, which resets anyway on every restart.
    {
        const std::string failed_path = log_root_dir() + "/ctrader_bar_failed.txt";
        if (std::remove(failed_path.c_str()) == 0) {
            printf("[STARTUP] Deleted stale ctrader_bar_failed.txt -- clean slate for bar requests\n");
        }
        fflush(stdout);
    }

    // Load OHLCBarEngine indicator state -- instant warm restart, no tick data request needed.
    // If .dat files exist and are <24hr old: m1_ready=true immediately on first tick.
    // Bars update live from on_spot_event (M15 bar closes pushed by broker every 15min).
    // This eliminates the 2-minute GoldFlow bar gate delay on every restart.
    {
        const std::string base = log_root_dir();
        const bool m1_ok  = g_bars_gold.m1 .load_indicators(base + "/bars_gold_m1.dat");
        const bool m5_ok  = g_bars_gold.m5 .load_indicators(base + "/bars_gold_m5.dat");
        const bool m15_ok = g_bars_gold.m15.load_indicators(base + "/bars_gold_m15.dat");
        const bool h4_ok  = g_bars_gold.h4 .load_indicators(base + "/bars_gold_h4.dat");
        if (m15_ok) {
            // Immediately seed TrendPullback EMAs + ATR from M15 bar state
            g_trend_pb_gold.seed_bar_emas(
                g_bars_gold.m15.ind.ema9 .load(std::memory_order_relaxed),
                g_bars_gold.m15.ind.ema21.load(std::memory_order_relaxed),
                g_bars_gold.m15.ind.ema50.load(std::memory_order_relaxed),
                g_bars_gold.m15.ind.atr14.load(std::memory_order_relaxed));
            // M1 EMA crossover for bar trend gate -- loaded from disk, no 15-min warmup
            if (m1_ok) {
                const double st_e9  = g_bars_gold.m1.ind.ema9 .load(std::memory_order_relaxed);
                const double st_e50 = g_bars_gold.m1.ind.ema50.load(std::memory_order_relaxed);
                const int st_trend  = (st_e9 > 0.0 && st_e50 > 0.0)
                    ? (st_e9 < st_e50 ? -1 : +1) : 0;
                g_trend_pb_gold.seed_m5_trend(st_trend);
                printf("[STARTUP] M1 bar state loaded: EMA9=%.2f EMA50=%.2f RSI=%.1f trend=%+d"
                       " -- GoldFlow/GoldStack bar gates active immediately\n",
                       st_e9, st_e50,
                       g_bars_gold.m1.ind.rsi14.load(std::memory_order_relaxed),
                       st_trend);
            } else if (m5_ok) {
                // Fallback: seed trend from M5 if M1 not available
                const double st_e9  = g_bars_gold.m1.ind.ema9 .load(std::memory_order_relaxed);
                const double st_e50 = g_bars_gold.m1.ind.ema50.load(std::memory_order_relaxed);
                const int st_trend  = (st_e9 > 0.0 && st_e50 > 0.0)
                    ? (st_e9 < st_e50 ? -1 : +1) : 0;
                g_trend_pb_gold.seed_m5_trend(st_trend);
            }
            // Immediately seed H4 HTF trend gate -- no need to wait for first tick
            if (h4_ok) {
                g_trend_pb_gold.seed_h4_trend(
                    g_bars_gold.h4.ind.trend_state.load(std::memory_order_relaxed));
            }
            printf("[STARTUP] Bar state loaded: M1=%s M5=%s M15=%s H4=%s"
                   " EMA50=%.2f ATR=%.2f H4_trend=%d\n",
                   m1_ok?"ok":"cold", m5_ok?"ok":"cold", m15_ok?"ok":"cold", h4_ok?"ok":"cold",
                   g_bars_gold.m15.ind.ema50.load(std::memory_order_relaxed),
                   g_bars_gold.m15.ind.atr14.load(std::memory_order_relaxed),
                   g_bars_gold.h4.ind.trend_state.load(std::memory_order_relaxed));
        } else {
            printf("[STARTUP] No bar state on disk (cold start) -- 15min M1 warmup required\n");
        }
        fflush(stdout);
    }

    // Load macro event calendar for OmegaVolTargeter high_impact_window() gate.
    // File: config\macro_events_today.txt (same dir as omega_config.ini)
    // Format: "HH:MM CURRENCY HIGH LABEL"  e.g. "13:30 USD HIGH NFP"
    // Must be updated daily (manually or via a pre-open script).
    // Graceful: if file absent, high_impact_window() returns false (no blocking).
    {
        // Mirror config resolution: try cwd first, then config\ subdir
        const std::string ev_cwd = "macro_events_today.txt";
        const std::string ev_cfg = "config\\macro_events_today.txt";
        std::ifstream test(ev_cwd);
        g_vol_targeter.load_events(test.is_open() ? ev_cwd : ev_cfg);
    }

    // TrendPullback gold: M15 bar EMAs seeded live from g_bars_gold.m15 each tick.
    g_bracket_gold.cancel_order_fn = [](const std::string& id) { send_cancel_order(id); };
    g_bracket_xag.cancel_order_fn  = [](const std::string& id) { send_cancel_order(id); };

    // ?? Configure new bracket engines ????????????????????????????????????????
    // US equity indices: arms both sides, captures whichever direction breaks out
    g_bracket_sp.symbol     = "US500.F"; g_bracket_sp.ENTRY_SIZE     = 0.01;
    g_bracket_nq.symbol     = "USTEC.F"; g_bracket_nq.ENTRY_SIZE     = 0.01;
    g_bracket_us30.symbol   = "DJ30.F";  g_bracket_us30.ENTRY_SIZE   = 0.01;
    g_bracket_nas100.symbol = "NAS100";  g_bracket_nas100.ENTRY_SIZE = 0.01;
    g_bracket_ger30.symbol  = "GER40";   g_bracket_ger30.ENTRY_SIZE  = 0.01;
    g_bracket_uk100.symbol  = "UK100";   g_bracket_uk100.ENTRY_SIZE  = 0.01;
    g_bracket_estx50.symbol = "ESTX50";  g_bracket_estx50.ENTRY_SIZE = 0.01;
    g_bracket_brent.symbol  = "BRENT"; g_bracket_brent.ENTRY_SIZE  = 0.01;
    g_bracket_eurusd.symbol = "EURUSD";  g_bracket_eurusd.ENTRY_SIZE = 0.01;
    g_bracket_gbpusd.symbol = "GBPUSD";  g_bracket_gbpusd.ENTRY_SIZE = 0.01;
    g_bracket_audusd.symbol = "AUDUSD";  g_bracket_audusd.ENTRY_SIZE = 0.01;
    g_bracket_nzdusd.symbol = "NZDUSD";  g_bracket_nzdusd.ENTRY_SIZE = 0.01;

    // ?? MAX_RANGE caps: ~0.4% of instrument price ?????????????????????????????
    // Prevents bracketing full day-range trending moves as if they were compression.
    // Evidence: ESTX50 bracket fired on 79.8pt range (1.4% of 5600) = entire London
    // open move, not compression. Rule: MAX_RANGE ? 2? MIN_RANGE ? 0.4% of price.
    g_bracket_sp.MAX_RANGE      = 25.0;   // ~0.40% of SP ~6200
    g_bracket_nq.MAX_RANGE      = 90.0;   // ~0.40% of NQ ~22500
    g_bracket_us30.MAX_RANGE    = 180.0;  // ~0.40% of DJ30 ~45000
    g_bracket_nas100.MAX_RANGE  = 90.0;   // ~0.40% of NAS100 ~22500
    g_bracket_ger30.MAX_RANGE   = 90.0;   // ~0.40% of GER40 ~22500
    g_bracket_uk100.MAX_RANGE   = 40.0;   // ~0.40% of UK100 ~10000
    g_bracket_estx50.MAX_RANGE  = 22.0;   // ~0.40% of ESTX50 ~5500
    g_bracket_brent.MAX_RANGE   = 1.20;   // ~0.40% of Brent ~$90 (oil tight)
    g_bracket_eurusd.MAX_RANGE  = 0.0008; // ~0.07% of EURUSD ~1.15 (FX tight)
    g_bracket_gbpusd.MAX_RANGE  = 0.0010; // ~0.08% of GBPUSD ~1.33
    g_bracket_audusd.MAX_RANGE  = 0.0006; // ~0.09% of AUDUSD ~0.70
    g_bracket_nzdusd.MAX_RANGE  = 0.0006; // ~0.10% of NZDUSD ~0.60
    // USDJPY/GOLD/XAGUSD: MAX_RANGE set after their configure() calls below
    g_bracket_usdjpy.symbol = "USDJPY";  g_bracket_usdjpy.ENTRY_SIZE = 0.01;

    // ?? Bracket calibration -- March 2026 actual prices ?????????????????????????
    // All params derived from real price levels and observed daily ranges.
    // configure(buf, lookback, RR, cooldown_ms, min_range, cfm, ctout, min_hold_ms,
    //           vwap_dist, struct_ms, fail_win_ms, atr_per, atr_ck, atr_rk, slip_buf, edge_mult)
    //
    // RR=2.5 for equity indices -- trail kicks in at 2.5R, stepped SL locks gains
    // cooldown=60s for indices -- slightly choppier intraday than commodities
    // fail_win=10s for indices -- sweeps resolve faster on liquid index futures
    // vwap_dist=0 everywhere -- pre-breakout price near VWAP by definition
    //
    // US500.F (~$6,600): daily range $120, typical compression $8, spread $0.50
    // Index bracket engines -- MIN_RANGE calibrated to current 2026 price levels.
    // Rule: MIN_RANGE >= 0.20% of instrument price. Below that is tick noise.
    // At wrong (old) values: UK100 $4 @ 9720 = 0.041%, ESTX50 $3 @ 5387 = 0.056% -- pure noise.
    // Cooldown raised 60s?180s: 60s cooldown re-armed into same chop 1 min after SL hit.
    // MIN_STRUCTURE_MS raised 20s?30s: 20s is too short for real index compression.
    //
    // configure(buf, lookback, RR, cooldown_ms, MIN_RANGE, cfm, ctout, min_hold_ms,
    //           vwap_dist, MIN_STRUCTURE_MS, FAILURE_WINDOW_MS, atr_period,
    //           atr_confirm_k, atr_range_k, slippage_buffer, edge_multiplier)
    //
    // US500.F (~6000): 0.20% = $12.0 min range
    g_bracket_sp.configure(    0.25, 30, 2.5, 180000, 12.0, 0.05, 4000, 10000, 0.0, 30000, 10000, 20, 0.15, 2.0, 0.30, 1.5);
    // USTEC.F (~21000): 0.20% = $42.0 min range
    g_bracket_nq.configure(    0.75, 30, 2.5, 180000, 42.0, 0.05, 4000, 10000, 0.0, 45000, 10000, 20, 0.15, 2.0, 2.50, 1.5);
    // DJ30.F (~43000): 0.20% = $86.0 min range
    g_bracket_us30.configure(  2.50, 30, 2.5, 180000, 86.0, 0.05, 4000, 10000, 0.0, 30000, 10000, 20, 0.15, 2.0, 3.00, 1.5);
    // NAS100 (~21000): same as USTEC
    g_bracket_nas100.configure(0.75, 30, 2.5, 180000, 42.0, 0.05, 4000, 10000, 0.0, 30000, 10000, 20, 0.15, 2.0, 1.00, 1.5);
    // GER40 (~22000): 0.20% = $44.0 min range
    g_bracket_ger30.configure( 1.00, 30, 2.5, 180000, 44.0, 0.05, 4000, 10000, 0.0, 30000, 10000, 20, 0.15, 2.0, 1.00, 1.5);
    // UK100 (~9720): 0.20% = $19.5 min range
    g_bracket_uk100.configure( 0.50, 30, 2.5, 180000, 20.0, 0.05, 4000, 10000, 0.0, 30000, 10000, 20, 0.15, 2.0, 0.50, 1.5);
    // ESTX50 (~5387): 0.20% = $10.8 min range
    g_bracket_estx50.configure(0.50, 30, 2.5, 180000, 11.0, 0.05, 4000, 10000, 0.0, 30000, 10000, 20, 0.15, 2.0, 0.50, 1.5);
    g_bracket_brent.configure(
        0.10,   // buffer
        30,     // lookback
        2.5,    // RR: raised 1.8?2.5. Brent has $2-5 daily ranges -- trail captures multi-hour moves.
        45000,  // cooldown_ms: reduced 120s?45s. Slightly longer than gold (oil choppier intraday).
        0.5,    // MIN_RANGE -- 0.50pts minimum (unchanged)
        0.05,   // CONFIRM_MOVE
        4000,   // confirm_timeout_ms
        10000,  // min_hold_ms
        0.0,    // VWAP_MIN_DIST: removed -- same rationale as gold/silver
        20000,  // MIN_STRUCTURE_MS
        12000,  // FAILURE_WINDOW_MS: raised 5s?12s -- oil sweeps similar duration to silver
        20,     // ATR_PERIOD
        0.15,   // ATR_CONFIRM_K
        2.0,    // ATR_RANGE_K
        0.10,   // SLIPPAGE_BUFFER
        1.5     // EDGE_MULTIPLIER
    );
    // FX brackets -- calibrated for March 2026 price levels
    // RR=2.0 for FX -- tighter pip-based moves, trail at 2R
    // cooldown=45s -- FX compresses and re-compresses faster than commodities
    // fail_win=8s -- FX sweeps resolve quickly (highly liquid, tight spread)
    // vwap_dist=0 everywhere -- pre-breakout FX near session VWAP
    //
    // MIN_RANGE raised across all FX pairs vs original calibration:
    //   Old EURUSD 0.00035 (3.5 pip) armed on 1-pip Asian tape noise ? junk bracket
    //   New 0.00060 (6 pip): genuine compression; below that is spread-level noise
    // MIN_STRUCTURE_MS raised 20s?45s: FX needs longer confirmation in thin tape
    // EDGE_MULTIPLIER raised 1.5?2.0: TP must cover 2? total round-trip cost
    //
    // EURUSD (~1.156): daily range ~100 pips, compression ~7 pips, spread ~1.4 pip
    g_bracket_eurusd.configure(0.00007, 30, 2.0, 45000, 0.00060, 0.05, 4000, 8000, 0.0, 45000, 8000, 20, 0.15, 2.0, 0.00010, 2.0);
    // GBPUSD (~1.330): daily range ~120 pips, compression ~8 pips, spread ~1.8 pip
    g_bracket_gbpusd.configure(0.00009, 30, 2.0, 45000, 0.00070, 0.05, 4000, 8000, 0.0, 45000, 8000, 20, 0.15, 2.0, 0.00012, 2.0);
    // AUDUSD (~0.701): daily range ~80 pips, compression ~5 pips, spread ~1.2 pip
    g_bracket_audusd.configure(0.00006, 30, 2.0, 45000, 0.00050, 0.05, 4000, 8000, 0.0, 45000, 8000, 20, 0.15, 2.0, 0.00008, 2.0);
    // NZDUSD (~0.583): similar to AUD, slightly wider spread
    g_bracket_nzdusd.configure(0.00007, 30, 2.0, 45000, 0.00050, 0.05, 4000, 8000, 0.0, 45000, 8000, 20, 0.15, 2.0, 0.00009, 2.0);
    // USDJPY (~149.5): daily range ~120 pips, compression ~25 pips, spread ~4 pip
    // MIN_RANGE raised 0.12?0.20 (2 pips)
    g_bracket_usdjpy.configure(0.02,    30, 2.0, 45000, 0.20,    0.05, 4000, 8000, 0.0, 45000, 8000, 20, 0.15, 2.0, 0.04,    2.0);
    g_bracket_usdjpy.MAX_RANGE = 0.60;   // ~0.40% of USDJPY ~150

    // Shadow mode + cancel wiring for all new bracket engines
    const bool shadow = (g_cfg.mode != "LIVE");
    auto wire_bracket = [&](auto& beng, int pending_timeout_sec = 180) {
        beng.shadow_mode         = shadow;
        beng.PENDING_TIMEOUT_SEC = pending_timeout_sec;
        beng.cancel_order_fn     = [](const std::string& id) { send_cancel_order(id); };
    };
    // Pending timeouts calibrated per symbol:
    // Commodities: longer compression periods before breaking (5-10 min)
    // Equity indices: faster breakouts (3-5 min)
    // FX: tightest -- often break within 2-3 min or reset
    wire_bracket(g_bracket_sp,      300);  // US500: 5min -- index compression holds ~3-5min
    wire_bracket(g_bracket_nq,      300);  // USTEC: 5min
    wire_bracket(g_bracket_us30,    300);  // DJ30:  5min
    wire_bracket(g_bracket_nas100,  300);  // NAS100: 5min
    wire_bracket(g_bracket_ger30,   300);  // GER40: 5min
    wire_bracket(g_bracket_uk100,   300);  // UK100: 5min
    wire_bracket(g_bracket_estx50,  300);  // ESTX50: 5min
    wire_bracket(g_bracket_brent,   480);  // Brent: 8min -- oil compresses longer than indices
    wire_bracket(g_bracket_eurusd,  180);  // EURUSD: 3min -- FX breaks fast or resets
    wire_bracket(g_bracket_gbpusd,  180);  // GBPUSD: 3min
    wire_bracket(g_bracket_audusd,  180);  // AUDUSD: 3min
    wire_bracket(g_bracket_nzdusd,  180);  // NZDUSD: 3min
    wire_bracket(g_bracket_usdjpy,  240);  // USDJPY: 4min -- JPY can compress longer
    apply_generic_fx_config(g_eng_eurusd);
    apply_generic_gbpusd_config(g_eng_gbpusd);
    apply_generic_audusd_config(g_eng_audusd);
    apply_generic_nzdusd_config(g_eng_nzdusd);
    apply_generic_usdjpy_config(g_eng_usdjpy);
    apply_generic_brent_config(g_eng_brent);

    // ?? SymbolConfig override -- load symbols.ini, apply per-symbol params ?????
    // Loads on top of the apply_* defaults above. Every symbol independently
    // tunable from symbols.ini with no shared inheritance.
    {
        // Try multiple paths -- binary may run from different working directories
        const std::vector<std::string> sym_ini_candidates = {
            "symbols.ini",
            "C:\\Omega\\symbols.ini",
            "C:\\Omega\\config\\symbols.ini",
        };
        std::string sym_ini;
        for (const auto& candidate : sym_ini_candidates) {
            if (g_sym_cfg.load(candidate)) { sym_ini = candidate; break; }
        }
        if (!sym_ini.empty()) {
            // Helper: apply SymbolConfig to a BreakoutEngine.
            // TP_PCT = SL_PCT * tp_mult (scales TP relative to existing SL).
            // MAX_SPREAD: symbols.ini stores absolute price units. BreakoutEngine
            // uses MAX_SPREAD_PCT (% of mid). Convert: pct = abs / typical_price * 100.
            // Typical prices: FX ~1-150, indices ~4000-50000, oil ~80-100.
            // For FX the absolute spread (e.g. 0.0002) divided by price (~1.1) * 100 = 0.018% -- correct.
            // For indices (e.g. 2.5 pts / 6000) * 100 = 0.042% -- correct.
            // So we store the raw abs value and let the caller provide typical price.
            // Simpler: we know the instrument types -- pass typical_price per symbol.
            auto apply_be = [](auto& eng, const SymbolConfig& c, double typical_price) {
                if (c.sl_mult > 0.0 && c.sl_mult != 1.0) eng.SL_PCT *= c.sl_mult;
                if (c.tp_mult > 0.0)                      eng.TP_PCT  = eng.SL_PCT * c.tp_mult;
                if (c.max_spread > 0.0 && typical_price > 0.0)
                    eng.MAX_SPREAD_PCT = (c.max_spread / typical_price) * 100.0;
                if (c.min_hold_ms  > 0)   eng.MAX_HOLD_SEC  = c.min_hold_ms / 1000;
                if (c.max_hold_sec > 0)   eng.MAX_HOLD_SEC  = c.max_hold_sec;
                // Use >= 0 so MIN_EDGE_BP=0 (indices) actually disables the bp check.
                // > 0.0 left the default 6.0 in place when symbols.ini said 0.
                if (c.min_edge_bp    >= 0.0) eng.EDGE_CFG.min_edge_bp  = c.min_edge_bp;
                if (c.min_breakout_pct> 0.0) eng.MIN_BREAKOUT_PCT  = c.min_breakout_pct;
                if (c.min_range      > 0.0)  eng.MIN_COMP_RANGE    = c.min_range;
                if (c.min_confirm_ticks > 0) eng.MIN_CONFIRM_TICKS = c.min_confirm_ticks;
            };

            // BreakoutEngine symbols -- typical prices for MAX_SPREAD_PCT conversion
            apply_be(g_eng_sp,     g_sym_cfg.get("US500.F"), 6000.0);
            apply_be(g_eng_nq,     g_sym_cfg.get("USTEC.F"), 20000.0);
            apply_be(g_eng_cl,     g_sym_cfg.get("USOIL.F"), 80.0);
            apply_be(g_eng_us30,   g_sym_cfg.get("DJ30.F"),  42000.0);
            apply_be(g_eng_nas100, g_sym_cfg.get("NAS100"),  20000.0);
            apply_be(g_eng_ger30,  g_sym_cfg.get("GER40"),   22000.0);
            apply_be(g_eng_uk100,  g_sym_cfg.get("UK100"),   8500.0);
            apply_be(g_eng_estx50, g_sym_cfg.get("ESTX50"),  5300.0);
            apply_be(g_eng_xag,    g_sym_cfg.get("XAGUSD"),  30.0);
            apply_be(g_eng_eurusd, g_sym_cfg.get("EURUSD"),  1.10);
            apply_be(g_eng_gbpusd, g_sym_cfg.get("GBPUSD"),  1.27);
            apply_be(g_eng_audusd, g_sym_cfg.get("AUDUSD"),  0.65);
            apply_be(g_eng_nzdusd, g_sym_cfg.get("NZDUSD"),  0.60);
            apply_be(g_eng_usdjpy, g_sym_cfg.get("USDJPY"),  150.0);
            apply_be(g_eng_brent,  g_sym_cfg.get("BRENT"), 85.0);

            // Per-symbol WATCH_TIMEOUT_SEC -- 120s indices, 120s FX
            g_eng_ger30.WATCH_TIMEOUT_SEC  = 240;
            g_eng_uk100.WATCH_TIMEOUT_SEC  = 240;
            g_eng_estx50.WATCH_TIMEOUT_SEC = 240;
            g_eng_xag.WATCH_TIMEOUT_SEC    = 240;
            g_eng_eurusd.WATCH_TIMEOUT_SEC = 240;
            g_eng_gbpusd.WATCH_TIMEOUT_SEC = 240;
            g_eng_audusd.WATCH_TIMEOUT_SEC = 240;
            g_eng_nzdusd.WATCH_TIMEOUT_SEC = 240;
            g_eng_usdjpy.WATCH_TIMEOUT_SEC = 240;
            g_eng_brent.WATCH_TIMEOUT_SEC  = 240;

            // BracketEngine symbols -- override configure() fields from symbols.ini.
            // NOTE: SLIPPAGE_BUFFER is NOT overridden here. The configure() calls above
            // already set correct per-symbol slippage in price-point units (0.50 for ESTX50,
            // 0.80 for gold, 0.00010 for EURUSD etc). The SLIPPAGE_EST_BP field in symbols.ini
            // is in basis-points which is only meaningful for FX -- converting bp*price/10000
            // for indices produces wildly inflated values (5bp*5600=2.80pts vs correct 0.50pts)
            // and blocks all index bracket trades. Do not re-derive it here.
            auto apply_bracket = [](auto& eng, const SymbolConfig& c) {
                if (c.min_range        > 0.0) eng.MIN_RANGE          = c.min_range;
                if (c.max_range        > 0.0) eng.MAX_RANGE          = c.max_range;
                if (c.min_structure_ms > 0)   eng.MIN_STRUCTURE_MS   = c.min_structure_ms;
                if (c.breakout_fail_ms > 0)   eng.FAILURE_WINDOW_MS  = c.breakout_fail_ms;
                if (c.min_hold_ms      > 0)   eng.MIN_HOLD_MS        = c.min_hold_ms;
                if (c.max_hold_sec     > 0)   eng.PENDING_TIMEOUT_SEC = c.max_hold_sec;
                if (c.tp_mult          > 0.0) eng.RR                  = c.tp_mult;
                if (c.max_spread       > 0.0) eng.MAX_SPREAD          = c.max_spread;
                // SLIPPAGE_BUFFER intentionally NOT set here -- configure() has correct values
            };
            apply_bracket(g_bracket_gold,   g_sym_cfg.get("XAUUSD"));
            apply_bracket(g_bracket_xag,    g_sym_cfg.get("XAGUSD"));
            apply_bracket(g_bracket_sp,     g_sym_cfg.get("US500.F"));
            apply_bracket(g_bracket_nq,     g_sym_cfg.get("USTEC.F"));
            apply_bracket(g_bracket_us30,   g_sym_cfg.get("DJ30.F"));
            apply_bracket(g_bracket_nas100, g_sym_cfg.get("NAS100"));
            apply_bracket(g_bracket_ger30,  g_sym_cfg.get("GER40"));
            apply_bracket(g_bracket_uk100,  g_sym_cfg.get("UK100"));
            apply_bracket(g_bracket_estx50, g_sym_cfg.get("ESTX50"));
            apply_bracket(g_bracket_brent,  g_sym_cfg.get("BRENT"));
            apply_bracket(g_bracket_eurusd, g_sym_cfg.get("EURUSD"));
            apply_bracket(g_bracket_gbpusd, g_sym_cfg.get("GBPUSD"));
            apply_bracket(g_bracket_audusd, g_sym_cfg.get("AUDUSD"));
            apply_bracket(g_bracket_nzdusd, g_sym_cfg.get("NZDUSD"));
            apply_bracket(g_bracket_usdjpy, g_sym_cfg.get("USDJPY"));
            std::cout << "[SYMCFG] All bracket engine params overridden from " << sym_ini << "\n";

            // Apply supervisor config from symbols.ini to each supervisor
            // apply_supervisor: wires SymbolConfig fields into supervisor cfg.
            // max_spread_pct is NOT in SymbolConfig -- it lives in OmegaConfig
            // per-symbol fields. Pass it explicitly per symbol so the supervisor's
            // spread gate fires at the correct threshold (was stuck at 0.10 default,
            // which at $24,000 = $2,400 threshold -- never triggered).
            auto apply_supervisor = [](omega::SymbolSupervisor& sup,
                                       const std::string& sym,
                                       const SymbolConfig& c,
                                       double max_spread_pct_override) {
                sup.symbol                          = sym;
                sup.cfg.allow_bracket               = c.allow_bracket;
                sup.cfg.allow_breakout              = c.allow_breakout;
                sup.cfg.min_regime_confidence       = c.min_regime_confidence;
                sup.cfg.min_engine_win_margin       = c.min_engine_win_margin;
                sup.cfg.min_winner_score            = c.min_winner_score;
                sup.cfg.min_bracket_score           = c.min_bracket_score;
                sup.cfg.max_false_breaks            = c.max_false_breaks;
                sup.cfg.bracket_in_quiet_comp       = c.bracket_in_quiet_comp;
                sup.cfg.breakout_in_trend           = c.breakout_in_trend;
                sup.cfg.cooldown_fail_threshold     = c.cooldown_fail_threshold;
                sup.cfg.cooldown_duration_ms        = static_cast<int64_t>(c.cooldown_duration_ms);
                sup.cfg.max_spread_pct              = max_spread_pct_override;
            };
            apply_supervisor(g_sup_sp,     "US500.F", g_sym_cfg.get("US500.F"), g_cfg.sp_max_spread_pct);
            apply_supervisor(g_sup_nq,     "USTEC.F", g_sym_cfg.get("USTEC.F"), g_cfg.nq_max_spread_pct);
            apply_supervisor(g_sup_cl,     "USOIL.F", g_sym_cfg.get("USOIL.F"), g_cfg.oil_max_spread_pct);
            apply_supervisor(g_sup_us30,   "DJ30.F",  g_sym_cfg.get("DJ30.F"),  g_cfg.us30_max_spread_pct);
            apply_supervisor(g_sup_nas100, "NAS100",  g_sym_cfg.get("NAS100"),  g_cfg.nas100_max_spread_pct);
            apply_supervisor(g_sup_ger30,  "GER40",   g_sym_cfg.get("GER40"),   g_cfg.eu_index_max_spread_pct);
            apply_supervisor(g_sup_uk100,  "UK100",   g_sym_cfg.get("UK100"),   g_cfg.eu_index_max_spread_pct);
            apply_supervisor(g_sup_estx50, "ESTX50",  g_sym_cfg.get("ESTX50"),  g_cfg.eu_index_max_spread_pct);
            apply_supervisor(g_sup_xag,    "XAGUSD",  g_sym_cfg.get("XAGUSD"),  g_cfg.silver_max_spread_pct);
            apply_supervisor(g_sup_eurusd, "EURUSD",  g_sym_cfg.get("EURUSD"),  g_cfg.fx_max_spread_pct);
            apply_supervisor(g_sup_gbpusd, "GBPUSD",  g_sym_cfg.get("GBPUSD"),  g_cfg.gbpusd_max_spread_pct);
            // AUDUSD/NZDUSD: use their dedicated max_spread_pct (0.030/0.035), NOT
            // fx_max_spread_pct (0.017). During Asia session AUD/NZD spreads widen to
            // ~2-3 pips (~0.025-0.030%), which exceeds 0.017% and locks the supervisor
            // in permanent HIGH_RISK_NO_TRADE. Their own spread thresholds were already
            // calibrated for this -- apply_supervisor was ignoring them.
            apply_supervisor(g_sup_audusd, "AUDUSD",  g_sym_cfg.get("AUDUSD"),  g_cfg.audusd_max_spread_pct);
            apply_supervisor(g_sup_nzdusd, "NZDUSD",  g_sym_cfg.get("NZDUSD"),  g_cfg.nzdusd_max_spread_pct);
            apply_supervisor(g_sup_usdjpy, "USDJPY",  g_sym_cfg.get("USDJPY"),  g_cfg.usdjpy_max_spread_pct);
            apply_supervisor(g_sup_brent,  "BRENT", g_sym_cfg.get("BRENT"), g_cfg.brent_max_spread_pct);
            apply_supervisor(g_sup_gold,   "XAUUSD",  g_sym_cfg.get("XAUUSD"),  g_cfg.bracket_gold_max_spread_pct);
            std::cout << "[SUPERVISOR] All supervisors configured from " << sym_ini << "\n";
            std::cout << "[SYMCFG] All engine params overridden from " << sym_ini << "\n";
        } else {
            // ?? CRITICAL: symbols.ini not found ??????????????????????????????????
            // Without symbols.ini: allow_bracket=false for all non-metals.
            // Bracket engines for indices/FX will NOT fire until symbols.ini is deployed.
            // This is intentional -- bracket config is not safe to run with compiled defaults.
            std::cout << "\n";
            std::cout << "[SYMCFG] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n";
            std::cout << "[SYMCFG] !!! CRITICAL: symbols.ini NOT FOUND -- BRACKET DISABLED !!!\n";
            std::cout << "[SYMCFG] !!! Searched:                                           !!!\n";
            std::cout << "[SYMCFG] !!!   symbols.ini                                       !!!\n";
            std::cout << "[SYMCFG] !!!   C:\\Omega\\symbols.ini                              !!!\n";
            std::cout << "[SYMCFG] !!!   C:\\Omega\\config\\symbols.ini                      !!!\n";
            std::cout << "[SYMCFG] !!! Copy symbols.ini to Omega.exe directory and restart !!!\n";
            std::cout << "[SYMCFG] !!! allow_bracket=false for all non-metals until fixed  !!!\n";
            std::cout << "[SYMCFG] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n";
            std::cout << "\n";
            // Assign symbol names directly so supervisor logs are readable even without ini
            g_sup_sp.symbol     = "US500.F"; g_sup_nq.symbol     = "USTEC.F";
            g_sup_cl.symbol     = "USOIL.F"; g_sup_us30.symbol   = "DJ30.F";
            g_sup_nas100.symbol = "NAS100";  g_sup_ger30.symbol  = "GER40";
            g_sup_uk100.symbol  = "UK100";   g_sup_estx50.symbol = "ESTX50";
            g_sup_xag.symbol    = "XAGUSD";  g_sup_gold.symbol   = "XAUUSD";
            g_sup_eurusd.symbol = "EURUSD";  g_sup_gbpusd.symbol = "GBPUSD";
            g_sup_audusd.symbol = "AUDUSD";  g_sup_nzdusd.symbol = "NZDUSD";
            g_sup_usdjpy.symbol = "USDJPY";  g_sup_brent.symbol  = "BRENT";
            // Without symbols.ini: disable bracket on non-metals (no bracket engine exists)
            for (auto* sup : {&g_sup_sp, &g_sup_nq, &g_sup_cl, &g_sup_us30, &g_sup_nas100,
                              &g_sup_ger30, &g_sup_uk100, &g_sup_estx50,
                              &g_sup_eurusd, &g_sup_gbpusd, &g_sup_audusd,
                              &g_sup_nzdusd, &g_sup_usdjpy, &g_sup_brent})
                sup->cfg.allow_bracket = false;
            // Raise cooldown threshold from default 3 to 20
            for (auto* sup : {&g_sup_sp, &g_sup_nq, &g_sup_cl, &g_sup_us30, &g_sup_nas100,
                              &g_sup_ger30, &g_sup_uk100, &g_sup_estx50, &g_sup_xag,
                              &g_sup_gold, &g_sup_eurusd, &g_sup_gbpusd, &g_sup_audusd,
                              &g_sup_nzdusd, &g_sup_usdjpy, &g_sup_brent})
                sup->cfg.cooldown_fail_threshold = 20;
        }
    }
    // ?? Hot-reload config watcher ??????????????????????????????????????????????
    // Watches omega_config.ini every 2s. On change: re-runs load_config() +
    // sanitize_config() + all apply_engine_config() calls -- zero downtime,
    // no position closes, no reconnect. Change any param in the .ini and it
    // takes effect within 2 seconds without rebooting Omega.
    // NOT reloadable: FIX host/port/credentials, mode (SHADOW/LIVE), code changes.
    OmegaHotReload::start(cfg_path, [&cfg_path]() {
        load_config(cfg_path);
        sanitize_config();
        apply_engine_config(g_eng_sp);
        apply_engine_config(g_eng_nq);
        apply_engine_config(g_eng_cl);
        apply_engine_config(g_eng_us30);
        apply_engine_config(g_eng_nas100);
        apply_generic_index_config(g_eng_ger30);
        apply_generic_index_config(g_eng_uk100);
        apply_generic_index_config(g_eng_estx50);
        apply_generic_silver_config(g_eng_xag);
        apply_generic_fx_config(g_eng_eurusd);
        apply_generic_gbpusd_config(g_eng_gbpusd);
        apply_generic_audusd_config(g_eng_audusd);
        apply_generic_nzdusd_config(g_eng_nzdusd);
        apply_generic_usdjpy_config(g_eng_usdjpy);
        apply_generic_brent_config(g_eng_brent);
        printf("[HOT-RELOAD] All engine configs refreshed\n");
        fflush(stdout);
    });

    // ?? Position sizing ???????????????????????????????????????????????????????
    // ENTRY_SIZE on each engine is the FALLBACK lot used only when
    // risk_per_trade_usd == 0. When risk sizing is active, compute_size()
    // calculates the lot dynamically and ENTRY_SIZE is never used.
    // NAS100 broker minimum is 0.10 lots -- enforced as fallback and in compute_size floor.
    if (g_cfg.risk_per_trade_usd <= 0.0) {
        // Fixed lot mode -- risk sizing disabled, use hardcoded fallbacks
        g_eng_sp.ENTRY_SIZE       = 0.01;
        g_eng_nq.ENTRY_SIZE       = 0.01;
        g_eng_cl.ENTRY_SIZE       = 0.01;
        g_eng_us30.ENTRY_SIZE     = 0.01;
        g_eng_nas100.ENTRY_SIZE   = 0.10;  // NAS100 broker minimum
        g_eng_ger30.ENTRY_SIZE    = 0.01;
        g_eng_uk100.ENTRY_SIZE    = 0.01;
        g_eng_estx50.ENTRY_SIZE   = 0.01;
        g_eng_xag.ENTRY_SIZE      = 0.01;
        g_eng_eurusd.ENTRY_SIZE   = 0.01;
        g_eng_gbpusd.ENTRY_SIZE   = 0.01;
        g_eng_brent.ENTRY_SIZE    = 0.01;
        // Bracket engine fallbacks -- same as breakout engines
        g_bracket_sp.ENTRY_SIZE       = 0.01;
        g_bracket_nq.ENTRY_SIZE       = 0.01;
        g_bracket_us30.ENTRY_SIZE     = 0.01;
        g_bracket_nas100.ENTRY_SIZE   = 0.10;  // NAS100 broker minimum
        g_bracket_ger30.ENTRY_SIZE    = 0.01;
        g_bracket_uk100.ENTRY_SIZE    = 0.01;
        g_bracket_estx50.ENTRY_SIZE   = 0.01;
        g_bracket_brent.ENTRY_SIZE    = 0.01;
        g_bracket_eurusd.ENTRY_SIZE   = 0.01;
        g_bracket_gbpusd.ENTRY_SIZE   = 0.01;
        g_bracket_audusd.ENTRY_SIZE   = 0.01;
        g_bracket_nzdusd.ENTRY_SIZE   = 0.01;
        g_bracket_usdjpy.ENTRY_SIZE   = 0.01;
        std::cout << "[SIZING] Fixed lot mode active (risk_per_trade_usd=0)\n"
                  << "[SIZING]   All instruments: 0.01 lots | NAS100: 0.10 lots\n";
    } else {
        // Risk-based sizing active -- ENTRY_SIZE is only a safety fallback,
        // compute_size() drives actual lot size from risk_per_trade_usd.
        // Set fallbacks to sensible minimums in case compute_size() ever bails.
        g_eng_sp.ENTRY_SIZE       = 0.01;
        g_eng_nq.ENTRY_SIZE       = 0.01;
        g_eng_cl.ENTRY_SIZE       = 0.01;
        g_eng_us30.ENTRY_SIZE     = 0.01;
        g_eng_nas100.ENTRY_SIZE   = 0.10;  // NAS100 broker minimum
        g_eng_ger30.ENTRY_SIZE    = 0.01;
        g_eng_uk100.ENTRY_SIZE    = 0.10;  // indices: $10 / ~8pt SL * $1/pt = 1.25 ? capped at max
        g_eng_estx50.ENTRY_SIZE   = 0.10;
        g_eng_xag.ENTRY_SIZE      = 0.01;
        g_eng_eurusd.ENTRY_SIZE   = 0.01;
        g_eng_gbpusd.ENTRY_SIZE   = 0.01;
        g_eng_brent.ENTRY_SIZE    = 0.01;
        // Bracket engine fallbacks
        g_bracket_sp.ENTRY_SIZE       = 0.10;
        g_bracket_nq.ENTRY_SIZE       = 0.10;
        g_bracket_us30.ENTRY_SIZE     = 0.10;
        g_bracket_nas100.ENTRY_SIZE   = 0.10;  // NAS100 broker minimum
        g_bracket_ger30.ENTRY_SIZE    = 0.10;
        g_bracket_uk100.ENTRY_SIZE    = 0.10;
        g_bracket_estx50.ENTRY_SIZE   = 0.10;
        g_bracket_brent.ENTRY_SIZE    = 0.01;
        g_bracket_eurusd.ENTRY_SIZE   = 0.01;
        g_bracket_gbpusd.ENTRY_SIZE   = 0.01;
        g_bracket_audusd.ENTRY_SIZE   = 0.01;
        g_bracket_nzdusd.ENTRY_SIZE   = 0.01;
        g_bracket_usdjpy.ENTRY_SIZE   = 0.01;
        std::cout << "[SIZING] Risk-based sizing active (risk_per_trade_usd=$"
                  << g_cfg.risk_per_trade_usd << ")\n"
                  << "[SIZING]   Lot size computed dynamically per trade from SL distance\n"
                  << "[SIZING]   Fallback (if compute_size fails): indices=0.10 | others=0.01\n";
    }

    // Wire account equity to edge model -- applies in all modes.
    // Shadow uses the same account_equity for Kelly sizing as live would.
    {
        const double acct_eq = g_cfg.account_equity;
        g_eng_sp.ACCOUNT_EQUITY     = acct_eq;
        g_eng_nq.ACCOUNT_EQUITY     = acct_eq;
        g_eng_cl.ACCOUNT_EQUITY     = acct_eq;
        g_eng_us30.ACCOUNT_EQUITY   = acct_eq;
        g_eng_nas100.ACCOUNT_EQUITY = acct_eq;
        g_eng_ger30.ACCOUNT_EQUITY  = acct_eq;
        g_eng_uk100.ACCOUNT_EQUITY  = acct_eq;
        g_eng_estx50.ACCOUNT_EQUITY = acct_eq;
        g_eng_xag.ACCOUNT_EQUITY    = acct_eq;
        g_eng_eurusd.ACCOUNT_EQUITY = acct_eq;
        g_eng_gbpusd.ACCOUNT_EQUITY = acct_eq;
        g_eng_brent.ACCOUNT_EQUITY  = acct_eq;
        std::cout << "[SIZING] account_equity=" << acct_eq
                  << (g_cfg.mode == "LIVE" ? " (LIVE)" : " (SHADOW -- same as live)") << "\n";
    }
    std::cout.flush();

    // GoldEngineStack config -- applies all [gold_stack] ini values.
    // Must be called AFTER load_config(). Defaults are safe (match prior constexpr).
    g_gold_stack.configure(g_cfg.gs_cfg);

    // LatencyEdgeStack config -- applies all [latency_edge] ini values.
    // Must be called AFTER load_config(). Defaults are safe (match prior constexpr).
    g_le_stack.configure(g_cfg.le_cfg);

    // ?? SHELVED ENGINE DISABLE -- 2026-03-31 ??????????????????????????????????
    // Engines disabled based on live performance audit. Each engine's own
    // guard (if (!enabled_) return noSignal()) prevents new entries.
    // Existing positions (if any) are still drained via has_open_position() paths.
    //
    // NBM indices: live data insufficient, not validated. Still shelved.
    g_nbm_sp.enabled     = false;
    g_nbm_nq.enabled     = false;
    g_nbm_nas.enabled    = false;
    g_nbm_us30.enabled   = false;
    // NBM gold london: RE-ENABLED 2026-04-01 -- live MT5 data confirms the logic
    // (London open ATR breakout, 51min hold, +$185). Omega NBM is identical concept.
    g_nbm_gold_london.enabled = true;
    g_nbm_oil_london.enabled  = false;
    //
    // ORB (OpeningRange): no live data. Shelved pending shadow validation.
    g_orb_us.enabled     = false;
    g_orb_ger30.enabled  = false;
    g_orb_uk100.enabled  = false;
    g_orb_estx50.enabled = false;
    g_orb_silver.enabled = false;
    //
    // Cross-asset: EIA fade, BrentWTI spread, FX cascade, carry unwind.
    // All have insufficient live data. Shelved pending shadow validation.
    g_ca_eia_fade.enabled    = false;
    g_ca_brent_wti.enabled   = false;
    g_ca_fx_cascade.enabled  = false;
    g_ca_carry_unwind.enabled = false;
    // ESNQ: already guarded by esnq_enabled=false in config. Belt-and-suspenders.
    g_ca_esnq.enabled        = false;
    // ?? END SHELVED ENGINE DISABLE ????????????????????????????????????????????

    // ?? Adaptive intelligence layer startup ???????????????????????????????????
    {
        const int64_t now_s = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        // Configure news blackout scheduler from config flags (defaults are fine, shown for clarity)
        g_news_blackout.scheduler.block_nfp   = true;
        g_news_blackout.scheduler.block_fomc  = true;
        g_news_blackout.scheduler.block_cpi   = true;
        g_news_blackout.scheduler.block_eia   = true;
        g_news_blackout.scheduler.block_cb    = true;

        // ?? Live calendar: inject exact event times from Forex Factory ????????
        // Fetches this week's HIGH-impact events over HTTPS and injects precise
        // blackout windows. Falls back gracefully to hardcoded schedule on failure.
        // Pre: 5 min before event. Post: 15 min after event.
        g_live_calendar.pre_min  = 5;
        g_live_calendar.post_min = 15;
        g_live_calendar.refresh_interval_sec = 86400; // re-fetch daily
        g_live_calendar.refresh(g_news_blackout, now_s);

        // Print final schedule (includes both hardcoded + live injected windows)
        g_news_blackout.print_schedule(now_s);

        // ?? Edge systems startup configuration ???????????????????????????????
        // TOD gate: load historical bucket data so gate is active from first trade
        {
            const std::string tod_path = log_root_dir() + "/omega_tod_buckets.csv";
            g_edges.tod.load_csv(tod_path);
            g_edges.tod.min_trades   = 30;    // need 30 trades per bucket before blocking
            g_edges.tod.min_win_rate = 0.38;  // block if WR < 38%
            g_edges.tod.min_avg_ev_usd = -2.0; // block if avg EV < -$2/trade
            g_edges.tod.enabled      = true;  // active in all modes -- shadow is a simulation
        }
        // Fill quality: load persisted fill history so adverse selection detection is
        // active from the first trade -- no minimum fill count, history survives restarts.
        g_edges.fill_quality.load_csv(log_root_dir() + "/fill_quality.csv");
        // Spread Z-score gate: starts building after first 20 ticks per symbol
        g_edges.spread_gate.window_ticks = 200;
        g_edges.spread_gate.max_z_score  = 3.0; // conservative: 3? anomaly
        g_edges.spread_gate.enabled      = true;  // active in all modes -- shadow is a simulation
        // Round number filter
        g_edges.round_numbers.proximity_frac = 0.08; // within 8% of increment = "near"
        // Previous day levels
        g_edges.prev_day.proximity_frac = 0.05; // within 5% of prior range = "near PDH/PDL"
        // FX fix engines
        g_edges.fx_fix.tp_pips        = 15.0;
        g_edges.fx_fix.sl_pips        = 8.0;
        g_edges.fx_fix.min_cvd_signal = 0.12; // need 12% normalised CVD to enter fix
        g_edges.fx_fix.enabled        = true;  // active in all modes -- shadow is a simulation
        // Fill quality
        g_edges.fill_quality.window_trades = 50;
        g_edges.fill_quality.adverse_threshold_bps = 2.0;
        g_edges.fill_quality.enabled = true;
        std::printf("[EDGES] All 7 edge systems initialised\n");

        // Adaptive risk: start with Kelly disabled for first 15 trades per symbol
        // (confidence ramps from 0?1 automatically as trades accumulate)
        g_adaptive_risk.kelly_enabled            = true;
        g_adaptive_risk.dd_throttle_enabled      = true;
        g_adaptive_risk.corr_heat_enabled        = true;
        g_adaptive_risk.vol_regime_enabled       = true;
        g_adaptive_risk.multiday_throttle_enabled = true;
        g_adaptive_risk.fill_quality_enabled     = true;
        g_adaptive_risk.fill_quality_scale       = 0.70;  // 30% size cut on adverse selection

        // Drawdown velocity circuit breaker: halt new entries for 15 min if we
        // lose more than 50% of daily_loss_limit within any 30-minute window.
        // e.g. daily_loss_limit=$200 ? fires if rolling 30-min loss > $100.
        // Set threshold_usd=0 to disable (off by default until shadow validates).
        g_adaptive_risk.dd_velocity.threshold_usd = g_cfg.daily_loss_limit * 0.5;
        g_adaptive_risk.dd_velocity.window_sec    = 1800;   // 30-minute window
        g_adaptive_risk.dd_velocity.halt_sec      = 900;    // 15-minute halt
        std::printf("[ADAPTIVE] DD-velocity threshold=$%.0f/30min halt=15min\n",
                    g_adaptive_risk.dd_velocity.threshold_usd);

        // Correlation cluster limits -- 2 is the safe default for all clusters
        g_adaptive_risk.corr_heat.max_per_cluster_us_equity = 2;
        g_adaptive_risk.corr_heat.max_per_cluster_eu_equity = 2;
        g_adaptive_risk.corr_heat.max_per_cluster_oil       = 1;  // never both OIL+BRENT open
        g_adaptive_risk.corr_heat.max_per_cluster_metals    = 2;
        g_adaptive_risk.corr_heat.max_per_cluster_jpy_risk  = 2;
        g_adaptive_risk.corr_heat.max_per_cluster_eur_gbp   = 1;  // EURUSD or GBPUSD, not both

        // Load Kelly performance history from previous sessions
        {
            const std::string kelly_dir = log_root_dir() + "/kelly";
            // ensure_parent_dir creates the directory if needed
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::create_directories(kelly_dir, ec);
            g_adaptive_risk.load_perf(kelly_dir);
        }

        // Load GoldFlowEngine ATR state -- eliminates 100-tick blind zone on restart
        {
            const std::string atr_path        = log_root_dir() + "/gold_flow_atr.dat";
            const std::string atr_backup_path = log_root_dir() + "/gold_flow_atr_backup.dat";

            g_gold_flow.load_atr_state(atr_path);  // try primary first

            // If primary failed (corrupt, missing, stale), fall back to backup (~60s older).
            // Backup is written every 60s so it survives a hard kill that corrupts the primary.
            if (g_gold_flow.current_atr() <= 0.0) {
                printf("[GFE] Primary ATR load failed -- trying backup %s\n",
                       atr_backup_path.c_str());
                g_gold_flow.load_atr_state(atr_backup_path);
                if (g_gold_flow.current_atr() > 0.0) {
                    printf("[GFE] ATR restored from backup: atr=%.4f\n",
                           g_gold_flow.current_atr());
                } else {
                    printf("[GFE] ATR backup also failed -- cold seed will be used\n");
                }
            }

            // Pass VIX at startup -- if VIX feed not yet live this gives 10pt default
            // which is far safer than the old 3pt hardcoded seed
            g_gold_flow.seed(0.0, g_macro_ctx.vix);  // VIX-scaled fallback if no ATR file
            printf("[GFE] Startup ATR: m_atr=%.4f (%s)\n",
                   g_gold_flow.current_atr(),
                   g_gold_flow.current_atr() > 0.0 ? "warmed" : "cold-seeded");

            // ?? Velocity trail shadow mode ????????????????????????????????????
            // Run velocity trail in observation mode by default: SL/time-stop logic
            // is fully active but [VEL-TRAIL-SHADOW] logs are emitted for every
            // regime classification so we can verify on real ticks before live.
            // To go live: set velocity_shadow_mode = false on both instances.
            g_gold_flow.velocity_shadow_mode        = true;
            g_gold_flow_reload.velocity_shadow_mode = true;
            printf("[GFE] Velocity trail: SHADOW mode (observation only -- verify regime logs before enabling live)\n");
            fflush(stdout);

            // ?? Hard stop broker-side order callback ??????????????????????
            // Wires GoldFlowEngine's on_hard_stop to send_hard_stop_order so
            // the broker holds a resting LIMIT order even if Omega crashes.
            // Fires once per position when adverse >= 20pts (tombstone-guarded).
            g_gold_flow.on_hard_stop = [](const std::string& sym, bool is_long,
                                          double qty, double sl_px, int64_t ts) {
                send_hard_stop_order(sym, is_long, qty, sl_px, ts);
            };
            g_gold_flow_reload.on_hard_stop = [](const std::string& sym, bool is_long,
                                                  double qty, double sl_px, int64_t ts) {
                send_hard_stop_order(sym, is_long, qty, sl_px, ts);
            };
            printf("[GFE] Hard stop: ARMED (broker LIMIT order fires at 20pts adverse)\n");
            fflush(stdout);

            // ?? Velocity add-on callback ??????????????????????????????????????????
            // Wires GoldFlowEngine's on_addon to fire g_gold_flow_addon when the base
            // position confirms a velocity expansion move (trail_stage>=2 + vol>2.5).
            // Add-on instance uses same session/risk gates as reload.
            // Shadow mode disabled -- gates verified in incident testing + code audit.
            g_gold_flow.addon_shadow_mode = false;  // LIVE: [GFE-ADDON] fires real entries
            g_gold_flow.on_addon = [](bool is_long, double bid, double ask,
                                      double atr, double base_trail_sl,
                                      int64_t base_entry_ts) {
                // Gate: addon instance must be flat (no double-stacking)
                if (g_gold_flow_addon.has_open_position()) {
                    printf("[GFE-ADDON] BLOCKED -- addon instance already has open position\n");
                    fflush(stdout);
                    return;
                }
                // Gate: daily loss proximity -- if within 20% of daily limit, skip
                {
                    const double daily_pnl = g_omegaLedger.dailyPnl();
                    if (daily_pnl < -g_cfg.daily_loss_limit * 0.80) {
                        printf("[GFE-ADDON] BLOCKED -- daily_pnl=$%.2f within 20%% of limit=$%.2f\n",
                               daily_pnl, g_cfg.daily_loss_limit);
                        fflush(stdout);
                        return;
                    }
                }
                // Seed addon instance ATR and force-entry
                const double addon_size = std::max(GFE_MIN_LOT,
                    std::min(g_gold_flow.pos.full_size * 0.5, GFE_MAX_LOT));
                g_gold_flow_addon.risk_dollars        = addon_size * std::fabs(
                    (is_long ? ask : bid) - base_trail_sl) * 100.0;
                g_gold_flow_addon.addon_shadow_mode   = false; // addon instance never recurses
                g_gold_flow_addon.velocity_shadow_mode = g_gold_flow.velocity_shadow_mode;
                g_gold_flow_addon.on_hard_stop = [](const std::string& sym, bool il,
                                                     double qty, double sl_px, int64_t ts) {
                    send_hard_stop_order(sym, il, qty, sl_px, ts);
                };
                // force_entry bypasses all persistence gates -- addon confirmation
                // was already done by the base position's 10+ gates.
                // Clamp session_slot: -1 (supervisor not warmed) -> 1 (London fallback).
                const int addon_slot = (g_macro_ctx.session_slot >= 0)
                    ? g_macro_ctx.session_slot : 1;
                const bool entered = g_gold_flow_addon.force_entry(
                    is_long, bid, ask, atr, 
                    static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count()),
                    addon_slot);
                if (entered) {
                    // Override the addon SL to be exactly the base trail SL
                    // force_entry sets SL based on ATR -- we want it at base trail
                    if (is_long  && base_trail_sl < g_gold_flow_addon.pos.entry)
                        g_gold_flow_addon.pos.sl = base_trail_sl;
                    if (!is_long && base_trail_sl > g_gold_flow_addon.pos.entry)
                        g_gold_flow_addon.pos.sl = base_trail_sl;
                    printf("[GFE-ADDON] ENTERED %s @ %.2f sl=%.2f(base_trail) size=%.4f "
                           "base_entry_ts=%lld\n",
                           is_long ? "LONG" : "SHORT",
                           is_long ? ask : bid,
                           g_gold_flow_addon.pos.sl,
                           g_gold_flow_addon.pos.size,
                           (long long)base_entry_ts);
                    fflush(stdout);
                    // Send live order
                    send_live_order("XAUUSD", is_long, g_gold_flow_addon.pos.size,
                                    is_long ? ask : bid);
                } else {
                    printf("[GFE-ADDON] force_entry failed (already has position?)\n");
                    fflush(stdout);
                }
            };
            printf("[GFE] Velocity add-on: LIVE mode -- [GFE-ADDON] fires on confirmed expansion moves\n");
            fflush(stdout);
        }

        // Load GoldStack vol baseline + governor EWM -- skips 400-tick regime warmup
        {
            const std::string gs_path = log_root_dir() + "/gold_stack_state.dat";
            g_gold_stack.load_atr_state(gs_path);
        }

        // Multi-day drawdown throttle: load history, log startup state
        {
            const std::string md_path = log_root_dir() + "/day_results.csv";
            g_adaptive_risk.multiday.load(md_path);
            const int streak      = g_adaptive_risk.multiday.consecutive_losing_days();
            const double md_scale = g_adaptive_risk.multiday.size_scale();
            g_telemetry.UpdateMultiDayThrottle(streak, md_scale,
                                               g_adaptive_risk.multiday.is_active() ? 1 : 0);
            if (g_adaptive_risk.multiday.is_active())
                std::cout << "[MULTIDAY-THROTTLE] *** ACTIVE *** consec_loss=" << streak
                          << " -> sizes halved this session\n";
        }

        // Wire fill quality adverse selection check into adjusted_lot().
        // Lambda avoids circular include between OmegaAdaptiveRisk and OmegaEdges.
        g_adaptive_risk.fill_quality_check_fn = [](const std::string& sym) -> bool {
            return g_edges.fill_quality.adverse_selection_detected(sym);
        };

        // Weekend gap size scale: halve lots during Fri 21:00 - Sun 22:00 UTC.
        g_adaptive_risk.weekend_gap_scale_fn = []() -> double {
            return weekend_gap_size_scale();
        };

        // VPIN: enabled, registers size-scale callback into adjusted_lot.
        // block_threshold=0.80 (entry blocked), high_threshold=0.60 (0.5? size).
        g_edges.vpin.enabled         = true;
        g_edges.vpin.bucket_size     = 50;
        g_edges.vpin.window_buckets  = 10;
        g_edges.vpin.high_threshold  = 0.60;
        g_edges.vpin.block_threshold = 0.80;
        g_adaptive_risk.vpin_scale_fn = [](const std::string& sym) -> double {
            return g_edges.vpin.size_scale(sym);
        };

        // Walk-forward OOS validation scale (RenTec #6).
        // Updated every 20 trades via handle_closed_trade -> g_walk_forward.update().
        // Returns 1.0 (pass), 0.75 (degraded), 0.50 (failing).
        // No penalty during warmup (< 40 trades).
        g_adaptive_risk.wfo_scale_fn = [](const std::string& sym) -> double {
            return g_walk_forward.scale(sym);
        };

        // Regime adaptor is enabled; in SHADOW mode it is informational only
        g_regime_adaptor.enabled = true;

        // Portfolio VaR: correlation-adjusted exposure gate.
        // Limit = 1.5 ? daily_loss_limit. At $200 daily limit ? blocks when
        // correlated exposure implies >$300 of potential simultaneous loss.
        g_portfolio_var.init_betas();
        g_portfolio_var.var_limit_usd = g_cfg.daily_loss_limit * 1.5;
        // Correlation matrix -- load warm state from previous session
        g_corr_matrix.load_state(log_root_dir() + "/corr_matrix.dat");
        // VPIN -- reset at session start (stale tick-classification carries no meaning)
        g_vpin.reset();
        g_vpin.toxic_threshold = 0.70;  // block entries above 70% toxic flow
        std::printf("[PORTFOLIO-VAR] limit=$%.0f (1.5? daily_loss_limit)\n",
                    g_portfolio_var.var_limit_usd);

        std::cout << "[ADAPTIVE] Kelly=" << (g_adaptive_risk.kelly_enabled ? "ON" : "OFF")
                  << "  DDthrottle="    << (g_adaptive_risk.dd_throttle_enabled ? "ON" : "OFF")
                  << "  CorrHeat="      << (g_adaptive_risk.corr_heat_enabled ? "ON" : "OFF")
                  << "  VolRegime="     << (g_adaptive_risk.vol_regime_enabled ? "ON" : "OFF")
                  << "  MultiDayDD="    << (g_adaptive_risk.multiday_throttle_enabled ? "ON" : "OFF")
                  << "  FillQuality="   << (g_adaptive_risk.fill_quality_enabled ? "ON" : "OFF")
                  << "  NewsBlackout="  << (g_news_blackout.enabled ? "ON" : "OFF")
                  << "  PartialExit="   << (g_partial_exit.enabled ? "ON" : "OFF")
                  << "  RegimeAdaptor=" << (g_regime_adaptor.enabled ? "ON" : "OFF") << "\n";

        std::cout << "[ADAPTIVE] Corr cluster limits:"
                  << " US_EQ=" << g_adaptive_risk.corr_heat.max_per_cluster_us_equity
                  << " EU_EQ=" << g_adaptive_risk.corr_heat.max_per_cluster_eu_equity
                  << " OIL="   << g_adaptive_risk.corr_heat.max_per_cluster_oil
                  << " METALS=" << g_adaptive_risk.corr_heat.max_per_cluster_metals
                  << " JPY="   << g_adaptive_risk.corr_heat.max_per_cluster_jpy_risk
                  << " EUR_GBP=" << g_adaptive_risk.corr_heat.max_per_cluster_eur_gbp << "\n";
    }

    // ?? Startup parameter validation -- logged on every start ?????????????????
    // This block documents the exact live values every engine will use.
    // Any mismatch between config intent and actual values is visible immediately.
    std::cout << "\n[OMEGA-PARAMS] ???????????????????????????????????????????\n"
              << "[OMEGA-PARAMS] ENGINE PARAMETER AUDIT (live values after all config overrides)\n"
              << "[OMEGA-PARAMS] ???????????????????????????????????????????\n"
              << "[OMEGA-PARAMS] US500.F  TP=" << g_eng_sp.TP_PCT  << "% SL=" << g_eng_sp.SL_PCT
              << "% vol=" << g_eng_sp.VOL_THRESH_PCT << "% mom=" << g_eng_sp.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_sp.MIN_BREAKOUT_PCT << "% gap=" << g_eng_sp.MIN_GAP_SEC
              << "s hold=" << g_eng_sp.MAX_HOLD_SEC << "s spread=" << g_eng_sp.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] USTEC.F  TP=" << g_eng_nq.TP_PCT  << "% SL=" << g_eng_nq.SL_PCT
              << "% vol=" << g_eng_nq.VOL_THRESH_PCT << "% mom=" << g_eng_nq.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_nq.MIN_BREAKOUT_PCT << "% gap=" << g_eng_nq.MIN_GAP_SEC
              << "s hold=" << g_eng_nq.MAX_HOLD_SEC << "s spread=" << g_eng_nq.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] USOIL.F  TP=" << g_eng_cl.TP_PCT  << "% SL=" << g_eng_cl.SL_PCT
              << "% vol=" << g_eng_cl.VOL_THRESH_PCT << "% mom=" << g_eng_cl.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_cl.MIN_BREAKOUT_PCT << "% gap=" << g_eng_cl.MIN_GAP_SEC
              << "s hold=" << g_eng_cl.MAX_HOLD_SEC << "s spread=" << g_eng_cl.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] DJ30.F   TP=" << g_eng_us30.TP_PCT << "% SL=" << g_eng_us30.SL_PCT
              << "% vol=" << g_eng_us30.VOL_THRESH_PCT << "% mom=" << g_eng_us30.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_us30.MIN_BREAKOUT_PCT << "% gap=" << g_eng_us30.MIN_GAP_SEC
              << "s hold=" << g_eng_us30.MAX_HOLD_SEC << "s spread=" << g_eng_us30.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] NAS100   TP=" << g_eng_nas100.TP_PCT << "% SL=" << g_eng_nas100.SL_PCT
              << "% vol=" << g_eng_nas100.VOL_THRESH_PCT << "% mom=" << g_eng_nas100.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_nas100.MIN_BREAKOUT_PCT << "% gap=" << g_eng_nas100.MIN_GAP_SEC
              << "s hold=" << g_eng_nas100.MAX_HOLD_SEC << "s spread=" << g_eng_nas100.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] XAGUSD   TP=" << g_eng_xag.TP_PCT  << "% SL=" << g_eng_xag.SL_PCT
              << "% vol=" << g_eng_xag.VOL_THRESH_PCT << "% mom=" << g_eng_xag.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_xag.MIN_BREAKOUT_PCT << "% gap=" << g_eng_xag.MIN_GAP_SEC
              << "s hold=" << g_eng_xag.MAX_HOLD_SEC << "s spread=" << g_eng_xag.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] AUDUSD   TP=" << g_eng_audusd.TP_PCT << "% SL=" << g_eng_audusd.SL_PCT
              << "% vol=" << g_eng_audusd.VOL_THRESH_PCT << "% mom=" << g_eng_audusd.MOMENTUM_THRESH_PCT
              << "% gap=" << g_eng_audusd.MIN_GAP_SEC << "s spread=" << g_eng_audusd.MAX_SPREAD_PCT << "% [ASIA]\n"
              << "[OMEGA-PARAMS] NZDUSD   TP=" << g_eng_nzdusd.TP_PCT << "% SL=" << g_eng_nzdusd.SL_PCT
              << "% vol=" << g_eng_nzdusd.VOL_THRESH_PCT << "% mom=" << g_eng_nzdusd.MOMENTUM_THRESH_PCT
              << "% gap=" << g_eng_nzdusd.MIN_GAP_SEC << "s spread=" << g_eng_nzdusd.MAX_SPREAD_PCT << "% [ASIA]\n"
              << "[OMEGA-PARAMS] USDJPY   TP=" << g_eng_usdjpy.TP_PCT << "% SL=" << g_eng_usdjpy.SL_PCT
              << "% vol=" << g_eng_usdjpy.VOL_THRESH_PCT << "% mom=" << g_eng_usdjpy.MOMENTUM_THRESH_PCT
              << "% gap=" << g_eng_usdjpy.MIN_GAP_SEC << "s spread=" << g_eng_usdjpy.MAX_SPREAD_PCT << "% [ASIA/TOKYO-FIX]\n"
              << "[OMEGA-PARAMS] GBPUSD   TP=" << g_eng_gbpusd.TP_PCT << "% SL=" << g_eng_gbpusd.SL_PCT
              << "% vol=" << g_eng_gbpusd.VOL_THRESH_PCT << "% mom=" << g_eng_gbpusd.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_gbpusd.MIN_BREAKOUT_PCT << "% gap=" << g_eng_gbpusd.MIN_GAP_SEC
              << "s spread=" << g_eng_gbpusd.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] EURUSD   TP=" << g_eng_eurusd.TP_PCT << "% SL=" << g_eng_eurusd.SL_PCT
              << "% vol=" << g_eng_eurusd.VOL_THRESH_PCT << "% mom=" << g_eng_eurusd.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_eurusd.MIN_BREAKOUT_PCT << "% gap=" << g_eng_eurusd.MIN_GAP_SEC
              << "s spread=" << g_eng_eurusd.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] GER40    TP=" << g_eng_ger30.TP_PCT  << "% SL=" << g_eng_ger30.SL_PCT
              << "% vol=" << g_eng_ger30.VOL_THRESH_PCT << "% mom=" << g_eng_ger30.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_ger30.MIN_BREAKOUT_PCT << "% gap=" << g_eng_ger30.MIN_GAP_SEC
              << "s hold=" << g_eng_ger30.MAX_HOLD_SEC << "s spread=" << g_eng_ger30.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] UK100    TP=" << g_eng_uk100.TP_PCT  << "% SL=" << g_eng_uk100.SL_PCT
              << "% vol=" << g_eng_uk100.VOL_THRESH_PCT << "% mom=" << g_eng_uk100.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_uk100.MIN_BREAKOUT_PCT << "% gap=" << g_eng_uk100.MIN_GAP_SEC
              << "s hold=" << g_eng_uk100.MAX_HOLD_SEC << "s spread=" << g_eng_uk100.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] ESTX50   TP=" << g_eng_estx50.TP_PCT << "% SL=" << g_eng_estx50.SL_PCT
              << "% vol=" << g_eng_estx50.VOL_THRESH_PCT << "% mom=" << g_eng_estx50.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_estx50.MIN_BREAKOUT_PCT << "% minrange=" << g_eng_estx50.MIN_COMP_RANGE
              << " gap=" << g_eng_estx50.MIN_GAP_SEC
              << "s hold=" << g_eng_estx50.MAX_HOLD_SEC << "s spread=" << g_eng_estx50.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] UKBRENT  TP=" << g_eng_brent.TP_PCT  << "% SL=" << g_eng_brent.SL_PCT
              << "% vol=" << g_eng_brent.VOL_THRESH_PCT << "% mom=" << g_eng_brent.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_brent.MIN_BREAKOUT_PCT << "% gap=" << g_eng_brent.MIN_GAP_SEC
              << "s hold=" << g_eng_brent.MAX_HOLD_SEC << "s spread=" << g_eng_brent.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] XAUUSD   GoldEngineStack active | gap=" << g_cfg.gs_cfg.min_entry_gap_sec
              << "s hold=" << g_cfg.gs_cfg.max_hold_sec << "s vwap_min=" << g_cfg.gs_cfg.min_vwap_dislocation
              << " spread_max=" << g_cfg.gs_cfg.max_entry_spread << "\n"
              << "[OMEGA-PARAMS] GoldStack MIN_ENTRY_GAP=30s MAX_HOLD=600s REGIME_FLIP_MIN=60s\n"
              << "[OMEGA-PARAMS] LeadLag=ACTIVE(3-window-confirm) SpreadDisloc=managed-only EventComp=managed-only\n"
              << "[OMEGA-PARAMS] ???????????????????????????????????????????\n\n";
    std::cout.flush();

    if (g_cfg.mode == "SHADOW") {
        if (g_cfg.shadow_ustec_pilot_only) {
            g_eng_nq.ENTRY_SIZE = g_cfg.ustec_pilot_size;
            g_eng_nq.MIN_GAP_SEC = g_cfg.ustec_pilot_min_gap_sec;
            g_eng_nq.MAX_TRADES_PER_MIN = std::min(g_eng_nq.MAX_TRADES_PER_MIN, 2);
            std::cout << "[OMEGA-PILOT] USTEC shadow pilot enabled | size=" << g_eng_nq.ENTRY_SIZE
                      << " min_gap=" << g_eng_nq.MIN_GAP_SEC
                      << " max_trades_per_min=" << g_eng_nq.MAX_TRADES_PER_MIN << "\n";
        } else {
            std::cout << "[OMEGA-PILOT] Multi-symbol shadow enabled | all configured engines may trade\n";
        }
        std::cout << "[OMEGA-MODE] SHADOW -- exact live simulation (orders paper only, all risk gates active)\n";
    }

    build_id_map();

    // Open log file and tee stdout into it
    // Log file: stdout/stderr teed to file via RollingTeeBuffer
    // g_tee_buf wires std::cout -> file. Console output preserved.
    {
        const std::string log_dir = "C:\\Omega\\logs";
        CreateDirectoryA(log_dir.c_str(), nullptr);
        g_orig_cout = std::cout.rdbuf();
        g_tee_buf   = new RollingTeeBuffer(g_orig_cout, log_dir);
        if (!g_tee_buf->is_open()) {
            // Log failed -- print to stderr (not tee'd) and continue without log
            // Do NOT return 1 -- process must keep running
            fprintf(stderr, "[OMEGA-LOG-WARN] Cannot open log under %s -- continuing without log\n",
                    log_dir.c_str());
            delete g_tee_buf;
            g_tee_buf = nullptr;
        } else {
            std::cout.rdbuf(g_tee_buf);
            std::cerr.rdbuf(g_tee_buf);
            std::cout << "[OMEGA] Log: " << g_tee_buf->current_path() << "\n";
        }
    }

    {
        const std::string trade_dir = log_root_dir() + "/trades";
        const std::string gold_dir  = log_root_dir() + "/gold";
        const std::string shadow_trade_dir = log_root_dir() + "/shadow/trades";
        const std::string header =
            "trade_id,trade_ref,entry_ts_unix,entry_ts_utc,entry_utc_weekday,"
            "exit_ts_unix,exit_ts_utc,exit_utc_weekday,symbol,engine,side,"
            "entry_px,exit_px,tp,sl,size,gross_pnl,net_pnl,"
            "slippage_entry,slippage_exit,commission,"
            "slip_entry_pct,slip_exit_pct,comm_per_side,"
            "mfe,mae,hold_sec,spread_at_entry,"
            "latency_ms,regime,exit_reason";

        const std::string trade_csv_path = trade_dir + "/omega_trade_closes.csv";
        ensure_parent_dir(trade_csv_path);
        g_trade_close_csv.open(trade_csv_path, std::ios::app);
        if (!g_trade_close_csv.is_open()) {
            std::cerr << "[OMEGA-FATAL] Failed to open full trade CSV: " << trade_csv_path << "\n";
            return 1;
        }
        // Write header only if file is empty -- never truncate, all trades must persist
        g_trade_close_csv.seekp(0, std::ios::end);
        if (g_trade_close_csv.tellp() == std::streampos(0))
            g_trade_close_csv << header << '\n';
        std::cout << "[OMEGA] Full Trade CSV: " << trade_csv_path << "\n";

        g_daily_trade_close_log = std::make_unique<RollingCsvLogger>(
            trade_dir, "omega_trade_closes", header);
        g_daily_gold_trade_close_log = std::make_unique<RollingCsvLogger>(
            gold_dir, "omega_gold_trade_closes", header);
        g_daily_shadow_trade_log = std::make_unique<RollingCsvLogger>(
            shadow_trade_dir, "omega_shadow_trades", header);

        // Trade opens CSV -- entry-time audit log, persists across restarts
        const std::string opens_header =
            "entry_ts_unix,entry_ts_utc,entry_utc_weekday,"
            "symbol,engine,side,"
            "entry_px,tp,sl,size,"
            "spread_at_entry,regime,reason";
        const std::string opens_csv_path = trade_dir + "/omega_trade_opens.csv";
        ensure_parent_dir(opens_csv_path);
        g_trade_open_csv.open(opens_csv_path, std::ios::app);
        if (!g_trade_open_csv.is_open()) {
            std::cerr << "[OMEGA-FATAL] Failed to open trade opens CSV: " << opens_csv_path << "\n";
            return 1;
        }
        g_trade_open_csv.seekp(0, std::ios::end);
        if (g_trade_open_csv.tellp() == std::streampos(0))
            g_trade_open_csv << opens_header << '\n';
        std::cout << "[OMEGA] Trade Opens CSV: " << opens_csv_path << "\n";
        g_daily_trade_open_log = std::make_unique<RollingCsvLogger>(
            trade_dir, "omega_trade_opens", opens_header);

        std::cout << "[OMEGA] Daily Trade Logs: " << trade_dir
                  << "/omega_trade_closes_YYYY-MM-DD.csv (UTC, 5-file retention)\n";
        std::cout << "[OMEGA] Daily Gold Logs: " << gold_dir
                  << "/omega_gold_trade_closes_YYYY-MM-DD.csv (UTC, 5-file retention)\n";
        std::cout << "[OMEGA] Daily Shadow Trade Logs: " << shadow_trade_dir
                  << "/omega_shadow_trades_YYYY-MM-DD.csv (UTC, 5-file retention)\n";
    }

    const std::string shadow_csv_path =
        resolve_audit_log_path(g_cfg.shadow_csv, "shadow/omega_shadow.csv");
    ensure_parent_dir(shadow_csv_path);
    g_shadow_csv.open(shadow_csv_path, std::ios::app);
    if (!g_shadow_csv.is_open()) {
        std::cerr << "[OMEGA-FATAL] Failed to open shadow trade CSV: " << shadow_csv_path << "\n";
        return 1;
    }
    // Write header only if file is empty -- never truncate, data must survive restarts
    g_shadow_csv.seekp(0, std::ios::end);
    if (g_shadow_csv.tellp() == std::streampos(0))
        g_shadow_csv << "ts_unix,symbol,side,engine,entry_px,exit_px,pnl,mfe,mae,"
                        "hold_sec,exit_reason,spread_at_entry,latency_ms,regime\n";
    std::cout << "[OMEGA] Shadow CSV: " << shadow_csv_path << "\n";

    // ?? Startup ledger reload -- restore today's closed trades from CSV ???????
    // g_omegaLedger is in-memory only and resets on restart. On restart mid-session
    // the GUI shows daily_pnl=$0 and total_trades=0 even though trades happened.
    // Fix: read today's daily rotating trade CSV on startup and replay into ledger
    // so daily P&L, win/loss counts, and engine attribution are correct immediately.
    // Reads: logs/trades/omega_trade_closes_YYYY-MM-DD.csv (full detail format).
    // Only replays if today's file exists -- no-op on first startup of the day.
    {
        const auto t_now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        struct tm ti_now{}; gmtime_s(&ti_now, &t_now);
        char today_date[16];
        snprintf(today_date, sizeof(today_date), "%04d-%02d-%02d",
                 ti_now.tm_year+1900, ti_now.tm_mon+1, ti_now.tm_mday);
        const std::string reload_path =
            log_root_dir() + "/trades/omega_trade_closes_" + today_date + ".csv";

        std::ifstream reload_f(reload_path);
        if (!g_cfg.reload_trades_on_startup) {
            std::cout << "[OMEGA] Startup reload: DISABLED (reload_trades_on_startup=false) -- clean PnL slate\n";
        } else if (reload_f.is_open()) {
            int reloaded = 0;
            std::string line;
            std::getline(reload_f, line); // skip header
            while (std::getline(reload_f, line)) {
                if (line.empty()) continue;
                // Parse CSV: trade_id(0), trade_ref(1), entry_ts_unix(2), entry_ts_utc(3),
                // entry_wd(4), exit_ts_unix(5), exit_ts_utc(6), exit_wd(7),
                // symbol(8), engine(9), side(10),
                // entry_px(11), exit_px(12), tp(13), sl(14), size(15),
                // gross_pnl(16), net_pnl(17), slip_e(18), slip_x(19), comm(20),
                // slip_epct(21), slip_xpct(22), comm_side(23),
                // mfe(24), mae(25), hold_sec(26), spread(27), lat(28),
                // regime(29), exit_reason(30)
                std::vector<std::string> tok;
                tok.reserve(32);
                std::string cell;
                bool in_q = false;
                for (char c : line) {
                    if (c == '"') { in_q = !in_q; continue; }
                    if (c == ',' && !in_q) { tok.push_back(cell); cell.clear(); continue; }
                    cell += c;
                }
                tok.push_back(cell);
                if (tok.size() < 20) continue;

                omega::TradeRecord tr;
                tr.id         = std::stoi(tok[0].empty() ? "0" : tok[0]);
                tr.entryTs    = std::stoll(tok[2].empty() ? "0" : tok[2]);
                tr.exitTs     = std::stoll(tok[5].empty() ? "0" : tok[5]);
                tr.symbol     = tok[8];
                tr.engine     = tok[9];
                tr.side       = tok[10];
                tr.entryPrice = std::stod(tok[11].empty() ? "0" : tok[11]);
                tr.exitPrice  = std::stod(tok[12].empty() ? "0" : tok[12]);
                tr.tp         = std::stod(tok[13].empty() ? "0" : tok[13]);
                tr.sl         = std::stod(tok[14].empty() ? "0" : tok[14]);
                tr.size       = std::stod(tok[15].empty() ? "0" : tok[15]);
                tr.pnl        = std::stod(tok[16].empty() ? "0" : tok[16]);
                tr.net_pnl    = std::stod(tok[17].empty() ? "0" : tok[17]);
                tr.slippage_entry = std::stod(tok[18].empty() ? "0" : tok[18]);
                tr.slippage_exit  = std::stod(tok[19].empty() ? "0" : tok[19]);
                tr.commission     = tok.size() > 20 ? std::stod(tok[20].empty() ? "0" : tok[20]) : 0.0;
                tr.mfe            = tok.size() > 24 ? std::stod(tok[24].empty() ? "0" : tok[24]) : 0.0;
                tr.mae            = tok.size() > 25 ? std::stod(tok[25].empty() ? "0" : tok[25]) : 0.0;
                tr.spreadAtEntry  = tok.size() > 27 ? std::stod(tok[27].empty() ? "0" : tok[27]) : 0.0;
                tr.regime         = tok.size() > 29 ? tok[29] : "";
                tr.exitReason     = tok.size() > 30 ? tok[30] : "";

                if (tr.symbol.empty() || tr.entryTs == 0) continue;

                g_omegaLedger.record(tr);
                // Also restore engine P&L attribution in telemetry
                g_telemetry.AccumEnginePnl(tr.engine.c_str(), tr.net_pnl);
                ++reloaded;
            }
            reload_f.close();
            if (reloaded > 0) {
                std::cout << "[OMEGA] Startup reload: " << reloaded
                          << " trades from " << reload_path
                          << " ? daily_pnl=$" << std::fixed << std::setprecision(2)
                          << g_omegaLedger.dailyPnl() << "\n";
            } else {
                std::cout << "[OMEGA] Startup reload: " << reload_path
                          << " found but empty (first run of day)\n";
            }
        } else {
            std::cout << "[OMEGA] Startup reload: no today CSV yet ("
                      << reload_path << ") -- clean start\n";
        }
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[OMEGA] WSAStartup failed\n"; return 1;
    }
    SSL_library_init(); SSL_load_error_strings(); OpenSSL_add_all_algorithms();

    if (!g_telemetry.Init()) std::cerr << "[OMEGA] Telemetry init failed\n";
    g_telemetry.SetMode(g_cfg.mode.c_str());
    g_telemetry.UpdateBuildVersion(OMEGA_VERSION, OMEGA_BUILT);
    if (g_telemetry.snap()) {
        g_telemetry.snap()->uptime_sec = 0;
        g_telemetry.snap()->start_time = g_start_time;
    }

    omega::OmegaTelemetryServer gui_server;
    std::cout.flush();  // flush before spawning GUI threads to avoid interleaved output
    gui_server.start(g_cfg.gui_port, g_cfg.ws_port, g_telemetry.snap());
    Sleep(200);  // let GUI threads print their startup lines before we continue
    std::cout << "[OMEGA] GUI http://localhost:" << g_cfg.gui_port
              << "  WS:" << g_cfg.ws_port << "\n";
    std::cout.flush();

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // ?? cTrader Open API depth feed (parallel to FIX -- read-only L2 data) ????
    // Provides real multi-level order book (ProtoOADepthEvent) to replace
    // FIX 264=1 single-level estimates. Runs in its own thread independently.
    if (g_cfg.ctrader_depth_enabled && !g_cfg.ctrader_access_token.empty() && g_cfg.ctrader_ctid_account_id > 0) {  // disabled: broker hasn't enabled Open API
        g_ctrader_depth.client_id           = "20304_NqeKlH3FEECOWqeP1JvoT2czQV9xkUHE7UXxfPU2dRuDXrZsIM";
        g_ctrader_depth.client_secret       = "jeYwDPzelIYSoDppuhSZoRpaRi1q572FcBJ44dXNviuSEKxdB9";
        g_ctrader_depth.access_token        = g_cfg.ctrader_access_token;
        g_ctrader_depth.refresh_token       = g_cfg.ctrader_refresh_token;
        g_ctrader_depth.ctid_account_id     = g_cfg.ctrader_ctid_account_id;
        g_ctrader_depth.l2_mtx              = &g_l2_mtx;
        g_ctrader_depth.l2_books            = &g_l2_books;
        // Do NOT load bar_failed from disk on startup.
        // The pre-seeded set below (XAUUSD:1 + live subs) is the correct blocked list.
        // Loading from disk adds stale entries (XAUUSD:0, XAUUSD:5, XAUUSD:7) that
        // permanently block the GetTickDataReq fallback -- causing vol_range=0.00 all day.
        // Evidence: April 2 2026 -- every restart loaded stale failures, bars never seeded,
        // GoldFlow ran blind all session.
        g_ctrader_depth.bar_failed_path_    = log_root_dir() + "/ctrader_bar_failed.txt";
        // load_bar_failed intentionally NOT called -- always start clean.
        // ?? PRIMARY PRICE SOURCE: cTrader depth ? on_tick ????????????????????
        // cTrader Open API streams every tick from the matching engine directly.
        // FIX quote feed can lag 0.5-2pts in fast markets due to gateway batching.
        // Proven: screenshot shows FIX=4643.54 while cTrader depth=4642.54 (1pt off).
        // Solution: cTrader depth drives on_tick() as primary price source.
        // FIX W/X handler calls on_tick() ONLY when cTrader depth is stale (>500ms).
        g_ctrader_depth.on_tick_fn = [](const std::string& sym, double bid, double ask) noexcept {
            // Track last cTrader tick time per symbol for FIX fallback staleness check
            set_ctrader_tick_ms(sym, std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            on_tick(sym, bid, ask);
        };
        // Stamp per-symbol tick time on EVERY depth event, not just when both sides present.
        // This prevents gold_size_dead from firing during incremental book fill and
        // triggering connection restarts that clear the book before L2 can stabilise.
        g_ctrader_depth.on_live_tick_ms_fn = [](const std::string& sym) noexcept {
            set_ctrader_tick_ms(sym, std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        };
        // Register atomic write callback -- cTrader thread writes derived scalars
        // (imbalance, microprice_bias, has_data) lock-free after each depth event.
        // FIX tick reads these atomics directly with no mutex contention at all.
        g_ctrader_depth.atomic_l2_write_fn = [](const std::string& sym, double imb, double mp, bool hd) noexcept {
            AtomicL2* al = get_atomic_l2(sym);
            if (!al) return;
            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            al->imbalance.store(imb, std::memory_order_relaxed);
            al->microprice_bias.store(mp, std::memory_order_relaxed);
            al->has_data.store(hd, std::memory_order_relaxed);
            al->last_update_ms.store(now_ms, std::memory_order_release);  // release: visible after imbalance/has_data
        };
        // Subscribe depth only for actively traded symbols -- not passive cross-pairs.
        // cTrader drops connection when too many depth streams are requested at once.
        for (int i = 0; i < OMEGA_NSYMS; ++i)
            g_ctrader_depth.symbol_whitelist.insert(OMEGA_SYMS[i].name);
        for (const auto& e : g_ext_syms)
            if (e.name[0] != 0) g_ctrader_depth.symbol_whitelist.insert(e.name);
        // Alternate broker names for gold/silver -- broker may not use .F suffix
        g_ctrader_depth.symbol_whitelist.insert("XAUUSD");
        g_ctrader_depth.symbol_whitelist.insert("SILVER");
        g_ctrader_depth.symbol_whitelist.insert("XAGUSD");
        g_ctrader_depth.symbol_whitelist.insert("NGAS");
        g_ctrader_depth.symbol_whitelist.insert("VIX");
        g_ctrader_depth.dump_all_symbols = false;  // audit complete -- USOIL.F id=2632 confirmed
        // Alias map: broker name ? internal name used by getImb/getBook
        // XAUUSD is already the canonical name -- no alias needed

        // ?? cTrader broker name ? internal name aliases ?????????????????????
        // BlackBull may use different names in cTrader Open API vs FIX feed.
        // All variants observed or expected mapped here.
        // Rule: internal name = FIX name (OMEGA_SYMS/g_ext_syms) -- aliases
        //       translate broker cTrader names to our internal names.
        // US indices
        g_ctrader_depth.name_alias["US500"]    = "US500.F";
        g_ctrader_depth.name_alias["SP500"]    = "US500.F";
        g_ctrader_depth.name_alias["SPX500"]   = "US500.F";
        g_ctrader_depth.name_alias["USTEC"]    = "USTEC.F";
        // NAS100 is NOT aliased to USTEC.F -- it is a separate cash instrument.
        // The NBM engine runs on "NAS100" and needs its own on_tick() calls.
        // USTEC.F (futures) and NAS100 (cash) have different prices.
        g_ctrader_depth.name_alias["NASDAQ"]   = "USTEC.F";
        g_ctrader_depth.name_alias["TECH100"]  = "USTEC.F";
        g_ctrader_depth.name_alias["US30"]     = "DJ30.F";
        g_ctrader_depth.name_alias["DJ30"]     = "DJ30.F";
        g_ctrader_depth.name_alias["DOW30"]    = "DJ30.F";
        g_ctrader_depth.name_alias["DOWJONES"] = "DJ30.F";
        // Metals
        g_ctrader_depth.name_alias["SILVER"]   = "XAGUSD";
        g_ctrader_depth.name_alias["XAGUSD"]   = "XAGUSD";
        // Oil -- USOIL.F id=2632 shows ~$102; may be Brent priced instrument
        // Aliases cover all known BlackBull oil names until dump_all_symbols confirms
        g_ctrader_depth.name_alias["USOIL"]    = "USOIL.F";
        g_ctrader_depth.name_alias["WTI"]      = "USOIL.F";
        g_ctrader_depth.name_alias["CRUDE"]    = "USOIL.F";
        g_ctrader_depth.name_alias["OIL"]      = "USOIL.F";
        g_ctrader_depth.name_alias["UKBRENT"]  = "BRENT";
        g_ctrader_depth.name_alias["BRENT.F"]  = "BRENT";
        // EU indices
        g_ctrader_depth.name_alias["GER30"]    = "GER40";   // old broker name
        g_ctrader_depth.name_alias["DAX"]      = "GER40";
        g_ctrader_depth.name_alias["DAX40"]    = "GER40";
        g_ctrader_depth.name_alias["FTSE100"]  = "UK100";
        g_ctrader_depth.name_alias["FTSE"]     = "UK100";
        g_ctrader_depth.name_alias["UK100"]    = "UK100";
        g_ctrader_depth.name_alias["STOXX50"]  = "ESTX50";
        g_ctrader_depth.name_alias["SX5E"]     = "ESTX50";
        g_ctrader_depth.name_alias["EUSTX50"]  = "ESTX50";
        // FX
        g_ctrader_depth.name_alias["EUR/USD"]  = "EURUSD";
        g_ctrader_depth.name_alias["GBP/USD"]  = "GBPUSD";
        g_ctrader_depth.name_alias["AUD/USD"]  = "AUDUSD";
        g_ctrader_depth.name_alias["NZD/USD"]  = "NZDUSD";
        g_ctrader_depth.name_alias["USD/JPY"]  = "USDJPY";
        // Other
        g_ctrader_depth.name_alias["NGAS"]     = "NGAS.F";
        g_ctrader_depth.name_alias["NATGAS"]   = "NGAS.F";
        g_ctrader_depth.name_alias["VIX"]      = "VIX.F";
        g_ctrader_depth.name_alias["VOLX"]     = "VIX.F";

        // ?? OHLC bar subscriptions -- XAUUSD M1+M5 only ???????????????????
        // XAUUSD spot id=41 (hardcoded, same as depth subscription).
        // On startup: requests 200 M1 + 100 M5 historical bars, then subscribes
        // live bar closes. Indicators (RSI, ATR, EMA, BB, swing, trend) are
        // written to g_bars_gold atomically and read by GoldFlow/GoldStack.
        //
        // REMOVED: US500.F and USTEC.F bar subscriptions.
        // ROOT CAUSE OF SESSION DESTRUCTION: BlackBull broker returns INVALID_REQUEST
        // for trendbar requests on US500.F and USTEC.F (cash/futures index instruments).
        // This causes read_one() to return rc=-1 (SSL connection drop) on EVERY reconnect,
        // immediately after the depth feed becomes stable. Effect:
        //   1. Reconnect cycle fires every 5s indefinitely
        //   2. XAUUSD M1 bars never seed (interrupted before 52 bars load)
        //   3. m1_ready=false -> Gates 3+4 inactive -> naked GoldFlow entries
        //   4. At 02:39 this caused a full process shutdown, missing the 8-min uptrend
        //
        // Evidence from logs: every single reconnect shows exactly:
        //   [CTRADER-BARS] USTEC.F history req period=1 count=200
        //   [CTRADER] Error:  -- INVALID_REQUEST
        //   [CTRADER] Connection error
        //
        // GER40 was removed earlier for the same reason (id=1899, same INVALID_REQUEST).
        // US500.F and USTEC.F now use tick-based vol estimation (same as GER40 fallback).
        // g_bars_sp and g_bars_nq remain allocated -- indicators just won't be seeded.
        // Engines that read g_bars_sp/g_bars_nq already handle m1_ready=false gracefully.
        g_ctrader_depth.bar_subscriptions["XAUUSD"]  = {41,   &g_bars_gold};
        // Index bar subscriptions removed -- broker sends INVALID_REQUEST for all
        // GetTrendbarsReq (pt=2137) calls on index symbols, dropping the TCP connection.
        // g_ctrader_depth.bar_subscriptions["US500.F"] = {2642, &g_bars_sp};
        // g_ctrader_depth.bar_subscriptions["USTEC.F"] = {2643, &g_bars_nq};
        // g_ctrader_depth.bar_subscriptions["GER40"]   = {1899, &g_bars_ger};

        // CRITICAL: Pre-seed bar_failed_reqs to permanently block GetTrendbarsReq (pt=2137).
        // BlackBull cTrader rejects pt=2137 with INVALID_REQUEST for ALL symbols/periods,
        // then sends a TCP RST which drops the live price feed connection.
        // This is the root cause of 2000+ reconnects per day.
        //
        // Fix: mark ALL period=1 requests as "failed" before start() so they are
        // skipped entirely. The dispatch loop routes them to GetTickDataReq (pt=2145)
        // instead, which BlackBull serves correctly.
        //
        // The bar_subscriptions loop uses period sentinels:
        //   period=1   -> was GetTrendbarsReq (pt=2137) -> NOW routed via tick fallback
        //   period=105 -> GetTickDataReq for M5
        //   period=107 -> GetTickDataReq for M15
        // With period=1 now also using tick fallback, pt=2137 is NEVER sent.
        g_ctrader_depth.bar_failed_reqs.insert("XAUUSD:1");        // block pt=2137 for XAUUSD M1
        // Pre-block live trendbar subs -- BlackBull sends TCP RST on pt=2135
        g_ctrader_depth.bar_failed_reqs.insert("XAUUSD:live:1");
        g_ctrader_depth.bar_failed_reqs.insert("XAUUSD:live:5");
        g_ctrader_depth.bar_failed_reqs.insert("XAUUSD:live:7");
        g_ctrader_depth.save_bar_failed(g_ctrader_depth.bar_failed_path_);
        std::cout << "[CTRADER] Pre-blocked trendbar reqs + live subs -- no crash loop\n";

        g_ctrader_depth.start();
        std::cout << "[CTRADER] Depth feed starting (ctid=" << g_cfg.ctrader_ctid_account_id << ")\n";

        // ?? Symbol subscription cross-check ??????????????????????????????????
        // Runs 5s after start() -- by then SymbolsListRes should have arrived
        // and all bar/depth subscriptions resolved.
        // Logs WARNING for any symbol that will fall back to FIX prices.
        std::thread([&]() {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            std::cout << "[CTRADER-AUDIT] Symbol subscription check:\n";
            // Check primary symbols
            for (int i = 0; i < OMEGA_NSYMS; ++i) {
                const std::string& name = OMEGA_SYMS[i].name;
                const bool has_ct = g_ctrader_depth.has_depth_subscription(name);
                std::cout << "[CTRADER-AUDIT]   " << name
                          << (has_ct ? " -> cTrader OK" : " -> *** FIX FALLBACK ONLY ***") << "\n";
            }
            // Check ext symbols
            for (const auto& e : g_ext_syms) {
                if (e.name[0] == 0) continue;
                const bool has_ct = g_ctrader_depth.has_depth_subscription(e.name);
                std::cout << "[CTRADER-AUDIT]   " << e.name
                          << (has_ct ? " -> cTrader OK" : " -> *** FIX FALLBACK ONLY ***") << "\n";
            }
            std::cout.flush();
        }).detach();
    } else {
        std::cout << "[CTRADER] Depth feed disabled -- add [ctrader_api] to omega_config.ini\n";
    }

    std::cout << "[OMEGA] FIX loop starting -- " << g_cfg.mode << " mode\n";

    // Push log to git on every startup so remote reads are never stale after restart.
    // Fire-and-forget -- does not block startup.
    std::system("cmd /c start /min powershell -WindowStyle Hidden"
                " -ExecutionPolicy Bypass"
                " -File C:\\Omega\\push_log.ps1 -RepoRoot C:\\Omega");

    // =========================================================================
    // STARTUP VERIFICATION THREAD
    // Checks all critical systems within 120s of launch.
    // Writes C:\Omega\logs\startup_status.txt:
    //   "OK"   = all systems go
    //   "FAIL: <reason>" = something is broken
    // DEPLOY_OMEGA.ps1 reads this file and aborts + alerts if FAIL.
    // =========================================================================
    std::thread([](){
        const std::string status_path = log_root_dir() + "/startup_status.txt";
        // Write STARTING immediately so deploy script knows process launched
        { std::ofstream f(status_path); f << "STARTING\n"; }

        const auto t0 = std::chrono::steady_clock::now();
        auto elapsed = [&]{ return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - t0).count(); };

        auto write_status = [&](const std::string& s) {
            std::ofstream f(status_path);
            f << s << "\n";
            std::cout << "[STARTUP-CHECK] " << s << "\n";
            std::cout.flush();
        };

        // --- Check 1: cTrader depth connects within 60s ---
        // cTrader waits 30s before first connect attempt, then 5-15s to auth
        while (elapsed() < 65) {
            if (g_ctrader_depth.depth_active.load()) break;
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        if (!g_ctrader_depth.depth_active.load()) {
            write_status("FAIL: cTrader depth not connected after 65s -- check token/network");
            return;
        }
        std::cout << "[STARTUP-CHECK] cTrader depth connected (" << elapsed() << "s)\n";
        std::cout.flush();

        // --- Check 2: L2 depth events flowing for XAUUSD ---
        // Check that cTrader is actually sending depth events (any side, not necessarily both).
        // has_data() requires BOTH bid AND ask sides simultaneously -- too strict at startup
        // Check FIX book (g_l2_books) which has real bid+ask from the FIX W/X feed.
        // cTrader atomic (g_l2_gold.imbalance) is ask-only from BlackBull depth.
        const auto t1 = std::chrono::steady_clock::now();
        bool l2_ok = false;
        while (std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - t1).count() < 30) {
            const double imb = g_l2_gold.imbalance.load(std::memory_order_relaxed);
            const bool   hd  = g_l2_gold.has_data.load(std::memory_order_relaxed);
            if (hd || imb < 0.499 || imb > 0.501) { l2_ok = true; break; }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!l2_ok) {
            const bool events_flowing = g_ctrader_depth.depth_events_total.load() > 10;
            if (events_flowing) {
                write_status("OK: cTrader live, L2 events flowing (book still filling)");
                return;
            }
            write_status("FAIL: Gold L2 data not flowing after 30s -- no depth events received");
            return;
        }
        std::cout << "[STARTUP-CHECK] Gold L2 live (" << elapsed() << "s)\n";
        std::cout.flush();

        // --- All checks passed ---
        const double imb = g_l2_gold.imbalance.load(std::memory_order_relaxed);
        write_status("OK: cTrader live, L2 live (imb=" + std::to_string(imb).substr(0,5) + ")");
    }).detach();
    // =========================================================================
    std::thread trade_thread(trade_loop);
    Sleep(500);  // Give trade connection 500ms head start before quote loop
    quote_loop();  // blocks until g_running=false

    // quote_loop has exited -- g_running is false, trade_loop will exit shortly.
    std::cout << "[OMEGA] Shutdown\n";
    // Stop cTrader depth feed before joining other threads
    g_ctrader_depth.stop();

    // Wait up to 5s for any pending close orders to be ACKed by broker before
    // tearing down the trade connection. Only matters in LIVE mode.
    if (g_cfg.mode == "LIVE") {
        const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < close_deadline) {
            std::lock_guard<std::mutex> lk(g_live_orders_mtx);
            bool any_pending = false;
            for (const auto& kv : g_live_orders)
                if (!kv.second.acked && !kv.second.rejected) { any_pending = true; break; }
            if (!any_pending) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::cout << "[OMEGA] Close orders settled\n";
    }

    // Wait up to 3s for trade_loop to finish its own logout+SSL_free sequence.
    {
        auto start = std::chrono::steady_clock::now();
        while (!g_trade_thread_done.load() &&
               std::chrono::steady_clock::now() - start < std::chrono::milliseconds(3000)) {
            Sleep(10);
        }
    }
    // If trade thread did not finish in 3s (stuck in SSL/reconnect), detach it
    // and force-exit the process. Calling join() on a stuck SSL thread hangs
    // indefinitely -- the process never exits and Ctrl+C appears to do nothing.
    // Evidence: [OMEGA] Shutdown printed but process hangs after [ADAPTIVE-RISK] lines.
    if (!g_trade_thread_done.load()) {
        std::cout << "[OMEGA] Trade thread still running after 3s -- detaching and forcing exit\n";
        std::cout.flush();
        if (trade_thread.joinable()) trade_thread.detach();
        // Flush/close logs before hard exit
        if (g_tee_buf) { g_tee_buf->flush_and_close(); std::cout.rdbuf(g_orig_cout); delete g_tee_buf; g_tee_buf = nullptr; }
        WSACleanup();
        ReleaseMutex(g_singleton_mutex);
        CloseHandle(g_singleton_mutex);
        g_shutdown_done.store(true);
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    }
    if (trade_thread.joinable()) trade_thread.join();
    gui_server.stop();
    if (g_daily_trade_close_log) g_daily_trade_close_log->close();
    if (g_daily_gold_trade_close_log) g_daily_gold_trade_close_log->close();
    if (g_daily_shadow_trade_log) g_daily_shadow_trade_log->close();
    if (g_daily_trade_open_log) g_daily_trade_open_log->close();
    g_trade_close_csv.close();
    g_trade_open_csv.close();
    g_shadow_csv.close();
    // Stop hot-reload watcher before saving state -- prevents reload during shutdown
    OmegaHotReload::stop();
    // ?? Edge systems shutdown -- persist TOD + Kelly + fill quality data ????????
    g_edges.tod.save_csv(log_root_dir() + "/omega_tod_buckets.csv");
    g_edges.tod.print_worst(15);
    g_edges.fill_quality.save_csv(log_root_dir() + "/fill_quality.csv");
    g_edges.fill_quality.print_summary();
    // Save Kelly performance on shutdown (not just rollover) so intra-session
    // trades survive process restart without re-warming for 15+ trades.
    g_adaptive_risk.save_perf(log_root_dir() + "/kelly");
    g_gold_flow.save_atr_state(log_root_dir() + "/gold_flow_atr.dat");
    g_gold_stack.save_atr_state(log_root_dir() + "/gold_stack_state.dat");
    g_trend_pb_gold.save_state(log_root_dir()  + "/trend_pb_gold.dat");
    g_trend_pb_ger40.save_state(log_root_dir() + "/trend_pb_ger40.dat");
    g_trend_pb_nq.save_state(log_root_dir()    + "/trend_pb_nq.dat");
    g_trend_pb_sp.save_state(log_root_dir()    + "/trend_pb_sp.dat");
    g_adaptive_risk.print_summary();
    if (g_tee_buf)   { g_tee_buf->flush_and_close(); std::cout.rdbuf(g_orig_cout); delete g_tee_buf; g_tee_buf = nullptr; }
    WSACleanup();
    ReleaseMutex(g_singleton_mutex);
    CloseHandle(g_singleton_mutex);
    g_shutdown_done.store(true);  // unblock console_ctrl_handler -- process may now exit
    return 0;
}



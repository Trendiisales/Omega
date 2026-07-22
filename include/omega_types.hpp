#pragma once  // OM-08 (audit 2026-07-13): TU-fragment guard
// omega_types.hpp -- extracted from main.cpp
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

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
    // Execution venue (2026-06-16 IBKR migration). BLACKBULL_FIX = legacy FIX path
    // (OmegaFIX, unchanged). IBKR = route orders via the native C++ IbkrExecutionEngine.
    // S-2026-06-29: DEFAULT FLIPPED BLACKBULL_FIX -> IBKR. The operator is no longer
    // on BlackBull for execution; the migration to IBKR is done (order_exec.hpp:140).
    // Leaving the default at BLACKBULL_FIX meant a mode=LIVE flip WITHOUT
    // OMEGA_EXECUTION_BROKER=IBKR (or the new ini key) would have silently routed REAL
    // orders to BlackBull. Strictly safer: IBKR exec is paper_only by default + refuses
    // on a live port, and send_live_order's hard SHADOW gate still blocks everything
    // while mode=SHADOW. The BlackBull cTrader FIX session itself stays -- it is now the
    // L2 MARKET-DATA feed only (FIX 264=0 full book), NOT the order path.
    std::string execution_broker = "IBKR";  // BLACKBULL_FIX | IBKR

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
    double dollar_stop_usd = 50.0;       // 0=disabled. Emergency runtime cut: close any position
                                         // immediately when unrealised loss exceeds this USD amount.
                                         // Independent of SL -- fires on live floating P&L every 250ms.
                                         // Default $50. Set 0 to disable.
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
    // S-2026-06-19 Phase 1 R-hierarchy (handoff item 1). Limits below are
    // expressed as multiples of base risk R (= risk_per_trade_usd). At the
    // 0.375%/$11k base, R=$41.25 -> weekly 7R, per-symbol 2R.
    double weekly_loss_limit = 0.0;      // 0=disabled. Block ALL new entries if rolling 7-day net loss > this (7R).
    double per_symbol_loss_limit = 0.0;  // 0=disabled. Block new entries on a SYMBOL once its daily net loss > this (2R).
    // S-2026-06-19 Phase 1 item 2: correlation-aware PortfolioGovernor (see
    // include/PortfolioGovernor.hpp). Flat fields here; the cfg struct is built
    // at the symbol_gate call site to avoid pulling the governor's includes
    // into this low-level header.
    bool   portfolio_governor_enabled    = false;  // master switch
    int    governor_max_total_campaigns  = 2;      // global cap on distinct-symbol campaigns
    int    governor_max_per_corr_group   = 1;      // 1 correlated index/equity campaign
    int    max_consec_losses = 3;
    int    loss_pause_sec    = 300;
    int    max_open_positions = 4;     // allow up to 4 concurrent positions across different symbols
    bool   independent_symbols = true;  // true: risk/position gating is per-symbol (recommended)
    int    auto_disable_after_trades  = 10;
    bool   shadow_ustec_pilot_only    = false;  // false: multi-symbol SHADOW research; true: restrict to GOLD + USTEC pilot
    bool   shadow_research_mode       = false;
    // S-2026-07-23a LIVE SIZING ([sizing] in omega_config.ini, hot-reloaded ~2s;
    // -1 = key absent -> code default stands). Applied with a loud
    // [SIZING-LIVE] ... CONFIRMED line on every change.
    double sizing_gold_lot        = -1.0;   // GoldDailyCbe parent (GLD shares / fut contracts)
    double sizing_gold_mimic_lot  = -1.0;   // GoldDailyCbeMimic per leg
    double sizing_stock_lot       = -1.0;   // StockDip/Turtle parents (shares)  // false: paper/live-like behavior; true: discovery mode with relaxed filters
    double ustec_pilot_size           = 0.35;   // reduced NQ pilot size vs default 1.0
    int    ustec_pilot_min_gap_sec    = 60;     // more opportunities than default 90s shadow NQ gap
    bool   ustec_pilot_require_session = true;  // keep USTEC pilot out of session-closed tape
    bool   ustec_pilot_require_latency = true;  // enforce latency gate in shadow for USTEC pilot
    bool   ustec_pilot_block_risk_off  = true;  // skip USTEC pilot in RISK_OFF regime
    bool   enable_extended_symbols    = true;   // subscribe + trade additional opportunity symbols
    bool   goldflow_enabled           = false;
    bool   indices_enabled            = true;   // false = disable all index engine on_tick calls
                                                // Avg winner $15 vs avg loser $74 = 0.20:1 payoff
                                                // 2yr MFE scan: microstructure signal, not structural

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
    double min_lot_fx      = 0.01;

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

    // Brent (UKBRENT)
    double brent_tp_pct                = 1.500;
    double brent_sl_pct                = 0.500;
    double brent_vol_thresh_pct        = 0.080;
    int    brent_min_gap_sec           = 90;
    double brent_momentum_thresh_pct   = 0.050;
    double brent_min_breakout_pct      = 0.060;
    double brent_max_spread_pct        = 0.300;  // 2026-05-28 (S37-Z): raised 0.120->0.300. BRENT BlackBull demo spread runs 0.12-0.20 ($0.10-0.16 abs at $78); 0.120% threshold tripped HIGH_RISK_NO_TRADE permanently -> [SUPERVISOR-BRENT] IN_COOLDOWN loop (37 events / 4hr observed 2026-05-27). 0.300% covers $0.23 abs spread at $77 with margin.
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

    // Bracket engines (Gold)
    int    bracket_gold_lookback      = 40;
    double bracket_gold_tp_pct        = 0.25;
    double bracket_gold_sl_pct        = 0.12;
    double bracket_gold_min_range_pct = 0.04;
    double bracket_gold_max_spread_pct = 0.10;  // 2026-05-28 (S37-Z): raised 0.06->0.10. At XAU $4452 0.06% = $2.67 cap; live NY-close roll widens spread to $3-5 ($0.07-0.11%) triggering [SUPERVISOR-XAUUSD] HIGH_RISK_NO_TRADE for 49 events / 4hr (2026-05-27 log). 0.10% = $4.45 covers typical roll without permanent block. Real broker rejection still gated by exec_score < 0.1.
    int    bracket_gold_min_gap_sec   = 90;
    int    bracket_gold_cooldown_sl_sec = 120;
    int    bracket_gold_max_hold_sec  = 1800;

    // GUI
    int         gui_port   = 7779;
    int         ws_port    = 7780;
    int         trade_port = 5212;   // FIX trade connection (orders)
    std::string shadow_csv = "omega_shadow.csv";
    // S25 2026-05-25: signal-level audit (every supervisor decision change).
    // Implemented after 2026-05-25 silent-loss incident: config keys existed
    // but no writer was wired, so signals never landed on disk.
    std::string shadow_signal_csv        = "omega_shadow_signals.csv";
    bool        enable_shadow_signal_audit = false;
    std::string log_file   = "";   // if set, tee all stdout+stderr here

    // GoldEngineStack config -- passed to g_gold_stack.configure()
    omega::gold::GoldStackCfg gs_cfg;

    // cTrader Open API config fields removed S13 2026-05-08 -- cTrader
    // surface culled; FIX 264=0 provides full L2.

    // LatencyEdgeStack le_cfg REMOVED at S13 Finding B 2026-04-24 — engine culled.
};

static OmegaConfig         g_cfg;
static std::atomic<bool>   g_running(true);
static std::atomic<bool>   g_emergency_close(false);  // set by GUI button ? closes all positions immediately

// cTrader Open API depth client removed S13 2026-05-08 -- FIX 264=0 provides L2.
// OHLC bar state -- populated by cTrader trendbar API (M1+M5 history + live bars)
// Read lock-free by flow engines and GoldStack via atomic accessors.
static SymBarState         g_bars_gold;   // XAUUSD M1/M5/H1/H4 bars + indicators
static SymBarState         g_bars_sp;     // US500.F M1/M5/H1 bars + indicators
static SymBarState         g_bars_nq;     // USTEC.F M1/M5/H1 bars + indicators
static SymBarState         g_bars_ger;    // GER40   M1/M5 bars + indicators
// FX bar-state (S-2026-07-06): feeds the desk price chart (/api/predictive_ranges) AND
// the FX BE-floor companion's H1 close stream. FX is subscribed for macro context (see
// tick_fx.hpp); these aggregate its ticks into M1/M5/M15/H1 bars. Chart display + companion
// only -- no FX trading engine (the 2026-06-29 FX removal stands for execution).
static SymBarState         g_bars_eurusd; // EURUSD  M1/M5/M15/H1 bars (chart + FX companion)
static SymBarState         g_bars_gbpusd; // GBPUSD
static SymBarState         g_bars_usdjpy; // USDJPY
static SymBarState         g_bars_audusd; // AUDUSD
static SymBarState         g_bars_nzdusd; // NZDUSD
static SymBarState         g_bars_usdcad; // USDCAD (S-2026-07-08c: DOWN-JUMP SHORT ladder companion feed)
// IndexSwingEngine -- H1+H4 swing entries for US500.F and USTEC.F
// shadow_mode=true always; configured in engine_init.hpp
// sl_pts / min_ema_sep / pnl_scale per-symbol calibration:
//   SP: sl_pts=8pt,  min_ema_sep=0.5pt, pnl_scale=0.5 ($50/pt * 0.01lot = $0.50/pt)
//   NQ: sl_pts=25pt, min_ema_sep=1.5pt, pnl_scale=0.2 ($20/pt * 0.01lot = $0.20/pt)
static omega::idx::IndexSwingEngine g_iswing_sp("US500.F",  8.0, 0.5, 0.5);
static omega::idx::IndexSwingEngine g_iswing_nq("USTEC.F", 25.0, 1.5, 0.2);

// S38 walk-forward survivors portfolio (13 cells across XAU/GER40/USTEC/USDJPY).
// Configured + seeded in engine_init.hpp. Per-cell enabled/shadow flags.
#include "SurvivorPortfolio.hpp"
static omega::survivor::Portfolio g_survivor;
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

// globals.hpp -- extracted from main.cpp
// Section: globals (original lines 435-964)
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

// ?? Per-symbol config manager -- loaded from symbols.ini at startup ????????????
static SymbolConfigManager g_sym_cfg;

static omega::SpEngine    g_eng_sp("US500.F");
static omega::NqEngine    g_eng_nq("USTEC.F");
static omega::OilEngine   g_eng_cl("USOIL.F");
static omega::Us30Engine  g_eng_us30("DJ30.F");
static omega::Nas100Engine g_eng_nas100("NAS100");
static omega::EuIndexEngine  g_eng_ger30("GER40");
static omega::EuIndexEngine  g_eng_uk100("UK100");
static omega::EuIndexEngine  g_eng_estx50("ESTX50");
static omega::BreakoutEngine g_eng_eurusd("EURUSD");
static omega::BreakoutEngine g_eng_gbpusd("GBPUSD");
static omega::BrentEngine    g_eng_brent("BRENT");
static omega::BreakoutEngine g_eng_audusd("AUDUSD");
static omega::BreakoutEngine g_eng_nzdusd("NZDUSD");
static omega::BreakoutEngine g_eng_usdjpy("USDJPY");

// Shared macro context -- updated each tick, read by SP/NQ shouldTrade()
static omega::MacroContext g_macro_ctx;

// SessionMomentum + VWAPSnapback + LiquiditySweepPro + LiquiditySweepPressure
// Primary gold executor -- sole handler for all XAUUSD ticks.
static omega::gold::GoldEngineStack g_gold_stack;
// Cross-asset engines
static omega::cross::EsNqDivergenceEngine  g_ca_esnq;
static omega::cross::OilEventFadeEngine    g_ca_eia_fade;
static omega::cross::BrentWtiSpreadEngine  g_ca_brent_wti;
static omega::cross::FxCascadeEngine       g_ca_fx_cascade;
static omega::cross::CarryUnwindEngine     g_ca_carry_unwind;
static omega::cross::OpeningRangeEngine    g_orb_us;     // US equity 13:30 UTC
static omega::cross::OpeningRangeEngine    g_orb_ger30;  // Xetra 08:00 UTC
static omega::cross::OpeningRangeEngine    g_orb_uk100;  // LSE 08:00 UTC, 15-min window
static omega::cross::OpeningRangeEngine    g_orb_estx50; // Euronext 09:00 UTC, 15-min window

// Engine 7: VWAP Reversion -- enter on reversal tick back toward daily VWAP
// Wired to: US500.F, USTEC.F, GER40, EURUSD
static omega::cross::VWAPReversionEngine   g_vwap_rev_sp;     // US500.F
static omega::cross::VWAPReversionEngine   g_vwap_rev_nq;     // USTEC.F
static omega::cross::VWAPReversionEngine   g_vwap_rev_ger40;  // GER40
static omega::cross::VWAPReversionEngine   g_vwap_rev_eurusd; // EURUSD

// Engine 9: Noise Band Momentum -- Zarattini/Aziz/Maroy research (Sharpe 3.0-5.9)
// Rolling ATR noise band since session open. Entry on band breakout.
// VWAP crossing is primary stop. One instance per instrument.
// Wired to: US500.F, USTEC.F, NAS100, DJ30.F
static omega::cross::NoiseBandMomentumEngine g_nbm_sp;    // US500.F -- NY 13:30-21:30 UTC
static omega::cross::NoiseBandMomentumEngine g_nbm_nq;    // USTEC.F -- NY 13:30-21:30 UTC
static omega::cross::NoiseBandMomentumEngine g_nbm_nas;   // NAS100  -- NY 13:30-21:30 UTC
static omega::cross::NoiseBandMomentumEngine g_nbm_us30;  // DJ30.F  -- NY 13:30-21:30 UTC

// NBM London session engines (07:00-13:30 UTC) -- covers the gap before NY open.
// XAUUSD and USOIL.F are the most liquid instruments in the London window.
// Session anchor = London open (07:00 UTC). Same ATR/band logic as NY engines.
// These are additional instances -- the gold stack and oil engines remain primary.
static omega::cross::NoiseBandMomentumEngine g_nbm_gold_london;  // XAUUSD  -- London 07:00-13:30 UTC
static omega::cross::NoiseBandMomentumEngine g_nbm_oil_london;   // USOIL.F -- London 07:00-13:30 UTC

//  both stripped. XAGUSD hard-blocked at on_tick.hpp routing layer. See

// Engine 8: Trend Pullback -- EMA9/21/50 trend + pullback to EMA50 + bounce confirmation
// Wired to: XAUUSD (gated -- no other gold position), GER40, USTEC.F, US500.F
// TrendPullback handles slow grind trades that VWAPReversion times out on.
// Enters on EMA50 pullback, trails ATR behind MFE, no timeout.
static omega::cross::TrendPullbackEngine   g_trend_pb_gold;   // XAUUSD
static omega::cross::TrendPullbackEngine   g_trend_pb_ger40;  // GER40
static omega::cross::TrendPullbackEngine   g_trend_pb_nq;     // USTEC.F
static omega::cross::TrendPullbackEngine   g_trend_pb_sp;     // US500.F

// HTF swing engines -- H1 trend + H4 regime breakout for XAUUSD
// H1SwingEngine:  ADX-filtered EMA pullback, 4-16hr hold, $15 risk, shadow_mode=true
// H4RegimeEngine: Donchian channel breakout, 1-3 day hold, $10 risk, shadow_mode=true
// Both start in shadow_mode. Never set shadow_mode=false without live validation.
static omega::H1SwingEngine  g_h1_swing_gold;   // XAUUSD H1 EMA+ADX trend
static omega::H4RegimeEngine g_h4_regime_gold;  // XAUUSD H4 Donchian breakout

// MinimalH4Breakout -- pure H4 Donchian breakout, no filters. Validated via
// 2yr tick sweep (27/27 configs profitable), walk-forward PF 1.35 OOS,
// cost stress PF 1.31 pessimistic, 13-day live L2 replay confirmed signals.
// Runs PARALLEL to H4RegimeEngine in shadow mode (independent, not mutex).
// See backtest/htf_bt_minimal.cpp + htf_bt_walkforward.cpp + htf_bt_costs.cpp.
// Created 2026-04-24 Session 11 Stage 1.
#include "MinimalH4Breakout.hpp"
static omega::MinimalH4Breakout g_minimal_h4_gold;  // XAUUSD pure H4 Donchian breakout

// MinimalH4US30Breakout -- DJ30.F sister of MinimalH4Breakout. Self-contained:
// builds its own H4 OHLC bars and ATR14 internally from tick stream (no
// g_bars_us30 exists -- BlackBull rejects trendbar API for index symbols).
// Validated via 2yr Tickstory tick sweep on DJ30.F: 27/27 configs profitable,
// best PnL config (D=10 SL=1.0x TP=4.0x): n=184, PF=1.54, +$637, WR=28.3%.
// See backtest/htf_bt_US30_results.txt + htf_bt_multi.cpp.
// Runs in shadow mode with cold-start warmup of ~40hrs (10 H4 bars).
// Created 2026-04-25.
#include "MinimalH4US30Breakout.hpp"
static omega::MinimalH4US30Breakout g_minimal_h4_us30;  // DJ30.F pure H4 Donchian breakout

// C1RetunedPortfolio -- Python-side Phase 2 winner ported to C++ for live shadow.
// Verdict source: phase2/donchian_postregime/CHOSEN.md.
// Backtest baseline: +74.12% / -5.85% / PF 1.486 / Sharpe 2.651 / WR 55.2%.
// Walk-forward TRAIN/VALIDATE/TEST all PASS. Post-regime PF 1.334 -> 1.630.
// Long-only, XAUUSD only, max_concurrent=4, 0.5% risk, shadow_mode=true default.
// Added 2026-04-29 Session "switch on viable system".
#include "C1RetunedPortfolio.hpp"
static omega::C1RetunedPortfolio g_c1_retuned;  // Donchian H1 + Bollinger H2/H4/H6 long

// TsmomPortfolio -- Tier-1 ship of 5 long tsmom cells (H1/H2/H4/H6/D1)
// from phase1/signal_discovery/POST_CUT_FULL_REPORT.md.
// Post-cut backtest: 27 of 32 master_summary cells survive; tsmom long
// family = 82% of total simulated edge. Long-only XAUUSD, max_concurrent=5,
// 0.5% risk, shadow_mode=true default. Pre-warms from
// phase1/signal_discovery/tsmom_warmup_H1.csv (6,156 H1 bars) so every cell
// is READY when the first live H1 bar arrives -- no cold-start window.
// Added 2026-04-30 Session "Tier-1 tsmom shipdown to Omega shadow".
#include "TsmomEngine.hpp"
static omega::TsmomPortfolio g_tsmom;  // 5 long cells: H1, H2, H4, H6, D1

// Disabled 2026-04-16 after 6-day sweep / 1.5M ticks showed no edge across 7776 configs.

// =============================================================================
// IndexFlowEngine -- L2 flow + EWM drift engines for US equity indices.
// Architecture: L2 persistence + EWM drift + ATR-prop SL
// + staircase trail. Per-symbol calibrated (see IndexFlowEngine.hpp).
//
// SHADOW mode: IndexMacroCrashEngine instances are shadow-only by default.
// NEVER set shadow_mode=false without explicit authorization.
//
// L2 data: fed from existing AtomicL2 instances (g_l2_sp, g_l2_nq, g_l2_nas,
// g_l2_us30) already updated by cTrader depth thread in omega_main.hpp.
// Pass l2_imb via: g_l2_sp.imbalance.load(std::memory_order_relaxed)
// =============================================================================
static omega::idx::IndexFlowEngine       g_iflow_sp("US500.F");
static omega::idx::IndexFlowEngine       g_iflow_nq("USTEC.F");
static omega::idx::IndexFlowEngine       g_iflow_nas("NAS100");
static omega::idx::IndexFlowEngine       g_iflow_us30("DJ30.F");
static omega::idx::IndexMacroCrashEngine g_imacro_sp("US500.F"); // shadow_mode=true always
static omega::idx::IndexMacroCrashEngine g_imacro_nq("USTEC.F"); // shadow_mode=true always

// VWAPAtrTrail -- ATR-proportional BE lock + trail upgrade for existing
// VWAPReversionEngine instances. Holds upgrade state only (no new entries).
// Applied each tick after g_vwap_rev_sp/nq.on_tick() in tick_indices.hpp.
static omega::idx::VWAPAtrTrail g_vwap_atr_trail_sp;   // US500.F
static omega::idx::VWAPAtrTrail g_vwap_atr_trail_nq;   // USTEC.F
static omega::idx::VWAPAtrTrail g_vwap_atr_trail_nas;  // NAS100 (no VWAPRev, unused)
static omega::idx::VWAPAtrTrail g_vwap_atr_trail_us30; // DJ30.F (no VWAPRev, unused)

// Co-location latency edge stack -- GoldSpreadDislocation + GoldEventCompression.
// GoldSilverLeadLag DELETED 2026-03-31. Both remaining engines run MANAGE-ONLY
// (new entries disabled -- RTT ~68ms, edge requires <1ms).
// (LatencyEdgeStack culled S13 Finding B 2026-04-24 — VPS RTT ~68ms,
//  latency edge requires <1ms; stack was hardcoded to no-op returns.
//  See backtest/CULL_S13_LATENCY_EDGE.md.)

// ?? Cross-engine deduplication -- file-scope so dispatch lambda can see it ??
// Per-symbol timestamp of the last entry across ALL engine types.
// Prevents simultaneous entries from GoldStack + Breakout + Bracket + TrendPB.
static constexpr int64_t CROSS_ENG_DEDUP_SEC = 30;
static std::mutex         g_dedup_mtx;
static std::unordered_map<std::string, int64_t> g_last_cross_entry;

// Bracket engines
#include "BracketEngine.hpp"
#include "MacroCrashEngine.hpp"
static omega::GoldBracketEngine   g_bracket_gold;

// ?? Hybrid bracket engines -- fire both sides simultaneously, cancel loser ??
// GoldHybridBracketEngine: compression range -> long stop + short stop -> cancel loser on fill
// IndexHybridBracketEngine: same for SP/NQ/DJ30/NAS100 with per-symbol calibration
// These are in SHADOW mode by default -- validated against live data before enabling.
#include "GoldHybridBracketEngine.hpp"
#include "IndexHybridBracketEngine.hpp"
static omega::GoldHybridBracketEngine         g_hybrid_gold;
static omega::idx::IndexHybridBracketEngine   g_hybrid_sp(omega::idx::make_sp_config());
static omega::idx::IndexHybridBracketEngine   g_hybrid_nq(omega::idx::make_nq_config());
static omega::idx::IndexHybridBracketEngine   g_hybrid_us30(omega::idx::make_us30_config());
static omega::idx::IndexHybridBracketEngine   g_hybrid_nas100(omega::idx::make_nas100_config());
static omega::MacroCrashEngine    g_macro_crash;
// (g_pullback_cont and g_pullback_prem removed S49 X5 — engine culled, see commit message of branch s49-x5-pullback-cull)

// CandleFlowEngine RESTORED 2026-04-29 with audit-tightened gates ----------
// S19 (2026-04-24) culled CFE after a 576-config sweep showed zero
// profitable tunings (best -$336 WR 36% on 222 trades). The 2026-04-29
// audit revisits the cull thesis -- overtrading at ~450/day is the cause
// of the bleed, not a fundamental signal-edge problem. Audit fix raises
// CFE_BODY_RATIO_MIN, CFE_COST_MULT, CFE_RSI_THRESH, CFE_DFE_DRIFT_*,
// and turns HMM_GATING_LIVE on, slashing the entry rate by ~50% and
// requiring stricter setups. Re-enabled in shadow_mode only. Will not
// place live orders. Validate with N>=30 shadow trades AND a multi-month
// backtest sweep before considering LIVE. If shadow shows the same bleed,
// re-cull. See backtest/cfe_sweep_v2.cpp for validation harness.
#include "CandleFlowEngine.hpp"
static omega::CandleFlowEngine    g_candle_flow;  // candle+DOM engine, shadow only


// 11-day / 3.4M tick full-L2 validation sweep on all available XAUUSD L2
//   Baseline    : T=4469 WR=20.8% PnL=-$12,770.36  (every day lost money)
//   576 configs : ZERO profitable, best=-$336.39 WR=36.0% N=222
// The engine overtrades (~450 trades/day), bleeds spread + commission on
// every marginal signal. Even the "best" tuned config across 576 parameter
// Dollar-stop / startup-lock / TOD-deadzone / HMM gating / DOM entry all
// combined cannot rescue the underlying signal. The signal has no edge.
// 41-cell REENTER_COMP walk-forward sweep + subsequent baseline analysis
// of XAUUSD tick data (train 04-13..17 / test 04-20..23). Best cell
// tol1000_be1_h5000: train -$35.84 / test -$6.36. Disabled baseline:
// train -$36.60 / test -$2.23. No parameter combination produced
// positive net PnL on either split. Engine entirely removed rather than

// EMACrossEngine -- EMA9/15 crossover scalper, both directions
// Sweep-confirmed 2026-04-16: 99 trades/6days, 46.5% WR, $402/6days = $67/day
// fast=9 slow=15 rsi_lo=40 rsi_hi=50 sl=1.5 rr=1.0 cross_exit=true
// shadow_mode=true until 2-week live shadow validates
#include "EMACrossEngine.hpp"
static omega::EMACrossEngine g_ema_cross;

// Original 3-day/1.86M tick sweep claimed T=22 WR=68.2% PnL=$594. That sample
// was too small to trust. Full 11-day/3.4M tick validation sweep on all
// available XAUUSD L2 data (l2_ticks 2026-04-09..23) shows NO edge:
//   Baseline    : T=285 WR=24.6% PnL=-$1171.82 MaxDD=$1679.46
//   Best-tuned  : T=51  WR=33.3% PnL=+$228.49  MaxDD=$240.58  (MaxDD > PnL)
// Even after 288-config grid sweep, no configuration achieves WR >= 40% or
// Sharpe-worthy risk-adjusted returns. Best-tuned config is SHORT-only,
// London-only, with MaxDD > PnL — not a validated edge.
// See backtest/bb_tuned_sweep_v2.cpp for the validation sweep that killed it.

#include "PDHLReversionEngine.hpp"
static omega::PDHLReversionEngine g_pdhl_rev;     // mean reversion inside daily range

#include "RSIReversalEngine.hpp"
static omega::RSIReversalEngine   g_rsi_reversal;

// RSIExtremeTurnEngine -- backtest-validated RSI extreme + sustained turn
// Entry: RSI <20 after 3 bars sustained fall (LONG), >70 after 3 bars rise (SHORT)
// Exit:  RSI crosses back to 55 (LONG) or 45 (SHORT). SL=0.5xATR. No DOM.
// shadow_mode=true until live validation
#include "RSIExtremeTurnEngine.hpp"
static omega::RSIExtremeTurnEngine g_rsi_extreme;  // RSI extreme + sustained turn engine

// Real-tick backtest result: 4320 trades / 2 years, -$3.8k -- momentum = negative EV.
// Walk-forward sweep (96 cells, 14 days of L2 data, T=116 on production params)
// showed no edge at any threshold x persist_ticks x session_filter combination.
// Production params (i=0.05 p=5 London+NY): WR=22%, -$47 over 14 days, MaxDD=$65.
// Higher thresholds starved the signal to T<=2 per 14 days -- untradeable.
// Sweep output: backtest/dpe_sweep/summary.csv, leaderboard_oos.csv.
// Full removal: engine header, globals.hpp/engine_init.hpp/tick_gold.hpp/
//               omega_main.hpp/gold_coordinator.hpp call sites deleted.

//  + GF_* constants removed S19 Stage 1B — engine culled)

// Engine pause tracking -- maps engine key to pause_until epoch sec
// Used by health watchdog to detect consecutive loss pauses
static std::unordered_map<std::string, int64_t> g_engine_pause;
// L2 watchdog: set true when cTrader depth (ctid=43014358) not flowing > 120s.
// Cleared automatically when L2 recovers. Written by L2 watchdog thread in omega_main.hpp.
// IMMUTABLE: ctid=43014358 is the only account delivering L2 depth.
static std::atomic<bool>     g_l2_watchdog_dead{false};   // true = L2 dead, XAUUSD entries blocked

// ?? FEED-STALE gate ??????????????????????????????????????????????????????????
// Set true when cTrader depth has subscribed for XAUUSD but delivered ZERO depth
// events for >= FEED_STALE_THRESHOLD_S seconds after subscription.
// Root cause of April 5/6 frozen-session: broker ACKs the depth sub but sends no
// events, leaving FIX fallback active with a stale cached price from the prior
// session. This produces 452 identical ticks (4677.03/4677.25) with vol_range=0.00
// for the entire session while the real market moves $40.
//
// BEHAVIOUR WHEN SET:
//   - Position management (SL/trail) continues unaffected
//   - [FEED-STALE] logged once per 60s so the frozen state is unmissable
//   - CTraderDepthClient escalates: re-subscribe -> full reconnect
//   - Cleared when XAUUSD depth events resume flowing
//
// Written by CTraderDepthClient recv_loop (per-symbol starvation watchdog).
// Read by tick_gold.hpp entry gates.
std::atomic<bool>            g_feed_stale_xauusd{false};  // true = cTrader depth subscribed but no XAUUSD events

// ?? Indices FORCE_CLOSE circuit breaker ??????????????????????????????????????
// Problem: on April 2 2026, repeated disconnect/reconnect cycles on US indices
// allowed engines to re-enter immediately after each reconnect FORCE_CLOSE,
// compounding losses -$340. Root cause: FORCE_CLOSE fires on disconnect, then
// the reconnect warmup (30s) expires, and the engine re-enters into the same
// losing conditions with no memory of the prior close.
//
// Fix: stamp a 30-minute indices cooldown whenever a FORCE_CLOSE fires on any
// US index symbol. New entries on ALL US index engines are blocked for 30 minutes.
// The cooldown is per-symbol-group (US indices share one timer) so a NQ disconnect
// doesn't block a simultaneously running SP position.
//
// US indices covered: US500.F, USTEC.F, DJ30.F, NAS100
// EU indices (GER40, UK100, ESTX50) are separate -- different session, independent issue.
//
// Cooldown: 30 min = 1800s. Long enough to cover a full disconnect/reconnect cycle
// and enough time for the regime causing the reconnect loop to resolve.
static constexpr int64_t INDICES_DISCONNECT_COOLDOWN_SEC = 1800; // 30 min
static std::atomic<int64_t> g_indices_disconnect_until{0}; // epoch sec: block US index new entries until
// US equity index bracket engines -- arms both sides on compression,
// captures the move regardless of direction. Eliminates wrong-direction losses.
static omega::BracketEngine g_bracket_sp;
static omega::BracketEngine g_bracket_nq;
static omega::BracketEngine g_bracket_us30;
static omega::BracketEngine g_bracket_nas100;
// EU index bracket engines
static omega::BracketEngine g_bracket_ger30;
static omega::BracketEngine g_bracket_uk100;
static omega::BracketEngine g_bracket_estx50;
// Oil/Brent bracket
static omega::BracketEngine g_bracket_brent;
// g_bracket_cl removed -- USOIL.F bracket was never dispatched in on_tick (dead code).
// Oil uses g_eng_cl (breakout) + g_ca_eia_fade + g_ca_brent_wti only.
// FX bracket engines
static omega::BracketEngine g_bracket_eurusd;
static omega::BracketEngine g_bracket_gbpusd;
static omega::BracketEngine g_bracket_audusd;
static omega::BracketEngine g_bracket_nzdusd;
static omega::BracketEngine g_bracket_usdjpy;

// Bracket trade frequency tracking
static int      g_bracket_gold_trades_this_minute = 0;
static int64_t  g_bracket_gold_minute_start       = 0;
// Rate-limit vars for new bracket engines (shared int/int64 pairs)
static int      g_bracket_idx_trades_this_minute  = 0;
static int64_t  g_bracket_idx_minute_start        = 0;
static int      g_bracket_fx_trades_this_minute   = 0;
static int64_t  g_bracket_fx_minute_start         = 0;

// ?? Per-symbol bracket trend bias system ?????????????????????????????????????
// Applies to ALL bracket symbols, not just gold.
//
// Problem: during a strong trend (e.g. gold -$66 in 20min), the bracket
// re-arms on every compression. The trend-direction leg wins; the counter-
// trend leg fires on dead-cat bounces ? BREAKOUT_FAIL / SL_HIT losses.
//
// Solution: track consecutive profitable exits per symbol. When N same-
// direction wins occur within TREND_WINDOW_MS, set a trend bias:
//   - Counter-trend re-arm is BLOCKED for COUNTER_BLOCK_MS
//   - L2 imbalance EXTENDS the block (confirming trend) or SHORTENS it (opposing)
//   - When bias is active and L2 strongly confirms, PYRAMIDING is allowed:
//     a second bracket arm in the trend direction with reduced size
//
// L2 integration:
//   l2 > L2_STRONG_THRESHOLD (0.70): bid-heavy ? long pressure ? confirms LONG bias,
//                                     opposes SHORT bias ? shorten SHORT bias block
//   l2 < L2_WEAK_THRESHOLD  (0.30): ask-heavy ? short pressure ? confirms SHORT bias,
//                                    opposes LONG bias ? shorten LONG bias block
//   0.30-0.70: neutral ? no adjustment
//
// Pyramiding: when trend bias is active AND L2 strongly confirms direction AND
// the trend-direction position is already open ? allow a second arm at reduced
// size (PYRAMID_SIZE_MULT) to compound the move.

// Types + constants + g_bracket_trend map + bracket_trend_bias accessor extracted
// to BracketTrendState.hpp at Session 6 P1 engine 1/8 so OmegaBacktest can link
// the accessor (it pulls GoldEngineStack.hpp but not globals.hpp). See that
// header for the full definitions -- behaviour is byte-identical in main Omega.
#include "BracketTrendState.hpp"

// Tracks which symbols currently have an active pyramid (add-on) bracket arm.
// Set when a pyramid bracket fires; cleared on any close for that symbol.
// On SL_HIT while symbol is in this set ? records last_pyramid_sl_ms.
static std::unordered_set<std::string> g_pyramid_clordids;

// ?? Per-symbol supervisors -- one per traded symbol ????????????????????????????
// Each supervisor classifies regime and grants engine permissions each tick.
// Config loaded from symbols.ini via apply_supervisor() at startup.
static omega::SymbolSupervisor g_sup_sp,      g_sup_nq,     g_sup_cl;
static omega::SymbolSupervisor g_sup_us30,    g_sup_nas100;
static omega::SymbolSupervisor g_sup_ger30,   g_sup_uk100,  g_sup_estx50;
static omega::SymbolSupervisor g_sup_eurusd, g_sup_gbpusd;
static omega::SymbolSupervisor g_sup_audusd,  g_sup_nzdusd, g_sup_usdjpy;
static omega::SymbolSupervisor g_sup_brent,   g_sup_gold;
// false-break counters per symbol (reset on cooldown / regime change)
static std::mutex g_false_break_mtx;
static std::unordered_map<std::string, int> g_false_break_counts;

// Book
static std::mutex                              g_book_mtx;
static std::unordered_map<std::string,double>  g_bids;

// ?? Passive L2 observer symbols ???????????????????????????????????????????????
// ?? Passive L2 observer symbols (whitelist-controlled) ????????????????????????
// Subscribe to these cross-pairs for L2 imbalance data ONLY -- never traded.
// WHY WHITELIST: SecurityList has 1481 broker symbols. Subscribing ALL would
// produce a massive FIX message and flood on_tick with unusable pairs.
// We selectively subscribe to ~10 high-signal cross-pairs that confirm our
// traded symbol directions without adding noise.
//
// Filtered out automatically:
//   - .P suffix  = premium/STP feed variants (duplicate data, different ID)
//   - .I suffix  = index variants
//   - .F suffix  = futures variants (we already handle BRENT, USOIL.F directly)
//   - Exotic/illiquid: EURHUF, EURNOK, GBPZAR, SEKJPY, MXNJPY etc.
//
// Cross-pair values used by engines:
//   EURJPY ? CarryUnwind risk-off confirmation (better than VIX alone)
//   GBPJPY ? GBPUSD FxCascade direction amplifier
//   EURGBP ? FxCascade pair selection (which pair leads EUR vs GBP)
//   AUDCAD ? USOIL/XAGUSD commodity flow proxy
//   AUDJPY ? Risk-off confirmation (classic carry barometer)
//   NZDJPY ? NZDUSD FxCascade confirmation
//   EURCHF ? Safe-haven flow (confirms risk-off alongside JPY)
//   CHFJPY ? Pure safe-haven intensity
static const std::unordered_set<std::string> PASSIVE_WHITELIST = {
    "EURJPY", "GBPJPY", "EURGBP", "AUDCAD",
    "AUDJPY", "NZDJPY", "EURCHF", "CHFJPY",
};
// O(1) lookup: name -> true for passive symbols (populated from SecurityList)
static std::mutex                              g_passive_syms_mtx;
static std::unordered_set<std::string>         g_passive_sym_names;  // name set for fast dispatch check
static std::unordered_map<int, std::string>    g_passive_syms;       // id -> name (for subscription)

// Cross-pair imbalance cache -- updated under g_l2_mtx, read each tick
struct CrossPairL2 {
    double eurjpy = 0.5;  // >0.65=EUR bid (risk-on); <0.35=JPY bid (risk-off)
    double gbpjpy = 0.5;  // GBPUSD cascade amplifier
    double eurgbp = 0.5;  // >0.5=EUR leads; <0.5=GBP leads (cascade selection)
    double audcad = 0.5;  // commodity flow for oil/silver direction
    double audjpy = 0.5;  // risk-off barometer (carry unwind confirmation)
    double nzdjpy = 0.5;  // NZDUSD cascade confirmation
    double eurchf = 0.5;  // safe-haven flow (complements JPY signal)
    double chfjpy = 0.5;  // pure safe-haven intensity
};
static CrossPairL2 g_cross_l2;
static std::unordered_map<std::string,double>  g_asks;

// L2Level and L2Book are defined in OmegaFIX.hpp (moved 2026-03-24 for 264=5 upgrade)
std::mutex                                g_l2_mtx;
std::unordered_map<std::string, L2Book>   g_l2_books;

// ?? Per-symbol atomic L2 derived scalars ??????????????????????????????????
// Written by cTrader depth thread after each depth event -- zero lock on hot path.
// FIX tick loop reads these directly without holding g_l2_mtx.
// Full L2Book (walls, vacuums, book_slope) still uses g_l2_mtx -- cold path only.
//
// WHY NOT atomic<L2Book>: L2Book=168 bytes. atomic<T> over 16 bytes is NOT
// lock-free on x86-64 -- MSVC falls back to a hidden internal mutex, which is
// strictly worse. atomic<double>=8 bytes, aligned = genuine lock-free MOV.
struct AtomicL2 {
    std::atomic<double>   imbalance{0.5};       // raw_bid/(raw_bid+raw_ask) from CTDepthBook
    std::atomic<double>   microprice_bias{0.0}; // microprice - mid, signed
    std::atomic<bool>     has_data{false};      // true when book has non-zero sizes
    std::atomic<int64_t>  last_update_ms{0};    // epoch-ms of last cTrader depth event
    std::atomic<int>      raw_bid{0};           // raw bid quote count from CTDepthBook (all levels)
    std::atomic<int>      raw_ask{0};           // raw ask quote count from CTDepthBook (all levels)
    // Microstructure edge score: delta-based signal computed from consecutive L2Book
    // snapshots by GoldMicrostructureAnalyzer in CTraderDepthClient on every DOM event.
    // Range 0..1. >0.65 = bullish DOM pressure, <0.35 = bearish DOM pressure, ~0.5 = neutral.
    // Replaces raw_imbalance() as input to L2 persistence tracking so the path
    // fires on real order-flow signals instead of perpetual 0.5.
    std::atomic<double>   micro_edge{0.5};      // GoldMicrostructureAnalyzer output (DOM delta score)

    // fresh(): true only when a real book update arrived within max_age_ms.
    // Prevents engines acting on stale/default-initialised imbalance (0.5).
    // Lock-free, called on hot path.
    bool fresh(int64_t now_ms, int64_t max_age_ms = 5000) const noexcept {
        const int64_t t = last_update_ms.load(std::memory_order_relaxed);
        return (t > 0) && ((now_ms - t) <= max_age_ms);
    }
};
static AtomicL2 g_l2_gold;    // XAUUSD
static AtomicL2 g_l2_sp;      // US500.F
static AtomicL2 g_l2_nq;      // USTEC.F
static AtomicL2 g_l2_cl;      // USOIL.F
static AtomicL2 g_l2_eur;     // EURUSD
static AtomicL2 g_l2_gbp;     // GBPUSD
static AtomicL2 g_l2_aud;     // AUDUSD
static AtomicL2 g_l2_nzd;     // NZDUSD
static AtomicL2 g_l2_jpy;     // USDJPY
static AtomicL2 g_l2_ger40;   // GER40
static AtomicL2 g_l2_uk100;   // UK100
static AtomicL2 g_l2_estx50;  // ESTX50
static AtomicL2 g_l2_brent;   // BRENT
static AtomicL2 g_l2_nas;     // NAS100
static AtomicL2 g_l2_us30;    // DJ30.F

// Map symbol name to AtomicL2* -- used by cTrader write path and FIX tick read path
static AtomicL2* get_atomic_l2(const std::string& sym) noexcept {
    if (sym=="XAUUSD") return &g_l2_gold;
    if (sym=="US500.F")  return &g_l2_sp;
    if (sym=="USTEC.F")  return &g_l2_nq;
    if (sym=="USOIL.F")  return &g_l2_cl;
    if (sym=="EURUSD")   return &g_l2_eur;
    if (sym=="GBPUSD")   return &g_l2_gbp;
    if (sym=="AUDUSD")   return &g_l2_aud;
    if (sym=="NZDUSD")   return &g_l2_nzd;
    if (sym=="USDJPY")   return &g_l2_jpy;
    if (sym=="GER40")    return &g_l2_ger40;
    if (sym=="UK100")    return &g_l2_uk100;
    if (sym=="ESTX50")   return &g_l2_estx50;
    if (sym=="BRENT")    return &g_l2_brent;
    if (sym=="NAS100")   return &g_l2_nas;
    if (sym=="DJ30.F")   return &g_l2_us30;
    return nullptr;
}

// ?? cTrader depth tick staleness tracker ?????????????????????????????????????
// Stores the last time (ms) a cTrader depth event arrived per symbol.
// FIX W/X suppresses on_tick when cTrader depth is fresh (<500ms) --
// prevents the 1pt lag from FIX gateway batching in fast markets.
static std::atomic<int64_t> g_ct_ms_xauusd{0}, g_ct_ms_sp{0},  g_ct_ms_nq{0},
                             g_ct_ms_cl{0},    g_ct_ms_eur{0},
                             g_ct_ms_gbp{0},   g_ct_ms_aud{0},  g_ct_ms_nzd{0},
                             g_ct_ms_jpy{0},   g_ct_ms_ger40{0},g_ct_ms_uk100{0},
                             g_ct_ms_brent{0}, g_ct_ms_nas{0},  g_ct_ms_us30{0};

static std::atomic<int64_t>* get_ctrader_tick_ms_ptr(const std::string& sym) noexcept {
    if (sym=="XAUUSD")  return &g_ct_ms_xauusd;
    if (sym=="US500.F") return &g_ct_ms_sp;
    if (sym=="USTEC.F") return &g_ct_ms_nq;
    if (sym=="USOIL.F") return &g_ct_ms_cl;
    if (sym=="EURUSD")  return &g_ct_ms_eur;
    if (sym=="GBPUSD")  return &g_ct_ms_gbp;
    if (sym=="AUDUSD")  return &g_ct_ms_aud;
    if (sym=="NZDUSD")  return &g_ct_ms_nzd;
    if (sym=="USDJPY")  return &g_ct_ms_jpy;
    if (sym=="GER40")   return &g_ct_ms_ger40;
    if (sym=="UK100")   return &g_ct_ms_uk100;
    if (sym=="BRENT")   return &g_ct_ms_brent;
    if (sym=="NAS100")  return &g_ct_ms_nas;
    if (sym=="DJ30.F")  return &g_ct_ms_us30;
    return nullptr;
}

// ?? Persistent state directory -- separate from logs/ ??????????????????????
// All .dat files (bars, ATR, kelly, trend state) live here.
// Never touched by git reset or log operations.
inline std::string state_root_dir() {
#ifdef _WIN32
    const char* path = "C:\\Omega\\state";
    ::CreateDirectoryA(path, nullptr);
    ::CreateDirectoryA("C:\\Omega\\state\\kelly", nullptr);
    return path;
#else
    return "state";
#endif
}


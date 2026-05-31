// globals.hpp -- extracted from main.cpp
// Section: globals (original lines 435-964)
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

// ── Step 2 Omega Terminal: engine snapshot registry ─────────────────────────
// EngineRegistry types live in include/EngineRegistry.hpp so OmegaApiServer.cpp
// (a separate translation unit) can read them without dragging in the rest of
// globals.hpp's engine-instance graph. Here we DEFINE the single g_engines
// instance with external linkage so the extern declaration in
// EngineRegistry.hpp resolves to exactly this object at link time. Engines
// self-register in init_engines() (include/engine_init.hpp, end of function).
#include "EngineRegistry.hpp"
omega::EngineRegistry g_engines;

// ── Step 3 Omega Terminal: per-engine "last close" side-table ───────────────
// EngineLastRegistry holds the most recent (last_ts_ms, last_pnl) per tr.engine
// string. Written from handle_closed_trade in include/trade_lifecycle.hpp on
// every closed trade (shadow + live), read by the snapshot lambdas in
// include/engine_init.hpp so /api/v1/omega/engines returns real
// last_signal_ts / last_pnl values instead of 0. See EngineLastRegistry.hpp
// header comment for the full design rationale.
#include "EngineLastRegistry.hpp"
omega::EngineLastRegistry g_engine_last;

// ── Step 3 Omega Terminal: open-position read-API registry ──────────────────
// OpenPositionRegistry holds engine-side snapshotter callbacks that return the
// list of currently-open positions in a uniform PositionSnapshot shape. Read
// by OmegaApiServer.cpp's build_positions_json() to serialise
// /api/v1/omega/positions. Sources self-register in init_engines()
// (include/engine_init.hpp). Step 3 ships only the HybridGold source; other
// engines (Tsmom/Donchian/EmaPullback/TrendRider/HBI) land in a follow-up.
#include "OpenPositionRegistry.hpp"
omega::OpenPositionRegistry g_open_positions;

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

// ── IBKR DOM bridge (S88, 2026-05-22) ───────────────────────────────────────
// Optional secondary L2 source from tools/ibkr_dom_bridge.py via TCP.
// READ-ONLY. Default OFF. Engines opt-in by checking g_ibkr_l2.<sym>.fresh().
// Primary L2 path (Blackbull FIX -> g_macro_ctx.gold_l2_imbalance) untouched.
// Started in omega_main.hpp when env OMEGA_IBKR_BRIDGE=1.
#include "IbkrDomConsumer.hpp"
static omega::ibkr::L2Bus         g_ibkr_l2;
static omega::ibkr::ConsumerStats g_ibkr_l2_stats;
static std::atomic<bool>          g_ibkr_l2_stop{false};

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
// 2026-05-23: NY-open ORB-swing instances for big-swing capture on US futures.
//   Same OpeningRangeEngine class; per-instance params set in engine_init.hpp
//   give them wider TP/SL + multi-hour hold so the existing CrossPosition
//   BE-lock + trail + TP-extend continuation logic actually has room to ride.
static omega::cross::OpeningRangeEngine    g_orb_nas100; // NAS100 NY 13:30 UTC (swing geometry)
static omega::cross::OpeningRangeEngine    g_orb_dj30;   // DJ30   NY 13:30 UTC (swing geometry)
static omega::cross::OpeningRangeEngine    g_orb_ger30;  // Xetra 08:00 UTC
static omega::cross::OpeningRangeEngine    g_orb_uk100;  // LSE 08:00 UTC, 15-min window

// GER40 London Breakout Short (2026-05-17): Asian range break below at London open.
// Validated: PF=1.42, WR=50.5%, 20/25 param combos profitable, 2.3yr tick data.
#include "Ger40LondonBreakoutEngine.hpp"
static omega::Ger40LondonBreakoutEngine g_ger40_london_brk;
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

// MinimalH4GER40Breakout -- GER40 sister of MinimalH4Breakout. Self-contained:
//   Validated via 2yr Tickstory tick sweep on GER40: 27/27 configs profitable.
//   2026-05-20 multi-symbol C++ sweep tuned to don=6 sl=2.0 tp=3.0 to=48 long_only=true.
//   n=50 Sh=3.67 PnL=$8.40 WR=58% MaxDD=$2.94.
//   Shadow-mode until n>=10 live trades validate backtest expectation.
#include "MinimalH4GER40Breakout.hpp"
static omega::MinimalH4GER40Breakout g_minimal_h4_ger40;  // GER40 pure H4 Donchian breakout

// EurGbpPairsEngine -- spread mean-reversion on EURUSD/GBPUSD H1 z-score.
//   2026-05-20 C++ engine sweep + 6-mode rigor harness:
//     Sh=7.31 IS=7.32 OOS=7.23 (cost=1.5pip/leg). 6/6 WF folds positive.
//     Monte Carlo n=10000: p<0.0001. Monthly: 14/14 positive (100%).
//     Cost stress 0-5pip: Sh 8.62 -> 4.27 monotonic. Robustness +/-20% all Sh>6.3.
//   Most robust pair edge found in exhaustive cross-asset search (~30 combos tested).
//   Real EURGBP single-instrument FAILS (15bps microstructure drowns synthetic edge).
//   2-leg execution (EURUSD + GBPUSD) is the correct path.
//   Shadow-mode until n>=30 live trades validate.
#include "EurGbpPairsEngine.hpp"
static omega::EurGbpPairsEngine g_eur_gbp_pairs;  // EURUSD/GBPUSD spread mean-rev

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

// TsmomPortfolioV2 -- CellEngine refactor Phase 2a shadow alongside g_tsmom.
//
// Status: REFACTOR-VALIDATION ENGINE, not a production trader. Always
// shadow_mode=true regardless of g_cfg.mode. Trades flow into a SEPARATE
// CSV ledger (logs/shadow/tsmom_v2.csv via omega::cell::shadow::tsmom_writer)
// and DO NOT touch g_omegaLedger -- otherwise daily PnL / drawdown / engine-
// cull / param-gate state would double-count every trade.
//
// Phase 2a: max_positions_per_cell=1 -- the Phase 2a contract per
// docs/CELL_ENGINE_REFACTOR_PLAN.md §4 is that the V2 ledger MUST match V1's
// byte-for-byte at max=1 for >= 5 trading days before cutover. Backtest
// parity over the 1-year tsmom_warmup_H1 corpus already passed
// (see backtest/results/bt_tsmom_v{1,2}.csv -- byte-identical, sha256 match).
// Live shadow exists to surface any edge cases the backtest didn't cover
// (bar gaps, weekend spreads, FORCE_CLOSE timing under reconnect).
//
// Phase 2b: flip max=10 once Phase 2a parity holds for the agreed window.
// Added 2026-05-01 Session "Phase 2a live shadow".
#include "CellEngine.hpp"
#include "TsmomStrategy.hpp"
#include "CellShadowLedger.hpp"
static omega::cell::TsmomPortfolioV2 g_tsmom_v2;  // 5 long cells (H1/H2/H4/H6/D1)

// DonchianPortfolio -- Tier-2 ship of 7 donchian cells (H2 long; H4/H6/D1
// long+short). Bidirectional, would have profited during the 2026-03-18
// BEAR cluster that long-only C1Retuned lost on. Note: H1 long is NOT in
// this engine -- it's the retuned cell already live in C1RetunedPortfolio.
// Added 2026-04-30 Session "Tier-1+2 ship".
#include "DonchianEngine.hpp"
static omega::DonchianPortfolio g_donchian;  // 7 cells: H2L, H4L+S, H6L+S, D1L+S

// EmaPullbackPortfolio -- Tier-3 ship of 4 ema_pullback long cells
// (H1/H2/H4/H6 long). 9/21 EMA pullback-and-recover pattern.
// Long-only Tier-3 -- per master_summary post-cut, only longs are profitable.
// Added 2026-04-30 Session "Tier-1+2+3 ship".
#include "EmaPullbackEngine.hpp"
static omega::EpbPortfolio g_ema_pullback;  // 4 cells: H1/H2/H4/H6 long

// TrendRiderPortfolio -- Tier-4 ship of 6 trend-rider cells (H2/H4 long+short,
// H6/D1 long). 40-bar Donchian breakout entry + stage trail (no TP, no time
// exit). Validated 184 trades/yr / pf 2.0 / +$19,633/yr at 0.05 lot baseline.
// Uses CONVICTION-TIERED sizing: risk_pct=0.010 + max_lot_cap=0.10 (2x other
// engines) -- earned by backtest pf 1.81-6.46. Projects ~$39K/yr at 0.10 cap.
// Source: distilled from the 2026-03-27 +$3,157 TrendPullback win logic,
// adapted for bar-based execution. Self-contained header.
// Added 2026-04-30 Session "Tier-4 trend-rider ship".
#include "TrendRiderEngine.hpp"
static omega::TrendRiderPortfolio g_trend_rider;  // 6 cells: H2L+S, H4L+S, H6L, D1L

// =============================================================================
//  Audit-disable flags (loser audit 2026-04-30)
//  ---------------------------------------------------------------------------
//  Per the 4-week shadow ledger audit, the following engines bled money and
//  are disabled at the dispatch sites in tick_gold.hpp / tick_indices.hpp.
//  Existing open positions still manage out via the dispatch site checking
//  has_open_position() before skipping; new entries are blocked.
//
//  To re-enable an engine, flip the flag to false in engine_init.hpp and
//  rebuild. Disabled engines remain compiled and warm-state intact.
//
//  ALWAYS update the bleed comment when audit-disabling, so future devs
//  know what triggered the disable. Pre-existing audit-disables:
//
//      g_disable_macro_crash   -- handled via g_macro_crash.enabled=false
//                                 in engine_init.hpp (engine has its own
//                                 enabled flag at line 97 of MacroCrashEngine.hpp).
//                                 -10,849pts in 4wk, 4.8% WR (2026-04-30 audit).
//
//      g_disable_candle_flow   -- -3,967pts in 4wk, 43.4% WR but -8.91 avg/trade
//                                 (2026-04-30 audit).
//
//      g_disable_bracket_gold  -- -324pts in 4wk, 12.8% WR
//                                 (2026-04-30 audit; small absolute but
//                                 sustained losing pattern).
//                                 2026-05-28: re-enabled by operator, then
//                                 RE-DISABLED same day after full audit:
//                                   * 2yr_audit: 59,471 trades, PF=0.618,
//                                     WR=28.41%, gross=-$311.58. All sessions
//                                     negative incl. London (PF 0.58 worst).
//                                     gold_session_ok=true at tick_gold.hpp:157
//                                     -- engine has NO session gate, arms 24/7.
//                                   * sweep_v5 found mask=0xE no-Asia
//                                     PF=1.16 n=109 +$0.70 on last 12mo.
//                                   * walk-forward sweep_v6 first 12mo
//                                     same params PF=0.71 n=18 -$0.27.
//                                     Edge regime-specific not real.
//                                 Conclusion: engine cannot cover costs
//                                 across regimes. Permanent disable until
//                                 entry logic redesigned (not just retune).
//
//      g_disable_index_flow    -- -112pts across 4 instances
//                                 (2026-04-30 audit; minor bleed but no
//                                 evidence of edge over 4wk).
//
//  GoldEngineStack sub-engine audit-disables (added 2026-04-30, post-1db4408):
//  -------------------------------------------------------------------------
//  Sub-engines below are pushed into GoldEngineStack::engines_ at construction
//  and toggled enabled_ each tick by RegimeGovernor::apply() based on regime.
//  A simple `enabled_=false` at startup is overwritten on the next regime
//  change, so we gate at the dispatch loop instead via
//  GoldEngineStack::set_subengine_audit_disabled() called from engine_init.hpp.
//  Total ~$3K bleed across these 5 over the 4-week ledger window.
//
//      g_disable_session_momentum         -- IMPULSE-regime session-open
//                                            volatility-expansion engine.
//                                            53.3% WR but bleed dominated
//                                            by negative tail trades.
//      g_disable_intraday_seasonality     -- COMPRESSION/MEAN_REV half-hourly
//                                            t-stat bias. Sharpe=1.08 in sim
//                                            but live edge collapsed.
//      g_disable_vwap_snapback            -- MEAN_REVERSION VWAP fade. Sample
//                                            never grew beyond a few trades
//                                            despite repeat re-enables.
//      g_disable_vwap_stretch_reversion   -- 2-sigma VWAP fade + deceleration.
//                                            Even with the 2026-04-09 ewm_drift
//                                            injection fix, live PnL net negative.
//      g_disable_dxy_divergence           -- Intermarket DXY-vs-XAU divergence.
//                                            Re-enabled 2026-04-?? after DX.F
//                                            feed fix; live trades insufficient
//                                            to justify continued exposure.
// =============================================================================
static bool g_disable_candle_flow              = true;
static bool g_disable_bracket_gold             = true;   // 2026-05-29 PERMANENT-DISABLE: 2yr re-audit under corrected gates (MAX_RANGE=19, MAX_SL_DIST_PTS=19) PF=0.705 gross=-$93.26 n=14568 every session loser (Asia PF 0.683, London 0.689, NY 0.722, Late_NY 0.722). 2yr sweep over 16 v6-validated configs: best PF=1.16 n=137 gross=+$0.84 (slip noise, not edge). Engine has no durable cross-regime edge. See /Users/jo/bt_reports/bracket_prodgates.txt + bracket_sweep_2yr.txt.
static bool g_disable_index_flow               = true;
// S46 2026-05-27: M5 scalp engines disabled pending real-class validation.
// All "validated" PnL numbers for these engines came from inline-reimpl
// harnesses with bar-extreme lookahead bias. gsp_s63_audit_bt confirmed
// the divergence: standalone +$33.5k vs real class -$12.4k on 26mo XAUUSD.
// The same architectural pattern (M5 bars + S63 cuts + cost-aware BE)
// applies to all entries below; until each is class-audited, disable.
static bool g_disable_xauusd_fvg               = true;  // S46: M5 FVG, S63 cuts, never class-audited
static bool g_disable_session_momentum         = true;  // GoldStack sub-engine
static bool g_disable_intraday_seasonality     = true;  // GoldStack sub-engine
static bool g_disable_vwap_snapback            = true;  // GoldStack sub-engine
static bool g_disable_vwap_stretch_reversion   = true;  // GoldStack sub-engine
static bool g_disable_dxy_divergence           = true;  // GoldStack sub-engine
static bool g_disable_asian_range              = true;  // GoldStack sub-engine — S99d 2026-05-18: bleed on live (-$5.28 first London fire, backtest only $279/2yr at 49.7% WR, marginal edge, retune queued

// Disabled 2026-04-16 after 6-day sweep / 1.5M ticks showed no edge across 7776 configs.

// =============================================================================
// IndexFlowEngine -- L2 flow + EWM drift engines for US equity indices.
// Architecture: L2 persistence + EWM drift + ATR-prop SL
// + staircase trail. Per-symbol calibrated (see IndexFlowEngine.hpp).
//
// L2 data: fed from existing AtomicL2 instances (g_l2_sp, g_l2_nq, g_l2_nas,
// g_l2_us30) already updated by cTrader depth thread in omega_main.hpp.
// Pass l2_imb via: g_l2_sp.imbalance.load(std::memory_order_relaxed)
// =============================================================================
static omega::idx::IndexFlowEngine       g_iflow_sp("US500.F");
static omega::idx::IndexFlowEngine       g_iflow_nq("USTEC.F");
static omega::idx::IndexFlowEngine       g_iflow_nas("NAS100");
static omega::idx::IndexFlowEngine       g_iflow_us30("DJ30.F");

// IndexMacroCrashEngine -- four-symbol parity, shadow-mode hardcoded.
// shadow_mode=true is pinned on the engine class (IndexFlowEngine.hpp:816,
// "NEVER set shadow_mode=false without explicit authorization"). Wired in
// each on_tick_<sym> handler in tick_indices.hpp.
static omega::idx::IndexMacroCrashEngine g_imacro_sp("US500.F");
static omega::idx::IndexMacroCrashEngine g_imacro_nq("USTEC.F");
static omega::idx::IndexMacroCrashEngine g_imacro_nas("NAS100");
static omega::idx::IndexMacroCrashEngine g_imacro_us30("DJ30.F");

// =============================================================================
// IndexIntradayDriftEngine (S37-Z 2026-05-28) -- BUY open / SELL close.
// Audited viable on SPX, USA30 (DJ30), UK100 over 2024-2026 corpus net of
// 1.5-3pt round-trip retail spread cost. Walk-forward both halves positive
// on all 3. See include/IndexIntradayDriftEngine.hpp header block for the
// audit table. NSXUSD and GER40 audited as marginal -- skip until WF fixes.
// =============================================================================
#include "IndexIntradayDriftEngine.hpp"
static omega::IndexIntradayDriftEngine   g_idd_sp;       // US500.F
static omega::IndexIntradayDriftEngine   g_idd_us30;     // DJ30.F  (USA30 corpus)
static omega::IndexIntradayDriftEngine   g_idd_uk100;    // UK100   (GBRIDXGBP corpus)

// Bug #3 (KNOWN_BUGS.md) cross-engine state. Two-part block: index_any_open()
// (defined later, after engine declarations) catches concurrent overlap;
// idx_recent_close_block() catches the documented 1-3min post-close whipsaw.
// record_index_close() called from ca_on_close in trade_lifecycle.hpp.
namespace omega { namespace idx {

inline std::atomic<int64_t> g_idx_last_close_ts{0};   // unix seconds
inline int g_index_min_entry_gap_sec = 120;           // KNOWN_BUGS.md default

// Called from the close-hook wrapper in tick_indices.hpp. Symbol must be one
// of the four US index symbols; other symbols are silently ignored so the
// helper is safe to call unconditionally from any close path.
inline void record_index_close(const std::string& symbol) noexcept {
    if (symbol == "US500.F" || symbol == "USTEC.F" ||
        symbol == "DJ30.F"  || symbol == "NAS100") {
        const int64_t now_s = static_cast<int64_t>(std::time(nullptr));
        g_idx_last_close_ts.store(now_s, std::memory_order_relaxed);
    }
}

inline bool idx_recent_close_block() noexcept {
    const int64_t last = g_idx_last_close_ts.load(std::memory_order_relaxed);
    if (last == 0) return false;
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    return (now - last) < static_cast<int64_t>(g_index_min_entry_gap_sec);
}

}} // namespace omega::idx

// VWAPAtrTrail -- REMOVED 2026-05-07 (engine audit phase 4).
//   All four instances (g_vwap_atr_trail_sp/nq/nas/us30) were declared but
//   never referenced anywhere in the dispatch hot path or supporting code.
//   The two _nas / _us30 instances were already commented as "no VWAPRev,
//   unused"; the _sp / _nq instances depended on VWAPReversionEngine which
//   was disabled (engine_init.hpp:447-457 enabled=false). Confirmed zero
//   non-declaration references via grep across include/ and backtest/.
//   See docs/INVENTORY_VERIFICATION_2026-05-07.md §10a for the audit trail.

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

// S12 P3c (2026-05-07): GoldHybridBracketEngine + IndexHybridBracketEngine
//   header files DELETED. Engines were dispatch-removed in S10 P3a (commit
//   ba5f0e9), globals/init/heartbeat/gate-reads removed in S11 P3b (commit
//   a6e3403), and the header files plus all #include refs removed in this
//   commit. Engine families fully retired. See NEXT_SESSION_S12.md for the
//   commit-trail summary.
// 2026-05-01 SESSION_h: GoldMidScalperEngine -- mid-band sister to HybridGold
//   targeting the $20-40 P&L zone (range $8-20 with TP_RR=4).  Shadow-only by
//   default; promote to live only after a 2-week paper validation.  Wired in
//   tick_gold.hpp dispatch block parallel to g_hybrid_gold.
#include "GoldMidScalperEngine.hpp"
// 2026-05-08 S19: GoldMicroScalperEngine -- bidirectional micro-tick scalper
//   for XAUUSD. Captures many small moves up and down via 20-tick z-score
//   reversion entry, locks profit fast (BE-arm at 0.5pt MFE), trails
//   aggressively (0.5pt below MFE post-BE), and exits immediately on
//   reversal (5-tick net delta or L2 slope flip). Calibrated 2026-05-08 on
//   28 days of XAUUSD L2 capture (6.7M ticks, 490-combo CRTP sweep) with
//   operating point Z=0.75 / TP=1.0 / SL=3.0 / BE=0.5 / TR=0.5 producing
//   34K+ trades at 86% WR / PF 3.05 in shadow simulation. Shadow-only by
//   default; promote to live only after a 2-week paper validation matches
//   backtest expectancy. Wired in tick_gold.hpp dispatch block parallel to
//   g_gold_midscalper.
#include "GoldMicroScalperEngine.hpp"

// 2026-05-11 S33d: XauTrendFollow4hEngine -- the 3-cell ensemble that
//   survived the realistic-fill 26-month cross-validation. Donchian N=20,
//   InsideBar, and ER0.20 mom=20 each as independent cells, max 3
//   concurrent positions. Shadow-only by default. Drives off the s_cur_h4
//   bar already aggregated in tick_gold.hpp.
//   Built from edge_hunt.cpp + top_cells_monthly.cpp results 2026-05-11.
#include "XauTrendFollow4hEngine.hpp"
static omega::XauTrendFollow4hEngine g_xau_tf_4h;

// S118 2026-05-19: XauTrendFollow1hEngine -- H1 long-only ensemble (2 cells:
//   EmaCross_20_80 + Donchian_N40).  Companion to XauTrendFollow4hEngine
//   EmaCross_8_21 (S116/S117) -- together they form the verified 3-cell
//   ensemble from S114 research (~$28k OOS / 25mo / Sharpe +3.21).  Driven
//   by the s_cur_h1 bar already aggregated in tick_gold.hpp.
#include "XauTrendFollow1hEngine.hpp"
static omega::XauTrendFollow1hEngine g_xau_tf_1h;

// S42 (2026-05-31): SessionMomentumEngine -- clock-based session-window long,
//   the first NON-trend-breakout edge (new signal axis: time-of-day). Two XAU
//   instances: NY-afternoon (16:00 UTC, 4h) and Asian overnight (23:00, 5h),
//   both gated by close>EMA200(h1). Self-aggregates H1 from the gold tick
//   stream via feed_tick() (wired in tick_gold.hpp). Validated lookahead-free
//   (xau_session_scan.cpp): NYpm PF1.57 6/6-to-5bp, o/n PF1.56. Live spread at
//   16h confirmed mean ~1.1bp p90 3.1bp << the 5bp death line. HARD shadow.
#include "SessionMomentumEngine.hpp"
static omega::SessionMomentumEngine g_xau_sess_nypm;
static omega::SessionMomentumEngine g_xau_sess_overnight;

// 2026-05-11 S33d: UstecTrendFollow5mEngine -- Donchian N=20 at 5m bars
//   on USTEC. Convergent edge across 4 unrelated signal families on the
//   15-day L2 sample. Shadow-only; KEEP shadow until 6+ months of USTEC
//   L2 capture confirm the 2-month finding.
#include "UstecTrendFollow5mEngine.hpp"
static omega::UstecTrendFollow5mEngine g_ustec_tf_5m;

// 2026-05-11 S33e: XauTrendFollowD1Engine -- daily-timeframe trend-follow
//   ensemble for XAU. 3 cells (Momentum lb=20, Keltner K=2.0, ADX_Mom adx>25),
//   all on sl2.0_tp4.0 ATR brackets. Synthesises D1 bars internally from the
//   H4 stream so no new bar aggregation needed in tick_gold.hpp. 0.01 lot/cell,
//   max 3 concurrent. Shadow-default. Lower cadence than 4h ensemble (~2
//   trades/month combined) but biggest per-trade edges in the project ($36-60).
#include "XauTrendFollowD1Engine.hpp"
static omega::XauTrendFollowD1Engine g_xau_tf_d1;

// 2026-05-20: XauTsmomFastD1Engine -- short-lookback D1 momentum (lb=5, sl=1.0,
//   tp=5.0, hold=20). Distinct from XauTrendFollowD1Engine's lb=20 cell.
//   Backtest 2yr daily XAU: IS Sh 6.69 / OOS Sh 7.65 / FUL Sh 7.57 at 1bps cost,
//   holds to FUL Sh 6.69 at 20bps. n=48 over 670 days. Shadow-default.
#include "XauTsmomFastD1Engine.hpp"
static omega::XauTsmomFastD1Engine g_xau_tsmom_fast_d1;

// 2026-05-20: XauTurtleD1Engine -- 40-day Donchian break (long-only).
//   Resurrection of TurtleTick signal archetype (retired S50 X1 Apr 27 2026).
//   Re-tested on 2yr daily: lb=40 hold=10 sl_atr=1.5 tp_atr=3.0.
//   Cost stress: 1bps IS Sh=8.08/OOS=18.96/FUL=13.57, 10bps FUL=13.01,
//                50bps FUL=10.51. WR=70%, n=20 over 670 days (sparse).
//   CAVEAT: low n, high variance in Sharpe estimate. Shadow only until
//           n>=5 live shadow trades validate.
#include "XauTurtleD1Engine.hpp"
static omega::XauTurtleD1Engine g_xau_turtle_d1;

// 2026-05-20: XauStopRunD1Engine -- stop-hunt rejection rally (long-only).
//   Resurrection of StopRunReversal archetype (retired S50 X2 Apr 27 2026).
//   Re-tested on 2yr daily: lb=5 hold=20 sl_atr=1.5 tp_atr=2.0.
//   Signal: bar.low broke 5d_low AND bar.close > 5d_low.
//   Cost stress: 1bps IS Sh=7.76/OOS=6.55/FUL=6.84, 10bps FUL=6.34,
//                50bps FUL=4.12. WR=65.5%, n=29 over 670 days.
//   Shadow only until n>=5 live shadow trades.
#include "XauStopRunD1Engine.hpp"
static omega::XauStopRunD1Engine g_xau_stop_run_d1;

// 2026-05-20: XauPullbackContH4Engine -- EMA10>EMA50 trend, pullback to EMA10.
//   Resurrection of PullbackCont archetype (retired S49 X5 Apr 26 2026).
//   Backtest 2yr H4 XAU: IS Sh=3.97, OOS Sh=4.06, FUL Sh=3.96, n=97, PnL=53.5%.
//   Densest of the resurrection batch (~50 trades/year on H4).
#include "XauPullbackContH4Engine.hpp"
static omega::XauPullbackContH4Engine g_xau_pullback_cont_h4;

// 2026-05-20: XauNbmD1Engine -- Noise Band Momentum on daily.
//   Signal pattern from main's disabled g_nbm_* (all set enabled=false).
//   Tested on XAU D1: IS Sh=9.60, OOS Sh=7.30, FUL Sh=8.01, n=25, PnL=35.6%.
//   Break ABOVE EMA20 + 2.0*ATR + 0.3*ATR momentum.
#include "XauNbmD1Engine.hpp"
static omega::XauNbmD1Engine g_xau_nbm_d1;

// 2026-05-20: XauEmaCrossH4Engine -- 20/100 golden cross H4 long.
//   Main has EMACrossEngine; this is a XAU-specific H4 cell.
//   Backtest 2yr XAU H4: IS Sh=4.45, OOS Sh=9.19, FUL Sh=7.15, n=20, PnL=12.2%.
//   OOS > IS (low overfit risk).
#include "XauEmaCrossH4Engine.hpp"
static omega::XauEmaCrossH4Engine g_xau_ema_cross_h4;

// 2026-05-20 mega-sweep batch (top 4 by cost-stressed Sharpe):
//
// XauPullbackContD1Engine -- D1 variant of PullbackContH4 with longer EMA.
//   Cost stress: 1bp Sh=8.73, 10bp Sh=8.38, 30bp Sh=7.61, 50bp Sh=6.83.
//   IS Sh=15.73 OOS Sh=6.90 n=33 PnL=71.6% WR=66.7%.
#include "XauPullbackContD1Engine.hpp"
static omega::XauPullbackContD1Engine g_xau_pullback_cont_d1;

// XauBBScalpD1Engine -- Bollinger Band fade D1 (close < BB lower).
//   Resurrection of g_bband_scalp archetype (main has enabled=false).
//   Cost stress: 1bp Sh=6.47, 10bp Sh=5.90, 30bp Sh=4.63, 50bp Sh=3.37.
//   IS Sh=10.07 OOS Sh=3.88 n=20 WR=68.4%.
#include "XauBBScalpD1Engine.hpp"
static omega::XauBBScalpD1Engine g_xau_bb_scalp_d1;

// XauSwingBreakD1Engine -- HH+HL pattern + 10-day high break.
//   Cost stress: 1bp Sh=5.49, 10bp Sh=5.08, 30bp Sh=4.17, 50bp Sh=3.25.
//   IS Sh=10.43 OOS Sh=9.45 n=18 PnL=42.5% WR=61.1%.
#include "XauSwingBreakD1Engine.hpp"
static omega::XauSwingBreakD1Engine g_xau_swing_break_d1;

// Ger40TurtleH4Engine -- 20-bar Donchian breakout on GER40 H4.
//   Turtle archetype applied to DAX H4. IS Sh=5.06 OOS Sh=4.60 FUL Sh=5.00
//   n=33 PnL=9.4% WR=63.6% mdd=2.0%.
#include "Ger40TurtleH4Engine.hpp"
static omega::Ger40TurtleH4Engine g_ger40_turtle_h4;

// S41 (2026-05-30): Ger40KeltnerH1Engine -- first robust NON-gold trend edge.
// GER40 H1 Keltner EMA20 k2.0 sl3.0, bull_LB=200 (slower than gold). Validated
// full param-plateau + cost-stress 1x/2x/3x (edge_validate_s41.cpp); engine-
// driven BT PF 2.34. Self-aggregates H1 from the GER40 tick stream (no
// g_bars_ger40 exists) via feed_tick(), like Ger40TurtleH4Engine.
#include "Ger40KeltnerH1Engine.hpp"
static omega::Ger40KeltnerH1Engine g_ger40_kelt;

// FxTurtleH4Engine -- 20-bar Donchian breakout on FX majors, long-only.
//   Built 2026-05-23 as the post-mortem-driven replacement for the S99-
//   killed FX session-open compression cohort. Same long-only Donchian
//   H4 pattern that earned Ger40TurtleH4 PF 4.60 OOS and EURUSD long-only
//   walk-forward PF 1.14-1.30 across all 3 OOS folds. One generic class
//   instantiated per pair with pair-specific pip math.
#include "FxTurtleH4Engine.hpp"
static omega::FxTurtleH4Engine g_eurusd_turtle_h4;
static omega::FxTurtleH4Engine g_gbpusd_turtle_h4;
static omega::FxTurtleH4Engine g_audusd_turtle_h4;
static omega::FxTurtleH4Engine g_nzdusd_turtle_h4;
static omega::FxTurtleH4Engine g_usdjpy_turtle_h4;

// 2026-05-20 mega_sweep2 candle-pattern batch (XAU D1, cost-stressed):
//
// XauDojiRejD1Engine -- prev=doji + current breaks prev high.
//   Cost 1bp Sh=9.87, 10bp Sh=9.43, 50bp Sh=7.48 (most robust mega2 winner).
//   IS Sh=6.97 / OOS Sh=11.20 n=23 PnL=44.6% WR=65.2%.
#include "XauDojiRejD1Engine.hpp"
static omega::XauDojiRejD1Engine g_xau_doji_rej_d1;

// XauOutsideBarD1Engine -- outside-bar engulf + bullish close.
//   Cost 1bp Sh=6.47, 10bp Sh=5.88, 50bp Sh=3.27.
//   IS Sh=3.72 / OOS Sh=8.39 n=34 PnL=43.0% WR=64.7%.
#include "XauOutsideBarD1Engine.hpp"
static omega::XauOutsideBarD1Engine g_xau_outside_bar_d1;

// XauInsideBarD1Engine -- inside bar consolidation + breakout.
//   Cost 1bp Sh=5.05, 10bp Sh=4.39, 50bp Sh=1.42 (less cost-robust).
//   IS Sh=4.29 / OOS Sh=3.96 n=49 (highest density), WR=71.4%.
#include "XauInsideBarD1Engine.hpp"
static omega::XauInsideBarD1Engine g_xau_inside_bar_d1;

// 2026-05-21: GoldD1TrendState -- D1 EMA200 regime gate for bidirectional engines.
//   After 2026-05-20 InsideBar SHORT lost -$52 in gold uptrend, added regime
//   filter. Queried by XauTrendFollow2h InsideBar + DonchianBreakout (short
//   path) + any other bidirectional XAU engine before firing dir-dependent
//   entries. Updated from H4 close events in tick_gold.hpp.
//   Access via singleton: omega::gold_d1_trend() -- avoids include-order
//   issues (GoldEngineStack.hpp is included before globals.hpp).
#include "GoldD1TrendState.hpp"

// 2026-05-11 S33k: XauTrendFollow2hEngine -- denser-cadence sibling of the
//   4h engine. 4 cells (Keltner K=2, Donchian N=20, Donchian N=50, InsideBar),
//   all on sl2.0_tp4.0 ATR brackets. All 3/3 Duka years +ve per cell.
//   Synthesises 2h bars internally from the H1 stream. 0.01 lot, max 4
//   concurrent. Shadow-default. Backtested +$3,380 over 30 months across 764
//   trades (~25 trades/month).  Correlated with 4h/D1 engines (same XAU
//   trend regime, different timescales) -- think of as one XAU multi-TF
//   position, not as independent diversification.
#include "XauTrendFollow2hEngine.hpp"
static omega::XauTrendFollow2hEngine g_xau_tf_2h;

// 2026-05-15 S91: GoldUltimateEngine -- standalone v12 OOS-validated XAUUSD
//   trend engine. 7-factor entry filter + edge-hour gate (01/05/23 UTC) +
//   ATR floor 2.5. SL=2ATR, TP=5ATR, trail at 3ATR MFE / 2ATR distance.
//   26-month backtest: PF=1.36, WR=41.8%, 311 trades, OOS PF=1.39 (265 trades).
//   Shadow mode for initial live validation.
#include "GoldUltimateEngine.hpp"
static omega::GoldUltimateEngine g_gold_ultimate_engine;

// 2026-05-11/12 S34 + S35-P3 + S35-P4: XauThreeBar30mEngine -- XAU M30
//   three-bar continuation. Engine added in S34 (b1932d2), retrofitted
//   with the standard ProtectedEngineGuards bundle in S35-P3 (1684cfc),
//   per-year cross-validated against M30 bars aggregated from the
//   2024-03..2026-04 M15 dataset in S35-P4 (3ee31de). The TUNED config
//   (BE arm at +1*ATR / trail at 0.75*ATR / ATR floor $0.30) produces
//   +$1488 over 25mo / 1058 trades / 66.2% WR / PF 1.27 with max DD
//   $282.83, and is positive in every year (2024 +$199, 2025 +$242,
//   2026 partial +$1047). HARD shadow + enabled=true at startup --
//   operator flips shadow_mode to kShadowDefault after 1+ month of
//   shadow-live data confirms the backtest on the broker's tape.
//   REQUIRES: tick_gold.hpp dispatch hook (M30 bar aggregation +
//   on_30m_bar/on_tick calls). Not yet wired as of S35-P4; engine
//   instantiated but dormant until wiring lands.
#include "XauThreeBar30mEngine.hpp"
static omega::XauThreeBar30mEngine g_xau_threebar_30m;

// 2026-05-24 S136: XauDonchian55GatedM30Engine -- XAU M30 Donchian-55 symmetric
//   with EMA50/200 regime gate + MFE-lock trail (arm=0.7R, lock=80%).
//   /Users/jo/edge_research validation: 27mo backtest IS PF 1.10 +$460 /
//   OOS PF 1.67 +$1556 / L2 forward PF 3.03 +$773 DD -$200 / 48 trades.
//   Walk-forward 4 anchored folds: every fold positive.
//   First XAU engine validated on L2 forward window (2026-04-09 → 2026-05-19).
#include "XauDonchian55GatedM30Engine.hpp"
static omega::XauDonchian55GatedM30Engine g_xau_d55_gated_m30;

// 2026-05-24 S136: Xau3BarMomGatedH4Engine -- XAU H4 three-bar momentum symmetric
//   with MFE-lock trail (arm=1.0R, lock=90%).
//   /Users/jo/edge_research validation: 27mo backtest IS PF 1.11 +$983 /
//   OOS PF 1.07 +$272 / L2 forward PF 1.53 +$365 DD -$257.
//   Walk-forward 4 folds aggregate OOS +$2931 (no trail).
#include "Xau3BarMomGatedH4Engine.hpp"
static omega::Xau3BarMomGatedH4Engine g_xau_3bar_mom_h4;

// 2026-05-24 S136: NasBbRevLongH1Engine -- NAS100 H1 Bollinger-band mean-revert
//   LONG. BE-then-trail at 1.5×ATR (arm=1R, switch=2R).
//   /Users/jo/edge_research validation: Walk-forward 4 anchored folds all positive,
//   OOS aggregate +$7912 / 145 trades, best fold PF 1.83.
//   No NAS100 L2 data on disk; L2 validation pending.
#include "NasBbRevLongH1Engine.hpp"
static omega::NasBbRevLongH1Engine g_nas_bbrev_long_h1;

// 2026-05-24 S136: Us303BarMomH1Engine -- US30 H1 three-bar momentum SYMMETRIC
//   (long+short). MFE-lock trail arm=1.0R lock=90%.
//   /Users/jo/edge_research validation on dow30_2yr.csv (2023-10 → 2025-10):
//   IS 161n PF 1.44 +$11,380 / OOS 32n PF 2.37 +$4489 DD -$682 Sharpe 1.91.
//   Walk-forward 4 folds ALL positive, aggregate OOS +$10,943 / 160 trades.
//   Symmetric variant added to defend against regime change (cf. XAU L2 failure
//   of LONG-only).
#include "Us303BarMomH1Engine.hpp"
static omega::Us303BarMomH1Engine g_us30_3bar_mom_h1;

// 2026-05-26 S37: Us30EnsembleEngine -- DJ30.F 4-cell ensemble.
//   Cells: atr_exp H1 sl=2tp=3, inside_brk H1 sl=3tp=5, atr_exp M30 sl=3tp=2,
//          ema_pullback_10_30 H4 sl=1.5tp=5. All LONG-only.
//   /Users/jo/edge_research validation 2yr Dukascopy USA30 (2023-10 -> 2025-10):
//     - 3-period intersection: every cell positive in all 3 periods (n>=20 PF>=1.1)
//     - walk-forward 4 anchored folds: 16/16 fold-cell combinations positive
//     - engine-sim integrated backtest (bare SL/TP, $4 RT cost): +$1411 / 1711n
//   BE+trail DEFAULTS OFF in header (sim showed they clip atr_exp winners).
//   Cells independent (up to 4 concurrent DJ30.F positions at engine lot).
//   3bar_mom_H1 NOT in this ensemble -- lives in g_us30_3bar_mom_h1 separately.
#include "Us30EnsembleEngine.hpp"
static omega::Us30EnsembleEngine g_us30_ensemble;

// 2026-05-12 S35-P6: UstecTrendFollowHtfEngine -- multi-timeframe trend-follow
//   ensemble for USTEC.F. 5 cells across M15/H1/H2/H4 (InsideBar2h, Stoch1h
//   20/80, ATR_Mom 1h mom=50, Donchian N=20 15m, Stoch4h 20/80). Each cell
//   was positive in EVERY ONE of 3 periods (2025H1, 2025H2, 2026 partial)
//   on 16 months of NSXUSD HISTDATA tick data (89.6M ticks). Engine wraps
//   the bare cells with engine_protections.hpp (BE arm, trail-after-BE, ATR
//   floor); the wrapped backtest produced +$11,732 net across all 3 periods,
//   53% WR, PF 1.05, 4,227 trades.
//   REQUIRES: tick_indices.hpp dispatch hook in the USTEC.F bar-builder
//   (M15 bar aggregation + on_15m_bar/on_tick calls). Not yet wired in
//   tick_indices.hpp as of S35-P6; engine instantiated but dormant until
//   wiring lands.
//   NOTE: UstecTrendFollow5mEngine cells (Donchian N=20 + Keltner K=2.0)
//   were NOT positive in the 3-period test on the same 16mo dataset
//   (Donchian -$2761/-$2420/-$621 across H1/H2/26). The existing M5 engine
//   has been left untouched per operator instruction; this HTF engine is a
//   companion, not a replacement.
#include "UstecTrendFollowHtfEngine.hpp"
static omega::UstecTrendFollowHtfEngine g_ustec_tf_htf;

// 2026-05-08 S20+: RiskMonitor -- per-engine logging-only risk surveillance.
//   Watches WR break-even, fire rate over/under, and spread-at-entry drift
//   for every engine in data/risk_monitor_thresholds.csv (calibrated by
//   backtest/calibrate_risk_thresholds). v1 emits WOULD-TRIP log lines only;
//   does NOT touch any engine's shadow_mode flag. Wired in:
//     - load at startup        : engine_init.hpp (load_thresholds call)
//     - close hook             : trade_lifecycle.hpp::handle_closed_trade
//     - fire hook (microscalper): GoldMicroScalperEngine::on_fire_hook,
//                                 bound from engine_init.hpp
#include "RiskMonitor.hpp"
// 2026-05-02: EurusdLondonOpenEngine -- first FX engine since the 2026-04-06
//   global FX disable. London-open compression bracket on EURUSD, 06:00-09:00
//   UTC session window, news-blackout-gated for NFP/CPI/FOMC/ECB. Shadow-only
//   by default; promote to live only after a 2-week paper validation.  Wired
//   in tick_fx.hpp::on_tick_eurusd() dispatch block. See
//   docs/SESSION_2026-05-02_EURUSD_LONDON_OPEN_HANDOFF.md for full design.
#include "EurusdLondonOpenEngine.hpp"
// 2026-05-02: UsdjpyAsianOpenEngine -- Asian-session sister engine to
//   EurusdLondonOpenEngine. 00:00-04:00 UTC compression-breakout on USDJPY
//   (Tokyo open + first 4 hours, pre-Frankfurt-handoff). Same architectural
//   pattern as the EURUSD engine with JPY pip math (1 pip = 0.01 price,
//   USD_PER_PRICE_UNIT=100 at 0.10 lot). News-blackout-gated for NFP/CPI/
//   FOMC/BoJ. Shadow-only by default; promote to live only after a 2-week
//   paper validation showing >=30 trades with WR >= 60% net positive after
//   costs. Wired in tick_fx.hpp::on_tick_usdjpy() dispatch block. See
//   docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_HANDOFF.md for full design.
#include "UsdjpyAsianOpenEngine.hpp"
// 2026-05-04 (audit-fixes-36 + S57): GbpusdLondonOpenEngine -- cable
//   sister to EurusdLondonOpenEngine. London-open compression-breakout on
//   GBPUSD with the live target window 07:00-10:00 UTC (one hour later
//   than EUR's 06-09; cable compressions cluster around the LSE 08:00 UTC
//   equity open). MIN_RANGE/MAX_RANGE widened ~50% vs EUR (12-75 pips
//   vs 8-50) to track GBPUSD's ~50% wider daily ATR (80-150 pips vs EUR
//   60-120). News-blackout-gated for BoE/UK CPI/UK GDP via the GBP
//   currency set, plus NFP/CPI/FOMC via the USD set (auto-included by
//   the OmegaNewsBlackout symbol-to-country mapping). Shadow-only by
//   default; session window RESTORED to 07-10 UTC (2026-05-04, post-S57):
//   the audit-fixes-36 0-24 visibility-only widening was reverted to the
//   live target after live tape on the gold cohort showed the widening
//   produced session-mismatch ✓BE → SL artefacts.
//   Wired in tick_fx.hpp::on_tick_gbpusd() dispatch block.
#include "GbpusdLondonOpenEngine.hpp"
// 2026-05-04 (audit-fixes-36 + S57): AudusdSydneyOpenEngine -- aussie
//   sister to UsdjpyAsianOpenEngine. Sydney-open + Tokyo-handoff
//   compression-breakout on AUDUSD with the live target window 22:00-02:00
//   UTC (Sydney 22:00 + Tokyo overlap, pre-Frankfurt cutoff). AUD pip
//   math (1 pip = 0.0001 price, USD_PER_PRICE_UNIT=10000 at 0.10 lot --
//   identical to EUR/GBP because AUD is also a USD-quote major). All
//   USDJPY S55-S59 tuned constants rescaled from JPY 0.01-pip units to
//   AUD 0.0001-pip units. News-blackout-gated for RBA/AU CPI/AU jobs via
//   the AUD currency set, plus NFP/CPI/FOMC via the USD set. Shadow-only
//   by default; session window RESTORED to 22-02 UTC (2026-05-04, post-S57)
//   with wraparound-aware in-window check now active in the engine -- the
//   audit-fixes-36 0-24 visibility-only widening was reverted. Wired in
//   tick_fx.hpp::on_tick_audusd() dispatch block.
#include "AudusdSydneyOpenEngine.hpp"
// 2026-05-04 (audit-fixes-36 + S57): NzdusdAsianOpenEngine -- kiwi sister
//   to AudusdSydneyOpenEngine. Wellington-open + Tokyo-handoff
//   compression-breakout on NZDUSD with the live target window 22:00-04:00
//   UTC (one hour wider than AUD's 22-02 to capture post-Tokyo-open
//   AUDNZD-cross flow settlement). NZD pip math identical to AUD/EUR/GBP
//   (1 pip = 0.0001 price, USD_PER_PRICE_UNIT=10000 at 0.10 lot -- NZD is
//   also a USD-quote major). All AUDUSD S55-S59 tuned constants reused
//   as PRE-SWEEP defaults. News-blackout-gated for RBNZ/NZ CPI/NZ jobs
//   via the NZD currency set, plus NFP/CPI/FOMC via the USD set.
//   Shadow-only by default; session window RESTORED to 22-04 UTC
//   (2026-05-04, post-S57) with wraparound-aware in-window check now
//   active in the engine -- the audit-fixes-36 0-24 visibility-only
//   widening was reverted. Retires the last [FX-NO-ENGINE] diag stub from
//   on_tick_audusd. Wired in tick_fx.hpp::on_tick_audusd() NZDUSD branch.
#include "NzdusdAsianOpenEngine.hpp"
// 2026-05-02: XauusdFvgEngine -- 15m FVG engine on XAUUSD. C++ port of
//   scripts/fvg_pnl_backtest_v3.py (v3 #5 ACCEPTED config). Cleared the
//   four-gate walk-forward bar at two independent train/test cutoffs
//   (PF 1.95 / 2.44 OOS). Shadow-only on first deployment regardless of
//   g_cfg.mode (pinned in engine_init.hpp). Wired into the gold cohort
//   exclusion gate via tick_gold.hpp::gold_any_open. See
//   docs/DESIGN_XAUUSD_FVG_ENGINE.md and HANDOFF_FVG_BACKTEST.md.
#include "XauusdFvgEngine.hpp"
// LogXauusdFvgCsv.hpp is included here (NOT from XauusdFvgEngine.hpp directly)
// so the engine header stays usable from the synthetic-trace verifier in
// backtest/verify_xauusd_fvg.cpp without dragging in the side-channel writer.
// engine_init.hpp wires the side-channel call inside g_xauusd_fvg.on_close_cb,
// and that call site needs the writer header visible at compile time.
#include "LogXauusdFvgCsv.hpp"
// 2026-05-05 (audit-fixes-40): per-engine liveness heartbeat. Detects "engine
//   wired in source but never receives ticks" failure mode (root-cause of the
//   2026-05-04..05 19h FX silence). Pulse calls in each tick dispatcher;
//   register_engine + check_misses + run_startup_self_test in main loop.
//   Header defines `static omega::EngineHeartbeat g_engine_heartbeat;` itself
//   per the SINGLE-TRANSLATION-UNIT include pattern.
#include "EngineHeartbeat.hpp"
// S11 P3b: g_hybrid_gold static decl removed (engine culled in P3a + P3b).
// S12 P3c (2026-05-07): GoldHybridBracketEngine.hpp file DELETED + #include
//   removed at line 347 above. Engine fully retired.
static omega::GoldMidScalperEngine            g_gold_midscalper;
static omega::GoldMicroScalperEngine          g_gold_microscalper;
// 2026-05-08 S20+: per-engine risk surveillance. Logging-only in v1.
static omega::RiskMonitor                     g_risk_monitor;
static omega::EurusdLondonOpenEngine          g_eurusd_london_open;
static omega::UsdjpyAsianOpenEngine           g_usdjpy_asian_open;
static omega::GbpusdLondonOpenEngine          g_gbpusd_london_open;
static omega::AudusdSydneyOpenEngine          g_audusd_sydney_open;
static omega::NzdusdAsianOpenEngine           g_nzdusd_asian_open;
// 2026-05-25 AtrMeanRevGrid -- forex mean-reversion grid (CRTP, see AtrMeanRevGridEngine.hpp).
// Ported from the AtrMeanRevGrid.mq5 MT5 EA. shadow_mode=true until backtest validates.
// Each instance: own indicator buffers + grid state. Seed via H1 CSV in engine_init.hpp.
#include "AtrMeanRevGridEngine.hpp"
static omega::AtrMeanRevGridEngine<omega::AmrTraits_EURUSD> g_amr_eurusd;
static omega::AtrMeanRevGridEngine<omega::AmrTraits_GBPUSD> g_amr_gbpusd;
static omega::AtrMeanRevGridEngine<omega::AmrTraits_AUDUSD> g_amr_audusd;
static omega::AtrMeanRevGridEngine<omega::AmrTraits_NZDUSD> g_amr_nzdusd;
// 2026-05-26 S37f -- EURGBP H1 X=5 SL=3 (validated: OOS PF 1.68, RF 1.39)
static omega::AtrMeanRevGridEngine<omega::AmrTraits_EURGBP> g_amr_eurgbp;

// 2026-05-26 S37g -- FxEnsembleEngine: 5 cross-family FX cells validated via
// deep edge hunt (fx_deep_hunt.py). Each instance enables one cell tuned to
// the pair's best edge per OOS validation. State-persistence inherited.
//   EURUSD H1 donchian_55 LONG SL=3 TP=3 MB=24  OOS PF 2.80 (Sharpe 1.75)
//   GBPUSD H2 bb_rev_20   LONG SL=3 TP=5 MB=96  OOS PF 3.24 (Sharpe 1.41)
//   AUDUSD H4 bb_rev_20   LONG SL=3 TP=2 MB=24  OOS PF inf  (thin n=5)
//   USDCAD H4 3bar_mom    SHORT SL=1.5 TP=5 MB=24 OOS PF 2.39 (Sharpe 1.76)
//   USDJPY H2 donchian_20 LONG SL=1.5 TP=5 MB=96 OOS PF 1.67 (Sharpe 0.88)
#include "FxEnsembleEngine.hpp"
static omega::FxEnsembleEngine g_fx_ens_eurusd("EURUSD");
static omega::FxEnsembleEngine g_fx_ens_gbpusd("GBPUSD");
static omega::FxEnsembleEngine g_fx_ens_audusd("AUDUSD");
static omega::FxEnsembleEngine g_fx_ens_usdcad("USDCAD");
static omega::FxEnsembleEngine g_fx_ens_usdjpy("USDJPY");
static omega::FxEnsembleEngine g_fx_ens_nzdusd("NZDUSD");  // S37h london_momo H2 SHORT
// 2026-05-26: Index AMR instances. Configs picked from deep eval sweep on
// real tick CSVs (SPXUSD/NSXUSD/GER40). See AtrMeanRevGridEngine.hpp traits.
static omega::AtrMeanRevGridEngine<omega::AmrTraits_US500>  g_amr_us500;
static omega::AtrMeanRevGridEngine<omega::AmrTraits_NAS100> g_amr_nas100;
static omega::AtrMeanRevGridEngine<omega::AmrTraits_GER40>  g_amr_ger40;
static omega::XauusdFvgEngine                 g_xauusd_fvg;
// 2026-05-18: GoldScalpPyramid -- M5 scalper with pyramid + aggressive trail
#include "GoldScalpPyramidEngine.hpp"
static omega::GoldScalpPyramidEngine          g_gold_scalp_pyramid;
// 2026-05-26 S38d: FxScalpPyramid -- 5 profitable FX pairs, shadow-mode.
//   Clone of GoldScalpPyramidEngine adapted for per-pair FX constants.
//   13-month standalone harness PnL @ 0.01 lot:
//     EURUSD +$1808 PF 1.56, USDJPY +$1688 PF 1.51, GBPUSD +$1207 PF 1.32,
//     USDCAD +$506 PF 1.23,  AUDUSD +$410 PF 1.23.
//   Skipped: NZDUSD (PF 1.07), EURGBP (PF 0.93).
//   All shadow_mode=true pending 14-day live shadow validation.
#include "FxScalpPyramidEngine.hpp"
static omega::FxScalpPyramidEngine            g_fx_scalp_eurusd;
static omega::FxScalpPyramidEngine            g_fx_scalp_usdjpy;
static omega::FxScalpPyramidEngine            g_fx_scalp_gbpusd;
static omega::FxScalpPyramidEngine            g_fx_scalp_usdcad;
static omega::FxScalpPyramidEngine            g_fx_scalp_audusd;
// 2026-05-31 S43: FX CARRY + CROSS-REVERSION -- first VALIDATED FX edges.
//   Carry (rate-diff premium, Dukascopy D1 2019-2026): full-11 carry-only
//   Sharpe 0.52, +12-14k bp, 5/6 blocks, cost-3x-proof. JPY crosses = 76-105%
//   of edge. Cross-RV D1: EURGBP PF 2.0, robust cluster. Both fidelity-passed
//   (backtest/fx_carry_engine_fidelity.cpp, fx_xrev_engine_fidelity.cpp).
//   shadow_mode=true. carry on 8 fed pairs (6 routed + EURJPY/GBPJPY); x-rev EURGBP.
#include "FxCarryEngine.hpp"
static omega::FxCarryEngine g_fx_carry_eurusd("EURUSD");
static omega::FxCarryEngine g_fx_carry_gbpusd("GBPUSD");
static omega::FxCarryEngine g_fx_carry_usdjpy("USDJPY");
static omega::FxCarryEngine g_fx_carry_audusd("AUDUSD");
static omega::FxCarryEngine g_fx_carry_nzdusd("NZDUSD");
static omega::FxCarryEngine g_fx_carry_usdcad("USDCAD");
static omega::FxCarryEngine g_fx_carry_eurjpy("EURJPY");
static omega::FxCarryEngine g_fx_carry_gbpjpy("GBPJPY");
#include "FxCrossRevEngine.hpp"
static omega::FxCrossRevEngine g_fx_xrev_eurgbp("EURGBP");
// 2026-05-31 S43f: FxSeasonal (Friday-long) -- component of the validated combined
//   thin-edge sleeve (Friday+COT+session, combined Sharpe 0.81, uncorrelated). Friday
//   leg weekly Sh 1.50 5/6 blocks. Cost-sensitive -> shadow. 9 pairs.
#include "FxSeasonalEngine.hpp"
static omega::FxSeasonalEngine g_fx_seas_eurusd("EURUSD");
static omega::FxSeasonalEngine g_fx_seas_gbpusd("GBPUSD");
static omega::FxSeasonalEngine g_fx_seas_usdjpy("USDJPY");
static omega::FxSeasonalEngine g_fx_seas_audusd("AUDUSD");
static omega::FxSeasonalEngine g_fx_seas_nzdusd("NZDUSD");
static omega::FxSeasonalEngine g_fx_seas_usdcad("USDCAD");
static omega::FxSeasonalEngine g_fx_seas_usdchf("USDCHF");
static omega::FxSeasonalEngine g_fx_seas_eurgbp("EURGBP");
static omega::FxSeasonalEngine g_fx_seas_eurjpy("EURJPY");
// S44 2026-05-31: IndexSeasonal -- equity-index day-of-week seasonality (Tue+Fri long).
//   best-2 sleeve Sharpe 0.69 vs 0.36 buy&hold, regime-robust, blk 5/6 (index_seasonal_sharpe.cpp).
#include "IndexSeasonalEngine.hpp"
static omega::IndexSeasonalEngine g_idx_seas_us500("US500.F");
static omega::IndexSeasonalEngine g_idx_seas_ustec("USTEC.F");
static omega::IndexSeasonalEngine g_idx_seas_ger40("GER40");
static omega::IndexSeasonalEngine g_idx_seas_dj30("DJ30.F");
static omega::IndexSeasonalEngine g_idx_seas_uk100("UK100");
static omega::IndexSeasonalEngine g_idx_seas_estx50("ESTX50");
// S44: IndexFomc -- pre-FOMC drift, US indices (decayed but alive, +11.8bp/event 2023-26).
#include "IndexFomcEngine.hpp"
static omega::IndexFomcEngine g_idx_fomc_us500("US500.F");
static omega::IndexFomcEngine g_idx_fomc_ustec("USTEC.F");
static omega::IndexFomcEngine g_idx_fomc_dj30("DJ30.F");
// 2026-05-19 S110: GoldRegimeDaily -- H4 EMA-cross trend-follow.
//   First gold engine to clear PF>1.20 AND PnL>$5K success criterion on 2025/6.
//   PF 2.35 / WR 92.6% / PnL $5,854 / N=54 trades over 16 months.
//   Mechanism: EMA9-cross-EMA21 on H4 + cost-cover BE (5pt) + tight trail
//   (BE-lock only) + trend-flip exit (EMA9 crosses back).
//   RISK_DOLLARS=$1200 (scaled 24x from default $50), LOT_MAX=2.50.
//   shadow_mode=true, enabled=false until operator approval.
#include "GoldRegimeDailyEngine.hpp"
static omega::GoldRegimeDailyEngine           g_gold_regime_daily;
// 2026-05-18 (part B): BBandScalp -- M1 Bollinger + RSI mean-reversion scalper
//   Structural-signal entry (BB extreme touch + RSI extreme) replacing the
//   tick-velocity entry that proved counter-predictive in QuickScalp.
//   shadow_mode=true by default. See engine class header for full design.
#include "BBandScalpEngine.hpp"
static omega::BBandScalpEngine                g_bband_scalp;
// S11 P3b: g_hybrid_sp / g_hybrid_nq / g_hybrid_us30 / g_hybrid_nas100 static
//   decls removed (engines culled in P3a + P3b).
// S12 P3c (2026-05-07): IndexHybridBracketEngine.hpp file DELETED + #include
//   removed at line 347 above. Engine family fully retired.

// Bug #3 (KNOWN_BUGS.md) cross-engine "index any open" predicate.
// Mirrors gold_any_open at tick_gold.hpp:36-50.
// S11 P3b: g_hybrid_sp/nq/us30/nas100 OR-terms removed -- engines culled in
//   P3a, globals/init removed in P3b. Predicate now covers iflow_* + minimal_h4_us30.
static inline bool index_any_open() noexcept {
    return  g_iflow_sp.has_open_position()      ||
            g_iflow_nq.has_open_position()      ||
            g_iflow_nas.has_open_position()     ||
            g_iflow_us30.has_open_position()    ||
            g_minimal_h4_us30.has_open_position();
}

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
// g_l2_watchdog_dead removed S13 2026-05-08 -- L2 watchdog thread + cTrader
// depth client both culled. FIX 264=0 owns L2 now; staleness is detected via
// the L2 tick CSV verifier instead.

// ?? FEED-STALE gate (DORMANT after S13) ??????????????????????????????????????
// Originally set by CTraderDepthClient recv_loop when the depth subscription
// ACK'd but no events flowed. With the cTrader surface culled (S13 2026-05-08)
// nothing writes this flag any more, but it stays defined because tick_gold.hpp
// entry gates still read it -- they now see a permanently-false value, which
// is correct behaviour (FIX 264=0 owns L2).
std::atomic<bool>            g_feed_stale_xauusd{false};

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
// 2026-05-30: AtomicL2 type + g_l2_* globals moved to L2Globals.hpp so
// engine headers can include them without cyclic deps through globals.hpp.
#include "L2Globals.hpp"

// cTrader depth tick staleness tracker (g_ct_ms_* + get_ctrader_tick_ms_ptr)
// removed S13 2026-05-08 -- cTrader Open API surface culled. FIX 264=0 is the
// sole L2 source; per-symbol staleness is now tracked via AtomicL2::last_update_ms.

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


// =============================================================================
//  retired_micro_engines.hpp  —  TOMBSTONE LIST (operator directive S-2026-06-19)
// =============================================================================
//  The sub-minute / microstructure / scalp engine class is RETIRED from the
//  active Omega system. These engines remain in the repo ONLY as historical
//  audit + backtest evidence. They MUST NOT be re-enabled, re-wired into
//  dispatch, or shown in the active engine dashboard without a full documented
//  edge (faithful tick BT + cross-regime walk-forward + 30-trade shadow), and
//  approval through the central brain (Phase 2).
//
//  STANDING RULE — no new engine of any of these classes:
//    * NO M1/M5 microstructure scalping
//    * NO spread-dislocation scalping
//    * NO latency-edge scalping
//    * NO Bollinger-band scalp
//    * NO gold microscalper
//    * NO FX scalp pyramid
//    * NO GoldScalpPyramid
//    * NO sub-minute / tick-triggered profit nibbling
//  Ticks are for honest fills / spread / slippage / execution sim ONLY — never
//  for micro ENTRY logic.
//
//  EVIDENCE (from the repo's own audits):
//    GoldBracket PF 0.36 · XauusdFvg 0.50 · VWAPRev 0.54/0.95 · TrendPB 0.24/0.27
//    BBandScalp 27-cfg / 154M-tick sweep PF 0.07-0.09, WR 7-8%
//    GoldRegimeDaily block: 78 high-frequency M5-M15 gold configs all NEGATIVE
//      after retail costs.
//
//  RETIRED ENGINES (all enabled=false in engine_init.hpp as of S-2026-06-19d/e):
//    g_gold_microscalper      (MicroScalperGold)     — last live scalp, now off
//    g_gold_midscalper        (MidScalperGold)       — registration removed 2026-06-02
//    g_gold_scalp_pyramid     (GoldScalpPyramid)     — S45
//    g_bband_scalp            (BBandScalp)           — 154M-tick sweep
//    g_fx_scalp_eurusd/usdjpy/gbpusd/usdcad/audusd   (FxScalpPyramid x5) —
//        CONFIG-AUTHORITY FIX: struct default enabled=true + no explicit disable
//        meant these were LIVE (shadow) despite "S45-disabled" docs. Now hard off.
//    g_nbm_gold_london        (NbmGoldLondon)        — S91
//
//  CONFIG-AUTHORITY MANDATE (Phase 0): every enabled=false MUST disable that
//  exact engine/cell at runtime. No "documentation-only" config. The startup
//  ENGINE_TABLE log is the audit surface — anything not in it is not trading.
// =============================================================================
#pragma once
// Intentionally header-only documentation. No code: the retirement is enforced
// by enabled=false in engine_init.hpp, not by this file. This file is the
// human + AI reference so no future session re-promotes a scalp engine by
// accident.

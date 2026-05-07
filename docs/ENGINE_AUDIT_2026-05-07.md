# ENGINE AUDIT — 2026-05-07

Foundation document. Started at the end of session 2026-05-07. Continue in next session.

**Goal:** enumerate every engine instance, classify status (live/shadow/disabled/dead), capture recent patch history, identify improvement candidates and gaps where new engines might add edge.

**Source of truth:** `include/globals.hpp` (all `static omega::*` engine declarations).

---

## Inventory by family (from globals.hpp scan, 2026-05-07)

### A. Compression-breakout cohort (FX + Gold session-open) — **PATCHED THIS SESSION**

| Instance | Class | Symbol | Window UTC | Status | S58 | S59 | S61 | Notes |
|---|---|---|---|---|---|---|---|---|
| g_eurusd_london_open | EurusdLondonOpenEngine | EURUSD | 06-09 | shadow | ✅ | ✅ | ✅ | Lineage source for FX cohort. 14mo backtest WR 66.6%. |
| g_usdjpy_asian_open | UsdjpyAsianOpenEngine | USDJPY | 00-04 | shadow | ✅ | ✅ | ✅ | S59 sweep tuned MIN_RANGE=0.20. |
| g_gbpusd_london_open | GbpusdLondonOpenEngine | GBPUSD | 07-10 | shadow | ✅ | ✅ | ✅ | S59 cohort projected $233/10d savings. |
| g_audusd_sydney_open | AudusdSydneyOpenEngine | AUDUSD | 22-02 | shadow | ✅ | ✅ | ✅ | Sydney pre-NY-close window. |
| g_nzdusd_asian_open | NzdusdAsianOpenEngine | NZDUSD | various | shadow | ✅ | ✅ | ✅ | Wider MAX_FILL_SPREAD (5pip) per NZD spread profile. |
| g_gold_midscalper | GoldMidScalperEngine | XAUUSD | mid-session | shadow | ✅ | ✅ | ✅ | S53 baseline; lineage source for FX cohort architecture. |

**Open items for this family:**
- Promotion to live requires 2-week clean shadow data. Watch: WR >= 60% OOS, no `COLD_START_BLOCK` after `RANGE_HIST_LOAD ok` in restart logs, `FILL_SPREAD_REJECT` count < 5% of fires.
- LOT_MAX currently capped at 0.10 (S58 conservative cut from S56's 0.20). Restoration to half-Kelly 0.20 candidate after 2-week confirmation.

---

### B. HTF/Swing portfolios (XAUUSD trend strategies)

| Instance | Class | Status | Recent activity | Notes |
|---|---|---|---|---|
| g_h1_swing_gold | H1SwingEngine | ? | unknown | XAUUSD H1 EMA+ADX trend |
| g_h4_regime_gold | H4RegimeEngine | ? | unknown | XAUUSD H4 Donchian breakout |
| g_minimal_h4_gold | MinimalH4Breakout | ? | unknown | Pure H4 Donchian |
| g_minimal_h4_us30 | MinimalH4US30Breakout | ? | S26 2026-04-25 warm-restart state | DJ30.F H4 Donchian |
| g_c1_retuned | C1RetunedPortfolio | ? | Donchian H1 + Bollinger H2/H4/H6 long | |
| **g_tsmom** | **TsmomPortfolio** | **shadow** | **MAE_EXIT_ATR=2.0 deployed 2026-05-04** | **5 cells: H1/H2/H4/H6/D1 long. Investigated this session.** |
| g_tsmom_v2 | TsmomPortfolioV2 | ? | unknown | Same 5 cells, V2 variant |
| g_donchian | DonchianPortfolio | ? | 7 cells: H2L, H4L+S, H6L+S, D1L+S | |
| g_ema_pullback | EpbPortfolio | ? | 4 cells: H1/H2/H4/H6 long | |
| g_trend_rider | TrendRiderPortfolio | ? | 6 cells: H2L+S, H4L+S, H6L, D1L | |

**Open items for this family:**
- **Tsmom backtest re-validation** (high priority, see handoff doc) — MAE_EXIT_ATR breaks parity with `phase1/signal_discovery/post_cut_revalidate_all.py`.
- All other engines have unknown current status. Need: pull recent shadow ledger, count fires/PnL per engine over last 30 days, flag any engine with <5 fires/month or net-negative PnL.

---

### C. Cross-asset engines (`omega::cross::`)

| Instance | Class | Symbol | Status | Notes |
|---|---|---|---|---|
| g_ca_esnq | EsNqDivergenceEngine | ES/NQ | ? | ES vs NQ divergence trade |
| g_ca_eia_fade | OilEventFadeEngine | USOIL.F | ? | Wed 15:30 UTC EIA inventory fade |
| g_ca_brent_wti | BrentWtiSpreadEngine | BRENT/USOIL | ? | Brent-WTI spread mean-reversion |
| g_ca_fx_cascade | FxCascadeEngine | FX | ? | FX cascade momentum |
| g_ca_carry_unwind | CarryUnwindEngine | FX/cross | ? | Risk-off carry unwind |
| g_orb_us | OpeningRangeEngine | US equity | 13:30 UTC | NY 13:30 open ORB |
| g_orb_ger30 | OpeningRangeEngine | GER40 | 08:00 UTC | Xetra 08:00 ORB |
| g_orb_uk100 | OpeningRangeEngine | UK100 | 08:00 UTC | LSE 08:00 ORB, 15-min |
| g_orb_estx50 | OpeningRangeEngine | ESTX50 | 09:00 UTC | Euronext 09:00 ORB, 15-min |
| g_vwap_rev_sp | VWAPReversionEngine | US500.F | ? | |
| g_vwap_rev_nq | VWAPReversionEngine | USTEC.F | ? | |
| g_vwap_rev_ger40 | VWAPReversionEngine | GER40 | ? | |
| g_vwap_rev_eurusd | VWAPReversionEngine | EURUSD | ? | |
| g_nbm_sp | NoiseBandMomentumEngine | US500.F | ? | NY session |
| g_nbm_nq | NoiseBandMomentumEngine | USTEC.F | ? | NY session |
| g_nbm_nas | NoiseBandMomentumEngine | NAS100 | ? | NY session |
| g_nbm_us30 | NoiseBandMomentumEngine | DJ30.F | ? | NY session |
| g_nbm_gold_london | NoiseBandMomentumEngine | XAUUSD | London 07:00-13:30 | |
| g_nbm_oil_london | NoiseBandMomentumEngine | USOIL.F | London 07:00-13:30 | |
| g_trend_pb_gold | TrendPullbackEngine | XAUUSD | shadow | save_state in shutdown sequence |
| g_trend_pb_ger40 | TrendPullbackEngine | GER40 | shadow | save_state in shutdown sequence |
| g_trend_pb_nq | TrendPullbackEngine | USTEC.F | shadow | save_state in shutdown sequence |
| g_trend_pb_sp | TrendPullbackEngine | US500.F | shadow | save_state in shutdown sequence |

---

### D. Index engines (`omega::idx::`)

| Instance | Class | Symbol | Status | Notes |
|---|---|---|---|---|
| g_iflow_sp | IndexFlowEngine | US500.F | ? | |
| g_iflow_nq | IndexFlowEngine | USTEC.F | ? | |
| g_iflow_nas | IndexFlowEngine | NAS100 | ? | |
| g_iflow_us30 | IndexFlowEngine | DJ30.F | ? | |
| g_imacro_sp | IndexMacroCrashEngine | US500.F | ? | |
| g_imacro_nq | IndexMacroCrashEngine | USTEC.F | ? | |
| g_imacro_nas | IndexMacroCrashEngine | NAS100 | ? | |
| g_imacro_us30 | IndexMacroCrashEngine | DJ30.F | ? | |
| g_vwap_atr_trail_sp | VWAPAtrTrail | US500.F | ? | |
| g_vwap_atr_trail_nq | VWAPAtrTrail | USTEC.F | ? | |
| g_vwap_atr_trail_nas | VWAPAtrTrail | NAS100 | comment: "no VWAPRev, unused" | LIKELY DEAD |
| g_vwap_atr_trail_us30 | VWAPAtrTrail | DJ30.F | comment: "no VWAPRev, unused" | LIKELY DEAD |

---

### E. Bracket engines (legacy)

`g_bracket_*` for all symbols. The handoff doc for EurusdLondonOpen flagged these as "dead-code (configured every startup but never invoked from any dispatch path)". Need to verify which ones still have dispatch paths and which are zombies.

| Instance | Class | Symbol | Active dispatch? |
|---|---|---|---|
| g_bracket_gold | GoldBracketEngine | XAUUSD | ? |
| g_bracket_sp/nq/us30/nas100 | BracketEngine | indices | ? |
| g_bracket_ger30/uk100/estx50 | BracketEngine | EU indices | ? |
| g_bracket_brent | BracketEngine | BRENT | ? |
| g_bracket_eurusd/gbpusd/audusd/nzdusd/usdjpy | BracketEngine | FX | LIKELY DEAD (per EUR engine handoff comment about g_bracket_eurusd dead-code) |

**Audit task:** grep tick_*.hpp dispatchers for each `g_bracket_*` to confirm dispatch wiring. The 5 FX BracketEngines were superseded by the new *LondonOpen/AsianOpen/SydneyOpen variants this session — they should be removed or formally deprecated.

---

### F. Per-symbol primary engines (Tier 0)

| Instance | Class | Symbol |
|---|---|---|
| g_eng_sp | SpEngine | US500.F |
| g_eng_nq | NqEngine | USTEC.F |
| g_eng_cl | OilEngine | USOIL.F |
| g_eng_us30 | Us30Engine | DJ30.F |
| g_eng_nas100 | Nas100Engine | NAS100 |
| g_eng_ger30 | EuIndexEngine | GER40 |
| g_eng_uk100 | EuIndexEngine | UK100 |
| g_eng_estx50 | EuIndexEngine | ESTX50 |
| g_eng_eurusd/gbpusd/audusd/nzdusd/usdjpy | BreakoutEngine | FX |
| g_eng_brent | BrentEngine | BRENT |

**Audit task:** these are the original "tier 0" engines. Last status check: 2026-04-06 global FX disable affected the FX BreakoutEngines (now superseded). Need: confirm each is still wired and producing fires; flag any that haven't traded in 30 days.

---

### G. Misc / standalone

| Instance | Class | Status | Notes |
|---|---|---|---|
| g_gold_stack | GoldEngineStack | ? | Multiple sub-engines per `GoldEngineStack.hpp`. Has save_atr_state. |
| g_xauusd_fvg | XauusdFvgEngine | ? | XAUUSD FVG (fair value gap) engine |
| g_macro_crash | MacroCrashEngine | DISABLED 2026-04-30 | Documented loss source: 4.8% WR, -10,849pts. Should be confirmed as disabled in `enabled` flag. |
| g_candle_flow | CandleFlowEngine | shadow only | comment: "candle+DOM engine, shadow only" |
| g_ema_cross | EMACrossEngine | ? | |
| g_pdhl_rev | PDHLReversionEngine | ? | "mean reversion inside daily range" |
| g_rsi_reversal | RSIReversalEngine | SHADOW (persistent WARN) | known shadow status |
| g_rsi_extreme | RSIExtremeTurnEngine | ? | "RSI extreme + sustained turn engine" |

---

### H. Deleted engines (ghosts to clean up)

- HybridBracketGold (`GoldHybridBracketEngine`) — DELETED in S12 P3c (2026-05-07).
- IndexHybridBracketEngine family (g_hybrid_sp/nq/us30/nas100) — DELETED in S12 P3c (2026-05-07).
- Stale references survive in commented-out code (e.g. `EMACrossEngine.hpp:65`) and doc comments. Low priority cleanup.

---

## Suggested audit workflow for next session

### Phase 1 — Inventory verification (1-2 hours)
1. For each `g_*` instance, confirm whether it has an active dispatch path in `tick_*.hpp` / `quote_loop.hpp`. Engines without dispatch are dead globals (memory only).
2. For each engine with active dispatch, confirm the `enabled` flag default in `engine_init.hpp`.
3. Output: a definitive list of LIVE / SHADOW / DISABLED / DEAD per engine.

### Phase 2 — Performance pull (1 session)
1. Pull last 30 days of `omega_shadow.csv` from VPS (or wherever the per-engine ledger lives).
2. Count fires per engine, sum of pnl, WR, avg trade. Flag:
   - <5 fires/month → underused, candidate for parameter widening or dispatch debugging
   - net-negative PnL → candidate for disable or rework
   - extreme outlier losses → candidate for SL/MAE-style guard

### Phase 3 — Improvement queue
1. Cross-reference recent docs (`docs/SESSION_*.md`) to capture recent patch lineage per engine — many will have known issues already documented.
2. Generate a prioritized "next patches" list ranked by expected $ impact / 10d (based on shadow ledger).
3. Identify gaps: symbols without an engine, sessions without coverage, microstructure regimes not yet exploited.

### Phase 4 — Removal pass
1. Remove zombie globals (declared in `globals.hpp` but with no dispatch).
2. Remove commented-out references to deleted engines (`g_hybrid_*`).
3. Reduce `globals.hpp` from current ~70 instances to actual live set.

---

## What I know from this session's exploration (carry-forward facts)

- `state_root_dir()` in `globals.hpp:814` returns `C:\Omega\state` on Windows; auto-creates the dir.
- `g_news_blackout` lives at `omega_types.hpp:332` — file-scope global, used by all FX/gold engines for NFP/CPI/FOMC/ECB blackouts.
- The codebase uses SINGLE-TRANSLATION-UNIT include model — `omega_types.hpp` -> `globals.hpp` -> engine headers, all in one TU compiled by `omega_main.cpp`.
- `OMEGA_BACKTEST` macro disables runtime I/O; backtest binaries link `backtest/<sym>_bt/<Engine>.hpp` (frozen variants) instead of `include/<Engine>.hpp` (live).
- Persistence convention: CSV via `fopen`/`fprintf`/`fgets`/`sscanf` (matches `OmegaAdaptiveRisk` and `OmegaEdges` patterns). State files at `state_root_dir()`.
- Shutdown save convention: explicit `g_*.save_*` calls in `omega_main.hpp` lines ~1100-1124 (gold_stack, trend_pb_*, kelly, bars).

---

*This document is a starting point. Next session should pick up at Phase 1 above. The handoff doc `SESSION_2026-05-07_S61_HANDOFF.md` covers all the immediate-priority items separate from this audit.*

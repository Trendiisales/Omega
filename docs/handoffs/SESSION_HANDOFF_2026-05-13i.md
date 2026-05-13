# Session Handoff — 2026-05-13 (NZST), part I

Read this first next session. Direct follow-up to part-H
(`SESSION_HANDOFF_2026-05-13h.md`). This session was a build-rescue +
follow-up planning session, not a fresh feature session.

## TL;DR

1. **origin/main was broken when this session started**. Commit
   `1e8d19e` ("S65: GUI position-source expansion") shipped to origin
   without a Mac canary build — the C++ wouldn't compile. Cause: 4
   broken position-source registrations in `engine_init.hpp`. **Fixed
   and pushed as commit `e0ebdca` ("S65-fix")**. Mac canary green;
   VPS deploy will now succeed.
2. **The part-H handoff was stale.** S62 / S63 / S64 / S65 were all
   already committed to origin/main before the session started, not
   in working tree as the handoff described. Two stale commit-push
   scripts (`S63_commit_push.sh`, `S64_build_commit_push.sh`,
   `S65_build_commit_push.sh`) cleaned up — the S64 deletion is folded
   into `e0ebdca`.
3. **S66 (GUI position-source expansion) is partially done in the
   working tree** — 6 of 9 engine families registered. NOT yet
   committed; awaiting operator review + canary build verification
   before commit.
4. **Two recommendation docs written** for next-session work:
   PDHL S63 tuning + simplest-effective gold engine direction.

## Commits this session

| commit | message | files |
|--------|---------|-------|
| `e0ebdca` (origin/main) | S65-fix: build break in engine_init.hpp + VWAPReversionEngine GUI accessors | CrossAssetEngines.hpp (+9), engine_init.hpp (mixed), S64_build_commit_push.sh (deleted) |

The S65-fix commit:
* Added 5 read-only public accessors to `VWAPReversionEngine`
  (`open_is_long` / `open_entry` / `open_sl` / `open_tp` / `open_size`)
  mirroring NoiseBandMomentum / TrendPullback convention.
* Rewrote 4 broken S65 registrations to use public accessors:
  XauThreeBar30m (used non-existent `p.size` — switched to
  `g_xau_threebar_30m.lot`), NBM gold-london, VWAPReversion x4 lambda,
  TrendPullback x2 lambda.
* mfe / mae set to 0.0 for the 3 private-pos engines (matches
  MinimalH4US30 precedent). Full mfe/mae exposure is S66-followup if
  needed.

## S66 partial — working tree

`include/engine_init.hpp` has 6 new register_source calls added before
the source-count cout. Bumps the count from 20 to 26 sources.

Engines registered this session:

| engine global | engine class | symbol | pos shape | mae tracked? |
|---|---|---|---|---|
| `g_ema_cross` | EMACrossEngine | XAUUSD | public `pos` | no |
| `g_h4_regime_gold` | H4RegimeEngine | XAUUSD | public `pos_` (struct) | no |
| `g_macro_crash` | MacroCrashEngine | XAUUSD | public `pos` (base only, not pyramid adds) | yes |
| `g_xau_tf_2h` | XauTrendFollow2hEngine | XAUUSD | `std::array<XauTf2hPos, kXauTf2hNumCells>` | yes |
| `g_xau_tf_4h` | XauTrendFollow4hEngine | XAUUSD | `std::array<XauTfPos, kXauTfNumCells>` | yes |
| `g_xau_tf_d1` | XauTrendFollowD1Engine | XAUUSD | `std::array<XauTfD1Pos, kXauTfD1NumCells>` | yes |

The three XauTrendFollow engines share an `_make_xau_tf_source` generic
lambda factory (C++14 generic lambda) since their pos struct field
names are identical (`active`, `is_long`, `entry_px`, `mfe`, `mae`).
Each emits one PositionSnapshot per active cell — these are multi-cell
engines (3–6 cells each).

**Operator action required before commit:**

```bash
cd ~/omega_repo
cmake --build build --target OmegaBacktest -j 2>&1 | tail -20
git diff --stat include/engine_init.hpp   # eyeball the change
```

If `OmegaBacktest` is green, commit + push:

```bash
git add include/engine_init.hpp
git commit -m "S66 partial: 6 more GUI position sources (EMACross/H4Regime/MacroCrash/XauTrendFollow 2h/4h/D1)"
git push origin main
```

The remaining S66 work (9 engine families) is **deferred to
S66-followup**:

* `BracketEngine` (XAU + 12 FX/index instances; pyramid leg array)
* `GoldEngineStack` (18 sub-engines via `legs_` vector)
* `IndexFlowEngine` x4 + `IndexMacroCrashEngine` x4
* `CandleFlowEngine` (multi-path state)
* `UstecTrendFollow5mEngine` + `UstecTrendFollowHtfEngine`
* `g_h1_swing_gold` (H1SwingEngine)
* Portfolio wrappers: `g_c1_retuned`, `g_donchian`, `g_ema_pullback`,
  `g_trend_rider`, `g_tsmom`, `g_tsmom_v2`
* FX `BreakoutEngine` instances: `g_eng_eurusd/gbpusd/audusd/nzdusd/usdjpy`

Per the part-H notes, these have non-CrossPosition shapes — each
registration is ~25 lines but needs case-by-case pos struct
inspection. Plan: read each engine's pos struct (Grep), mirror the
existing template. ~250 lines total addition.

## Recommendation docs (next-session reading)

1. `outputs/S63_PDHL_TUNING_RECOMMENDATION_2026-05-13.md` — PDHL
   defaults (LOSS_CUT_PCT=0.04 / BE_ARM_PCT=0.025 / BE_BUFFER_PCT=0.01)
   are **redundant, not too loose** — they relabel exits without
   capturing new edge. Proposed tighter values (0.025 / 0.020 / 0.008)
   plus a verification harness command. Single-block `engine_init.hpp`
   patch ready to paste.
2. `outputs/SIMPLEST_GOLD_ENGINE_RECOMMENDATION_2026-05-13.md` —
   **Don't build a fresh engine.** Promote `g_minimal_h4_gold` first
   (H4 Donchian breakout, 4-param surface, 27/27 sweep configs
   profitable, currently shadow_mode). `g_bracket_gold` is the
   parallel candidate but is audit-disabled (-$324 / 12.8% WR in 4wk
   shadow). Recommendation includes the smallest-possible parameter
   surface for a fresh engine if one is ever needed.

## Carry-over from part-H still pending

* **VPS deploy** (operator-Windows-side):
  ```powershell
  cd C:\Omega
  .\OMEGA.ps1 deploy
  ```
  Lands S62 / S63 / S64 / S65 / S65-fix on the live trading box.
  Expected log lines on first launch (from part-H):
  ```
  [GBP-LDN-OPEN] RANGE_HIST_LOAD ok n=N age_s=M
  [GBP-LDN-OPEN] POST_TRADE_BLOCK_LOAD restored=... consec_count=N consec_block_rem_s=M
  (same for EUR/AUD/NZD/JPY)
  [OmegaApi] g_open_positions sources registered (20 sources: ...)
  ```
  After S66 partial lands, the last line will show **26 sources**.

* **S63 LOSS_CUT/BE_RATCHET tuning** for the other 6 S63 engines
  (NBM gold-london, XauusdFvg, XauThreeBar30m, IndexMacroCrash x4,
  RSIReversal, IndexFlow x4). The PDHL recommendation doc above is
  the template; apply per-engine after the PDHL tightening is
  backtest-verified. Index engines need an index tape
  (`~/Tick/SPXUSD/` etc.) which the operator has.

## Files modified this session — working tree (post-push)

```
M include/engine_init.hpp                                           (S66 partial)
?? outputs/S63_PDHL_TUNING_RECOMMENDATION_2026-05-13.md              (new)
?? outputs/SIMPLEST_GOLD_ENGINE_RECOMMENDATION_2026-05-13.md         (new)
?? docs/handoffs/SESSION_HANDOFF_2026-05-13i.md                      (this file)
```

The two outputs/ docs are typically gitignored (session-local). The
handoff doc lives in docs/handoffs/ and ships with commits.

## Files NOT modified

Per CLAUDE.md core list:
* `OmegaCostGuard.hpp` — untouched
* `OmegaTradeLedger.hpp` — untouched
* `SymbolConfig.hpp` — untouched
* `OmegaFIX.hpp` — untouched
* `OmegaApiServer.hpp` — untouched
* `GoldPositionManager.hpp` — untouched
* `on_tick.hpp` / `trade_lifecycle.hpp` / `order_exec.hpp` — untouched

Engine-class additions limited to:
* `CrossAssetEngines.hpp` — 5 additive public accessor one-liners on
  `VWAPReversionEngine` (S65-fix, already in `e0ebdca`). Matches the
  pre-existing NBM/TrendPullback convention. No behaviour change.

## Standing audit at session end

CLAUDE.md ungated-engine sweep continues to expect ONLY:
`LatencyEdgeEngines` (S13 culled), `RSIExtremeTurnEngine` (S52
disabled), `SweepableEngines` + `SweepableEnginesCRTP` (research-only,
not in live runtime). All other production engines remain cost-gated.
No regression today.

## Pre-commit checklist for next session

Before any S66-followup commits, verify:

1. `cmake --build build --target OmegaBacktest -j` is green on Mac
2. Pos struct shape for the engine has been read (Grep)
3. Public accessors exist OR added as additive one-liners (don't add
   private member access without an explicit getter)
4. `tick_value_multiplier` accepts the symbol string
5. `g_last_tick_bid.find(sym)` covers the symbol (otherwise
   `current = entry` is the fallback)
6. Source count in the trailing `std::cout` is bumped to match

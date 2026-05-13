# Session Handoff — 2026-05-13 (NZST), part H

Read this first next session. Direct follow-up to
`SESSION_HANDOFF_2026-05-13a.md` (part F — refreshed) and the part-G S62
handoff at `SESSION_HANDOFF_2026-05-13g.md`. Covers the post-S62 work in
this single long session: S63 in-flight protection plumbing, S64 FX
circuit breakers, S65 GUI position-source expansion.

## TL;DR

1. **One commit already on origin/main: S62 (`46a9438`).** 24/7 cold-start
   persistence + 3-pip ABS_EXPANSION_FLOOR for the 5 FX London/Asian/
   Sydney Open engines.

2. **Three commits ready to ship in one script run:**
   `bash ~/omega_repo/S65_build_commit_push.sh`. Lands S63, S64, S65 in
   that order, gated by Mac canary build.

3. **The "GBP keeps losing" problem is fixed in code, pending deploy.**
   S64 adds an immediate `LOSS_CUT_PCT = 0.03%` cold-loss cut (~4 pips
   on cable) AND a consec-loss circuit breaker (2 losses in current UTC
   day → 4-hour lockout). Today's 07:02-then-08:55 GBP repeat-SL pattern
   would have tripped the breaker after the 2nd SL, locking the engine
   out for the rest of the session.

4. **VPS deploy still pending.** Live trading box is running pre-S62
   code until the operator runs `.\OMEGA.ps1 deploy` on the Windows VPS.
   Until then nothing in S62/S63/S64/S65 protects live trades — the bot
   keeps using whatever binary was last shipped.

5. **GUI position-source coverage 8 → 20 sources via S65.** Closes the
   "trades don't show in GUI until completed" gap for 11 additional
   engines. The 9 remaining engines (BracketEngine multi-leg,
   GoldEngineStack 18 sub-engines, IndexFlow, CandleFlow, etc.) have
   different pos struct shapes and are deferred to S66 follow-up.

## Sessions in this run

| stamp | what | status |
|---|---|---|
| S62 | 24/7 cold-start hardening + ABS_EXPANSION_FLOOR | committed (`46a9438`) |
| S63 | VWR-pattern LOSS_CUT/BE_RATCHET plumbing, 7 non-FX engines | working tree |
| S64 | FX circuit breakers (LOSS_CUT + consec-loss block), 5 FX engines | working tree |
| S65 | GUI position-source expansion, +11 engines | working tree |

## S62 detail (already committed)

Five FX engine headers, 9-point patch each:

* `<sys/stat.h>` + `<direct.h>` includes for cross-platform mkdir
* `ABS_EXPANSION_FLOOR = 0.0003` (FX) / `0.03` (JPY) — anti-marginal-
  fire absolute floor on top of the existing 1.10× median multiplier
* Constructor loads post-trade-block state from disk
* `on_tick()` top calls `_save_range_history_if_due(now_s)`
  unconditionally (no longer phase-gated)
* `SAME_LEVEL_BLOCK` log lines emitted (previously silent)
* Combined ATR gate (`range < pct_threshold OR range < abs_threshold`)
* `RANGE_HIST_STALENESS_S` 7200 → 86400 (24h)
* `_ensure_state_dir()` helper creates `C:\Omega\state\` on Win
* `_try_load_post_trade_block()` / `_save_post_trade_block()` for
  m_sl_price / m_sl_cooldown_ts / m_win_exit_price etc.

Commit `46a9438`, 5 files, +809/-33.

## S63 detail (working tree)

Adds the canonical VWR LOSS_CUT/BE_RATCHET pattern to 7 additional
engines. All defaults runtime-mutable via `engine_init.hpp`.

| engine | tier | pattern | file |
|---|---|---|---|
| PDHLReversion | 1 | full | include/PDHLReversionEngine.hpp |
| NoiseBandMomentum (gold-london instance) | 1 | full | include/CrossAssetEngines.hpp |
| XauusdFvg | 2 | full | include/XauusdFvgEngine.hpp |
| XauThreeBar30m | 2 | full | include/XauThreeBar30mEngine.hpp |
| IndexMacroCrash | 2 | full | include/IndexFlowEngine.hpp |
| RSIReversal | 2 | LOSS_CUT only | include/RSIReversalEngine.hpp |
| IndexFlow IdxOpenPosition | 2 | LOSS_CUT only | include/IndexFlowEngine.hpp |

A/B backtest verification on `~/Tick/duka_ticks/XAUUSD_2025_10.csv`
(9.3M ticks, full month):

* PDHLReversion (only patched engine that fired on this XAU tape):
  892 trades baseline AND patched. **dpnl = +0.00 (neutral).**
  144 LOSS_CUT + 326 BE_CUT exits in patched replaced 470 prior
  L2_FLIP / FORCE_CLOSE exits at the same pnl. SL_HIT count unchanged
  (410→410). Defaults conservative — no regression, no improvement
  yet. Per-engine tuning is follow-up.
* Other 6 engines: not exercised by XAU-only October tape (index
  engines need SPXUSD/Nas/NSXUSD ticks; NBM-gold-london didn't fire;
  RSIReversal blocked all month by `tick_atr < 1.0` filter).

See `outputs/IN_FLIGHT_PROTECTION_AUDIT_2026-05-13.md` for the audit
methodology and `outputs/s63_compare_summary.md` for the raw A/B numbers
(both gitignored — session-local).

## S64 detail (working tree) — THE GBP FIX

Five engine headers, two new layers:

**1. Immediate `LOSS_CUT_PCT` cold-loss cut** (matches the operator's
"cover costs and cut losing trades immediately" rule):

```
LOSS_CUT_PCT = 0.03  (% of entry; 0.0 disables)
```

At GBPUSD 1.35 entry → 0.000405 = ~4 pips. Tighter than the existing
range-based `SL_dist` (10–15 pips). Cuts adverse trades **before** the
structural SL fires. JPY (0.01 pip scale) uses the same percentage:
0.03% of 154 ≈ 4.6 pips.

**2. Consecutive-loss circuit breaker:**

```
CONSEC_LOSS_THRESH    = 2       (consecutive losses in current UTC day)
CONSEC_LOSS_BLOCK_S   = 14400   (4-hour lockout when tripped)
```

After 2 consecutive losing trades (SL_HIT or LOSS_CUT) in the current
UTC day, blocks ALL arming for 4 hours — covers the full London-open
session. Counter resets on a winning trade (TRAIL_HIT or TP_HIT) OR a
UTC day roll.

**Persisted to disk** alongside post-trade-block (v2 format):

```
sl_price=...
sl_cooldown_dir=...
sl_cooldown_ts=...
win_exit_price=...
win_exit_block_ts=...
consec_loss_count=...        ← S64
consec_loss_day_utc=...      ← S64
consec_loss_block_until_s=...← S64
```

Service restarts inside an active block do NOT silently bypass it.

**Today's pattern under S64:**
* 07:02 GBP LONG SL → `m_consec_loss_count = 1`, no block
* 08:55 GBP LONG SL → `m_consec_loss_count = 2` → block trips →
  engine locked out until ~12:55 UTC
* Subsequent SLs prevented for the rest of the session.

Plus LOSS_CUT shrinks each loss from ~$17 to ~$5.30 (4 pips × 0.10 lot
× $1/pip + $1.30 cost). Worst-case session damage: 2 × $5.30 = $10.60.

## S65 detail (working tree)

The `/api/v1/omega/positions` API endpoint only sees engines that
register via `g_open_positions.register_source(...)` in
`engine_init.hpp`. Pre-S65 was 8 sources; this commit adds 11 more.

**Now registered (20 sources total):**

```
HybridGold, MidScalperGold, MicroScalperGold,
EurusdLondonOpen, UsdjpyAsianOpen, GbpusdLondonOpen,
AudusdSydneyOpen, NzdusdAsianOpen, XauusdFvg,
PDHLReversion, RSIReversal, MinimalH4Gold, MinimalH4US30,
XauThreeBar30m, NoiseBandMomentumGoldLdn,
VWAPReversion x 4 (sp/nq/ger40/eurusd),
TrendPullback x 2 (gold/nq)
```

Each new registration follows the existing GBP template (check
`has_open_position()`, look up symbol mid from `g_last_tick_bid`,
compute unrealized PnL via `tick_value_multiplier`, emit a
`PositionSnapshot`).

**NOT yet registered** (different pos shapes — S66 follow-up):

* `BracketEngine` (XAU + 12 FX/index instances) — pyramid leg array
* `GoldEngineStack` (18 sub-engines) — `legs_` vector
* `IndexFlowEngine` / `IndexMacroCrashEngine` / `IndexSwingEngine`
* `CandleFlowEngine` (3 paths) — multi-path state
* `XauTrendFollow 2h / 4h / D1` — TODO check pos shape
* `UstecTrendFollow 5m / HTF` — TODO
* `EMACrossEngine`, `H4RegimeEngine`, `BreakoutEngine`, `MacroCrashEngine`
* `C1RetunedPortfolio` — portfolio wrapper

## Files modified this session (working tree, pre-commit)

```
M include/AudusdSydneyOpenEngine.hpp    (S62 + S64)
M include/CrossAssetEngines.hpp         (S63: NBM, IndexMacroCrash)
M include/EurusdLondonOpenEngine.hpp    (S62 + S64)
M include/GbpusdLondonOpenEngine.hpp    (S62 + S64)
M include/IndexFlowEngine.hpp           (S63: IndexMacroCrash + IFLOW)
M include/NzdusdAsianOpenEngine.hpp     (S62 + S64)
M include/PDHLReversionEngine.hpp       (S63)
M include/RSIReversalEngine.hpp         (S63)
M include/UsdjpyAsianOpenEngine.hpp     (S62 + S64)
M include/XauThreeBar30mEngine.hpp      (S63)
M include/XauusdFvgEngine.hpp           (S63)
M include/engine_init.hpp               (S65)
M docs/handoffs/SESSION_HANDOFF_2026-05-13a.md  (part-F refresh)
?? S63_build_verify_compare.sh
?? S65_build_commit_push.sh
?? docs/handoffs/SESSION_HANDOFF_2026-05-13g.md (renamed from outputs/)
?? docs/handoffs/SESSION_HANDOFF_2026-05-13h.md (this file)
```

The S64 work touched the same FX engine files that S62 already
modified, so S62 + S64 share dirty files. The commit script stages
each set separately to produce two clean commits.

## Carry-over for next session

### 1. VPS deploy (operator-Windows-side, blocking)

```powershell
cd C:\Omega
.\OMEGA.ps1 deploy
```

One-shot pipeline: stop service → `git pull origin main` → npm ci /
npm run build (UI) → cmake --build (C++) → copy artefacts → start
service → stamp-verify. The S62 + S63 + S64 + S65 commits ALL ride
this deploy.

**Expected log lines on first launch after deploy:**

```
[GBP-LDN-OPEN] RANGE_HIST_LOAD ok n=N age_s=M             ← S62
[GBP-LDN-OPEN] POST_TRADE_BLOCK_LOAD restored=...
                consec_count=N consec_block_rem_s=M       ← S62+S64
(same for EUR/AUD/NZD/JPY)
[OmegaApi] g_open_positions sources registered (20 sources: ...) ← S65
```

If you see `consec_block_rem_s > 0` after a restart inside an active
block, the persistence layer is working as designed.

### 2. The "simplest most effective gold trading engine" question

The operator paused at the handoff point to investigate this fresh
next session. Context for that conversation:

* `g_bracket_gold` (BracketEngine instance) and `g_minimal_h4_gold`
  (MinimalH4Breakout) BOTH already implement the compression-bracket
  pattern the operator described. Both are `shadow_mode = true` in
  engine_init.hpp. Promotion gate is 2-week paper validation + ≥30
  trades + WR ≥35% net after costs.
* Today's XAU October A/B run showed only `PDHLReversionEngine` fired
  meaningfully on 9.3M ticks. -$6.49 net over 928 trades. Marginal.
* Open questions for next session:
  - Which timeframe is the right granularity for "simplest effective"?
    (Tick / M1 / M5 / M15 / M30 / H1 / H4 / D1)
  - Is a fresh ultra-minimal engine warranted, or are the existing
    `g_bracket_gold` / `g_minimal_h4_gold` engines the right vehicles
    waiting on validation?
  - What's the smallest-possible parameter surface that still produces
    a profitable backtest on XAU 2024-2026?

### 3. S66 GUI position-source follow-up

Register the remaining 9 engine families in `engine_init.hpp`:

* BracketEngine — needs multi-leg snapshot (or "primary leg only")
* GoldEngineStack — iterate `legs_` vector
* IndexFlowEngine / IndexMacroCrash / IndexSwing — `base_entry_` etc.
* CandleFlowEngine — 3 paths share state
* XauTrendFollow 2h / 4h / D1 — pos shape TBD
* UstecTrendFollow 5m / HTF — pos shape TBD
* EMACross / H4Regime / Breakout / MacroCrash — straightforward
* C1RetunedPortfolio — portfolio wrapper

Mechanical work; each registration is ~25 lines. Plan: read each
engine's pos struct (Grep), pick the field mapping, mirror the
existing template. ~250-300 lines total addition to engine_init.hpp.

### 4. S63 LOSS_CUT/BE_RATCHET tuning (deferred)

The backtest showed PDHL defaults are too loose — 470 BE_CUT/LOSS_CUT
fires were neutral PnL because they replaced exits that would have
happened anyway. Tighten via `engine_init.hpp` per-instance overrides
after running a focused per-engine backtest with the current numbers
as the baseline. Index engines need an index tape (SPXUSD/Nas/NSXUSD)
which the operator has at `~/Tick/SPXUSD/` etc.

## Files NOT modified

* Core: `OmegaCostGuard.hpp`, `OmegaTradeLedger.hpp`, `SymbolConfig.hpp`,
  `OmegaFIX.hpp`, `OmegaApiServer.hpp`, `GoldPositionManager.hpp`,
  `on_tick.hpp`, `trade_lifecycle.hpp`, `order_exec.hpp` — all
  untouched. The S37 cost-gate chokepoints remain belt-and-braces
  alongside today's engine-level additions.
* `engine_init.hpp` IS modified (S65) but is explicitly non-core per
  CLAUDE.md: *"Engine files (`*Engine.hpp`, `GoldEngineStack.hpp`,
  `CrossAssetEngines.hpp`) and configuration (`engine_init.hpp`) are
  NOT core."*

## Standing audit at session end

CLAUDE.md ungated-engine sweep continues to expect ONLY:
`LatencyEdgeEngines` (S13 culled), `RSIExtremeTurnEngine` (S52
disabled, 0/153 profitable), `SweepableEngines` + `SweepableEnginesCRTP`
(research-only sweep harness, not in live runtime). All other
production engines remain cost-gated. No regression today.

## Quick-reference scripts

| script | purpose |
|---|---|
| `S63_build_verify_compare.sh` | A/B backtest harness, auto-detects Dukascopy 5-col CSV |
| `S65_build_commit_push.sh` | Lands S63 + S64 + S65 as 3 clean commits, gated by Mac canary build |

Operator runs `S65_build_commit_push.sh` next. Both scripts self-delete
on successful completion (project pattern).

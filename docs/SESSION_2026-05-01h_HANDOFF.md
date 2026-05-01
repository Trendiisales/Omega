# SESSION 2026-05-01h HANDOFF

## TL;DR

Two big results this session:

1. **Found the silent regression that explained "zero gold trades all day".**
   `GoldHybridBracketEngine::STRUCTURE_LOOKBACK` was clobbered from 120 → 20
   in commit `6c85c1b` (2026-04-07 "Wire DOM into
   GoldHybridBracketEngine"), a 619-line refactor whose commit message did
   not mention the lookback change. The mismatch between
   `STRUCTURE_LOOKBACK=20` and `MIN_RANGE=6.0` (which had been raised the
   day before in commit `04ae0f9` *because* the lookback was 120) made the
   engine require $6 of XAUUSD price travel within ~6 seconds — only news
   spikes qualify. Restored.
2. **Drafted a new sister engine `GoldMidScalperEngine`** purpose-built for
   the $20-40 capture zone the user wants. Wired into globals / engine_init
   / tick_gold dispatch, shadow-only on first deploy.

Plus documented one **carry-over blocker**: `.git/index.lock` is a stale
0-byte file in the host-mounted repo that the sandbox cannot unlink. User
must remove from Mac terminal before commits go through.

## What I changed (5 files modified, 1 file created, 2 files newly tracked)

### Tier 1 — REGRESSION FIX

`include/GoldHybridBracketEngine.hpp:101`

```
static constexpr int    STRUCTURE_LOOKBACK   = 120;   // restored from 20
```

Plus a 22-line comment block above it documenting the regression history so
this can't get clobbered silently again. The `static_assert` in any future
test file will catch it: `STRUCTURE_LOOKBACK == 120`.

### Tier 2 — DELIBERATELY SKIPPED

Originally proposed: drop `g_bracket_gold.ATR_RANGE_K` 2.0 → 1.0 to make
GoldBracket fire more in low-vol. **Skipped because:**

- Inspection of `engine_init.hpp:277` shows `g_bracket_gold.ATR_RANGE_K = 0.0`
  is *already disabled* for gold (uses fixed `MIN_RANGE = 2.5pt` floor).
  The Tier 2 plan was based on the configure() default values; the actual
  override is more permissive than what I planned to change to.
- Every other bracket constant carries audit lineage — S20 (MAX_HOLD),
  S21 (CONFIRM_PTS/SECS), S22c (MAX_SL_DIST_PTS=6.0 — eliminated 36/36
  wide-SL losers), S13 (REGIME_FLIP). Touching any of them risks unwinding
  validated improvements. Per Jo's standing rule "Never modify core code
  unless instructed clearly", I did not.
- The bracket's silence on today's tape may be a regime mismatch (slow
  trending, no clean compressions for the 600-tick window) rather than a
  bug. Worth observability work next session before any tuning.

### Tier 3 — NEW ENGINE for $20-40 capture

**New file: `include/GoldMidScalperEngine.hpp`** (470 lines, near-clone of
GoldHybridBracketEngine with tuned constants).

Constants:

| Parameter             | HybridGold | MidScalper | Reason |
|-----------------------|-----------:|-----------:|--------|
| `STRUCTURE_LOOKBACK`  |        120 |        300 | ~90 sec window finds $8+ compressions |
| `MIN_ENTRY_TICKS`     |         15 |         30 | Larger warmup |
| `MIN_BREAK_TICKS`     |          3 |          5 | Sweep guard, matches GoldBracket S22c |
| `MIN_RANGE`           |       $6.0 |       $8.0 | Bottom of TP $18 zone |
| `MAX_RANGE`           |      $25.0 |      $20.0 | Top of TP $42 zone |
| `TP_RR`               |        2.0 |        4.0 | Asymmetric reward for slower fires |
| `PENDING_TIMEOUT_S`   |         30 |        120 | Wider compressions need longer waits |
| `COOLDOWN_S`          |         60 |        180 | Avoid double-firing same structure |
| `DIR_SL_COOLDOWN_S`   |        120 |        240 | Same |

TP envelope: range $8 → TP $18; range $10 → TP $22; range $15 → TP $32;
range $20 → TP $42. Right in the $20-40 sweet spot.

Inherits all audit-validated guards: S20 trail-arm guards, S43 mae tracker,
S47 T4a ATR-expansion gate (with 2026-04-29 ratchet fix), S51 1A.1.a
spread_at_entry, S52 MFE_TRAIL_FRAC=0.40, AUDIT 2026-04-29 close-path
mutex, audit-fixes-18 SpreadRegimeGate.

Log namespace: `[MID-SCALPER-GOLD]` and `[MID-SCALPER-GOLD-DIAG]`.
`tr.engine = "MidScalperGold"`. `tr.regime = "MID_COMPRESSION"`.

**Wiring (4 file edits):**

- `include/globals.hpp` — added `#include "GoldMidScalperEngine.hpp"` and
  `static omega::GoldMidScalperEngine g_gold_midscalper;`.
- `include/engine_init.hpp` — added explicit `g_gold_midscalper.shadow_mode = true;`
  (pinned regardless of g_cfg.mode), `g_engines.register_engine("MidScalperGold", ...)`,
  and `g_open_positions.register_source("MidScalperGold", ...)`.
- `include/tick_gold.hpp` — added a new dispatch block right after the
  HybridGold block. Calls `g_gold_midscalper.on_tick(...)` on every XAUUSD
  tick (FIX 2026-04-07 pattern: feeds window even when can_enter=false).
  Locally re-derives `ms_vol_ok` because `hybrid_vol_ok` from the
  HybridGold block is out of scope. Does NOT add the engine to
  `gold_any_open` so it runs alongside other engines while shadow-only;
  one-line addition required there once promoted to live.

No FIX-order send block — shadow mode means PENDING auto-fills when price
crosses bracket boundaries (handled inside on_tick).

## Verification

- `g++ -fsyntax-only -std=c++20 -DOMEGA_BACKTEST=1` on a probe TU including
  both `GoldHybridBracketEngine.hpp` and `GoldMidScalperEngine.hpp` with
  static_asserts: **clean**, asserts confirm `STRUCTURE_LOOKBACK == 120`
  for HBG and `STRUCTURE_LOOKBACK == 300, MIN_RANGE == 8.0,
  MAX_RANGE == 20.0, TP_RR == 4.0` for MidScalper.
- Brace balance check on all 5 modified/created files: all balance.
- `tick_gold.hpp` and `engine_init.hpp` cannot be syntax-checked in
  isolation (single-TU pattern, depends on Windows headers and
  globals.hpp). The dispatch block I added is a literal copy of the
  existing HybridGold pattern (lines 2076-2137) with renamed variables;
  the registration block follows the existing HybridGold pattern at lines
  1815 and 1894. MSVC will catch anything I missed at the next rebuild.

## ⚠ BLOCKER for commit — `.git/index.lock`

When attempting `git add` in the sandbox:

```
fatal: Unable to create '...omega_repo/.git/index.lock': File exists.
```

The lock file is 0 bytes and owned by the sandbox UID, but the sandbox
cannot `rm` it (Operation not permitted from the host mount). The handoff
of session g predicted this exact issue.

## RECOVERY — first thing next session

```bash
cd ~/omega_repo

# Clear the stale lock from Mac (sandbox couldn't)
rm -f .git/index.lock

# Confirm the 4 modified + 1 new file appear, plus the 2 untracked handoff docs
git status

# Stage the engine work + the new handoff docs (f, g, h)
git add \
    include/GoldHybridBracketEngine.hpp \
    include/engine_init.hpp \
    include/globals.hpp \
    include/tick_gold.hpp \
    include/GoldMidScalperEngine.hpp \
    docs/SESSION_2026-05-01f_HANDOFF.md \
    docs/SESSION_2026-05-01g_HANDOFF.md \
    docs/SESSION_2026-05-01h_HANDOFF.md

git status
git commit -m "Gold engine fix + MidScalper

Tier 1: restore HybridGold STRUCTURE_LOOKBACK 20->120 (silent
regression in commit 6c85c1b 2026-04-07 'Wire DOM into
GoldHybridBracketEngine'; explanation in header comment block).

Tier 3: add GoldMidScalperEngine (sister to HybridGold) tuned for
the \$20-40 capture zone (range \$8-20 with TP_RR=4 -> TP \$18-42).
Shadow-only on first deploy. Wired in globals.hpp + engine_init.hpp
(register_engine + register_source) + tick_gold.hpp (dispatch block
parallel to HybridGold).

Plus session handoff docs f, g, h."

git push origin omega-terminal
```

Then VPS:

```powershell
cd C:\Omega
.\QUICK_RESTART.ps1 -Branch omega-terminal
```

## What you should see immediately on the new build

Once VPS redeploys, in `latest.log`:

```
[OmegaApi] g_engines registered (15 engines)        <- was 14, now 15 (added MidScalperGold)
[OmegaApi] g_open_positions sources registered (2 sources: HybridGold, MidScalperGold)
```

Within ~30 ticks after restart, both engines should emit DIAG lines:

```
[HYBRID-GOLD-DIAG] ticks=N phase=0 window=N/120 range=X.XX spread=X.XX
[MID-SCALPER-GOLD-DIAG] ticks=N phase=0 window=N/300 range=X.XX spread=X.XX
```

Critical confirmation queries:

```powershell
# 1. HybridGold lookback restored — look for /120 not /20 in window
Select-String -Path C:\Omega\logs\latest.log -Pattern "HYBRID-GOLD-DIAG.*window=\d+/120" | Select-Object -Last 5

# 2. MidScalper alive
Select-String -Path C:\Omega\logs\latest.log -Pattern "MID-SCALPER-GOLD-DIAG" | Select-Object -Last 10

# 3. Either engine ARMED?
Select-String -Path C:\Omega\logs\latest.log -Pattern "HYBRID-GOLD\] ARMED|MID-SCALPER-GOLD\] ARMED" | Select-Object -Last 10

# 4. Either engine FIRED?
Select-String -Path C:\Omega\logs\latest.log -Pattern "HYBRID-GOLD\] FIRE|MID-SCALPER-GOLD\] FIRE" | Select-Object -Last 10

# 5. Either engine recorded a trade?
Select-String -Path C:\Omega\logs\latest.log -Pattern "HYBRID-GOLD\] EXIT|MID-SCALPER-GOLD\] EXIT" | Select-Object -Last 10
```

In the GUI: `LDG HybridBracketGold` for HBG trades, `LDG MidScalperGold`
for MidScalper trades. (The engine string in the ledger is
`HybridBracketGold` not `HybridGold` — see SESSION_g handoff line 5.)

## Engine audit summary (for the record)

Audited every gold engine for the same silent-regression pattern as HBG.
Only HBG had it. Other findings:

| Engine | Status |
|---|---|
| `g_hybrid_gold` | **REGRESSED — fixed this session** |
| `g_bracket_gold` | OK; configured externally with explicit values; conservative by design (S20/S21/S22c/S13 audits) |
| `g_gold_stack` (multi-strategy: AsianRange, OpeningRange, VWAPStretch etc.) | OK; reasonable per-strategy thresholds |
| `g_trend_pb_gold` | OK; ATR-floored at 5pt arm / 3pt trail |
| `g_nbm_gold_london` | OK; session-windowed 07:00-13:30 UTC |
| `g_h1_swing_gold` / `g_h4_regime_gold` | OK; bar-based, session_max=6/5 by design |
| `g_macro_crash` | EXPLICITLY DISABLED (`enabled=false` engine_init.hpp:155) — 4-week audit −10,849pt bleed |
| `g_candle_flow` | OK; SHADOW only, heavy audit-fixes pass on Apr 29 |
| `g_ema_cross` | SOFT-CULLED per S18 audit |
| `g_rsi_reversal` / `g_rsi_extreme` | OK; bar-ATR cost-gated, blocked today by quiet tape (`[RSI-EXT-BLOCK] cost_gate: atr=1.28 < 1.50x` confirms working as designed) |
| `g_hybrid_sp` / `nq` / `us30` / `nas100` | OK; per-instance config (`structure_lookback=180`), cannot silently regress like HBG (which used `static constexpr`) |

## Diagnostic finding — bracket engine `range=0.00` in DIAG is misleading

`[GOLD-BRK-DIAG]` shows `range=0.00 brk_hi=0.00 brk_lo=0.00` always when
phase=IDLE. This is the **post-arm locked range** (`m_locked_hi -
m_locked_lo`), which is 0 until ARM. It does NOT show the live computed
600-tick window range. So we cannot tell from the existing diag whether
the bracket's structure window is hitting `eff_min_range`.

**Optional follow-up (next session):** add `live_range=$X.XX
inside_ticks=N` fields to the bracket DIAG line in `BracketEngine.hpp`.
Pure additive observability, no behaviour change. Would unblock root-cause
investigation of "why bracket isn't firing despite can_arm=1".

## Promotion gate for MidScalper (live)

Engine ships shadow-only. Before flipping to live:

1. **2 weeks of paper data** captured in `g_omegaLedger` filtered by
   `engine = "MidScalperGold"`.
2. **Positive expectancy** in the $20-40 zone — at least 30 trades, WR ≥ 35%
   (with TP_RR=4, breakeven WR is 25%), net positive after costs.
3. **No SL clusters** — no day with 3+ consecutive SL_HIT (would suggest
   direction-detection failure rather than valid compressions).
4. **One-line live promotion**: change `engine_init.hpp` from
   `g_gold_midscalper.shadow_mode = true;` to
   `g_gold_midscalper.shadow_mode = kShadowDefault;` AND add
   `|| g_gold_midscalper.has_open_position()` to the `gold_any_open`
   chain in `tick_gold.hpp:36-46` to enforce 1-at-a-time on live path.

## Session g recovery commit — already shipped

The 3 files mentioned in session g handoff (App.tsx, HomePanel.tsx,
MarketDataProxy.cpp) were already committed before this session began,
in commits `9d80831` (HomePanel) and `f084d63` (App + MarketDataProxy).
No carry-over from session g recovery — only the engine work this
session and the persistent `dist-step7-*/` cleanup which still needs
`sudo rm -rf` from Mac.

## Carry-over from earlier sessions (still open)

1. `QUICK_RESTART.ps1` stderr-mangling on npm blocks (~lines 627/636).
2. `QUICK_RESTART.ps1` default `-Branch="main"` — still need flip to
   `omega-terminal` or merge.
3. Banner `:7779` line still hardcoded; terminal UI is on `:7781`.
4. `OMEGA_FRED_KEY` env var unset on VPS service (CURV panel returns 503).
5. `omega-terminal/dist-step7-*/` and `dist-verify/` directories — need
   `sudo rm -rf` from Mac terminal (sandbox cannot unlink).
6. **NEW:** `.git/index.lock` is a stale 0-byte lock file the sandbox
   cannot unlink. Must `rm -f .git/index.lock` from Mac terminal at the
   start of every session that involves git operations.

## Open task list for next session

1. **First thing:** `rm .git/index.lock` then run the staged commit + push
   command block above.
2. **VPS redeploy via QUICK_RESTART.ps1 -Branch omega-terminal** and run
   the 5 verification queries above.
3. **Confirm HybridGold is now firing** trades into the ledger (was zero
   all session before this fix).
4. **Monitor MidScalper diag** for the first 30 minutes to confirm window
   fills (should hit window=300/300 within ~90 sec) and start emitting
   ARMED / FIRE lines once a $8-20 compression appears.
5. **If neither engine fires after 30 min on a trading session,** the next
   investigation is bracket DIAG observability (add `live_range` /
   `inside_ticks` to the bracket DIAG so we can see whether the 600-tick
   window is actually hitting `eff_min_range=2.5`).
6. **Optional:** S52 trail-quality follow-up; bracket post-restart
   blackout fix (persist structure window); HelpPanel rewrite; tab labels
   showing args; favicon + title.

## Lessons

1. **Audit large refactor commits for silent constant regressions.** The
   `6c85c1b` commit changed STRUCTURE_LOOKBACK 120→20 inside a 619-line
   "Wire DOM into …" rewrite without mentioning it. The validated value
   from the prior day's commit was lost. Linting rule: any commit that
   touches `static constexpr` values without mentioning them in the
   commit message should fail review.
2. **Don't trust commit titles when reading diffs.** "Wire DOM" and
   "route logging via std::cout" are both common refactor patterns that
   tend to also touch unrelated lines incidentally. The audit pass this
   session checked every other engine's `route logging` and DOM-wire
   commits — none of the others had similar regressions. But the pattern
   is real and worth a repo-wide sweep at some point.
3. **`static constexpr` parameters are vulnerable to silent regression
   in clones/refactors.** The fact that `HybridGold` used class constants
   (clobberable) while `IndexHybridBracketEngine` uses per-instance
   config struct values (each instrument's value explicit in
   `engine_init.hpp`) made HBG uniquely exposed to this exact bug.
   Worth considering a uniform pattern (header-default + engine_init
   override) for all per-engine tunables in the future.

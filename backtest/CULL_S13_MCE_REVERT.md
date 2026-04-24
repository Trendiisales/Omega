# Session 13 Finding A — MacroCrashEngine Threshold Revert

## Status
**ACCEPTED.** `MacroCrashEngine` base thresholds reverted to class-default
(London/NY) values: `ATR_THRESHOLD` 4.0→6.0, `VOL_RATIO_MIN` 2.0→2.5,
`DRIFT_MIN` 3.0→5.0. Asia coverage is preserved by the pre-existing
session-aware logic inside the engine class.

Repo HEAD at change: `3955e125` (post S13 orphan cull).
Change trigger: Session 13 engine audit + user instruction "look at
fixing the macro engine because we know it was working when we had
these huge spikes and drops".

## Why the engine was broken

`include/engine_init.hpp` lines 120–122 (before this change) set:
```cpp
g_macro_crash.ATR_THRESHOLD   = 4.0;   // lowered 8.0->4.0: "covers Asia spikes"
g_macro_crash.VOL_RATIO_MIN   = 2.0;   // lowered 2.5->2.0
g_macro_crash.DRIFT_MIN       = 3.0;   // lowered 6.0->3.0
```

Author's stated reasoning (in the inline comments): lower the BASE
thresholds so Asia sessions pass, on the assumption that London/NY
would naturally clear the lower bar.

The assumption was **wrong**.

`include/MacroCrashEngine.hpp` already had session-aware thresholds
built into the engine class:
```cpp
double ATR_THRESHOLD    = 6.0;    // class default: London/NY
double VOL_RATIO_MIN    = 2.5;
double DRIFT_MIN        = 5.0;

double ATR_THRESHOLD_ASIA  = 4.0; // class default: Asia override
double VOL_RATIO_MIN_ASIA  = 2.0;
double DRIFT_MIN_ASIA      = 3.0;
```

And at entry the engine switches on `session_slot`:
```cpp
const bool is_asia = (session_slot == 6);
const double eff_atr_threshold = is_asia ? ATR_THRESHOLD_ASIA : ATR_THRESHOLD;
```

By overriding the **base** threshold to 4.0 in engine_init, the base
no longer differs from the Asia override. Result: London/NY sessions
used Asia-grade gates, firing on small wobbles that were not real
macro events.

## Evidence

**S17 live audit (8 days, 2026-04-14 to 2026-04-23):**
- MacroCrash: 8 trades, 12.5% WR, **-$35.81 total (-$4.48/trade)**
- Zero positive sessions.

**Engine design proof point (class docstring):**
> PROOF (Apr 2 crash, 207pt move):
>   Single entry actual:                      $292
>   + Velocity trail:                         $5,012 (two-day)
>   + Pyramid (3 adds, cost-covered):         $4,270 on single trade alone
>   Worst case with pyramid:                  -$160

Apr 2 tariff crash: ATR hit ~15pt, drift hit ~20pt. Original gates
(ATR=6 / drift=5 / vol=2.5) cleared comfortably. The engine's designed
edge is **real macro events**, not Asia chop.

## The fix

Restore the base thresholds to class-default values:
```cpp
g_macro_crash.ATR_THRESHOLD   = 6.0;   // reverted 4.0->6.0
g_macro_crash.VOL_RATIO_MIN   = 2.5;   // reverted 2.0->2.5
g_macro_crash.DRIFT_MIN       = 5.0;   // reverted 3.0->5.0
```

Asia coverage is preserved by the engine's internal
`is_asia ? ATR_THRESHOLD_ASIA : ATR_THRESHOLD` switch, using the
already-set `ATR_THRESHOLD_ASIA=4.0` / `VOL_RATIO_MIN_ASIA=2.0` /
`DRIFT_MIN_ASIA=3.0` class defaults.

**Unchanged:** `BASE_RISK_USD=80.0`, `STEP1_TRIGGER_USD=80.0`,
`STEP2_TRIGGER_USD=160.0`. The STEP1/STEP2 relaxations (200→80,
400→160) are correct — they reflect the 0.01-lot SHADOW size (small
dollar exposure) and are independent of session volatility.

**Shadow mode:** unchanged (`shadow_mode=true`). Engine must prove
the revert works in shadow before any `shadow_mode=false` flip.

## Expected behaviour after revert

- **London/NY sessions:** engine requires ATR>6, drift>5, vol_ratio>2.5
  — the bar that historically caught Apr 2 crash and other macro events.
  Small wobbles (ATR=4-5) that drove the 12.5% WR will no longer fire.
- **Asia sessions:** engine uses ATR>4, drift>3, vol_ratio>2.0 — the
  previously-intended Asia coverage — via the class's internal switch.

Expected outcome over the next shadow window: fewer trades, higher WR,
mean revert toward the class's original 69% WR design target.

## Risk assessment

**Low risk.**
1. Change is a 3-line config revert — restores documented class-default
   values that were validated against the Apr 2 crash data.
2. Engine is in `shadow_mode=true` — no live order exposure.
3. Asia session gating still works via the unchanged
   `ATR_THRESHOLD_ASIA` / `DRIFT_MIN_ASIA` / `VOL_RATIO_MIN_ASIA`
   class-member values.
4. Fix cannot break any other engine — `g_macro_crash` is self-contained.

## Verification post-merge

After `QUICK_RESTART.ps1`:
- MCE startup log should read:
  `[MCE] MacroCrashEngine ARMED (shadow_mode=true) ATR>6 vol>2.5x drift>5`
- Shadow fires should drop substantially in London/NY quiet periods.
- Next genuine macro event (NFP, FOMC, unexpected news) should still
  fire the engine — those moves produce ATR>10 which clears any gate.

## Follow-up

After 10+ shadow fires accumulate post-revert:
1. Audit WR / per-trade P&L.
2. If ≥60% WR and net-positive, candidate for `shadow_mode=false` flip.
3. If still negative, deeper investigation of exit logic (trail / pyramid).

---

*Document generated 2026-04-24, Session 13 Stage 2, Claude.*
*Commits with this revert + Finding B LatencyEdge cull together.*

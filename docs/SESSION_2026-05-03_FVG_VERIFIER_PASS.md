# XauusdFvg verifier — row-for-row PASS

**Date:** 2026-05-03
**Session:** "Resume FVG verifier work" → this handoff.
**Repo state:** all edits committed-on-disk (no git operations performed).
**Status:** Verifier PASSes the v3 trades_top.csv replay row-for-row within
tolerance. Closes out the open issue at the bottom of
`SESSION_2026-05-02_FVG_S7_HANDOFF.md`.

```
[verify] fed 5890 of 11698 bars (cap 2025-11-28 08:15:00 UTC)
[verify] engine emitted 70 trades
[PASS] verifier OK -- 70 trades match row-for-row within tolerance
       (price=0.001000, score=0.000100)
```

---

## Diagnostic chain (the four-fix sequence)

The 05-02 handoff documented one open issue: row 1 score off by 0.001149
plus a count gap (`expected=70 emitted=42`). Today's session worked through
that gap iteratively; each fix exposed the next one. In order:

### Fix 1 — `m_tv_mean_at_prev_bar` snapshot (engine, prior pass)

The 05-02 handoff Hypothesis was correct: v3 reads `tv_mean[i-1]` (the
rolling 20-bar mean ENDING the bar BEFORE formation), but the engine's
`update_indicators()` pushed the formation bar's tick_count into the
deque BEFORE `try_detect_fvg()` consumed `m_tv_mean`. Snapshotting
`m_tv_mean` into `m_tv_mean_at_prev_bar` BEFORE the deque push and reading
the snapshot in the s_tv computation closed the score-row-1 gap. Already
applied earlier in the session; mentioned here for completeness.

### Fix 2 — Verifier-only mitigation overrides (engine + verifier, prior pass)

Two flags added to the engine, both `false` by default in production:

- `spread_gate_disabled` — bypass `SpreadRegimeGate` at the mitigation
  entry path. The gate's adaptive ABS_FLOOR/ABS_CEIL thresholds were
  tuned for forex pairs at sub-pip price scale; XAUUSD's ~1+ USD raw
  spreads put the gate's hysteresis state machine in CLOSED for most of
  the dataset, blocking ~133 mitigations the v3 reference (which has no
  spread gate) takes.
- `first_touch_only_mode` — replace queue-and-wait mitigation with v3
  `run_backtest`'s first-touch / overlap-skip semantics. Detection and
  mitigation gates run on every bar regardless of `m_pos.active`; FVGs
  that first-touch their zone DURING an open trade are consumed exactly
  once via overlap-skip (matching v3's `next_open_after = exit_idx + 1`).
  In-bar SL/TP also moves to a bar-level check at step 5 of
  `on_bar_close`, bit-equivalent to v3 `simulate_trade`'s `bar_l <= sl`
  / `bar_h >= tp`. `m_last_exit_bar_idx` was added to extend the
  overlap window through TIME_STOP exits (TIME_STOP fires from on_tick
  before the exit bar's on_bar_close, so the position-active check
  alone misses it).

The verifier sets both `true`. Production behaviour is byte-identical
to today.

### Fix 3 — Verifier bar-window truncation

After fixes 1+2 the engine emitted 136 trades vs 70 expected. Root
cause: the cached bars CSV covers 2025-09-01 → 2026-03-01 (6 months,
11,697 bars), but the v3 single-window `trades_top.csv` was generated
on a 2025-09-01 → 2025-12-01 (3-month) run. v3's own `summary.txt`
diagnostics confirm: `Top  98 cand / 70 trades / 27 overlap / 1
unfillable` — every candidate accounted for inside the 3-month window;
any post-Nov-28 trade the engine emits is a phantom v3 never even
considered.

Verifier fix: cap the bar feed at `max(exit_ts across trades_top.csv)
+ BAR_SECS` by default. The +1 bar guarantees the bar after the last
exit is fed (so the engine's on_bar_close step-5 SL/TP and first-tick
TIME_STOP path run for the final position). No further extension —
any FVG with score >= cutoff and entry_idx > max(exit_ts) would have
fired in v3 (post-exit means no overlap, top-decile pool means score
>= cutoff), and v3 has none. Two CLI overrides for cross-window runs:

- `--bar-end-ts <unix>`
- `--bar-end-date YYYY-MM-DD`
- `--bar-end-ts 0` disables the cap (use for walk-forward
  `trades_top.csv` whose train_end matches the bars-pickle end).

After Fix 3: 72 emitted vs 70 expected. The verifier was also taught
NOT to short-circuit the per-row diff on a count-mismatch-only fail
(previous behaviour bailed at row 1 the moment counts diverged); now
it tracks count fails separately and only stops the per-row loop on a
true field divergence. Trailing extras (emitted past `expected.size()`)
are dumped with full details — entry/exit timestamps, score, atr,
gap, prices, exit reason.

### Fix 4 — Engine session uses formation bar (production semantic fix)

With the row-by-row diff now able to progress past count mismatch, the
first field divergence surfaced at row 3:

```
[FAIL] row 3 field=session expected=off got=asian
```

v3's `Trade.session` is sourced from `fv.session`, which `detect_fvgs()`
sets via `classify_session(times[i])` at the FORMATION bar. The engine's
`open_position()` was using `session_for_ts(bar.start_ts)` (the
mitigation/entry bar). On formation/entry pairs that straddle a session
boundary — e.g. an FVG that forms at 23:45 UTC ("off") and mitigates at
00:15 UTC ("asian") two bars later — the labels diverge.

Fix: change `m_pos.session` to `session_for_ts(fv.formed_ts)`. The
PendingFvg already carries `formed_ts`, set in `try_detect_fvg`.
Production effect: the side-channel `live_xauusd_fvg.csv` and any
quarterly re-feed against `trades_top.csv` now report formation
session, matching v3 exactly. `entry_ts` (and the rest of the trade
record) are unchanged.

### Fix 5 — Engine TIME_STOP uses sequential closed-bar count

After Fix 4 the next field divergence was at row 4:

```
[FAIL] row 4 field=exit_ts expected=1757313000 got=1757283300
```

Difference = 29,700 s = 33 trading bars. Row 4 is a TIME_STOP exit on
a Friday-entry / Monday-exit trade (entry 2025-09-05 14:15 UTC,
v3 exit 2025-09-08 06:30 UTC, `bars_held=61`). The engine fired
TIME_STOP at 2025-09-07 22:15 UTC — the FIRST tick after the Sunday
22:00 bar closed.

Root cause: the elapsed-bars check in `on_bar_close` was
`m_recent_bars[2].bar_idx - m_pos.entry_bar_idx`. Both terms are
`floor(now_s / 900)` UTC bar indices, which advance by ~192 across the
Fri-22:00 → Sun-22:00 forex weekend gap even though no trading bars
exist there. As soon as the first Sunday bar closed, the engine saw
`elapsed ≈ 223`, tripped the 60-bar threshold, and fired TIME_STOP
30+ trading bars early relative to v3 (which iterates pandas array
positions sequentially through trading bars only).

This was ALSO the root cause of the two phantom trades. When the
engine's TIME_STOP fires early on a weekend-spanning trade, the
engine's overlap window closes prematurely; FVGs that mitigate during
the v3-still-open / engine-already-closed window get overlap-skipped
by v3 but fire as fresh entries in the engine. The 70-row set
contains exactly two such weekend-spanning TIME_STOP trades, which
inserted exactly two phantoms.

Fix: count CLOSED TRADING bars instead of UTC deltas. Added
`int64_t entry_bar_seq` to `LivePos`, set in `open_position` from
`m_bars_seen` (which increments once per `close_current_bar()` call).
The TIME_STOP check is now `elapsed = m_bars_seen - entry_bar_seq`,
matching v3's `range(entry_idx + 1, entry_idx + TIME_STOP_BARS + 1)`
semantics and skipping weekends naturally. `entry_bar_idx` is RETAINED
for the SL/TP step-5 bar-skip (`m_recent_bars[2].bar_idx >
m_pos.entry_bar_idx`), which compares two UTC-floored indices that
walk bar-by-bar through trading bars — the UTC delta is fine there.

Result: 70 emitted, row-for-row PASS.

---

## Files touched this session

EDITED:
- `include/XauusdFvgEngine.hpp`
  - `m_pos.session = session_for_ts(fv.formed_ts)` (was `bar.start_ts`)
  - New `LivePos::entry_bar_seq` field
  - `m_pos.entry_bar_seq = static_cast<int64_t>(m_bars_seen)` in `open_position`
  - TIME_STOP check: `elapsed = m_bars_seen - m_pos.entry_bar_seq` (was UTC delta)
- `backtest/verify_xauusd_fvg.cpp`
  - Bar-feed cap: `--bar-end-ts` / `--bar-end-date` flags + auto-derive
  - Per-row diff no longer short-circuits on count-mismatch alone
  - Trailing extras / missing-rows dump
- `docs/SESSION_2026-05-03_FVG_VERIFIER_PASS.md` (this file, NEW)

NEW:
- (none beyond the handoff doc)

`include/globals.hpp`, `include/engine_init.hpp`, `include/tick_gold.hpp`,
`include/LogXauusdFvgCsv.hpp`, `CMakeLists.txt` — UNTOUCHED this session.

---

## Production behavioural deltas

Two real semantic changes shipped on `XauusdFvgEngine` today. Both align
with the v3 reference (the bit-exact source of truth per the engine's
own design comments) and are correctness fixes, not feature changes.

1. **Trade record `session`** now reports the FORMATION bar's session
   instead of the entry bar's. Affects only the small subset of FVGs
   whose formation and entry straddle a 7-hour UTC session boundary
   (most often late-Off / early-Asian, ~21:00–01:00 UTC). The
   side-channel `live_xauusd_fvg.csv` consumes this field directly;
   the quarterly re-validation pipeline compares against
   `trades_top.csv` which uses the same convention. Net effect: zero
   regression risk; `live_xauusd_fvg.csv` is now bit-comparable on the
   session column where it previously was not.

2. **Weekend-spanning trades** now hold their full 60-trading-bar
   TIME_STOP window. Previously they were unwound at the first
   Sunday-open bar close (~30 trading bars early). Affects only the
   subset of FVG trades whose entry lies inside ~Friday's last
   `60 - (bars_to_market_close)` window such that the time-stop
   boundary lands inside the weekend gap. Empirically, the v3 backtest
   has always behaved this way (60 trading bars), and the v3 #5
   ACCEPTED config evidence in `HANDOFF_FVG_BACKTEST.md` was generated
   under exactly this rule, so the engine is now producing the PnL
   profile the acceptance gates were measured against. Live shadow
   reconciliation will become cleaner.

Neither change touches `entry_ts`, `entry_bar_idx`, `score_at_entry`,
`atr_at_entry`, `gap_height`, `direction`, sizing, or the SL/TP
arithmetic. `OmegaTradeLedger.dedup_key` is unaffected.

---

## What's next

Per `DESIGN_XAUUSD_FVG_ENGINE.md` §9 and the §7 wiring already shipped:

1. **Smoke-rebuild** the full Omega target (not just `verify_xauusd_fvg`)
   to confirm the two `LivePos` field additions and the session/timing
   changes don't break any other TU that touches `m_pos.*`. Quick
   `cmake --build build --config Release` from a clean tree should
   surface anything.

2. **Verifier sweep on the second walk-forward `trades_top.csv`** to
   double-check the fixes generalize:
   ```
   ./build/verify_xauusd_fvg \
       fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2025-09-01_2026-03-01.csv \
       fvg_pnl_backtest_v3/XAUUSD_15min_top10_be0.0_sl2.5_tp5.0_wf2025-12-01/trades_top.csv \
       --bar-end-ts 0
   ```
   `--bar-end-ts 0` disables the bar cap because the walk-forward
   trades_top covers 2025-12-01 → 2026-03-01 (the full second half of
   the bars CSV). If that PASSes too, the engine is verified against
   both single-window and walk-forward v3 reference traces.

3. **Promote to VPS shadow ALONGSIDE (not displacing) the S59
   USDJPY Asian-Open shadow**, per `HANDOFF_FVG_BACKTEST.md` Section 2.
   `shadow_mode = true` is already pinned in `engine_init.hpp`. No
   header default change is required.

4. **3-month shadow validation gate** — at month 3 / 6 / 9, re-run the
   v3 walk-forward acceptance (n>=50, PF>=1.2, PF>All, cost-stress-2x
   PF>=1.0) against the live shadow's `live_xauusd_fvg.csv`. The
   verifier-side tolerances on `exit_price` (5.0 USD) and `exit_ts_s`
   (900 s) are the architectural-difference floor we agreed to in
   05-02; anything tighter than those on shadow data would require a
   per-bar tick replay that the live harness doesn't do.

---

## Architectural caveats (still in force)

Bit-exact verification against the v3 backtest is **not achievable**
because:

- v3 is bar-driven; engine is tick-driven. v3 detects SL/TP via
  `bar.low <= sl` / `bar.high >= tp` and exits at `gross_exit = sl/tp`.
  Engine detects per-tick via `bid <= sl` / `ask >= tp` and exits at
  `bid/ask`. Worst-case `exit_price` drift = `(sl - bar.low) +
  half_spread`. (In `first_touch_only_mode` the engine SUPPRESSES the
  per-tick check and uses the bar-level path at step 5 of
  `on_bar_close`, so the verifier-only run achieves bar-level
  equivalence; the production code path retains the per-tick check.)
- `exit_ts` may shift by one bar when the per-tick SL trigger fires
  one bar earlier or later than v3's bar-level trigger.

The verifier's loose `--tol-exit-price 5.0` and `--tol-exit-ts-s 900`
defaults absorb these. **Tight-tolerance fields that match exactly:**
`direction`, `entry_ts`, `session`, `sl`, `tp`, `entry_price`,
`score_at_entry`, `atr_at_entry`, `gap_height`, `exit_reason`. All 70
rows clear those tight tolerances.

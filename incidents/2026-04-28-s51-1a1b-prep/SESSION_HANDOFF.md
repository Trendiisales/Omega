# Session Handoff — 2026-04-28 — S51 1A.1.b Engine Fixes Authorised

**HEAD at handoff:** `ae8bf79049ceae02bbaac806b673593375e830a0`
**Date:** 2026-04-28
**Reason for handoff:** Session approaching context limit (~75-85%). User authorised four fixes (D6, EMACross, AsianRange, VWAPStretch) but starting them in this session risks running out of budget mid-fix. Continuing in a fresh session.

---

## State summary at this point

### Live trading
- **VPS still running ed95e27** (verified via service stdout log earlier this session: `[OK] Git hash: ed95e27 -- verify against GitHub HEAD`).
- Service status: Running, mutex held by service-controlled instance.
- **No urgency to deploy** d6ed7ba8 (Option C build-system fix) or any newer commit — the live binary is fine where it is. The HBG_T spread-fix in D4 (`ea3e7943`) is backtest-path only; live FIX trades are unaffected.

### Repo state
Commits in this session, in order:
1. `ea3e7943` — D4: HBG_T spread-at-entry fidelity port + audit memo created
2. `d6ed7ba8` — Option C: `OMEGA_BUILD_SWEEP=OFF` gate + `--target Omega` in 3 PS1 scripts + post-incident notes
3. `ae8bf790` — D5_RESULTS.md memo (current HEAD)

All three commits verified byte-exact via contents API. RESTART_OMEGA.ps1 was already correct pre-session and remains untouched.

### Session-persistence pattern
The `incidents/2026-04-28-s51-1a1b-prep/` folder now contains two memos: `AUDIT_MEMO.md` (1A.1.b prep + post-incident notes from the VPS OOM) and `D5_RESULTS.md` (this session's sweep analysis). This handoff file becomes the third. **Future sessions should read all three before doing any 1A.1.b work.**

---

## What was authorised this session (carrying forward)

User said "i authorize all" in response to:

| ID | Description | Files affected | Live impact |
|---|---|---|---|
| **D6** | HBG-only extended-grid re-sweep (lower trail_frac, higher sl_frac, higher max_range) | `backtest/OmegaSweepHarness.cpp` (sweep grid only) | None |
| **E1** | EMACross degenerate-grid filter (`FAST < SLOW` constraint) | `backtest/OmegaSweepHarness.cpp` (factory) and/or `include/SweepableEngines.hpp` | None |
| **E2** | EMACross hardcoded RSI dead-band review at `SweepableEngines.hpp:991-998` | `include/SweepableEngines.hpp` (engine logic) | **None — affects HBG_T only via shared file, but sibling engines untouched. Live `EMACrossEngine.hpp` unchanged.** |
| **D7** | AsianRangeT diagnostic counters (8 counters across the gates) | `include/SweepableEngines.hpp` (AsianRangeT class instrumentation) | None — instrumentation only |
| **D8** | VWAPStretch structural fix (SL/TP/SIGMA redesign) | `include/SweepableEngines.hpp` (VWAPStretchT class) and/or harness grid | None — backtest-only engine |

**All five touch only sweep/backtest code paths.** No live engine modification. No `Omega.exe` rebuild required for any of these. The VPS does not need to be touched.

---

## Recommended order in next session

The dependencies and information flow:

1. **D6 first** (lowest risk, highest information).
   - One file change: `OmegaSweepHarness.cpp` grid extension.
   - Re-run on Mac. ~17 min.
   - Result decides whether HBG-focused work is the next month or whether the whole sweep program needs rethinking.

2. **E1 second** (cheapest and most decisive bug-fix).
   - Add a `static_assert` or factory-side filter so `FAST < SLOW` is enforced.
   - Re-run EMACross sweep within the same D6 run if possible (combined Mac run).

3. **D7 third** (AsianRange diagnostic).
   - Add 8 counters to `AsianRangeT::process` then run **default-param-only** combo on 1M ticks. Decisive in minutes.
   - Identifies the >100x trade-frequency mismatch root cause.
   - Once root cause is known, shipping the fix is a separate small commit.

4. **E2 fourth** (EMACross RSI dead-band review).
   - Requires re-reading the live `EMACrossEngine.hpp` and confirming whether the harness's hardcoded dead-bands are a port artefact or a deliberate live-engine feature.
   - May be a no-op if live has the same bands intentionally.

5. **D8 last** (VWAPStretch structural fix).
   - Largest scope. Best done after the simpler engines are clean so the harness behaviour is reliable.

Each of these is **one or two commits**, each its own clean session if needed. Don't try to bundle them.

---

## Next-session opener (paste verbatim)

```
/ultrathink

Continuing Omega S51 1A.1.b engine fixes. HEAD pinned at
ae8bf79049ceae02bbaac806b673593375e830a0 on main.

Three memos in incidents/2026-04-28-s51-1a1b-prep/ document state:
  AUDIT_MEMO.md      -- prep audit + VPS OOM post-incident notes
  D5_RESULTS.md      -- sweep results + four-engine pathology audit
  SESSION_HANDOFF.md -- this handoff (read first)

User authorised D6, E1, E2, D7, D8 in prior session. None started.
Recommended order: D6 -> E1 -> D7 -> E2 -> D8 (per SESSION_HANDOFF.md).

VPS is on ed95e27 (live, fine, no urgency to deploy newer commit).

Pick up at: which of D6/E1/D7/E2/D8 do you want first?
```

---

## Key facts the next session must NOT forget

- **Live FIX path** uses 3-arg `confirm_fill` at `order_exec.hpp:445,453`. Live trades carry `spread_at_entry=0.0` until that's updated. Pre-existing condition, not a regression.
- **PnL units** in sweep CSVs are `price-points × 0.01 lot`. Multiply by 100 for $-equivalent.
- **AsianRangeT and AsianRangeEngine are line-for-line nearly identical** — the harness bug is somewhere subtle that static reading didn't pin down. Runtime counters are the answer.
- **EMACrossT degenerate combos** (`FAST >= SLOW`) are ~20% of the grid and produce the misleading "15 top combos with 0 trades" result in the summary.
- **VWAPStretchT's hardcoded `is_decelerating()` threshold of `slow > 0.01`** at `SweepableEngines.hpp:388` is essentially always satisfied on tick-level data, making the deceleration check trivial. Likely needs to scale with price or be removed.
- **Memory rule #30** is wrong-on-disk (says 16,807 combos / 84k total, X2 spec). Real spec is X3 pairwise: 490 combos/engine, 1,960 total. User should action via `memory_user_edits` when ready.

---

## Outstanding memory rule deltas (queued for `memory_user_edits`)

Carrying forward from prior sessions plus this one:

1. **Replace** rule #30: "S51 sweep harness uses pairwise 2-factor (X3 design): 5 params/engine, C(5,2)=10 pairs × 7×7=49 = 490 combos/engine, 1,960 across 4 engines (DXY skipped). 17k full-grid (X2) was rejected — template instantiation OOMs past ~2k."

2. **Add**: "S51 1A.1.a shipped at HEAD 2d29241e1: HBI got mae+spread populators (mirrors HBG S43 mae); HBG got symmetric spread fix. Live FIX path order_exec.hpp:445,453 still uses 3-arg confirm_fill so live spread_at_entry=0.0 until updated."

3. **Add**: "S51 1A.1.b D4 shipped at ea3e7943: HBG_T (SweepableEngines.hpp) got spread_at_entry field + 4-arg confirm_fill default-arg + PENDING call-sites pass spread + _close reads field. Mirrors live HBG. No engine behavioural change."

4. **Add**: "Option C shipped at d6ed7ba8: OmegaSweepHarness gated behind OMEGA_BUILD_SWEEP=OFF default; QUICK_RESTART/REBUILD_AND_START/DEPLOY_OMEGA all gain --target Omega. Two independent guards prevent VPS rebuild OOM (X3 template tuple OOMs MSVC). Mac sweep now requires `cmake .. -DOMEGA_BUILD_SWEEP=ON`."

5. **Add**: "D5 baseline sweep ran 2026-04-28 at HEAD d6ed7ba8: 16.8 min, 154M ticks. HBG produced usable signal at grid edges (best total_pnl=0.47, score=0.40, 44 trades, 70.5% WR at 1:1 RR). EMACross/AsianRange/VWAPStretch all broken: 0-2 trades or 0.5% WR. Sweep CSVs measure PnL in price-points × 0.01 lot — multiply by 100 for $-equivalent."

6. **Add**: "VPS rebuild scripts must always invoke `cmake --build --target Omega` — never bare `cmake --build` — to avoid pulling dev-only targets like OmegaSweepHarness that OOM MSVC. Pattern is now consistent across QUICK_RESTART/REBUILD_AND_START/DEPLOY_OMEGA/RESTART_OMEGA."

7. **Add**: "Always persist Stage audit + decision logs + session handoffs to `incidents/<date>-<slug>/` in the repo before session end. /home/claude/... does not survive session resets and uploaded chat documents do not feed the next session automatically."

8. **Add**: "D1 (MIN_TRAIL_ARM_PTS_STRONG) and D2 (DIR_SL_COOLDOWN_S) deprioritised after D5: HBG's trail logic already barely fires at current settings, and an engine firing 1.8 trades/month doesn't benefit from more cooldowns. D6 (HBG extended-grid re-sweep) is higher leverage and authorised."

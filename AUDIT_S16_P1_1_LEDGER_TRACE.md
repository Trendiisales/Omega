# P1-1 — Static Trace of `[LEDGER-CORRUPT-TS]` Sources

Prepared: 2026-05-08, S16
Scope: P1-1 from `HANDOFF_S16_AFTER_S15_P0_FIXES.md` — trace the unit-mismatch bug surfaced by the new `trade_lifecycle.hpp` sanitizer (P0-1, in working tree, uncommitted at handoff time).
Repo HEAD at trace: `313b0aa06688169da84b77125bce06babfb64a2c` plus the five S15 P0 working-tree fixes (verified present at session start).

---

## TL;DR

Three engines were named in the audit comment as historical sources of the 48 corrupt-timestamp rows in `audit_input/omega_shadow.csv`:

1. `MacroCrash` — already patched at `on_tick.hpp:929-932` (the canonical reference fix; passes `ds_now_ms` instead of `ds_now`).
2. `CandleFlowEngine` — 5 rows.
3. `HybridBracketGold` (the engine label tagged from `SweepableEngines._close` and the CRTP variant) — 1 row.

Static analysis of all entry/exit timestamp assignment sites in the current tree finds **no remaining unit-mismatch in any of the three named engines or in any other engine**. Every site classified is consistent: entry and exit use the same time scale (seconds in current main, with one symmetric ms-source pattern that always divides both halves by 1000).

Conclusion: the historical 48 corrupt rows are most likely **already fixed in current main** by the same Apr-21 refactor that landed the MacroCrash patch (the shadow tape ends 2026-04-08, entirely pre-fix). The S15 P0-1 sanitizer should produce zero `[LEDGER-CORRUPT-TS]` log lines on a post-deploy fresh tape. If it does, P1-1 closes as resolved.

If any line does appear, the table at the end of this document tells you which file the offending engine lives in.

---

## What the bug looks like

Sanitizer at `trade_lifecycle.hpp:43-67` emits when `tr.exitTs < tr.entryTs`. The audit's tape signature is `hold_sec ≈ -entryTs`, which means `exitTs - entryTs ≈ -entryTs`, so `exitTs ≈ 0` (or vastly smaller than `entryTs`). Two compatible causes:

- Case A — `exitTs` was 0 (uninitialised or default-constructed before assignment).
- Case B — unit mismatch: `entryTs` in milliseconds (~1.7e12) while `exitTs` in seconds (~1.7e9) → the difference equals `−entryTs` to within four significant figures.

The MacroCrash fix was Case B: `force_close(..., ds_now)` was called with `ds_now` (seconds) where the function parameter `now_ms` expected milliseconds. Inside the engine, `tr.exitTs = static_cast<int64_t>(now_ms / 1000)` then divided seconds by 1000 → `exitTs ≈ 1.7e6` (six-digits seconds-of-seconds garbage), while `tr.entryTs = pos.entry_ms / 1000` was a normal ~1.7e9. Result: `exitTs << entryTs`, sanitizer fires.

---

## Static-analysis pass

### Method

1. Re-read the P0-1 sanitizer block at `trade_lifecycle.hpp:43-67` to confirm the log fields (engine, symbol, side, entryTs, exitTs, exitReason).
2. Re-read the canonical fix at `on_tick.hpp:929-932`.
3. Enumerated every `tr.exitTs = …` assignment in `include/`. 41 sites across 33 files.
4. Enumerated every `tr.entryTs = …` and `entry_ts = …` assignment to check pairing. 60+ sites.
5. For each site, traced the RHS to its origin (now_ms / now_s / nowSec() / bar.start_ts / pos.entry_ts_ms etc.) and classified by unit.
6. For the three engines named in the audit comment, traced the public API surface (on_tick, force_close) to its callers in `tick_gold.hpp`, `on_tick.hpp`, `quote_loop.hpp`, `config.hpp`.

### Engine-by-engine classification

| Engine                       | Entry-side write                                     | Exit-side write                              | Caller passes correct units | Verdict                                           |
| ---------------------------- | ---------------------------------------------------- | -------------------------------------------- | --------------------------- | ------------------------------------------------- |
| `MacroCrashEngine`           | `tr.entryTs = pos.entry_ms / 1000`                   | `tr.exitTs = static_cast<int64_t>(now_ms / 1000)` | `on_tick.hpp:932` passes `ds_now_ms` (post-fix) | **Already fixed.** Reference patch.               |
| `CandleFlowEngine`           | `tr.entryTs = pos.entry_ts_ms / 1000` (L1583, L1856) | `tr.exitTs = now_ms / 1000` (L1584, L1857)   | `tick_gold.hpp:2334` passes `now_ms_g` (real ms, computed at L690 via `system_clock::now()`) | **Consistent.** No external `force_close` caller anywhere. |
| `HybridBracketGold` (`SweepableEngines._close`, also CRTP at `SweepableEnginesCRTP.hpp:1063-1095`) | `tr.entryTs = pos.entry_ts` (L1084) — `pos.entry_ts` set from `m_last_tick_s` (L932) | `tr.exitTs = now_s` (L1084) | `_close()` parameter is `now_s`; callers within `SweepableEngines` all pass the same `now_s` they received from `on_tick(...)` | **Consistent.** Both halves are seconds. |

### Other engines reviewed for symmetric-unit pattern

All inspected engines pair entry and exit on the same time scale:

- Seconds-only engines (entry from `now_s` / `nowSec()` / `idx_now_sec()` / `ca_now_sec()` / `m_last_tick_s`, exit from same family): `LatencyEdgeEngines`, `AudusdSydneyOpenEngine`, `GbpusdLondonOpenEngine`, `EurusdLondonOpenEngine`, `NzdusdAsianOpenEngine`, `UsdjpyAsianOpenEngine`, `RSIReversalEngine`, `RSIExtremeTurnEngine`, `BracketEngine`, `BreakoutEngine` (`now = nowSec()` at L1095), `IndexFlowEngine`, `CrossAssetEngines` (all 4 sites), `GoldEngineStack`, `GoldMidScalperEngine`, `XauusdFvgEngine` (`bar.start_ts` consistent both halves).
- Symmetric ms→sec engines (entry stored as `entry_ts_ms` / `entry_ms` / `ets`, both halves emitted as `… / 1000`): `CandleFlowEngine`, `MacroCrashEngine`, `EmaPullbackEngine`, `TsmomEngine`, `DonchianEngine`, `TrendRiderEngine`, `MinimalH4Breakout`, `MinimalH4US30Breakout`, `HTFSwingEngines`, `EMACrossEngine`, `CellEngine`, `PDHLReversionEngine`, `C1RetunedPortfolio`, `SweepableEngines` (the `pos.ets / 1000` site at L1404).

### One pre-existing edge case worth noting

`omega_main.hpp:343` in the CSV-replay path:

```
tr.exitTs = std::stoll(tok[5].empty() ? "0" : tok[5]);
```

Falls back to `0` if a CSV column is empty. This will fire the sanitizer if a malformed replay file is fed in, but it will NOT fire on live shadow tapes (this code path runs only during ledger-replay startup). Recommend leaving as-is — the sanitizer's `[CORRUPT_TS]` tag is the right behaviour for a malformed replay row.

---

## What I deliberately did NOT do

- Modify any source file. The user pref is "never modify core code unless instructed clearly", and this is a static-analysis pass. The P0-1 sanitizer is the live instrument; it does the actual catching at runtime.
- Re-investigate the MacroCrash branch. Already fixed at `on_tick.hpp:929-932`, audit-acknowledged.
- Touch any of the five S15 P0 fixes still in working tree.
- Speculate about new bugs in engines that aren't on the audit's three-engine list. The static pass found none, and chasing imaginary bugs without runtime evidence is wasted budget.

---

## What you need to do before this can fully close

P1-1 cannot be marked resolved until the P0-1 sanitizer has been deployed and a fresh shadow tape has accumulated. Two paths from here:

### Path 1 — P0s already deployed

If you've already run `.\OMEGA.ps1 deploy` on the VPS with the S15 working-tree changes and have a tape from after that build, send me whichever is easier:

- the new `omega_shadow.csv` covering >= 100 closed shadow trades since deploy, OR
- a `grep "[LEDGER-CORRUPT-TS]"` of the binary's stderr/stdout log from the same period.

If the grep is empty after >= 100 trades or >= 1 week, I'll mark P1-1 closed-resolved.

If the grep has even one line, I'll use the engine-name field (first token after the tag) plus the suspect table above to find the offending file:line and propose a focused fix in the same shape as the MacroCrash patch.

### Path 2 — P0s not yet built

Then there's nothing more to do on P1-1 until the build lands. Move to the next P1 item (P1-2 `VWAPReversion FORCE_CLOSE-only exits` is the cleanest static-analysis target with current data).

---

## Suspect-routing table — for use when a `[LEDGER-CORRUPT-TS]` line appears

If the `engine=` field in the log line is X, look here first:

| `engine=`               | First file to inspect                                  | Anchor                                                                                    |
| ----------------------- | ------------------------------------------------------ | ----------------------------------------------------------------------------------------- |
| `MacroCrash`            | `on_tick.hpp:929-932`                                  | The reference fix is here — confirm `ds_now_ms` arg is still in place (the S15 audit says yes). |
| `CandleFlowEngine`      | `tick_gold.hpp:2334`                                   | Verify `now_ms_g` is still computed at L690 in ms, not regressed to seconds.              |
| `HybridBracketGold`     | `SweepableEngines.hpp:1064-1095` and CRTP variant      | Check every call site of `_close(...)` in the same file passes `now_s` (seconds), not `now_ms`. |
| `BracketEngine`         | `BreakoutEngine.hpp:1095, 1191, 1221`                  | `now = nowSec()` already; failure here would mean a regression of `nowSec()` itself.      |
| any `*Open*Engine` (Sydney/London/Asian/etc.) | `m_last_tick_s` setter in same file (~L370 in each) | Should be `now_s`. If anything else, that's the bug.                                       |
| `IndexFlow`             | `IndexFlowEngine.hpp:390, 495, 997, 1349`              | All four sites use `idx_now_sec()`. Failure means `idx_now_sec()` semantics regressed.     |
| `CrossAsset*` (`EsNqDiv`, `EIAFade`, `BrentWTI`, `FxCascade`, `CarryUnwind`, `OpeningRange`, `VWAPReversion`, `TrendPullback`, `NoBSMomentum`) | `CrossAssetEngines.hpp:294, 1676, 1737, 1798, 1857`     | All use `ca_now_sec()` for both halves. Failure means `ca_now_sec()` semantics regressed.  |
| `EMACross`              | `SweepableEngines.hpp:1353, 1404, 1405`                | `pos.ets = now_ms`, then `pos.ets / 1000` and `now_ms / 1000`. Both halves divided. If only one side regressed, the inconsistency is here. |
| `CellEngine` / `Tsmom` / `Donchian` / `TrendRider` / `EmaPullback` / `MinimalH4*` / `HTFSwing` / `C1Retuned` | the engine's own `tr.entryTs` / `tr.exitTs` site (in earlier table) | All use the symmetric `… _ms / 1000` pattern. Failure means one half lost the `/ 1000`. |

---

## Sources

- [`include/trade_lifecycle.hpp:43-67`](computer:///Users/jo/omega_repo/include/trade_lifecycle.hpp) — P0-1 sanitizer (the instrument).
- [`include/on_tick.hpp:929-932`](computer:///Users/jo/omega_repo/include/on_tick.hpp) — MacroCrash reference fix.
- [`AUDIT_S15_ENGINES.md`](computer:///Users/jo/omega_repo/AUDIT_S15_ENGINES.md) — engine-by-engine audit; P0-1 is at top.
- [`AUDIT_S15_P0_FINDINGS.md`](computer:///Users/jo/omega_repo/AUDIT_S15_P0_FINDINGS.md) — P0 triage.
- [`HANDOFF_S16_AFTER_S15_P0_FIXES.md`](computer:///Users/jo/omega_repo/HANDOFF_S16_AFTER_S15_P0_FIXES.md) — S16 handoff (this work item is P1-1 there).

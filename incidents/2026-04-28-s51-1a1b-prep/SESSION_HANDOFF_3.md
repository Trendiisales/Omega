# Session Handoff #3 — 2026-04-29 — G1 shipped, G1CLEAN baseline locked

**HEAD at handoff:** `678addbb029913f1d06eed3a8944fcdb31cf4ee0`
**Date:** 2026-04-29 NZST early morning (Wed)
**Predecessors:**
- `incidents/2026-04-28-s51-1a1b-prep/SESSION_HANDOFF.md` (#1, end of D5)
- `incidents/2026-04-28-s51-1a1b-prep/SESSION_HANDOFF_2.md` (#2, end of D6+E1)
- This file (#3, post-G1, post-G1CLEAN-baseline)

**Reason for handoff:** Context budget approaching the 85% mark. All planned work for this session is committed and verified. Clean wrap-up before any further coding.

---

## TL;DR

The AsianRange harness bug is **fully resolved**. The threading hypothesis from D7 was correct. G1 (`thread_local int64_t g_sim_now_ms` in `backtest/OmegaTimeShim.hpp`) is shipped and verified. The G1CLEAN sweep is the new locked baseline.

Outcomes by engine:
- **HBG: byte-identical** between multi-thread D6+E1 and single-shim G1CLEAN. All HBG findings stand without reservation.
- **EMACross: byte-identical**. No edge in any quadrant. Concept-dead on this grid.
- **AsianRange: now operational**, 262–388 trades/combo (was 1–3). 354/382 vs live = 92.7% match. But every top-50 combo loses; class-drift audit recommended before declaring "no edge".
- **VWAPStretch: structural pathology confirmed real**, not a harness ghost. Trade count tripled but WR remains 0.5–1.5%. D8 still warranted.

D6.3 (drop trail_frac from sweep) is **rejected**. trail_frac=0.125 differs from 0.158-0.500 in stddev_q; the param has effect at the floor of the grid only because trail_dist clamps against the hardcoded `mfe*0.20`.

---

## What was shipped this session (5 commits, all verified byte-equivalent via contents API)

| SHA | Description |
|---|---|
| `0227281534` | D7+HBG-DIAG results memo + raw TSVs + D7-FIX-2 script |
| `bb0f4a4f` | DETERMINISM_GUARDS.md design doc (G1..G4 spec) |
| `049508598` | **G1 fix shipped: `thread_local` on g_sim_now_ms / g_sim_start_ms / g_sim_started in `backtest/OmegaTimeShim.hpp`** |
| `678addbb` | D6E1_G1CLEAN results memo + locked baseline summary |

All three of: parent SHA verified pre-push; new SHA confirmed; file md5 disk == api after push.

---

## State at handoff

### Live trading
- **VPS HEAD: `ed95e27c`**. Untouched throughout this whole session. Service running, mutex held. **No urgency to deploy** any newer commit. No live impact from G1 (live binary does not include `OmegaTimeShim.hpp`).

### Repo
- **`origin/main`: `678addbb029913f1d06eed3a8944fcdb31cf4ee0`**
- Mac local should be at the same SHA after `git pull`.

### Mac binary
- Build at: `~/omega_repo/build/Release/OmegaSweepHarness`
- Built: 2026-04-29 ~01:00 NZST with `-DOMEGA_BUILD_SWEEP=ON -DOMEGA_SWEEP_DIAG=OFF`
- Contains G1 (verified by the byte-identical HBG csv across runs).

### Locked baseline
- `~/omega_repo/sweep_D6E1_G1CLEAN_20260429_005927/` — first deterministic 4-engine sweep on 154M ticks under G1.
- Wall: 1235.3s (20.6 min).
- Canonical analysed-output is committed at `incidents/2026-04-28-s51-1a1b-prep/D6E1_G1CLEAN_run/sweep_summary.txt`.

---

## Authorisation menu for next session

### Already authorised, ready to ship (each is one commit)

| ID | Description | Risk | Cost |
|---|---|---|---|
| **E2** | EMACross hardcoded RSI dead-band review at SweepableEngines.hpp:991-998 | Low | ~30 min |
| **D8** | VWAPStretch structural fix (SL/TP/sigma redesign experiments) | None (sweep grid only) | ~1 hr + sweep |

### Newly recommended (need authorisation)

| ID | Description | Risk | Why |
|---|---|---|---|
| **G2** | Determinism self-test in harness (twice-run smoke at startup, exit 2 on mismatch) | Low | Catches any future race. Should ship before further sweep grid changes. |
| **G3** | Per-engine FNV-1a input checksum reported at sweep end | Negligible | Different bug-class than G1; complementary. |
| **G4** | CONCURRENCY.md + Python lint rule | None | Prevents future authors adding new globals to the time path. |
| **D6.1** | HBG max_range rebase 32→16 (geometric grid 8..32) | None | Top-50 27/50 floor-clipped at 16. |
| **D6.2** | HBG min_range rebase 3→6 (geometric grid 3..12) | None | Top-50 43/50 ceiling-clipped at 6.0. |
| **HBG-FIX-1** | Expose hardcoded `mfe*0.20` constant (SweepableEngines.hpp:954) as sweep param `mfe_lock_frac`; demote `trail_frac` to fixed | Low | The actual trail-aggressiveness lever is currently outside the sweep grid. |
| **E1.1** | Extend E1 filter to flag `rsi_lo ≥ rsi_hi` as degenerate | Low | ~10 lines; removes second degeneracy class. |
| **ASN-AUDIT** | Read AsianRangeT vs live AsianRangeEngine line-by-line; produce diff memo. Live achieves 49.7% WR; harness best 43.5%. Possible port drift. | None (audit only) | Required before declaring AsianRange "no edge". |
| **write_csv fail-loud** | Propagate fopen failure to non-zero exit code in OmegaSweepHarness.cpp | None | Removes silent-fail class permanently. |

### Rejected this session (do not pursue)

- **D6.3** (drop trail_frac from sweep) — counter-evidence in G1CLEAN. trail_frac=0.125 differs in stddev_q from 0.158-0.500. Clamped, not dead.

---

## Recommended priority order for next session

The data after G1CLEAN suggests the following sequencing:

1. **G2 first** (~1-2 hrs). Determinism self-test before any further sweep grid changes. Locks in the G1 win.
2. **G4 second** (~1 hr). Concurrency doc + lint. Ship with G2 to prevent regression.
3. **HBG-FIX-1** (~1 hr). Surfaces the real trail param. Required before D6.1/D6.2 to avoid wasting a sweep run.
4. **D6.1 + D6.2 + E1.1 + write_csv fail-loud** as one combined commit (~30 min). All small grid/filter/infra changes.
5. **Re-run sweep** with the new grid (~21 min). Locks the next baseline.
6. **ASN-AUDIT** (~1 hr). Reading exercise. Decide AsianRange's fate.

Items 1–5 fit comfortably in one session. Item 6 belongs to its own session.

E2, D8, and G3 can come in any subsequent session.

---

## What CANNOT be assumed at session start

- **Memory rule 29 was rewritten this session.** Do not trust earlier sessions' SHAs or descriptions of S51 1A.1.b state. Read this handoff first.
- **`AsianRange has no edge` is a soft conclusion**, not a hard one. ASN-AUDIT is required before declaring the engine dead.
- **All HBG findings from D5/D6/D6+E1 are NOW VERIFIED CLEAN** (byte-identical CSV pre/post G1). They were a soft conclusion before; they are hard now.

---

## Pre-delivery rule reminder (carried forward, do not skip)

Per memory rule 30 plus the additions earned this session:

1. State assumptions before writing logic against them.
2. Read referenced files via the GitHub contents API before writing against them — never assume.
3. Check whether existing system already solves the problem before proposing new code.
4. Flag any number/claim whose provenance was not verified this session.
5. After every push: confirm new SHA via API, confirm files in commit via API, byte-equivalence md5(disk) == md5(api).
6. For zsh-on-Mac: write scripts to `/tmp/<name>.sh` via `<< 'OMEGAEOF' ... OMEGAEOF` heredoc, then execute. Avoid pasting multi-line code blocks at the shell prompt.

---

## Next-session opener (paste verbatim)

```
/ultrathink

Continuing Omega S51 1A.1.b. HEAD pinned at
678addbb029913f1d06eed3a8944fcdb31cf4ee0 on main.

G1 shipped and verified. G1CLEAN baseline locked. Read these in order
before touching code:
  1. incidents/2026-04-28-s51-1a1b-prep/SESSION_HANDOFF.md   (#1)
  2. incidents/2026-04-28-s51-1a1b-prep/SESSION_HANDOFF_2.md (#2)
  3. incidents/2026-04-28-s51-1a1b-prep/SESSION_HANDOFF_3.md (#3 - read first)
  4. incidents/2026-04-28-s51-1a1b-prep/D7_RESULTS.md
  5. incidents/2026-04-28-s51-1a1b-prep/DETERMINISM_GUARDS.md
  6. incidents/2026-04-28-s51-1a1b-prep/D6E1_G1CLEAN_RESULTS.md

Top of mind:
  - HBG numbers are now hard-verified clean (byte-identical CSV pre/post G1).
    Combo 261: 49 trades, 63.3% WR, score 0.4377. Edge is real.
  - AsianRange now operational (262-388 trades/combo) but every top-50
    combo has total_pnl < 0. ASN-AUDIT recommended before any conclusion.
  - VWAPStretch structural pathology IS REAL. D8 still warranted.
  - D6.3 (drop trail_frac) REJECTED. Counter-evidence in G1CLEAN data.

VPS still on ed95e27c. No urgency.

Authorised, queued: E2, D8.
Newly recommended (need auth): G2, G4, HBG-FIX-1, D6.1, D6.2, E1.1,
ASN-AUDIT, G3, write_csv fail-loud.

Recommended priority: G2 -> G4 -> HBG-FIX-1 -> D6.1+D6.2+E1.1+write_csv
combined commit -> re-run sweep -> ASN-AUDIT in a separate session.

First action: pick from the authorisation menu in SESSION_HANDOFF_3.md.
```

---

## One thing I owe future-Jo a memo for

The AsianRange harness ghost was a **stacked bug**. Two independent issues — the threading race (G1 fixed) AND the genuine "no edge on this grid" pathology (still under investigation) — were sitting on top of each other. We chased the first one for two sessions thinking it was the only issue. The lesson: when a metric looks impossibly wrong (382× live vs 1× harness), the right question is "what stacked issues could produce this?" not "which single bug is it?"

This is also why ASN-AUDIT matters. We've fixed *one* thing in the AsianRange path; we don't yet know if there's a *second* port-drift issue underneath that's causing the 6pp WR gap (live 49.7% vs harness best 43.5%).

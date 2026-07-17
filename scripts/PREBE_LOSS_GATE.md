# PRE-BE LOSS GATE — `scripts/prebe_loss_audit.sh`

**Added** S-2026-07-17, operator mandate *"why is there not a check"*.
**Enforces** the hard rule `feedback-no-prebe-loss-ever` (BOTH systems): a
companion / mimic / ladder clip must **NEVER book net<0 before break-even is
covered**. The only compliant shape is BE-ENTRY / BE-floor-on-open — open only
once favour ≥ BE, book *nothing* until BE is covered, and floor the exit at
entry (+cost) thereafter.

---

## Why this gate exists (the hole it closes)

`scripts/adverse_protection_audit.sh` checks that an engine header carries an
`ADVERSE-PROTECTION:` verdict **tag**. It answers *"did someone backtest the
adverse-protection step and record a verdict?"* — nothing more. It never looks
at the booking path.

That is not enough. Every vulnerable companion carried such a tag and **passed**
the adverse audit while still booking pre-BE-negative clips:

| Engine | Pre-BE-negative booking sites |
|---|---|
| `FxUpJumpLadderCompanion` | `book_clip_ LOSS_CUT` (5×thr pre-arm), `TRAIL_STOP` (sub-BE trail), `WINDOW_EXIT` |
| `GoldTrendMimicLadder` | `book_clip_` pre-arm `LOSS_CUT` (−lc_pct), `TRAIL_STOP`, `WINDOW_CAP` |
| `BeCascadeCompanionEngine` | `jf_book_ RESTORE_PREBE / PREBE_STOP`, `emit_clip_ PREBE_CUT / REVERSAL_CUT / ENGINE_EXIT`, `emit_be_clip_ BE_TRAIL_CLIP / ENGINE_EXIT` (honest real column = worse-of H1 close, can be < BE) |
| `StockDayMoverLadderCompanion` | `book_clip_ LOSS_CUT / WINDOW_EXIT` |

A written rule with no gate fails the first time a session ignores it (that is
exactly how BE-CASCADE DOT/NEAR booked −178). So this is a **build gate**, not
prose.

## Relationship to the adverse-protection audit (complementary, not duplicate)

| Gate | Question it answers | Mechanism |
|---|---|---|
| `adverse_protection_audit.sh` | *"Is there a verdict?"* | An `ADVERSE-PROTECTION:` tag is present in the header. |
| `prebe_loss_audit.sh` (this) | *"Can a clip book pre-BE negative?"* | Every clip-booking site is BE-floor-on-open protected or grandfathered. |

An engine can (and the vulnerable ones did) pass the first and fail the second.
Both run in `scripts/mac_canary_engines.sh`, the mandated pre-commit canary.

---

## How it works

### 1. In-class header set (derived, not hardcoded)
`include/*Companion*.hpp` ∪ `include/*Ladder*.hpp` ∪ `include/*Mimic*.hpp`
∪ any `include/*.hpp` defining a `class`/`struct` whose **name** contains
`Companion | Ladder | Mimic`. An empty derivation = **exit 2** (structural
break — never a silent pass), mirroring `ungated_engine_audit.sh`.

### 2. Clip-booking sites (safe-by-default reason classification)
A booking site is a call to an **emitter** —
`book_clip_ | jf_book_ | emit_clip_ | emit_be_clip_` — that carries a quoted
reason token. Function *definitions* (no reason token) and `book_mimic_stop_`
(books at the resting stop ≥ BE, no reason token) are not sites.

A site is **safe-by-exemption** only if **every** reason token on the line is a
recognised BE-floor reason:

- `BE_FLOOR`, `RESTORE_FLOOR`, `FLOOR_TRAIL_STOP` — floor books, settle ≥ BE.
- `FLOOR_TRAIL_CLIP` — the intrabar mimic-floor exit; books at `lg.stop_px`
  ratcheted to ≥ `le*(1+RT)`, so `gross_real = gross − RT ≥ 0`. **Provably
  floored** in the REAL column.

**Everything else is RISK-by-default** — `LOSS_CUT`, `PREBE_CUT`, `PREBE_STOP`,
`REVERSAL_CUT`, `REVERSAL_EXIT`, `WINDOW_EXIT`, `WINDOW_CAP`, `TRAIL_STOP`,
`ENGINE_EXIT`, `RESTORE_PREBE`, **and any brand-new reason**. Safe-by-default is
deliberate: a new negative reason cannot slip in silently.

> Note `BE_TRAIL_CLIP` and `ENGINE_EXIT` are **deliberately RISK**, not safe,
> even though their MODEL column is clamped ≥ 0. The be_floor-family *honest real
> column* books the **worse-of H1 close** (`gross_real = (cur/le − 1)`), which can
> be below BE when price gapped through the stop between closes. Those are the
> exact pre-BE-negative clips this gate is meant to expose.

### 3. Compliance markers (BE-floor-on-open recognition)
A RISK site must be BE-floor protected, recognised by:

- **FILE-level** (clears the whole header — the real fix landed):
  `confirm_anchor_epx` · `be_floor_on_open` · `mimic_floor && be_floor`
- **SITE-level** (within ±`WINDOW` lines, default 6, of the booking site):
  the file markers above, or a `std::max(entry,..)` / `std::min(entry,..)` floor
  clamp on the booked fill/level.

A RISK site with no marker is **UNPROTECTED**.

### 4. Allowlist (`scripts/prebe_loss_allowlist.txt`) — grandfather + self-retire
One header basename per line + a documented reason. An UNPROTECTED site only
passes if its header is allowlisted. Categories: **backfill-owed** (currently
vulnerable, floor fix landing this session), **retired/dormant** (must gain a
floor before re-enable), **out-of-class** (a directional real-position engine
mis-caught — a real ride-to-flip position is not an additive companion clip).

**Self-retiring:** once an agent lands a `confirm_anchor_epx` / `be_floor_on_open`
marker in a grandfathered header, the gate recognises it as **CLEAN without the
allowlist entry**, and the now-stale entry is **WARNed** so the list stays honest.
The allowlist should shrink to empty as the session's floor fixes land.

### Exit codes
- **0** — clear: every risk site is BE-floored-on-open or grandfathered.
- **1** — a NEW unprotected pre-BE booking site (fix it or document it).
- **2** — structural breakage (no in-class headers derived / allowlist missing).

---

## Current-tree status (S-2026-07-17, pre-fix)

```
ALLOWLISTED (backfill-owed): include/BeCascadeCompanionEngine.hpp   — 8 sites
ALLOWLISTED (backfill-owed): include/FxUpJumpLadderCompanion.hpp    — 3 sites
ALLOWLISTED (backfill-owed): include/GoldTrendMimicLadder.hpp       — 5 sites
ALLOWLISTED (backfill-owed): include/StockDayMoverLadderCompanion.hpp — 4 sites
--- prebe-loss audit: 0 clean, 4 allowlisted(backfill-owed), 0 new-unprotected-site(s) across 20 risk site(s) ---
PASS (green only because of the grandfather allowlist; 4 engines owe a floor fix)
```

As the session's BE-floor-on-open fixes land, each engine drops out of the
allowlist (WARNed stale) and reports `CLEAN`.

---

## Later: runtime boot gate (sketch — NOT yet implemented in main.cpp)

The static gate proves the *source* can't book pre-BE negative. A runtime gate
would prove the *configured cells* can't, mirroring Chimera's `[MIMIC-FLOOR-GATE]`.
Proposed shape (to add later, in `engine_init.hpp` after all companion cells are
constructed — **do not implement now**):

```
// [PREBE-FLOOR-GATE] boot self-test — every companion cell must floor.
int n = 0, floored = 0, viol = 0;
for (auto& cell : all_companion_cells()) {          // Fx/Gold/Stock ladders, BeCascade legs
    ++n;
    // simulate an immediate adverse tick right after (hypothetical) open:
    double booked = cell.simulate_worst_prebe_clip();   // returns net the cell WOULD book
    if (booked >= 0.0 - 1e-9) ++floored;                // floored: never books < 0 pre-BE
    else { ++viol; log("[PREBE-FLOOR-GATE] VIOLATION %s books %.2f pre-BE", cell.tag(), booked); }
}
log("[PREBE-FLOOR-GATE] %d/%d floored %d VIOLATION", floored, n, viol);
if (viol) { /* refuse to arm those cells (or hard-abort boot, per operator) */ }
```

This complements the static gate the same way the seed `[SEED]` boot line
complements the seed-registry static gate: source-level guarantee at commit time,
behavioural guarantee at boot time.

---

## ⚠️ S-2026-07-17f HONESTY CORRECTION — the gate is a CONFIG property, not an execution guarantee

The adversarial verify (17d disaster-stop sweep) found the crypto
`UpJumpLadderCompanion::book_mimic_stop_` booked floored exits at the **resting-stop
LEVEL** (`lg.stop_px`, which is ≥ BE by construction), **NOT** the price that actually
pierced it. So the live `MIMIC-FLOOR-GATE ... 0 VIOLATION` line + the shadow ledger's
`floorMin=+0.0 / nNeg=0` were a **MODEL/SHADOW property, not execution truth** —
PF=999 / nNeg=0 was the mechanically-impossible tell. Under honest worse-of fills a
floored leg **can** book below BE on a gap-through.

**What this gate actually proves:** that every companion cell is *configured*
floored-on-open (BE-ENTRY + anchor + floor). That is a real and valuable static/config
property — it removes the immediate-entry and un-anchored-reclip pre-BE **windows**.
It does **NOT** prove that no clip ever books negative: a gap through the ≥BE stop
still books its true sub-BE tail. Fix S-17f (crypto b9e350e): `book_mimic_stop_` now
books the ACTUAL fill so the ledger shows the real tail instead of clamping it to +0
(a display-truth fix, `feedback-content-parity-not-just-plumbing`). **Never cite this
gate — or `MIMIC-FLOOR-GATE` — as a "no clip books negative" execution guarantee.**

---

## OUT-OF-CLASS: real/parent directional positions are NOT companions

Some cells LOOK like they book pre-BE-negative clips but are **out of scope** for the
no-pre-BE-loss rule because they are **real / parent directional positions carrying a
backtested structural stop**, not shadow companions mimicking a parent. They *honestly*
book a real stop loss — that IS the correct behaviour for a directional trade, and the
rule (`feedback-no-prebe-loss-ever`) governs COMPANIONS, not directional entries.

Documented out-of-class cells (crypto; ruled S-2026-07-17f, operator):

| Cell(s) | Why out-of-class |
|---|---|
| **PJ jump_floor** (AAVE / ETH / GRT) | Real up-jump spot parent position; carries the widest per-cell backtested structural stop; books its real stop loss honestly (no clamp). Not a mimic. |
| **Campaign** (UNI / TRX / LDO) | Real virtual-parent campaign position; keeps its structural `pstop`. A directional book, not an additive companion clip. |

The dominance/no-pre-BE-loss framing does not apply to these — judge them as directional
strategies (`feedback-companion-independent-engine` is about the *reverse* case:
companions must be judged standalone, never vs the parent). Canonical: Memory-Chimera
`UpJump2pctSpotParent` + the campaign entity.

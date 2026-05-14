# S63 state-classification audit — IndexMacroCrash x4 + IndexSwing — 2026-05-14 (part W)

**Status:** COMPLETE. Read-only audit closing the bonus findings from the
2026-05-14 part-V audit memo (`outputs/S63_STATE_CLASSIFICATION_2026-05-14.md` §6).

**Verdict:**

| Engine | Predicted (part-V) | Confirmed | Action |
|---|---|---|---|
| IndexMacroCrashEngine x4 | A (class-default) | **A** (class-default) | None |
| IndexSwingEngine x2 | E (no S63 hooks) | **E** | Queue (see §4) |

Both predictions held. No code or config changes required.

## 1. Scope

Part-V audit §6 flagged two engines as out-of-scope for the original
5-engine classification (`IndexFlow x4`, `XauusdFvg`, `PDHL`,
`RSIReversal`, `XauThreeBar30m`) but visible during the read of
`IndexFlowEngine.hpp`. Part-V predicted state A for `IndexMacroCrash`
pending init verification, and state E for `IndexSwing`.

This memo verifies both predictions using the same method as the
part-V audit memo §2 (grep class body + grep init + grep repo-wide
for set-sites).

## 2. IndexMacroCrashEngine x4 (US500/USTEC/NAS100/DJ30)

### 2.1 Class declaration

**File:** `include/IndexFlowEngine.hpp:962-1219`.

**S63 implementation:**

- Field declarations at `IndexFlowEngine.hpp:967-974`. Comment block:

  ```
  // S63 2026-05-13 VWR-pattern in-flight protection (LOSS_CUT + BE_RATCHET).
  //   Macro-crash fade is mean-reversion style with a 60-min timeout
  //   profile (cfg_.max_hold_sec). Defaults use VWR's index-tuned values
  //   (US500 @ 7400, 0.08 -> 5.92pt LOSS_CUT). Override per-instance in
  //   engine_init.hpp if needed.
  double LOSS_CUT_PCT  = 0.08;
  double BE_ARM_PCT    = 0.05;
  double BE_BUFFER_PCT = 0.02;
  ```

  Full S63 trio with non-zero defaults. The comment text matches the
  canonical VWR pattern phrasing.

- Management-path at `IndexFlowEngine.hpp:1118-1196` (in
  `manage_position()`). The S63 block at L1123-1150 runs BEFORE the
  staircase BE/trail logic, so cold-loss / giveback cuts take priority.
  Two gated phases:
  - **BE_RATCHET** (L1126-1137):
    `if (BE_ARM_PCT > 0.0 && BE_BUFFER_PCT >= 0.0 && base_entry_ > 0.0)`.
    Emits a `BE_CUT` partial when MFE reaches arm threshold and current
    move falls below buffer.
  - **LOSS_CUT** (L1139-1150):
    `if (LOSS_CUT_PCT > 0.0 && base_entry_ > 0.0)`.
    Emits a `LOSS_CUT` partial when adverse distance exceeds cut threshold.

### 2.2 Instances

Per `globals.hpp:286-289`:

```
omega::idx::IndexMacroCrashEngine g_imacro_sp  ("US500.F");
omega::idx::IndexMacroCrashEngine g_imacro_nq  ("USTEC.F");
omega::idx::IndexMacroCrashEngine g_imacro_nas ("NAS100");
omega::idx::IndexMacroCrashEngine g_imacro_us30("DJ30.F");
```

Four instances, one per index instrument.

### 2.3 Init blocks

**Repo-wide grep** for `g_imacro_(sp|nq|nas|us30)\.(LOSS_CUT_PCT|BE_ARM_PCT|BE_BUFFER_PCT)`:
zero hits.

**Grep `g_imacro_` in `engine_init.hpp`:**

| Line | Content | Type |
|---|---|---|
| 3329-3336 | `g_open_positions.register_source("IndexMacroCrash<X>", _make_imacro_source(...))` | GUI position-source registration, NOT engine configuration |

No `shadow_mode`, `enabled`, or S63 assignments anywhere. The class
default `shadow_mode = true` at `IndexFlowEngine.hpp:964` (commented
"NEVER change without authorization") applies. The class-body
hardcoded shadow guard is the canonical "shadow until explicit
authorization" pattern.

### 2.4 Verdict — STATE A (class-default route)

`LOSS_CUT_PCT = 0.08`, `BE_ARM_PCT = 0.05`, `BE_BUFFER_PCT = 0.02`
authoritative at runtime. Full S63 trio fires on every managed tick.
Engine is hard shadow (class-body guard) so all S63 fires are on
simulated trades.

Same shape as `XauusdFvg`, `PDHL`, `XauThreeBar30m` from the part-V
audit: full S63 trio + class-default route + hard shadow at the
class level. The class-default route §5.2 lesson from the part-V
memo applies — no `engine_init.hpp` override doesn't imply "S63 not
active"; the class-body default IS the runtime value.

## 3. IndexSwingEngine

### 3.1 Class declaration

**File:** `include/IndexFlowEngine.hpp:1360-1580`.

**S63 implementation:** **NONE.**

Grep across the full class body (L1360-L1578, before the closing brace
and trailing `s_trade_id_` definition) for
`LOSS_CUT_PCT|BE_ARM_PCT|BE_BUFFER_PCT|S63` returned zero hits.
Confirmed via the repo-wide grep result returning no `S63` matches in
the 1360+ line range of `IndexFlowEngine.hpp`.

The engine uses a different management pattern entirely:

- Fixed `sl_pts_` at construction (per-symbol calibration: 8.0 for
  US500.F, 25.0 for USTEC.F).
- Simple BE lock at 1× `sl_pts_` profit (L1519-1526).
- Trail at 0.5× `sl_pts_` behind MFE once BE locked (L1528-1534).
- `SWING_MAX_HOLD_SEC = 28800` (8h timeout with the part-L VWR
  winner-exemption — `cur_move_s <= 0.0` required to fire timeout).

The fixed-point SL + 0.5× trail design is structurally different from
the percentage-based S63 protection. Adding S63 would mean introducing
a new design path that competes with the existing BE/trail logic, not
just adding a guarded check.

### 3.2 Instances

Per `omega_types.hpp:300-301`:

```
static omega::idx::IndexSwingEngine g_iswing_sp("US500.F",  8.0, 0.5, 0.5);
static omega::idx::IndexSwingEngine g_iswing_nq("USTEC.F", 25.0, 1.5, 0.2);
```

Only two instances (US500.F + USTEC.F). Note this is NOT four instances
— US30 and NAS100 are not declared for IndexSwing, unlike IndexFlow
and IndexMacroCrash. The part-V audit memo §6.2 referenced
`omega_types.hpp:300-301` correctly; the "x4" framing applies only to
IndexFlow and IndexMacroCrash.

### 3.3 Init block

`engine_init.hpp:1498-1503`:

```
// ?? IndexSwingEngine configure -- shadow mode confirmed ??????????????????
// sl_pts / min_ema_sep / pnl_scale set at construction in omega_types.hpp.
// shadow_mode=true: engine tracks paper P&L only, no FIX orders sent.
// NEVER set shadow_mode=false without explicit authorization + live validation.
g_iswing_sp.shadow_mode = true;
g_iswing_nq.shadow_mode = true;
```

Only `shadow_mode` reaffirmed. The class-body default is also
`shadow_mode = true` (`IndexFlowEngine.hpp:1362`), so this is a
re-affirmation block, not an override. The "shadow until explicit
authorization" comment matches the IndexMacroCrash pattern.

### 3.4 Verdict — STATE E

No S63 fields, no S63 management-path. The engine relies on:
- Fixed `sl_pts_` SL (which IS hit-stop-loss exit protection, but
  not the configurable percentage-based S63 pattern).
- Single-stage BE lock at 1× sl_pts_ profit.
- Single-stage trail at 0.5× sl_pts_ behind MFE.
- 8h max-hold with VWR winner exemption.

To transition state E → A would require:
1. Adding `LOSS_CUT_PCT` / `BE_ARM_PCT` / `BE_BUFFER_PCT` member fields
   with non-zero class defaults.
2. Adding a guarded S63 management-path block (mirroring the
   IndexMacroCrash pattern at `IndexFlowEngine.hpp:1123-1150`) that
   runs BEFORE the existing fixed-SL/trail logic.
3. No `engine_init.hpp` change required if class defaults are chosen
   (per the class-default route lesson).

**Decision:** queue for future evaluation, not actioned this session.
Justification:
- Engine is hard shadow at the class level — no live capital at risk.
- The fixed `sl_pts_` already provides cold-loss protection at a
  predetermined distance; the value-add of S63 here is a giveback
  cut (BE_RATCHET phase) on top of the existing SL-only protection.
- Per-instrument backtest evidence required before any S63 addition
  is justified — same decision rule as the VWR/UTF5m phase-3 protocol.
- Engine activity is constrained (London + NY only, 30min cooldown,
  4h direction-flip cooldown) — total expected trade count is low.

## 4. Queued follow-ups

### 4.1 IndexSwing state-E → A decision

Optional. Decision criteria mirror the broader per-instrument S63
gating protocol established by VWR S71 and UTF5m S73:

1. Run a baseline WF on `IndexSwingEngine` shadow trades to measure
   current P&L distribution (no S63 active).
2. Run a candidate WF with S63 added at IndexMacroCrash-equivalent
   defaults (0.08 / 0.05 / 0.02 — same instrument-price scale).
3. Compare PF, avg-pnl, p95 loss distribution.
4. Activate only if the candidate WF passes the standard gate
   (≥3/4 windows `avg_pnl ≥ +0.001` AND aggregate PF ≥ 1.20) AND
   shows meaningful improvement over baseline.

Total expected wall-clock: 1-2 hours if the WF infrastructure
(`scripts/utf5m_wf_t1.py` template) is reused. Lower priority than
the queued XauTrendFollow trio (which has clearer Phase 3 P3 outcome
signal to compare against).

### 4.2 IndexMacroCrash — none required

State A confirmed. No action.

**Optional hygiene** (mirrors part-V audit memo §5.1): could add an
explicit re-affirmation block in `engine_init.hpp` ahead of the
position-source registration at L3329, mirroring the
`g_vwap_rev_ger40` precedent. Zero runtime effect; one separate
commit per instance (or one bundled commit since they share a class).
Lower priority than the 5 part-V-flagged hygiene commits (which are
the canonical scope for that work).

## 5. Standing audits at session end

**Core code preserved.** None of `OmegaCostGuard.hpp`,
`OmegaTradeLedger.hpp`, `SymbolConfig.hpp`, `OmegaFIX.hpp`,
`OmegaApiServer.hpp`, `GoldPositionManager.hpp` were modified.

**Engine code untouched.** No modifications to `IndexFlowEngine.hpp`
or any other `*Engine.hpp` file. This audit was read-only.

**Engine config untouched.** No modifications to `engine_init.hpp`
in this audit. (Other commits this session — hygiene blocks for the
part-V-flagged 5 engines — are tracked separately.)

## 6. Outcome and lessons

**Closed:** part-V bonus findings §6.1 (IndexMacroCrash) and §6.2
(IndexSwing). Both predictions held.

**Method efficiency:** total audit wall-clock for both engines well
under 15 minutes (re-using the part-V memo §2 recipe). The method
generalises cleanly to the ~25-engine universe-wide sweep
continuation (part-V audit memo §7 item 5).

**Lesson reinforced:** the part-V audit memo §5.2 lesson — that S63
state classification must check BOTH the class-default route AND
the explicit-init-override route — held again. IndexMacroCrash's
shape (full S63 trio in class body, no init override, hard shadow at
class level) is the same as XauusdFvg / PDHL / XauThreeBar30m. The
class-default route is the prevailing pattern.

**Lesson new:** an engine with NO S63 hooks (state E) is not
necessarily missing protection — it may be using a structurally
different SL/trail design (fixed-points + simple trail in
IndexSwing's case). State E does not imply "unprotected" — it
implies "protected by a non-S63 mechanism". The decision to transition
to S63 is a design choice, not a remediation, and requires
per-instrument evidence.

## 7. Cumulative S63 state inventory (updated)

Adding the two engines from this audit to the part-V inventory:

| State | Engines | Notes |
|---|---|---|
| A (explicit init) | g_vwap_rev_ger40, g_ustec_tf_5m | S63 wired; UTF5m disabled by S68 |
| A (class-default) | IndexFlow x4, IndexMacroCrash x4 (**new**), XauusdFvg, PDHL, RSIReversal (disabled), XauThreeBar30m | All shadow at class level except IndexFlow (which has no enabled flag) and PDHL (which has shadow_mode=true enabled=true) |
| B | VWAPReversion SP/NQ/EURUSD | Documented backtest evidence for zeroing |
| E (no hooks) | IndexSwing x2 (**new**), + ~25 unaudited | IndexSwing uses fixed-pts SL design; others pending |

The state-A roster gained 4 instances (IndexMacroCrash x4). The state-E
roster gained 2 confirmed (IndexSwing x2). The ~25-engine outstanding
universe sweep remains the largest open audit task.

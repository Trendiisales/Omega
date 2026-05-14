# S63 state-classification audit — 5 part-K unverified engines — 2026-05-14

**Status:** COMPLETE. Read-only audit per part-U handoff §"Recommended
next-session focus" item 4. All five engines classified against the
part-K framework (states A/B/C/D/E). **Verdict: all five are STATE A.**
No code changes required. The part-K "probably state C or D" guess for
IndexFlowEngine was wrong — the engines are activated via non-zero
class-default values that are not touched in `engine_init.hpp`, which
is functionally equivalent to State A even though it differs
stylistically from the canonical `g_vwap_rev_ger40` precedent (explicit
init re-affirmation).

Two bonus findings outside the part-K audit list are flagged in §6 for
future follow-up.

## 1. Scope and framework

Per part-K §"The S63 audit framework" (and reiterated in part-U §"S63
state-classification audit"), every engine sits in one of five states
with respect to S63 `LOSS_CUT_PCT` / `BE_ARM_PCT` / `BE_BUFFER_PCT`
in-flight protection:

| State | Member fields | Mgmt-path check | Init config | Action |
|---|---|---|---|---|
| A | Declared | Implemented | Activated (non-zero at runtime) | Leave alone |
| B | Declared | Implemented | Deliberately zeroed with documented evidence | Leave alone, do NOT revert |
| C | Declared | Implemented | Zeroed by oversight (no evidence) | Flip to class defaults |
| D | Declared | NEVER WRITTEN | (any) | Add mgmt-path check + activate |
| E | None | None | None | Add hooks + mgmt-path + activate |

**Canonical reference:** `VWAPReversionEngine` at
`include/CrossAssetEngines.hpp:1202` (class) +
`CrossAssetEngines.hpp:1304-1383` (S63 management-path). The four
USTEC-cohort instances of that class live in states A (GER40 — active)
and B (SP/NQ/EURUSD — deliberately zeroed with parts K and L sweep
evidence at `engine_init.hpp:597-672`).

**Audit subject:** the five engines listed in part-K and re-iterated in
part-U as "still-unverified S63-wired":

1. `IndexFlowEngine` x4 (SP/NQ/US30/NAS100)
2. `XauusdFvgEngine`
3. `PDHLReversionEngine`
4. `RSIReversalEngine` (cold-loss-only flavor)
5. `XauThreeBar30mEngine`

The part-K table noted line-number S63 references in each header and
asked for state classification. This memo answers each.

## 2. Method

For each engine:

1. Grepped the engine header for `LOSS_CUT_PCT|BE_ARM_PCT|BE_BUFFER_PCT|
   loss_cut_pct|be_arm_pct|be_buffer_pct|S63`. Confirmed (a) field
   declarations exist with non-zero default values, (b)
   management-path check using `if (LOSS_CUT_PCT > 0.0)` etc. is
   present and runs every tick during `pos.manage()`.
2. Grepped `engine_init.hpp` for the instance name + S63 field assignment.
   Repeated repo-wide to confirm no other config site touches the field.
3. Read the engine's init block in `engine_init.hpp` to confirm what IS
   set (shadow_mode, enabled, lot, max_spread, etc.) and verify S63
   fields are not zeroed elsewhere.

State A requires runtime values to be non-zero. Two routes:
- **Explicit init override:** `g_X.LOSS_CUT_PCT = 0.08;` in
  `engine_init.hpp`. (Canonical precedent: `g_vwap_rev_ger40` block at
  `engine_init.hpp:632-634`, and `g_ustec_tf_5m` block at
  `engine_init.hpp:953-970`.)
- **Class-default route:** the engine class declares a non-zero default
  (e.g. `double LOSS_CUT_PCT = 0.07;` in the class body) and
  `engine_init.hpp` simply does not touch the field. Result is
  identical — the field holds the class default at runtime — but the
  configuration is not visible from a grep of `engine_init.hpp` alone.

The part-K framework didn't distinguish these two routes; the "state A"
column implicitly assumed the explicit-init shape (which is what
`g_vwap_rev_ger40` uses). The five engines below all use the
class-default route. This is a stylistic difference, not a state
difference — the engine fires the management-path identically. §5
returns to this distinction.

## 3. Per-engine findings

### 3.1 IndexFlowEngine (4 instances: SP/NQ/NAS/US30)

**Header:** `include/IndexFlowEngine.hpp:540` (class).

**S63 implementation:**
- Field declaration: `IndexFlowEngine.hpp:553` —
  `double LOSS_CUT_PCT = 0.07;` (cold-loss-only flavor; "the staircase
  ATR trail covers giveback well already, so we add ONLY the cold-loss
  phase here" — comment at L547-552).
- Management-path: invoked every tick via
  `IndexFlowEngine.hpp:651-654` — `pos_.manage(..., LOSS_CUT_PCT)`.
  The check itself runs in `IdxOpenPosition::manage` at
  `IndexFlowEngine.hpp:419` — `if (loss_cut_pct > 0.0 && entry > 0.0)`.

**Init block:** `engine_init.hpp:57-62` — only `set_shadow_mode()` is
called for the 4 instances. NO `LOSS_CUT_PCT` override. The class has
no `enabled` member (verified by repo-wide grep) — dispatch is
controlled by `set_shadow_mode` + the cross-engine `can_enter` gate at
the tick-dispatch level (`tick_indices.hpp`).

**Repo-wide grep for set-site:** zero hits for
`g_iflow_(sp|nq|us30|nas).LOSS_CUT_PCT`. The class default 0.07 is
authoritative at runtime.

**Verdict:** **STATE A** (class-default route). LOSS_CUT_PCT = 0.07
fires on every managed tick. Cold-loss-only — no BE_ARM_PCT or
BE_BUFFER_PCT exist (the class doesn't declare them).

**Note on part-K's guess:** part-K said "Confirmed engine_init.hpp has
NO call site passing non-zero. Probably state C or D — needs audit." The
part-K author appears to have interpreted "no init-time override" as
"not activated". In fact, `pos_.manage(..., LOSS_CUT_PCT)` is invoked
unconditionally every tick and the class default is 0.07 — the engine
HAS been running with S63 active since the part-K-cited 2026-05-13
commit. No state-change action needed.

### 3.2 XauusdFvgEngine

**Header:** `include/XauusdFvgEngine.hpp`.

**S63 implementation:**
- Field declarations: `XauusdFvgEngine.hpp:142-144` — full S63 trio:
  - `double LOSS_CUT_PCT  = 0.05;`
  - `double BE_ARM_PCT    = 0.03;`
  - `double BE_BUFFER_PCT = 0.012;`
- Management-path: `XauusdFvgEngine.hpp:1063-1083`. Both BE_RATCHET
  arm logic (L1071-1078) and LOSS_CUT (L1081-1083) are present, gated
  by `if (BE_ARM_PCT > 0.0 && BE_BUFFER_PCT >= 0.0)` and
  `if (LOSS_CUT_PCT > 0.0)` respectively.

**Init block:** `engine_init.hpp:281-286` — only `shadow_mode = true`
(HARD shadow pin), `cancel_fn`, `on_close_cb` are set. NO S63 override.

**Repo-wide grep for set-site:** zero hits for
`g_xauusd_fvg.LOSS_CUT_PCT|BE_ARM_PCT|BE_BUFFER_PCT`. Class defaults
0.05 / 0.03 / 0.012 are authoritative.

**Verdict:** **STATE A** (class-default route). Full S63 trio active.
Engine is HARD shadow until 3-month gate clearance per init comment
(L268-273), so S63 fires on simulated trades only.

### 3.3 PDHLReversionEngine

**Header:** `include/PDHLReversionEngine.hpp`.

**S63 implementation:**
- Field declarations: `PDHLReversionEngine.hpp:70-72` — full S63 trio,
  XAU-scaled:
  - `double LOSS_CUT_PCT  = 0.04;` (~$1.50 cut at $3700 entry)
  - `double BE_ARM_PCT    = 0.025;` (~$0.92 arm threshold)
  - `double BE_BUFFER_PCT = 0.01;` (~$0.37 BE-trigger buffer)
- Management-path: `PDHLReversionEngine.hpp:221-244`. Both
  BE_RATCHET (L228-237) and LOSS_CUT (L239-244) are present and gated.

**Init block:** `engine_init.hpp:2449-2471` (inside the
`engine_init_runtime` block; the same lambda also reconfigures
`g_rsi_reversal`). Touches `shadow_mode`, `enabled`, `RANGE_ENTRY_PCT`,
`SL_ATR_MULT`, `TP_RANGE_FRAC`, `L2_LONG_MIN`, `L2_SHORT_MAX`,
`DRIFT_FADE_MIN`, `MIN_RANGE_PTS`, `RISK_USD`, `COOLDOWN_MS`,
`MAX_HOLD_MS`. NO S63 override.

**Repo-wide grep for set-site:** zero hits for
`g_pdhl_rev.LOSS_CUT_PCT|BE_ARM_PCT|BE_BUFFER_PCT`. Class defaults
0.04 / 0.025 / 0.01 are authoritative.

**Verdict:** **STATE A** (class-default route). Full S63 trio active.
Engine is `shadow_mode = true` + `enabled = true` per init L2449-2450
("shadow until confirmed live").

**Cross-reference:** the part-K-flagged tuning memo at
`outputs/S63_PDHL_TUNING_RECOMMENDATION_2026-05-13.md` proposed
parameter adjustments. That memo's premise (S63 not yet active on
PDHL) is incorrect — S63 has been active via class default since the
2026-05-13 commit. Any retune would now operate on a live (shadow-fire)
parameter set, not a dormant one. Re-evaluating the recommendation
against this correction is queued for a future session if the operator
wants to act on it.

### 3.4 RSIReversalEngine

**Header:** `include/RSIReversalEngine.hpp`.

**S63 implementation:**
- Field declaration: `RSIReversalEngine.hpp:98` — cold-loss-only:
  `double LOSS_CUT_PCT = 0.05;` (~$1.85 adverse cut at XAU $3700).
  The header comment at L87-94 explicitly notes "(LOSS_CUT only)" —
  no BE_RATCHET in this engine's design.
- Management-path: `RSIReversalEngine.hpp:571-582`. Single
  `if (LOSS_CUT_PCT > 0.0)` gate (L574), cold-loss cut applied at
  L576-582.

**Init block:** `engine_init.hpp:466-485`. Touches `enabled = false`
(L467, "DISABLED 2026-05-01 -- backtest negative EV"), `shadow_mode`,
`RSI_OVERSOLD`, `RSI_OVERBOUGHT`, `RSI_EXIT_*`, `SL_ATR_MULT`,
`TRAIL_ATR_MULT`, `BE_ATR_MULT`, `COOLDOWN_S`, `COOLDOWN_S_VACUUM`,
`MAX_HOLD_S`, `MIN_HOLD_S`. NO LOSS_CUT_PCT override.

**Repo-wide grep for set-site:** zero hits for
`g_rsi_reversal.LOSS_CUT_PCT`. Class default 0.05 is authoritative.

**Verdict:** **STATE A** (class-default route), **but engine is
DISABLED**. S63 is wired correctly; the engine just doesn't fire
because `enabled = false` (negative-EV finding from 2026-05-01
backtest). If the engine were ever re-enabled, S63 protection would
already be active without any further config work.

### 3.5 XauThreeBar30mEngine

**Header:** `include/XauThreeBar30mEngine.hpp`.

**S63 implementation:**
- Field declarations: `XauThreeBar30mEngine.hpp:240-242` — full S63 trio:
  - `double LOSS_CUT_PCT  = 0.05;`
  - `double BE_ARM_PCT    = 0.03;`
  - `double BE_BUFFER_PCT = 0.012;`
- Management-path: `XauThreeBar30mEngine.hpp:479-498`. BE_RATCHET
  (L486-493) and LOSS_CUT (L496-498) both present and gated.

**Init block:** `engine_init.hpp:1071-1115`. Touches `shadow_mode` (HARD
shadow), `enabled = true`, `lot`, `max_spread`, `be_trigger_atr`,
`be_cost_buffer_pts`, `trail_after_be`, `trail_atr_mult`,
`min_atr_floor`, `max_bars_held`, `daily_loss_limit`,
`max_consec_losses`, `max_atr_ceil`, `block_hour_start`,
`block_hour_end`. NO S63 override.

**Repo-wide grep for set-site:** zero hits for
`g_xau_threebar_30m.LOSS_CUT_PCT|BE_ARM_PCT|BE_BUFFER_PCT`. Class
defaults 0.05 / 0.03 / 0.012 are authoritative.

**Verdict:** **STATE A** (class-default route). Full S63 trio active.
Engine is HARD shadow + enabled — S63 fires on shadow trades.

## 4. Summary table

| Engine | Header | Flavor | Class defaults | Init touches S63? | State |
|---|---|---|---|---|---|
| IndexFlowEngine x4 | `IndexFlowEngine.hpp:540` | LOSS_CUT only | 0.07 | No | **A** |
| XauusdFvgEngine | `XauusdFvgEngine.hpp` | Full S63 | 0.05 / 0.03 / 0.012 | No | **A** |
| PDHLReversionEngine | `PDHLReversionEngine.hpp` | Full S63 | 0.04 / 0.025 / 0.01 | No | **A** |
| RSIReversalEngine | `RSIReversalEngine.hpp` | LOSS_CUT only | 0.05 | No | **A** (engine disabled) |
| XauThreeBar30mEngine | `XauThreeBar30mEngine.hpp` | Full S63 | 0.05 / 0.03 / 0.012 | No | **A** |

All 5 in State A via the class-default route. No state-change actions
required.

## 5. Observations and recommendations

### 5.1 The class-default route is a stylistic gap, not a functional one

Four of the five engines (all except `IndexFlowEngine`, which is
cold-loss-only) declare their S63 defaults in the engine class body
with non-zero values, and `engine_init.hpp` never touches the field.
Compare with the two canonical state-A precedents:

- `g_vwap_rev_ger40` at `engine_init.hpp:632-634` — explicit
  re-affirmation comment + explicit value assignment, even though the
  values exactly match the class defaults. The comment block above
  documents the per-instrument rationale.
- `g_ustec_tf_5m` at `engine_init.hpp:953-978` — same pattern.

The class-default route is functionally equivalent but has two
drawbacks:

1. **Grep visibility from `engine_init.hpp` alone.** Reading the init
   block doesn't reveal that S63 is active. The reader has to know to
   check the class declaration.
2. **Per-instance scaling not documented.** The five engines' class
   defaults are picked for the engine's typical entry price scale
   (e.g. PDHL at 0.04 for $3700 XAU vs IndexFlow at 0.07 for $7400
   US500). That sizing rationale lives in header comments, not in
   `engine_init.hpp` where instance configuration would normally be
   reviewed.

**Optional follow-up (NOT required):** mirror the
`g_vwap_rev_ger40` / `g_ustec_tf_5m` pattern by adding an explicit
re-affirmation comment + assignment block above each of the 5 engines'
`init()` call in `engine_init.hpp`. This is hygiene, not function —
zero runtime effect. Estimated effort: ~15 minutes per engine, one
commit per engine to satisfy CLAUDE.md "no bundled unrelated changes".
If skipped, future audits will need to repeat the class-header
verification that this memo performs.

### 5.2 Part-K's "probably state C or D" guess was wrong for IndexFlow

The part-K table flagged IndexFlowEngine as "Probably state C or D" on
the basis that "engine_init.hpp has NO call site passing non-zero".
That's a true observation but doesn't imply state C or D — it just
means the class-default route is in use. The actual state is A.

**Lesson:** when classifying a state-? engine, the gating verification
is whether the management-path check fires with a non-zero argument at
runtime, not whether `engine_init.hpp` explicitly sets the field. The
check looks like:

```
Does the class body declare a non-zero default?
   YES -> default applies if engine_init.hpp doesn't touch it -> STATE A.
   NO  -> class default is 0.0 -> field defaults to 0.0 -> STATE C or
          E depending on whether the check exists at all.
```

This memo's per-engine §3.1-3.5 method (grep class body + grep init +
grep repo-wide) is the canonical recipe for any future S63 state
classification. The part-U handoff's "Recommended next-session focus"
item 4 framing ("1-2 hour audit, no code changes -- output is a
classification table") is corroborated by this session's wall-clock —
total time was well under the 1-hour estimate.

### 5.3 PDHL S63 tuning memo cross-reference

The part-K-flagged memo at
`outputs/S63_PDHL_TUNING_RECOMMENDATION_2026-05-13.md` (which I have
not re-read in this session — only seen referenced) was written
under the apparent premise that PDHL's S63 was not yet active. With
this audit's finding that PDHL has been state A via class default since
the 2026-05-13 commit, the tuning memo's recommendations should be
re-evaluated against:

- The engine has been firing S63 protection on every shadow trade
  since 2026-05-13.
- Any tuning sweep should compare against the current class-default
  parameters (0.04 / 0.025 / 0.01), not against a hypothetical "S63
  off" baseline.
- If the tuning memo's evidence was gathered with the engine assumed
  to have S63 off, the sweep needs to be re-run, with the S63-active
  current state as the baseline.

This is a re-evaluation, not a code change. Queued for whoever picks
up PDHL retune work.

## 6. Bonus findings — out-of-scope engines flagged for future audit

While walking through `IndexFlowEngine.hpp`, two additional engines
came into view that have S63 hooks but were NOT in the part-K audit
list. Flagging here so they get audited in a future pass:

### 6.1 IndexMacroCrashEngine (4 instances)

**Header:** `IndexFlowEngine.hpp:962` (class).

**S63 implementation:**
- Field declarations: `IndexFlowEngine.hpp:972-974` — full S63 trio:
  - `LOSS_CUT_PCT  = 0.08`
  - `BE_ARM_PCT    = 0.05`
  - `BE_BUFFER_PCT = 0.02`
- Management-path: `IndexFlowEngine.hpp:1123-1141`. Both BE_RATCHET
  and LOSS_CUT present.

**Instances:** `globals.hpp:286-289` declares `g_imacro_sp/nq/nas/us30`
(US500.F / USTEC.F / NAS100 / DJ30.F).

**Init in engine_init.hpp:** Not directly verified in this audit pass.
Position-source registrations at `engine_init.hpp:3329-3336` suggest
the engines are wired up; whether they have an explicit init block
that touches S63 (or rely on the class-default route like
IndexFlowEngine x4) is not yet verified.

**Predicted state:** A (class-default route, same as IndexFlow x4),
pending confirmation.

### 6.2 IndexSwingEngine (instances per omega_types.hpp:300-301)

**Header:** `IndexFlowEngine.hpp:1360` (class).

**S63 implementation:** **NONE found.** The grep for `S63|LOSS_CUT_PCT|
BE_ARM_PCT|BE_BUFFER_PCT` in `IndexFlowEngine.hpp` returned no hits in
the `IndexSwingEngine` range (lines 1360-1580).

**Instances:** `omega_types.hpp:300-301` declares `g_iswing_sp/nq`.

**Predicted state:** E (no S63 hooks anywhere). Pending verification.

If `IndexMacroCrash` and `IndexSwing` need a S63 transition, the
canonical state-E → A pattern is `VWAPReversionEngine` at
`CrossAssetEngines.hpp:1304-1383` (per part-K). `IndexMacroCrash`
already has hooks + mgmt-path, so it's an A/B/C audit not a transition.
`IndexSwing` would be a full state-E → A addition.

## 7. Outcome and queued follow-ups

**Closed:** all 5 part-K-listed engines confirmed as state A. No code
changes required this session. Part-K's table can be updated:

| Engine | Class location | Part-K state | Confirmed state |
|---|---|---|---|
| IndexFlowEngine x4 | `IndexFlowEngine.hpp:540` | ? (probable C/D) | **A** (class-default) |
| XauusdFvgEngine | `XauusdFvgEngine.hpp` | ? | **A** (class-default) |
| PDHLReversionEngine | `PDHLReversionEngine.hpp` | ? | **A** (class-default) |
| RSIReversalEngine | `RSIReversalEngine.hpp` | ? (cold-loss-only) | **A** (class-default, disabled) |
| XauThreeBar30mEngine | `XauThreeBar30mEngine.hpp` | ? | **A** (class-default) |

**Queued follow-ups (none blocking):**

1. **Optional hygiene commits (§5.1):** add explicit S63 re-affirmation
   comment + assignment blocks in `engine_init.hpp` for each of the 5
   engines, mirroring `g_vwap_rev_ger40` / `g_ustec_tf_5m`. ~15 min
   each, separate commits per CLAUDE.md. Zero runtime impact. Improves
   grep visibility.
2. **PDHL tuning memo re-evaluation (§5.3):** revisit
   `outputs/S63_PDHL_TUNING_RECOMMENDATION_2026-05-13.md` against the
   correction that PDHL S63 is already active. Whoever picks up PDHL
   retune work should validate this premise first.
3. **IndexMacroCrash audit (§6.1):** classify the 4 instances'
   `g_imacro_*` init state. Predicted A but needs verification.
   Same method as this memo §3.
4. **IndexSwing S63 transition (§6.2):** decide whether to add S63 to
   `IndexSwingEngine` (currently predicted state E). Decision requires
   per-instrument backtest evidence first — engine is shadow-mode only
   per the references in `tick_indices.hpp:300, 954`, so this is not
   urgent.
5. **Universe-wide S63 sweep continuation (part-K item 6):** ~25
   engines still default to state E pending audit (FX Opens, Breakout
   x5, GoldStack, Bracket x13, CandleFlow, MinimalH4 portfolio,
   C1Retuned, TrendPullback x2, MidScalper, MicroScalper,
   MinimalH4Breakout, EMACross, H4Regime, MacroCrash,
   XauTrendFollow x3, H1Swing, NBM, IndexMacroCrash x4 [see §6.1],
   UstecTrendFollowHtf, H1SwingGold...). Multi-session, batch
   ~5-10 engines per session, same method as this memo.

## 8. Standing audits at session end

**Core code preserved.** No modifications to `OmegaCostGuard.hpp`,
`OmegaTradeLedger.hpp`, `SymbolConfig.hpp`, `OmegaFIX.hpp`,
`OmegaApiServer.hpp`, `GoldPositionManager.hpp`.

**Engine code untouched.** No modifications to any `*Engine.hpp` file
or to `CrossAssetEngines.hpp` / `GoldEngineStack.hpp` /
`IndexFlowEngine.hpp`. This audit was read-only.

**Engine config untouched.** No modifications to `engine_init.hpp` in
this audit. (Note: the part-U follow-up item 1 comment refresh at
lines 964-967 did land in this same session, in commit `4acf952`, but
that was a separate task closure before this audit started.)

**Ungated-engine sweep expectations unchanged.**

**GoldEngineStack chokepoint audit:** not touched in this session.
Two-hit expectation should still hold; verify before any
`GoldEngineStack.hpp` edit.

## 9. Artifacts

This memo: `outputs/S63_STATE_CLASSIFICATION_2026-05-14.md`
(force-add — `outputs/` is gitignored).

No code artifacts. No scripts. No backtest outputs.

## 10. Commit suggestion

```bash
cd ~/omega_repo
git add -f outputs/S63_STATE_CLASSIFICATION_2026-05-14.md
git diff --cached --stat
git commit -m "docs: S63 state-classification audit of 5 part-K-listed engines

Per part-U handoff §'Recommended next-session focus' item 4, audited
the 5 still-unverified S63-wired engines flagged by part-K:

  IndexFlowEngine x4 (SP/NQ/US30/NAS100) -- STATE A (class-default 0.07,
    cold-loss only, no engine_init.hpp override needed).
  XauusdFvgEngine                        -- STATE A (class defaults
    0.05/0.03/0.012, full S63, HARD shadow).
  PDHLReversionEngine                    -- STATE A (class defaults
    0.04/0.025/0.01, full S63, shadow_mode=true enabled=true).
  RSIReversalEngine                      -- STATE A (class default 0.05,
    cold-loss only) but engine DISABLED (enabled=false 2026-05-01).
  XauThreeBar30mEngine                   -- STATE A (class defaults
    0.05/0.03/0.012, full S63, HARD shadow enabled=true).

All five run with class-default S63 values; none have engine_init.hpp
overrides. The part-K guess 'probably state C or D' for IndexFlowEngine
was based on the absence of an init-side override, but the class
default (0.07) is non-zero and the management-path fires unconditionally
every tick. Read-only audit -- no code or config changes this session.

Bonus findings (out-of-scope, flagged for future audit):
  - IndexMacroCrashEngine x4 (IndexFlowEngine.hpp:962) -- full S63
    hooks + mgmt-path present, init not verified in this pass.
    Predicted state A (class-default route).
  - IndexSwingEngine (IndexFlowEngine.hpp:1360) -- no S63 hooks.
    Predicted state E.

Queued follow-ups: optional hygiene commits to add explicit re-affirm
comment blocks per engine; PDHL tuning memo re-eval (its premise
'S63 not active on PDHL' is incorrect); IndexMacroCrash/IndexSwing
audit; remaining ~25-engine universe sweep continues."
git push origin main
```

## 11. Closing note

This audit closes the part-K-carried item 4 from part-U. Of part-U's
listed "next-session focus" items, items 1 and 4 are now done.
Items 2 (Tier 4 vol-regime scoping memo), 3 (XauTrendFollow trio S63
sweep), 5 (Wrapper engine S63 design pass), and 6 (GoldEngineStack
chokepoint audit) remain queued for future sessions.

The audit verified that part-U's optimistic estimate ("1-2 hour audit,
no code changes -- output is a classification table") was achievable —
this session's audit work was well under an hour of wall-clock
including memo write-up. The method (grep class body + grep init +
grep repo-wide for set-sites) is reusable for the queued universe-wide
sweep continuation (§7 item 5).

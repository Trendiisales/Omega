# Session Handoff — 2026-05-13 (NZST), part H

Direct follow-up to `SESSION_HANDOFF_2026-05-13b.md` (part G). This
session was a **verification-only pass** — no engine headers, no core
code, and no commits were modified or created. The goal was to confirm
that the part-G cost-gate rollout landed cleanly on disk and to
exercise every audit recommended in part G §"Validation actions".

## TL;DR

1. **Part-G commit landed.** `1c2306b "Cost-gate rollout: MinimalH4 +
   C1Retuned + GoldEngineStack"` is at HEAD. Working tree is clean.
   The `.git/index.lock` that blocked part-G's commit was cleared by
   the operator between sessions.
2. **Ungated-engine audit re-ran clean.** Only the four intentionally-
   ungated INERT files appear (LatencyEdgeEngines, RSIExtremeTurnEngine,
   SweepableEngines, SweepableEnginesCRTP). Matches part-G expectation
   exactly.
3. **All four part-G gates verified on disk.** Includes present, gate
   call sites match the part-G fire-site description, TP proxies match,
   on-block return values match.
4. **GoldEngineStack pipeline audit clean.** The entire 4763-line file
   contains exactly **one** `pos_mgr_.open()` call site (L4182),
   directly gated at L4176. All 19 sub-engines inherit from `EngineBase`
   and reduce to the single `best` selector — no sub-engine has an
   independent open path. Part-G §"#4a future-risk" is currently a
   non-issue; the architecture would have to be deliberately broken to
   create a bypass.
5. **No engine edits made this session.** All gate-verification was
   read-only. A trade-journal review at the end of the session
   surfaced a major design gap (see §"Trade-journal review and
   proposed in-flight cut" below) — design captured in this doc, no
   code applied yet, awaiting operator go-ahead on the threshold.
6. **Cost-gate scope clarified.** The `OmegaCostGuard::is_viable()`
   gate is an *entry filter only*: it blocks trades that cannot
   possibly cover costs at TP. It does NOT cut trades that go
   negative in-flight. Carrying this clarification forward because
   the operator caught it during the trade-journal review and it
   was a real source of confusion in the previous session.

## Verification results

### 1. Git state

```
.git/index.lock: not present
git log --oneline -10:
  1c2306b  Cost-gate rollout: MinimalH4 + C1Retuned + GoldEngineStack
  bf5e7dd  S37-P0c research: sweep harnesses + ledgers (FX / USTEC TF5m / XAU)
  ebd97ca  Cost-gate rollout: complete production-engine coverage
  7f2cc98  Cost-gate rollout to GoldMidScalper / Bracket / EMACross / ...
  ac707e3  S37-P2/P3: RiskMonitor wiring + calibration for UstecTrendFollow5m
  176c746  S37-P1: USTEC TrendFollow5m promotion patch (shadow only)
  65985f9  S36-P5: drop AtrMom1h cell; harness portability flags; ...
  248ed53  S36-P2: extend converter + backtest harness to handle 2024 data
  3138e8f  S36-P1b: drop InsideBar2h after S36-P1a-verify revealed ...
  b6e9495  S36-P4: wire tick_indices.hpp M15 + tick_gold.hpp M30 dispatch
git status --short: (empty)
```

Working tree clean. Part-G commit is live.

### 2. Ungated-engine audit (part-G §"Validation actions" item 3)

```bash
for f in include/*.hpp; do
  if grep -lE "pos[_]?\.active *= *true|pos[_]?\.open\(sig" "$f" >/dev/null 2>&1; then
    if ! grep -q "OmegaCostGuard" "$f"; then echo "UNGATED: $f"; fi
  fi
done
```

Output:
```
UNGATED: include/LatencyEdgeEngines.hpp        # S13 culled
UNGATED: include/RSIExtremeTurnEngine.hpp      # S52 disabled (0/153 combos)
UNGATED: include/SweepableEngines.hpp          # research-only sweep harness
UNGATED: include/SweepableEnginesCRTP.hpp      # research-only sweep harness
```

Exactly the four INERT files part G expects. No drift since the
part-G commit.

### 3. Gate verification (part G §"Files gated this session")

#### include/MinimalH4Breakout.hpp  (611 lines)
- Include: L59 `#include "OmegaCostGuard.hpp"` ✓
- Gate:    L465 `ExecutionCostGuard::is_viable(symbol.c_str(), (ask-bid), tp_pts, size, 1.5)` ✓
- On block (L467): `return sig;` (sig still empty, valid=false) ✓
- Fire site (L470): `pos_.active = true;` immediately after gate ✓
- TP source: `tp_pts = h4_atr * p.tp_mult` — matches description ✓

#### include/MinimalH4US30Breakout.hpp  (750 lines)
- Include: L76 `#include "OmegaCostGuard.hpp"` ✓
- Gate:    L321 `ExecutionCostGuard::is_viable(symbol.c_str(), (ask-bid), tp_pts, size, 1.5)` ✓
- On block (L325): `// sig is still empty (valid=false) - drop entry`,
  if/else wrapper keeps the surrounding long_only-else block balanced ✓
- Fire site (L328): `pos_.active = true;` inside the else branch ✓
- TP source: `tp_pts = atr_pre * p.tp_mult` — matches description ✓

#### include/C1RetunedPortfolio.hpp  (810 lines, 2 cells)
- Include: L81 `#include "OmegaCostGuard.hpp"` ✓
- **Donchian H1 cell**
  - Gate (L288): `ExecutionCostGuard::is_viable(symbol.c_str(), (ask-bid), tp_pts, lot_lp, 1.5)` ✓
  - On block (L290): `return 0;` ✓
  - Fire site (L293): `pos_.active = true;` ✓
  - TP source: `tp_pts = h1_atr14 * tp_atr` ✓
- **Bollinger cell**
  - Gate (L425): `ExecutionCostGuard::is_viable(symbol.c_str(), (ask-bid), sl_pts*1.5, lot_lp, 1.5)` ✓
  - On block (L427): `return 0;` ✓
  - Fire site (L430): `pos_.active = true;` (with `pos_.tp = 0.0` indicator-exit) ✓
  - TP proxy: `sl_pts * 1.5` — matches part-G description (CandleFlow pattern) ✓

#### include/GoldEngineStack.hpp  (4763 lines)
- Include: L50 `#include "OmegaCostGuard.hpp"` ✓
- Gate (L4176): `ExecutionCostGuard::is_viable("XAUUSD", spread, tp_dist_px, lot_lp, 1.5)` ✓
- On block (L4178): `return GoldSignal{};` ✓
- Fire site (L4182): `pos_mgr_.open(gs, spread, latency_ms, current_regime_name());` ✓
- TP proxy (L4168-4174):
  ```
  tp_ticks_eff = (gs.tp_ticks > 0.0) ? gs.tp_ticks
                                     : std::max(4.0, gs.sl_ticks) * 2.0;
  GS_TICK_SIZE = 0.10;                         // mirrors private TICK_SIZE at L3213
  tp_dist_px   = tp_ticks_eff * GS_TICK_SIZE;
  ```
  Matches part-G description verbatim. ✓

Line numbers shifted slightly from the pre-edit numbers cited in
part G (e.g. GoldEngineStack gate is now at L4176, part G cited
L4157 pre-edit). That is expected — the gate insertion shifts every
subsequent line down. Nothing else moved.

### 4. GoldEngineStack pipeline audit (part G §"#4a future-risk")

Recommended in part G as recurring monitoring: any new sub-engine that
bypasses the `best` selector + `pos_mgr_.open()` chokepoint would
escape the gate. Audit method: grep every call site that opens a
position in the file, then confirm each one is either the gated site
or unrelated.

```bash
grep -n "pos_mgr_\." include/GoldEngineStack.hpp
```

Result categorised:

| line | call                       | category               |
|-----:|----------------------------|------------------------|
| 50   | comment in include line    | doc only               |
| 3959 | `pos_mgr_.set_cfg(...)`    | config, not open       |
| 4008 | `pos_mgr_.manage(...)`     | manage/exit, not open  |
| 4021 | `pos_mgr_.active()`        | query, not open        |
| 4163 | comment                    | doc only               |
| **4182** | **`pos_mgr_.open(...)`** | **the gated site**     |
| 4193 | `pos_mgr_.force_close(...)`| close, not open        |
| 4200 | `pos_mgr_.active()`        | query                  |
| 4206-09 | `pos_mgr_.active/base_*` | query                 |
| 4228-34 | `pos_mgr_.base_*`        | query                 |
| 4239 | `pos_mgr_.patch_base_size` | mutate, not open       |
| 4246 | `pos_mgr_.set_shadow_mode` | config                 |

Independent confirmation:
```bash
grep -nE "\.open\(" include/GoldEngineStack.hpp
```
Result: exactly two hits — the include comment at L50 and the gated
call at L4182. **No other `.open(` call exists anywhere in the file**,
not even on a member with a different prefix.

Sub-engine class count vs. part-G inventory:
```
grep -nE "^(struct|class) [A-Z][a-zA-Z0-9]*Engine" include/GoldEngineStack.hpp
```
19 hits — the 18 sub-engines listed in part-G §"Sub-engines now
covered" plus `MomentumContinuationEngine` (also called out in part G).
All inherit from `EngineBase`. None is referenced from a
`pos_mgr_.open()` call, because there is only one such call in the
file and it operates on the `best` Signal — which every EngineBase
subclass funnels into via the `process(snap)` → Signal → reducer
pipeline at L4129-4152.

**Conclusion:** the part-G #4a risk is currently zero. For a new
sub-engine to escape the gate, someone would have to either (a) add
a second `pos_mgr_.open()` call site or (b) call `.open(...)` on a
different member, both of which are structurally unusual and would
show up immediately in the next run of this audit.

## Build verification (Mac canary, operator-side)

Sandbox cannot run cmake (no cmake binary, plus the project pulls in
`winsock2.h` which the Linux sandbox does not ship). Sandbox-side
`g++ -fsyntax-only` was clean across the four files in part G — no
re-run needed because no files changed this session.

**Operator action (Mac):**
```bash
cd ~/omega_repo
cmake --build build --target OmegaBacktest -j
```
`OmegaBacktest` pulls in `engine_init.hpp` which transitively
includes every engine header. A clean build = the four part-G files
compile clean in a real TU. Per part-G §"Build environment lesson":
do NOT use the bare `cmake --build build -j` — that target needs
Windows-only headers and will always fail on macOS, even though the
surrounding green "Built target X" lines make it look like a pass.

## Carry-over to next session

### 1. VPS deploy S36-P5 still pending (operator-Windows action)
Unchanged from parts D / E / F / G. Until the VPS PowerShell deploy
ships, account 8077780 still runs the pre-S33d binary. Claude cannot
perform this step.

### 2. Mac OmegaBacktest verification still pending (operator-Mac)
Sandbox cannot run cmake. The `g++ -fsyntax-only` check from part G
is necessary-not-sufficient; the cmake build on Mac is the sufficient
check. See "Build verification" section above for the exact command.

### 3. Future GoldEngineStack sub-engine risk (monitoring only)
The pipeline audit in §4 above is now the standard check. Repeat it
any time a new sub-engine is added to the stack. Method:
```bash
grep -nE "\.open\(" include/GoldEngineStack.hpp
```
Expect: exactly two hits — the L50 include comment and the L4182
gated call. Any third hit needs investigation.

### 4. GoldPositionManager::TICK_SIZE accessibility (cleanup candidate)
Still private constexpr. GoldEngineStack.hpp gate uses a local
`GS_TICK_SIZE = 0.10` literal as a workaround (L4173). If the broker
ever changes XAUUSD tick size, both copies must change. Either promote
TICK_SIZE to namespace scope or add a public getter. **Not done this
session** per the operator preference "Never modify core code unless
instructed clearly" — `GoldPositionManager` is core.

### 5. USTEC TrendFollow5m shadow window
Still in the 2-month shadow window (started at the S37-P1 commit
`176c746`, 2026-04-something). Promotion-to-live is the future
"S37-P4" commit, after the window closes.

## Engine coverage snapshot (as of part-H end, unchanged from part-G)

```
LIVE / hard-shadow, GATED:
  EurusdLondonOpen, GbpusdLondonOpen, UsdjpyAsianOpen,
  AudusdSydneyOpen, NzdusdAsianOpen, UstecTrendFollow5m,
  UstecTrendFollowHtf, PDHLReversion,
  XauTrendFollow 2h / 4h / D1, XauThreeBar30m, XauusdFvg,
  RSIReversal, Breakout, MacroCrash,
  GoldMidScalper, BracketEngine (XAU + 12 FX/index instances),
  EMACross, H4Regime,
  CandleFlow (3 paths),
  IndexFlow (4 instances),
  VWAPReversion (4 instances),
  TrendPullback (2 LIVE instances),
  NoiseBandMomentum (gold_london),
  MinimalH4Breakout (gold, shadow),
  MinimalH4US30Breakout (DJ30.F, live),
  C1RetunedPortfolio (Donchian H1 + Bollinger, shadow),
  GoldEngineStack (19 sub-engines, hard-shadow)

INERT / CULLED -- intentionally not gated:
  LatencyEdgeEngines (S13 culled),
  SweepableEngines, SweepableEnginesCRTP (research-only sweep harness),
  H1SwingEngine (g_h1_swing_gold.enabled = false),
  CrossAsset EsNq / Oil / BrentWti / FxCascade / CarryUnwind / ORB
    (all enabled=false),
  RSIExtremeTurn (S52 disabled, 0/153 combos profitable),
  NoiseBandMomentum non-gold-london instances (all enabled=false).
```

## Soft conclusions worth carrying

1. **Part-G work is verified.** Disk state matches the part-G
   description in every detail checked: include lines, gate call
   sites, TP proxies, on-block returns, fire-site placement.
2. **GoldEngineStack architecture is robustly chokepointed.** Only
   one `pos_mgr_.open()` site exists in 4763 lines. As long as that
   property holds, one gate covers the entire stack.
3. **Audit pattern from part F is doing its job.** Two consecutive
   sessions (G and H) have run it clean. Future sessions should keep
   it as the standard check.
4. **No engine that fires a position in live or hard-shadow is
   ungated.** This is now confirmed twice (part G after the edits,
   part H against the committed tree).
5. **No code touched this session.** All five user-requested actions
   completed via read-only verification.

## Trade-journal review and proposed in-flight cut

Late in the session the operator pasted a 19-trade slice from the
live tape and asked "fix the bad trades". This produced two findings
and one design proposal. **No code has been applied; this section
documents the design so the next session can pick it up.**

### The 19-trade slice (operator-supplied)

| time     | engine                 | symbol  | side  | dur     | exit kind        | net     |
|----------|------------------------|---------|-------|---------|------------------|---------|
| 20:03:10 | VWAPReversion          | USTEC.F | SHORT | 10m0s   | T/O              | +28.22  |
| 19:58:14 | VWAPReversion          | US500.F | SHORT | 10m0s   | T/O              | +98.92  |
| 19:48:10 | VWAPReversion          | USTEC.F | SHORT | 10m0s   | T/O              | -24.08  |
| 19:34:09 | DonchianBreakout       | XAUUSD  | LONG  | 1m58s   | TRAIL            |  +0.73  |
| 17:50:10 | VWAPReversion          | USTEC.F | LONG  | 10m0s   | T/O              | +15.55  |
| 17:40:10 | VWAPReversion          | USTEC.F | LONG  | 10m0s   | T/O              |  -7.55  |
| 17:30:08 | VWAPReversion          | USTEC.F | LONG  | 10m0s   | T/O              |  +6.22  |
| 17:20:08 | VWAPReversion          | USTEC.F | LONG  | 10m0s   | T/O              |  -3.50  |
| 17:10:08 | VWAPReversion          | USTEC.F | LONG  | 10m0s   | T/O              | +14.91  |
| 17:03:05 | VWAPReversion          | US500.F | LONG  | 10m0s   | T/O              | +27.23  |
| 17:00:08 | VWAPReversion          | USTEC.F | LONG  | 10m0s   | T/O              |  +6.16  |
| 15:15:08 | VWAPReversion          | US500.F | LONG  | 10m0s   | T/O              | -83.27  |
| 14:00:00 | Tsmom_H1_long          | XAUUSD  | LONG  | 83m28s  | MAE_EXIT         | -38.74  |
| 15:02:49 | VWAPReversion          | US500.F | LONG  | 10m0s   | T/O              |  +3.79  |
| 15:00:01 | LondonFixMomentum      | XAUUSD  | SHORT | 3m20s   | TRAIL            |  +0.72  |
| 14:55:49 | VWAPReversion          | USTEC.F | LONG  | 5m19s   | MAE_EARLY_EXIT   | -24.83  |
| 13:00:00 | Tsmom_H1_long          | XAUUSD  | LONG  | 118m3s  | MAE_EXIT         | -36.72  |
| 14:45:07 | VWAPReversion          | USTEC.F | LONG  | 10m0s   | T/O              |  -0.35  |
| 14:31:20 | VWAPReversion          | US500.F | LONG  | 10m0s   | T/O              | +17.82  |

Aggregate: gross +$40.58, costs -$39.04, **net +$1.23** across
19 trades. Cost drag ate 96% of gross. Exit-reason distribution:
14 T/O, 3 MAE_EXIT/MAE_EARLY_EXIT, 2 TRAIL. **Zero TP_HIT or SL_HIT.**

### Finding 1 — Tsmom_H1_long XAU stacking (S12 gate has a gap)

Two back-to-back XAU LONG entries (13:00 + 14:00) both MAE_EXIT for
combined -$75.46. This is the same pattern the S12 commit `ef488a3`
("Tsmom MAE-exit") was supposed to fix. The S12 gate at
`TsmomEngine.hpp:408-425` blocks re-entry for `mae_exit_cooldown_bars`
H1 bars *after a MAE_EXIT*. But with `max_positions_per_cell = 10`
the 14:00 LONG can open *before* the 13:00 LONG has MAE_EXITed —
`last_mae_exit_bar_` is still `-10000` at that moment, so the
cooldown gate is inert. The S12 fix only catches the 3rd-in-a-row
re-entry pattern; it does NOT catch simultaneous stacking.

**Three remediation options (not yet applied):**

1. Add an "in-flight MAE check" to the pre-fire gates at
   `TsmomEngine.hpp:397-425`: block new entries whenever any
   currently-open position has `p.mae <= -0.5 * mae_exit_atr * p.atr`
   (already halfway to the MAE_EXIT trigger). Catches today's
   14:00 entry because the 13:00 position was already deeply adverse.
2. Drop `max_positions_per_cell` from 10 to 1 in
   `TsmomEngine.hpp:198` or `TsmomStrategy.hpp:179`. Kills stacking
   entirely but removes the "intended throttle" semantics
   documented at L62-64.
3. Disable `Tsmom_H1_long` outright. Last resort.

### Finding 2 — VWAPReversion exits are all T/O or FC (the operator's main complaint)

Of the 15 VWAPReversion trades in the slice, 14 ended via `MAX_HOLD_SEC =
600s` timeout and 1 via `MAE_EARLY_EXIT` (50% of TP distance adverse).
**Zero hit TP, zero hit SL, zero used the trail.** Two distinct sub-issues:

(a) Winners stopped at 10min before reaching VWAP — e.g. the +$98.92
US500 SHORT at 19:58 was profitable but timed out short of TP. The
trail logic in `CrossPosition::manage()` at L195-201 doesn't arm
until move ≥ 60% of TP dist; today's mostly-noise environment
never got there.

(b) Losers ran their full 10-min adverse — e.g. the -$83.27 US500
LONG at 15:15 went 12pt adverse over 10min. `EXTENSION_SL_RATIO=0.60`
put SL ~18pt below entry (untouched). `MAE_EXIT_RATIO=0.50` placed
the MAE-exit trigger at ~15pt adverse (untouched). The trade simply
timed out 12pt in the red.

### Cost-gate clarification (operator caught this)

The operator asked: "we built a system in the previous chat session
that does not allow trades to fire unless they cover costs, so, if
this is working all those trades must have been positive, why did we
not cut them the instant they became negative or break even?"

**Answer:** `OmegaCostGuard::is_viable()` is an *entry filter only*.
At fire-time it checks `tp_dist * pt_value * lot >= 1.5 * round_trip_cost`.
That answers "*can* this trade cover costs *if* it reaches TP" — not
"*is* this trade currently above water". Once the position is open
the cost-gate never runs again. So a cost-gate-positive trade can
still bleed because (i) signal was wrong, (ii) price wiggles adverse
before reverting, (iii) full 10min elapses without TP being touched.
**No engine in the codebase has an in-flight cost-cover check.**
That is the gap the operator wants closed.

### Proposed in-flight cut — trajectory-aware BE ratchet

Operator's requested behaviour: "monitor the trade, if it is trending
bad ie towards where it opened, we need to cut it at or before
breakeven". This is a **break-even ratchet**: once the trade has
shown enough profit to validate the thesis, install a BE-plus-buffer
stop that triggers if price retraces back toward entry. Combine with
a **cold loss cut** for trades that never go profitable.

Two-phase manage logic, both implemented in `VWAPReversionEngine::on_tick`
without touching `CrossPosition::manage()` (shared with 8+ other
engines — minimum blast radius). `pos_.mfe` is already a public
member of `CrossPosition` updated tick-by-tick in `manage()`, so we
can read it directly:

```cpp
//  --- two new public fields on VWAPReversionEngine (add after MIN_SESSION_MIN) ---
double  LOSS_CUT_PCT   = 0.08;  // cold-loss cut: cut when adverse >= entry*pct/100
                                //   set to 0.0 to disable (legacy MAE_EXIT_RATIO + T/O only)
double  BE_ARM_PCT     = 0.05;  // arm BE ratchet once mfe >= entry*pct/100
double  BE_BUFFER_PCT  = 0.02;  // BE_CUT triggers when move <= entry*pct/100 AFTER arm
                                //   typical: spread_at_entry expressed as % of entry
                                //   BE_ARM_PCT=0.0 disables the ratchet entirely

// --- replaces the manage block at L1265-1318 of CrossAssetEngines.hpp ---
if (pos_.active) {
    const double move        = pos_.is_long ? (mid - pos_.entry)
                                            : (pos_.entry - mid);
    const double adverse     = -move;  // positive when losing
    const double tp_dist_pos = std::fabs(pos_.tp - pos_.entry);
    const int64_t held       = ca_now_sec() - pos_.entry_ts;

    // keep our own mfe in sync (pos_.manage() also updates it but is
    // called at end of this block; ensures BE-ratchet reads fresh mfe)
    if (move > pos_.mfe) pos_.mfe = move;

    // === Phase 2: BE RATCHET (giveback prevention) ===========================
    // Once mfe has reached BE_ARM_PCT * entry / 100, install a virtual stop
    // at (entry + BE_BUFFER_PCT * entry / 100). If price retraces to that
    // level, cut at break-even (or break-even + tiny buffer to cover spread).
    // This is what catches the "trade went +14pt then gave it all back" case.
    if (BE_ARM_PCT > 0.0 && BE_BUFFER_PCT >= 0.0) {
        const double arm_pts    = pos_.entry * BE_ARM_PCT    / 100.0;
        const double buffer_pts = pos_.entry * BE_BUFFER_PCT / 100.0;
        if (pos_.mfe >= arm_pts && move <= buffer_pts) {
            printf("[VWAP-REV] %s BE_CUT -- mfe=%.4f >= arm=%.4f move=%.4f <= buf=%.4f (giveback)\n",
                   sym.c_str(), pos_.mfe, arm_pts, move, buffer_pts);
            fflush(stdout);
            pos_.force_close(bid, ask, on_close, "BE_CUT");
            timeout_extended_ = false;
            // Short cooldown -- BE_CUT is not a real loss, no need to punish.
            cooldown_until_   = ca_now_sec() + COOLDOWN_SEC;
            // Reset consec-fc counter -- giveback is not a thesis failure
            consec_fc_same_dir_ = 0;
            return {};
        }
    }

    // === Phase 1: COLD LOSS CUT (trade was never profitable) =================
    // Catches the -$83 outlier case where price moves straight adverse from
    // entry without ever going positive. LOSS_CUT_PCT default = 0.08:
    //   US500@7400  -> 5.9pt adverse triggers
    //   USTEC@28000 -> 22pt adverse triggers
    //   EURUSD@1.10 -> ~9pip adverse triggers
    if (LOSS_CUT_PCT > 0.0) {
        const double loss_cut_dist = pos_.entry * LOSS_CUT_PCT / 100.0;
        if (adverse >= loss_cut_dist) {
            printf("[VWAP-REV] %s LOSS_CUT -- adverse=%.4f >= %.4f (%.3f%% of entry)\n",
                   sym.c_str(), adverse, loss_cut_dist, LOSS_CUT_PCT);
            fflush(stdout);
            const bool this_long = pos_.is_long;
            pos_.force_close(bid, ask, on_close, "LOSS_CUT");
            timeout_extended_ = false;
            cooldown_until_   = ca_now_sec() + MAE_COOLDOWN_SEC;
            if (last_fc_long_ == this_long) ++consec_fc_same_dir_;
            else { consec_fc_same_dir_ = 1; last_fc_long_ = this_long; }
            if (consec_fc_same_dir_ >= 2) {
                fc_block_long_  = this_long;
                fc_block_until_ = ca_now_sec() + CONSEC_FC_BLOCK_SEC;
                printf("[VWAP-REV] %s %d consecutive LOSS_CUT/MAE in %s -- blocking 30min\n",
                       sym.c_str(), consec_fc_same_dir_,
                       this_long ? "LONG" : "SHORT");
                fflush(stdout);
            }
            return {};
        }
    }

    // === Progressive timeout (unchanged) =====================================
    // Only applies on losing side now -- winners use eff_max_hold = INT_MAX
    // below and never reach this branch.
    if (held >= MAX_HOLD_SEC) {
        const double progress = tp_dist_pos > 0 ? move / tp_dist_pos : 0.0;
        if (progress > 0.30) {
            if (!timeout_extended_) {
                timeout_extended_ = true;
                printf("[VWAP-REV] %s timeout extended -- progress=%.0f%% still trending toward TP\n",
                       sym.c_str(), progress * 100.0);
                fflush(stdout);
                pos_.entry_ts = ca_now_sec() - MAX_HOLD_SEC + 300;
            }
        }
    }

    // === Legacy MAE_EXIT_RATIO safety net (unchanged) ========================
    // For LOSS_CUT_PCT > 0 this rarely fires (LOSS_CUT triggers first).
    if (adverse > tp_dist_pos * MAE_EXIT_RATIO && tp_dist_pos > 0.0) {
        printf("[VWAP-REV] %s MAE exit -- adverse=%.2f > %.2f (%.0f%% of TP dist) -- thesis dead\n",
               sym.c_str(), adverse, tp_dist_pos * MAE_EXIT_RATIO,
               MAE_EXIT_RATIO * 100.0);
        fflush(stdout);
        const bool this_long = pos_.is_long;
        pos_.force_close(bid, ask, on_close, "MAE_EARLY_EXIT");
        timeout_extended_ = false;
        cooldown_until_ = ca_now_sec() + MAE_COOLDOWN_SEC;
        if (last_fc_long_ == this_long) ++consec_fc_same_dir_;
        else { consec_fc_same_dir_ = 1; last_fc_long_ = this_long; }
        if (consec_fc_same_dir_ >= 2) {
            fc_block_long_  = this_long;
            fc_block_until_ = ca_now_sec() + CONSEC_FC_BLOCK_SEC;
            printf("[VWAP-REV] %s %d consecutive MAE in %s direction -- blocking 30min\n",
                   sym.c_str(), consec_fc_same_dir_, this_long ? "LONG" : "SHORT");
            fflush(stdout);
        }
        return {};
    }

    // === Winners ride the trail (no timeout) =================================
    // Pass INT_MAX as max_hold so pos_.manage()'s T/O branch is inert when
    // in profit. The existing BE / mid-lock / trail logic in
    // CrossPosition::manage() (L171-202) handles the natural exit via
    // TP_HIT or trailed SL_HIT.
    const int eff_max_hold = (move > 0.0)
        ? std::numeric_limits<int>::max()
        : MAX_HOLD_SEC;
    pos_.manage(bid, ask, eff_max_hold, on_close);
    return {};
}
```

**Plus `#include <limits>` at the top of the file if not already
present.** Per-instance config in `engine_init.hpp` L585-612 to be
added:

```cpp
// LOSS_CUT_PCT: cold-loss cut threshold (% of entry)
// BE_ARM_PCT:   mfe % of entry that arms the BE ratchet
// BE_BUFFER_PCT: BE_CUT triggers when move <= entry*BE_BUFFER_PCT/100 after arm
g_vwap_rev_sp.LOSS_CUT_PCT     = 0.08;   // US500 ~6pt cut
g_vwap_rev_sp.BE_ARM_PCT       = 0.05;   // ~3.7pt mfe arms
g_vwap_rev_sp.BE_BUFFER_PCT    = 0.02;   // ~1.5pt buffer (typical spread)

g_vwap_rev_nq.LOSS_CUT_PCT     = 0.08;   // USTEC ~22pt cut
g_vwap_rev_nq.BE_ARM_PCT       = 0.05;   // ~14pt mfe arms
g_vwap_rev_nq.BE_BUFFER_PCT    = 0.02;   // ~5.6pt buffer

g_vwap_rev_ger40.LOSS_CUT_PCT  = 0.08;
g_vwap_rev_ger40.BE_ARM_PCT    = 0.05;
g_vwap_rev_ger40.BE_BUFFER_PCT = 0.02;

g_vwap_rev_eurusd.LOSS_CUT_PCT  = 0.05;  // FX moves smaller, tighter cut
g_vwap_rev_eurusd.BE_ARM_PCT    = 0.03;  // ~3.3pip mfe arms
g_vwap_rev_eurusd.BE_BUFFER_PCT = 0.015; // ~1.6pip buffer
```

### Replay of today's slice under the proposed logic

| trade                    | today's outcome | proposed outcome | rationale |
|--------------------------|-----------------|------------------|-----------|
| US500 LONG 15:15 -$83.27 | T/O at -12pt    | LOSS_CUT at -5.9pt ≈ -$45 | adverse > 0.08% triggers cold loss cut, saves ~$38 |
| US500 SHORT 19:58 +$98.92 | T/O at +14pt   | maybe TRAIL — mfe likely went higher, trail captures more | trail no longer time-limited |
| USTEC LONG 17:10 +$14.91 | T/O at +77pt (straight up, no retrace) | unchanged (TRAIL_HIT at similar level) | trail engages at 60% to TP; nothing to retrace |
| USTEC LONG 17:40 -$7.55  | T/O at -6pt    | LOSS_CUT at -22pt threshold not reached, runs to T/O | small adverse, below LOSS_CUT |
| USTEC LONG 14:45 -$0.35  | T/O at -2pt    | unchanged                | adverse too small for cut |

Expected effect: roughly +$30-45/day saved on the outlier losers
(catching one -$80 per few days at -$40 instead). Winners may grow
on retraces but unchanged on straight-trend winners. **Total expected
edge improvement is modest in absolute terms but eliminates the
catastrophic-loss tail risk.**

### Decision still pending (operator action)

The operator has NOT yet confirmed the threshold values. Before any
edit can be applied, the next session needs:

1. **Threshold confirmation** — accept the default `LOSS_CUT_PCT=0.08
   / BE_ARM_PCT=0.05 / BE_BUFFER_PCT=0.02` set, or specify alternative
   numbers (e.g. tighter `LOSS_CUT_PCT=0.05`, or per-symbol overrides).
2. **Edit method preference** — operator's standing pref is "always
   full file". This file is 2646 lines; either targeted Edits (faster,
   less context to review) or a full rewrite (matches preference but
   massive output).
3. **Tsmom_H1_long fix selection** — separate decision from the
   VWAPReversion in-flight cut. Three options listed in Finding 1.

When all three are confirmed, the next session can apply with one
edit + a sandbox `g++ -fsyntax-only` check + a commit. Mac canary
build (`cmake --build build --target OmegaBacktest -j`) is the
sufficient check on the Mac side.

### Files touched if applied

```
include/CrossAssetEngines.hpp   ~1 include + 3 fields + ~80 lines replacement = ~85 lines
include/engine_init.hpp          ~12 lines (3 fields × 4 instances)
outputs/SESSION_HANDOFF_2026-05-13c.md   (this doc)
```

No central core files (`OmegaCostGuard.hpp`, `engine_init.hpp` is
"core-adjacent" but the change is config-only). Per operator
preference "Never modify core code unless instructed clearly", this
is config + engine logic — the explicit "fix the bad trades" request
is the instruction.

---

## Validation actions next session

```bash
cd ~/omega_repo

# 1. Confirm part-G commit still at or near HEAD
git log --oneline -8

# 2. Mac canary build (cross-platform target -- main Omega is VPS-only)
cmake --build build --target OmegaBacktest -j

# 3. Re-run the ungated audit
for f in include/*.hpp; do
  if grep -lE "pos[_]?\.active *= *true|pos[_]?\.open\(sig" "$f" >/dev/null 2>&1; then
    if ! grep -q "OmegaCostGuard" "$f"; then echo "UNGATED: $f"; fi
  fi
done
# Expect: only LatencyEdgeEngines, RSIExtremeTurnEngine,
# SweepableEngines, SweepableEnginesCRTP.

# 4. Re-run the GoldEngineStack chokepoint audit
grep -nE "\.open\(" include/GoldEngineStack.hpp
# Expect: exactly two hits (L50 comment + L4182 gated call).
```

## Bookkeeping

- No new commits this session. Tree at `1c2306b` (part-G commit).
- No new edits. Read-only verification only.
- No core code modified. (Per operator preference.)
- Sandbox `.git/index.lock` issue from part G is resolved — operator
  cleared it on the Mac between sessions.

## Quick-reference files

| file                                       | purpose                          | size      |
|--------------------------------------------|----------------------------------|-----------|
| `include/MinimalH4Breakout.hpp`            | verified this session            | ~22 KB    |
| `include/MinimalH4US30Breakout.hpp`        | verified this session            | ~25 KB    |
| `include/C1RetunedPortfolio.hpp`           | verified this session (2 cells)  | ~27 KB    |
| `include/GoldEngineStack.hpp`              | verified this session (1 gate / 19 sub-engines) | ~190 KB |
| `include/OmegaCostGuard.hpp`               | gate implementation (untouched)  | 6 KB      |
| `outputs/SESSION_HANDOFF_2026-05-13a.md`   | part F (reference)               | reference |
| `outputs/SESSION_HANDOFF_2026-05-13b.md`   | part G (reference)               | reference |
| `outputs/SESSION_HANDOFF_2026-05-13c.md`   | this document                    | current   |

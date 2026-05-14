# Session Handoff — 2026-05-14 (NZST), part AB

Read this first next session. Direct follow-up to part-AA
(`SESSION_HANDOFF_2026-05-14p.md`). Long, multi-stream session that
reopened after part-AA closed: live-trade-log analysis surfaced a real
engine bug (S86 hotfix), a load-bearing VPS git-state divergence (P0
finding driving the new Deploy Hygiene section in CLAUDE.md), and a
clean investigation result on the UstecTrendFollow5m +$728 trade that
queues the next session's primary work.

> **Naming.** File letter `q` for today's 17th session. Verbal "part AB"
> continues from AA.

## TL;DR

1. **S86 hotfix landed** (commit `f7c18f7`). `VWAPReversionEngine`
   TIMEOUT-aware consec-FC tracking. Closes a hole in an existing
   protection. Live-bug evidence from this session: VWR on US500.F
   fired **9 consecutive SHORT trades** against an uptrend on 10-min
   cadence (14:42 → 16:02 UTC 5/14), all exiting via TIMEOUT, with the
   30-min direction-block never engaging because TIMEOUT exits don't
   increment `consec_fc_same_dir_`. Net −$147 for one engine on one
   day. With S86, trades 3-9 of that pattern would have been blocked
   after the second consecutive same-direction TIMEOUT-loss. Symmetric
   fix across all four VWR instances.

2. **CRITICAL P0 FINDING — VPS git state was three-way divergent**:
   working tree HEAD detached at `18b62c8` (S24, late April), running
   binary `491fd94` (S81, built 5/14 06:49 UTC), origin/main at
   `f7c18f7` (S86, this session's hotfix). Multiple stop-bleed commits
   over the prior week (S68, S82, others) had been shipped under the
   assumption "commit + push + operator-side rebuild = it's live", but
   the working tree never moved off S24 after the 2026-05-10 broken-
   build cycle. Root cause: `Omega.exe.broken_65d91b4_*` and
   `Omega.exe.broken_9bc02f9_*` failed builds on 5/10, operator
   reverted to S24 (`Omega.exe.working_18b62c8_20260510_*`) via
   detached checkout, the detached state persisted while the binary
   was rebuilt from a different state at S81 four days later. The
   protection actually-in-production for the past week was
   unverifiable. **Caught only because the S86 deploy attempt
   triggered the check.**

3. **CLAUDE.md Deploy Hygiene section landed** (commit `571641d3`).
   Codifies session-start hash-alignment check, post-deploy verify,
   and OMEGA.ps1-as-canonical-build-path discipline so this can't
   recur. New rule: any session that may commit engine code or
   shipping config MUST first verify VPS HEAD == origin/main ==
   running-binary hash. Mismatch is P0.

4. **USTEC +$728 trade investigation — CLEAN RESULT**. The trade was
   `UstecTrendFollow5m_Donchian` LONG entered 5/13 14:55 UTC, exited
   5/14 00:31 UTC TP_HIT, 9.6h hold (mostly overnight). It fired
   because the binary was pre-S68-deploy (still old S24-era binary
   that didn't have the disable). The engine performed correctly per
   its design (Donchian N=20 breakout, R:R=2:1, ATR-based sizing).
   The Phase 3 closure memo (S73, `outputs/UTF5M_PHASE3_RESULTS_2026-05-14.md`)
   shows the engine has REAL EDGE: 1,403 trades on 829-day tape, avg
   +$0.667/trade, gross +$936, but aggregate PF=1.1154 missed the
   1.20 gate by 0.085 due to one anomalous low-vol window (w1, 2024-H2
   → 2025-Q1, avg=−$0.376). w3 (2025-Q4 → 2026-Q2, the recent window)
   individually clears at PF=1.210 — the engine works on RECENT data.
   **The Phase 3 §7 explicitly recommends a vol-regime gate as the
   fix.** That gate is exactly what S85 (this session, earlier)
   plumbed into VWAPReversion. Port-and-run on UTF5m is the
   next-session priority #1.

## Commits this session (post part-AA)

| Commit | Message | Files |
|---|---|---|
| `f7c18f7` | S86 hotfix: VWAPReversion TIMEOUT-aware consec-FC tracking | `include/CrossAssetEngines.hpp` (1 file, +69/−2) |
| `571641d3` | docs(CLAUDE.md): Deploy Hygiene section — prevent recurrence of VPS HEAD divergence | `CLAUDE.md` (+~80 lines) |
| `<HANDOFF_HASH>` | docs: part-AB handoff | `docs/handoffs/SESSION_HANDOFF_2026-05-14q.md` |

`origin/main` was at `2a2b677` (part-AA close-out) at session reopen.
End-state sits at the closing handoff commit below.

## What did NOT land this session

- **VPS deploy of S86 is in-flight** as of this handoff write. Source
  tree is on main (post-checkout, BOM-only `symbols.ini` discarded,
  disables verified at HEAD). Operator was running `.\OMEGA.ps1 deploy`
  when handoff write began. **Verify status next session before any
  new work.** If the deploy didn't complete, S86 is still not in
  production and the 9-trade VWR US500.F bleed pattern can recur.
- **S87 sweep driver** for VWR Tier 4 (`scripts/vrev_vol_gate_p1.py`).
  Still queued from part-AA. The S85 engine code + S86 harness CLI
  (when written) need this driver to actually execute the Phase A
  sweep.
- **UTF5m Tier 4 port** — the next-session priority #1 surfaced this
  session. See "Recommended next-session focus" below.
- **PROVENANCE-vs-WF reconciliation on XauTrendFollow trio** — part-Z
  #1 priority, still queued.
- **EmaPullback per-cell tuning** — part-Z #4. Still queued.
- **Universe-wide S63 sweep continuation** (~21 state-E engines).
  Still queued.
- **Tsmom_H1_long direction-block audit** — surfaced this session as
  a question. The engine produced 6 of 6 losses on 5/14 (−$145), all
  hourly cadence longs that went adverse and held 9-12h. Need to
  check if it has any consec-loss / direction-block mechanism. If
  not, similar shape to the S86 VWR bug. Could be S88 next session.

## Recommended next-session focus

**Hard ordering.** #1 must precede everything else.

### 1. VERIFY VPS DEPLOY STATE (5 min, BEFORE any new work)

Per the new CLAUDE.md Deploy Hygiene section. Operator runs:

```powershell
cd C:\Omega
git rev-parse HEAD
git rev-parse origin/main
Get-ChildItem C:\Omega\Omega.exe | Select Name, LastWriteTime
Get-Content C:\Omega\logs\omega_service_stderr.log -Tail 5 | Select-String "Git hash|version="
```

All three values (HEAD, origin/main, stderr Git hash) MUST match. If
not, S86 isn't live, and the bleed pattern from 5/14 can recur.
Reconcile before any new work.

### 2. UTF5m Tier 4 vol-regime gate port (THIS SESSION'S MAIN WORK; 2-3 hrs)

**Goal:** Port the S85 Tier 4 framework from `VWAPReversionEngine` to
`UstecTrendFollow5mEngine`, run Phase 1 sweep + Phase 3 WF, decide
re-enable based on verdict.

**Why:** Phase 3 closure memo §7 item 1 explicitly recommends this.
The +$728 trade on 5/13-14 is one tail-event observation of an engine
with confirmed real edge (avg +$0.667/trade across 1,403 trades).
The single failing window (w1, 2024-H2) has the LOW-VOL fingerprint
the vol-regime gate is designed to suppress. If the gate lifts w1's
avg_pnl above zero, aggregate PF clears 1.20 and the engine
re-enables under WF discipline.

**Steps:**

1. **Engine code port** (~30 min). Copy the S85 pattern from
   `include/CrossAssetEngines.hpp` (look for the comment block "Tier 4
   vol-regime gate fields" + the `vg_*` private members + the daily
   H/L tracker block in on_tick + the entry-time decision before
   pos_.open() + the gating in BE_RATCHET / LOSS_CUT). Apply mechanically
   to `include/UstecTrendFollow5mEngine.hpp`. The architecture is
   different (M5-bar-driven via on_5m_bar, not tick-driven via
   on_tick), so the daily H/L tracking belongs in on_5m_bar at bar
   close. The entry-time decision belongs in the cell's fire path.
2. **Harness CLI port** (~20 min). `backtest/UstecTrendFollow5mBacktest.cpp`
   needs `--vol-gate-enabled`, `--vol-pct-threshold N`,
   `--atr-lookback-days N` flags applied to the engine instance after
   construction. Mirror the S86 (planned) harness CLI for VWR.
3. **Phase 1 sweep** (~20 min). 14-cell matrix: 2 lookbacks × 7
   thresholds (50, 60, 70, 75, 80, 85, 90), Form A only, symmetric.
   Adapt `scripts/utf5m_wf_t1.py` (which exists from S73). Decision
   criterion: best cell beats baseline by ≥10% on aggregate PF AND
   clears ≥1.20.
4. **If Phase 1 PASSes**, run Phase 3 WF (~85s runtime) on same
   4-window split as S73. Verdict: aggregate PF ≥ 1.20 AND ≥3/4 wins
   → re-enable; else stay disabled.
5. **Verdict memo + commit** (~30 min). Force-add to outputs/.

**Key references:**
- `include/CrossAssetEngines.hpp` — S85 Tier 4 pattern (search "Tier 4
  vol-regime gate fields" + "vg_atr_history_" + "vg_s63_active_")
- `include/UstecTrendFollow5mEngine.hpp` — target for port
- `outputs/UTF5M_PHASE3_RESULTS_2026-05-14.md` — closure memo with §7
  vol-regime gate recommendation
- `outputs/TIER4_VOL_REGIME_SCOPING_2026-05-14.md` — design memo with
  Phase A protocol
- `backtest/UstecTrendFollow5mBacktest.cpp` — harness target for CLI
  flag additions
- `scripts/utf5m_wf_t1.py` — Phase 3 WF driver (existing, may need
  --vol-gate flag passthrough)

**Decision pre-commit:** if Phase 1 surfaces no clear PASS cell
(no threshold beats baseline by ≥10% PF), stop the line. Don't
retune to fit. Document the negative result.

### 3. VWR USTEC.F Tier 4 continuation (S87 sweep driver) (1-1.5 hrs)

S85 engine code is in place but unexercisable until S86 (harness CLI,
**different S86 from the hotfix that just landed — needs renumbering
to S88 or similar**) and S87 sweep driver land. Same Phase A protocol
as UTF5m above. Less urgent than UTF5m only because UTF5m has
stronger evidence base for the gate working (Phase 3 §7 explicit
recommendation; UTF5m's w1 anomaly closer-fit to the gate hypothesis).

**Numbering note**: S86 was used for the TIMEOUT-fix hotfix. The VWR
harness CLI work that part-AA called "S86" needs a new number. Suggest
**S88** = VWR harness CLI, **S89** = VWR sweep driver, to keep numeric
ordering clean.

### 4. PROVENANCE-vs-WF reconciliation on XauTrendFollow trio (~1-2 hrs)

Part-Z #1 priority. Untouched. Still load-bearing for any future XTF
re-enable decision.

### 5. Tsmom_H1_long direction-block audit (~30 min)

Surfaced this session: 6 of 6 trades on 5/14 lost (−$145), hourly
cadence, all longs into adverse drift. Same shape as the VWR US500.F
bleed S86 just fixed. Need to read `include/TsmomEngine.hpp` /
`include/TsmomStrategy.hpp` to see if it has any consec-loss /
direction-block mechanism. If not, that's an analogous fix.

### 6. Universe-wide S63 sweep continuation (~21 state-E engines)

Multi-session. Lower priority but cleanly scoped.

## Live trade ledger analysis (5/7 → 5/14)

Found via VPS `C:\Omega\logs\trades\omega_trade_closes_*.csv`. Aggregate
across all per-day files:

| Metric | Value |
|---|---|
| Total trades | 3636 |
| Negative trades | 219 |
| Win rate (raw) | **94.0%** (misleading — see below) |
| Sum negative PnL | −$2,724.75 |
| Sum positive PnL | +$11,048.02 |
| Net PnL | **+$8,323.27** |

**The 94% WR is a HEAVY-CHURN ARTIFACT.** 3,289 of 3,636 trades came
from MicroScalperGold on 5/8 alone (+$8.5K from that single day).
Strip 5/8: the rest of the week is roughly **−$500 to −$1,000 net
across the suite** (positive on 5/10/5/11/5/14 by accident, negative
on 5/7/5/12/5/13).

**Engine bleed concentration (negative trades only):**

| Engine | Neg count | Sum | Avg |
|---|---|---|---|
| MicroScalperGold | 130 | −$1,007 | −$7.74 (small-$ but 6 tail $-100+ on 5/8 XAUUSD) |
| **VWAPReversion** | 31 | −$426 | **−$13.75 (S86 fix targets this)** |
| **Tsmom_H1_long** | 13 | −$412 | **−$31.69 (audit queued #5)** |
| MidScalperGold | 9 | −$53 | small |
| EurusdLondonOpen | 8 | −$160 | FX-open shape |
| GbpusdLondonOpen | 5 | −$70 | FX-open shape |
| Tsmom + EmaPullback + Donchian + ... | varied | smaller losses | scatter |

**Symbol concentration:** XAUUSD takes 168 of 219 losses (77%) and
−$1,814 of −$2,725 dollar losses (67%). Gold dominates the bleed.

**5/14 specifically — single bad day:**

26 trades, 23 losses, 3 "wins". Two engines drove the bleed:
- **VWAPReversion US500.F: 9 SHORTs, all TIMEOUT, all 100% loss**
  (the 10-min cadence pattern that S86 directly fixes)
- **Tsmom_H1_long XAUUSD: 6 hourly LONGs, all loss, all TIME_EXIT or
  MAE_EXIT** (similar shape, no fix yet — queued #5)

The 3 "wins": +$728 USTEC accident (pre-S68-deploy in-flight),
+$1.89 EmaPullback BE_CUT (basically flat), +$0.73 DonchianBreakout
TRAIL_HIT (basically flat). Strip the USTEC accident: 23 losses + 2
flat = −$374 across 25 real trades, 92% loss rate.

## USTEC +$728 investigation — full evidence

Engine: `UstecTrendFollow5m_Donchian` (Cell A of UstecTrendFollow5m).

**Trade record:**
- Entry: 2026-05-13T14:55:00Z LONG (London afternoon / NY pre-open)
- Exit: 2026-05-14T00:31:05Z TP_HIT (early Asia)
- Hold: 9.6h, mostly overnight
- Net PnL: +$728.34
- Cell: Donchian N=20, sl=2.0×ATR, tp=4.0×ATR, R:R=2:1

**Why it fired (despite engine being disabled per S68 commit 2026-05-12 12:14 UTC):**
Binary running at trade entry was the OLD pre-S68 build. S68 commit
existed in git but hadn't been deployed (see VPS HEAD divergence
finding). The engine was effectively still live in production at
trade time. The new binary at 5/14 06:49 UTC contains S68 — and
there are NO new USTEC entries after that timestamp. The disable IS
effective in the current binary.

**Was the trade typical or an outlier?**
Per Phase 3 w3 (the recent regime, 2025-Q4 → 2026-Q2): 460 trades, avg
+$1.26/trade. The +$728 trade is ~580× the average. **Outlier — a
tail-of-distribution winner.** This is HOW R:R=2:1 trend-follow makes
money: ~28% WR with rare large winners that cover the steady drip of
2-ATR SL hits.

**Does the engine have real edge?**
Yes. Phase 3 closure memo (S73) — 1,403 trades on 829-day tape:
- Aggregate gross: +$936
- Aggregate avg PnL: +$0.667/trade
- Aggregate PF: **1.1154** (failed 1.20 gate by 0.085)
- 3 of 4 windows pass `avg_pnl ≥ +0.001` (gate met)
- Per-window trajectory: w0 +0.118 → w1 −0.376 → w2 +0.950 → w3 +1.261
- w3 individually CLEARS the 1.20 PF gate (1.210)

**Why was it disabled if it has edge?**
Because the strict pre-committed decision rule was "and-conjunctive":
both the windows-passing gate AND the aggregate-PF gate must be met.
PF gate failed by 0.085. We honored the rule.

**What's the recommended fix?**
Per Phase 3 closure memo §7 item 1, **VERBATIM**:

> "Vol-regime gate as a Tier 4 candidate. Same as VWR's §7 item 1 —
> only fire when prior 5-day realised vol (or ATR) exceeds a threshold.
> If w1's 'low-vol → bad fills' hypothesis is correct, suppressing
> trades in that regime should lift w1 from avg_pnl=−0.376 toward
> neutral, which would lift aggregate PF above 1.20 without modifying
> signal mechanics."

The mechanism is: w1 had the lowest tick count (17.2M vs 25.9-46.1M
elsewhere), suggesting a low-volatility regime. Donchian breakouts and
EWM-VWAP reversions BOTH depend on range expansion. Tight ranges
starve both strategies → false breakouts get whipsawed → losing trades
dominate. Suppressing entries in low-vol regimes removes the bad
trades without removing the winners.

**This is exactly what S85 plumbed into VWAPReversion this session.**
The infrastructure exists. Port-and-run is the next session.

**What other engines could benefit (per Phase 3 §6.3 hypothesis):**

| Engine | Architecture | State | Tier 4 likely to help? |
|---|---|---|---|
| **UstecTrendFollow5m** | Donchian + Keltner breakout, M5 USTEC | DISABLED, PF FAIL by 0.085 | **YES — explicitly recommended** |
| **XauTrendFollow 2h/4h/D1** | Donchian-style, multi-TF XAU | DISABLED, Phase 3 FAIL | YES (after PROVENANCE reconciled) |
| **VWAPReversion (USTEC.F)** | EWM-VWAP mean-reversion | State B | S85 engine code in place; needs harness CLI |
| **VWAPReversion (US500.F / EURUSD)** | Same | State B | Once USTEC.F sweep proves concept |
| VWAPReversion (GER40) | Same | State A active | Already profitable; over-engineering risk |
| MinimalH4Gold | H4 Donchian breakout, XAU | LIVE PF=1.48 | Already works; over-engineering risk |
| EmaPullback (XAU) | EMA-9/21 pullback (mean-reversion) | LIVE state A | Different shape; less applicable |

## Important lessons / don't-repeat

1. **VPS git hygiene was a real systemic gap.** Multiple stop-bleed
   sessions (S68, S82, others) were shipped under the assumption "commit
   + push + operator-side rebuild = it's live", but the working tree
   never moved off S24 after the 2026-05-10 broken-build cycle. The
   protection actually-in-production for the past week was unverifiable.
   Caught only because the S86 deploy attempt triggered the new check.
   **CLAUDE.md Deploy Hygiene section now codifies the session-start
   hash-alignment check.** Run it FIRST in any session that may ship
   engine code.

2. **TIMEOUT exits bypass consec-FC counters → engine can fire
   indefinitely into adverse direction.** S86 fixes this for VWR.
   Same shape likely affects other engines (Tsmom_H1_long is the
   immediate suspect). Audit pattern: any engine with a `consec_fc`
   / `direction_block` / `cooldown_after_loss` mechanism should
   verify TIMEOUT / TIME_EXIT exits update the counter.

3. **The +$728 USTEC outlier doesn't establish edge — but the Phase 3
   distribution does.** 1,403 trades, avg +$0.667/trade, 3/4 windows
   profitable. The single trade is ONE observation of a genuinely
   positive-expectancy (but PF-gate-failing) engine. The gate failure
   is regime-concentrated (one bad year) and matches exactly the
   hypothesis the vol-regime gate is designed for. The +$728 is
   evidence the engine WORKS when conditions are right; the disable
   is evidence the conditions weren't right enough OF THE TIME to
   clear the strict aggregate gate.

4. **Selection-effect discipline applies symmetrically.** Earlier this
   session the L2-month MinimalH4Gold result showed cfg #20 (PF=1.74
   on 25-month) collapsed to PF=0.00 on the L2 month. Same lesson:
   one window's "best" doesn't establish edge. The +$728 USTEC trade
   is the SAME RISK in reverse — one window's exceptional winner
   doesn't justify re-enabling. Both directions need the discipline
   of pre-committed gates and bidirectional confirmation.

5. **Decision-rule discipline is what makes the system honest.** The
   UTF5m Phase 3 verdict was FAIL by 0.085 PF — close enough that
   "almost passes" is tempting. The Phase 3 memo §7 item 4 explicitly
   warns against post-hoc relaxing the rule because it sets a
   precedent that softens every future engine validation. The ONLY
   legitimate path back to re-enable is a NEW gate (Tier 4 vol-regime)
   that mathematically lifts the aggregate above 1.20 without
   modifying the underlying decision rule. That's the work queued.

6. **Heredoc-with-quoted-EOF is the safe commit-message pattern.**
   `git commit -m "..."` with embedded `\$`, parens, or anything
   shell-meaningful breaks paste. Use:
   ```bash
   cat > /tmp/msg.txt <<'EOF'
   <message body>
   EOF
   git commit -F /tmp/msg.txt
   ```
   The single-quoted `'EOF'` tells bash to treat body as literal —
   no variable expansion, no backtick substitution, no escape
   interpretation. Works for any character of any complexity.

## Files modified this session — final state

```
M  include/CrossAssetEngines.hpp            (S86 f7c18f7 committed)
M  CLAUDE.md                                (571641d3 committed)
A  docs/handoffs/SESSION_HANDOFF_2026-05-14q.md  (this file, pending)
```

VPS-side state (operator's working tree as of handoff write):
- HEAD now on main at `571641d3` or later (post-checkout)
- Discarded: BOM-only `symbols.ini` change
- Untracked debris: many tick CSVs, .dll files, old binary backups,
  scratch scripts. Most should be `.gitignore`d. Out-of-band cleanup.
- Still-untracked at repo root: `KILL_MICROSCALPER` (manual kill
  switch — leave alone unless operator says otherwise)

## Standing audits at session end

**Core code preserved.** None of `OmegaCostGuard.hpp`,
`OmegaTradeLedger.hpp`, `SymbolConfig.hpp`, `OmegaFIX.hpp`,
`OmegaApiServer.hpp`, `GoldPositionManager.hpp` were modified.

**Engine code: only `CrossAssetEngines.hpp` (S86, additive). engine_init.hpp
untouched.** All four live VWR instances behaviour change is gated by
`was_losing` — TP_HIT / trailed-SL-above-entry close paths are
unaffected. Only TIMEOUT-loss / SL-below-entry paths get the new
consec-FC increment.

**Stop-bleed disables intact (per HEAD source):**
- `g_vwap_rev_nq.enabled = false` at L667
- `g_vwap_rev_nq.LOSS_CUT_PCT/BE_ARM_PCT/BE_BUFFER_PCT = 0.0` at L701-703
- `g_ustec_tf_5m.enabled = false` at L1009

**MinimalH4Gold live state confirmed unchanged:**
- `g_minimal_h4_gold.shadow_mode = false` (S67 promotion)
- Production config: D=10 SL=1.5 TP=4.0 (PF stable across 25-month + L2 month)

**S63 state inventory unchanged from part-Z/AA.** S86 is a Tier-4-style
fix layered on existing consec-FC mechanism, not new S63 plumbing.

**Ungated-engine sweep expectations unchanged.**

**GoldEngineStack chokepoint audit:** not touched.

## Stash state at session end

```
$ git stash list
(empty)  -- expected
```

Inherited clean from part-AA. No new stashes this session.

## Operational notes

- **Sandbox bash continues to be dead.** All builds, harness runs, and
  commits operator-side via Mac (paste-back). Sandbox-side file tools
  (Read / Grep / Glob / Edit / Write) worked normally throughout.
- **VPS deploy of S86 was in-flight at handoff write.** Source tree is
  on main; binary rebuild + service restart pending. **First action
  next session is to confirm the deploy completed.**
- **Two real engine commits + one docs commit + one handoff commit.**
  Wall-clock ~3-4 hrs total session, longer than typical because of
  the VPS HEAD divergence diagnostic detour. The work product is
  proportional: a real live-bug fix (S86), a systemic fix to deploy
  hygiene (CLAUDE.md), and a clean investigation result that
  unblocks the next session's primary work (UTF5m Tier 4 port).

## Pre-commit checklist for next session

Standard project CLAUDE.md checklist applies, PLUS the new Deploy
Hygiene section (added this session).

**Mandatory at session start:**

```powershell
cd C:\Omega
git rev-parse HEAD
git rev-parse origin/main
Get-ChildItem C:\Omega\Omega.exe | Select Name, LastWriteTime
Get-Content C:\Omega\logs\omega_service_stderr.log -Tail 5 | Select-String "Git hash|version="
```

All values matching. If not, P0 — investigate before any new ship.

**Standard pre-commit:**
- Mac canary green: `cmake --build build --target OmegaBacktest -j`
- `git diff` reviewed: only intended changes
- Commit messages reference `S<N>` numbering. **Numbering note:** S86
  was used for the TIMEOUT-fix hotfix this session. The VWR harness
  CLI work part-AA called "S86" needs a new number — suggest **S88**
  for VWR harness CLI, **S89** for VWR sweep driver, to keep numeric
  ordering clean. UTF5m Tier 4 port (next session priority #2) gets
  whatever's next after that.
- Heredoc-with-quoted-EOF for commit messages with any shell-meaningful
  characters.
- Unrelated changes split via `git add -p`.
- For engine_init.hpp settings touching `LOSS_CUT_PCT` / `BE_ARM_PCT` /
  `BE_BUFFER_PCT` / `enabled`: read the comment block above.
- For S63 management-path additions: call-site activation in same
  commit.

**Mandatory post-deploy:**

```powershell
Get-Content C:\Omega\logs\omega_service_stderr.log -Tail 10 | Select-String "Git hash|version="
git rev-parse origin/main
```

The `Git hash:` value MUST match origin/main. If it shows the prior
hash, the rebuild didn't pick up the new code OR the service restarted
from the OLD binary. The deploy didn't take.

## Closing note

Long, dense session. Started as a clean part-AA close-out, reopened
when the operator asked about live losses, surfaced one live engine
bug (S86), one systemic deployment-hygiene gap (Deploy Hygiene section
added to CLAUDE.md), and one clean investigation result that
unblocks the next session's primary work.

The story arc: this session's S85 Tier 4 vol-regime gate was built for
VWR USTEC.F. The Phase 3 closure memo for UTF5m USTEC.F (written last
week, S73) explicitly recommends the SAME mechanism. The +$728 USTEC
trade was empirical evidence that UTF5m has the same regime-concentrated
edge profile that motivates the gate. Porting S85's framework to UTF5m
is a mechanical mirror, runs in 2-3 hours, and could rescue an engine
whose disable cost the suite a real winner this week.

The deploy gap discovery is the larger structural finding. Multiple
weeks of work on stop-bleeds may have been ineffective in production
because the working tree wasn't where we thought it was. Going forward
the new Deploy Hygiene check catches this at session start in 30
seconds. The bigger fix — adding hash-mismatch detection to the
service startup pipeline so the system itself refuses to start a
divergent binary — is engine code, separate session, lower priority
than the work queue above.

### Suggested commit command for closing the part-AB session

```bash
cd ~/omega_repo

# (a) Sanity check
git status
# Expect only: docs/handoffs/SESSION_HANDOFF_2026-05-14q.md (this file)

# (b) Stage and commit via heredoc-tempfile pattern (per S86 lesson)
git add docs/handoffs/SESSION_HANDOFF_2026-05-14q.md

cat > /tmp/handoff_ab_msg.txt <<'EOF'
docs: part-AB handoff -- S86 hotfix + VPS deploy hygiene + USTEC investigation result

Captures the part-AB session arc:
  - S86 hotfix (already on origin/main at f7c18f7): VWAPReversion
    TIMEOUT-aware consec-FC tracking. Live-bug fix from 5/14 evidence
    (9 consecutive VWR US500.F SHORTs against an uptrend, all TIMEOUT,
    no direction-block engagement). Symmetric across all four VWR
    instances. Conservative -- closing a hole in EXISTING protection.

  - Deploy Hygiene section landed in CLAUDE.md (already on origin/main
    at 571641d3). Codifies session-start hash-alignment check, post-
    deploy verify, OMEGA.ps1-as-canonical-build-path discipline. Added
    after live discovery of three-way VPS HEAD divergence: working
    tree at S24 (April), running binary at S81 (this morning), origin/
    main at S86 (this session). Multiple stop-bleed commits over the
    prior week had been shipped under the assumption "commit + push +
    rebuild = live", but the working tree had never moved off S24
    after the 2026-05-10 broken-build cycle.

  - USTEC +$728 investigation -- CLEAN RESULT. Trade fired because
    binary was pre-S68-deploy. Engine has REAL edge per Phase 3 (1403
    trades, avg +$0.667, gross +$936) but PF=1.1154 missed the 1.20
    gate by 0.085. Phase 3 closure memo S73 explicitly recommends
    vol-regime gate as the fix. That gate is exactly what S85 (this
    session, earlier) plumbed into VWAPReversion. Port-and-run on
    UTF5m is next session's priority #1.

Next-session priorities (revised from part-AA):
  1. VERIFY VPS deploy state at session start (mandatory per new
     Deploy Hygiene section).
  2. UTF5m Tier 4 vol-regime gate port (S85 framework -> UTF5m
     engine + harness CLI + Phase 1 sweep + Phase 3 WF + verdict).
     Could rescue a disabled engine whose edge is real but
     regime-concentrated.
  3. VWR Tier 4 continuation (harness CLI as S88, sweep driver as
     S89). Same protocol, less urgent than UTF5m.
  4. Tsmom_H1_long direction-block audit. 6 of 6 trades on 5/14
     lost (-$145), shape similar to S86 VWR bug. Possibly analogous
     fix.
  5. PROVENANCE-vs-WF reconciliation on XauTrendFollow trio.
  6. EmaPullback per-cell tuning.
  7. Universe-wide S63 sweep continuation.

Numbering note: S86 was used for the TIMEOUT-fix hotfix. VWR harness
CLI work part-AA called "S86" needs renumbering -- suggest S88 for
VWR harness CLI, S89 for VWR sweep driver.

Live evidence trail and full investigation details in the handoff
doc. Particularly the per-engine bleed concentration and the UTF5m
Phase 3 cross-window trajectory.

Handoff at docs/handoffs/SESSION_HANDOFF_2026-05-14q.md.
EOF

git commit -F /tmp/handoff_ab_msg.txt

# (c) Push and verify
git push origin main
git log --oneline -6
```

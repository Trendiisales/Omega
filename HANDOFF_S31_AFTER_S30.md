# HANDOFF S31 — After S30 (XAU gated-variant test on S30 TOP-1)
**Session date:** 2026-05-11
**Branch:** `main` at `9bc02f9` (unchanged HEAD; S31 work uncommitted)
**Mode:** `omega_config.ini` line 75 still `mode=SHADOW` (rule 3 honored)
**max_lot_gold:** still `0.01` (rule 4.6 honored)
**This session's commits:** zero. Operator commits per their own cadence.

---

## 0a. COWORK FOLDER MOUNTS — request these FIRST before any work

| # | Host path | Purpose | Required for |
|---|-----------|---------|--------------|
| 1 | `/Users/jo/omega_repo` | Repo root, harness, outputs, all S30/S31 CSVs. | EVERY task. Mount FIRST. |
| 2 | `/Users/jo/Tick`       | L2 captures including XAU L2 days 2026-04-09..14. | Required for any future L2 work (S31 already copied/symlinked the relevant XAU files into the repo's outputs/l2_xau_daily/ staging dir; future sessions can use that staging dir directly without re-mounting /Users/jo/Tick). |

Path-mapping rule from S28/S29/S30 §6 still applies: **use the host path
with Read/Write/Edit/Grep/Glob; use the `/sessions/<slug>/mnt/...` form
ONLY with `mcp__workspace__bash`**.

---

## 0. ONE-PARAGRAPH STATE OF THE WORLD

S30 closed with a 256-cell --wide-fine sweep that surfaced the new
TOP-1 candidate (TP=35, SL=12, z=2.0, W=200, Asia 0-7 UTC) at daily
mean **+$2.31** and **CI95 [+$0.79, +$3.95]** — strict-CI-positive on
the 623-day Dukascopy corpus, ungated. The S30 §3.1 priority was: run
the equivalent with **gates ON** and determine whether the S27 L2 +
regime gates AMPLIFY, leave UNCHANGED, or KILL the new candidate.
**S31 discovered the literal §3.1 experiment cannot be run on the
623-day Dukascopy corpus** — Dukascopy daily CSVs are bid/ask only,
so the gate's defaults (l2_imb=0.5 vs imb_long=0.502 / imb_short=0.498)
block every signal: 0/256 cells trade. S31 ran two complementary
experiments instead: (a) full L2 + regime gates on a 19-day
L2-equipped recent corpus (15 days from /Users/jo/omega_repo/data/ +
4 days from /Users/jo/Tick/l2_data/), and (b) spread-only gate (L2
neutralized via `--imb-long 0.5 --imb-short 0.5`) on the full
623-day Dukascopy corpus. **Spread-only on 623d: UNCHANGED** — TOP-1
daily mean +$2.31 → +$2.12 (-8.1%), CI95 [+$0.79, +$3.95] →
[+$0.56, +$3.68], still strict-CI-positive (11 CI-positive cells vs
10 ungated). **Full L2 + regime gates on 19d: KILL** — TOP-1 daily
mean +$12.36 → **-$5.36**, WR 36.7% → 21.9%, CI95_lo +$0.01 → -$16.53.
Importantly, the 19d L2 sample is small (wide CI95s) and recent
(2026-04..05 regime only), so the KILL verdict cannot generalize back
to 2023-2025 conditions. The S30 ungated TOP-1 cell ALSO tops the
19d ungated L2 sample (rank #6 by CI95_lo, +$12.36/day) —
sign-preserved across two completely different regimes. **The
gates rearrange the optimal cell entirely** — the best gated_l2_19
cell is TP=40/SL=20/z=2.0/W=100 (wider stop, smallest window), not
the TP=35/SL=12/z=2.0/W=200 ungated TOP-1. **Do NOT deploy live
based on this** — the §3.1 question has a nuanced answer (gates
preserve the edge on spread alone, but rearrange the optimal cell
on real L2 data), and the live-deployment validation checklist from
S29 §3.1 / S30 §2.4 remains incomplete.

---

## 1. WHAT THIS SESSION DID (S31 chronological)

### 1.1 Verify on-disk state (S30 rule 2)

- HEAD = `9bc02f9` ✓ (same as S30 / S29 / S28; S25 commit still latest)
- `omega_config.ini` line 75 → `mode=SHADOW` ✓
- `omega_config.ini` line 195 → `max_lot_gold=0.01` ✓
- 623 daily CSVs in `outputs/duka_xauusd_daily/` ✓
- All S29/S30 binaries on disk: `_v2`, `_v2_ext`, `_v2_linux`, `_v2_s28`,
  `_v2_s29`, `_v2_s30` (70,136 B exactly per S30 handoff §1.2) ✓
- `honest_backtest_xauusd_v2.cpp` 1140 lines (S30 final state) ✓
- Modifications on core files (omega_config.ini, omega_main.hpp,
  order_exec.hpp, OmegaTradeLedger.hpp, IndexFlowEngine.hpp,
  RiskMonitor.hpp, trade_lifecycle.hpp, microscalper_crtp_sweep.cpp,
  IndexBacktest.cpp) all PRE-EXISTING from prior sessions (no commits
  since S25). **S31 did not touch any of them.**

### 1.2 Regression test (rule 11)

Ran `backtest/honest_backtest_xauusd_v2_s30 --wide --latency 1` and
`_s29 --wide --latency 1` on day 2024-05-22, stripped INFO/timing
lines, compared via md5sum:

```
5b22cd55aa20e0f488e5896a9ef00678  /tmp/s29.txt
5b22cd55aa20e0f488e5896a9ef00678  /tmp/s30.txt
```

Bit-identical to the S30 handoff's documented expected md5. _s30
binary behavior matches _s29 on bare --wide, confirming the S30
extensions are backwards-compatible.

### 1.3 Harness inspection (NO source change in S31)

The S30 §3.1 plan suggested possibly adding `--wide-fine-gated`. S31
read the source and confirmed the existing `--wide-fine` and `--gated`
flags **already compose correctly** via the `build_params` lambda
(line 1005-1025 of honest_backtest_xauusd_v2.cpp) which captures the
outer `gated` flag by reference and assigns it to `P.gated` for
every cell in the grid sim loop (line 1083-1093). **No harness
extension was needed.** No source changes. Same _s30 binary used
throughout S31. `honest_backtest_xauusd_v2.cpp` remains 1140 lines.

### 1.4 The blocking-discovery: Dukascopy corpus is bid/ask only

After confirming the plumbing works, S31 attempted a smoke test:

```
backtest/honest_backtest_xauusd_v2_s30 --wide-fine --gated \
  --session 0-7 --latency 1 outputs/duka_xauusd_daily/2024-05-22.csv
```

Result: **0/256 configs trade.** Verdict: "production-fill total $+0.00
vs honest-fill total $+0.00 — NO honest-fill config survives." This
is because:

- The daily CSV header is `ts_ms,bid,ask` only (3 columns).
- Default Tick fields when missing columns: `l2_imb=0.5, regime=0,
  watchdog_dead=0` (load_ticks() line 542).
- Gate filter (line 247-255):
  - regime 0 passes (regime_ok allows {0, 2}) ✓
  - watchdog_dead 0 passes ✓
  - spread > 1.0 USD/oz check would only matter if signal fires on a
    wide-spread tick (mean spread on 2024-05-22 was 0.44, max 2.36)
  - **L2_imb check: LONG fires only if l2_imb ≥ imb_long=0.502;
    SHORT fires only if l2_imb ≤ imb_short=0.498. At l2_imb=0.5,
    BOTH fail → every signal blocked.**

This is the structural reason the literal §3.1 experiment cannot
be run on the 623-day Dukascopy corpus. The S30 handoff §3.1 plan
("just run --wide-fine --gated") presumed the gates would degrade
to spread/regime-only on tick-only data; in practice the L2 part
of the gate is the strictest filter and blocks everything.

A "gate-lite" probe (`--gated --imb-long 0.5 --imb-short 0.5`,
collapsing the L2 thresholds to no-ops) reproduced the ungated
result cent-for-cent on 2024-05-22 ($+35.00 production / $+35.12
honest both gated-lite and ungated), confirming that on this day
the spread/regime/watchdog gates were also no-ops (signal fires
happened to coincide with tight-spread ticks).

### 1.5 Operator decision

Operator selected "Do both: 21-day L2 + 623-day spread-only" via
AskUserQuestion. S31 ran both experiments.

### 1.6 L2 corpus inventory (19 unique XAU days)

Source dirs scanned:

- `/Users/jo/omega_repo/data/l2_ticks_XAUUSD_*.csv` — 15 files,
  dates 2026-04-22 → 2026-05-08
- `/Users/jo/Tick/l2_data/l2_ticks_*.csv` — 6 files, dates 2026-04-09
  → 2026-04-14 (includes a part1/part2 split for 04-14)

Headers confirmed identical: `ts_ms,mid,bid,ask,l2_imb,l2_bid_vol,
l2_ask_vol,depth_bid_levels,depth_ask_levels,depth_events_total,
watchdog_dead,vol_ratio,regime,vpin,has_pos,micro_edge,ewm_drift`.
load_ticks() reads only the columns it knows about (ts_ms, bid,
ask, l2_imb, regime, watchdog_dead).

Tick mount files contained XAU (bid in $4700s range, consistent
with gold price in 2026-04). After de-duping the 04-14 split,
**19 unique L2 XAU days** staged via symlinks under
`outputs/l2_xau_daily/`:

```
2026-04-09 / 04-10 / 04-13 / 04-14
2026-04-22 / 04-23 / 04-24 / 04-26 / 04-27 / 04-28 / 04-29 / 04-30
2026-05-01 / 05-03 / 05-04 / 05-05 / 05-06 / 05-07 / 05-08
```

L2/regime data is real (l2_imb min 0.25, max 1.0 on 2026-04-09;
regime histogram across days shows all 4 regimes {0,1,2,4} populated).

### 1.7 L2 sweep (gated + ungated on the 19 L2 days)

- `OUT_G=outputs/wide_fine_l2_gated.csv` — `--wide-fine --gated`,
  9728 data rows = 19 × 256 × 2. **Wall: 18 s.**
- `OUT_U=outputs/wide_fine_l2_ungated.csv` — `--wide-fine`,
  9728 data rows. **Wall: 23 s.**

All in a single bash chunk. Same _s30 binary, --session 0-7, --latency 1.

### 1.8 623-day spread-only-gate sweep

- `OUT_CSV=outputs/duka_wide_fine_asia_gated_spread.csv`
- Flags: `--wide-fine --gated --imb-long 0.5 --imb-short 0.5
   --session 0-7 --latency 1`
- 14 bash chunks of 30–40 days each (under the 45 s sandbox limit).
- **Total wall: ~4.5 min** for 623 days.
- Output: 318,977 rows (1 header + 623 × 256 × 2 data).

The S30 §6.3 chunk-and-comm pattern was reused without modification.

### 1.9 Aggregation + verdict (single unified script)

`scripts/duka_xau_s31_gated_compare.py` is a NEW script (272 lines)
that reads all 4 CSVs (ungated 623d, spread-only 623d, ungated 19d
L2, gated 19d L2), aggregates each with the same 1000-iter
bootstrap CI95 methodology used in S30 (seed=42, net of $0.06/RT
Prime), and emits:

- `outputs/s31_gated_compare_summary.csv` — 2048 rows
  (4 conditions × 256 cells × 2 fills)
- `outputs/s31_gated_compare_verdict.md` — the headline answer

This was preferred over forking the S30 fine_aggregate.py three
separate times: single pass, single source of truth, side-by-side
tables.

---

## 2. THE HEADLINE FINDING (the answer to §3.1)

### 2.1 TOP-1 cell across 4 conditions

| Condition | Days | Trades | WR% | Total net $ | Daily mean net $ | CI95 net daily $ |
|---|---|---|---|---|---|---|
| ungated_623 (S30 baseline) | 623 | 725 | 41.8 | +$1439.92 | +$2.3113 | [+$0.79, +$3.95] ★ |
| spread_only_623 | 623 | 728 | 41.4 | +$1323.39 | +$2.1242 | [+$0.56, +$3.68] ★ |
| ungated_l2_19 | 19 | 49 | 36.7 | +$234.84 | +$12.36 | [+$0.01, +$24.12] ★ (marginal) |
| gated_l2_19 | 19 | 41 | 21.9 | -$101.79 | **-$5.36** | [-$16.53, +$4.31] |

### 2.2 Verdicts

- **Spread-only gate on 623d: UNCHANGED.** Daily mean drops 8.1%, CI95
  narrows but stays strict-positive. 11 strict-CI-positive cells (one
  more than ungated). The spread filter is a near-no-op on the
  Dukascopy tape — the signal already fires almost exclusively when
  spread < 1.0 USD/oz.
- **Full L2 + regime gates on 19d L2: KILL.** Daily mean flips from
  +$12.36 to **-$5.36**. Trade count drops 49 → 41 (-16%), WR drops
  36.7% → 21.9%. The gate filters out winners disproportionately on
  the L2 days. CI95 widens enough that the upper bound is still
  marginally positive (+$4.31), but the central tendency is firmly
  negative.

### 2.3 Cross-corpus sign preservation (a positive sub-finding)

The S30 ungated TOP-1 cell (35/12/2.0/200) on the 19d ungated L2
sample produces **+$12.36/day, CI95 [+$0.01, +$24.12]** — barely
strict-CI-positive in a tiny sample, ranked #6 by CI95_lo across the
256 cells. The same geometry that's CI-positive on Dukascopy 2023-25
is also CI-positive (just barely) on BlackBull L2 2026-04-05. **The
edge survives a regime change** at the geometry level when gates are
off. This is a meaningful piece of out-of-sample evidence the S30
candidate isn't pure Dukascopy-specific noise.

### 2.4 What the gates actually do on real L2 data (rearrange the optimum)

On `gated_l2_19`, the best cell is **TP=40, SL=20, z=2.0, W=100** —
**wider stop, smaller TP-to-SL ratio (2:1 vs the ungated 35:12 ≈
2.92:1), smallest window**. The ungated TOP-1 family (W=200, SL=12)
is COMPLETELY ABSENT from the gated_l2_19 top 10. By contrast on
ungated_l2_19 the TOP-1 (35/12/2.0/200) appears at rank #6.

This is consistent with how the gates work:

- The l2_imb thresholds (0.502 / 0.498) bias entries toward
  imbalance-confirmed signals.
- The regime filter (regime ∈ {0, 2}) eliminates trades during
  regime 1 and 4 ticks.
- Together, these filter out a different set of trades than the
  geometry filter does — and the resulting trade tape needs a
  different geometry to be profitable.

If gates work on a different sub-tape, gates and geometry can't be
optimized independently — they have to be co-optimized. **The §3.1
plan implicitly assumed gates would be a near-orthogonal modifier.
They are not.**

### 2.5 What this means for deployment

- **Still NOT deployable.** The headline answer is mixed:
  spread-only-gating doesn't hurt at 623-day scale; full L2 gating
  appears to hurt on a small recent sample. Neither result is
  decisive.
- **The S30 ungated TOP-1 remains the strongest candidate** for the
  next step in the validation cascade (S29 §3.1 / S30 §2.4
  checklist), but the §3.1 gated-validation step did NOT cleanly
  pass.
- **If the operator wants to push the TOP-1 toward deployment**, the
  next two questions are:
  1. **Co-optimize gates + geometry** — run --wide-fine on the L2
     corpus with the gates as a hyperparameter, find the best
     (geometry, gate-settings) joint pair. Won't generalize from
     19 days, but tells us whether gated XAU even has a coherent
     local optimum on the L2 sub-tape.
  2. **Get more L2 history** — capture/find another 60-180 days of
     L2-equipped XAU to make any gated CI95 meaningful. Without
     more L2 history, the gated question is structurally
     unanswerable at deployment-grade rigor.

### 2.6 Operator-owned decisions remain operator-owned

Mode=SHADOW stays; max_lot_gold=0.01 stays; no live recommendation.

---

## 3. WHAT THE NEXT SESSION SHOULD DO

In priority order. Numbering continues the S30 §3 sequence.

### 3.1 (Highest) Cooldown + latency sensitivity for TOP-1 (was S30 §3.2)

The §3.1 question is now substantively answered (UNCHANGED on
spread, KILL on small L2 sample, edge survives regime change at
geometry level). The next strongest defensive test on the
TOP-1 is the cooldown/latency sweep:

- `--cooldown` ∈ {25, 50, 200, 400} keeping all else equal
  (default 100)
- `--latency` ∈ {0, 2, 3} (default 1)

16 small single-config sweeps via:
```
--single 35,12,2.0 --window 200 --session 0-7 --cooldown N --latency M
```
~25 min total wall clock. Tells us whether the default
cooldown=100 and latency=1 are accidentally over-tuned.

### 3.2 (Highest) Co-optimization on the 19-day L2 corpus

Given the §2.4 finding that gates rearrange the optimum entirely,
the right way to ask the gated question on the L2 corpus is a
JOINT sweep over geometry AND gate parameters:

- The current --wide-fine grid scans 256 (TP, SL, z, W) cells.
- A meaningful gated sweep would also vary imb_long ∈ {0.50, 0.51,
  0.52, 0.53} and imb_short ∈ {0.50, 0.49, 0.48, 0.47}, and
  max_spread_pt ∈ {0.5, 1.0, 1.5, 2.0}.
- That's 256 × 16 × 4 = 16,384 cells × 2 fills × 19 days =
  ~620k rows. Probably needs a small harness extension
  (`--wide-fine-gates` flag) to vary gate params per cell.

Alternative cheaper version: pick the gated_l2_19 TOP-1
(40/20/2.0/100) and run a finer mesh around it
(`--wide-fine` re-keyed to TP{35,40,45}, SL{15,18,20,22},
z{1.5,2.0,2.5}, W{50,100,200,400}) on the L2 corpus only.

### 3.3 (Medium) Find or build more L2 history

S30 §3.5 / S29 §3.4 list a tick-only regime classifier as a
multi-session project. With more L2 history available, the
gated question becomes answerable at 623-day-equivalent rigor.
The classifier project might be the only way to get there
without buying historical L2 data.

### 3.4 (Lower) Cross-currency port of the winning geometry

Unchanged from S30 §3.3. The bps-equivalent grid scaling on
EUR/USD / GBP/USD / USD/JPY using
`scripts/histdata_to_blackbull.py` is still a valid next step
if the operator wants breadth.

### 3.5 (Optional) Explicit Python replay verification of TOP-1

Unchanged from S30 §3.4. One-day Python replay of the TOP-1 on
its top winning day (2025-05-08) for cent-perfect C++/Python
match. The S31 _s30 regression check against _s29 carries the
S29 §3.4 verification transitively.

### 3.6 DO NOT DO

- Do not deploy live yet.
- Do not flip mode=LIVE for any reason without explicit instruction.
- Do not modify the protected core engine files
  (microscalper_crtp_sweep.cpp, omega_main.hpp, order_exec.hpp,
  OmegaTradeLedger.hpp, IndexFlowEngine.hpp, RiskMonitor.hpp,
  trade_lifecycle.hpp).
- Do not re-run any sweep already on disk.
- Do not interpret the gated_l2_19 KILL result as definitive — 19
  days is too small a sample to disprove a 623-day ungated edge.
- Do not interpret the ungated_l2_19 +$12.36/day as expected daily
  PnL — bootstrap CI95 is [+$0.01, +$24.12], lower bound is
  essentially zero.

---

## 4. CARRIED-FORWARD OPEN ITEMS FROM PRIOR HANDOFFS

- Part 1B §4 ledger correction — still uncommitted (operator owns).
- Part 2 §3 fill-model direction — still open.
- Part 2 §4 signal port — `GoldMicroScalperEngine` faithful port still open.
- S26 Part 3 cross-window stability — S30 confirmed W=200 unique;
  S31 confirms that under gates the optimal window shifts to
  W=100 on the L2 sub-tape (geometry/gate co-optimization needed).
- S30 §3.5 tick-only regime classifier — bumps in priority given
  §3.3 above.

---

## 5. RULES FOR THE NEXT SESSION

1. Read this handoff + S30 + S29 + S28 + S27 + S26 Part 3 + Part 2 +
   Part 1B end-to-end before touching anything.
2. Verify on-disk state: `git status`, `git log -1` (expect `9bc02f9`),
   mode=SHADOW, max_lot_gold=0.01, harness binaries on disk
   including the unchanged _s30 (70,136 B).
3. **DO NOT flip back to mode=LIVE** without explicit operator
   instruction.
4. **DO NOT recommend deploying any strategy live** including the
   S30 TOP-1 — §3.1 returned a mixed verdict and further tests
   are needed.
5. `honest_backtest_xauusd_v2.cpp` remains **1140 lines** (S31 did
   not touch source). S29 rule 5 still applies for future
   modifications: full file output in chat unless operator changes
   threshold.
6. **Never modify** the protected core files (list at S30 §5.6).
7. Operator preference: warn at 70% context with summary; stop and
   write a follow-up handoff. Don't stretch.
8. Operator preference: full file output when changing files.
9. The new staging dir `outputs/l2_xau_daily/` holds 19 symlinks
   to L2 XAU days from /Users/jo/omega_repo/data/ and
   /Users/jo/Tick/l2_data/. Future sessions can re-use this
   staging dir directly without re-inventorying.
10. **STAY ON XAU.** Cross-currency port (S30 §3.3, S31 §3.4)
    remains lowest priority.
11. The `_s30` binary is still the active one. NO new binary in
    S31. Bit-identical regression vs _s29 on bare --wide
    re-confirmed in S31 §1.2.
12. For aggregating any fine-grid CSV, use
    `scripts/duka_xau_fine_aggregate.py` (single CSV) or
    `scripts/duka_xau_s31_gated_compare.py` (multi-condition).
    Do NOT use the older `duka_xau_phase_aggregate.py` — it
    collapses windows.
13. The bash sandbox /tmp is permission-denied. Use
    `outputs/_tmp_sweep/` (or similar under outputs/) for
    intermediate files when chunking sweeps.
14. The S29 runner script becomes slow once OUT_CSV grows past
    ~16 MB. The inline-`comm` pattern from S30 §6.3 (and reused
    in S31 §6.3 below) is the preferred resume-safe driver for
    large sweeps.

---

## 6. REPRODUCING THIS SESSION'S KEY RESULTS

### 6.1 Verify on-disk state (per S30 §6.2)

```bash
cd /Users/jo/omega_repo
git log -1 --oneline                                # 9bc02f9
sed -n '75p;195p' omega_config.ini                  # mode=SHADOW; max_lot_gold=0.01
ls outputs/duka_xauusd_daily/ | wc -l               # 623
wc -l backtest/honest_backtest_xauusd_v2.cpp        # 1140
ls -la backtest/honest_backtest_xauusd_v2_s30       # 70,136 bytes
```

### 6.2 Regression check (s30 vs s29 on 2024-05-22)

```bash
DAY=outputs/duka_xauusd_daily/2024-05-22.csv
backtest/honest_backtest_xauusd_v2_s29 --wide --latency 1 "$DAY" 2>/dev/null \
  | grep -v -E '^\[INFO\]|simulated.*configs in' > /tmp/s29.txt
backtest/honest_backtest_xauusd_v2_s30 --wide --latency 1 "$DAY" 2>/dev/null \
  | grep -v -E '^\[INFO\]|simulated.*configs in' > /tmp/s30.txt
md5sum /tmp/s29.txt /tmp/s30.txt
# Expected: both md5 = 5b22cd55aa20e0f488e5896a9ef00678
```

### 6.3 L2 staging (19 unique XAU days)

```bash
REPO=/Users/jo/omega_repo
TICK=/Users/jo/Tick
STAGE="$REPO/outputs/l2_xau_daily"
mkdir -p "$STAGE"
# Repo data files
for f in "$REPO"/data/l2_ticks_XAUUSD_*.csv; do
  d=$(basename "$f" | sed -E 's/l2_ticks_XAUUSD_(.*)\.csv/\1/')
  ln -sf "$f" "$STAGE/$d.csv"
done
# Tick mount files (skip part-splits, don't clobber repo files)
for f in "$TICK"/l2_data/l2_ticks_*.csv; do
  bn=$(basename "$f")
  case "$bn" in *_part*) continue;; esac
  d=$(basename "$f" | sed -E 's/l2_ticks_(.*)\.csv/\1/')
  [ -e "$STAGE/$d.csv" ] && continue
  ln -sf "$f" "$STAGE/$d.csv"
done
ls "$STAGE" | wc -l   # 19
```

### 6.4 L2 sweep (gated + ungated)

```bash
cd /Users/jo/omega_repo
BIN=backtest/honest_backtest_xauusd_v2_s30
STAGE=outputs/l2_xau_daily

# Gated
OUT=outputs/wide_fine_l2_gated.csv
rm -f "$OUT"
for f in "$STAGE"/*.csv; do
  "$BIN" --wide-fine --gated --session 0-7 --latency 1 \
    --csv-out "$OUT" --label wide_fine_l2_gated "$f" > /dev/null 2>&1 || true
done

# Ungated
OUT=outputs/wide_fine_l2_ungated.csv
rm -f "$OUT"
for f in "$STAGE"/*.csv; do
  "$BIN" --wide-fine --session 0-7 --latency 1 \
    --csv-out "$OUT" --label wide_fine_l2_ungated "$f" > /dev/null 2>&1 || true
done
```

Both should finish in ~20-30 s. Expected row counts: 9728 data
rows each (19 × 256 × 2).

### 6.5 623-day spread-only-gate sweep (chunked, resume-safe)

```bash
cd /Users/jo/omega_repo
OUT_CSV=outputs/duka_wide_fine_asia_gated_spread.csv
BIN=backtest/honest_backtest_xauusd_v2_s30
LABEL=duka_wide_fine_asia_gated_spread
DAILY_DIR=outputs/duka_xauusd_daily
TMPDIR=outputs/_tmp_sweep
mkdir -p "$TMPDIR"
CHUNK=30   # safe under 45 s bash sandbox

# Repeat until "state: 623 / 623":
ls "$DAILY_DIR" | sort > "$TMPDIR/all.txt"
awk -F, 'NR>1{print $1}' "$OUT_CSV" 2>/dev/null | sort -u > "$TMPDIR/done.txt"
comm -23 "$TMPDIR/all.txt" "$TMPDIR/done.txt" > "$TMPDIR/todo.txt"
processed=0
while IFS= read -r d; do
  (( processed >= CHUNK )) && break
  "$BIN" --wide-fine --gated --imb-long 0.5 --imb-short 0.5 \
    --session 0-7 --latency 1 --csv-out "$OUT_CSV" --label "$LABEL" \
    "$DAILY_DIR/$d" > /dev/null 2>&1 || true
  processed=$((processed + 1))
done < "$TMPDIR/todo.txt"
days_done=$(awk -F, 'NR>1{print $1}' "$OUT_CSV" | sort -u | wc -l)
echo "state: $days_done / 623"
```

Expected total: 318,977 rows. Wall clock: ~4.5 min across ~16 chunks.

### 6.6 Unified aggregation + verdict

```bash
python3 scripts/duka_xau_s31_gated_compare.py
# Outputs:
#   outputs/s31_gated_compare_summary.csv   (2048 rows = 4 conds × 256 × 2)
#   outputs/s31_gated_compare_verdict.md    (the headline answer)
```

---

## 7. FILES CREATED / MODIFIED THIS SESSION

### Modified (0):

S31 did NOT modify any source files (rule 6 / S30 §5.5 honored).
`honest_backtest_xauusd_v2.cpp` remains 1140 lines (S30 final
state).

### Created (scripts, 1):

- `scripts/duka_xau_s31_gated_compare.py` — 272-line aggregator
  that reads 4 condition CSVs (ungated 623d, spread-only 623d,
  ungated 19d L2, gated 19d L2), computes 1000-iter bootstrap
  CI95 per (tp, sl, z, window, fill_model) per condition, and
  emits a unified summary CSV + verdict MD. Net of Prime $0.06/RT.

### Created (data outputs, 4):

- `outputs/wide_fine_l2_gated.csv` — 9728 data rows, 19 L2 days
  × 256 cells × 2 fills under full L2 + regime gates.
- `outputs/wide_fine_l2_ungated.csv` — 9728 data rows, same 19
  L2 days ungated (apples-to-apples baseline).
- `outputs/duka_wide_fine_asia_gated_spread.csv` — 318,977 data
  rows, 623 Dukascopy days × 256 cells × 2 fills with
  spread-only gate (`--gated --imb-long 0.5 --imb-short 0.5`).
- `outputs/s31_gated_compare_summary.csv` — 2048 rows, 4
  conditions × 256 cells × 2 fills aggregated.

### Created (verdict, 1):

- `outputs/s31_gated_compare_verdict.md` — **the answer to S30 §3.1**.

### Created (staging dir, 1):

- `outputs/l2_xau_daily/` — 19 symlinks to L2 XAU CSVs (15 from
  /Users/jo/omega_repo/data/, 4 from /Users/jo/Tick/l2_data/
  after part-split de-dup). Pre-staged so future L2 sweeps can
  iterate directly without re-inventorying.

### Created (handoff, 1):

- `HANDOFF_S31_AFTER_S30.md` — this file.

### Not touched (rule 6 compliance):

All protected core engine files, all S26P4/S27/S28/S29/S30
scripts and binaries (including the unchanged `_s30` binary),
`omega_config.ini`, the S30 `duka_xau_fine_aggregate.py`,
`duka_xau_fine_validate.py`, all sweep CSVs from S30, etc.

---

## 8. NEXT-SESSION FIRST-MESSAGE TEMPLATE

> Read `HANDOFF_S31_AFTER_S30.md`, `HANDOFF_S30_AFTER_S29.md`,
> `HANDOFF_S29_AFTER_S28.md`, `HANDOFF_S28_AFTER_S27.md` end-to-end.
> Confirm on-disk state (`git status`, mode=SHADOW,
> max_lot_gold=0.01, _s30 binary 70,136 B). Verify s30 regression
> on day 2024-05-22 still md5-matches `5b22cd55…`.
>
> The §3.1 question now has a mixed answer (UNCHANGED on spread-only
> 623d, KILL on full-L2 19d). Next priorities from S31 §3:
> 1. Cooldown + latency sensitivity for TOP-1 (~25 min wall)
> 2. Gate-and-geometry co-optimization on the 19-day L2 corpus
>    (either a small harness extension or a fine mesh around the
>    gated TOP-1 cell)
> 3. Find or build more L2 history (S30 §3.5 tick-only regime
>    classifier is the long path)
> 4. Cross-currency port of the winning geometry
> 5. Explicit Python replay verification of TOP-1
>
> Do NOT recommend live deployment regardless of result. The §3.1
> verdict does NOT clear the deployment validation cascade.

---

## 9. CONTEXT BUDGET WARNING (S30 rule 7 compliance)

S31 stayed comfortably under the operator's 70% threshold. No
source file was modified, so no full-file paste was needed. All
new outputs are on disk and re-readable in future sessions.

If the operator wants to see the new aggregator script or the
verdict MD in chat in a future session, both are at:

- `/Users/jo/omega_repo/scripts/duka_xau_s31_gated_compare.py`
- `/Users/jo/omega_repo/outputs/s31_gated_compare_verdict.md`

---

End of S31 handoff.

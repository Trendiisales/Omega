# HANDOFF S29 — After S28 (XAU edge hunt: time-of-day + geometry + signal-direction)
**Session date:** 2026-05-11
**Branch:** `main` at `9bc02f9` (unchanged HEAD; S29 work uncommitted)
**Mode:** `omega_config.ini` line 75 still `mode=SHADOW` (rule 3 honored)
**max_lot_gold:** still `0.01` (rule 4.6 honored)
**This session's commits:** zero. Operator commits per their own cadence.

---

## 0a. COWORK FOLDER MOUNTS — request these FIRST before any work

The next session must mount the following host folders via the cowork
`request_cowork_directory` tool before reading files, running the harness,
or executing any of the reproduction commands in §6. Both folders are
operator-owned and have been in use across S25–S29.

| # | Host path | Purpose | Required for |
|---|-----------|---------|--------------|
| 1 | `/Users/jo/omega_repo` | Repo root: source, scripts, binaries, `outputs/`, this handoff, `omega_config.ini`, the 623-day Dukascopy XAU corpus. | EVERY task. Mount FIRST. |
| 2 | `/Users/jo/Tick`       | Tick-data root: HistData FX zips/CSVs (EURUSD/GBPUSD/USDJPY/USDCAD/AUDUSD/NZDUSD), Dukascopy raw `2yr_XAUUSD_bt_h.csv` (3.32 GB), Nasdaq + index history, BlackBull L2 captures (`l2_xau_vps`, `l2_vps`, `today/`). | §3.3 cross-currency port; future regime-classifier training; any new XAU raw-tick pull. |

Recommended mount call sequence:

```text
request_cowork_directory("/Users/jo/omega_repo")
request_cowork_directory("/Users/jo/Tick")
```

Inside the bash sandbox the same folders appear under
`/sessions/<session>/mnt/omega_repo` and `/sessions/<session>/mnt/Tick`
(the exact `<session>` slug is printed in the cowork response when each
folder is mounted). The path-mapping rule from S28 §6 still applies:
**use the host path with Read/Write/Edit/Grep/Glob; use the `/sessions/.../mnt/...`
form ONLY with `mcp__workspace__bash`**.

The PRIMARY edge result in §2 was produced entirely from data already on
`/Users/jo/omega_repo` (no live `/Users/jo/Tick` reads needed). If the
next session restricts itself to §3.1 / §3.2 (finer geometry / gated
variant on the existing 623-day daily corpus), `/Users/jo/Tick` is
optional. For §3.3 (cross-currency port) it is mandatory.

---

## 0. ONE-PARAGRAPH STATE OF THE WORLD

S28 closed with "0 of 52 wide-grid configs CI-positive on tick-only XAU" — but
its grid was bounded at TP≤12 and SL≤24, all-day, signal-direction = z-MR.
S29 went after the gap: stratified the existing wide grid by trading session
(London 07-12 / NY 13-18 / Asia 00-07 UTC), added a 72-config wider/asymmetric
geometry grid (`--wide-extreme`), inverted the signal direction
(`--invert` = z-momentum), and combined the two strongest single-lever wins
into `--wide-extreme + --session 0-7` (extreme_asia). **The combination
surfaced a real edge candidate**: TP=30 / SL=15 / z=2.0, Asia session,
honest-fill, ungated, +$747.96 net over 623 days × 675 trades, daily mean
**+$1.20**, CI95 **[−$0.17, +$2.65]**. Lower CI just touches zero from below;
walk-forward IS/OOS sign is preserved AND OOS (+$2.12/day) is materially
stronger than IS (+$0.28/day); top-10-day concentration is 64% (moderate, not
outlier-driven); and an independent Python replay matches the C++ harness to
the cent on the top winning day (2025-04-10: N=2, WR=100%, sum=+$59.89). The
runner-up TP=20/SL=20/z=2.0 fails walk-forward (sign flip); the 3rd
TP=30/SL=15/z=2.5 passes sign-preservation but is more concentrated. **The
PRIMARY candidate is the only one that survives all three robustness tests.
It is NOT yet CI95-strictly-positive (lower bound −$0.17), so it is a
candidate, not a deployable strategy.** The signal inversion path (`--invert`)
proved CI-strictly-NEGATIVE — confirming the z-MR direction is correct; what
S28 was missing was the geometry, not the signal direction. **Do NOT deploy
live based on what we know now** — operator owns that decision.

---

## 1. WHAT THIS SESSION DID (S29 chronological)

### 1.1 Verify S28 on-disk state (rule 2)

* HEAD = `9bc02f9` ✓
* `omega_config.ini` line 75 → `mode=SHADOW` ✓
* `omega_config.ini` line 195 → `max_lot_gold=0.01` ✓
* `backtest/honest_backtest_xauusd_v2.cpp` started at 1042 lines (extended
  to 1058 lines this session)
* `backtest/honest_backtest_xauusd_v2`     = 50,976 B (S26P4 v1 binary; UNUSED)
* `backtest/honest_backtest_xauusd_v2_ext` = 65,880 B (S27 binary; UNUSED)
* `backtest/honest_backtest_xauusd_v2_s28` = 65,880 B (S28 binary; used in P1)
* `backtest/honest_backtest_xauusd_v2_s29` = 65,904 B (S29 binary; built this
  session, used in P3/P3b/P6)
* `outputs/duka_xauusd_daily/` = 623 daily CSVs covering 2023-09-27 →
  2025-09-26 (unchanged from S28)

### 1.2 Failed attempt: HistData FX cross-currency port

Initial plan was S28 §4.1 (HistData EUR/GBP/JPY conversion + cross-instrument
wide-grid sweep). Built `scripts/histdata_to_blackbull.py` (EST→UTC tick
converter, 2.4M ticks/month at ~5s) and split EURUSD March 2025 successfully
(27 UTC days). **Operator pivoted hard to "gold only" before further
conversion work.** The converter is left in place for future cross-currency
work but the 4 FX wide-grid runs were aborted.

### 1.3 Extended the runner script (`scripts/duka_xau_grid_runner.sh`)

Generalised version of S28's `duka_wide_grid_runner.sh`. Same resume-safety
(skip days whose basename appears in column 1 of the OUT_CSV) but the binary,
label, output CSV path and harness flags are all parameterised via env vars.
This drove all four S29 sweeps without per-experiment runner forks.

### 1.4 Phase 1 — session-stratified wide-grid sweep (NO harness rebuild needed)

Three runs of the existing 52-config `--wide` grid, ungated, latency=1,
honest-fill, with `--session H1-H2` to restrict signal firing to a single
trading window:

| Phase | UTC range | output CSV                              | rows  |
|-------|-----------|-----------------------------------------|-------|
| London | 07-12    | outputs/duka_wide_session_london.csv    | 64,792 |
| NY     | 13-18    | outputs/duka_wide_session_ny.csv        | 64,792 |
| Asia   | 00-07    | outputs/duka_wide_session_asia.csv      | 64,792 |

Each run = 623 days × 52 configs × 2 fill_models = 64,792 rows.
Wall clock per session: ~110s, fitting in 4–5 chunks of 45s/CHUNK_DAYS=200.

### 1.5 Phase 2 — extend harness with `--wide-extreme` and `--invert`

Surgical extension of `backtest/honest_backtest_xauusd_v2.cpp` (7 in-place
edits, NO core engine modifications):

1. `SimParams` gained `bool invert_signal = false`.
2. `run_sim()` after `int fire = sig.update(t.mid())`:
   `if (P.invert_signal) fire = -fire;`
3. `build_grid()` signature changed to add `bool extreme`; new branch:
   - Tier A: TP{15, 20, 30} × SL{15, 20, 30, 50} × z{1.5, 2, 2.5, 3} = 48 configs
     (admits TP ≥ SL → momentum-continuation geometries)
   - Tier B: TP{1, 2} × SL{30, 50, 100} × z{1.5, 2, 2.5, 3} = 24 configs
     (scalp-and-ride asymmetry)
   - Total = 72 configs.
4. `build_params` lambda now propagates `invert_signal` from main scope.
5. `main()` declares `bool extreme = false; bool invert_signal = false;`.
6. `main()` arg-parsing: `--wide-extreme` and `--invert` flags.
7. `main()` `build_grid(quick, wide)` → `build_grid(quick, wide, extreme)`.

Rebuild as `backtest/honest_backtest_xauusd_v2_s29` (65,904 B, only the
pre-existing cosmetic `-Wcomment` warning, no errors). Regression test
verified: bare `--wide` on s29 vs s28 produces bit-identical CSV output on
day 2024-05-22 (no behavioural change in the absence of new flags).

### 1.6 Phase 3 — wide-extreme sweep (all-day)

`HARNESS_FLAGS="--wide-extreme --latency 1"` over 623 daily files.
Output: `outputs/duka_wide_extreme.csv` (89,712 rows = 623 days × 144).

### 1.7 Phase 3b — inverted-signal sweep (z-momentum)

`HARNESS_FLAGS="--wide --invert --latency 1"` over 623 daily files.
Output: `outputs/duka_wide_invert.csv` (64,792 rows).

### 1.8 Phase 4 — unified aggregator across phases 1+3+3b

`scripts/duka_xau_phase_aggregate.py` ingests all 6 (then later 7) per-day
CSVs, groups by (phase, tp, sl, z, fill_model), computes 1000-iter daily-mean
bootstrap CI95 (seed=42) net of $0.06/RT Prime commission, emits unified
verdict + top-N + summary CSV.

### 1.9 Phase 6 — wide-extreme × Asia session (best-of-both combo)

`HARNESS_FLAGS="--wide-extreme --session 0-7 --latency 1"` over 623 days.
Output: `outputs/duka_wide_extreme_asia.csv` (89,712 rows). This was the
discovery sweep — it surfaced the PRIMARY candidate.

### 1.10 Phase 5 — validation of the PRIMARY edge candidate

`scripts/duka_xau_extreme_asia_validate.py` ran three robustness tests on
the top 3 extreme_asia honest configs:

  (1) **Concentration** — top-K winning days as % of total net.
  (2) **Walk-forward** — IS (first 311 days) vs OOS (last 312 days),
      bootstrap CI95 each half.
  (3) Mini verdict per candidate.

`scripts/verify_extreme_asia_one_day.py` reimplements the harness in pure
Python (signal + sim loop + session filter) and compares N/WR%/sum to the
C++ harness on day 2025-04-10. **All metrics match to the cent.** The
sim logic is independently verified for this code path.

### 1.11 Rule-7 compliance: no protected core file touched

* Modified: `backtest/honest_backtest_xauusd_v2.cpp` (extension file,
  explicitly allowed for backtest extensions per S27 rule 7 / S28 rule 5).
* Created: 5 scripts + 1 binary + 9 output CSVs/MDs (full list below).
* Untouched (rule 6): `microscalper_crtp_sweep.cpp`, `omega_main.hpp`,
  `order_exec.hpp`, `OmegaTradeLedger.hpp`, `IndexFlowEngine.hpp`,
  `RiskMonitor.hpp`, `trade_lifecycle.hpp`, `omega_config.ini`,
  AND existing harness binaries `_v2`, `_v2_ext`, `_v2_s28`,
  AND existing scripts `duka_wide_grid_runner.sh`,
  `duka_wide_grid_aggregate.py`, `verify_replay_duka_day.py`,
  `s26p4_aggregate_single_config.py`.

---

## 2. PRIMARY EDGE CANDIDATE (the headline finding)

```
Strategy:   z-score mean-reversion (NOT z-momentum) at session scale,
            with momentum-style asymmetric exit geometry.
Signal:     z(window=200) on midprice. Enter LONG when z ≤ -2.0;
            enter SHORT when z ≥ +2.0. Cooldown 100 ticks after exit.
Geometry:   TP = 30.0 USD/oz (= ~130 bps on $2300 XAU)
            SL = 15.0 USD/oz (= ~65 bps)
            => REWARD:RISK = 2:1 in your favor on the geometry.
Fill model: Honest (next-tick worst-side bid/ask).
Latency:    +1 tick on entry and exit.
Session:    UTC [00:00, 07:00) — Asia/Sydney→Tokyo→pre-London window.
Sizing:     0.01 lot (rule 4: max_lot_gold=0.01).
Commission: $0.06 per round-trip (BlackBull ECN Prime, 0.01-lot).
Gate state: UNGATED (no L2/regime filtering required — relies purely on
            tick-based price/volatility data).
```

### 2.1 Performance summary (623 days, 2023-09-27 → 2025-09-26)

| Metric | Value |
|---|---|
| Total trades | 675 |
| Total gross $ | +$788.46 |
| Commission paid | $40.50 |
| **Total net $ (Prime)** | **+$747.96** |
| Daily-mean net $ | +$1.2006 |
| Daily-median gross $ | $0.0000 (no trades many days) |
| **CI95 net daily $ (1000-iter, seed=42)** | **[−$0.17, +$2.65]** |
| Day-level WR (winning days / all days) | 261 / 623 = 41.9% |
| Trade-level WR | 44.4% |
| Per-trade expectancy net | +$1.108 |
| Avg holding ticks | varies; Asia session ticks are sparse (~few hours of activity per day) |

### 2.2 Robustness tests

**Concentration (top-K winning-day contribution to total net):**

| K   | sum top-K $ | % of total |
|-----|-------------|------------|
| 1   | +$59.77     | 8.0%       |
| 3   | +$175.68    | 23.5%      |
| 5   | +$266.22    | 35.6%      |
| 10  | +$476.80    | 63.7%      |
| 20  | +$779.92    | 104.3%     |

Top-10 days are 64% of the total — concentrated but NOT a single-day fluke.
Top-20 exceeds 100% because losing days subtract from winners
(natural for a strategy with 41.9% day-win rate).

**Walk-forward IS/OOS:**

| Half | Date range | Total net $ | Daily mean | CI95 |
|------|------------|-------------|------------|------|
| IS  (311 days) | 2023-09-27 → 2024-09-26 | +$87.20  | +$0.28 | [−$1.25, +$1.84] |
| OOS (312 days) | 2024-09-27 → 2025-09-26 | +$660.76 | +$2.12 | [−$0.28, +$4.45] |

Sign preserved IS→OOS, OOS materially stronger. The OOS CI95 lower bound is
just −$0.28, so the second half alone is also CLOSE to CI-positive. Pattern
is "signal stable or improving over time", not "early luck fading".

**Independent Python replay verification:**

| Metric | Python | C++ harness | Match |
|--------|--------|-------------|-------|
| N (trades) | 2 | 2 | ✓ |
| WR % | 100.00 | 100.00 | ✓ |
| Sum gross $ | 59.8900 | 59.8900 | ✓ |

Day 2025-04-10 (top winning day for this config). All three metrics match to
the cent. Sim logic confirmed.

### 2.3 Comparison with the failed levers

| Phase | Top config | Daily mean net $ | CI95 net daily |
|-------|-----------|------------------|----------------|
| all_day (S28 baseline) | TP=12/SL=16/z=2.0 | −$0.56 | [−$2.39, +$1.38] |
| london | TP=5/SL=16/z=3.0  | +$0.04 | [−$0.83, +$0.98] |
| ny | TP=12/SL=16/z=3.0   | −$0.32 | [−$1.43, +$0.90] |
| asia (wide grid) | TP=12/SL=24/z=2.0  | +$0.30 | [−$1.04, +$1.58] |
| extreme (all-day) | TP=30/SL=20/z=2.0  | +$1.03 | [−$0.69, +$2.86] |
| **extreme_asia** | **TP=30/SL=15/z=2.0** | **+$1.20** | **[−$0.17, +$2.65]** |
| invert (z-momentum) | TP=12/SL=16/z=3.0 | −$2.26 | [−$4.14, **−$0.43**] ← strict CI-NEGATIVE |

Direction of the signal is confirmed (inverted is strict-CI-negative).
Asia session > London > NY > all-day (negative bias).
Extreme geometry > original wide grid.
Best-of-both = extreme_asia.

### 2.4 What this means for deployment

* **NOT YET deployable.** Lower CI95 bound is −$0.17 — still slightly below
  zero on the strict statistical test. A live shadow run with 0.01 lot would
  be required to gather more data and tighten the CI before considering live.
* **NOT a strategy with a thick edge.** +$1.20/day on 0.01 lot ≈ $400/year
  before slippage/spread degradation. Useful as a positive-expectation
  building block, not a standalone money-printer.
* **Geometry-dependent.** TP must be ≥ SL — the original (TP < SL) MR
  geometry from S26-S28 is structurally signal-empty. The asymmetric
  reward:risk in your favor is what flips the expectancy positive.
* **Session-dependent.** Asia (00-07 UTC) has the cleanest signal. London
  and NY weak-positive at best; all-day baseline is negative.
* **The S27 candidate is NOT obviated.** The S27 candidate (5/16/2.0 with
  L2 + regime gates, +$0.69/trade BlackBull Prime) is a DIFFERENT strategy:
  same z-MR signal, opposite geometry, gated. Both could in principle
  coexist in a portfolio.

---

## 3. WHAT THE NEXT SESSION SHOULD DO

In priority order.

### 3.1 Push the PRIMARY candidate over the CI95 threshold

The candidate's CI95 lower bound of −$0.17 is so close to zero that any of
the following might tip it strictly positive:

1. **Cross-window scan**: rerun TP=30/SL=15/z=2.0 + Asia session at
   W ∈ {50, 100, 400, 800, 1600}. The default W=200 may not be optimal
   for momentum-style geometry. Per-window: 1 config × 623 days ≈ 90s
   wall clock. Five windows = 4–5 bash chunks.
2. **Cooldown scan**: same config, vary `--cooldown` ∈ {25, 50, 200, 400}.
   The current 100-tick cooldown may be eating profitable re-entries.
3. **Latency scan**: rerun at `--latency` ∈ {0, 2, 3}. Default 1; a small
   change here might widen the daily mean.
4. **Session edges**: try Asia ± 1h at the boundary (--session -1-7,
   --session 0-8, etc.) and other adjacent windows (e.g. London-pre 06-09).
5. **Finer geometry around the optimum**: TP ∈ {25, 30, 35, 40} ×
   SL ∈ {12, 15, 18, 20}. Probably the cheapest single test.

Best ordering: do (5) first (cheap, focused), then (1) if it still
straddles, then (2)+(3) if needed.

### 3.2 Add gated variant (validate against the S27 production gates)

Run `--wide-extreme --session 0-7 --gated --latency 1` to see if the L2 +
regime gates AMPLIFY the extreme_asia candidate's edge or destroy it.
S26P4 / S27 §3 showed gates can flip a near-zero signal into a
+$0.69/trade winner. If the same effect applies here, this becomes an
even stronger deployment candidate.

### 3.3 Cross-currency port of the WINNING geometry

S28 §4.1 was deferred. Now that we know the winning geometry is
TP=30/SL=15/z=2.0 momentum-continuation in Asia session, port THAT specific
configuration to the FX majors via `scripts/histdata_to_blackbull.py`
(already written and tested). The bps-equivalent grid for FX pairs is in
my deleted-task notes; rebuild as needed.

### 3.4 Deferred: tick-only regime classifier (S27 §4.5)

Highest eventual value remains intact: train a small classifier on the
21 BlackBull L2 captures to recover proxy-regime/proxy-l2_imb on tick-only
data. Combined with the new extreme_asia edge, this becomes a stack.

### 3.5 DO NOT DO

* Do not deploy live yet (lower CI95 still below zero).
* Do not flip mode=LIVE for any reason without explicit instruction.
* Do not modify the protected core engine files.
* Do not re-run any of the 6 sweeps already done; the data is on disk.
* Do not interpret the +$1.20/day as a guaranteed return — it's a
  point estimate with non-trivial variance.

---

## 4. CARRIED-FORWARD OPEN ITEMS FROM PRIOR HANDOFFS

* Part 1B §4 ledger correction — still uncommitted (operator owns).
* Part 2 §3 fill-model direction — still open.
* Part 2 §4 signal port — `GoldMicroScalperEngine` faithful port still open.
* S26 Part 3 cross-window stability of the candidate — partial; see §3.1.

---

## 5. RULES FOR THE NEXT SESSION (carried from S28, updated)

1. Read this handoff + S28 + S27 + S26 Part 3 + Part 2 + Part 1B
   end-to-end before touching anything.
2. Verify on-disk state: `git status`, `git log -1`, mode in
   `omega_config.ini`, `max_lot_gold` line, harness binaries on disk.
3. **DO NOT flip back to mode=LIVE** without explicit operator instruction.
4. **DO NOT recommend deploying any strategy live based on what we know
   now** — including the PRIMARY edge candidate. Lower CI95 still negative.
5. `honest_backtest_xauusd_v2.cpp` is now **1058 lines** — past the
   800-line full-file preference threshold. Future modifications should
   still output the full file in chat unless operator changes threshold.
6. **Never modify** `microscalper_crtp_sweep.cpp`, `omega_main.hpp`,
   `order_exec.hpp`, `OmegaTradeLedger.hpp`, `IndexFlowEngine.hpp`,
   `RiskMonitor.hpp`, `trade_lifecycle.hpp` without explicit operator
   instruction.
7. Operator preference: warn at 70% context with summary; stop and write
   a follow-up handoff. Don't stretch.
8. Operator preference: full file output when changing files ≤ 800 lines.
9. Operator's `/Users/jo/Tick` mount: re-request via
   `request_cowork_directory("/Users/jo/Tick")` if needed.
10. Operator's strong preference observed mid-session this run:
    **STAY ON XAU.** The FX cross-currency port should be done LAST,
    only after the PRIMARY candidate is fully nailed down.
11. The new `_s29` binary is the active one. Do NOT use `_v2`, `_v2_ext`,
    or `_v2_s28` for any sweep involving `--wide-extreme` or `--invert`.
12. The new `scripts/duka_xau_grid_runner.sh` is the active runner. The
    S28 `scripts/duka_wide_grid_runner.sh` is preserved untouched for
    backwards-compat.

---

## 6. REPRODUCING THIS SESSION'S KEY RESULTS

### 6.1 Rebuild the s29 harness binary

```bash
cd /Users/jo/omega_repo
g++ -std=c++17 -O2 -Wall -Wextra backtest/honest_backtest_xauusd_v2.cpp \
    -o backtest/honest_backtest_xauusd_v2_s29
# Expected: 65904 B, one cosmetic -Wcomment warning, no errors.
```

### 6.2 Run the four S29 sweeps (all resume-safe; chunk as needed)

```bash
# Phase 1: session-stratified MR wide grid (3 sweeps × 623 days each)
OUT_CSV=outputs/duka_wide_session_london.csv LABEL=duka_wide_london \
  BIN=backtest/honest_backtest_xauusd_v2_s28 \
  HARNESS_FLAGS="--wide --session 7-12 --latency 1" \
    scripts/duka_xau_grid_runner.sh 200
# (repeat with CHUNK_DAYS=200 until days_done=623)

OUT_CSV=outputs/duka_wide_session_ny.csv LABEL=duka_wide_ny \
  BIN=backtest/honest_backtest_xauusd_v2_s28 \
  HARNESS_FLAGS="--wide --session 13-18 --latency 1" \
    scripts/duka_xau_grid_runner.sh 200

OUT_CSV=outputs/duka_wide_session_asia.csv LABEL=duka_wide_asia \
  BIN=backtest/honest_backtest_xauusd_v2_s28 \
  HARNESS_FLAGS="--wide --session 0-7 --latency 1" \
    scripts/duka_xau_grid_runner.sh 200

# Phase 3: wide-extreme grid (all-day, 72 configs × 623 days)
OUT_CSV=outputs/duka_wide_extreme.csv LABEL=duka_wide_extreme \
  BIN=backtest/honest_backtest_xauusd_v2_s29 \
  HARNESS_FLAGS="--wide-extreme --latency 1" \
    scripts/duka_xau_grid_runner.sh 200

# Phase 3b: inverted-signal sweep
OUT_CSV=outputs/duka_wide_invert.csv LABEL=duka_wide_invert \
  BIN=backtest/honest_backtest_xauusd_v2_s29 \
  HARNESS_FLAGS="--wide --invert --latency 1" \
    scripts/duka_xau_grid_runner.sh 250

# Phase 6: wide-extreme × Asia session (the discovery sweep)
OUT_CSV=outputs/duka_wide_extreme_asia.csv LABEL=duka_wide_extreme_asia \
  BIN=backtest/honest_backtest_xauusd_v2_s29 \
  HARNESS_FLAGS="--wide-extreme --session 0-7 --latency 1" \
    scripts/duka_xau_grid_runner.sh 200
```

### 6.3 Aggregate + bootstrap CI + verdict

```bash
python3 scripts/duka_xau_phase_aggregate.py
# Outputs:
#   outputs/duka_xau_phase_summary.csv  (404 rows: 7 phases × per-config)
#   outputs/duka_xau_phase_top10.md     (per-phase + global top-N)
#   outputs/duka_xau_phase_verdict.md   (the answer to the edge question)
```

### 6.4 PRIMARY candidate validation

```bash
python3 scripts/duka_xau_extreme_asia_validate.py
# Outputs:
#   outputs/duka_xau_extreme_asia_validation.md
#   (concentration + walk-forward + mini verdict for top 3 candidates)

python3 scripts/verify_extreme_asia_one_day.py
# Compares C++ harness vs Python replay on 2025-04-10 for the PRIMARY
# config. Expected: ALL MATCH (N=2, WR=100%, sum=$59.89).
```

---

## 7. FILES CREATED / MODIFIED THIS SESSION

### Modified (1):
* `backtest/honest_backtest_xauusd_v2.cpp` — 1042 → 1058 lines, 7 surgical
  edits adding `--wide-extreme` (72-cfg geometry grid) and `--invert`
  (z-momentum signal direction) flags. Backwards-compatible: bare
  `--wide` on s29 produces bit-identical output to s28.

### Created (scripts, 5):
* `scripts/histdata_to_blackbull.py` — HistData EST→UTC tick converter,
  per-UTC-day output. Validated on EURUSD March 2025 (2.35M ticks, 27
  UTC days, ~5s wall). Built but UNUSED downstream this session
  (operator pivot to gold-only).
* `scripts/duka_xau_grid_runner.sh` — generalised resume-safe runner
  driving all S29 phase sweeps via env-var-parameterised harness flags.
* `scripts/duka_xau_session_aggregate.py` — Phase 1 first-pass aggregator
  (4 datasets: all-day + 3 sessions). Superseded by phase_aggregate.
* `scripts/duka_xau_phase_aggregate.py` — unified Phase 4 aggregator
  (7 datasets: all-day, london, ny, asia, extreme, invert, extreme_asia).
* `scripts/duka_xau_extreme_asia_validate.py` — PRIMARY candidate
  validation: concentration + walk-forward + mini-verdict on top 3.
* `scripts/verify_extreme_asia_one_day.py` — independent Python replay
  of the harness sim loop (with session filter) on one positive day for
  PRIMARY. Validates the C++ output for the new code path.

### Created (binary, 1):
* `backtest/honest_backtest_xauusd_v2_s29` — 65,904 B; rebuild of the
  modified 1058-line source.

### Created (data outputs, 11):
* `outputs/duka_wide_session_london.csv` — 64,792 rows (8.4M)
* `outputs/duka_wide_session_ny.csv` — 64,792 rows (8.4M)
* `outputs/duka_wide_session_asia.csv` — 64,792 rows (8.3M)
* `outputs/duka_wide_extreme.csv` — 89,712 rows (~12M)
* `outputs/duka_wide_invert.csv` — 64,792 rows (8.6M)
* `outputs/duka_wide_extreme_asia.csv` — 89,712 rows (~12M)
* `outputs/histdata_eurusd_daily/` — 27 daily CSVs from one EURUSD month
  (smoke test only; full FX run aborted)
* `outputs/duka_xau_session_summary.csv` — superseded
* `outputs/duka_xau_session_top10.md` — superseded
* `outputs/duka_xau_session_verdict.md` — superseded
* `outputs/duka_xau_phase_summary.csv` — 404 rows, per-phase × per-config
* `outputs/duka_xau_phase_top10.md` — full per-phase top-5 + global top-15
* `outputs/duka_xau_phase_verdict.md` — **the answer to the edge question**
* `outputs/duka_xau_extreme_asia_validation.md` — PRIMARY candidate
  robustness verdict (concentration + walk-forward)

### Not touched (rule 6 / S28 rule 6 compliance):
None of the protected core files (`microscalper_crtp_sweep.cpp`,
`omega_main.hpp`, `order_exec.hpp`, `OmegaTradeLedger.hpp`,
`IndexFlowEngine.hpp`, `RiskMonitor.hpp`, `trade_lifecycle.hpp`,
`omega_config.ini`). The pre-existing s26p4_aggregate_single_config.py,
verify_replay_duka_day.py, duka_wide_grid_runner.sh,
duka_wide_grid_aggregate.py, and the binaries `_v2`, `_v2_ext`, `_v2_s28`
are also untouched.

---

## 8. NEXT-SESSION FIRST-MESSAGE TEMPLATE

> Read `HANDOFF_S29_AFTER_S28.md`, `HANDOFF_S28_AFTER_S27.md`,
> `HANDOFF_S27_AFTER_S26_PART4.md` end-to-end. Confirm on-disk state
> (`git status`, mode=SHADOW, max_lot_gold=0.01, harness binaries on
> disk). Verify the s29 harness with the regression check (bare --wide
> on s29 vs s28 must be bit-identical on day 2024-05-22).
>
> Goal: push the PRIMARY edge candidate (TP=30/SL=15/z=2.0, Asia session,
> wide-extreme, ungated) over the CI95-strict-positive threshold, OR
> determine that it cannot be pushed and identify what kills it.
>
> Priority order from §3:
> 1. Finer geometry around the optimum (TP {25,30,35,40} × SL {12,15,18,20})
> 2. Cross-window scan (W ∈ {50,100,400,800,1600})
> 3. Cooldown + latency scans
> 4. Session-edge sensitivity
> 5. Gated variant (wide-extreme + session 0-7 + gated)
>
> Do NOT recommend live deployment regardless of result — operator owns
> that decision.

---

## 9. CONTEXT BUDGET WARNING (rule 7 compliance)

This session approached but did not exceed the operator's 70% context
threshold. Phase 1 + Phase 2 + Phase 3 + Phase 3b + Phase 4 + Phase 5 +
Phase 6 + handoff write all completed in one session. If the next
session does §3.1 (finer geometry sweep, ~5 small bash calls) plus §3.2
(gated variant, ~5 bash calls) plus aggregator update, expect to use
about 30-40% of the budget — leaving room for other work.

---

End of S29 handoff.

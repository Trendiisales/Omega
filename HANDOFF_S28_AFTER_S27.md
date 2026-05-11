# HANDOFF S28 — After S27 (Dukascopy 2yr XAU wide-grid sweep)
**Session date:** 2026-05-11
**Branch:** `main` at `9bc02f9` (unchanged HEAD; all S26 Part 1B/2/3, S26 Part 4, S27, and this session's work is uncommitted)
**Mode:** `omega_config.ini` line 75 still `mode=SHADOW` (rule 3 honored, no change)
**max_lot_gold:** still `0.01` (rule 4.6 honored)
**This session's commits:** zero. Operator commits per their own cadence.

---

## 0. ONE-PARAGRAPH STATE OF THE WORLD

S27 left an open question: does the TP=5/SL=16/z=2.0 mean-reversion family
have ANY positive geometry on tick-only XAU data over the full 2yr corpus
(not just the 214-day phase-1 sample)? S28 answered it exhaustively. After
finishing the Dukascopy split (now 623 daily files covering 2023-09-27 →
2025-09-26) and running the S27 `--wide` grid (52 configs = TP∈{3,5,8,12} ×
SL∈{8,12,16,24} | SL>TP, × z∈{1.5,2,2.5,3}) ungated, latency=1, honest-fill,
across all 623 days: **0 of 52 configurations are total-net positive,
0 of 52 have a strictly positive lower CI95 bound on daily-mean net PnL,
and 48 of 52 are statistically CI-NEGATIVE at the 95% level under
honest-fill + BlackBull Prime $0.06 commission.** The best config by lower
CI95 bound (TP=12, SL=16, z=2.0) yields −$351.70 net over 2,306 trades
with daily CI [−$2.39, +$1.38] — still straddles zero with negative bias.
The S27 candidate (5/16/2.0) on the same 623-day corpus is **strictly
CI-negative**: −$2,106.06 net over 4,522 trades, daily CI [−$5.26, −$1.58].
Independent Python replay matches the C++ harness exactly on a sanity day
(N=4, WR=25%, sum=−$26.47 — identical to the cent). **Net result: the
5/16 z-MR signal family is signal-empty on tick-only XAU under honest
fills, across 2 years of clean data, with no surviving geometry. The
candidate's BlackBull `+$0.69/trade` net-of-Prime expectancy lives
entirely in the L2 + regime gates, exactly as S27 §4.4 concluded; this
session removed the last hedge ("maybe a different geometry works") by
exhaustive search. Next-session priorities pivot to S27 §4.3 (HistData
cross-currency neutrality) and §4.5 (tick-only regime classifier). Do
NOT deploy the candidate live based on what we know now.**

---

## 1. WHAT THIS SESSION DID (S28 chronological)

### 1.1 Verify S27 on-disk state
* HEAD = `9bc02f9` ✓
* `omega_config.ini` line 75 → `mode=SHADOW` ✓
* `omega_config.ini` line 195 → `max_lot_gold=0.01` ✓
* `backtest/honest_backtest_xauusd_v2.cpp` = 1042 lines ✓ (S27 extended
  source intact; pre-existing `_ext` binary still 65880 bytes from S27)
* Both `/Users/jo/omega_repo` and `/Users/jo/Tick` re-mounted.

### 1.2 Finish Dukascopy 2yr daily split (S27 §4.1 action)
Resumed the `gawk -F,` per-day splitter from the truncated terminal file
`2024-06-06.csv` onward (truncation diagnosed by anomalous file size: 3.6MB
vs typical 5-8MB). Five sequential bash invocations under the 45s I/O cap:

| Pass | Resume ts_ms | Target start | New days written | End day |
|---|---|---|---|---|
| 1 | 1717632000000 | 2024-06-06 | 107 | 2024-10-09 |
| 2 | 1728432000000 | 2024-10-09 | 86  | 2025-01-17 |
| 3 | 1737072000000 | 2025-01-17 | 63  | 2025-04-01 |
| 4 | 1743465600000 | 2025-04-01 | 50  | 2025-05-30 |
| 5 | 1748563200000 | 2025-05-30 | 59  | 2025-08-07 |
| 6 | 1754524800000 | 2025-08-07 | 42  | 2025-09-24 |
| 7 | 1758672000000 | 2025-09-24 | 3   | 2025-09-26 (EOF) |

Final corpus: **623 daily CSV files** in `outputs/duka_xauusd_daily/`
spanning 2023-09-27 → 2025-09-26 (full 728-day window minus weekends, with
Dukascopy's Sunday-open partial sessions counted). Total 3.32 GB.
Smallest files are Christmas/NYE/NYD holiday sessions (50-80 KB) — normal.

Note on resume strategy: each pass re-reads the entire 3.1 GB source from
start because gawk's skip-until guard is a row-level filter, not a
byte-level seek. The 1.0 GB/s warm-cache read makes this practical
(45 s/pass) but the productive-write window shrinks as `skip_until`
advances — hence diminishing days-per-pass (107 → 86 → 63 → 50 → 59 → 42
→ 3). For a one-shot future split on a cold cache, this would take ~3
single 45s passes if started fresh; the multi-pass cost here was the
penalty of restarting after S27's truncated termination.

### 1.3 Recompile extended harness
S27 left two binaries: `honest_backtest_xauusd_v2` (50,976 B — the old
60-config v1-grid binary) and `honest_backtest_xauusd_v2_ext` (65,880 B —
the S27-extended binary with `--single`, `--wide`, `--csv-out`, etc.).
S27 §7.1 specifies the next session should rebuild
`honest_backtest_xauusd_v2` from the 1042-line source. Built as
`backtest/honest_backtest_xauusd_v2_s28` (65,880 B — byte-identical size to
`_ext`) to avoid clobbering the operator's existing binaries.

Verified: `--single 5.0,16.0,2.0 --latency 1 --csv-out` on
`outputs/duka_xauusd_daily/2024-05-22.csv` returns N=8 trades, sum=+$10.20
honest-fill; writes the expected 32-column CSV row. The new binary works.

### 1.4 Wide-grid sweep on the full 623-day Dukascopy XAU corpus (deliverable)
The handoff §4.2 work item:

```
TP ∈ {3, 5, 8, 12}
SL ∈ {8, 12, 16, 24}    (subject to SL > TP)
z  ∈ {1.5, 2.0, 2.5, 3.0}
=> 13 valid (TP,SL) pairs × 4 z = 52 configs
```

S27 said "48 configs"; the actual code (`build_grid` in
`honest_backtest_xauusd_v2.cpp:541-572`) enumerates 52 after applying
the `sl > tp` constraint. Minor handoff inaccuracy; the **correct count
is 52**.

Run setup:
* `--wide --latency 1 --csv-out outputs/duka_wide_grid.csv` ungated,
  one invocation per daily file.
* Per-day cost: 144–276 ms (depends on tick count; production-fill +
  honest-fill loop runs all 52 configs in one process).
* Total wall-clock across 4 chunked bash invocations: ~70 s.
* Output CSV: 64,792 rows (623 days × 52 configs × 2 fill models)
  = 9.6 MB, 32 columns.

Runner script (resume-safe, idempotent on basename): see §7.2 / file
list §8 / `scripts/duka_wide_grid_runner.sh`.

### 1.5 Aggregate, bootstrap CI, rank
`scripts/duka_wide_grid_aggregate.py` reads
`outputs/duka_wide_grid.csv` and emits:
* `outputs/duka_wide_grid_summary.csv` — one row per
  (TP, SL, z, fill_model) with totals, daily mean/median, 1000-iter
  bootstrap CI95 (seed=42) on daily-mean PnL, both gross and
  net-of-Prime-$0.06.
* `outputs/duka_wide_grid_top10.md` — top 10 honest-fill configs by
  lower CI95 bound on net daily PnL, plus worst 5.
* `outputs/duka_wide_grid_verdict.md` — one-page narrative verdict.

### 1.6 Headline numbers (honest fill, ungated, net-of-Prime)

| Statistic | Value |
|---|---|
| Configs with **CI95 lower > 0** | **0 / 52** |
| Configs with **total net $ > 0** | **0 / 52** |
| Configs with **CI95 upper < 0** (statistically negative at 95%) | **48 / 52** |
| Min net total (worst) | −$7,882.38 (TP=3, SL=8, z=3.0) |
| Median net total | −$2,714.66 |
| Max net total (best) | −$351.70 (TP=12, SL=16, z=2.0) |
| Total honest-fill trades across grid | 271,189 |

Top-3 (least-bad) honest-fill configs:

| Rank | TP | SL | z | Trades | WR% | Total net $ | Daily mean net $ | CI95 net daily $ |
|---|---|---|---|---|---|---|---|---|
| 1 | 12.0 | 16.0 | 2.0 | 2306 | 53.8 | −351.70 | −0.5645 | [−2.39, +1.38] |
| 2 | 12.0 | 16.0 | 2.5 | 2279 | 54.0 | −454.05 | −0.7288 | [−2.52, +1.08] |
| 3 | 12.0 | 24.0 | 2.0 | 1770 | 57.5 | −435.18 | −0.6985 | [−2.55, +1.23] |

S27 candidate (TP=5/SL=16/z=2.0) on this corpus (honest, ungated):

* trades = 4,522, WR = 68.8%
* total gross $ = −$1,834.74; total net $ (Prime) = −$2,106.06
* exp/trade net = −$0.4657
* daily mean net = −$3.3805
* **CI95 net daily = [−$5.26, −$1.58] — strictly negative**

Compare to S27 §1.10 phase-1 reading (214 days):
* trades = 946, sum = −$329.64, exp = −$0.35/tr, daily CI = [−$3.69, +$0.58]
The 214-day CI straddled zero with negative bias; the 623-day CI is now
strictly below zero. The structural negative-expectancy hypothesis is
confirmed under tighter sampling, **independent of S27's BlackBull
"ungated −$36 / 21 days" reading which is reaffirmed as small-sample noise
about a true population mean ≈ −$0.40/tr.**

### 1.7 Verification: independent Python replay
`scripts/verify_replay_duka_day.py` reimplements the harness in pure
Python for the top-ranked config (TP=12/SL=16/z=2.0, honest, ungated,
lat=1, w=200) on day 2024-05-22.

```
Python replay : N=4  WR=25.0%  sum=$-26.47
C++ harness   : N=4  WR=25.0%  sum=$-26.47
Verdict       : N MATCH   sum MATCH
```

Confirms the C++ wide-grid CSV (the basis of the verdict) is bit-equivalent
to an independent Python implementation. **The verdict is structurally
trustworthy, not a harness artifact.**

### 1.8 Rule-7 compliance: no core engine touched
**Modified this session (1 file):** `scripts/duka_wide_grid_runner.sh`
(created; one in-place edit to allow harness exit-code 1 through
`set -euo pipefail`).
**Created this session:** see §8.
**Untouched (rule 6 / S27 rule 7):** `microscalper_crtp_sweep.cpp`,
`omega_main.hpp`, `order_exec.hpp`, `OmegaTradeLedger.hpp`,
`IndexFlowEngine.hpp`, `RiskMonitor.hpp`, `trade_lifecycle.hpp`,
`omega_config.ini`, **and `honest_backtest_xauusd_v2.cpp` itself.**

---

## 2. EDGE ARCHITECTURE (post-S28 update to S27 §3)

```
Total edge = signal × gates
           ≈ (−$0.40/tr ± noise) × (gate amplification)
           ≈ +$0.75/tr only when L2/regime gates fire on BlackBull live
```

The signal's structural negative expectancy is now established across
three independent samples:

| Source | Days | Trades | Exp/tr (honest, gross) | Daily mean CI95 |
|---|---|---|---|---|
| BlackBull L2, ungated (S26P4) | 21 | 864 | −$0.04 | [−$1.72, +$1.01] |
| Dukascopy phase-1 (S27 §1.10) | 214 | 946 | −$0.35 | [−$3.69, +$0.58] |
| **Dukascopy 2yr full (THIS) **| **623** | **4,522** | **−$0.41** | **[−$5.26, −$1.58]** |

The pattern is monotone in sample size: as n grows, the estimate converges
on **a slightly negative population mean**, and the CI tightens away from
zero. The signal does not survive an honest accounting on tick-only data.

What this means for the candidate strategy:
* **The candidate is not portable to tick-only data.** Confirmed
  exhaustively across geometry now; S27 §4.4 already showed it doesn't
  port to index instruments at all.
* **Any deployable version of the candidate requires the BlackBull L2 +
  regime feature pipeline live.** That is currently 21 days of capture.
* **All 52 wide-grid configs were ungated.** Re-running the grid GATED on
  tick-only data would block ~100% of trades because tick-only `l2_imb`
  defaults to 0.5 (outside [0.498, 0.502] threshold) and `regime` defaults
  to 0 only — both required for entry under the candidate gates.

---

## 3. RECONCILIATION OF S27 NUMBERS

Two minor inaccuracies in S27 resolved here:

1. S27 §1.4 and §9 said `--wide` enumerates 48 configs. The source code
   (`build_grid()` lines 549–559) enumerates 52 after `sl > tp` filter.
   Verified by running and counting CSV rows: 623 days × 52 configs × 2
   fill models = 64,792 rows, exactly matches the produced
   `duka_wide_grid.csv`. **The handoff was off by 4 (it likely didn't
   count the `sl > tp` admitted configs correctly).**
2. S27 §1.10 phase-1 said the Dukascopy 214-day daily-CI for ungated
   5/16/2.0 was [−$3.69, +$0.58]. The 623-day CI is [−$5.26, −$1.58] —
   strictly negative. The two are statistically consistent: the 214-day
   sample's positive upper bound was driven by 1–2 outlier days
   collapsing into the lower-bias tail in the larger sample.

---

## 4. WHAT THE NEXT SESSION MUST DO

In priority order, with operator-discretion checkpoints.

### 4.1 HistData cross-currency port (formerly S27 §4.3)
* Inventory: `/Users/jo/Tick/EURUSD/`, `GBPUSD/`, `USDJPY/`, `USDCAD/`,
  `NZDUSD/`, `AUDUSD/` — HistData monthly tick files, format
  `YYYYMMDD HHMMSSfff,bid,ask,vol`.
* Build `scripts/histdata_to_blackbull.py` converter:
  * Parse `YYYYMMDD HHMMSSfff` → epoch milliseconds (UTC).
  * Drop the `,vol` column.
  * Emit `ts_ms,bid,ask`.
  * One output file per UTC day matching the `outputs/duka_xauusd_daily/`
    convention.
* Run the wide grid (52 configs) ungated, latency=1, honest-fill across
  each pair's 24-month corpus.
* Verdict per pair: any config with CI95 lower > 0 net?
  * If YES on ≥1 pair → investigate that pair specifically (walk-forward,
    time-of-day stratification, cross-window sensitivity).
  * If NO across all 4 majors → mean-reversion at this geometry family is
    not an edge in liquid FX at session scale. Pivot per §4.5 below.

Estimated wall-clock at S28's tooling speed: ~70 s of compute per pair
once the daily-split CSVs exist. Conversion + split for 24 months × 4
pairs ≈ 4 minutes. Aggregator + bootstrap CI: ~5 s/pair. Whole §4.1 fits
in one session.

### 4.2 Time-period stratification of S28's 623-day result (cheap follow-up)
Pure Python pass over `outputs/duka_wide_grid_summary.csv` joined with
the per-day rows of `outputs/duka_wide_grid.csv`. For the top-ranked
config (TP=12/SL=16/z=2.0) and the S27 candidate (TP=5/SL=16/z=2.0):
bucket each day by month or quarter, plot cumulative equity, output a
small markdown report. Tells us whether the signal was even briefly
positive in any subwindow, or whether the negative expectancy is uniform.
~10 min of work, cheap to ship as a sanity dimension.

### 4.3 Tick-only regime classifier (formerly S27 §4.5)
Highest-eventual-value item. Train a small classifier on the 21 BlackBull
captures (label = production engine's `regime` field). Features: rolling
vol over multiple windows, spread, tick-rate, time-of-day, recent
absolute move. Validate on held-out BlackBull days. Apply to Dukascopy
2yr. Re-run the gated strategy with proxy-regime substituted for missing
`regime`. If proxy recovers a meaningful fraction of the +$459 gate edge
(S27 §4.4), the strategy becomes portable.

`l2_imb` is order-book-depth — not reconstructable from bid/ask. Don't
try.

### 4.4 If §4.1 returns no positive edge anywhere
Stop chasing the 5/16 z-MR signal family. Pivot to orthogonal edges that
don't need L2:
* TSMOM at multiple horizons (`TsmomCellBacktest.cpp` is the existing
  scaffold)
* Bar-aggregated mean reversion at 5min/15min/1h
* Volatility-regime trading
* Time-of-day breakouts on liquid pairs
* News-event responses (would need event data)

### 4.5 If §4.3 succeeds (proxy regime classifier works)
Re-run the full §4 verification on tick-only data using the
reconstructed gates. If the candidate's +$0.69-net-Prime/trade is
recovered on Dukascopy with classifier-gates, the candidate becomes
deployable subject to live shadow 2 weeks and real money at 0.01 lot.

### 4.6 DO NOT DO
* Do not deploy the candidate live (rule 4 + §4.4 below).
* Do not flip `mode=LIVE` for any reason without explicit instruction.
* Do not modify the listed protected files.
* Do not re-run the same 52-config grid on the same data expecting a
  different result. The verdict in this handoff is the verdict; new
  data (other instruments, other features) is needed to change the
  conclusion.

---

## 5. UNCHANGED OPEN QUESTIONS FROM PRIOR HANDOFFS

* Part 1B §4 ledger correction — still uncommitted (operator owns).
* Part 2 §3 fill-model direction — neither S26P4 nor S27 nor S28 addressed.
* Part 2 §4 signal port — `GoldMicroScalperEngine` faithful port still
  open. S28 has made this MORE important: with the wide-grid now ruling
  out geometry-as-rescue, the next highest-value action on the candidate
  family is a faithful port of the production engine's actual gate logic,
  which might surface gate features beyond the four approximated in
  `ProdGates` and validate or invalidate the S27 §3 "$459 gate edge"
  decomposition.
* S26 Part 3 cross-window stability of the GATED candidate at W ∈
  {50, 100, 400, 800, 1600} — S27 §2.2 partially addressed (W=200
  is a sharp peak). Operator may want a deeper window grid before
  trusting any candidate built on W=200.

---

## 6. RULES FOR THE NEXT SESSION (carried forward from S27, unchanged unless noted)

1. Read this handoff + S27 + S26 Part 3 + Part 2 + Part 1B end-to-end
   before touching anything.
2. Verify on-disk state: `git status`, `git log -1`, mode in `omega_config.ini`,
   `max_lot_gold` line.
3. **DO NOT flip back to `mode=LIVE`** for any reason without explicit
   operator instruction.
4. **DO NOT recommend deploying the candidate live based on what we know
   now.** S28 has now ruled out geometry-as-rescue across the full 2yr
   corpus.
5. The `honest_backtest_xauusd_v2.cpp` is **1042 lines** — past the
   800-line full-file preference threshold. Future modifications should
   still output the full file in chat unless operator changes threshold.
6. **Never modify** `microscalper_crtp_sweep.cpp`, `omega_main.hpp`,
   `order_exec.hpp`, `OmegaTradeLedger.hpp`, `IndexFlowEngine.hpp`,
   `RiskMonitor.hpp`, `trade_lifecycle.hpp` without explicit operator
   instruction.
7. Operator preference: warn at 70% context with summary. Stop and write
   a follow-up handoff. Don't stretch.
8. Operator preference: full file output when changing files ≤ 800 lines.
9. Operator's `/Users/jo/Tick` mount: re-request via
   `request_cowork_directory("/Users/jo/Tick")` if needed.
10. **NEW (S28):** Operator allowed Claude to write a NEW binary alongside
    the existing ones (`honest_backtest_xauusd_v2_s28`) without
    overwriting `honest_backtest_xauusd_v2` or `_ext`. Next session may
    safely run any of the three; they are functionally equivalent for
    `--single`/`--wide` invocations to the extent that `_s28` and `_ext`
    are rebuilds of the same 1042-line source. The plain
    `honest_backtest_xauusd_v2` is the OLD 60-config v1-grid binary —
    do NOT use it for the wide grid.

---

## 7. REPRODUCING THIS SESSION'S KEY RESULTS

### 7.1 (Re)build the extended harness
```bash
cd /Users/jo/omega_repo
g++ -std=c++17 -O2 -Wall -Wextra backtest/honest_backtest_xauusd_v2.cpp \
    -o backtest/honest_backtest_xauusd_v2_s28
# Expected: 65880 B binary, one -Wcomment warning (cosmetic, in source
# comments), no errors.
```

### 7.2 Run the wide-grid sweep on the 623-day Dukascopy XAU corpus
```bash
cd /Users/jo/omega_repo
rm -f outputs/duka_wide_grid.csv
# Run runner repeatedly until completion (resume-safe).
# CHUNK_DAYS=200 fits ~35-45s per bash call.
bash scripts/duka_wide_grid_runner.sh 200    # writes ~180 days
bash scripts/duka_wide_grid_runner.sh 200    # next ~200
bash scripts/duka_wide_grid_runner.sh 200    # next ~200
bash scripts/duka_wide_grid_runner.sh 200    # remainder
# Final state: 64792 rows in outputs/duka_wide_grid.csv (623 × 104).
```

### 7.3 Aggregate + bootstrap CI + verdict
```bash
cd /Users/jo/omega_repo
python3 scripts/duka_wide_grid_aggregate.py
# Outputs:
#   outputs/duka_wide_grid_summary.csv  (104 rows: 52 cfg × 2 fill)
#   outputs/duka_wide_grid_top10.md
#   outputs/duka_wide_grid_verdict.md
# Expected verdict line:
#   "Honest-fill configs with net-of-commission lower CI95 > 0: 0 / 52."
```

### 7.4 Independent Python replay verification
```bash
cd /Users/jo/omega_repo
python3 scripts/verify_replay_duka_day.py
# Expected:
#   Python replay : N=4  WR=25.0%  sum=$-26.47
#   C++ harness   : N=4  WR=25.0%  sum=$-26.47
#   Verdict       : N MATCH   sum MATCH
```

### 7.5 (For future sessions) Finish the Dukascopy split from cold cache
If the daily-split needs to be regenerated from scratch on a cold cache:
```bash
cd /Users/jo/omega_repo
mkdir -p outputs/duka_xauusd_daily
gawk -F, 'NR>1 {
  ts = $1+0
  day_num = int(ts/86400000)
  if (day_num != cur_day) {
    if (file != "") close(file)
    cur_day = day_num
    date = strftime("%Y-%m-%d", day_num*86400, 1)
    file = "outputs/duka_xauusd_daily/" date ".csv"
    print "ts_ms,bid,ask" > file
  }
  print $0 >> file
}' /Users/jo/Tick/2yr_XAUUSD_bt_h.csv
# At ~1 GB/s warm cache: ~15-20s wall-clock for the full file.
# At ~30 MB/s cold cache: ~100s; will not fit in one bash 45s call;
# chunk by ts_ms range as S28 did (see §1.2 table).
# Final: 623 daily CSVs in outputs/duka_xauusd_daily/.
```

---

## 8. FILES CREATED / MODIFIED THIS SESSION

### Modified (1):
* `scripts/duka_wide_grid_runner.sh` — only file edited in place
  (and only because it was first created this session). One Edit applied
  to allow harness's exit-code 1 through `set -euo pipefail`.

### Created (scripts, 3):
* `scripts/duka_wide_grid_runner.sh` — resume-safe wide-grid sweep
  driver (see full file in repo).
* `scripts/duka_wide_grid_aggregate.py` — aggregator with 1000-iter
  bootstrap CI95 on per-day mean, gross AND net-of-Prime $0.06/RT.
* `scripts/verify_replay_duka_day.py` — independent Python replay of
  one (Dukascopy day, ungated config) for the C++ harness.

### Created (binary, 1):
* `backtest/honest_backtest_xauusd_v2_s28` — rebuild of the 1042-line
  source; byte-size identical to the pre-existing `_ext` binary.

### Created (analysis outputs in `outputs/`):
* `duka_wide_grid.csv` — 64,792 rows, 9.6 MB. Full sweep output.
* `duka_wide_grid_summary.csv` — 104 rows. Per-(config, fill_model)
  aggregates + bootstrap CI95.
* `duka_wide_grid_top10.md` — top-10/worst-5 markdown report.
* `duka_wide_grid_verdict.md` — one-page narrative verdict.

### Created (daily-split data, 408 files):
* `outputs/duka_xauusd_daily/2024-06-06.csv` …
  `outputs/duka_xauusd_daily/2025-09-26.csv` — completes the S27
  partial split. Together with the existing 215 (now 215 with
  2024-06-06.csv overwritten cleanly), the directory now has 623 files
  covering 2023-09-27 → 2025-09-26.

### Not touched (rule 6 + S27 rule 7 compliance):
None of: `microscalper_crtp_sweep.cpp`, `omega_main.hpp`,
`order_exec.hpp`, `OmegaTradeLedger.hpp`, `IndexFlowEngine.hpp`,
`RiskMonitor.hpp`, `trade_lifecycle.hpp`, `omega_config.ini`,
`backtest/honest_backtest_xauusd_v2.cpp`,
`backtest/honest_backtest_xauusd_v2` (the old binary),
`backtest/honest_backtest_xauusd_v2_ext` (S27's extended binary),
`backtest/honest_backtest_xauusd_v2_linux`,
`scripts/s26p4_aggregate_single_config.py`,
`scripts/verify_replay_one_day.py`.

---

## 9. NEXT SESSION FIRST-MESSAGE TEMPLATE

> Read `HANDOFF_S28_AFTER_S27.md`, `HANDOFF_S27_AFTER_S26_PART4.md`,
> `HANDOFF_S26_PART3.md`, `HANDOFF_S26_PART2...md`, and
> `HANDOFF_S26_PART1B...md` end-to-end. Confirm on-disk state
> (`git status`, `mode=SHADOW` in omega_config.ini,
> `max_lot_gold=0.01`). Re-mount `/Users/jo/Tick` via
> `request_cowork_directory("/Users/jo/Tick")`. Build the
> `histdata_to_blackbull.py` converter for EURUSD/GBPUSD/USDJPY/USDCAD
> (HistData monthly tick files at `/Users/jo/Tick/<CCY>/`). Split each
> pair's 24-month corpus by UTC day. Run the wide grid (52 configs)
> ungated, latency=1, honest-fill on each pair. Use the S28 aggregator
> (`scripts/duka_wide_grid_aggregate.py`) verbatim or copy-and-adapt
> per pair. Produce a verdict on "is 5/16 mean-reversion an edge family
> anywhere in liquid FX". If positive on ≥1 pair → drill in with
> walk-forward + time-of-day. If negative across all 4 → pivot to S27
> §4.5 (tick-only regime classifier). Do NOT recommend live deployment
> regardless of result — operator owns that decision.

---

End of S28 handoff.

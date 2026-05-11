# HANDOFF S27 — After S26 Part 4 and Dukascopy phase 1

**Session date:** 2026-05-11
**Branch:** `main` at `9bc02f9` (unchanged HEAD; all S26 Part 1B/2/3 plus this session's work is uncommitted)
**Mode:** `omega_config.ini` line 75 still `mode=SHADOW` (rule 3 honored, no change)
**max_lot_gold:** still `0.01` (rule 4.6 honored)
**This session's commits:** zero. Operator commits per their own cadence.

---

## 0. ONE-PARAGRAPH STATE OF THE WORLD

The S26 Part 3 candidate (TP=5 / SL=16 / z=2.0 GATED on BlackBull XAUUSD L2 data)
**passes the handoff §4.2 hard latency gate** (11/18 firing days profitable at
latency=5, both gross and net of BlackBull Prime commission $0.06/round-trip)
but **fails three soft tests**:

1. §4.3 bootstrap 95% CI on per-day expectancy is strictly above zero only
   at latency=1. Latency ≥ 3 straddles zero.
2. §2.2 window sensitivity shows W=200 is a sharp peak; W ∈ {50, 100, 400,
   800, 1600} all have CIs that straddle zero. Window appears overfit.
3. §4.4 cross-instrument is a clean REJECT: US500 (−$279), USTEC (−$3,907),
   NAS100 (−$313) over 12–13 day samples; XAUUSD-ungated on the same 21 days
   is −$36. **The edge is in the GATES, not the SIGNAL.**

A 214-day phase-1 run on Dukascopy 2yr XAUUSD tick data
(`/Users/jo/Tick/2yr_XAUUSD_bt_h.csv`) confirms this:
ungated raw signal at the same TP=5/SL=16/z=2.0 geometry produces 946 trades,
WR=66.4%, expectancy −$0.35/trade, total −$329.64. The 95% CI on daily-mean
is [−$3.69, +$0.58] — straddles zero with a slight negative bias. The 21-day
BlackBull ungated WR was 74.1% on n=864 trades; under a 10× larger sample,
the population WR is **66.4%**, well below the 76.2% breakeven for 5pt/16pt
geometry. The 8 percentage-point WR delta between BlackBull and Dukascopy
on the SAME ungated signal is mostly explained by spread distribution
(BlackBull median 0.22pt vs Dukascopy median 0.48pt).

**Net result:** the candidate strategy is XAUUSD-specific, gate-dependent,
window-fragile, latency-fragile, and not portable to tick-only data. It
remains a thin +$0.69-net-Prime expectancy at latency=1 *if* you also have
the BlackBull L2 `l2_imb` and `regime` features computed live. The candidate
**should not** be deployed at the §4.6 lot size based on what we know now.
The signal alone is structurally near-zero / slight-negative across 9+
months of clean tick data. **Capturing more BlackBull L2 days is the only
data action that moves the candidate toward deployable.**

---

## 1. WHAT THIS SESSION DID (S27 chronological)

### 1.1 S26 Part 3 reproduction
Bit-for-bit reproduction of the 21-day --gated --wide sweep using the
v2-baseline binary recompiled on Linux. Per-day rows and aggregate
($+423.03, 13/21 profitable, 18 firing) match HANDOFF_S26_PART3.md §3
exactly.

### 1.2 BlackBull commission determination
Web-confirmed against `blackbull.com/en/our-company/account-comparison/`
and contract specs: **ECN Prime $3/side per standard lot** = $0.06
round-trip per 0.01-lot trade. ECN Institutional $4/RT/std-lot = $0.04
per 0.01-lot. Standard account commission-free but with wider spreads
(roughly +0.5pt) that make the strategy unprofitable at micro-lot scale.

Net-of-Prime expectancy at the candidate setting: $0.69/trade
(vs $0.75 gross). Calendar-day profitable days: 13/21 → 12/21 net
(only 2026-05-06 flips from +$0.04 to −$0.08).

### 1.3 Modified `backtest/honest_backtest_xauusd_v2.cpp` in place
Per handoff §4.1 explicit instruction. Added 12 CLI flags:

```
--single TP,SL,z       run ONE config (no grid)
--latency N            override LATENCY_TICKS
--commission C         per round-trip USD (default 0.0)
--window W             override z-score window (default 200)
--imb-long F           override L2 long imb threshold (default .502)
--imb-short F          override L2 short imb threshold (default .498)
--max-spread S         override MAX_SPREAD_PT (default 1.0)
--session H1-H2        only enter when UTC hour in [H1, H2)
--early-exit-z X       exit early on z divergence by X (X<=0 disables)
--early-exit-ticks K   max look-ahead for early-exit (default 200)
--tick-mult M          USD per pt per std-lot (default 100 for XAUUSD)
--size-lots L          contract size in std-lots (default 0.01)
--cooldown C           cooldown ticks after exit (default 100)
--csv-out PATH         append per-(config,fill_model) summary CSV
--trade-log PATH       append per-trade CSV
--label LBL            free-form label in CSV
```

File grew from 654 → 1042 lines. Default invocation (no new flags,
`--gated --wide` as before) is **bit-identical** to v2-baseline, verified
by re-running the full 21-day sweep and diffing every output file:
zero diffs, total still $+423.03 exactly.

Only file modified this session. No core-engine files touched (rule 7
respected: `microscalper_crtp_sweep.cpp`, `omega_main.hpp`,
`order_exec.hpp`, `OmegaTradeLedger.hpp`, `IndexFlowEngine.hpp`,
`RiskMonitor.hpp`, `trade_lifecycle.hpp` all untouched).

### 1.4 §4.1 single-config latency sweep (deliverable)
21 days × latency ∈ {1, 3, 5, 10} × single config TP=5/SL=16/z=2.0 gated
= 84 runs. Outputs:

- `outputs/single_config_5_16_2.0_gated.csv` (full per-(day, latency,
  fill_model) summary, 168 data rows)
- `outputs/single_config_5_16_2.0_gated_compact.csv` (handoff §4.1's
  prescribed compact format)
- `outputs/single_config_5_16_2.0_gated_trades.csv` (per-trade log)
- `outputs/single_config_5_16_2.0_gated.md` (markdown report with
  4 aggregate rows, day-by-day per latency, regression-vs-Part3 PASS,
  caveats)
- `outputs/s26p4_equity_curve_lat{1,3,5,10}_{gross,netPrime}.png` (8 PNGs)
- `outputs/s26p4_pnl_histogram_lat{1,3,5,10}.png` (4 PNGs)
- `outputs/s26p4_holdtime_histogram_lat{1,3,5,10}.png` (4 PNGs)

Aggregator: `scripts/s26p4_aggregate_single_config.py` (full source in
repo). Regression check vs Part 3 §3: latency=1 sum=+$423.03 PASS.

### 1.5 §4.2 latency-robustness hard gate
Acceptance criterion: ≥10/18 firing-days profitable at latency=5.
**Result: 11/18 (gross), 11/18 (net Prime). PASS.**

### 1.6 §4.3 bootstrap 95% CI (1000 iters, seed=42)

| latency | gross daily mean | net Prime daily mean |
|---|---|---|
| 1  | +$20.14 [+$2.79, +$37.88] | +$18.53 [+$1.21, +$35.96] |
| 3  | +$10.17 [−$7.96, +$29.99] | +$8.57 [−$9.45, +$28.44] |
| 5  | +$14.32 [−$2.45, +$31.86] | +$12.73 [−$4.01, +$30.29] |
| 10 | +$8.53 [−$7.14, +$24.46]  | +$6.97 [−$8.59, +$23.20]  |

Only latency=1 is statistically significant at 95%.

### 1.7 §4.4 cross-instrument port (REJECT)

| Instrument | Days | Firing | Profitable | Total $ | Daily 95% CI |
|---|---|---|---|---|---|
| XAUUSD ungated (sanity) | 21 | 21 | 8/21 | −$36.03 | [−$24.14, +$21.27] |
| US500 | 12 | 12 | 2/12 | −$278.96 | [−$47.66, +$3.76] |
| USTEC | 13 | 13 | 2/13 | −$3,907.09 | [−$457, −$148] |
| NAS100 | 2 | 2 | 0/2 | −$312.55 | [−$275, −$37] |

Reasons:
* US500/USTEC/NAS100 captures have all-zero `l2_imb` and `regime=0` only,
  so the L2 gates cannot fire on indices. With l2_imb defaulting to 0.5,
  even invoking `--gated` blocks 100% of trades; results above use ungated.
* TP/SL scaled per-instrument to keep TP/σ and SL/σ ratios constant
  (XAU median 200-tick SD ≈ 0.37pt → US500 TP=8.28, SL=26.49;
   USTEC TP=22.74, SL=72.78; NAS100 TP=24.30, SL=77.76).
* The signal-only result on the SAME 21 XAUUSD days is −$36 vs +$423
  gated, so the gates account for $459 of the edge. The signal alone is
  the part that ports; it doesn't have edge.

### 1.8 Operator-requested §2.x sweeps

| Sweep | Setting → result | Verdict |
|---|---|---|
| §2.6 imb threshold | 0.502/0.498→+$423, 0.510/0.490→+$307, 0.520→+$43, 0.550→+$92, 0.600→+$53 with only 18 trades | Baseline 0.502 is the sweet spot |
| §2.2 window | W=50→+$263 [CI−6,+30]; W=100→+$152; **W=200→+$423 [CI+3,+38]**; W=400→−$103; W=800→+$54; W=1600→+$177 | Sharp peak at W=200, likely overfit |
| §2.7 session (UTC) | 00-04: −$36; 04-08: +$3; **08-12: +$57**; 12-16: +$32; **16-20: +$49**; 20-24: −$38 | Clean liquidity pattern: 08-20 UTC positive, 20-04 UTC negative |
| §2.5 early-exit on z-div | All variants reduce total profit and break CI significance | Keep OFF |

### 1.9 Independent Python replay verification
`scripts/verify_replay_one_day.py` reimplements the harness in pure Python
for day 2026-04-22, TP=5/SL=16/z=2.0 gated, latency=1. Result:
N=26, WR=84.6%, sum=+$65.59 — exact match to the C++ harness.

### 1.10 Dukascopy 2yr XAUUSD phase-1 signal-neutrality test
Operator pointed to `/Users/jo/Tick/` (49GB of historical tick data).
Inventory:
* `2yr_XAUUSD_bt_h.csv` (3.3GB, 111.6M ticks Sept 2023 → Sept 2025,
  already in BlackBull schema `ts_ms,bid,ask`)
* `dow30_2yr.csv` (1.7GB, Dow Jones, date,time,bid,ask format)
* `h1_data.csv` / `h2_data.csv` (~1.5GB each, year-1 and year-2 splits)
* Per-currency folders: EURUSD, GBPUSD, USDJPY, USDCAD, NZDUSD, AUDUSD
  (HistData monthly tick files, format `YYYYMMDD HHMMSSfff,bid,ask,vol`)
* `Nas/` (Nasdaq tick data)
* `duka_ticks/` (monthly XAUUSD Dukascopy raw, 30 files)

Sandbox RAM constraint (3.8GB) prevented loading whole 2yr file (~5.3GB
parsed); split by UTC day via gawk into
`outputs/duka_xauusd_daily/`. The bash-tool 45-second timeout cap
prevented finishing the full split in one invocation. Phase 1 collected
214 clean days (Sept 2023 → June 5, 2024); the remaining ~290 days are
not yet split.

Ran TP=5/SL=16/z=2.0 ungated AND spread-cap-only on the 214 days:

| Run | Trades | WR | Total $ | Net Prime $ | Exp/tr | CI95 daily mean |
|---|---|---|---|---|---|---|
| ungated | 946 | 66.4% | −$329.64 | −$386.40 | −$0.35 | [−$3.69, +$0.58] |
| spread-cap only | 944 | 66.6% | −$282.38 | −$339.02 | −$0.30 | [−$3.53, +$0.80] |

CI tightened ~10× vs 21-day analysis. Signal is structurally
neutral-to-slight-negative on Dukascopy. **The 21-day BlackBull "ungated
−$36" reading was a small-sample noise around the same zero-line.**

---

## 2. SCALING / TESTING / VERIFICATION FRAMEWORK (operator asked)

**Scaling:** The v2-ext harness now parameterises every previously-hardcoded
constant. Scaling tests = bash loops over CSV inputs, no recompiles. CSV
output is the single source-of-truth; markdown/PNG reports are derivable
via aggregator scripts. To go from "1 day × 1 setting" to "n days × m
settings" is two nested `for` loops.

**Exhaustive testing within the data we have:**
* One-axis-at-a-time sweeps preferred over full grids (each axis sweep
  is ~10 runs vs 10^k for full grid; same statistical power per axis).
* Walk-forward split where sample allows. Not useful here because §4.4
  pre-empted the question: cross-instrument REJECT means the candidate
  is XAUUSD-specific, walk-forward on a 21-day single-instrument sample
  would just split the noise.
* Cross-instrument testing acts as a second OOS dimension.
* Bootstrap 1000-iter CI on daily PnL gives 95% bounds.

**Verification:** three layers, all done this session:
1. Bit-for-bit reproduction of Part 3 §3 result with the extended
   harness using v2-baseline flags.
2. Independent Python implementation matches C++ on one (day, config).
3. Every sweep preserves the latency=1 baseline regression value
   ($+423.03) when run with default parameters — implicit check that
   sweep complexity didn't break the baseline.

---

## 3. EDGE ARCHITECTURE (definitive after this session)

```
Total edge = signal × gates
           ≈ (~$0/trade) × (gate amplification)
           ≈ +$0.75/trade only when gates fire
```

The signal (200-tick z-MR at 2σ, TP=5, SL=16) is structurally near-zero
expectancy across ALL data we have:
* 21 days BlackBull ungated: −$36 / 864 trades = −$0.04/tr
* 214 days Dukascopy ungated: −$330 / 946 trades = −$0.35/tr

The gates (`watchdog_dead==0`, spread≤1.0, `regime ∈ {0,2}`, l2_imb in
[0.498, 0.502] favouring direction) transform this to +$0.75/tr on
BlackBull's 21-day capture. The gates require:
* L2 order book imbalance (NOT in tick data anywhere we have)
* Production engine's regime classifier output (NOT reconstructable
  from bid/ask alone with the data we have)
* Spread (recoverable from any bid/ask feed)
* Watchdog state (broker-specific, defaults to "healthy" in backtest)

So:
* Strategy is portable ONLY where L2 is captured.
* L2 is captured ONLY on BlackBull live (21 days so far through 2026-05-08).
* The CI at lat=1 [+$2.79, +$37.88] daily-mean rests entirely on 21 days.
* Capturing more BlackBull L2 days is the binding constraint on tightening
  this CI.

---

## 4. WHAT THE NEXT SESSION MUST DO

### 4.1 Finish the Dukascopy 2yr split (~290 remaining days)
The split must run in 3-4 bash invocations of <45s each because the
virtiofs Mac mount is the I/O bottleneck (~30 MB/s effective write).
Resume from June 6, 2024 onward. Use `gawk` with optimized strftime
(once per day, not per row):

```bash
gawk -F, 'NR>1 {
  day_num = int($1/86400000)
  if (day_num != cur_day) {
    if (file != "") close(file)
    cur_day = day_num
    date = strftime("%Y-%m-%d", day_num*86400, 1)
    file = "outputs/duka_xauusd_daily/" date ".csv"
    print "ts_ms,bid,ask" > file
  }
  print >> file
}' /sessions/admiring-clever-franklin/mnt/Tick/2yr_XAUUSD_bt_h.csv
```

To resume from a specific date, prefix with a `BEGIN { skip_until = ... }`
guard that only starts writing once a target ts_ms is reached.

### 4.2 Wide-grid sweep on Dukascopy 214-day XAU (this session attempted)
Run the `--wide` grid (48 configs) ungated on the 214 daily files.
If ANY configuration in {TP=3,5,8,12} × {SL=8,12,16,24} × {z=1.5,2,2.5,3}
produces positive expectancy under honest fills, that's a tick-only
candidate worth investigating. If NONE do, the entire mean-reversion
family is signal-empty at session scale on XAU tick data.

### 4.3 Cross-currency signal neutrality
EURUSD, GBPUSD, USDJPY, USDCAD HistData files at
`/Users/jo/Tick/<CCY>/HISTDATA_COM_ASCII_<CCY>_T<YYYYMM>/` need format
conversion to BlackBull schema. HistData format is
`YYYYMMDD HHMMSSfff,bid,ask,vol`. Build a one-shot converter
(`scripts/histdata_to_blackbull.py`), then split by day, then run
TP=5/SL=16/z=2.0 ungated across each pair's 24 months.

If raw signal is neutral on every major pair across 2 years, mean
reversion at this geometry simply isn't an edge in liquid FX. If even
ONE pair shows positive expectancy at scale, that's a finding.

### 4.4 Time-period stratification on the 214-day XAU result
The per-day CSV `outputs/duka_2yr_signal_neutrality.csv` already has
every day's PnL. One Python pass can bucket by month or quarter, output
expectancy per period, plot cumulative equity. Tells us whether the
signal worked early/late/throughout the 2yr window.

### 4.5 Tick-only regime classifier (the highest-eventual-value item)
Train a small classifier on the 21 BlackBull captures (label = the
production engine's `regime` field). Features: rolling vol over multiple
windows, spread, tick-rate, time-of-day, recent absolute move. Validate
on held-out BlackBull days. Apply to Dukascopy 2yr. Re-run the gated
strategy with proxy-regime substituted for missing `regime`. If proxy
recovers a meaningful fraction of the +$459 gate edge, the strategy
becomes portable.

`l2_imb` cannot be reconstructed from tick-only — it's an order book
depth feature. Don't try.

### 4.6 If 4.2 / 4.3 produce no positive edge anywhere
Stop chasing this signal family. Pivot to orthogonal edges that don't
need L2:
* TSMOM at multiple horizons (`TsmomCellBacktest.cpp` is the existing
  scaffold)
* Bar-aggregated mean reversion at 5min/15min/1h
* Volatility-regime trading
* Time-of-day breakouts on liquid pairs
* News-event responses (would need event data)

### 4.7 If 4.5 succeeds (proxy regime classifier works)
Re-run the full §4 verification on tick-only data using the
reconstructed gates. If the candidate's +$0.69-net-Prime/trade is
recovered on Dukascopy with classifier-gates, the candidate becomes
deployable subject to §4.5 (live shadow 2 weeks) and §4.6 (real money
at 0.01 lot).

---

## 5. UNCHANGED OPEN QUESTIONS FROM PRIOR HANDOFFS
* Part 1B §4 ledger correction — still uncommitted (operator owns).
* Part 2 §3 fill-model direction — Part 4 didn't address.
* Part 2 §4 signal port — `GoldMicroScalperEngine` faithful port still
  open. This is more important than ever now that §4.4 has shown the
  gates are the entire alpha; a faithful port of the engine's actual
  gate logic might surface additional gate features beyond the four
  approximated in `ProdGates`.

---

## 6. RULES FOR THE NEXT SESSION (unchanged from S26 Part 3)
1. Read this handoff + S26 Part 3 + Part 2 + Part 1B end-to-end before touching anything.
2. Verify on-disk state: git status, git log -1, mode in omega_config.ini.
3. **DO NOT flip back to `mode=LIVE`** for any reason without explicit operator instruction.
4. **DO NOT recommend deploying the candidate live based on what we know now.** §4.4 already
   shows the candidate is XAUUSD-specific and gate-dependent.
5. The `honest_backtest_xauusd_v2.cpp` is now **1042 lines** — past the 800-line full-file
   preference threshold mentioned in S26 Part 3. Future modifications should still output
   the full file in chat (operator preference) unless the operator changes the threshold.
6. **Never modify** `microscalper_crtp_sweep.cpp`, `omega_main.hpp`, `order_exec.hpp`,
   `OmegaTradeLedger.hpp`, `IndexFlowEngine.hpp`, `RiskMonitor.hpp`, `trade_lifecycle.hpp`
   without explicit operator instruction. Same exception: Part 1B ledger correction is
   done and uncommitted.
7. Operator preference: warn at 70% context with summary. Stop and write a follow-up
   handoff. Don't stretch.
8. Operator preference: full file output when changing files ≤800 lines.
9. Operator's `/Users/jo/Tick` mount has 49GB of historical data; new session must
   re-request access via `request_cowork_directory("/Users/jo/Tick")` after mount.

---

## 7. REPRODUCING THIS SESSION'S KEY RESULTS

### 7.1 Build the extended harness
```bash
cd /Users/jo/omega_repo
g++ -std=c++17 -O2 -Wall -Wextra backtest/honest_backtest_xauusd_v2.cpp \
    -o backtest/honest_backtest_xauusd_v2
```

### 7.2 Reproduce §4.1 single-config latency sweep
```bash
CSV=outputs/single_config_5_16_2.0_gated.csv
TRADELOG=outputs/single_config_5_16_2.0_gated_trades.csv
rm -f "$CSV" "$TRADELOG"
DAYS="<the 21 viable days from S26 Part 3 §7>"
for LAT in 1 3 5 10; do
  for f in $DAYS; do
    backtest/honest_backtest_xauusd_v2 --single 5.0,16.0,2.0 --gated \
        --latency $LAT --csv-out "$CSV" --trade-log "$TRADELOG" \
        --label "s26p4_lat${LAT}" "$f"
  done
done
python3 scripts/s26p4_aggregate_single_config.py
```

Expected at latency=1: 18 firing, 13 profitable, sum=+$423.03.

### 7.3 Reproduce §4.4 cross-instrument
```bash
CSV=outputs/cross_instrument_5_16_2.0.csv
rm -f "$CSV"
# XAUUSD ungated (sanity)
for f in $XAU_DAYS; do
  backtest/honest_backtest_xauusd_v2 --single 5.0,16.0,2.0 --latency 1 \
      --csv-out "$CSV" --label "xinst_XAUUSD_lat1" "$f"
done
# US500 with scaled TP=8.28/SL=26.49
# USTEC with scaled TP=22.74/SL=72.78
# NAS100 with scaled TP=24.30/SL=77.76
# (run only files >=3MB; smaller are partial captures)
```

### 7.4 Reproduce Dukascopy phase-1
```bash
# Split (one bash invocation per ~9 months due to 45s I/O cap)
mkdir -p outputs/duka_xauusd_daily
gawk -F, 'NR>1 { ... see §4.1 above ... }' \
    /sessions/admiring-clever-franklin/mnt/Tick/2yr_XAUUSD_bt_h.csv
# Drop truncated last file before running

# Run
CSV=outputs/duka_2yr_signal_neutrality.csv
rm -f "$CSV"
for f in outputs/duka_xauusd_daily/*.csv; do
  backtest/honest_backtest_xauusd_v2 --single 5.0,16.0,2.0 --latency 1 \
      --csv-out "$CSV" --label "duka_ungated" "$f"
  backtest/honest_backtest_xauusd_v2 --single 5.0,16.0,2.0 --latency 1 \
      --gated --imb-long 0.0 --imb-short 1.0 \
      --csv-out "$CSV" --label "duka_spread_only" "$f"
done
```

Expected on the 214-day sample:
* ungated: 946 trades, WR=66.4%, total=−$329.64, CI=[−$3.69, +$0.58]
* spread-only: 944 trades, WR=66.6%, total=−$282.38, CI=[−$3.53, +$0.80]

### 7.5 Reproduce verification
```bash
python3 scripts/verify_replay_one_day.py
# Expected: N=26  WR=84.6%  sum=$+65.59  Verdict: N MATCH   sum MATCH
```

---

## 8. FILES CREATED / MODIFIED THIS SESSION

### Modified (1):
* `backtest/honest_backtest_xauusd_v2.cpp` — 654 → 1042 lines, +12 CLI
  flags, bit-identical default behavior. See full file in repo.

### Created (Python aggregators / verification):
* `scripts/s26p4_aggregate_single_config.py`
* `scripts/verify_replay_one_day.py`

### Created (analysis outputs — all in `outputs/`):
* `single_config_5_16_2.0_gated.csv` (the §4.1 full CSV)
* `single_config_5_16_2.0_gated_compact.csv` (the §4.1 prescribed compact format)
* `single_config_5_16_2.0_gated_trades.csv` (per-trade log)
* `single_config_5_16_2.0_gated.md` (markdown report)
* `s26p4_equity_curve_lat{1,3,5,10}_{gross,netPrime}.png` (8)
* `s26p4_pnl_histogram_lat{1,3,5,10}.png` (4)
* `s26p4_holdtime_histogram_lat{1,3,5,10}.png` (4)
* `cross_instrument_5_16_2.0.csv`
* `sweep_imb_threshold.csv`
* `sweep_window.csv`
* `sweep_session.csv`
* `sweep_early_exit.csv`
* `duka_xauusd_daily/*.csv` (214 daily-split Dukascopy files;
  remaining ~290 days not yet split)
* `duka_2yr_signal_neutrality.csv` (Dukascopy phase-1 results)
* `gated_wide_ext/honest_bt_<date>.txt` (21 per-day reproduction outputs;
  bit-identical to baseline `gated_wide/`)
* `gated_wide_ext/` is the reproduction-via-extended-binary test dir;
  `gated_wide/` is the original reproduction.

### Not touched (rule 7 compliance):
None of: `microscalper_crtp_sweep.cpp`, `omega_main.hpp`, `order_exec.hpp`,
`OmegaTradeLedger.hpp`, `IndexFlowEngine.hpp`, `RiskMonitor.hpp`,
`trade_lifecycle.hpp`, `omega_config.ini`.

---

## 9. NEXT SESSION FIRST-MESSAGE TEMPLATE

> Read `HANDOFF_S27_AFTER_S26_PART4.md`, `HANDOFF_S26_PART3.md`,
> `HANDOFF_S26_PART2...md`, and `HANDOFF_S26_PART1B...md` end-to-end.
> Confirm on-disk state (`git status`, `mode=SHADOW` in omega_config.ini,
> `max_lot_gold=0.01`). Re-mount `/Users/jo/Tick` via
> `request_cowork_directory("/Users/jo/Tick")`. Finish the Dukascopy 2yr
> split for the back ~290 days (handoff §4.1). Run the wide-grid sweep
> on the full 504-day Dukascopy XAU set (§4.2). Convert HistData
> EURUSD/GBPUSD/USDJPY/USDCAD to BlackBull schema and run cross-currency
> signal neutrality (§4.3). Produce a verdict on "is mean-reversion at
> 5/16 geometry a viable edge family on tick-only data anywhere in our
> 2yr corpus". Do NOT recommend live deployment regardless of result —
> operator owns that decision.

---

End of S27 handoff.

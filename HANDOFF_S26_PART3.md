# HANDOFF — S26 PART 3 — SINGLE-CONFIG VERIFICATION OF THE LEADING CANDIDATE

Date: **2026-05-12** (NZ)
Branch: `main` at `9bc02f9` — Part 1B + Part 2 + Part 3 work all uncommitted.
Operator: **Jo**.
Prior handoffs (READ FIRST): `HANDOFF_S26_PART1B...` (ledger correction), `HANDOFF_S26_PART2...` (honest backtest harness + 3-day candidate refutation).

---

## TL;DR

Part 3 ran the honest backtest across **21 substantive XAUUSD trading days** (2026-04-09 through 2026-05-07), exposed the Part 2 "TP=5/SL=8 candidate" as a 3-day cherry-pick, and identified a new structurally different leading candidate:

> **TP=5.0pt / SL=16.0pt / z=2.0, GATED**
> 13/21 calendar days profitable. On the 18 firing days: 13/18 = **72% profitable**. Sum **+$423** at 0.01 lot, MDD $67, WR 77.3%, expectancy +$0.75/trade.

Also this session: engine flipped **LIVE → SHADOW** in `omega_config.ini` per operator instruction.

The next session's job is **NOT** to deploy this. The next session's job is to write a focused single-config verification harness that explicitly tests THIS ONE setting against ALL current L2 data and produces a clean per-day report — so the operator can see exactly what we have before any further decisions.

---

## 1. FILES TOUCHED THIS SESSION

### Modified (uncommitted)
* `omega_config.ini` lines 67-76 — `[mode]` block. `mode=LIVE` → `mode=SHADOW` with a dated comment block citing this handoff. Hot-reload (~2s) picks it up live. `order_exec.hpp:135` gates `send_live_order` on `g_cfg.mode == "LIVE"`, so this single edit suffices to muzzle FIX traffic on the running engine.

### Created (uncommitted)
* `backtest/honest_backtest_xauusd_v2.cpp` — 524-line sibling of v1 honest backtest harness from Part 2. Does NOT modify v1. Two new composable flags:
   * `--gated` — adds production-style entry gates (spread cap 1.0pt, l2_imb directional confirm 0.502/0.498, regime ∈ {0,2}, watchdog_dead==0). Approximates GoldMicroScalperEngine entry conditions without porting the engine.
   * `--wide` — replaces the v1 6×4×3 geometry grid (TP 0.79..5pt, SL 3..8pt) with a session-scale grid TP ∈ {3,5,8,12}pt × SL ∈ {8,12,16,24}pt × z ∈ {1.5,2.0,2.5,3.0}.

### Outputs (not in repo; live in operator-side outputs folder)
* `outputs/honest_bt_full/` — 21 per-day leaderboards under the v1 grid (ungated baseline).
* `outputs/gated_v1grid/` — 21 per-day leaderboards under `--gated` (Experiment A).
* `outputs/wide_ungated/` — 21 per-day leaderboards under `--wide` (Experiment B).
* `outputs/gated_wide/` — 21 per-day leaderboards under `--gated --wide` (A+B combined).
* `outputs/aggregate_honest_bt.py` — aggregator parameterised by input directory.
* `outputs/agg_{gated_v1grid,wide_ungated,gated_wide}.txt` — per-mode aggregate reports.

---

## 2. THE 21-DAY HEADLINE NUMBERS

Comparison table (60-config grid, 21 substantive XAUUSD days, 0.01 lot, honest fills):

| Run                              | Profitable-majority configs (≥11/21) | Best config             | Best sum USD | Best days+ |
|----------------------------------|--------------------------------------|-------------------------|-------------:|-----------:|
| Baseline (v1 grid, ungated)      | 1 / 60                               | TP=1.5/SL=8/z=2.5       |       −$138 |     11/21 |
| Experiment A (v1 grid + gates)   | 3 / 60                               | TP=1.5/SL=8/z=1.5       |       +$178 |     11/21 |
| Experiment B (wide grid no gates)| 14 / 60                              | TP=3/SL=24/z=2.5        |       +$260 |     14/21 |
| **Experiment A+B (wide + gates)**| **17 / 60**                          | **TP=5/SL=16/z=2.0**    |     **+$423** |     13/21 |

Effects are additive. Gates address signal noise; widening geometry addresses tight-SL bleed (the §6.2 insight from Part 2). Both are required to produce >10 profitable-majority configs.

---

## 3. THE LEADING CANDIDATE — TP=5pt / SL=16pt / z=2.0, GATED

Per-day breakdown across all 21 days (`--gated --wide` from `honest_backtest_xauusd_v2`):

| Date       | N  | WR%   | sum USD  | MDD    | worst  | best  | notes                  |
|------------|----|-------|---------:|-------:|-------:|------:|------------------------|
| 2026-04-09 | 22 | 72.7  |   +5.81  |  49.91 | −13.90 | +5.58 | profitable             |
| 2026-04-10 | 32 | 75.0  |  +27.40  |  27.52 | −14.49 | +5.97 | profitable             |
| 2026-04-13 | 49 | 79.6  |  +52.34  |  54.67 | −14.78 | +5.13 | profitable             |
| 2026-04-14 | 30 | 66.7  |  −40.76  |  64.31 | −15.08 | +5.50 | **worst day**          |
| 2026-04-15 | 33 | 75.8  |  +13.15  |  34.40 | −14.34 | +5.12 | profitable             |
| 2026-04-16 | 21 | 71.4  |   −4.65  |  34.31 | −14.82 | +5.62 | small loss             |
| 2026-04-17 | 44 | 84.1  |  +78.10  |  18.23 | −16.30 | +5.20 | profitable             |
| 2026-04-20 | 20 | 65.0  |  −37.82  |  45.64 | −14.89 | +5.11 | losing day             |
| 2026-04-21 | 57 | 73.7  |   +7.49  |  67.48 | −14.13 | +5.23 | profitable             |
| 2026-04-22 | 26 | 84.6  |  +65.59  |  16.72 | −12.06 | +5.18 | profitable             |
| 2026-04-23 | 65 | 81.5  |  +96.48  |  49.09 | −15.11 | +5.24 | profitable             |
| 2026-04-24 | 34 | 88.2  | +106.99  |  17.71 | −12.80 | +5.27 | **best day**           |
| 2026-04-27 | 18 | 77.8  |  +13.46  |  21.64 | −14.67 | +5.15 | profitable             |
| 2026-04-28 | 42 | 76.2  |  +14.32  |  60.26 | −15.27 | +5.22 | profitable             |
| 2026-04-29 | 25 | 68.0  |  −38.35  |  50.37 | −15.73 | +5.15 | losing day             |
| 2026-04-30 | 31 | 74.2  |   −1.80  |  47.97 | −14.94 | +5.12 | breakeven              |
| 2026-05-01 |  0 |  —    |   +0.00  |   0.00 |   —    |   —   | **regime-gated, 0 trades** |
| 2026-05-04 |  0 |  —    |   +0.00  |   0.00 |   —    |   —   | **regime-gated, 0 trades** |
| 2026-05-05 |  0 |  —    |   +0.00  |   0.00 |   —    |   —   | **regime-gated, 0 trades** |
| 2026-05-06 |  2 | 50.0  |   +0.04  |   5.00 |  −5.00 | +5.04 | nearly gated           |
| 2026-05-07 | 13 |100.0  |  +65.24  |   0.00 |    —   | +5.02 | profitable (clean run) |

**Aggregates**
* 21 calendar days, **18 firing days, 3 zero-trade days** (regime gate correctly muzzled)
* Calendar-day profitability: **13/21 = 62%**
* Firing-day profitability: **13/18 = 72%**
* Total trades: 564 (avg 31/firing-day)
* Total WR: 77.3%
* Sum PnL: **+$423.03**
* Per-trade expectancy: **+$0.75**
* Worst day: −$40.76 (2026-04-14)
* Best day: +$106.99 (2026-04-24)
* Worst-day MDD: $67.48 (2026-04-21)

**Diagnostic on the 3 zero-trade days (2026-05-01/04/05)**: On these days the production-engine regime classifier flagged 99.2%, 99.8%, and 100.0% of ticks as **not in regime {0,2}**. The l2_imb field was also 100% biased one direction on each (i.e., never crossed below 0.5), indicating a regime/feature shift in the capture from 2026-05-01 onwards. The gate behaved correctly — it would not have fired the production engine on those days either.

---

## 4. WHAT THE NEXT SESSION MUST DO — IN ORDER

### 4.1 The explicit single-config verification test (PRIMARY ASK)

The operator asked: "I want an explicit test on this setting showing what we have."

Currently `honest_backtest_xauusd_v2.cpp` only runs grids. Add a fourth CLI mode:

```
--single TP,SL,z      run ONE config (gated path implied; latency parameterised)
--latency N           override LATENCY_TICKS (default 1)
```

Then run **TP=5.0 / SL=16.0 / z=2.0, gated, latency ∈ {1, 3, 5, 10}** across every XAUUSD L2 file currently in `data/`. As of 2026-05-12 that is 21 substantive days plus possibly newer captures (the engine may have appended `l2_ticks_XAUUSD_2026-05-08+.csv` files past the dataset Part 3 used). Run the new harness against **everything ≥3MB** in `data/l2_ticks_*XAUUSD*.csv` and `data/l2_ticks_2026-04-*.csv` (the unprefixed format from before the 04-22 rename).

Produce a single consolidated CSV `outputs/single_config_5_16_2.0_gated.csv`:

```
date, latency, n_trades, wr_pct, sum_usd, mdd_usd, worst_usd, best_usd, exp_per_trade, tp_hits, sl_hits
```

Plus a markdown summary `outputs/single_config_5_16_2.0_gated.md` with:
* 4 aggregate rows (one per latency)
* Day-by-day for each latency
* Equity curve plot (matplotlib, PNG saved to outputs/) — cumulative PnL over time
* Per-trade PnL histogram
* Hold-time distribution

The aggregator should also flag any new firing days (i.e., days the original 21 didn't include) and produce a regression comparison: does the original 21-day result reproduce exactly under latency=1?

### 4.2 Latency robustness gate (BEFORE any live talk)

The §3 concern from Part 2 was that LATENCY_TICKS=1 is optimistic for retail FIX RTT (50-300ms). The wider SL=16 in the candidate should absorb more latency cost than SL=8 did, but the magnitude is unverified.

Acceptance criterion: **the candidate is REJECTED if it does not remain profitable on ≥10 of 18 firing days at LATENCY_TICKS=5.** This is the hard gate before anything else.

### 4.3 Statistical floor

13/18 = 72% has a 95% binomial CI of roughly [47%, 90%]. We do not yet know whether the true win-day rate is 50% or 85%. To tighten the CI, we need either:
* More days — when newer L2 captures land, re-run.
* The same config on other instruments — see 4.4.

### 4.4 Instrument breadth

The repo's `data/` directory has roughly 14 days each of `l2_ticks_US500_*`, `l2_ticks_USTEC_*`, `l2_ticks_NAS100_*`. If TP=5/SL=16/z=2.0 gated is a structural property of mean-reversion at session scale, the same family (suitably re-scaled to each instrument's point value) should produce edge on those captures too. If it works only on XAUUSD, it's XAUUSD-specific and the CI from 4.3 is the binding constraint.

Note: point values differ. The harness's TICK_MULT=100.0 is XAUUSD 0.01 lot. For US500/NAS100 indices and the rest, the next session needs to either parameterise TICK_MULT per instrument or compute trade dollars from contract spec.

### 4.5 Live shadow

Only after 4.1–4.4 produce a robust candidate, deploy as live shadow for **2 calendar weeks minimum**, with the §4 ledger correction (Part 1B) and the Layer 3 disparity script (`scripts/disparity_post_mortem.py`) running daily. Compare backtest vs shadow live by day; require <30% absolute PnL divergence for ≥10 of 14 days before considering real capital.

### 4.6 ONLY AFTER ALL ABOVE

Real money at **0.01 lot** (broker minimum). Double the lot only after every 2 clean shadow weeks. Worst-case 0.01 lot MDD here is ~$67/day; at 0.10 lot that's $670; at 1.0 lot that's $6,700. The current `max_lot_gold=0.01` ceiling in `omega_config.ini` is the right starting point.

---

## 5. UNCHANGED OPEN QUESTIONS FROM PRIOR HANDOFFS

* **Part 1B §4 ledger correction** — uncommitted; operator owns the commit.
* **Part 2 §3 fill-model direction** — was production-fill systematically over- or under-stating profit? Part 3 didn't address this; still open.
* **Part 2 §4 signal port** — Part 3's `--gated` approximates the production engine but is not a true port. A faithful port of `GoldMicroScalperEngine`'s entry logic is still open work. The gated approximation suggests gates matter more than signal specifics; a faithful port would confirm or refute that.

---

## 6. RULES FOR THE NEXT SESSION

1. **Read this handoff + Part 2 + Part 1B end-to-end before touching anything.**
2. Verify on-disk state: `git status`, `git log -1`, `git diff --stat omega_config.ini`. The single config-line change should match this handoff's claim.
3. Confirm shadow: `grep -A2 "^\[mode\]" omega_config.ini` must show `mode=SHADOW`.
4. **DO NOT** flip back to `mode=LIVE` for any reason without explicit operator instruction.
5. **DO NOT** recommend running this candidate live based on Part 3 alone. The §4.2 latency gate and the §4.4 instrument breadth must clear first.
6. The Part 3 `honest_backtest_xauusd_v2.cpp` file is 524 lines, well under the operator's 800-line full-file threshold. If you modify it, output the full file in chat per operator preference.
7. **Never modify `microscalper_crtp_sweep.cpp`, `omega_main.hpp`, `order_exec.hpp`, `OmegaTradeLedger.hpp`, `IndexFlowEngine.hpp`, or any other core engine file unless the operator explicitly instructs it.** The Part 1B ledger fix is the one exception, and it's already done and uncommitted.
8. Operator preference: warn at 70% context with summary. Stop and write a follow-up handoff. Don't stretch.
9. Operator preference: full file output when changing files ≤ 800 lines.

---

## 7. REPRODUCING PART 3'S RESULTS

```bash
cd /Users/jo/omega_repo

# Build the v2 harness
g++ -std=c++17 -O2 -Wall -Wextra backtest/honest_backtest_xauusd_v2.cpp \
    -o backtest/honest_backtest_xauusd_v2

# The 21 viable XAUUSD days used in Part 3:
DAYS="data/l2_ticks_2026-04-09.csv data/l2_ticks_2026-04-10.csv \
      data/l2_ticks_2026-04-13.csv data/l2_ticks_2026-04-14.csv \
      data/l2_ticks_2026-04-15.csv data/l2_ticks_2026-04-16.csv \
      data/l2_ticks_2026-04-17.csv data/l2_ticks_2026-04-20.csv \
      data/l2_ticks_2026-04-21.csv data/l2_ticks_XAUUSD_2026-04-22.csv \
      data/l2_ticks_XAUUSD_2026-04-23.csv data/l2_ticks_XAUUSD_2026-04-24.csv \
      data/l2_ticks_XAUUSD_2026-04-27.csv data/l2_ticks_XAUUSD_2026-04-28.csv \
      data/l2_ticks_XAUUSD_2026-04-29.csv data/l2_ticks_XAUUSD_2026-04-30.csv \
      data/l2_ticks_XAUUSD_2026-05-01.csv data/l2_ticks_XAUUSD_2026-05-04.csv \
      data/l2_ticks_XAUUSD_2026-05-05.csv data/l2_ticks_XAUUSD_2026-05-06.csv \
      data/l2_ticks_XAUUSD_2026-05-07.csv"

# Reproduce Experiment A+B (gated wide):
mkdir -p outputs/gated_wide
for f in $DAYS; do
  d=$(echo "$f" | grep -oE '2026-[0-9]{2}-[0-9]{2}')
  backtest/honest_backtest_xauusd_v2 --gated --wide "$f" \
      > outputs/gated_wide/honest_bt_${d}.txt
done

# Extract just the TP=5/SL=16/z=2.0 row from each day under honest fills:
for f in outputs/gated_wide/honest_bt_*.txt; do
  d=$(basename "$f" | grep -oE '2026-[0-9]{2}-[0-9]{2}')
  row=$(awk '/FILL MODEL: honest-fill/,/VERDICT/' "$f" | \
        grep -E "^\s*[0-9]+\s+5\.00\s+16\.0\s+2\.0\s+")
  echo "$d  $row"
done
```

Expected aggregate matches §3 of this handoff exactly: 13/21 calendar profitable, 18 firing days, +$423.03 total.

---

## 8. NEXT SESSION FIRST-MESSAGE TEMPLATE

> Read `HANDOFF_S26_PART1B...md`, `HANDOFF_S26_PART2...md`, and `HANDOFF_S26_PART3.md` end-to-end. Confirm on-disk state (git status, mode=SHADOW in omega_config.ini). Add `--single TP,SL,z` and `--latency N` flags to `backtest/honest_backtest_xauusd_v2.cpp`. Run it for **TP=5.0 / SL=16.0 / z=2.0 gated** at LATENCY_TICKS ∈ {1, 3, 5, 10} across every XAUUSD L2 CSV currently in `data/` (including any captures past 2026-05-07 the engine has appended since). Produce `outputs/single_config_5_16_2.0_gated.csv` (one row per day-per-latency) and `outputs/single_config_5_16_2.0_gated.md` (with equity curve PNG and per-trade histograms). Report. Do NOT recommend live deployment regardless of the result — the operator owns that decision.

---

End of S26 Part 3 handoff.

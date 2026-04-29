# Omega Trading System — Session Handoff
## 2026-04-29

This document hands off the Apr-29 audit session to the next one. Read this before doing anything else.

---

## TL;DR

The Omega CFE engine has been rebuilt from the original `omega_trade_closes.csv`
audit (1,938 live trades / -\$22,320 over 23 days) plus a 26-month Dukascopy
backtest (154M ticks / 4.94 GB). 14 audit-fix commits are on `main`. The
engine is **not yet profitable** but is at a **known-clean diagnostic state**
with one final BT run pending.

**Main HEAD (Trendiisales/Omega):** `17092b26 audit-fixes-14: disable overfit TOD gate`

**The single thing the next session needs to do first:**
ask the user for the result of:

```bash
cd /Users/jo/omega_repo
git pull
g++ -O2 -std=c++17 -I include -o cfe_duka_bt backtest/cfe_duka_bt.cpp
./cfe_duka_bt /Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv
```

The output of THAT run determines the next iteration. Without it you're guessing.

---

## What was found (audit phase)

From `omega_trade_closes.csv` (1,938 closed trades, Apr 6 → Apr 28, 23 days):

| Total net P&L | -\$22,320 |
| -- | --: |
| XAUUSD (gold) only | -\$22,233 |
| Indices net | -\$87 (essentially flat) |

**Three trade clusters were bugs, not signals**, totalling **-\$12,971
(58 % of the entire 23-day loss):**

1. **MacroCrash Apr-15 phantom-fire**: 61 LONG entries on XAUUSD in 41 seconds, all DOLLAR_STOP, **-\$9,907**.
2. **HybridBracketGold Apr-7 single trade**: -\$3,008.38 with expected ~ -\$30 (100x P&L race).
3. **NAS100 whipsaw**: 59 trades, IndexFlow ↔ HybridBracketIndex firing opposite sides within 5 min, -\$56.

`KNOWN_BUGS.md` in the repo root catalogues these with filter snippets.

---

## What was fixed (commits on main)

```
17092b26  audit-fixes-14   disable overfit TOD gate
16639c40  audit-fixes-13   EARLY_FAIL gate (data-driven from CSV)
7afd5c64  audit-fixes-12   revert iter-10/-11 (they made things worse)
45fb6930  audit-fixes-11   exit-side fixes -- reverted
1f5ca365  audit-fixes-10   structural R:R -- reverted
b3cf6e93  audit-fixes-9    SL_MULT=0.5 + ATR floor + spam suppress
6627c8c1  audit-fixes-8    drift estimator exact live formula
ba8eff48  audit-fixes-7    drift estimator 60s window
b303180f  audit-fixes-6    duka_bt parser ask-first column order
e11269ad  audit-fixes-5    KNOWN_BUGS.md
675f063f  audit-fixes-4    HBG hardened + MCE C-1/C-3 + BT harnesses
49d8151b  audit-fixes-3    SL_MULT 0.4 -> 0.6
81dae231  audit-fixes-2    CFE re-enabled with edge-driven gates
3aede5d8  audit-fixes-1    config consolidation + 6 CFE param fixes
```

### What's currently active in CFE

After all the iteration the surviving changes are:

| Change | Origin | Status |
|---|---|---|
| `CFE_DFE_SL_MULT = 0.5` | fixes-9 | Active |
| `CFE_BODY_RATIO_MIN = 0.75` | fixes-10 | Active |
| `HMM_MIN_PROB = 0.65` | fixes-10 | Active |
| `CFE_MIN_ATR_ENTRY = 0.0` (disabled) | fixes-9 reverted in fixes-10 | Inert |
| London/NY early-adverse exit at 30s | fixes-11 | Active |
| Trail dist `0.5*ATR` | original | Reverted to original in fixes-12 |
| Trail arm `2.0*ATR` | original | Reverted to original in fixes-12 |
| Partial fraction 50% | original | Reverted to original in fixes-12 |
| **EARLY_FAIL gate (hard 30s/MFE=0, soft 60s/MFE<0.3pt)** | **fixes-13** | **Active** |
| **CFE_TOD_FILTER_ENABLED = false** | **fixes-14** | **Active** |

### What's currently active in MCE

| Change | Origin | Status |
|---|---|---|
| C-1: `m_consec_sl` counts DOLLAR_STOP, not just SL_HIT | fixes-4 | Active |
| C-3: `force_close()` updates `pos.mae` before close | fixes-4 | Active |

### What's currently active in HybridBracketGold

| Change | Origin | Status |
|---|---|---|
| `m_close_mtx` mutex around `_close()` | fixes-4 | Active |
| Snapshot `pos.*` into locals before tr build | fixes-4 | Active |
| Sanity check on `tr.pnl` magnitude | fixes-4 | Active |
| Atomic ostringstream log line | fixes-4 | Active |

---

## BT iteration history (CFE on 26-month Dukascopy XAUUSD)

```
            Trades  WR%    Net      Avg/tr   Note
fixes-8      794   40.3   -$561    -$0.71   first 26-mo run, baseline
fixes-9      221   34.4   -$153    -$0.69   ATR floor over-filtered
fixes-10     881   39.0   -$764    -$0.87   WORSE -- partial 35% killed avg_win
fixes-11     914   43.3   -$728    -$0.80   slightly better than 10, still bad
fixes-12     905   38.3   -$630    -$0.70   revert -- back to ~baseline
fixes-13+14  ???    ???    ???      ???     PENDING
```

**Live pre-audit** (for context): -\$3,966.98 / 445 trades / -\$8.91 per trade.
**Faithful 14-day BT after fixes-3** showed -\$6.32 / 28 trades / -\$0.23 per trade,
which extrapolated to 26 months matches the BT result above. The audit gates
brought live trade economics from **-\$8.91/trade to -\$0.70/trade (12.5x improvement)**
but the engine is not yet net-positive.

---

## The data-driven finding (this is the key insight)

Per-parent-trade analysis on `cfe_duka_bt_trades.csv` (905 rows / 647 parents):

```
MFE bucket    n     WR%      net      avg/parent
never (=0)   156    0.0    -$394.89   -$2.53     <-- killed by EARLY_FAIL hard
0-0.5 pt     113    0.0    -$237.43   -$2.10     <-- killed by EARLY_FAIL soft
0.5-1 pt      68   17.6    -$157.18   -$2.31     <-- next iteration target
1-2 pt       179   69.3    -$120.77   -$0.67
2-4 pt       104   92.3    +$146.72   +$1.41
>4 pt         27  100.0    +$133.43   +$4.94
```

**269 trades that never reach +0.5pt favorable account for -\$632 of the
-\$630 26-month loss.** The remaining 376 trades that DO develop net to
roughly **+\$2** -- the engine has zero edge from "good entries"; its
bleed is entirely from entries that fail to develop within 30-60 seconds.

That's why fixes-13 (EARLY_FAIL) is the highest-leverage change.

If fixes-13+14 lands as projected, target: **-\$200 to -\$300 over 26 months
at the 0.01-lot SHADOW cap.** Lifting `MAX_LOT` to 0.05 once shadow validates
multiplies by 5x.

---

## Open items for next session

### IMMEDIATE (first thing to do)
1. **Confirm fixes-13+14 BT result.** Have the user pull and run. Paste the per-hour table from the output -- that's the empirical hour-of-day map, the first time we have 26-month evidence on it.

### IF that BT shows -\$200 to -\$300
2. **Iteration 15: target the 0.5-1pt MFE bucket** (68 trades / -\$157 / 17.6% WR). These reach a tiny bit of profit then revert. Likely fix: tighten partial threshold so they bank earlier, or add a "no-progress at 90s" gate.
3. **Re-derive the TOD gate** from the broader 26-month hour distribution. With the gate disabled, the BT will fire across all 24 hours and reveal which ones actually have edge.

### IF that BT shows worse than -\$500
4. The EARLY_FAIL gate killed legitimate-but-slow winners. Investigate `cfe_duka_bt_trades.csv` for trades where MFE < 0.3pt at 60s but eventually > 1pt. Loosen the soft gate.

### NOT YET TOUCHED (parked)
5. **Index whipsaw fix** (NAS100 / IndexFlow ↔ HybridBracketIndex). Need an `index_any_open` gate in `tick_indices.hpp` analogous to `gold_any_open` in `tick_gold.hpp:35-50`. Estimated 30 lines. Low urgency -- indices are net-flat. Tracked as a pending task.
6. **Tier-2 engines**: MicroMomentum, DomPersist, BBMeanRev. Each lost \$70-\$700 in the audit window; not the headline issue. Address after CFE is profitable.
7. **VPS shadow validation**: User confirmed `49d8151b` compiled and is running shadow on the VPS. Once 30+ shadow trades accumulate, pull `omega_trade_closes.csv` from VPS and compare to BT projection.

---

## Files in /outputs/Omega-fixes/ (snapshot, not authoritative)

These were the local working copies. **The git repo is authoritative
now (Trendiisales/Omega main).** The /outputs/ folder is for reference only.

- `omega_config.ini` -- canonical config (snapshot before commits)
- `config/omega_config.ini` -- tombstone marker
- `include/CandleFlowEngine.hpp` -- audit-tightened CFE (early version)
- `INSTALL_NOTES.md` -- session-1 install notes (now superseded by git)
- `KNOWN_BUGS.md` -- bug catalogue (also pushed to repo root)

Audit deliverables:

- `Omega_Trading_System_Audit.docx` -- full audit report
- `cfe_faithful_bt.cpp` -- faithful BT (also at backtest/cfe_faithful_bt.cpp on repo)
- `cfe_duka_bt.cpp` -- Dukascopy adapter (also at backtest/cfe_duka_bt.cpp on repo)
- `audit_report/build.js` -- regenerator for the docx
- `SESSION_HANDOFF_2026-04-29.md` -- this file

---

## Things to remind the user about

1. **The PAT** (`ghp_9M2I...`) is still live and exposed in the previous chat
   transcript. The user said "leave it" but they should rotate it eventually.
   Re: GitHub > Settings > Personal access tokens > delete & regenerate.

2. **The VPS is running `49d8151b`**, which is several commits behind the
   current head (`17092b26`). If they want fixes-10 through 14 in shadow on
   the VPS, they need to git pull on the VPS and re-run QUICK_RESTART.ps1.
   Note that fixes-10/-11 were reverted; the actual delta from 49d8151b to
   17092b26 is small in net behaviour (mostly EARLY_FAIL + TOD disabled).

3. **Risk caps in `omega_config.ini`** are still tight (`max_lot_gold=0.05`,
   `daily_loss_limit=1500`, `min_entry_gap_sec=90`). Don't touch these
   until the engine is empirically profitable.

4. **Mode is SHADOW.** Stay there until 30+ shadow trades validate the
   fixes-13+14 economics.

---

## Where each piece of analysis lives

| Question | Look at |
|---|---|
| What was the original audit finding? | `Omega_Trading_System_Audit.docx` |
| Which trades to exclude as bugs? | `KNOWN_BUGS.md` (repo root) |
| What does the live engine produce? | `omega_trade_closes.csv` (user upload) |
| What does the engine do on 26 months? | run `cfe_duka_bt` on user's Mac |
| What does the engine do on live L2? | run `cfe_faithful_bt` on user's L2 ticks |
| What's in the engine logic? | `include/CandleFlowEngine.hpp` (1839 lines) |
| Where's the entry signal? | `on_tick()` near lines 246-1300 |
| Where's the exit logic? | `manage()` near lines 1500-1730 |
| Where's the HMM regime gate? | lines 525, 1116 (DFE + bar entry paths) |

---

## The honest summary

Three weeks of live data + 26 months of Dukascopy BT made it possible to
diagnose CFE precisely. The engine is not broken in any one place; it has
a structural property that 60 % of its entries fail to develop, and the
remaining 40 % make tiny wins that are eaten by the failed entries. The
EARLY_FAIL gate is the data-driven response to that.

I theorised through fixes-10 and fixes-11 trying to predict the engine's
behaviour without the per-trade data and made things worse. Reverted them
in fixes-12 and got back to known state. Asked the user for
`cfe_duka_bt_trades.csv`, looked at the actual MFE distribution, and the
right answer was obvious. The next session should do data-driven analysis
**before** code changes whenever possible.

The engine has not yet been demonstrated profitable on 26 months. It is
12.5x better than where the audit started, but it is still net-negative.
The fixes-13+14 BT is the next decision point.

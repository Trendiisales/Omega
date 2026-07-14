# Sub-30m gold BE-mimic RE-CHECK — GoldDon15m / GoldDon10m (S-2026-07-14, operator re-open order)

**Context:** the 14u session backtested BE-mimics for the two newly-wired sub-30m engines and
returned NO (10m 0/216 cells; 15m 12/216 close-grade passers collapsed at 1m intrabar truth,
best cell +$14,161/+$7,155 → +$2,475/−$4,799 WF1-neg). Tombstoned "re-open only with a NEW
mechanism basis". **Operator explicitly re-opened** ("i want 2x mimics added for these 2 …
max protections"). New basis found: the prior grid fixed `be=0.10` and capped `arm≤0.5` —
the **arm dimension above 0.5 was never tested**. This study sweeps it, at **1m intrabar
truth ONLY** (no close-grade column exists anywhere in this study, so the granularity
optimism that faked the first 15m pass is structurally absent).

## VERDICT — both books PASS at 1m truth; WIRED SHADOW

| Book | Cell (be/arm/lc/cap/pend) | Leg | n(6mo) | usd @1×/@2× | PF | WF h1/h2 | worst | maxDD | FULL |
|---|---|---|---|---|---|---|---|---|---|
| **GoldDon15mMimic** | 0.10/1.00/0.5/96/12 | T gb.08 | 102 | **+$10,789/+$8,415** | 1.80 | +12.76/+9.76 | −0.55% | 2.2% | + |
| | | W gb.20 | 102 | +$11,266/+$8,892 | 1.84 | +13.78/+9.87 | −0.55% | 2.2% | + |
| **GoldDon10mMimic** | 0.10/1.00/1.0/144/12 | T gb.08 | 205 | **+$14,028/+$9,227** | 1.37 | +17.23/+12.36 | −1.05% | 7.9% | + |
| | | W gb.20 | 205 | +$12,475/+$7,674 | 1.33 | — | −1.05% | ~7.9% | + |

Gate (all met, per leg): n≥25, net>0 @1×(5bp) AND @2×(10bp), PF≥1.3, WF both halves >0
(parent-study split 2026-04-14), FULL-window (2024-06→) net >0. Judged STANDALONE
(companion independent-engine rule).

## Why the prior NO and this PASS coexist (not a contradiction)

The failed space was `arm ∈ {0.15,0.25,0.5}`: at sub-30m grain the trail arms almost
immediately and whipsaws on 1m noise — that churn is what the 1m truth check exposed.
At `arm=1.0` the trail arms only after +1% MFE (~$40/oz), so legs resolve as: cold cut
at −lc, flush at cap, or a trail that engages only on real runs. Different operating
regime, same companion-at-BE mechanism (books nothing until BE covered — the only
mimic type permitted, feedback-no-immediate-entry-upjump-mimic-only).

## arm≥2.0 family REJECTED despite bigger nets (overfit trap, recorded so nobody re-adds it)

Extension sweep showed monotone improvement with arm and cap: 10m `arm=99` (never arms)
cap192 = +$47k PF1.95. That is NOT a mimic — the trail never engages, the book degenerates
to "enter at BE+0.1%, stop −lc, hold 32h", i.e. a no-trail time-stop clone of the parent
trend riding the 2026 gold ramp (WF1 +69 vs WF2 +27, regime-lopsided). Chosen cells are
interior (arm 0.75/1.25 neighbors pass), cap-insensitive (15m cap48–192 ±3%; 10m flat
above 144), which shows the TRAIL is doing the work, not the hold.

## Max-protection notes (the operator ask)

- 15m lc=0.5 is the tightest cut in the wired mimic family — worst clip −0.55% ≈ −$220
  per $40k clip; book maxDD 2.2%. lc0.8 costs ~35% of net at 2× the DD → 0.5 is BEST.
- 10m lc=1.0: lc1.5 fails the W leg (PF<1.3) at neighboring cells; 1.0 both bounds the
  worst clip (−1.05% ≈ −$420) and maximizes gate-robust net.
- DRAWDOWN-CANCEL ACTIVE on both (be0.10 legs enter barely in profit, the cut fires for
  real — same finding as the 5-book bb study, NOT the near-inert be=1.0 regime).

## Model / fidelity / repro

- Harness `backtest/gold_sub30_mimic_1m_bt.cpp` — exact `GoldTrendMimicBook::on_h1_bar()`
  replica managed on the **1m stream** (pend/cap in parent bars, scaled ×tf). Entries =
  REAL streams: `DUMP_ENTRIES=1 DON15_STOP=1 DON10_SWEEP=1 ./gold_subh30_tf_bt`, cfg
  `15m|DONf|60/35 stop3.5ATR` (629) and `10m|DONf|30/35 stop3.0ATR` (1,359).
- Data `/Users/jo/Tick/xau_1m_spliced_2024_2026.csv` (837,302 bars, CERTIFIED CLEAN
  2026-07-14; spot-at-MGC-cost substitution as in the parent study).
- Sweeps: full 4,096-cell grid/book (be{.10,.25,.5,1}×arm{.15,.25,.5,1}×gb{.08,.1,.2,.3}
  ×lc{.5,1,1.5,2}×cap{12,24,48,96}×pend{6,12}) + refinement (arm .75–2.0, cap→144) +
  arm-limit extension (arm 2.5/3/99, cap→192). Pass counts: 15m 38/512 dual-leg (full),
  73/240 (refinement); 10m 80/512, 24/240.
- Live caveat: the live book manages on native 15m/10m bars (coarser than the 1m sim) —
  same accepted relationship as the five bb-wired mimics (validated fine, managed native).
  Shadow ledger is the judge; M1/tick-vs-live parity re-check owed before any LIVE flip
  (standing mimic caveat).

```
c++ -std=c++17 -O2 backtest/gold_sub30_mimic_1m_bt.cpp -o /tmp/gold_sub30_mimic_1m_bt
/tmp/gold_sub30_mimic_1m_bt <entries.csv> /Users/jo/Tick/xau_1m_spliced_2024_2026.csv <15|10>
```

Wired: `engine_init.hpp` GoldTrendMimicLadder block (books 14–15) + `omega_main.hpp`
mimic_tag. Vault: `GoldSub30mStudy2026H1.md` (tombstone superseded) + log. 2026-07-14.

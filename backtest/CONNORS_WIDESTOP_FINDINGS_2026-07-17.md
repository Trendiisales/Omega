# ConnorsRSI2 WIDE catastrophic-stop test — REJECTED both hosts (2026-07-17)

**Question (operator-approved, S-2026-07-17i handoff item 1):** MR parents (ConnorsRSI2 NAS + GER)
have NO stop (`o.sl=0`). Standing verdict "no cold cut on MR" was about TIGHT cuts — does a WIDE
disaster stop (K×ATR20 below avg entry) cap the tail without killing the edge?

**Answer: NO. Wide stops REJECTED on both hosts. The standing no-cold-cut verdict extends to wide
catastrophic stops.** The tail protection ConnorsRSI2 actually has — regime gate (SMA200 / asym
bear-veto) + MAXHOLD10 + BOOK_CAP — is what caps the tail; a price stop makes the tail WORSE.

## Harness (faithful)

`backtest/connors_widestop_bt.cpp` — drives the REAL `ConnorsRSI2Engine` with the exact certified
tick pattern of `connors_regime_gate_audit.cpp` (NAS, deployed GATE=1 + SCALEIN, 8pt RT) and
`connors_ger_gate_audit.cpp` (GER, deployed SMA200 gate, no scale-in, 3pt RT). **Baseline reproduces
the certified figures exactly: NAS n=142 PF4.43; GER n=226 PF1.35.** Stop = harness overlay on daily
bar h/l (engine untouched): stop_px = avg_entry − K×ATR20(entry), re-anchored on scale-in, live from
the day after entry, worse-of gap fills (bar open if opens below stop), `force_close()` → real
`_close()` → cost applied. Engine may re-enter at the same close (no cooldown — as a live overlay
would behave). Two modes: **wick** (intraday resting stop) and **close-eval** (exit at cash close if
close≤stop — the strongest defensible variant for a close-acting engine). K ∈ {2,3,4,5,6}; 1× and
2× cost. Data both CERTIFIED CLEAN by `data_integrity_gate.py`.

## Results (1× cost; 2× cost same shape)

### NAS100 (deployed: GATE=1 bear-veto, scale-in, 8pt)
| cell | n | PF | net | maxDD | worst | stops |
|---|---|---|---|---|---|---|
| BASE no-stop | 142 | **4.43** | 25456 | **1353** | **−806** | 0 |
| K=2 wick | 165 | 2.97 | 25449 | 1376 | −1018 | 24 |
| K=3 wick | 153 | 2.87 | 24069 | 1984 | −1519 | 13 |
| K=4 wick | 145 | 3.96 | 25369 | 1619 | −919 | 3 |
| K=5/6 wick | 144 | 3.99/3.86 | 25092/24812 | 2018/2297 | −892/−1067 | 2 |
| K=3..6 close-ev | 143-147 | 3.57-4.13 | ~25k | 1971-2294 | −986..−1458 | 1-5 |

**No NAS cell improves ANY risk metric.** Worst trade worse in every cell, maxDD worse in every
cell, PF lower in every cell, net ≤ base. 2022 bucket identical everywhere (n=8 +1842 — the gate is
what protects the bear, the stop never binds).

### GER40 (deployed: SMA200 gate, 3pt)
| cell | n | PF | net | maxDD | worst | stops |
|---|---|---|---|---|---|---|
| BASE no-stop | 226 | **1.35** | **5734** | 2895 | −1470 | 0 |
| K=2..6 wick | 227-255 | 1.11-1.22 | 2434-4292 | 2885-3788 | −1893..**−2969** | 5-46 |
| K=6 close-ev | 226 | 1.37 | 5891 | 2656 | −1470 | 4 |
| K=2..5 close-ev | 227-247 | 1.23-1.33 | 4025-5534 | 2408-3136 | −1079..−1931 | 5-31 |

Wick stops: net −25..−58%, worst trade balloons to −2969 (vs −1470 base). Close-eval K=6 is a
noise-level cosmetic (+2.7% net on n=4 hits, worst unchanged, ragged non-monotone K-sensitivity
→ selection overfit, not structure).

## Mechanism (the kill replicates the real mechanism — episode-level audit, K=4 wick)

- **NAS: zero saves in 10.5 years.** All 3 stop-hits lost MORE than the base ride: −919 vs −559
  (2018-03), −717 vs −352 (2018-10), −797 vs −368 (2019-07). The stop sells the wick low of exactly
  the dip the engine is paid to buy; the bounce then happens without the position.
- **GER: the stop MANUFACTURES the new worst trade.** 2025-04-03 (tariff crash): stopped −2969
  (gap-through, fill at the open below the stop) vs base ride −1158 — the stop made the episode
  2.6× worse. Two further losses exist ONLY because of the stop: after a stop-out the engine is
  still oversold → re-enters at the close → stopped again (2020-02-25 −902, 2021-11-24 −498).
- GER saves exist (COVID 2020-02-21 +827, 2020-10-14 +672, 2018-01-25 +427) but total damage
  (worse-off wick exits + manufactured re-entries + the 2025 gap blowout) exceeds them ~2:1.
- **The 2022 bucket is IDENTICAL in every cell on both hosts** — regime gate exposure control, not
  a price stop, is what handles the bear. A wide stop adds nothing where it was supposed to help.

## Verdict

- **No stop of any width on ConnorsRSI2 (NAS or GER).** Tail lever, if ever wanted, is BOOK_CAP /
  smaller lot / the regime gate — exposure control, not price stops.
- Engine header ADVERSE-PROTECTION annotation extended with this verdict (wide-stop tested, ref here).
- The live NAS −$777 underwater trade class (GUI question that triggered this) is by-design: worst
  faithful base trade −806pt books at MR exit; a stop would have made that class worse, not better.

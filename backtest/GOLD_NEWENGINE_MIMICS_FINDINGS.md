# BE-mimic validation — 5 new gold engines (S-2026-07-14bb)

**Task:** the mandatory BACKTESTED BE-mimic verdict for the mimic books of the 5 new gold
engines being wired concurrently (gate: project-mimic-drawdown-cancel-gate +
feedback-engine-loss-protection-provision). **BACKTEST ONLY — nothing wired here.**
Operator spec: *"add a mimic for each, same BE threshold for mimic"* → find ONE COMMON
`be_entry_pct` passing the gate on ALL 5 books, prefer the smallest common passer.

## VERDICT — common be = 0.10, ALL 5 BOOKS PASS (dual-grain)

**be_entry_pct = 0.10 is the smallest common passer** and passes the full gate on every
book at BOTH close-grade and fine-grain (M30 sub-bar) management: net>0 after cost at 1×
(5bp) AND 2× (10bp), WF both halves positive (split 2025-07-01), PF ≥ 1.3 both legs at
fine grain, plateau ≥ 15/54 combos per book. Alternates: be=0.15 fails GoldDonH1 at fine
grain (1/54 combos, W leg +0.2% @2× — fragile); be=0.5 fails MgcTf1h outright (0/54);
**be=1.0 is the conservative fallback** (passes all 5, PF 2.0–4.6, DD ≤ 4%, ~⅓ the net).
The survivor-cell trap (`engine_init.hpp` XAU_4h_DonchN20: shipped be=0.10 FAILED 540/540)
does NOT recur here — these parents are M30/H1-cadence with far more entries and the
fine-grain check was run cell-by-cell (below); the trap cell was an H4 parent judged at
M1 truth, and its failure mode (close-grade collapse) is exactly what the FINE columns
test. Honest limit: finest grain in the certified data is M30, not M1/tick — an M1/tick
re-check is owed before any LIVE sizing (same caveat as every wired mimic).

## Model + fidelity

- Harness `backtest/gold_newengine_mimic_bt.cpp` — EXACT replica of
  `include/GoldTrendMimicLadder.hpp` `GoldTrendMimicBook::on_h1_bar()` close-grade
  semantics: legs PENDING at parent-open trigger px; ENTER at the BE level once the
  favorable bar extreme clears +be% (both directions; shorts trig*(1−be/100)); managed
  from the NEXT bar; pre-arm LOSS_CUT books exactly −lc%; arm at +arm% MFE; peak-giveback
  trail books (1−gb)·peak (BE-floored); window-cap flush after cap_bars native bars;
  adverse-first intrabar l→h→c sequencing; pend_bars cancel.
- **Parent entries are the REAL streams**, not re-implementations: env-gated
  `DUMP_ENTRIES=1` added to `backtest/gold_shorttf_bothways_bt.cpp` (KELT/TF1H/DON
  configs, S-2026-07-14ax) and `backtest/mgc_tf1h_port_bt.cpp` (real
  XauTrendFollow1hEngine, VT=0 LC=0 port config, S-2026-07-14ay). Default behavior of
  both harnesses unchanged.
- Data: `backtest/data/mgc_30m_spliced_2024_2026.csv` (CERTIFIED CLEAN 2026-07-14).
  Window 2024-09-01 → 2026-07-13 (22.5 mo), WF split 2025-07-01.
- Cost: `rt_cost_bp=5` per clip (the conservative live XAU_4h_DonchN20 debit; real MGC
  RT $4.10 on ~$40k notional ≈ 1bp) + 2× stress (10bp). USD at 10oz × entry px notional
  (≈ 1 MGC per leg).
- Native mgmt bar: M30 for GoldKeltM30, H1 for the rest. FINE = mgmt on the M30 stream
  with pend/cap ×2 (H1 books); for the M30 book M30 IS the finest grain (CLOSE = FINE).
- Judged STANDALONE (companion independent-engine rule) — additive book, never vs the
  parent's ride.

Sweep: be {0.10,0.15,0.25,0.5,1.0} × arm {0.15,0.25,0.5} × gb {0.08,0.10,0.20,0.30} ×
lc {1.0,1.5,2.0} × cap {12,24,48} (M30 book {24,48,96}) × pend {6,12} = 1080 cells/book,
close + fine = 10 sweeps. Leg gate: n≥25, net>0 @1× and @2×, WF both halves >0.

## Dual-grain plateau (combos passing BOTH grains, of 54 per be; T+W legs both required)

| be | MgcTf1h | GoldKeltM30 | GoldTfBw1040 | GoldTfBw20100 | GoldDonH1 | all-5? |
|---|---|---|---|---|---|---|
| **0.10** | **23** | **37** | **28** | **15** | **19** | **PASS (smallest)** |
| 0.15 | 33 | 52 | 47 | 41 | 1 | fragile (DonH1) |
| 0.25 | 54 | 36 | 49 | 54 | 9 | pass, thin DonH1 |
| 0.50 | 0 | 44 | 46 | 22 | 49 | FAIL (MgcTf1h) |
| 1.00 | 21 | 52 | 29 | 47 | 30 | PASS (fallback) |

## Per-book verdicts at common be=0.10 (chosen params; CLOSE vs FINE side by side)

All books: 2 legs, T = gb 0.08, W = gb 0.20, pend_bars=12. n = clips per leg over 22.5 mo.
net in %-points summed per leg (1×/2× cost); usd at 10oz notional per clip; WF at 1×.

### MgcTf1h — arm 0.50, lc 2.0, cap 24 (H1)
| leg | grain | n | net1x | net2x | PF | WR | WF h1/h2 | worst | maxDD | usd 1×/2× |
|---|---|---|---|---|---|---|---|---|---|---|
| T gb.08 | close | 234 | +48.8% | +37.1% | 1.93 | 76% | +28.1/+20.7 | −2.05% | 12.6% | +17,685/+13,149 |
| T gb.08 | **FINE** | 234 | +40.8% | +29.1% | 1.78 | 76% | +25.2/+15.6 | −2.05% | 12.7% | +14,490/+9,954 |
| W gb.20 | close | 234 | +41.3% | +29.6% | 1.79 | 76% | +23.9/+17.5 | −2.05% | 13.4% | +14,622/+10,086 |
| W gb.20 | **FINE** | 234 | +35.4% | +23.7% | 1.68 | 76% | +22.0/+13.4 | −2.05% | 13.6% | +12,225/+7,688 |

### GoldKeltM30 — arm 0.25, lc 2.0, cap 96 (M30 native; CLOSE == FINE, M30 is the data's finest grain)
| leg | grain | n | net1x | net2x | PF | WR | WF h1/h2 | worst | maxDD | usd 1×/2× |
|---|---|---|---|---|---|---|---|---|---|---|
| T gb.08 | M30 | 822 | +97.1% | +56.0% | 1.55 | 87% | +35.0/+62.0 | −2.05% | 8.5% | +38,397/+22,617 |
| W gb.20 | M30 | 822 | +69.0% | +27.9% | 1.39 | 87% | +24.4/+44.6 | −2.05% | 9.1% | +27,340/+11,560 |

### GoldTfBw1040 — arm 0.15, lc 1.0, cap 48 (H1)
| leg | grain | n | net1x | net2x | PF | WR | WF h1/h2 | worst | maxDD | usd 1×/2× |
|---|---|---|---|---|---|---|---|---|---|---|
| T gb.08 | close | 692 | +97.3% | +62.7% | 1.87 | 84% | +36.1/+61.1 | −1.05% | 4.9% | +39,861/+26,562 |
| T gb.08 | **FINE** | 692 | +65.5% | +30.9% | 1.64 | 86% | +19.6/+45.9 | −1.05% | 6.3% | +27,673/+14,373 |
| W gb.20 | close | 692 | +74.1% | +39.5% | 1.66 | 84% | +25.8/+48.3 | −1.05% | 5.2% | +30,748/+17,449 |
| W gb.20 | **FINE** | 692 | +47.3% | +12.7% | 1.46 | 86% | +14.1/+33.2 | −1.05% | 7.5% | +20,170/+6,871 |

### GoldTfBw20100 — arm 0.15, lc 2.0, cap 48 (H1)
| leg | grain | n | net1x | net2x | PF | WR | WF h1/h2 | worst | maxDD | usd 1×/2× |
|---|---|---|---|---|---|---|---|---|---|---|
| T gb.08 | close | 600 | +76.6% | +46.6% | 1.73 | 90% | +30.1/+46.5 | −2.05% | 14.6% | +29,261/+17,703 |
| T gb.08 | **FINE** | 600 | +53.0% | +23.0% | 1.56 | 91% | +17.6/+35.4 | −2.05% | 12.8% | +20,575/+9,017 |
| W gb.20 | close | 600 | +55.3% | +25.3% | 1.52 | 90% | +20.7/+34.6 | −2.05% | 14.8% | +20,971/+9,413 |
| W gb.20 | **FINE** | 600 | +35.1% | +5.1% | 1.37 | 91% | +10.9/+24.2 | −2.05% | 12.7% | +13,461/+1,903 |

### GoldDonH1 — arm 0.50, lc 2.0, cap 12 (H1)
| leg | grain | n | net1x | net2x | PF | WR | WF h1/h2 | worst | maxDD | usd 1×/2× |
|---|---|---|---|---|---|---|---|---|---|---|
| T gb.08 | close | 267 | +38.5% | +25.1% | 1.55 | 60% | +21.6/+16.9 | −2.05% | 7.0% | +14,361/+9,268 |
| T gb.08 | **FINE** | 267 | +28.8% | +15.5% | 1.42 | 62% | +17.3/+11.5 | −2.05% | 8.5% | +10,425/+5,332 |
| W gb.20 | close | 267 | +29.2% | +15.9% | 1.42 | 60% | +18.7/+10.5 | −2.05% | 7.6% | +10,590/+5,497 |
| W gb.20 | **FINE** | 267 | +22.4% | +9.0% | 1.33 | 62% | +16.3/+6.1 | −2.05% | 9.2% | +7,711/+2,619 |

**Close→fine shrink:** H1 books give back 16–37% of close-grade net at M30 truth (trail
whipsaw on sub-bars) but every leg stays gate-green — the honest planning numbers are the
FINE rows. This is the same collapse *direction* that killed USTEC_4h_ZMR, at survivable
magnitude.

## Drawdown-cancel (lc) verdict — per book (the expected "near-inert" finding does NOT hold at be=0.10)

At be=0.10 the leg enters barely in profit, so the pre-arm LOSS_CUT **fires for real**
(it is what bounds every worst clip at −lc−cost). The near-inert regime only appears at
be=1.0 (survivor-cell finding, `engine_init.hpp:1669`). Per book (fine-grain T-leg net at
lc 1.0/1.5/2.0):
- **MgcTf1h:** +30.0/+37.5/+40.8 — lc ACTIVE, monotone looser-is-better; **lc=2.0**.
- **GoldKeltM30:** +70.4/+85.2/+97.1 — lc ACTIVE, same shape; **lc=2.0**.
- **GoldTfBw1040:** +65.5/+56.0/+56.9 — lc ACTIVE, tighter is better here (fade-prone
  pullback parents); **lc=1.0**.
- **GoldTfBw20100:** +46.7/+41.5/+53.0 — weakly active, non-monotone (±12%); **lc=2.0**.
- **GoldDonH1:** +21.1/+26.3/+28.8 — lc ACTIVE, looser better; **lc=2.0**.
Every book carries the lc backstop (free cut — mimic never touches the real trade);
worst clip is exactly −(lc+0.05)% ≈ −$820 (lc2) / −$420 (lc1) per $40k clip.

## ADVERSE-PROTECTION verdict lines (paste-ready for engine_init comments)

```
// MgcTf1hMimic     be0.10/arm0.5/lc2.0/cap24/pend12 legs T gb8 + W gb20 (H1 feed).
//   Validated S-2026-07-14bb (gold_newengine_mimic_bt, REAL engine entries, fine-grain M30):
//   T +40.8%/leg PF1.78, W +35.4% PF1.68, WF +25.2/+15.6, 2x-cost +29.1/+23.7, worst -2.05%,
//   DD 12.7%, n=234. DRAWDOWN-CANCEL: lc=2 ACTIVE at be0.10 (bounds worst clip); lc1 costs 26% net.
// GoldKeltM30Mimic be0.10/arm0.25/lc2.0/cap96/pend12 legs T gb8 + W gb20 (M30 feed).
//   Validated S-2026-07-14bb: T +97.1%/leg PF1.55, W +69.0% PF1.39, WF +35.0/+62.0,
//   2x-cost +56.0/+27.9, worst -2.05%, DD 8.5%, n=822 (M30 = finest data grain; M1 check owed
//   pre-LIVE). DRAWDOWN-CANCEL: lc=2 ACTIVE (lc1 costs 27% net) -- keep 2.0.
// GoldTfBw1040Mimic be0.10/arm0.15/lc1.0/cap48/pend12 legs T gb8 + W gb20 (H1 feed).
//   Validated S-2026-07-14bb (fine-grain M30): T +65.5%/leg PF1.64, W +47.3% PF1.46,
//   WF +19.6/+45.9, 2x-cost +30.9/+12.7, worst -1.05%, DD 6.3%, n=692.
//   DRAWDOWN-CANCEL: lc=1 ACTIVE and BEST here (fade-prone parents; lc2 -13% net).
// GoldTfBw20100Mimic be0.10/arm0.15/lc2.0/cap48/pend12 legs T gb8 + W gb20 (H1 feed).
//   Validated S-2026-07-14bb (fine-grain M30): T +53.0%/leg PF1.56, W +35.1% PF1.37,
//   WF +17.6/+35.4, 2x-cost +23.0/+5.1 (W thin at 2x -- honest), worst -2.05%, DD 12.8%, n=600.
//   DRAWDOWN-CANCEL: lc=2 weakly active, non-monotone; kept as the free backstop.
// GoldDonH1Mimic   be0.10/arm0.5/lc2.0/cap12/pend12 legs T gb8 + W gb20 (H1 feed).
//   Validated S-2026-07-14bb (fine-grain M30): T +28.8%/leg PF1.42, W +22.4% PF1.33,
//   WF +17.3/+11.5, 2x-cost +15.5/+9.0, worst -2.05%, DD 9.2%, n=267.
//   DRAWDOWN-CANCEL: lc=2 ACTIVE, looser-is-better within sweep -- keep 2.0.
```

## Honest caveats

1. **Fine grain = M30, not M1/tick.** The certified data's finest bar is M30. Level fills
   (BE entry, trail, lc) are still level-booked; the live resting-exec path books actual
   crossing prices. An M1/tick re-check is owed before any LIVE flip/sizing (standing
   mimic caveat; XAU_4h_DonchN20 precedent).
2. **GoldTfBw20100 W leg at 2× fine = +5.1% ($1.9k/22.5mo)** — passes but thin; the T leg
   carries that book at stress cost.
3. WF halves gated at 1× cost (both halves also positive at 2× on every T leg; W legs not
   re-gated at 2× per half).
4. Parent overlap: all 5 books trigger off the SAME MGC tape — mimic clips will correlate
   across books (and with the wired MgcTF4h/2h mimics). Book-level capital allocation is
   the operator's call (companion rule: standalone risk/return presented, operator sizes).
5. MgcTf1h entry stream: trigger px = signal-bar close + 0.10 spread (the engine's ask
   fill); pyramid adds do NOT re-trigger legs (one parent open per closed record's
   original entryTs).
6. be=1.0 fallback (all 5 pass, PF 2.0–4.6, DD ≤ 4%): use if the operator prefers the
   survivor-cell-style conservative profile over net.

## Repro

```
c++ -O2 -std=c++17 backtest/gold_shorttf_bothways_bt.cpp -o /tmp/shorttf_bt
DUMP_ENTRIES=1 /tmp/shorttf_bt backtest/data/mgc_30m_spliced_2024_2026.csv | grep '^ENTRY|' ...
c++ -O2 -std=c++17 -Iinclude backtest/mgc_tf1h_port_bt.cpp -o /tmp/tf1h_bt
VT=0 LC=0 DUMP_ENTRIES=1 /tmp/tf1h_bt <h1-from-splice.csv>   # H1 = 2xM30 aggregation
c++ -O2 -std=c++17 backtest/gold_newengine_mimic_bt.cpp -o /tmp/mimic_bt
/tmp/mimic_bt <entries.csv> backtest/data/mgc_30m_spliced_2024_2026.csv <h1|m30>  [FINE=1]
```
Entry files: rows `dir,entry_px,entry_ts`. Sweep lists overridable via
BE_LIST/ARM_LIST/GB_LIST/LC_LIST/CAP_LIST/PEND_LIST; COSTBP env (default 5).

Vault: `Memory-Omega/wiki/entities/GoldNewEngineMimics.md`. 2026-07-14.

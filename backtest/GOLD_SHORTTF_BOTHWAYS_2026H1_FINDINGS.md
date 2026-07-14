# Gold short-TF BOTH-WAYS study — 2026-01-14 → 2026-07-14 at MGC cost (S-2026-07-14ax)

**Question (operator):** with the certified IBKR MGC cost basis (~0.41pt RT vs spot ~1.4pt),
what quicker-turnover gold mechanism is viable LONG **and** SHORT over the very volatile
last 6 months? Backtest + report only — nothing wired, nothing deployed.

## Verdict (one paragraph)

**YES — symmetric M30/H1 breakout-trend mechanisms clear the full gate both-ways on this
window.** Best single candidate: **KELT M30 Keltner k1.25 close-through breakout, 2×ATR(20)
initial stop, 2.5×ATR trail, 96-bar time stop** — n=240 (9.2 trades/wk), **+$12,225 @1× /
+$11,241 @2× cost per 1 MGC**, PF 1.33, maxDD $5,458 (best DD-adjusted of every config),
long +$4,415 / short +$7,811, WF halves +$6,911/+$5,314, full-22mo context +$17,330 PF 1.20.
The **short side carries this half-year** (the Feb–Jun ~1,780pt bear leg); the long side still
made money against a net −760pt tape (Jan melt-up capture) — genuine timing, not beta.
Honest caveat: over the FULL 22-month history the short legs are thin everywhere (best PF
1.09–1.12) — the short edge is a property of this volatile/bear half-year; the symmetric
wiring is what lets the book capture whichever side the regime pays.

## Cost model (certified)

- MGC micro-future, 10oz, $10 per 1.0pt per contract.
- RT = $2.08 commission (0.208pt) + 0.10pt spread + 0.10pt slippage = **0.41pt RT ($4.10)**.
- 2× stress = 0.82pt ($8.20). Every table below reports both. Source:
  `include/GoldExecSpreadBasis.hpp` + GoldPhase1CostVenue vault page; matches the
  registry §7 MGC=1 basis.

## Data + integrity (HARD GATE respected)

| file | span | verdict |
|---|---|---|
| /Users/jo/Tick/mgc_30m_hist.csv | 2024-06-03..2026-06-03, 23,616 rows | CERTIFIED CLEAN |
| /Users/jo/Omega/data/mgc_30m_hist.csv | 2026-05-13..2026-07-13, 1,957 rows (nightly refresh) | CERTIFIED CLEAN |
| **backtest/data/mgc_30m_spliced_2024_2026.csv** (this study) | 2024-06-03..2026-07-13, 24,907 rows | **CERTIFIED CLEAN** (gated 2026-07-14) |

Splice by `backtest/mgc_30m_splice.py` (newer file wins on the 666-bar overlap; close diff
median 0.3pt, max 4.0pt ≈ 0.09% — two IBKR CONTFUT pulls). All >6h gaps = weekends. H1
built by aggregating 2×M30. Real-MGC bars for both signal AND fill — no spot hybrid needed.

**Window path:** 4,766 → **5,758 (Jan-29 high)** → **3,975 (Jun-30 low)** → 4,006. Net
−760pt with a +1,000pt rally and a −1,780pt decline inside it. Both-legs-positive results
on this tape are timing, not drift capture.

## Harness

`backtest/gold_shorttf_bothways_bt.cpp` (self-contained, plain c++17). Fidelity: signals on
bar close, managed from next bar; intrabar **adverse-first** (stop before target);
**gap-honest fills** (bar opening beyond a stop fills at the open, not the stop level —
added mid-study; moved results <5%, robustness sign); fixed COST_RT deducted per trade;
env `COST_RT` override. Windows: 6MO = 2026-01-14..2026-07-14, WF split at 2026-04-14,
FULL = 2024-09-01.. (3mo warmup discarded). **Parity check:** independent Python
re-implementation of the winning config reproduced it EXACTLY (n=240, $+12,225.0, PF 1.33,
identical leg splits).

Tombstone check (second-brain, 4 query variants): no tombstone on any mechanism built here.
Keltner-M30 was falsified 2026-06-20 **at spot cost 1.60pt RT** — the 0.41pt futures basis
is the sanctioned re-open. Operator hands-off list (ORB family, scalps, GoldEmaCrossM30,
GoldSessionTrendPullback, XauStraddleM30) untouched. Prior art honoured: GoldDeepDive
2026-07-08 ("MGC 30m Donchian short side dead at fair cost" — confirmed below on full
history; the 6mo window is a different regime) and registry §7 (spot LOSS_CUT kills MGC —
no LC anywhere here; ATR stops are the protection).

## Results — 6MO window, $ per 1 MGC (net1x / net2x = 1× / 2× cost)

### TF1H — H1 EMA trend-follow both-ways (2×ATR stop, ATR trail, reversal exit, no LC)

| config | n (/wk) | net1x | net2x | PF | WR | L / S | WF h1/h2 | worst / maxDD | full-22mo |
|---|---|---|---|---|---|---|---|---|---|
| ema10/40 trail2.0 | 205 (7.9) | **+11,464** | +10,623 | 1.29 | 39% | +6,709 / +4,755 | +8,846/+2,617 ✓ | −1,822 / 7,430 | +19,795 PF1.23 |
| ema20/100 trail2.0 | 176 (6.8) | **+11,367** | +10,646 | 1.33 | 43% | +3,830 / +7,537 | +6,887/+4,480 ✓ | −1,822 / 7,858 | +18,474 PF1.25 |
| ema20/100 trail2.5 | 134 (5.2) | +9,478 | +8,929 | 1.29 | 40% | +3,053 / +6,425 | +5,519/+3,959 ✓ | −2,303 / 9,775 | +21,560 PF1.31 |
| ema10/40 trail2.5 | 164 (6.3) | +6,283 | +5,611 | 1.16 | 33% | +3,310 / +2,973 | ✓ | −2,303 / 8,922 | +15,359 |
| ema20/50 trail2.0 | 193 (7.4) | +5,644 | +4,853 | 1.14 | 37% | +1,342 / +4,302 | ✓ | −1,822 / 9,206 | +13,525 |
| (4 remaining cells) | | +2,381..−243 | | | | | 3✓ 1✗ | | all full-positive |

Plateau: 8/9 positive @1×, 7/9 full-gate PASS. Trail 2.0 dominates. Full-history legs of
ema10/40 t2.0: L +$17,806 PF 1.47 / S +$1,988 PF 1.04.

### KELT — M30 Keltner(EMA20, k×ATR20) close-through breakout

| config | n (/wk) | net1x | net2x | PF | WR | L / S | WF h1/h2 | worst / maxDD | full-22mo |
|---|---|---|---|---|---|---|---|---|---|
| **k1.25 trail2.5** | **240 (9.2)** | **+12,225** | **+11,241** | **1.33** | 41% | **+4,415 / +7,811** | **+6,911/+5,314 ✓** | **−1,240 / 5,458** | **+17,330 PF1.20** |
| k1.50 trail2.5 | 202 (7.8) | +8,542 | +7,714 | 1.28 | 41% | +3,860 / +4,682 | +5,332/+3,210 ✓ | −1,290 / 4,839 | +13,773 PF1.19 |
| k2.00 trail2.5 | 149 (5.7) | +5,315 | +4,704 | 1.22 | 40% | +2,721 / +2,594 | +3,657/+1,658 ✓ | −1,290 / 5,293 | +5,653 PF1.10 |
| k1.25 fixedR tp2R | 188 (7.2) | +9,392 | +8,621 | 1.21 | 35% | **−1,137** / +10,529 | ✓ | −1,240 / 8,723 | +9,772 |
| k1.25 fixedR tp3R | 160 (6.2) | +7,202 | +6,546 | 1.17 | 30% | −58 / +7,260 | ✓ | −1,240 / 7,413 | +6,653 |
| (k1.5/k2.0 fixedR, 4 cells) | | +4,161..−2,295 | | | | | 0✓ 4✗ | | |

Plateau: 8/9 positive @1×; the **trail column is 3/3 full-gate PASS with both legs
positive** — that's the mechanism's plateau. Fixed-R exits underperform and push the long
leg negative (tight targets cut the January runners). Full-history legs of the winner:
L +$13,175 PF 1.32 / S +$4,154 PF 1.09.

### DON — Donchian Nin/Nout close-through + 3×ATR hard stop

| config | n (/wk) | net1x | net2x | PF | L / S (6mo) | WF | worst/maxDD | full-22mo L / S |
|---|---|---|---|---|---|---|---|---|
| m30 40/20 | 88 (3.4) | +7,908 | +7,547 | 1.33 | +6,287 / +1,621 | ✓ | −1,859/6,946 | +20,253 PF1.83 / **−4,527 PF0.86** |
| h1 20/10 | 77 (3.0) | +7,403 | +7,088 | 1.33 | +4,683 / +2,721 | ✓ | −2,287/8,006 | +16,892 PF1.68 / +73 PF1.00 |
| m30 20/10 | 157 (6.0) | +7,409 | +6,765 | 1.21 | +533 / +6,876 | ✓ | −1,859/7,571 | +13,643 / +4,675 |
| h1 55/27 | 33 (1.3) | +6,648 | +6,513 | 1.44 | **−1,578** / +8,226 | ✓ | −1,863/5,694 | +8,169 / +5,204 |
| h1 40/20 | 43 (1.7) | +6,083 | +5,906 | 1.33 | **−2,729** / +8,812 | ✓ | −1,510/8,250 | — |
| m30 55/27 | 64 (2.5) | +4,660 | +4,398 | 1.25 | +1,137 / +3,524 | ✓ | −1,859/4,673 | — |

Plateau 6/6 positive, but h1 40/20 and h1 55/27 have a NEGATIVE 6mo long leg (short-carried
— fails the both-legs gate) and m30 40/20's full-history short is decisively negative
(PF 0.86 — exactly the GoldDeepDive 2026-07-08 verdict). Best both-legs DON: **h1 20/10**,
but at 3.0/wk it's half the turnover of KELT/TF1H for similar net.

### VXF / VXB — H1 vol-expansion fade / continuation: **NOT VIABLE**

VXF (fade): 4/6 positive @1× but **0/9 WF PASS** — the fade edge existed Jan–May and died
in June (Jun −$2,165 on the best cell; H2 half negative on every cell). VXB (continuation):
2/3 negative; the lone green (k3.0, +$1,735) is n=15 in 6mo (misses the turnover point) AND
full-history negative (−$3,373 PF 0.66) = window fluke. Both mechanisms honestly dead at
this cost basis; do not pursue.

## Monthly breakdown (winner + runners-up, 6mo, $ per 1 MGC)

| config | Jan14+ | Feb | Mar | Apr | May | Jun | Jul1–14 |
|---|---|---|---|---|---|---|---|
| KELT k1.25 trail2.5 | +7,546 | −1,731 | +2,589 | −2,341 | +972 | +4,426 | +764 |
| TF1H ema10/40 t2.0 | +10,804 | −1,154 | +1,834 | −3,128 | +848 | +2,118 | +141 |
| TF1H ema20/100 t2.0 | +9,230 | −4,557 | +2,690 | +586 | +1,224 | +1,932 | +262 |
| DON m30 40/20 | +2,208 | +2,485 | +3,647 | −4,094 | −1,041 | +3,949 | +753 |

Shape is honest trend-following: big Jan (melt-up) and Jun (bear acceleration) months,
gives back in the Feb/Apr regime turns. No config avoids the turn months.

## Adverse-protection verdicts (mandate: backtested statement per candidate)

- **KELT k1.25 trail2.5 (the candidate):** hard 2×ATR(20) initial stop + 2.5×ATR trail +
  96-bar (2-day) time stop, gap-honest fills. Backtested worst trade 6mo **−$1,240 per
  1 MGC (124pt — an April-crash expansion bar)**, maxDD $5,458. Tightening exits HURTS: the fixed-R (tp2R/tp3R) variants of the same entry lose
  the long leg entirely. Verdict: ATR-stop+trail is the protection; no LC — consistent
  with registry §7 (spot LC=0.5% kills MGC intrabar).
- **TF1H:** 2×ATR stop + 2.0×ATR trail + reversal exit. Worst −$1,822 / DD $7,430.
  Wider trails (3.0) degrade net AND DD — do not widen. No LC by design (registry §7 trap).
- **DON:** 3×ATR adverse-first hard stop (MgcFastDonchian-family verdict). Worst −$2,287
  (h1 20/10) / DD $8,006.
- Loss driver everywhere is regime-turn clusters (Feb, Apr), not single-trade blowups —
  worst single trades are 1.5–2.3× the average winner, stops held through the wildest
  window in the data (gap-honest fills verified).

## Gate summary

VIABLE both-ways (net>0 @2×, both WF halves +, n≥25, both legs positive, protection stated):
**KELT k1.25/k1.5/k2.0 trail2.5; TF1H ema10/40 t2.0, ema20/100 t2.0/t2.5, ema20/50 t2.0/t2.5,
ema10/40 t2.5/t3.0; DON m30 40/20, m30 20/10, h1 20/10, m30 55/27.** Short-carried (long leg
negative — report-only): DON h1 40/20, h1 55/27; KELT fixedR row. Dead: VXF, VXB, KELT
k1.5/k2.0 fixedR.

**Single best candidate: KELT M30 k1.25 trail2.5** — highest turnover (9.2/wk vs ~1–2/wk
for the live 4h/2h stack), best maxDD, both legs positive, 3-config plateau all-PASS,
survives 2× cost with 92% of net intact, +$17.3k PF 1.20 over the full 22 months.

## Caveats (read before any wire decision)

1. **The short edge is regime-specific.** Full-history short legs: KELT +$4.2k PF 1.09,
   TF1H +$2.0–3.8k PF 1.04–1.10, DON m30 40/20 **negative**. This half-year paid shorts
   because of the Feb–Jun bear leg. A symmetric engine is the correct structure (it holds
   whichever side pays) but do NOT project the 6mo short-side run-rate forward.
2. Results are bar-grade (M30 real-MGC bars), not tick-grade; the 0.10pt slippage allowance
   inside COST_RT is the intrabar-fill buffer. Fill convention = stop level (gap-honest).
3. Single contract, no compounding, no session filter, no overlap netting with the live
   MgcTF4h/2h engines — a live wire would need overlap accounting vs the existing MGC stack
   (same instrument, correlated entries) + registry §7 feed-path parity + warm-seed + the
   canary gates. None of that is done here (backtest-only per task).
4. Splice seam (2026-05-13..06-03): two IBKR pulls, ≤0.09% close disagreement, newer wins;
   May results are modest for every config — no seam-driven green.

## Files

- Harness: `backtest/gold_shorttf_bothways_bt.cpp` (build line in header)
- Splice: `backtest/mgc_30m_splice.py` → `backtest/data/mgc_30m_spliced_2024_2026.csv` (CERTIFIED)
- Raw run output: scratchpad `run_final.txt` (tables above are the full content)
- Vault: `Memory-Omega/wiki/entities/GoldShortTfBothWays2026H1.md`

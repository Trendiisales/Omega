# Gold SUB-30m TF study (1m/5m/10m/15m) — 2026-01-14 → 2026-07-14 at MGC cost (S-2026-07-14ba)

**Operator ask:** "i also still want a shorter timeframe gold engine check 1m, 5m, 10 min,
15 min and show me results use same last 6 month data". Extends the S-2026-07-14ax study
(`GOLD_SHORTTF_BOTHWAYS_2026H1_FINDINGS.md`, commit 55b16e4a): same mechanisms, same
param grids, same MGC cost model, same gate discipline — pushed DOWN to sub-30m bars.
**Backtest + report only — nothing wired, nothing deployed.**

## Verdict (one paragraph)

**Below 15m the edge does not exist — and it is NOT a cost story at 1m.** At 1m every one
of 18 configs loses heavily (−$9.4k…−$58k @1×, PF 0.81–0.94, both legs negative); even at
ZERO cost the gross PnL is still negative (e.g. best-family KELT k1.0 t2.0: gross −$7.8k
before the $48k of RT costs) — the mechanisms have no signal at that grain, friction just
doubles the bleed. 5m is dead (best rows PF ≤ 1.14, and the best cell TF ema10/40 t2.5
+$16.1k is FULL-history NEGATIVE −$5.0k = window fluke). 10m is the transition zone —
13/18 configs positive @1×, best DON 40/20 +$9.6k PF 1.21 — but **zero rows clear PF≥1.3**.
At 15m the coarse grid shows ONE full gate pass (DON 55/27), and the mandated fine
neighbourhood sweep proves it is a **genuine plateau, not a fluke: 25 of 30 cells in
Nin 45–70 × Nout 20–35 are GATE-PASS+LEGS** (every fail is in the fast-exit Nout=20
column, WF-H2 marginal). Plateau peak: **15m DON 55/35 — n=126 (4.8/wk), +$19,019 @1× /
+$18,502 @2× per 1 MGC, PF 1.83, maxDD $4,450, worst −$810, L +$5,513 / S +$13,506,
WF +$15,350/+$3,669, FULL 25mo +$20,330 PF 1.30** (neighbour 60/35 nearly identical).
The originally-sampled 55/27 (+$15,729, PF 1.60, DD $4,749) sits inside the plateau.
This family beats the ax M30 KELT winner on net, PF AND drawdown at half the turnover.
**Bottom line: 1m/5m structurally unviable at any cost (no signal, not a friction
problem); 10m PF-capped at 1.22 (no pass); 15m slow-exit Donchian is a certified-plateau,
report-only candidate alongside the ax M30/H1 winners.**

## Cost model (identical to ax, certified)

- MGC micro-future, 10oz, $10 per 1.0pt per contract.
- RT = $2.08 commission (0.208pt) + 0.10pt spread + 0.10pt slippage = **0.41pt RT ($4.10)**.
- 2× stress = 0.82pt ($8.20). Every row reports both. `cost%` = total 1× RT cost /
  gross winning $ before costs (how much of the raw win-side friction eats).

## Data — honest substitution, integrity-gated

**No MGC bars finer than 30m exist anywhere in the corpus** (exhaustive /Users/jo/Tick
search; VPS L2 XAU logs prune to 3 days; bulk IBKR pulls via the production gateway are
forbidden). Signals AND fills therefore use **spot XAUUSD 1m bars** with the **MGC cost
basis** applied — stated substitution. Note: the MGC CONTFUT series prints ~110–130pt above
spot in this window (back-adjusted continuous-contract offset, ≈2.7% of level); point-PnL
of ATR-scaled mechanisms is comparable within ~3%, cost basis unchanged.

| input | span (UTC) | verdict |
|---|---|---|
| /Users/jo/Tick/xau_m1_2024_2026.csv (true-UTC 1m, duka tick corpus) | 2024-03-01..2026-04-24 | CERTIFIED CLEAN |
| /Users/jo/Tick/xau_h2026mar_jun_m1.csv (histdata T202603-06) | 2026-03-01..2026-06-26 | CERTIFIED CLEAN |
| backtest/data/xau_1m_duka_tail_2026.csv (duka 1m candles) | 2026-06-28..2026-07-13 | CERTIFIED CLEAN |
| **/Users/jo/Tick/xau_1m_spliced_2024_2026.csv** (this study, 837,302 bars) | 2024-03-01..2026-07-13 | **CERTIFIED CLEAN** (gated 2026-07-14) |

Splice: `backtest/xau_1m_splice_2026h1.py`. Two provenance findings verified empirically:
1. **The histdata-derived file's clock is Europe/London local stored as epoch** — NOT the
   documented duka_ticks_to_h1_fill EST convention (that shift gave 43pt RMSE). Close-diff
   RMSE vs the true-UTC file on their 8-week overlap = **0.0000** at shift 0 before the EU
   DST switch (2026-03-29) and at −3600 after it. Converted piecewise, re-verified per
   segment (27,536 + 26,157 overlap bars, RMSE 0.0000 both).
2. **Tail = dukascopy daily 1m-candle files** (BID/ASK_candles_min_1.bi5, mid of both),
   fetched by `backtest/fetch_xau_candles_daily.py` after the tick endpoint spent the
   evening in a degraded state (74s/request waves, confirmed server-side from two
   networks). Candle decode verified EXACT against the histdata overlap on Jun-25/26
   (close absdiff median 0.0, max 0.0005 = rounding). Jul-14 candles were not yet
   published at fetch time, so the tape ends **2026-07-13 24:00 UTC** — same effective end
   as the ax study's MGC data (07-13).

Tombstone pre-mine (second-brain, operator-ordered): **no tombstone** on sub-30m gold
TF/breakout mechanisms (top hits: DataCertificationGate + unrelated D1 engines). The
2026-06-20 Keltner-M30 spot-cost falsification was already legitimately re-opened at MGC
cost by ax. ORB-family/scalp hands-off list untouched.

## Harness

`backtest/gold_subh30_tf_bt.cpp` (self-contained c++17; build line in header). Loads the
1m splice, resamples to 5m/10m/15m internally. Fidelity identical to ax: signal on bar
close, managed from next bar, intrabar adverse-first, gap-honest fills (bar opening beyond
the stop fills at the open), fixed COST_RT per RT, env `COST_RT` override. The O(N)
monotonic-deque Donchian was verified bar-exact vs the ax O(n·N) implementation (5 window
sizes on the MGC 30m file, 0 mismatches). Windows: 6MO = 2026-01-14..2026-07-15, WF split
2026-04-14, FULL = 2024-06-01.. (3mo warmup discarded) for context.

Mechanisms (ax families, symmetric long/short, **no LOSS_CUT anywhere** — registry §7:
spot LC kills MGC intrabar; ATR stops are the protection):
- **KELT** k∈{1.0,1.25,1.5} × trail∈{2.0,2.5,3.0}: Keltner(EMA20, k×ATR20) close-through,
  2×ATR stop, trail×ATR trail, 2-trading-day time stop scaled per TF (2880/576/288/192 bars).
- **DON** (20/10),(40/20),(55/27) close-through, 3×ATR hard stop, opposite-channel exit.
- **TF** dual-EMA (10/40),(20/100),(10/20) × trail∈{2.0,2.5}, 0.5×ATR impulse, 2×ATR stop,
  reversal exit.

Gate (ax discipline): net>0 @1× AND @2×, PF≥1.3, both WF halves positive → GATE-PASS;
+LEGS = both legs also positive.

## Results — 6MO window, $ per 1 MGC

### 1m — ALL 18 CONFIGS FAIL, and not because of cost

| family | n range (/wk) | net1x range | PF range | legs | cost% |
|---|---|---|---|---|---|
| KELT (9 cells) | 6,420–11,752 (247–452) | **−$33.4k…−$56.0k** | 0.81–0.86 | both neg, all cells | 13.5–18.6 |
| DON (3 cells) | 2,520–5,601 (97–215) | **−$9.4k…−$36.0k** | 0.84–0.94 | both neg | 8.0–11.7 |
| TF (6 cells) | 9,120–14,774 (351–568) | **−$34.8k…−$57.9k** | 0.84–0.87 | both neg | 15.1–18.7 |

Decisive point: best-family cell KELT k1.0 t2.0 pays $48.2k in 1× costs on n=11,752 but is
**gross-negative before any cost (−$7.8k)**. FULL 25mo: every cell −$41k…−$250k. 1m gold
trend/breakout is structurally dead — no cost basis rescues it. WF: 36/36 half-windows
negative.

### 5m — DEAD (no gate pass; the green cells are window flukes)

| config (best of family) | n (/wk) | net1x | net2x | PF | L / S | WF h1/h2 | FULL 25mo |
|---|---|---|---|---|---|---|---|
| TF ema10/40 trail2.5 | 2,028 (78) | +16,114 | +7,799 | 1.14 | +4,064 / +12,050 | +14,275/+1,839 | **−4,982 PF 0.98** |
| TF ema20/100 trail2.5 | 1,717 (66) | +13,813 | +6,773 | 1.13 | +2,754 / +11,059 | +11,103/+2,710 | +2,671 PF 1.01 |
| DON 55/27 | 490 (19) | +6,348 | +4,339 | 1.11 | −181 / +6,530 | +6,086/+263 | +7,694 PF 1.06 |
| KELT (all 9) | 1,167–2,186 | **−$1.3k…−$7.1k** | all neg | 0.92–0.99 | long leg neg 8/9 | — | all neg |

All 18 cells PF ≤ 1.14 → 0 gate passes. The positive TF rows are carried by the 6mo short
leg and WF-H1; over the FULL history the best cell is negative — exactly the "window
fluke" signature the ax gate exists to catch. 2× cost removes 52–81% of the best nets.

### 10m — transition zone: 13/18 positive @1×, ZERO gate passes (PF ceiling 1.22)

| config (top rows) | n (/wk) | net1x | net2x | PF | L / S | WF h1/h2 | FULL 25mo |
|---|---|---|---|---|---|---|---|
| TF ema20/100 trail2.0 | 1,103 (42) | +12,295 | +7,772 | 1.16 | +685 / +11,609 | +11,466/+829 | +11,948 PF 1.07 |
| TF ema10/20 trail2.0 | 1,422 (55) | +11,082 | +5,252 | 1.11 | +1,439 / +9,644 | +14,401/**−3,319** | +3,595 |
| TF ema10/40 trail2.0 | 1,311 (50) | +10,338 | +4,963 | 1.11 | +476 / +9,863 | +10,800/**−462** | +7,350 |
| DON 40/20 | 308 (12) | +9,577 | +8,314 | 1.21 | +2,636 / +6,942 | +8,163/+1,414 | +16,618 PF 1.16 |
| DON 55/27 | 234 (9.0) | +8,170 | +7,210 | 1.22 | +2,668 / +5,501 | +5,124/+3,046 | +11,436 PF 1.13 |
| KELT best (k1.25 t2.5) | 776 (30) | +3,295 | +113 | 1.05 | −714 / +4,009 | +3,200/+95 | −4,230 |

DON is the only family that keeps both WF halves positive AND is FULL-history positive —
but PF 1.21–1.22 misses the 1.3 bar. KELT never recovers its long leg at this grain.

### 15m — ONE full gate pass; the rest positive but sub-gate

| config | n (/wk) | net1x | net2x | PF | WR | cost% | worst / maxDD | L / S | WF h1/h2 | gate | FULL 25mo |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **DON 55/27 stop3ATR** | **140 (5.4)** | **+15,729** | **+15,155** | **1.60** | 39.3% | 1.4 | −1,066 / 4,749 | **+6,803 / +8,927** | **+11,979/+3,750** | **GATE-PASS+LEGS** | **+19,590 PF 1.28** |
| DON 20/10 | 354 (13.6) | +10,458 | +9,006 | 1.21 | 37.6% | 2.4 | −986 / 5,077 | +1,781 / +8,677 | +8,186/+2,272 | PF fail | +9,059 PF 1.07 |
| DON 40/20 | 192 (7.4) | +7,283 | +6,496 | 1.21 | 38.5% | 1.9 | −1,341 / 5,519 | +2,183 / +5,100 | +6,283/+1,000 | PF fail | +9,133 PF 1.11 |
| TF ema10/40 trail2.0 | 872 (33.5) | +13,466 | +9,890 | 1.18 | 35.3% | 3.9 | −1,162 / 5,383 | +3,537 / +9,928 | +10,433/+3,033 | PF fail | +10,724 PF 1.06 |
| TF ema20/100 trail2.0 | 740 (28.5) | +11,517 | +8,483 | 1.17 | 35.3% | 3.8 | −1,167 / 5,742 | +3,985 / +7,532 | +9,501/+2,016 | PF fail | +9,396 PF 1.06 |
| KELT k1.00 trail3.0 | 505 (19.4) | +9,698 | +7,627 | 1.15 | 34.3% | 2.8 | −988 / 7,926 | +6,342 / +3,355 | +6,022/+3,676 | PF fail | +15,213 PF 1.11 |
| KELT k1.25 trail3.0 | 457 (17.6) | +7,183 | +5,309 | 1.12 | 34.4% | 2.9 | −1,221 / 5,550 | +5,612 / +1,571 | +5,408/+1,775 | PF fail | +11,565 PF 1.09 |
| (11 remaining cells) | | −130…+10,233 | | 1.00–1.12 | | | | | 3 WF-H2 neg | all fail | |

15/18 cells positive @1× at 15m — the mechanisms come alive here — but in the coarse grid
the ax PF≥1.3 bar is cleared by exactly one cell. Hence the fine sweep below.

### 15m DON fine neighbourhood sweep (Nin 45–70 × Nout 20–35, `DON15_SWEEP=1`)

**25 / 30 cells GATE-PASS+LEGS** — a broad plateau. All five fails are Nout=20 (fast
channel exit) cells whose WF-H2 is marginally negative (−$42…−$1,186); every Nout≥23
column passes end-to-end. The plateau strengthens toward SLOW exits (Nout 31–35):

| cell | n (/wk) | net1x | net2x | PF | worst / maxDD | L / S | WF h1/h2 | FULL 25mo |
|---|---|---|---|---|---|---|---|---|
| **55/35 (peak)** | 126 (4.8) | **+19,019** | **+18,502** | **1.83** | **−810 / 4,450** | +5,513 / +13,506 | +15,350/+3,669 | +20,330 **PF 1.30** |
| 60/35 | 123 (4.7) | +18,808 | +18,304 | 1.84 | −810 / 4,268 | +5,573 / +13,235 | +15,478/+3,330 | +21,326 PF 1.32 |
| 50/35 | 134 (5.2) | +16,654 | +16,105 | 1.63 | −1,344 / 6,264 | +4,116 / +12,538 | +13,547/+3,107 | +17,064 PF 1.23 |
| 55/31 | 131 (5.0) | +16,427 | +15,890 | 1.67 | −1,066 / 3,403 | +6,955 / +9,472 | +13,231/+3,196 | +18,758 PF 1.27 |
| 55/27 (coarse-grid cell) | 140 (5.4) | +15,729 | +15,155 | 1.60 | −1,066 / 4,749 | +6,803 / +8,927 | +11,979/+3,750 | +19,590 PF 1.28 |
| 55/23 | 144 (5.5) | +15,027 | +14,437 | 1.61 | −1,066 / 3,789 | +7,341 / +7,686 | +10,851/+4,176 | +19,206 PF 1.28 |
| plateau spread (25 passes) | 120–170 | +10,381…+19,019 | | 1.32–1.84 | | all legs + | all WF + | +$13.9k…+$21.3k |
| fails (5: all Nout=20) | 141–180 | +7,222…+10,788 | | 1.22–1.42 | | | WF-H2 −42…−1,186 | |

Reading: at 15m the ENTRY channel width barely matters (45–70 equivalent); the edge
lives in the SLOW exit — wider out-channels keep the winners through 15m noise. 2× cost
removes only 3–5% of net anywhere on the plateau (cost% 1.2–1.7).

## Cost-share analysis (does the grain kill us via friction?)

| TF | typical cost% of gross wins | 2× cost survival of best cell | verdict |
|---|---|---|---|
| 1m | 8–19% | n/a (all negative) | edge ABSENT pre-cost; cost doubles the bleed |
| 5m | 5.3–7.8% | 48% of net survives | thin PF, best cell FULL-neg → fluke |
| 10m | 2.0–5.2% | 87% (DON 40/20) | real but PF-capped at 1.22 |
| 15m | 1.3–4.1% | **96% (DON 55/27)** | viable at the top cell |
| 30m (ax ref) | ~2 (KELT winner) | 92% | certified plateau |

Friction scales exactly as expected (cost% roughly halves per TF doubling), but the
binding constraint below 15m is the SIGNAL, not the cost: gross-pre-cost is already
negative at 1m and only marginal at 5m.

## Comparison vs the ax M30/H1 winners (same window, same gate)

| candidate | n (/wk) | net1x | net2x | PF | maxDD | legs | plateau |
|---|---|---|---|---|---|---|---|
| KELT M30 k1.25 t2.5 (ax winner) | 240 (9.2) | +12,225 | +11,241 | 1.33 | 5,458 | +4,415/+7,811 | 3/3 trail cells PASS |
| TF1H ema10/40 t2.0 (ax) | 205 (7.9) | +11,464 | +10,623 | 1.29 | 7,430 | +6,709/+4,755 | 7/9 PASS |
| **15m DON 55/35 (this study)** | 126 (4.8) | **+19,019** | **+18,502** | **1.83** | **4,450** | +5,513/+13,506 | **25/30 fine-sweep PASS** |

The 15m slow-exit Donchian family beats both ax winners on net, PF and drawdown, at half
the turnover, on a broader verified plateau. A 55-bar 15m channel is a ~13.8h lookback —
structurally the same "half-day-to-day breakout" idea as the M30 DON, sampled finer, with
the slow out-channel exit (31–35 bars ≈ 8h) doing the extra work of riding through 15m
noise.

## Adverse-protection verdicts (mandate: backtested statement per candidate)

- **15m DON plateau (25 passing cells, peak 55/35):** 3×ATR(14) adverse-first hard stop,
  opposite slow-channel close-through exit, gap-honest fills. Backtested 6mo worst trade
  **−$810 per 1 MGC at the peak cell** (plateau range −$810…−$1,520), maxDD $3,083–$7,983
  (peak cell $4,450). No LC — registry §7 (spot LC kills MGC); the 3×ATR stop is the
  protection, verified through the Feb/Apr regime-turn clusters on the wildest tape in
  the dataset.
- **10m/15m DON runners-up:** same 3×ATR protection; worst −$986…−$1,341.
- **15m TF cells:** 2×ATR stop + trail + reversal exit; worst −$1,162…−$1,167.
- 1m/5m: all fail regardless — no protection tuning changes a gross-negative mechanism.

## Caveats (read before any wire discussion)

1. **Spot-bars substitution.** Signals and fills are spot XAUUSD 1m aggregates at MGC
   cost. Bar-grade, not tick-grade; the 0.10pt slippage inside COST_RT is the intrabar
   buffer. A live 15m MGC engine would need MGC-feed parity checks (registry §7) before
   any trust transfer.
2. **Plateau verified, but coarse-grid neighbours differ.** The mandated fine sweep
   (Nin 45–70 × Nout 20–35) passes 25/30 cells, so the 15m DON edge is NOT a single-cell
   fluke — but note the coarse-grid cells 20/10 and 40/20 (different exit RATIO, 2:1 fast
   exits) sit at PF 1.21: the plateau requires the SLOW exit (Nout ≥ 23, best 31–35).
   Any wire must come from the verified plateau region, not the 2:1-ratio family.
3. **Short leg is regime-carried** here exactly as in ax: 6mo shorts +$8.9k vs FULL-history
   modest. Symmetric wiring is the correct structure; do not project the short-side
   run-rate forward.
4. **Overlap with the live MGC stack** (MgcTF4h/2h + newly wired M30/H1 engines, same
   instrument) is not netted — same caveat as ax §3.
5. Window end is 2026-07-13 24:00 UTC (duka Jul-14 candles unpublished at fetch time) —
   identical effective end to the ax data.

## Files

- Harness: `backtest/gold_subh30_tf_bt.cpp`
- Splice: `backtest/xau_1m_splice_2026h1.py` → `/Users/jo/Tick/xau_1m_spliced_2024_2026.csv`
  (25MB, corpus-resident; fully reproducible from the two Tick inputs + committed tail)
- Tail fetcher: `backtest/fetch_xau_candles_daily.py` → `backtest/data/xau_1m_duka_tail_2026.csv` (committed)
- Vault: `Memory-Omega/wiki/entities/GoldSub30mStudy2026H1.md`

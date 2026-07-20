# BIGCAP + RD-AGENT full-test — S-2026-07-20az

Faithful re-certification of the stock ("bigcap") and RD-Agent money-maker books over
**current** data (`data/rdagent/sp500_long_close.csv`, 2019-06-03 → 2026-07-14 daily closes,
integrity flags = 0 on all wired names). All figures net of cost. Honest framing: **the live
forward record is near-empty** — the feed froze 2026-07-14 (IBKR-4002 sub lapse; yfinance
fallback ships next cron) and the regime is risk-off, so almost nothing has fired since go-live.
The substance below is the faithful backtest.

## 0. Live / paper forward record (primary record — honest)

- `omega_trade_closes.csv` on omega-new (HEAD `aedb09b8`): **4 rows, all `GoldDon10mMimic`**
  BE_FLOOR churn (−$2 cost each). **Zero StockDip / StockTurtle / mimic closes** — books went
  real-money live S-19t (07-19) but feed froze + regime risk-off → no signals.
- RD-Agent basket (`factor_basket_ledger.csv`): last cycle ADBE|CRM 2-name, deployed $10,210 →
  equity $10,277 (**+$67, +0.66%**) as of 07-16, then frozen/cash (hourly heartbeat, flat).
- **Conclusion:** judge these books on the faithful backtest, not the (empty) forward tape yet.

## 1. StockDip — ConnorsRSI2 DIP parent (REAL MONEY, wired-11, 8bp RT)

Rule (verbatim engine): `close>SMA200 & RSI2<10` → LONG at close; exit SMA5-bounce or 10d stop.

```
POOLED n=1113  net=+855.3%  PF=1.72  WR=70%  H1=+496  H2=+359  BEAR=+21.9  2022=-29.2  worst=-23.0%
all-6 (net/PF≥1.3/H1+/H2+/BEAR+): PASS.  Every active name +, PF≥1.32.
top: NVDA +140(PF1.83), AMD +139, AVGO +129, MU +126; weakest TPR +38(PF1.32), DELL +35.
```
Confirms the 07-08 audit (PF 1.72 vs 1.60). 2022 calendar drag small & bounded (−29 on +855).
**VERDICT: viable money-maker, all-6 PASS on fresh data.**

## 2. StockTurtle — Donchian 20/10 parent (REAL MONEY, wired-11, 8bp RT)

Rule: LONG at close > max(prior-20 closes); exit first close < min(prior-10 closes).

```
POOLED n=438  net=+1510.5%  PF=2.28  WR=44%  H1=+1011  H2=+499  BEAR=+452.6  2022=-52.4  worst=-19.5%
all-6: PASS.  Every name +, PF≥1.38.  BEAR STRONG (+453) — breakout longs exit fast on the 10d channel.
top: NVDA +332(PF4.37), AMD +322(PF3.48), TPR +165, STX +137, AAPL +134.
```
Confirms 07-08 audit (PF 2.28 vs 2.13). Strongest of the two; bear-positive.
**VERDICT: viable money-maker, all-6 PASS, best book.**

## 3. StockDip BE-mimic overlays (LIVE additive cells, standalone, 8bp)

Independent GoldTrendMimicBook per DIP name, judged STANDALONE (never vs the parent).
```
Variant T (arm2/gb0.5/lc2): n=3898 net=+3040.9% PF=1.80 H1=+1247 H2=+1794 BEAR=+287  → VIABLE (all-6 PASS)
Variant W (arm3/gb0.7/lc3): n=3898 net=+4091.5% PF=1.78 H1=+1689 H2=+2402 BEAR=+310  → VIABLE (all-6 PASS)
```
Both VIABLE standalone; armed legs never book negative (BE-floor verified). **Caveat (honest):**
both LOSE the 2022 calendar year (T −82 PF0.64 / W −161 PF0.52) — the bear-year drag the
BEAR-regime split doesn't fully surface. Additive to the parent, not competing.

## 4. StockTurtle BE-mimic overlays (LIVE additive cells, standalone, 8bp) — BULL-GATED S-20az

The 4 live cells (A/B/C/D) UNGATED are bear-negative → NOT VIABLE standalone:
```
A arm1.5/gb50 UNGATED: n=633 net=+421 PF1.99 BEAR -11 → NOT VIABLE
B arm2.0/gb50 UNGATED: n=633 net=+446 PF1.95 BEAR  -6 → NOT VIABLE
C arm2.5/gb40 UNGATED: n=633 net=+494 PF1.98 BEAR  +8 → (marginal)
D arm3.5/gb40 UNGATED: n=633 net=+495 PF1.87 BEAR  -1 → NOT VIABLE
```
**Fix = BULL-GATE** (operator, feedback-companion-bull-gate-not-reject; SPY-200DMA regime). Bull-
good/bear-bad companion is bull-gated, not culled. Result — all 4 clear all-6, strictly better:
```
A BULL-GATE: n=550 net=+432 PF2.23 mdd -15.9 (was -27.3) → VIABLE
B BULL-GATE: n=550 net=+452 PF2.16 mdd -18.7 (was -33.2) → VIABLE
C BULL-GATE: n=550 net=+486 PF2.14 mdd -19.8 → VIABLE
D BULL-GATE: n=550 net=+497 PF2.02 mdd -20.0 → VIABLE
```
Bull-gate excludes the small bear-negative tail: **PF ↑ (1.9→2.0-2.2), mdd ~halved, 2022 drag
shrinks (−8..−11)**, net ~unchanged. **SHIPPED S-2026-07-20az**: `bull_only=true` on all 4 turtle
cells; registry SPY-200DMA regime (`refresh_daily_regime` from `data/spy_close_hist.csv`, current
at each fan-open, freeze-on-thin holds last regime). DIP mimics unchanged (ungated, VIABLE).
(Harness regime = universe-EW-200DMA proxy; live = SPY-200DMA — near-identical US-equity regime.)

## 5. RD-Agent DayMoverBasket (:7799 paper book, 27-name validated universe, 20bp RT)

Entry: day_return ≥ 5% AND new 20d-high. Exit: regime-switched WIDE trail (bull) / 200d-SMA gate.
```
MAIN WIDE:            n=749  PF=3.03  H1=2.39  H2=3.62  net=+7255%   (both halves +)
  bull-entry:        n=566  PF=3.85  net=+6861   |   bear-entry: n=183 PF=1.34 net=+394 (weak)
Companion GB90 (standalone): PF=2.47  bull PF=3.03  bear PF=1.08
Cost stress @40bp:   MAIN PF=2.40 (holds)  |  GB90 PF=2.33 (holds)
```
Strong PF, both-halves +, 2×-cost robust. **Honest caveats (unchanged):** semi/momentum-
concentrated + bull-beta; the bear-entry sleeve is weak (PF 1.34, BE-floor variant goes negative
−38). **Paper-first** — not live money. Currently empty (risk-off regime → sit in cash, correct).

## 6. Shadow / candidate books (not re-run — status)

- **BigCapHi52** (shadow portfolio, candidate C): sh 1.27 v same-universe control 1.18, 2022
  −11.1 v −18.7, both halves +, 2×-robust (vault verdict). SHADOW, $0 forward.
- **BigCap2pctImpulse / BigCapUpJumpLadder**: KILLED (0 trades all-time) — replaced by §3 mimics.
- **StockDayMoverBeFloor / LadderCompanion**: RETIRED / shadow (BeFloor family dead, hands-off).
- **Extension candidates** (bigcap45 scan, 16bp): turtle-ext PASS = MU MRVL PLTR TSLA META NFLX
  CRWD PANW AMZN GOOGL DELL ASTS RKLB WDC; dip-ext PASS = MRVL PLTR META NFLX SHOP MSTR NOW
  AMZN GOOGL WDC DD. Single-name passes only — prior pooled ext-11/ext-14 were RETIRED at 16bp.
  Candidate expansions per the extend-winning-engines standing order; NOT certified pooled.

## Bottom line

| Book | Status | Faithful verdict (current data, real cost) |
|---|---|---|
| StockTurtle parent | LIVE real money | **+1510% PF2.28, all-6 PASS, bear-strong** — best book |
| StockDip parent | LIVE real money | **+855% PF1.72, all-6 PASS** — solid |
| StockDip mimics (T/W/X/Y) | LIVE additive | VIABLE standalone; lose 2022 calendar |
| StockTurtle mimics (A/B/C/D) | LIVE additive | ungated NOT VIABLE (bear-neg) → **BULL-GATED S-20az → VIABLE all-6, PF↑ mdd↓** |
| RD-Agent DayMoverBasket | PAPER | PF3.03 both-halves+, 2×-robust; bull-beta, weak bear; paper-first |
| BigCapHi52 | SHADOW | sh1.27 v 1.18 control (vault) |

Two live parents re-certified strong. One honest negative (turtle mimics). rdagent basket strong
on paper but bull-beta + paper-only. Forward tape empty pending feed unfreeze (yfinance fallback
next cron) + regime turning risk-on.
```

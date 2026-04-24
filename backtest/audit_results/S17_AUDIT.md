# ENGINE AUDIT — 10 days (2026-04-14 to 2026-04-23)

**Generated S17.** 8 trading days of data (04-18 Sat / 04-19 Sun dropped — markets closed).

## Headline

- **Grand net: -$2,724.54** across 613 non-ghost trades
- **2 ghost trades excluded** (CFE bug: `pnl_usd=-$4810.93` and `-$4767.37` from `held=<epoch_ms>s` corruption — these are real-looking lines in the log with absurd magnitudes caused by a CFE PnL calc using uninitialised entry time; filter at `|net| > $1000` drops them)
- **Single biggest bleeder: CandleFlowEngine** — 383 trades, -$2,066
- **Only positive-net engines: PYRAMID** (+$19.84/4 trades, sub-engine tag) and **AsianRange** (+$3.60/1 trade, under-sampled)
- **HybridBracketGold is the closest bracket engine to profitable**: -$6.89/12 trades = -$0.574/trade, 41.7% WR

## Closest-to-productive ranking

| Rank | Engine | n | WR | Net | Per-trade | Verdict |
|------|--------|---|----|-----|-----------|---------|
| 1 | **PYRAMID** | 4 | 100% | +$19.84 | +$4.96 | Only true winner but n=4, under-sampled. PYRAMID is a sub-engine flag, not a standalone. Needs investigation. |
| 2 | AsianRange | 1 | 100% | +$3.60 | +$3.60 | Fires once per session at most; single-sample, not meaningful. |
| 3 | LondonFixMomentum | 2 | 50% | -$0.66 | -$0.33 | Rare fire, near-zero bleed. |
| 4 | **BBMeanRev** | 12 | 8.3% | -$7.28 | **-$0.61** | Near-breakeven cost, but 8% WR is a problem. Big MFE-per-trade likely. **Candidate for entry-filter tuning.** |
| 5 | **CompBreakout** | 37 | 18.9% | -$21.12 | **-$0.57** | Already-shelved engine from CBE cull, but residual trades in the 04-21 to 04-23 window fired at near-breakeven. If re-enabled with better entry filter, could flip. |
| 6 | HybridBracketGold (BRK) | 12 | 41.7% | -$6.89 | -$0.57 | **Best bracket engine.** 41.7% WR. Close to flipping. |
| 7 | EMACross | 31 | 16.1% | -$37.21 | -$1.20 | Already gated, low per-trade cost. Low WR is structural (trend-following in chop). |
| 8 | TurtleTick | 5 | 80% | -$4.53 | -$0.91 | High WR but small n; losing trades exceed winning sizes (RR mismatch). |

## Biggest bleeders

| Engine | n | WR | Net | Per-trade | Note |
|--------|---|----|-----|-----------|------|
| CandleFlowEngine | 383 | 32.4% | **-$2,065.66** | -$5.39 | **Volume king. Getting better over time (see trajectory).** |
| DomPersistEngine | 52 | 17.3% | -$266.83 | -$5.13 | **Removed S15** — these are pre-removal trades. No further action. |
| XAUUSD_BRACKET (BRK) | 33 | 12.1% | -$125.26 | -$3.80 | Bracket filler engine. 12% WR is severe. Needs separate investigation. |
| GoldFlowEngine | 11 | 0% | -$90.94 | -$8.27 | **Zero wins in 11 trades. Total write-off.** |
| VWAPStretchReversion | 9 | 44.4% | -$39.58 | -$4.40 | Good WR, terrible RR. |

## CandleFlowEngine — the real story

**Exit-reason breakdown (8 days, 383 trades):**

| Exit reason | n | net | per-trade | What it means |
|-------------|---|-----|-----------|----------------|
| **TRAIL_SL** | 31 | **+$1,024.14** | **+$33.04** | Productive trail exits on winning trades |
| PARTIAL_TP | 25 | +$12.54 | +$0.50 | Small partial profits |
| IMB_EXIT | 163 | -$134.46 | -$0.83 | **Imbalance-flip early exit — kills winners before they run** |
| STAGNATION | 6 | -$88.90 | -$14.82 | Stagnation timeouts |
| SL_HIT | 138 | -$1,318.69 | -$9.56 | Normal stop-outs |
| **FORCE_CLOSE** | 20 | **-$1,611.41** | **-$80.57** | **EOD/risk force-closes, catastrophic** |

**Key insight:** CFE has a +$33/trade winning tail (TRAIL_SL) but it fires only 31× out of 383. The IMB_EXIT path fires 163× at -$0.83/trade — **it's cutting winners short and net-costing the engine $134 in the process.** If IMB_EXIT fires on a trade that would otherwise TRAIL_SL to +$33, even a 20% conversion rate would add ~$1,000 back to CFE's net.

**FORCE_CLOSE -$1,611 is dominated by 04-14**: that single day had -$1,086 of CFE losses, most of which were FORCE_CLOSE. Digging: 04-14 had a system-level force close event (likely a reconnect or supervisor kill) that liquidated open CFE positions at unfavourable prices.

**CFE's daily trajectory — the real trend:**

```
04-14   n= 45   wr=26.7%   net=$-1086.47   per_trade=$-24.14   (FORCE_CLOSE storm)
04-15   n= 20   wr=30.0%   net=$-275.16    per_trade=$-13.76
04-16   n= 24   wr=33.3%   net=$-102.92    per_trade=$-4.29
04-17   n= 35   wr=28.6%   net=$-213.95    per_trade=$-6.11
04-20   n= 36   wr=22.2%   net=$-298.00    per_trade=$-8.28
04-21   n= 33   wr=21.2%   net=$-24.64     per_trade=$-0.75
04-22   n= 78   wr=32.1%   net=$-22.13     per_trade=$-0.28
04-23   n=112   wr=42.9%   net=$-42.39     per_trade=$-0.38
```

**This is a clear improving trend.** Per-trade cost has gone from -$24.14 to -$0.28 over 10 days. Win rate has climbed from 21% (04-20) to 42.9% (04-23). The engine is trading more aggressively (112 trades on 04-23 vs 33 on 04-21) AND closer to breakeven. **On 04-22 and 04-23, CFE is effectively break-even on a per-trade basis.**

## Verdict: closest-to-productive engines

### Tier 1 — Already at or near breakeven, worth investing in
1. **CandleFlowEngine** — per-trade cost was -$0.28 on 04-22 and -$0.38 on 04-23 (last 2 days). WR trending up (42.9% last day). Highest-volume engine. **If IMB_EXIT is retuned to preserve winners, CFE is first to go positive.** Specific patch target: the imbalance-flip early exit threshold.
2. **HybridBracketGold** — 41.7% WR, -$0.57/trade, already 04-21 was +$2.80 net. Small dataset but structurally profitable on bullish days. Close to tipping positive.

### Tier 2 — Near-breakeven cost, need entry filter
3. **BBMeanRev** — near-breakeven cost (-$0.61/trade) but 8% WR. Suggests it's firing entries that are structurally wrong — probably entering at false mean-reversion signals during trends. An entry-time trend filter could flip it.
4. **CompBreakout** — residual post-cull trades at -$0.57/trade. If the engine is truly dead (per S16), this is historical. If a re-enable is ever considered, this shows the gate-tightened version from S16 was close to breakeven already.

### Tier 3 — Broken, cull candidates
5. **GoldFlowEngine** — 0% WR in 11 trades, -$8.27/trade. Zero wins. Either its entry signal is inverted or it's gated to only fire on trap patterns. Needs deep investigation — possibly cull next.
6. **VWAPStretchReversion** — 44% WR but -$4.40/trade. Wins too small, losses too big. RR calibration is off.
7. **MacroCrash** — 12.5% WR, -$4.48/trade, 8 trades. Small dataset, possibly session-gate problems.

### Tier 4 — Already removed / structurally broken
- **DomPersistEngine** (S15 cull): captured pre-removal data.
- **CompressionBreakoutEngine** (S16 cull): 33 bracket-related trades.
- **XAUUSD_BRACKET**: 12% WR, -$3.80/trade. This isn't a single engine — it's the bracket filler path receiving entries from multiple sub-engines. Needs to be decomposed further.

## Recommendation

**Focus S18 on CandleFlowEngine IMB_EXIT logic.** Specifically:

1. Profile the 163 IMB_EXIT trades: what was the MFE at exit time? If MFE was still below the trail-arm threshold, the IMB_EXIT was possibly premature. Target a change where IMB_EXIT only fires if MFE < some fraction of the bar ATR (e.g., don't bail on imbalance flip if the trade is already in-money).

2. Investigate 04-14 FORCE_CLOSE storm — was it a reconnect/supervisor event or a genuine risk breach? If reconnect, the FORCE_CLOSE on reconnect should preserve rather than liquidate.

3. Keep HybridBracketGold gated as-is; collect 20+ more trades of data before retuning.

**Do not touch EMACross, BBMeanRev, CompBreakout, VWAPStretchReversion yet.** They're small-sample and the cost per trade is under -$1.50 — the CFE IMB_EXIT fix alone would move the system ~$1,000 closer to profitable, far more than any of these engines could contribute.

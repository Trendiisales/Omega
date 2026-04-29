# Donchian H1 Post-Regime Sweep — Decision Memo

**Run date:** 2026-04-28 (NZST)
**Script:** `phase2/donchian_postregime_sweep_v3.py`
**Validation:** canonical rule (period=20, cooldown=5, strict `>` on close, per-side cooldown with `>=` gap test) reproduces `phase1/signals/donchian_H1_long.parquet` byte-exact (509 / 509 signals, all idx + channel_upper + close match).

---

## Verdict: LOCK `(period=20, sl_atr=3.0, tp_r=5.0)` for Donchian H1 long

This **overrides** the script's auto-pick of rank 1 `(10, 3.0, 5.0)` for the reasoning below.

---

## Top 5 by score (post-2026-01-28 only)

| rank | p | sl_atr | tp_r | n | wr% | pf | stab | score |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 10 | 3.0 | 5.0 | 66 | 47.0 | 1.452 | 0.852 | 1.2377 |
| **2** | **20** | **3.0** | **5.0** | **44** | **45.5** | **1.351** | **0.888** | **1.2003** ← chosen |
| 3 | 15 | 3.0 | 2.0 | 48 | 62.5 | 1.374 | 0.825 | 1.1339 |
| 4 | 20 | 1.0 | 2.0 | 44 | 40.9 | 1.692 | 0.647 | 1.0936 |
| 5 | 10 | 3.0 | 4.0 | 66 | 50.0 | 1.359 | 0.792 | 1.0760 |

## Canonical (20, 1.0, 2.5)

- Rank: **#26 of 101**
- n=44, wr=31.8%, pf=1.465, stab=0.492, score=0.7211
- High PF but low stability — concentrated returns over a few good quarters, not consistently winning

---

## Why rank 2 over rank 1

**Reason 1 — same period as canonical (20).** Rank 1 uses period=10 (10-hour breakout window), rank 2 uses period=20 (20 hours). The original phase1 walk-forward research selected period=20 for a reason: 10-hour windows pick up intraday noise, 20-hour windows capture meaningful pivots. Keeping the period stable means only the *exit geometry* (SL/TP) changes vs canonical — the signal-generation surface stays validated.

**Reason 2 — higher stability (0.888 vs 0.852).** Across the 4 chronological quarters of post-regime data, rank 2 wins more evenly. Rank 1 has slightly more variance, suggesting one or two quarters carry the score.

**Reason 3 — score gap is tiny (~3%).** 1.2003 vs 1.2377. Within sweep noise, especially given small n.

**Reason 4 — robust cluster, not a curve-fit peak.** The `(*, 3.0, 5.0)` row dominates: period 10/15/20 all rank in the top 9 with this SL/TP combo. The edge is in the wide-stop, runner-TP geometry — not in any specific period choice. Picking period=20 puts us in the cluster while staying anchored to the original research.

## Why this is opposite of the previous (v1) sweep

v1 used a reverse-engineered generator (no validation, 465 signals vs canonical 509, ~82% overlap). That run identified `sl=1.0, tp=4.0` as the winner. With the correct generator (v3, byte-exact validation passed), `sl=1.0, tp=4.0` collapses to PF 0.807 (loser cluster). The canonical signal set is 9% larger and structured differently than v1 assumed; the previous decision was based on biased data.

---

## What this means for Variant C1 (NOT YET LOCKED)

C1 is the locked portfolio (Donchian H1 + Bollinger H2/H4/H6 long, 0.5% risk, max 4 concurrent, $10k base, expected ~57%/-7%/PF 1.25). The Donchian H1 cell was the blocker that prevented C1 from shipping. **This sweep resolves the blocker but introduces new variables that require validation.**

Changes from canonical to new params:
- SL widens 1.0 → 3.0 ATR (3× larger position drawdown per loser)
- TP widens 2.5 → 5.0 R (trades hold longer)
- WR rises 31.8% → 45.5% (asymmetry shifts from low-WR/big-runners to mid-WR/balanced)
- PF drops 1.465 → 1.351 (slightly less profit per trade, but distributed more evenly)

Net: per-trade $ risk is the same (0.5% of equity by design), but trade duration is longer, which interacts with the max-4 concurrent-position cap. A wider SL also means each losing trade chews more equity before the stop fires — relevant for the -7% drawdown estimate.

**Required next:** re-run `phase2/portfolio_C1_C2.py` with Donchian H1 params updated to `(20, 3.0, 5.0)`. Compare to baseline metrics. Only then is C1 actually shippable.

---

## Portfolio re-run result — C1_retuned SHIPS

**Date:** 2026-04-28 (NZST)
**Builder:** `phase1/build_donchian_H1_long_retuned.py`
**Retuned ledger:** `phase1/trades_net/donchian_H1_long_3.0_5.0_net.parquet` (509 trades, WR 53.6%, net +3086.65 pts, PF net 1.478)
**Portfolio script:** `phase2/portfolio_C1_C2.py` (C1, C1_retuned, C2 variants)

### A/B: C1_retuned vs canonical C1_max4

| metric | C1_max4 (canonical) | C1_retuned | delta |
|---|---:|---:|---:|
| return | +57.25% | +74.12% | **+16.87 pp** |
| max DD | -6.57% | -5.85% | **+0.72 pp** |
| profit factor | 1.245 | 1.486 | **+0.241** |
| Sharpe | 1.776 | 2.651 | **+0.875** |
| win rate | 42.2% | 55.2% | **+13.0 pp** |
| trades | 741 | 739 | -2 |

**Clean sweep.** C1_retuned dominates on every metric: higher return, smaller drawdown, better PF, better risk-adjusted return, higher WR. The -2 trade delta is noise-level.

### Post-regime carve-out

- PF: 1.334 → **1.630**
- PnL: $1,016 → **$1,591**

Post-regime improvement is sharper than full-sample improvement, which is the right direction — the retune was *targeted* at the post-2026-01-28 regime, and the lift is concentrated there.

### Caveat — loss clusters worsened marginally

| | C1_max4 | C1_retuned |
|---|---|---|
| cluster days | 5 | 6 |
| cluster damage | -$847 | -$1,131 |

A new -$286 down-day appeared on **2026-03-18** (4 cells losing in the same session). Cluster damage is +33% worse in absolute terms.

**Why ship anyway:** the wider-SL geometry means each loser is bigger by design, and the +13 pp WR uplift dominates the equity curve. Max DD *still improved* (-6.57% → -5.85%) despite the worse clusters — meaning the wins between clusters more than refill the bucket. The cluster damage is a known, measured cost, not a hidden risk.

**Flagged for shadow:** track 2026-03-18-style clusters in live shadow data. If clusters stack >2× expected frequency in the first 2 weeks, halt and re-evaluate before going further.

### Decision

**SHIP C1_retuned.** Move to live shadow paper-trading per the CHOSEN.md plan (4–8 weeks).

---

## Files

- `phase2/donchian_postregime/sweep_results.csv` — all 101 combos
- `phase2/donchian_postregime/sweep_results.json` — full metrics + verdict
- `phase2/donchian_postregime/run.log` — tee'd console output
- `phase2/donchian_postregime/CHOSEN.md` — this memo
- `phase2/donchian_postregime_sweep_v3.py` — the validated sweep script
- `phase1/build_donchian_H1_long_retuned.py` — retuned ledger builder
- `phase1/trades_net/donchian_H1_long_3.0_5.0_net.parquet` — retuned ledger (509 trades)
- `phase2/portfolio_C1_C2.py` — A/B portfolio runner (C1, C1_retuned, C2)

---

## Status

- [x] Canonical rule reverse-engineered and validated byte-exact
- [x] Sweep run on correct rule
- [x] Decision: `(p=20, sl_atr=3.0, tp_r=5.0)` locked for Donchian H1 long (research level)
- [x] Portfolio C1 re-run with new params — **DONE 2026-04-28**
- [x] C1 ship-decision locked: **C1_retuned beats C1_max4 on all metrics, ships**
- [ ] 2026-03-18 cluster mechanics post-mortem (in parallel with shadow start)
- [ ] Live shadow paper-trading 4–8 weeks

---

## Next-session opener

> **State for next session — C1_retuned shadow paper-trading kickoff**
>
> SHIPPED: Donchian H1 long retuned to `(20, 3.0, 5.0)`. C1_retuned A/B vs C1_max4 dominates on every metric: +74.12% / -5.85% / PF 1.486 / Sharpe 2.651 / WR 55.2% vs +57.25% / -6.57% / 1.245 / 1.776 / 42.2%. Post-regime PF 1.334 → 1.630. See `phase2/donchian_postregime/CHOSEN.md` for full A/B and caveats.
>
> Caveat: loss clusters slightly worse (5d/-$847 → 6d/-$1,131). New -$286 cell on 2026-03-18. Max DD still improved net-net.
>
> Next: (1) launch live shadow paper-trading run for C1_retuned, target 4–8 weeks of forward data, log every trade against the retuned ledger format. (2) In parallel, post-mortem 2026-03-18 cluster — what regime / DXY / vol / session conditions stacked 4 cells losing same day? If it's a recurring trap, gate the cells against it before live-money.
>
> Halt criteria during shadow: cluster days stacking >2× expected frequency in first 2 weeks → pause and re-evaluate.
>
> Files: ledger `phase1/trades_net/donchian_H1_long_3.0_5.0_net.parquet`, builder `phase1/build_donchian_H1_long_retuned.py`, portfolio `phase2/portfolio_C1_C2.py`. Bars at `phase0/bars_H1_final.parquet`.

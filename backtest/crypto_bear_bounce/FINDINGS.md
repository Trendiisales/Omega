# Crypto Bear-Bounce Study — long-only BTC/ETH in a downtrend (S-2026-07-03)

**Question (operator):** we hold a full crypto stack but NZ regulation forces
LONG-ONLY. BTC/ETH are in a deep downtrend (BTC −51% off the Oct-2025 ATH,
below the 200D SMA since Dec-2025) full of violent bounces. Build engines that
trade those bounces with maximum protection and maximum aggression, without
manufacturing an edge that isn't there.

**Answer in one line:** in the falling-knife phase NO long-only entry family
survives costs — the protective edge is FLAT; the tradeable long edge below the
200D SMA lives ONLY in the recovery sub-regime (close above a RISING 50D SMA),
where a EMA9-reclaim engine ("**BearRecovery**") is PF 5-7 with tiny drawdown
and catches every bear→bull turn months before the Luke 200D gate reopens.

---

## 1. Data + provenance

- Coinbase Exchange public candles, BTC-USD + ETH-USD, **hourly, 2017-01 →
  2026-07** (83,224 bars each; `pull_coinbase.py`). Integrity-gated: 0 spike
  glitches, worst gap 16h. Covers three full bears (2018 −84%, 2022 −77%,
  2025-26 −51% and running) + two full bulls.
- Harness `bear_bounce_bt.py`: portfolio event loop, signals on COMPLETED
  daily/4h bars only, fills at NEXT hourly open, stops filled intrabar with
  extra slip, worst-case same-bar ordering (stop before TP), mark-to-market
  equity curve. Costs **6 bps/side + 10 bps stop-slip** (IBKR MBT/MET measured
  2-8 bps ladder); stress runs at 2-3×.
- Grade: **daily-signal / hourly-mark faithful sim on spot prices** — not
  tick-level, not the live book's code path. SHADOW before live per standing
  rule.

## 2. Bounce anatomy (why trend/dip engines die here)

Pivot-low bounces in bear regime (close < SMA200), lowest-low ±3d pivots:

| period | median rally | median days | left after +1d confirm |
|---|---|---|---|
| 2018 | +15-23% | 6 | ~7-14% |
| 2022 | +15-20% | 6-7 | ~8-9% |
| 2025 | +13-23% | 13 | ~7-14% |
| **2026** | **+9-10%** | **4-6** | **~5-8%** |

Bounces are real but SHORT and getting smaller. Anything that confirms late
(trend entries) and exits on a flip (confirms 2-3 days after a 5-day bounce
tops) enters late AND exits late — negative capture by construction.

## 3. Seven candidate families tested — the knife-phase law

All long-only, regime-gated to close < SMA200, structural stop, real costs.
Headline PF / net over 9.5y, and the two grinding bears that matter:

| candidate | overall | 2022 | 2026 | verdict |
|---|---|---|---|---|
| C1 flush→4h-reclaim MR | PF 0.89 | PF 0.63 | PF 9.4 (n=7) | dead (exit-sensitive, unstable) |
| C2 4h Donchian rally-rider | PF 1.55 +$130k | **PF 0.61 −$22k** | **PF 0.10 −$36k** | recovery-only; bleeds in knife |
| C3 daily EMA9-reclaim rider | PF 1.84 | PF 0.71 | PF 0.46 | same |
| C4 flush-armed breakout | PF 0.65 | PF 0.80 | PF 0.15 | dead |
| C5 confirmed-low + TP-into-strength | PF ~1.0 | PF 0.23-0.56 | PF 0.28-0.57 | dead |
| C6 panic snapback (1h cascade, gold-PanicBounce transfer) | PF 0.9-1.1 | PF ≤0.98 | PF ≤0.89 | dead — crypto cascades keep cascading |
| C7 W-bottom (higher-low + strong close) | PF 0.90 | PF 0.31 | PF 0.17 | dead |

Giveback clips, tight trails, dual-symbol confirmation, faster sub-gates
(20/30D MA) were all swept on top: none rescue the knife phase. This
independently reconfirms two standing findings: Luke "crypto dips keep
dipping" (entry A dead) and the intraday cost+noise wall.

**THE LAW: in the knife phase (close < SMA200 and NOT above a rising SMA50),
every tested long-only entry loses after costs. FLAT is the edge. Any future
session proposing a knife-phase crypto long must beat this study first.**

## 4. The engine that survives: BearRecovery (C3 + recovery sub-gate)

**Spec (daily bars, UTC, evaluated on completed bars only):**
- Regime gate: close < SMA200 (bear) **AND close > SMA50 AND SMA50 > SMA50[5d ago]**
  (recovery sub-regime: intermediate trend has turned).
- Entry: daily close crosses ABOVE EMA9 after ≥3 consecutive closes below.
  Fill next hour open.
- Initial stop: entry-day low − 0.5 × ATR14(daily), intrabar.
- **BE-and-ride floor: after MFE ≥ +2%, floor = entry (lock 0). Ride the EMA9
  exit; only the floor bounds the downside.** (operator BE-and-ride semantics)
- Exit: first daily close < EMA9 (ride-wide; bounces are ridden to the flip).
- No giveback clip, no cold cut, no TP (all swept, all hurt or no-op — §5).
- Sizing: risk-based off the structural stop, **2% equity risk** per trade
  (aggressive tier; see §6), concurrent BTC+ETH allowed.

**Results (2% risk, $100k, 2017-2026):** n=20, **PF 7.23**, net +$38.7k,
worst trade −4.8%, book maxDD −5.9% (mark-to-market), median hold 43h.
- Walk-forward halves: 2017-21 **+$21.1k** / 2022-26 **+$17.5k** — both positive.
- Per symbol: BTC PF 8.6 / ETH PF 6.2 — both positive.
- Cost stress ×2: PF 4.81; ×3: PF 4.49 — cost-robust (few trades, multi-day holds).
- Sensitivity: below_min 2/4 → net +$52k/+$15k; BE arm 1.5%/3% → PF 13.6/5.6.
  Broad plateau, not a knife-edge fit.
- Behavior by year: pays when a bear ENDS or a real multi-week rally comes
  (2019 +$6.2k, 2020 +$15.3k, 2023 +$18.1k, 2024 +$3.6k); tiny probe costs in
  grinding years (2018 −$349, 2022 −$3.2k, 2025 −$55, 2026 −$894 across 4
  probes). It is the "catch the turn early" engine — it engaged Apr-May 2026
  and exited at breakeven-floor when the bounce failed at SMA50.

**ADVERSE-PROTECTION verdict (mandate): BE-and-ride floor arm=2% lock=0 —
backtested: net UP ($22.8k→$23.4k @1% risk), PF UP (5.16→8.85), worst trade
improved (−8.3%→−4.8%), maxDD flat. Cold cut (never-green v2): NO-OP at every
value 3-12% — the EMA9-flip exit already bounds never-green trades (same
verdict as the live crypto companion book). Giveback clips 30/50/70%: net
−62% to −90% — proven harmful, DO NOT add.**

## 5. Protection sweep receipts (1% risk baseline)

| config | n | PF | net | worst | mDD |
|---|---|---|---|---|---|
| baseline (stop + EMA9 exit) | 20 | 5.16 | $22,770 | −8.3% | −3.8% |
| **+ BE floor arm2% lock0** | 20 | **8.85** | **$23,381** | **−4.8%** | −3.7% |
| + BE arm3% lock0 | 20 | 6.91 | $24,278 | −8.3% | −3.7% |
| + cold cut (any 3-12%) | 20 | 5.16 | $22,770 | −8.3% | −3.8% |
| + giveback 30%/gate3% | 20 | 1.78 | $2,673 | −8.3% | −2.0% |
| stop 0.25/1.0/2.0 ATR | 20 | 5.4/4.9/5.0 | $28k/$16k/$10k | | |

## 6. Aggression = sizing, not more trades

| risk/trade | PF | net (9.5y) | book maxDD |
|---|---|---|---|
| 1% | 5.16 | $22.8k | −3.8% |
| **2% (ship)** | 4.32 | $37.6k | −5.9% |
| 3% (max aggressive) | 4.18 | $50.5k | −7.8% |
| 5% | 4.20 | $61.4k | −9.9% |

The edge is rare (~2 signals/yr) and asymmetric; the correct way to be
aggressive is size, and the BE floor makes size cheap (worst trade −4.8%).
Anything that adds FREQUENCY in the knife adds losses (§3).

## 7. The full regime ladder (the "solution")

| phase | definition (daily close) | book |
|---|---|---|
| KNIFE | < SMA200, not above rising SMA50 | **FLAT — no long engine exists (proven)** |
| RECOVERY | < SMA200, > rising SMA50 | **BearRecovery** (this engine), 2-3% risk |
| BULL | > SMA200 | Luke daily system (entry C, ride-wide) — already validated |

This closes the standing gap: the old book was Luke-gated (idle below SMA200)
with nothing below; now every regime has an explicit, backtested posture, and
the bear→bull handoff is continuous (BearRecovery is in from the first
rising-SMA50 reclaim; Luke takes over above SMA200).

## 8. Limitations / owed

- n=20 over 9.5y — statistically thin by construction (the sub-regime is
  rare). Mitigated by: WF-halves, per-symbol, cost-stress, sensitivity plateau
  all positive; and by the fact the DOWNSIDE is structurally bounded (floor).
- Search breadth disclosed: 7 entry families × protection grids were examined
  to establish the knife law; the champion was selected from that search →
  treat live shadow as the true out-of-sample. **SHADOW first**, per standing
  rule; wire into the ~/Crypto book (`tools/crypto_bear_recovery.cpp` emits the
  signals) and judge on the shadow ledger.
- Spot prices; MBT/MET execution adds roll + weekend-gap nuances (CME closed
  weekends — crypto isn't; a weekend flip exits Monday). PAXOS spot avoids
  this (24/7) at higher bps.
- SOL and the alt book were not tested (data pulled for BTC/ETH only); the
  law is asserted for the majors only.

## 9. C++ port (S-2026-07-03, operator: "no python in Omega")

The deployable engine is **C++**: `include/CryptoBearRecoveryEngine.hpp`
(self-contained; internal UTC daily aggregation from `on_price()`, intrabar
stop/BE-floor marks, `seed_from_daily_csv()` warm-seed with `[SEED]` boot line,
ADVERSE-PROTECTION annotation, `state()` ladder accessor). Signal CLI:
`tools/crypto_bear_recovery.cpp` (pipe Coinbase daily-candle JSON or a
ts,o,h,l,c CSV on stdin → one JSON state/signal line; zero deps beyond curl).
Warm-seeds: `phase1/signal_discovery/warmup_{BTC,ETH}USD_D1.csv` (500 days).

**Port verification** — `faithful_bear_recovery_bt.cpp` drives the REAL engine
class over the same hourly corpus: n=20 (Python 20), same entry dates, PF 9.06
vs 8.85 @1% risk, worst −6.0% vs −4.8%. One trade diverges (ETH 2019-03-28)
because the C++ engine fills at the daily open, one hour earlier than the
Python harness's next-hour fill — the C++ timing is the live-correct one (the
book acts right after the UTC daily close). Audits: adverse-protection PASS,
ungated-engine audit clean, `g++ -fsyntax-only` clean.

Note the study harness (`bear_bounce_bt.py`) and data puller stay Python — they
are research tooling like the rest of `backtest/*.py`, not shipping Omega code.

## Repro

```
python3 pull_coinbase.py /tmp/crypto_bear_bounce          # ~6 min (data pull)
python3 bear_bounce_bt.py --data /tmp/crypto_bear_bounce --phase entries   # candidate families
python3 bear_bounce_bt.py --data /tmp/crypto_bear_bounce --phase c5|c6|c7|subgate|recovery
python3 bear_bounce_bt.py --data /tmp/crypto_bear_bounce --phase protect   # protection sweeps
python3 bear_bounce_bt.py --data /tmp/crypto_bear_bounce --phase final     # champion + WF + trade list

# C++ faithful arbiter (drives the real engine class):
g++ -O2 -std=c++17 -I../../include -o /tmp/cbr_bt faithful_bear_recovery_bt.cpp
/tmp/cbr_bt /tmp/crypto_bear_bounce 0.01

# live signal (from repo root):
g++ -O2 -std=c++17 -Iinclude -o /tmp/cbr tools/crypto_bear_recovery.cpp
curl -s "https://api.exchange.coinbase.com/products/BTC-USD/candles?granularity=86400" | /tmp/cbr BTC-USD
```

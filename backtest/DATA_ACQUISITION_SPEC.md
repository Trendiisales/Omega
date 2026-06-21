# Data Acquisition Spec — new asset classes (2026-06-21)

Origin: 4 discovery rounds proved the equity+gold daily corpus is tapped out; proxy
tests (Yahoo 11yr, `data_value.py` / `bonds_test.py`) proved the *value* lives in 3 new
data classes. This is the procurement + ingest + engine + validation plan for each.

**Key cost insight:** IBKR (already held, LIVE gw port 4001) delivers nearly all of #1
(futures), much of #3 (VX futures + delayed options), and #2's raw prices. The only real
external spend is **one cheap equity point-in-time vendor (~$40/mo)** to kill survivorship
on #2. Everything else is build effort, not vendor bills.

All three gate the same way (standing rules): faithful daily/tick BT, both-regime +
both-halves, cost-inclusive, SHADOW before live, live shadow-ledger = the only real number.

---

## #2 STOCK UNIVERSE — best new alpha (TOP PICK)

Proxy result: market-neutral x-sec momentum (6mo lb) Sh 0.65, **BEAR +0.44**, both-halves+.
The one genuine new *alpha* stream. Proxy was 50 current survivors → inflated; real edge
needs survivorship-free point-in-time membership.

- **Acquire:** 100-500 liquid US large/mid-caps, daily OHLCV adjusted, 12-15yr, **point-in-time
  index membership** (S&P500/1000) + delisted names + delist dates.
- **Vendor:** **Norgate Data** (~US$33-65/mo, "Platinum") — purpose-built survivorship-free
  PIT membership + delisted, the retail-quant standard. Alt: Shardar/Nasdaq Data Link
  (~$50-150/mo, adds fundamentals). IBKR prices are cheap but NOT clean PIT → biased, avoid
  for the universe.
- **Fields:** `date, open, high, low, close, adj_close, volume, index_member_flag(date), delist_date`.
- **Storage:** `C:\Omega\data\stocks_pit\<TICKER>_d1.csv` + `universe_membership.csv` (date→constituent set).
- **Engine:** `CrossSectionalStockMomentumEngine` — **clone the existing `CrossSectionalIndexEngine`
  chassis** (already built, multi-leg, regime-gated, next-open fills). 6mo lookback skip-1mo,
  rank PIT-universe, long top decile / short bottom decile, monthly rebalance, market-neutral,
  inverse-vol leg weights. Wire the new `PortfolioVolScaler` (970f00ad) on top — momentum-crash
  is the −37% DD risk, vol-management is its known fix (Moreira-Muir + Barroso-Santa-Clara).
- **Cost model:** equity ~2-5 bps/side + **short borrow** (the catch — model 50-300 bps/yr on the
  short leg; cheap on large-caps, lethal on small). Long-only top-decile is the fallback if
  borrow kills the short leg.
- **Validate:** faithful daily BT 2010-2026, both-regime, bear-positive must reproduce on PIT
  (expect Sharpe < proxy 0.65 once survivorship removed — but bear-positive market-neutral
  property is structural, should hold). Shadow ≥30 rebalances before live size.
- **Effort:** MED. Data trivial (Norgate API). Engine = clone + universe plumbing + borrow model.

## #1 BROAD FUTURES — best crisis-hedge / diversifier

Proxy result: 16-ETF TSMOM flips bear −2.08→−0.16, **2022 −1.35→+1.49**, maxDD halved.
Low solo Sharpe (0.37) but NEG-correlated to the equity book = portfolio Sharpe lift + tail
hedge. Every sleeve (rates/FX/commodity) made money in the 2022 rate-shock by going short.

- **Acquire:** 20-40 liquid futures, continuous back-adjusted daily, 15yr + roll calendar:
  rates `ZT ZF ZN ZB` (curve), FX `6E 6J 6B 6A 6C`, commodity `GC SI CL NG HG ZC ZS ZW`,
  equity `ES NQ RTY` + intl (`FDAX FESX` if entitled). Keep front+back per expiry for carry/roll-yield.
- **Source:** **IBKR (≈$0 — already held)** via `reqHistoricalData` per contract + build the
  continuous series (roll on volume/OI). OR **CSI Data / Norgate Futures (~$30/mo)** for clean
  pre-built back-adjusted continuous (saves the roll-build — recommended to de-risk the plumbing).
- **Storage:** `C:\Omega\data\futures\<SYM>_cont_d1.csv` (back-adjusted) + `<SYM>_rolls.csv`.
- **Engine:** `ManagedFuturesEngine` — 12m-sign TSMOM, per-market target-vol scaling, equal-risk
  basket, L/S, monthly; + a **carry leg** (roll-yield = front/back basis sign). Correlation-cluster
  cap (already in `OmegaAdaptiveRisk.corr_heat`) + `PortfolioVolScaler` apply.
- **Cost model:** futures commissions + 1-tick slippage (MUCH tighter than CFD — this is why it
  works where spot-CFD intraday died; see `omega-intraday-spot-cfd-cost-wall`).
- **Validate:** faithful daily BT; **2022 crisis-alpha (+1.49) MUST reproduce** on real futures;
  both-regime. The value is the negative correlation to the existing book — measure book+sleeve
  combined Sharpe/DD, not the sleeve alone.
- **Effort:** HIGH (continuous-contract build + roll calendar + 20-40 instrument plumbing + a
  futures-feed bridge on the VPS). Data ~$0-30/mo.

## #3 INDEX OPTIONS — highest carry, worst tail

Proxy result: VRP short-vol (VIXY+contango) Sh 0.57, **CAGR 17%**, but maxDD −58% (−92% raw).
Real premium, lethal left tail. Only viable with real options structure + a mandatory tail-hedge.

- **Acquire:** SPX/index option chains daily (strike/expiry/bid/ask/IV/greeks) for put-write +
  dispersion; OR minimum-viable = **VX futures term structure** (front/back) for the VRP roll.
- **Source:** **IBKR delayed (≈$0)** for VX futures + delayed SPX chain snapshots (the **GEX
  snapshotter is already half-built** — `omega-gex-dealer-gamma`). Clean systematic chains =
  **ORATS (~$100-300/mo)** or LiveVol if scaling up.
- **Storage:** `C:\Omega\data\options\spx_chain_<date>.csv` + `vx_term_d1.csv`.
- **Engine:** `VrpHarvestEngine` — short vol via VX-roll or SPX put-write/strangle, contango-gated
  (`VIX/VIX3M<1`), **hard tail-stop + standing long-OTM-put tail-hedge** (the −58% DD is the
  whole risk — un-hedged short vol is uninsurable). Size SMALL (≤ small % of book).
- **Validate:** the tail IS the strategy risk — BT must span 2018 volmageddon, 2020 COVID, 2022;
  tail-hedge mandatory pre-live; never live without the put-hedge leg.
- **Effort:** HIGH + risky. GEX module = head start. Data ~$0 (IBKR/VX) → $100-300/mo (ORATS).

---

## Phased plan (value × feasibility)

| Phase | Acquire | Vendor / source | Cost | Build | Why first |
|-------|---------|-----------------|------|-------|-----------|
| **1** | #2 stock PIT universe | Norgate (~$40/mo) | low | clone CrossSectionalIndexEngine | best new alpha, cheapest, lowest build risk |
| **2** | #1 broad futures | IBKR (held) or CSI ($30) | ~$0-30/mo | continuous-contract build + ManagedFuturesEngine | crisis-hedge for the existing book |
| **3** | #3 index options | IBKR delayed / VX (held) | ~$0 start | extend GEX snapshotter + VrpHarvest + tail-hedge | high carry, only after tail-hedge proven |

**Free parallel track:** let the live SHADOW LEDGER accumulate clean trades (26→500+) — unlocks
meta-labeling at zero cost, just months of runtime.

**Recommendation:** start Phase 1 (Norgate + clone the CrossSectional chassis) — cheapest,
fastest, best risk-adjusted alpha, and the engine chassis already exists.

# Gold (XAU/MGC) Simple Long+Short Engine — Salvage + Honest Backtest

Date: 2026-07-20 (S-2026-07-20ag)
Session: Omega research (RESEARCH ONLY — no live box, no deploy, no push; local commit only)
Mandate: a SIMPLE, viable long **and** short gold book that only trades when it can cover cost.
Verdict: **DEAD as a long+short book.** Nothing clears the gate in both regimes. The *only*
structure with any signal is a **long-only gold trend-follower**, and it "works" solely because
the 6-month test window is a strong bull — it is net-**negative** in the 2022 bear. That is
market **beta (being long a rising asset), not a certifiable edge**, and there is no viable short
side anywhere. Do **not** build a symmetric long/short gold engine on this evidence.

---

## PART 1 — Scaffolding salvaged (from the DEAD AurumBreakPullback)

Lifted ONLY the edge-neutral plumbing into a clean reusable harness. Source of the salvage:
`backtest/aurum/AurumBreakPullback.cpp` (audited DEAD — `AURUM_BREAKPULLBACK_AUDIT_2026-07-20.md`).

**`backtest/gold_ls_harness.hpp`** (new, reusable) — kept verbatim, edge-neutral:
- Streaming tick/quote CSV → 1-minute bars (`MinuteCsvReader`, midpoint of bid/ask, no OOM on 46M ticks).
- Howard-Hinnant civil-date + London/NY **DST** clocks (`london_clock`/`new_york_clock`).
- CSV column inference; N-minute `BarAggregator`; `EMA`/`ATR` indicators.

**DROPPED (the dead / dishonest parts):**
- The entire break-pullback **entry thesis** (no edge; net-negative every leg in the audit).
- The fatal `RegimeReady` state-clobber (made the engine trade 0 as shipped).
- The **fill-at-level + fixed-slip** booking — the audit's headline dishonesty (`close_position`
  booked exits at the stop LEVEL minus a fixed tick, hiding gap-through tails).

**REPLACED with an HONEST fill book** (`HonestBook` in the harness; cost embedded in every fill):
- Stop-order breakout entry = `worse-of(open, trigger) + half-cost`.
- Market-on-open entry = `open + half-cost`.
- Protective stop exit = `worse-of(open, stop) − half-cost` — a gap-through is booked at the
  **real price the bar traded through** (`min(open,stop)` long / `max` short), never level+slip.
- Limit target exit booked at the target level (a favourable gap is **not** credited).
- No look-ahead: signals from **completed** bars, execution on the **next** bar; an open position
  is managed on bar *b* using indicator state only through *b−1*; the stop is checked **before**
  the favourable-excursion update within a bar.

**`backtest/gold_ls_bt.cpp`** (new) — the strategy driver. Streams a file once, runs LONG and
SHORT engines simultaneously & independently for one structure, records raw (pre-cost) fill levels,
and applies cost analytically so 1× and 2× are exact. Cost-viability entry filter (OmegaCostGuard
analog): a trade is taken only if the structure's expected reward (bp) ≥ `viab_mult × 5bp`.

---

## PART 2 — Data, costs, priors

**Data integrity gate (mandatory, per file):**

| File | Rows | Span / regime | Verdict |
|---|---|---|---|
| `/Users/jo/Tick/xau_6mo_corrected.csv` | 46,465,933 | 2025-11-02 … 2026-04-24 — **BULL** | CERTIFIED CLEAN |
| `/Users/jo/Tick/xau_2022bear_tick.csv` | 17,834,719 | 2022-06-01 … 2022-09-30 — **BEAR** | CERTIFIED CLEAN |
| `2yr_XAUUSD_daily.csv` | 738 | — | REJECTED (backward ts) — **not used** |

**MGC:** no minute/tick MGC exists anywhere in `/Users/jo/Tick` (only H1/H4/30m). MGC is therefore
treated as an **XAU proxy** (MGC ≈ XAU, corr ≈ 1). Any MGC book would inherit these XAU verdicts;
it cannot be independently certified on the data on hand. **Stated limitation.**

**Costs (honest, IBKR):** round-trip = 2×1.5bp + spread ≈ **5bp**, embedded inside fills. 2× = 10bp.

**Prior tombstones checked (did not re-derive what is already judged):**
- `XauTurtleD1Engine` (40d Donchian, long-only) — shadow; the honest re-cert this same day
  (`GOLDMIMIC_HONEST_RECERT_2026-07-20`) COLLAPSED the live fast-Donchian gold books
  (GoldDon10m −227%, XAU_4h_DonchN20 −42%). Fast Donchian on gold is a known chop-loser.
- `GoldEmaCrossM30Engine` (EMA cross both dirs) — **DEAD**, net −$1292 PF0.57.
- `XauTsmomFastD1`, `XauPullbackContD1`, `XauInsideBarD1` — disabled (regime/WF fails).
- Consistent prior signal: **gold intraday trend/breakout is long-biased and regime-fragile;
  no robust short.** This study re-confirms it on honest fills across two regimes.

---

## PART 3 — Structures tested (each LONG and SHORT, separately)

Four plain, well-understood structures; bar size swept to give trend a fair (slow) shot.

| Structure | What it is | Exit |
|---|---|---|
| **DONCH** | Donchian(N) breakout trend (Turtle) | ATR stop + opposite-Donchian trail + chandelier |
| **EMA** | EMA(fast/slow) regime cross trend | ATR stop + chandelier + opposite-cross |
| **MR** | ATR-band mean reversion (buy dip / sell rip vs EMA) | ATR stop + limit target at the mean + time |
| **ORB** | NY session opening-range breakout (London omitted for tractability) | ATR stop + ATR target + session flatten |

**Gate (per structure, per direction):** net > 0 **and** PF ≥ 1.3 (n ≥ 20), holds at **1× and 2×**
cost, **both** chronological halves positive, in **both** regimes (bull 6mo + 2022 bear).

### Results — 1× cost, ALL, both regimes (net in bp; sum of per-trade net returns)

| Structure / dir | BULL n | BULL net | BULL PF | BEAR n | BEAR net | BEAR PF | Gate |
|---|--:|--:|--:|--:|--:|--:|---|
| DONCH b15 n20 long | 282 | +970 | 1.16 | 198 | −1321 | 0.52 | FAIL |
| DONCH b15 n20 short | 285 | −1817 | 0.75 | 204 | −403 | 0.84 | FAIL |
| DONCH b60 n20 **long** | 61 | **+2228** | **2.12** | 48 | **−534** | **0.53** | **bull-only** |
| DONCH b60 n20 short | 60 | −344 | 0.89 | 59 | −152 | 0.89 | FAIL |
| DONCH b60 n40 long | 47 | +1332 | 1.70 | 29 | −615 | 0.32 | FAIL (bear) |
| DONCH b240 n20 long | 17 | +1753 | 2.94 | 11 | −646 | 0.17 | FAIL (n, bear) |
| DONCH b240 n20 short | 11 | +430 | 1.49 | 18 | −203 | 0.76 | FAIL (n, bear) |
| EMA b30 20/50 **long** | 52 | **+1561** | **2.13** | 37 | **−362** | **0.51** | **bull-only** |
| EMA b30 20/50 short | 52 | +193 | 1.10 | 37 | −41 | 0.94 | FAIL |
| EMA b60 20/50 long | 25 | −315 | 0.77 | 27 | −784 | 0.19 | FAIL |
| MR b5 k2.5 long | 635 | −1630 | 0.88 | 587 | −2915 | 0.50 | FAIL |
| MR b5 k2.5 short | 850 | −4465 | 0.67 | 528 | −2202 | 0.58 | FAIL |
| MR b15 k2.5 long | 191 | −1106 | 0.86 | 202 | −1252 | 0.60 | FAIL |
| MR b15 k2.5 short | 306 | −3484 | 0.58 | 179 | −553 | 0.79 | FAIL |
| ORB b5 or30 long | 100 | −962 | 0.47 | 78 | −302 | 0.59 | FAIL |
| ORB b5 or30 short | 100 | −503 | 0.70 | 82 | −683 | 0.28 | FAIL |

*(Every SHORT cell fails in every regime. Every MEAN-REVERSION and ORB cell fails everywhere —
gold trends through the bands and runs the reversion over. Confirms priors.)*

### The only two non-FAIL cells — both LONG trend, both bull-only, robust *within* bull only

Both pass the full bull sub-gate (both halves +, 2× cost +) but are decisively negative in bear:

**DONCH b60 n20 long** (1-hour Donchian-20 breakout):
- BULL 1×: n61 net **+2228bp** PF **2.12** avgR 0.43 · h1 +1430 PF2.80 · h2 +799 PF1.67 · 2× +1922 PF1.89 ✅
- BEAR 1×: n48 net **−534bp** PF **0.53** · both halves negative · 2× −774 PF0.42 ❌

**EMA b30 20/50 long** (30-min EMA20/50 regime cross):
- BULL 1×: n52 net **+1561bp** PF **2.13** avgR 0.40 · h1 +838 PF2.79 · h2 +722 PF1.79 · 2× +1300 PF1.84 ✅
- BEAR 1×: n37 net **−362bp** PF **0.51** · both halves negative · 2× −546 PF0.38 ❌

---

## PART 4 — VERDICT: DEAD (for the long+short mandate)

1. **No structure clears both regimes in either direction.** Zero PASS-BOTH cells.
2. **No viable SHORT anywhere.** Every short cell is net-negative or PF<1.3 in both regimes. Gold's
   intraday behaviour in both windows offered no honest, cost-covering short edge.
3. **Mean-reversion and ORB are dead** on gold across both regimes (gold trends; bands/ranges get run over).
4. **The only signal is a LONG-ONLY trend-follower** (1h Donchian-20 breakout, or 30m EMA20/50 cross).
   It is genuinely net-positive after honest gap-through fills at both 1× and 2× cost, both halves —
   **but only in the 6-month bull window.** It flips net-negative (PF ≈ 0.5) in the 2022 bear. That
   is being **long a rising asset = beta, not alpha.** One bull sample cannot certify it; a single
   regime is exactly the fragility the gate exists to catch, and the priors (XauTurtleD1 collapse,
   GoldEmaCrossM30 DEAD) already flagged gold trend as long-biased and regime-fragile.

### Honest recommendation

- **Do NOT build a symmetric simple long/short gold engine.** The short side has no edge on this data;
  a long+short book would just bleed the short leg.
- **Do NOT ship the long-only trend cell as a "certified" book.** It is uncertified bull beta on ONE
  bull sample — it dies in the bear. Per the operator's bull-gate rule I am *flagging* it (long-only
  gold-trend has real bull-regime signal) rather than dismissing it, but it is **not viable as an
  all-weather book** and must not be presented as one.
- If a long-only gold-trend sleeve is ever pursued, it needs (a) an external **bull-regime gate**
  (only long when a higher-TF trend filter is up — e.g. price > slow MA), tested to actually go flat
  through 2022-style bears, and (b) certification across **more than one bull window**. That is a
  different, gated product — not the simple both-ways book requested. On the evidence here: **DEAD.**

---

## Reproduce

```bash
cd backtest
clang++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -o /tmp/gold_ls_bt gold_ls_bt.cpp
# one structure, both dirs x both costs x all/h1/h2 in one pass:
/tmp/gold_ls_bt --file /Users/jo/Tick/xau_6mo_corrected.csv --mode DONCH --bar 60 --don_n 20 --trail_atr 3.0
/tmp/gold_ls_bt --file /Users/jo/Tick/xau_2022bear_tick.csv --mode DONCH --bar 60 --don_n 20 --trail_atr 3.0
```

Costs 5bp (1×) / 10bp (2×) embedded in fills. Full grid: modes {DONCH,EMA,MR,ORB} × bar
{5,15,30,60,240} × both regimes. All RESULT lines were captured in the S-2026-07-20ag grid run.
```
```

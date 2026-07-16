# StockDip MIMIC overlay — backtest findings (S-2026-07-15)

**Ask:** 2 independent MIMIC engines (Tight + Wide, gold `GoldTrendMimicBook` pattern)
layered on `StockDipTurtleEngine` DIP entries, with mandatory downside protection
(pre-arm loss-cut + post-arm BE-floor), judged **STANDALONE**.

Harness: `backtest/stockdip_mimic_bt.py` · Data: `backtest/data/bigcap_daily_ohlc/<SYM>.csv`
(Yahoo split+div-adjusted daily OHLC, 2016-07..2026-07; STX+TPR fetched this session via the
same data-only Yahoo path). Universe = the 11 wired StockDip DIP all-6 passers:
MU, NVDA, AVGO, DELL, CRDO, STX, INTC, AMD, AAPL, TPR, MSFT. Cost = **8 bp RT** debit/clip.

## Mechanism replicated (faithful, not strawman)

- **Trigger** = StockDip DIP entry reconstructed exactly per `StockDipTurtleSym`:
  `close > SMA200(prior 200 closes)` AND `RSI2(incl today, Cutler) < 10` → LONG at that
  day's close. 3,892 entries across the 11 names (2017-05 .. 2026-07), 267 in bear regime.
- **Mimic leg** = exact port of `GoldTrendMimicBook::on_h1_bar` (long, dir=+1), managed on
  each SUBSEQUENT **daily** bar intrabar low→high→close (adverse-first): arm at `arm_pct` MFE;
  post-arm trail keeps `(1-gb)·peak` (**BE-floored**, since peak≥arm>0 → an armed leg can never
  book negative — verified: every `TRAIL_STOP` clip ≥ `(1-gb)·arm − cost > 0`); pre-arm
  `LOSS_CUT` at `−lc_pct`; independent `WINDOW_CAP` flush at the close after `cap_bars` (=10
  daily bars, matching StockDip's 10-day max hold). Fills book AT the stop level (resting-stop
  convention — the engine's own `book_clip_` convention on the native-bar path).
- **BE-entry gate NOT used** (`be_entry_pct=0`): the task's protection spec is pre-arm loss-cut
  + post-arm BE-floor, which is the enter-at-trigger `GoldTrendMimicBook` default. (A BE-entry
  overlay is a separate lever, not requested here.)
- **Standalone judging** (feedback-companion-independent-engine): every number below is the
  mimic's OWN book, net of 8 bp. NEVER compared to riding the stock or to StockDip's own return.
  Metric = additive per-clip return book: `net% = Σ (clip_ret − 8bp)` in % of clip notional;
  PF = Σwins/|Σlosses|; drawdown = max drawdown of the chronological banked curve.

## Verdict table (pooled, all 11 names, net of 8 bp)

| Variant | arm | gb | lc | cap | n | net% | avg/clip | PF | WR | worst | banked mdd | H1 | H2 | bull | proxy-bear | **2022** | all-6 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **T (Tight)** | 2.0 | 0.5 | 2.0 | 10 | 3892 | **+3023%** | +0.78% | 1.79 | 52.7% | −2.08% | **−91%** | +1217 | +1806 | +2736 | +287 | **−82** | ✅ |
| **W (Wide)**  | 3.0 | 0.7 | 3.0 | 10 | 3892 | **+4085%** | +1.05% | 1.78 | 55.1% | −3.08% | **−167%** | +1665 | +2420 | +3775 | +310 | **−161** | ✅ |

Both variants clear the standalone all-6 gate: **net>0, PF≥1.3, both time-halves>0, both
regimes (proxy) >0, all-11 names net-positive** (T: every name +108%..+495%; W: +101%..+671%).
Exit mix — T: LOSS_CUT 1832(−3811%) · TRAIL_STOP 1808(+4219%) · WINDOW_CAP 250(+2607%).
W: LOSS_CUT 1674(−5156%) · TRAIL_STOP 1335(+2145%) · **WINDOW_CAP 880(+7080%)**.

## The loss-cut / drawdown-cancel verdict (REQUIRED, project-mimic-drawdown-cancel-gate)

The grid is monotonic: **net improves as `lc` loosens** (lc 1.0→3.0 adds ~+1000% net) — the
loss-cut is net-NEGATIVE in isolation. This is the *exact* `StockDipTurtleEngine` header
warning reproduced ("a cold loss-cut on a mean-reverter cuts the dip it is paid to buy"):
StockDip buys RSI2<10 dips that keep falling short-term before mean-reverting, so a tight cut
fires near the low. **BUT** the cut is the mandated protection — it roughly HALVES the banked
drawdown and the 2022 bleed (T lc2.0: mdd −91%/2022 −82%; loosest lc3.0: mdd −167%/2022 −161%).
Verdict: **keep the loss-cut as a protective give-up.** lc=2.0 (Tight) is the recommended
protection point; tighter still (arm1.0/gb0.3/lc1.5 → net +2874%, mdd −75%, 2022 **−39%**) is the
max-protection alternative if the operator wants the 2022 bleed cut further at ~150% less net.
BE-floor confirmed working — no armed leg books negative.

## Regime honesty (the 2022 bleed is real — shown, not hidden)

The market-regime proxy (equal-weight index of the 11 vs its own 200DMA) reads **bear-positive**
for BOTH variants — but that pools every sub-200DMA day, dominated by fast V-bounce risk-off
(Dec-2018, Mar-2020, Apr-2025) which a dip-buyer clips profitably. The **2022 calendar year**
(the canonical sustained bear) **bleeds for both variants** (T −82%, W −161%, WR ~34%, PF ~0.6).
Honest read: **this is a bull / fast-bounce-risk-off edge with a sustained-bear-grind
vulnerability.** Tight's protection materially cushions it (−82% vs W's −161%). A broad-market
bull gate (e.g. SPY>200DMA at trigger — equities, so the no-200DMA-in-crypto rule does not
apply) is the obvious follow-up lever to kill the 2022 bleed; not backtested here, flagged as
the next step.

## Recommended configs (GoldTrendMimicBook-style, per-name, cadence = DAILY)

```cpp
// VARIANT T — Tight (RECOMMENDED: best drawdown control, protection-first)
GoldTrendMimicBook::Config{
  .trigger_tag="StockDipMimicT", .live_sym=<per-name>,
  .legs={{"T",0.50}},            // gb=0.50 (keep 50% of peak, BE-floored)
  .arm_pct=2.0, .lc_pct=2.0,     // arm +2%, pre-arm loss-cut −2%
  .be_entry_pct=0.0,             // enter at trigger; protection = lc + BE-floor
  .cap_bars=10,                  // DAILY bars (matches StockDip 10-day hold)
  .rt_cost_bp=8.0, .notional=10000, .bull_only=false };
//   max-protection alt: arm=1.0 gb=0.30 lc=1.5  (net +2874%, mdd −75%, 2022 −39%)

// VARIANT W — Wide (books most gross; WEAKER protection — 880/3892 clips ride to
//             the 10-day cap, i.e. closer to "buy-dip-hold-10d" than a tight trail)
GoldTrendMimicBook::Config{
  .trigger_tag="StockDipMimicW", .live_sym=<per-name>,
  .legs={{"W",0.70}},            // gb=0.70 (keep 30% of peak)
  .arm_pct=3.0, .lc_pct=3.0,
  .be_entry_pct=0.0, .cap_bars=10,
  .rt_cost_bp=8.0, .notional=10000, .bull_only=false };
```

Wiring notes: StockDip is per-name, so wire one mimic book per name keyed `StockDip_<SYM>`,
fed that name's daily bars (registry `on_bar`) and triggered by that name's DIP open
(`on_trend_open`). Ship SHADOW with the same auto-retirement backstop StockDip uses.

## Caveats (do not over-read the +3000%)

1. **Selection bias**: the 11 names are StockDip's *in-sample all-6 winners* (mega-cap tech in
   a 2017-2026 secular bull). The edge is conditional on that curated universe — +3000% is NOT
   portable to arbitrary names. Judge it as "does this book add net-positive additive clips on
   the names we already trade," which it does.
2. **Daily-close grade** (same as StockDayMoverLadder / registry §6): managed on real daily
   O/H/L (touch detection honest) but same-bar arm+exit is capped at daily resolution and fills
   book AT the level (registry §5 model-fill optimism — real intraday fills penetrate the stop).
   Do NOT re-grade on ticks we don't have; the arm/lc/gb levels are the calibration.
3. **2022 sustained-bear bleed is real** (both variants) — accept it or add the market bull-gate.
4. Recommend LIVE only after a C++ engine-header parity run (registry §6 mandate: drive the
   wired book over the same entries, compare net/clips to this python harness before sizing).

**Bottom line:** both variants are **VIABLE standalone** on the wired universe; **Variant T
(arm2.0/gb0.5/lc2.0/cap10)** is the recommended ship — it books +3023% net / PF 1.79 / 52.7% WR
with the best protection (mdd −91%, worst clip −2.08%), all-6 pass, all-11 names positive. Variant
W books more gross (+4085%) but is under-protected (mdd −167%, 2022 −161%) and is effectively a
hold-to-cap book — ship it only if the operator wants the looser, higher-variance sleeve.

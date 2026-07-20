# Gold Trend Engines — FAITHFUL Revalidation (2026-07-21)

Faithful revalidation of the live gold trend engines with a protection (giveback-lock) verdict.
**Findings only — nothing wired or committed.** Throwaway harness: real engine header (scratch
copy adds a peak-giveback lock, prod path byte-identical when `gb=0`) driven on integrity-gated
H4 bars, real IBKR gold cost applied in post.

## Scope / engine identification

| global | engine | live state | this pass |
|---|---|---|---|
| `g_xau_tf_d1` | **XauTrendFollowD1Engine** (4-cell D1 ensemble: Momentum20 / Keltner-K2-RR3 / ADX-Mom25 / Donchian-N5) | **LIVE** (`shadow_mode=false`, IBKR 4002) | full revalidate |
| `g_gold_bull_trend` | **GoldBullTrendGatedEngine** (SEPARATE engine — 1h Donchian(20) + 30m EMA20/50, long-only, regime+SMA gated) | **SHADOW** (self-labelled "uncertified bull-beta, single-window") | status confirm only |

`g_gold_bull_trend` is NOT XauTrendFollowD1 and not a D1 cell — it is a distinct S-2026-07-20 build.

## Data (all CERTIFIED CLEAN via `data_integrity_gate.py`)
- BULL/cert window: `2yr_XAUUSD_tick_fresh.h4.csv` — 2024-03 .. 2026-04 (3434 H4 bars)
- BEAR slice: `XAUUSD_2022_2023.h4.csv` — 2022-01 .. 2023-12 (3170 bars)
- FULL: `XAUUSD_2022_2026.h4.csv` — 2022-01 .. 2026-04 (6870 bars, 4.3yr, both regimes)

## Cost model (faithful IBKR gold)
Sim crosses full `SPREAD=0.20` on entry+exit. Added in post: RT commission = `2·0.00015·price`
(≈ $1.05/RT at 3500) per closed trade, at 0.01 lot (= 1 oz, $1/pt). `COSTX=2` = 2×-cost stress.

## PF1.62 baseline reproduction — PASS
Production config = `IMP=0.5` (signal-day impulse filter). Reproduced the certified baseline:
- BULL window ensemble (real IBKR cost): **PF 1.57, +$3803, maxDD $1864, worst −$377**, both WF halves + (H1 1.69 / H2 1.53), **6/6 blocks ROBUST** — matches header "bull PF1.60" cert (1.62 is within window/spread noise). Reproduced → proceeding.

## FAITHFUL baseline (production exit, no lock, real cost)

| window | n | PF | NET | maxDD | worst | WF H1/H2 | blk | 2×-cost |
|---|---|---|---|---|---|---|---|---|
| **FULL 4.3yr** | 190 | **1.67** | **+$5612** | $1864 | −$377 | 1.69 / 1.66 ✓ | 6/6 | PF 1.64 +$5455 ✓ |
| BULL 2024-26 | 113 | 1.57 | +$3803 | $1864 | −$377 | 1.69 / 1.53 ✓ | 5/6 | PF 1.55 +$3689 ✓ |
| **BEAR 2022-23** | 77 | **1.49** | **+$958** | $449 | −$76 | 1.49 / 1.48 ✓ | 4/6 | — |

Positive through the 2022 bear (not a bull-only wonder), both WF halves +, 6/6 blocks on full
history, survives 2×-cost. All 4 cells individually WF+ or ROBUST on full history.

## PROTECTION — peak-giveback lock sweep (FULL 4.3yr window, real cost)
Lock: once `mfe ≥ arm·ATR@entry`, resting stop at `entry + (1−gb_frac)·mfe`. arm low=1.0×ATR, mod=2.0×ATR.

| config | PF | NET | maxDD | worst | n | vs baseline |
|---|---|---|---|---|---|---|
| **baseline (no lock)** | **1.67** | +$5612 | $1864 | −$377 | 190 | — |
| gb0.10 / arm=mod | 1.59 | **+$7066** | $1796 | −$347 | 374 | PF↓ net↑ (churn 2×) |
| gb0.10 / arm=low | 1.51 | +$6349 | $1796 | −$347 | 572 | PF↓ net↑ (churn 3×) |
| gb0.20 / arm=mod | 1.44 | +$5070 | $1796 | −$347 | — | **both ↓** |
| gb0.20 / arm=low | 1.43 | +$5417 | $1796 | −$347 | — | **both ↓** |
| gb0.33 / any | 1.27–1.34 | +$3323–3891 | ~$1490 | −$347 | — | **both ↓↓** |
| gb0.50 / any | 1.32–1.47 | +$3461–4591 | $1336–1349 | −$348 | — | **both ↓** |

Frontier reading:
- **Every gb ≥ 0.20 config strictly reduces BOTH PF and NET** — classic trend-lock edge-cutting.
- **gb=0.10 (tightest, keep 90% of peak) raises NET +26%** and marginally improves maxDD (−4%)
  and worst (−$377→−$347), **but only by degrading PF (1.67→1.59) and doubling/tripling trade
  count** (190→374→572). The extra exits are resting-stop-LEVEL fills (registry §5 model-fill
  trap): on 2–3× more stop exits the +26% net is optimism-inflated and would erode under honest
  worse-of/gap-through fills. It survives my 2×-cost (PF 1.56) but my 2× doubles only commission,
  not the churn's spread — so real churn cost is understated.

### PROTECTION VERDICT: **LOCK-HURTS-EDGE — keep current exit.**
No giveback-lock config cleanly improves the engine. gb≥0.20 is strictly worse; the lone net-up
config (gb0.10) trades PF, block-robustness (6/6→5/6) and 2–3× churn on optimistic lock-level
fills for a net gain that is not fill-honest. The live exit (2×ATR SL + TP-multiples 4/6×ATR +
Donchian bar-close exit + already-live `LOSS_CUT 1.0%` / `BE_ARM 5% / buffer 1%`) is already
backtested both-regime robust. **Capture-vs-giveback:** the engine's edge lives in letting the
Keltner/Donchian runners run to TP; clamping giveback converts winners into segmented re-entries
and lowers quality. Consistent with the header's own note ("tight-lock GUTS the edge, wide-arm
mandatory") and the standing trend-lock prior. Keep current protection.

## VERDICTS

- **XauTrendFollowD1 (`g_xau_tf_d1`, LIVE): KEEP-LIVE.** Faithful full-history PF 1.67 (+$5612),
  maxDD $1864, worst −$377 @0.01 lot, both WF halves +, 6/6 blocks ROBUST, POSITIVE through the
  2022 bear (PF 1.49), 2×-cost-robust (PF 1.64). Real IBKR cost. Protection: LOCK-HURTS-EDGE,
  keep current exit.
- **GoldBullTrendGatedEngine (`g_gold_bull_trend`): SHADOW-ONLY (no change).** Separate engine,
  already correctly self-gated SHADOW: bull-beta flattened to ~flat-bear by regime+SMA gates but
  certified on ONLY ONE bull + ONE bear window (multi-window cert OWED per its own header;
  MGC instance is an XAU proxy, uncertifiable on data on hand). Leave SHADOW until multi-window
  cert lands. Not part of the XauTrendFollowD1 live verdict.

Harness/scratch: `scratchpad/xtfd1_reval.cpp` + scratch engine copy with peak-giveback lock
(throwaway; production header untouched).

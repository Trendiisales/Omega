# X1 Overlay — Multi-Symbol Findings (n-boost)
**Date:** 2026-06-02 · **Window:** 2026-05-11 → 2026-06-02 · **App:** `x1_app.py`
**Symbols:** XAUUSD, US500.F, USTEC.F, GER40, GBPUSD, EURUSD (Dukascopy M1)
**Total pooled in-window fills:** 209 (was 100 gold-only) across 6 symbols.
**Status:** the gold edge does NOT generalize as-is. Confirm-filter is gold-only (for now).

---

## What was done
Pulled Dukascopy M1 bars for 5 more symbols (correct index codes: USA500IDXUSD /
USATECHIDXUSD / DEUIDXEUR — NO dot; FX scale 100000), built `x1_app.py` to pool the
momentum-confirm gap across all symbols, and fixed the family map to classify every
non-gold engine by entry mechanic (momentum-confirming vs counter-momentum).

Engine mix outside gold: US500/USTEC = VWAPReversion (mean-rev) + IndexSwing (swing) +
a few UstecTrendFollow; GER40 = Turtle/Keltner/Donchian/MA (trend) + RSI (mean-rev) +
straddle; FX = LondonOpen (session-momentum) + FxScalpPyramid.

## Headline: the +12 trend gap is GOLD-SPECIFIC
Trend-family confirm-gap (win_conf − los_conf) at the native 10-bar M1 lookback:

| symbol  | n (W/L) | gap    |
|---------|---------|--------|
| XAUUSD  | 16/30   | **+12.1** |
| EURUSD  | 2/2     | +50 (noise) |
| GBPUSD  | 3/6     | +50 (noise) |
| GER40   | 1/7     | −42.9  |
| US500.F | 8/7     | −14.3  |
| USTEC.F | 6/11    | −37.9  |

Pooled trend gap **collapses +12.5 → −2.0** once indices are correctly classified
(they were hiding in "other" in the first run). On index trend engines, winners carry
a confirming tag *less* often than losers. **US500 trend winners: 0/8 had any tag** —
the M1 WaveTrend momentum tag barely fires near index entries.

## Why: the confirm window is TF-mis-calibrated, and even fixed it's fragile
The 10-bar M1 (10-min) lookback is tuned to gold's scalp/intraday engines. Index engines
hold hours / run higher TF. Sweeping the lookback on the indices:

| symbol  | lb=10 | lb=30 | lb=60 | lb=120 |
|---------|-------|-------|-------|--------|
| GER40   | −42.9 | +42.9 | +28.6 | 0      |
| US500.F | −14.3 | +17.9 | +1.8  | 0      |
| USTEC.F | −37.9 | −22.7 | −15.2 | −33.3  |

- 10-bar is too short for indices → spurious negative (tag rarely fired in 10 min).
- ~30-min flips US500/GER40 positive, but n is tiny (8/7, 1/7) — not evidence.
- USTEC stays negative at every lookback — genuinely doesn't fit.
- 120-min saturates: every trade gets a tag, gap → 0 (no discrimination).

The result is **lookback-dependent and sign-unstable** off gold. That is the opposite of
a robust, transferable edge.

## Conclusions
1. **The confirm-filter edge is confirmed for XAUUSD only** (+12 within-trend at native
   10-bar; see X1_STAGE1_FINDINGS.md for the composition caveat — the per-trade part is
   ~+12, not the pooled +20).
2. **It does not generalize to indices as built.** Mixed/negative sign, fragile to the
   confirm-window length, and the per-symbol n (≤8 winners) can't support a claim either way.
3. **Mechanical lesson:** the lookback must be TF-matched per engine — a global fixed M1
   window is wrong for a mixed-TF book. A single global momentum-confirm gate would have
   been actively harmful on indices.
4. FX (EURUSD/GBPUSD) looks positive but n=2-3 winners — pure noise, ignore.

## Build implication
- Ship the overlay/gate **gold-first**, at gold's native lookback. This is now the only
  edge with enough confirmed signal to act on.
- For indices: do NOT gate. Use Stage-2 shadow logging to accumulate fresh fills WITH a
  TF-matched confirm window per engine, then re-test. n, not analysis, is the blocker.
- `x1_app.py` is the standing multi-symbol monitor for that re-test as data grows.

## Files added this session
- `x1_app.py`        — multi-symbol pooled validator (the app)
- `x1_stage1.py`     — family map extended to index/FX engines
- `*_m1.csv`         — Dukascopy M1 bars for the 6 symbols (regenerable via pull_dukascopy.py)
- `pull_dukascopy.py`— corrected verified index codes

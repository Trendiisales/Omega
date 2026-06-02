# SESSION_HANDOFF — X1-style Overlay Validation
**Date:** 2026-06-02
**Slug:** incidents/2026-06-02-x1-overlay-validation/
**Read this first on continuation.**

---

## What this workstream is
Build and validate an X1-Algo-style overlay (WaveTrend oscillator + momentum /
retracement chart tags) on Omega's own data, as a read-only tool for checking
trades — then, if it earns it, integrate it into Omega.

Origin: Jo shared screenshots of the retail "X1 Algo" TradingView indicator.
Decoded: it is a WaveTrend oscillator (LazyBear) + a momentum regime tag + a
counter-trend retracement tag + bar coloring + an "X1 Flash" reversal
oscillator (the second component is NOT yet built — see Backlog).

## Current state (DONE)
- `x1_validate.py` — complete, symbol/source-agnostic offline validation engine.
  Aggregates ticks→bars, computes WaveTrend + 4 non-repainting tag families,
  forward-return validation, real-fill overlay, renders chart. Smoke-tested on
  synthetic data, then run on real data.
- `pull_dukascopy.py` — reusable Dukascopy tick downloader → OHLC bars.
- Real run completed: XAUUSD, 22,223 M1 bars, 2026-05-11 → 2026-06-02, overlaid
  against 100 of 109 real XAUUSD fills from `omega_trade_closes.csv`.

## Status of the result: SUGGESTIVE, NOT VALIDATED
The headline signal is real but at ~2 standard errors — promising, not proven.
Do not promote to anything live without the Stage-1 refinement below.

## Key findings (see X1_OVERLAY_FINDINGS.md for full numbers)
1. Momentum tags are NOT predictive standalone (hit 46–48%, wrong-sign mean).
2. Retracement tags show a mild mean-reversion tilt (53–57% in the expected
   direction) but economically marginal at M1 (~1–2 bps vs ~$0.30 spread).
3. THE USEFUL ONE: momentum tag as an entry FILTER separates winners from
   losers — 71.9% of winning trades vs 51.5% of losing trades had a confirming
   momentum tag in the 10 bars before entry.

## Caveats (must address before integration)
- Trend-following engines (Tsmom, DonchianBreakout, EmaPullback) bake in
  momentum-before-entry; the winners>losers gap is the real part, not the level.
- Significance is ~2 SE (32 winners; n≈232 retracement events). Needs more data.
- Timeframe mismatch: forward test uses 5–20 min horizons, but many trades hold
  hours. M1 test likely UNDERSTATES the real edge.

## Data provenance (reproducible — data NOT committed to repo)
M1 bars regenerated with:
```
python3 pull_dukascopy.py --symbol XAUUSD --from 2026-05-11 --to 2026-06-02 \
    --tf 1 --out XAUUSD_2026-05_m1.csv
```
Dukascopy notes: path month is ZERO-indexed (May="04"); .bi5 = LZMA1, 20-byte
`>iiiff` records; XAUUSD scale = points/1000. 371/552 hours had data (rest =
weekends/closed). Decoded prices cross-checked against real fill levels (~4720).

## How to re-run the validation
```
python3 x1_validate.py --bars XAUUSD_2026-05_m1.csv --symbol XAUUSD \
    --trades omega_trade_closes.csv --out xau_may_real.png
```

## NEXT STEPS — agreed direction: integrate + refine
See INTEGRATION_PLAN section in X1_OVERLAY_FINDINGS.md. Staged & gated:
- Stage 1 (refine, offline): per-engine breakdown; horizons matched to hold
  time; more symbols; more/longer data to push significance past noise.
- Stage 2 (shadow-only integration): read-only module that LOGS WaveTrend +
  momentum state at each signal into the trade log (new columns). No gating.
  Accumulates forward out-of-sample confirmation on live fills.
- Stage 3 (GUI overlay): render oscillator + tags on the Omega GUI (7779/7780).
- Stage 4 (filter gate): ONLY if Stage 2 confirms on fresh fills — config-flagged,
  shadow-first, full pre-delivery checklist, per-engine.

## CHANGE-CONTROL STATUS
Nothing in this workstream has touched core/engine code. All artifacts are
research tooling. Integration (Stage 2+) requires explicit per-file work and the
mandatory Omega pre-delivery process. VPS untouched.

## Open thread
- Repo commit of this handoff was BLOCKED: PAT in notes returned 401
  (expired/revoked). Needs a fresh PAT to push to incidents/. Files delivered
  via chat as a fallback.

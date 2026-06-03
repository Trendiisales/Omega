# Viable new gold engine: early-week (Mon+Tue) long seasonality (2026-06-03)
After intraday price/oscillator/microstructure all tested dead, the CALENDAR axis
shows a real gold edge. 2yr daily XAUUSD open->close return by weekday:

FULL: Mon +0.26% win61% t2.27 | Tue +0.24% win62% t1.90 | Wed +0.16% t1.41 | Thu/Fri/Sun flat
Walk-forward (split halves) -- Mon+Tue COMBINED long:
  H1 (older):  +0.14%/day win 62% t1.57  sum +15.1%
  H2 (OOS):    +0.36%/day win 62% t2.49  sum +40.8%
  FULL:        +0.25%/day win 62% t2.93  sum +55.9%
Both halves positive, win 62% in BOTH = robust. Individual Mon vs Tue strength
ROTATES between halves (data-mine risk on single day) -> trade Mon+Tue COMBINED.
Gross +0.25%/day vs ~0.01% cost -> strongly cost-positive. Different axis (calendar)
= survives where price/book signals die.

## Recommendation: build GoldSeasonalEngine (mirror IndexSeasonalEngine)
Long gold at Mon+Tue session open, flat at session close, VIX/risk-off gated (like
IndexSeasonal Tue/Fri indices). Validate full: cost-modelled backtest + DSR, shadow
first. This is the one viable NEW gold engine found across the deep dives.

## DEEP VALIDATION (2026-06-03) — PASSES, build-worthy
Cost-modelled sim (2yr daily, open->close, round-trip cost):
  Mon+Tue     : net +24%/yr Sharpe 1.84 win 61% maxDD 9.8%
  Mon+Tue+Wed : net +31%/yr Sharpe 2.00 win 59% maxDD 10.3%
Robustness:
  - per-year POSITIVE every year: 2024 +5.1%, 2025 +24.3%, 2026 +22.0%
  - both WF halves positive (win 62% each)
  - cost-robust: 5x cost (0.10%) still +15.6%/yr Sharpe 1.20
  - DSR: t=2.70, haircut ~1 for 6 day-trials -> t~1.70 > 1.6 (survives)
VERDICT: build GoldSeasonalEngine. Mon+Tue (clean, t>1.9 both days, Sharpe 1.84) or
Mon+Tue+Wed (Sharpe 2.0, Wed weaker alone t1.4 but lifts portfolio). Recommend Mon+Tue
core. Mirror IndexSeasonalEngine: long XAUUSD at session open Mon/Tue, flat at close,
risk gate. Warm-seed + shadow + heartbeat. Strongest new edge from the deep-dives.

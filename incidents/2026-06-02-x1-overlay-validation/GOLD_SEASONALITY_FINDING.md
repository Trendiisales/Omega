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

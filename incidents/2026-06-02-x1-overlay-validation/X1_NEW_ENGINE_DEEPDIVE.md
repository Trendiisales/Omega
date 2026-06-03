# Deep dive: what NEW engines does the WaveTrend/momentum data unlock?
Date: 2026-06-03. Q: can the new dashboard data (WaveTrend, momentum, MTF) drive a
new standalone engine (e.g. a bracket/OCO that reads the data)?

## Verdict: NO new standalone engine — the data is a GATE, not a signal.
Every standalone use tests net-negative on 2yr gold (cost 0.37):

| candidate | result | PF |
|---|---|---|
| WaveTrend directional cross (+regime) | net -2117..-18 | 0.83-1.00 |
| EMA9xEMA21 + RSI50 ("BUY" arrows) | net -205..-603 | 0.93-0.98 |
| WaveTrend OB/OS reversal (retracement) | ~1-2bps, sub-spread | <1 (prior) |
| momentum tags standalone | ~47% hit (X1 validation) | no edge |

## Bracket / OCO "drop the unfilled leg" idea = ALREADY EXISTS
That is XauStraddle (OCO box breakout, cancels other leg) + the Bracket engines.
XauStraddle is net+ (PF 1.65, +partial 30%@0.7R). Bracketing on WaveTrend-extreme
instead of the box is just a re-timed straddle; box-breakout already captures the
vol-expansion. Trend-biasing the straddle HURTS (-33% net, tested).

## Where the WaveTrend data DOES add value (deployed)
- Momentum-confirm GATE on XauTrendFollow 1h/4h/D1 (+12pp within-trend, live shadow).
- Dashboard situational awareness (regime/RSI/MTF/screener).

## Conclusion
No profitable new engine hides in this oscillator data — it is confirm/context only.
New-engine value lives in NEW data (L2 microstructure, FX cohort — see roadmap),
not the WaveTrend oscillator. This deep dive prevented building 2-3 net-negative
engines. Harnesses: backtest/wavetrend_directional_bt.cpp, emacross_signal_bt.cpp.

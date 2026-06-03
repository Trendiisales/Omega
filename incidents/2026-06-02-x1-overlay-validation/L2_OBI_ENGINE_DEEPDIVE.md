# Deep dive: L2 OBI as a tradeable engine — verdict NO (2026-06-03)
Re-tested on CLEAN good-depth data (post bridge-fix): ibkr_l2_XAUUSD 2026-06-01/02/03,
50.5 hrs, 667,755 rows, 100% non-zero depth/imbalance.

## OBI directional scalp (l2_obi_replay) — DEAD on clean data
Every horizon/threshold loses; win rate 33-46% (consistently <50%):
  TH0.15 H3000 : tr23144 PF0.77 win40%
  TH0.20 H1000 : tr28513 PF0.63 win33%
  TH0.20 H5000 : tr12094 PF0.82 win44%
  TH0.25 H10000: tr3974  PF0.84 win46%
OBI does NOT predict next-move profitably. Old "54-56% tilt" does not translate to
tradeable edge. Following OBI wins <50% (slightly contrarian/absorption), but moves
are sub-spread so fading is cost-dead too.

## OBI overlay-on-straddle (l2_obi_overlay_test) — UNVALIDATABLE (too few trades)
3 good days = only 5 straddle trades (1 DISAGREE lost -30, 4 NEUTRAL won +28). n=1
disagree = anecdote, not evidence. With no per-move edge underneath, overlay unlikely
to help. Need weeks of good-depth capture to even attempt; not worth it given the
negative per-move result.

## Conclusion: do NOT wire any L2 engine.
Signal exists (book imbalance is real) but has no tradeable edge on clean data —
same outcome as the WaveTrend deep-dive. Supersedes the cost-dead framing in
[[omega-l2-obi-overlay-not-scalp]]: it's not just cost, the directional edge is
absent on clean data. New-engine value is NOT in OBI. Harnesses: l2_obi_replay.cpp,
l2_obi_overlay_test.cpp. Good-depth L2 capture continues live (no action needed).

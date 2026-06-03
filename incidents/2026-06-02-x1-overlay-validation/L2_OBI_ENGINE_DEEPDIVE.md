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

## UPDATE: different L2 feature classes also dead (2026-06-03)
Feature information-edge study (backtest/l2_feature_edge_study.py) on the same 668k
clean rows, forward mid move at 2s/5s:
  imb_level   : 49.7/49.9% directional hit (coin flip)
  imb_flow    : 49.4/49.5% (the change-not-level idea ALSO dead)
  lvl_imb     : 48.3/48.8% (no / slight inverse)
  events->|move| corr -0.01 ; tot_vol->|move| -0.01 (bursts don't predict expansion)
  spread->|move| corr +0.08/+0.07  <- ONLY signal: wider spread predicts bigger move,
    but MAGNITUDE not direction, weak, and exploiting = trading when cost highest.
Every directional feature ~50% on 668k samples. Book efficient at these horizons.
True queue/iceberg/sweep need RICHER capture (per-level ladder sizes + trade prints)
which we do NOT log -- that's an infra project before any edge is testable. VERDICT:
stop L2 engine-hunting on current top-of-book capture. No engine.

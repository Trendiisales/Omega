# Signal Discovery -- NEGATIVE RESULT

_Generated: 2026-04-29T11:24:01.598769+00:00_

## Verdict

None of the four candidate setups (compression_break, spike_reverse, momentum_pullback, level_retest_reject) cleared all three gates (t-stat > 2.0 on a 30s-5min horizon, freq >= 5.0/day, rr-ratio > 1.2 after a 0.65pt cost).

## What this means

Per the night-handoff and wrap-handoff Step 6 directive, the build directive is to **STOP, surface, and not build the engine on weak edge.**

## Recommended next probes

1. **Tighter setup definitions.** Try narrower thresholds (e.g. compress<0.30, spike z>4, mom24>1.5) -- accept lower frequency for higher t-stat.
2. **Session restriction.** Filter to UTC 07:00-16:00 (London/NY overlap) and re-evaluate; off-hour noise may dilute the in-session edge.
3. **OFI / order-book features.** L2 ticks were not present in this corpus (L1 only).  If a Level-2 feed becomes available the OFI setup is worth re-examining.
4. **Multi-bar confirmation.** Require 2-3 bar pattern confirmation rather than single-bar entry; reduces false positives at small cost in frequency.
5. **Ensemble.** Combine setups using weighted vote -- individual edges may not clear gates but a combined signal could.

## Per-setup near-miss summary

| setup | n | freq/day | best short-horiz t-stat | best rr |
|---|---:|---:|---:|---:|
| compression_break | 18,248 | 49.99 | -26.15 | 0.51 |
| spike_reverse | 11,014 | 30.18 | -14.39 | 0.61 |
| momentum_pullback | 523,156 | 1433.30 | -107.25 | 0.59 |
| level_retest_reject | 464,849 | 1273.56 | -83.79 | 0.65 |
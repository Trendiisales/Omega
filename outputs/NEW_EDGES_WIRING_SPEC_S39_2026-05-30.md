# New-Edges Wiring Spec — S39 2026-05-30

From the walk-forward edge pipeline (EDGE_PIPELINE_S39_TRUSTED_SYSTEM). What is
already live, and the faithful build plan for what is not.

## ALREADY LIVE in shadow (no action — confirmed in binary 1b33d11)
| edge | where | status |
|---|---|---|
| XAU 4h Donchian N=20 | XauTrendFollow4h cell 0 (mask 0x49 bit 0) | enabled, shadow |
| XAU 4h Keltner | XauTrendFollow4h cell 3 (bit 3) | enabled, shadow |
| XAU 1h Donchian (long, signal-exit) | XauTrendFollow1h cell 1 (mask 0x03) | enabled, shadow |
| XAU 1h EmaCross 20/80 | XauTrendFollow1h cell 0 | enabled, shadow |

The XAU Donchian complex — the strongest, most-stress-tested edge — is wholly
wired. XauTrendFollow1h fired 0 trades 05-11..29 NOT from a bug: it is long-only
and gold fell that window (~$4670→$4543), so a long-breakout engine correctly
sat out. The signal-based Donchian exit IS the "wide bracket" the harness liked.

## NOT wired — faithful build plan (do NOT rush; per Engine Warm-Seed Mandate)

### Fidelity caveat (read first)
`edge_pipeline` uses a SIMPLIFIED model: fixed ATR brackets, both-sides, bar
touch-fills. Production engines use signal/channel exits, often long-only, with
spread/ATR/cost gates. The 1h case proved the two differ. **Before wiring, each
candidate MUST be re-validated through the PRODUCTION engine class** (feedback-
cpp-first + feedback-harness-fidelity) — not promoted on the simplified sweep.

### Edge 1 — GER40 4h RSI-extreme (N=7, 30/70)  [#2 edge; both-sides robust 6/6/5/6]
- Reuse path: NOT clean — `RSIReversalEngine` is XAU-hardcoded (`tr.symbol=
  "XAUUSD"`). `Us30EnsembleEngine::_sig_rsi_extreme_h1()` already implements the
  RSI-extreme signal for an index → best template.
- Build: add a GER40 ensemble (mirror Us30Ensemble) OR generalize RSIReversal
  to multi-symbol. RSI(7), enter long on cross-up through 30, short on cross-
  down through 70 (the engine is symmetric; the harness shows both sides work).
- Warm-seed: GER40 H4 CSV (phase1/signal_discovery/warmup_GER40_H4.csv exists).
- CAVEAT: narrow param ridge (only N=7 solid; N=9→5/6, N=10→4/6) → fragile.
  Ship shadow, small size, watch for decay.

### Edge 2 — NSX/USTEC 4h Vol-Expansion breakout (range>2×ATR → trade direction) [tier-2, 5/6]
- New family (no existing engine). Closest base: BreakoutEngine.hpp.
- Build: on H4 close, if (high-low) > 2.0×ATR14, enter in the bar's direction;
  bracket sl1.5/tp3 ATR. Warm-seed: USTEC/NSX H4 CSV.
- CAVEAT: 5/6 (one losing block), thinner (n=123). Lowest priority.

### Edge 3 — GER40 1h Donchian N=15  [tier-2, 5/6]
- Could be a cell in a GER40 trend engine. Ger40TurtleH4 is H4 Donchian-style;
  a 1h variant needs its own bar aggregator + warm-seed (GER40 H1 CSV).
- CAVEAT: 5/6, DAX H1. Lower priority than Edge 1.

## Mandatory checklist per new engine (CLAUDE.md Engine Warm-Seed Mandate)
1. Engine class with `bool enabled` + `shadow_mode` (default shadow).
2. Warm-seed path (seed_from_*_csv or seed_h4_engine) + bundled CSV. Boot log
   MUST show one `[SEED]` line — absence = P1 (engine silent for days).
3. Dispatch wiring at the symbol's tick/bar call site.
4. `register_engine` + heartbeat registration.
5. `scripts/mac_canary_engines.sh` green + OmegaBacktest build green (MSVC-class
   errors slip past clang — the 2026-05-30 deploy lesson).
6. Deploy → verify `[SEED]` line + Git-hash match, mode stays SHADOW.
7. Accumulate ≥30 shadow trades → run `deflated_sharpe_gate.py` → promote only
   on the gate.

## Recommended order
Edge 1 (GER40 RSI, #2 edge) first as the template — one engine, done to the full
standard, validated via production class, shadow-deployed. Then Edge 3, then
Edge 2. One per session; no multi-engine big-bang (the documented failure mode).

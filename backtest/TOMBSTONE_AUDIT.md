# TOMBSTONE_AUDIT.md — which engines were killed on a TRUSTWORTHY number

Per BACKTEST_TRUTH.md. Classifies tombstoned engines by **kill-basis quality**. The point:
re-check the ones killed on bad/optimistic numbers; leave the faithfully-killed ones dead.
Stops both failure modes (wrongly-killed winners + wrongly-resurfaced losers).

## Basis classes
- **POLLUTED** — killed on a number we now know was corrupted (ledger artifact: 100× lot,
  phantom hold, ×1000 data glitch). **RE-CHECK on faithful harness + clean ledger.** Highest priority.
- **BAR-REPLAY-ONLY** — killed (or kept) on a bar-replay backtest, never confirmed faithfully.
  Suspect in BOTH directions (overstated → maybe never worked; or sweep mis-ran → maybe killed
  a winner). Re-check via the faithful arbiter before trusting.
- **FAITHFUL/LEDGER-CONFIRMED** — killed on the engine-faithful tick BT or a clean live ledger.
  **Stays dead.** Do not re-mine.
- **OPERATOR-ORDER** — operator killed it on a strategy call (e.g. "big caps only", "winners
  only"). Stays dead unless the operator revisits; not a measurement question.

---

## RE-CHECK CANDIDATES (killed on POLLUTED / unconfirmed numbers)

### ⚠️ The 2026-06-15 "6mo shadow-book BT" cull batch — POLLUTED basis, RE-AUDIT OWED
Memory `omega-tombstone-ledger-pollution`: this whole batch shared a polluted-ledger basis
(pre-fix lot=1.0 → 100× on metals + 53-day phantom holds). **GoldOrbRetrace from this same
batch was already un-tombstoned at PF2.38; NasOrb retuned to PF1.88.** The rest were never
re-checked — they are the prime wrongly-killed-winner suspects:

| engine | killed on | status | action |
|---|---|---|---|
| `g_gold_oversold` | "6mo shadow-book BT −$457" | enabled=false | **RE-CHECK** faithful + clean ledger |
| `g_xau_threebar_30m` | "6mo shadow-book BT −$371" | enabled=false | **RE-CHECK** |
| `g_ema_pullback` (EpbPortfolio) | "6mo shadow-book BT −$275" | enabled=false | **RE-CHECK** (note: edge may live as a CELL in XauTrendFollow1h, +$6,202 — verify standalone-vs-cell before resurrect) |
| `g_xau_straddle_m15` | "6mo shadow-book BT −$559" | enabled=false | **RE-CHECK** (caveat: M30 sibling +$996 is the known survivor; fast-TF may genuinely be worse) |
| `g_donchian` (DonchianPortfolio) | "6mo shadow-book BT −$186" | enabled=false | **RE-CHECK** (note: vol-targeted Donchian edge claimed to live inside XauTrendFollow1h — verify) |

→ Re-run each on the faithful arbiter (clone `faithful_engine_bt_TEMPLATE.cpp`) over a CLEAN
   ledger window (artifact-filtered) before accepting the kill. GoldOrb proves at least one
   winner was in this batch.

#### ✅ RE-CHECK DONE 2026-06-16 (ledger + faithful cross-regime, `gold_xregime_recheck.cpp`)
Result: **the 06-15 batch was NOT uniformly wrong, but none of the 5 is a robust winner — all 5 stay TOMBSTONED.** GoldOrb was the real wrongful-kill; these are not.

| engine | clean live ledger | faithful x-regime (bull / crash / down) | verdict |
|---|---|---|---|
| GoldOversoldBounce | +46 (bull-only window) | **+419 / −455 / +45** | **bull-beta dip-buyer — dies in the crash; cull DEFENSIBLE.** Live +46 was bull-only and misleading. STAY DEAD. **Can't regime-gate it:** gate ON → 0 trades (blocks its own oversold dips — gate ⊥ mean-rev); gate OFF → keeps the −455 crash. +419 was n=5/22mo = not an edge anyway. |
| EmaPullback | +9/n6 (bull) | 0 trades standalone (equity-sizing + risk-off gates need live env); LONG-only = bull-beta by construction | STAY DEAD |
| Donchian | ~−100/n29 (mild neg) | 0 trades standalone (gated) | STAY DEAD (no winner signal) |
| Xau3Bar30m | NO live trades | 0 trades standalone (protection-stack + HMM gate) | UNEVALUABLE — stays dead |
| XauStraddleM15 | NO live trades | 0 trades standalone | UNEVALUABLE — stays dead |

**Lesson:** ledger-only nearly resurrected GoldOversold (+46 bull window). The crash block flipped it.
Cross-regime (incl. the 2026-03 −12.6% gold crash) is mandatory; a bull-only positive is not edge.
The 4 zero-trade engines need live-env gate neutralization (g_live_equity / RegimeState / protection
stack) to fire standalone — not worth it given ledger + structure already show no winner.

### Older sweep-based culls — BAR-REPLAY-ONLY, lower priority
| engine | killed on | action |
|---|---|---|
| `IndexHybridBracket` (×4), `GoldHybridBracket` | S11 P3b 2026-05-07 walk-forward sweep | re-check IF revisited — bar-replay basis, never faithfully confirmed |
| `LatencyEdgeStack` | S13 2026-04-24 | bar-replay/finding cull — low priority |
| various S52 (2026-05-01) trade-quality culls | walk-forward | basis unverified — check before any re-mine |

---

## STAY DEAD (FAITHFUL / LEDGER-CONFIRMED or OPERATOR-ORDER)

| engine | basis | verdict |
|---|---|---|
| `g_fvgcont_nas` (FvgContinuation) | **FAITHFUL** — engine-faithful tick BT net-neg all regimes (2026-06-16, fvg_engine_bt.cpp); bar-replay PF1.65/1.25 were artifacts | **STAY DEAD** |
| `g_xauusd_fvg` (XauusdFvg) | slice artifact + same FVG faithful finding | STAY DEAD |
| `g_eurusd_turtle_h4` | **FAITHFUL** cross-regime PF0.88, regime-luck (2026-06-16) | STAY DEAD |
| `g_gbpusd_turtle_h4` | **FAITHFUL** cross-regime PF0.88 (2026-06-16) | STAY DEAD |
| `IndexSwing` | **LIVE LEDGER** net-negative (2026-06-02) | STAY DEAD |
| `PumpScalp` variants, `C1Retuned`, mean-rev book | **OPERATOR-ORDER** (big-caps-only / winners-only) | operator call |
| SqueezeSlingshot | ~~bar-replay borderline~~ → **FAITHFUL-CONFIRMED DEAD 2026-06-18**: squeeze_xregime_nas.cpp IS tick-driven on the real SqueezeSlingshotEngine (NOT bar-replay — label was wrong). 5 variants all bear-negative, none both-WF-halves, cost-fragile; best (H1_strictC PF1.08) bull/H2-concentrated. Path-B predicate (squeeze_predicate.cpp): fire→fwd-move 1.03× baseline NAS / 1.13× SPX = no vol-gate edge, direction coinflip; only tier≥2 thin (n24). | **confirmed faithfully — STAYS DEAD, do not re-mine** |

---

## How to action a RE-CHECK (cheap, ~minutes)
1. `tools/analytics/ledger_analytics.py --since <clean-window>` — read the live shadow ledger
   first, artifact-filtered. Often ends it without a backtest (BACKTEST_TRUTH.md §1).
2. If no/insufficient ledger: clone `faithful_engine_bt_TEMPLATE.cpp` for the engine, run over
   cross-regime m1 blocks. net+ AND both-halves+ in EVERY block = real; else stays dead.
3. Record the result + basis-class in this file. Never re-kill/re-resurrect on bar-replay alone.

_Last updated 2026-06-16. Audit is grep-level (engine_init kill-comments + memory); basis marked
"unverified" where the kill-comment did not state a faithful/ledger source — verify before acting._

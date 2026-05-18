# Session Handoff — 2026-05-18 part B (NZST)

Read this first next session. Direct follow-up to part-A
(`SESSION_HANDOFF_2026-05-18a.md`). The big finding of this session
is structural, not local: **the validated GSP $15K backtest is a
bar-level-harness artifact and does not survive tick-level execution.**
That changes how every engine in this codebase should be validated.

## TL;DR

1. **Three bug fixes shipped (S99h, S99j):** 100x tick-value double-
   application in GSP/QSC/BBS close PnL, and missing `tr.spreadAtEntry`
   population in GSP/QSC. Live VPS now displays correctly-scaled
   shadow trades. See git log for details.

2. **GSP disabled live (S100c):** `g_gold_scalp_pyramid.enabled =
   false`. The validated edge does not transfer to tick-level
   execution; the shadow ledger entries were misleading. Shadow stays
   true so any accidental re-enable doesn't go straight to broker.

3. **New engine GoldSessionBreakoutEngine.hpp built + swept (S100):**
   Diagnostic-grounded variant of GSP with three new filters
   (H4-trend gate, NY-only session, ATR-of-ATR squeeze) + pyramid.
   Sweep showed every config negative. NOT the engine's fault — same
   shape that produced GSP -$16,935 in the class-direct audit. Engine
   correctly built and verified bug-free; just doesn't have edge with
   trail philosophy this tight.

4. **The deep finding — bar-level vs tick-level exit gap:**
   - GSP harness (`gold_scalp_pyramid_bt.cpp`) is inline-bar-level.
     Reports +$15,084 / PF 1.45 / avgWin $12.58 / 72 TP_HITs.
   - GSP class direct (`gsp_s63_audit_bt.cpp` config A:S63-OFF) is
     tick-level. Reports -$16,935 / PF 0.62 / avgWin $5.54 / **0
     TP_HITs**.
   - Same parameters, same tape, same code path conceptually.
   - The harness assumes trail captures bar's intra-bar peak; tick-
     level can only see ticks chronologically.
   - $32K delta across 5436-7110 trades on 154M ticks.
   - The "$3,998 from 72 TP_HITs" is pure paper money — tick-level
     trades never reach TP because the trail intervenes first.

5. **Implication for every engine in this repo:** any backtest from
   an inline-implementation harness (htf_bt_minimal, older
   honest_backtest variants, anything that re-implements engine logic
   rather than driving the production class) needs re-validation
   tick-level. The S63 audit harness is the template — drive the
   actual class, get the honest number.

## Commits this session

| Commit | Message | Status |
|---|---|---|
| `2b6ae51` | S99h: fix 100x tick-value double-application in GSP+QSC close pnl | ✓ pushed |
| `68c4203` | S99i: BBandScalpEngine + sweep (NO EDGE, enabled=false) | ✓ pushed |
| `<S99j hash>` | S99j: populate tr.spreadAtEntry in GSP+QSC for cost pipeline | ✓ pushed (push after S99h-cleanup amend) |
| `<S100 hash>` | S100: GoldSessionBreakoutEngine + sweep harness | working tree -- commit before session close |
| `<S100b hash>` | S100b: GSP S63 audit harness + GSB harness S63-off fix | working tree -- commit before session close |
| `<S100c hash>` | S100c: disable g_gold_scalp_pyramid live (bar-vs-tick edge gap) | THIS HANDOFF + engine_init.hpp edit |

## The bar-vs-tick gap, in numbers

| Metric | GSP harness (paper) | GSP class tick-level (real) | Delta |
|---|---|---|---|
| Trade count | 5436 | 7110 | +31% (tick processes more bars/signals) |
| Win rate | 71.0% | 71.0% | 0 (entries identical) |
| Avg win | $12.58 | $5.54 | **-56%** |
| Avg loss | -$21.25 | -$21.78 | ~same (SL geometry holds) |
| TP_HIT count | 72 | **0** | **harness paper money** |
| TRAIL_HIT | 3765 (most wins) | 5071 | tick-level fires trail earlier |
| SL_HIT | 1529 | 1962 | tick-level misses trail more often |
| Total PnL | **+$15,084** | **-$16,935** | **$32,019 gap** |
| PF | 1.45 | 0.62 | inverted |

Smoking gun: same configuration, same code intent, $32K gap entirely
from exit-resolution implementation difference.

## What did NOT land this session

- The exit-philosophy sweep itself (wider trail / bar-close trail /
  give-back trail). Scoped + understood, not built.
- A tick-level-validated tradeable engine. We have shadow-disabled
  engines and a clear research direction, no shipping product.

## Recommended next-session focus

**Single primary goal: build the GSP exit-philosophy variants sweep
and find out whether ANY exit philosophy recovers $5-8K of tradeable
edge in tick-level execution.**

The plan:

1. Refactor `GoldScalpPyramidEngine` to make exit philosophy a runtime
   parameter (current default = "tick"; add "bar_close" and
   "give_back" modes). This is a small refactor of `_manage_position`
   — branch on the new enum. Keep tick-level as default to preserve
   live engine behaviour.

2. Build `backtest/gsp_exit_variants_bt.cpp` — drives the class
   directly, sweeps:
   - `trail_tight` ∈ {0.12, 0.20, 0.30, 0.40}
   - `exit_philosophy` ∈ {tick, bar_close, give_back}
   - Holds all other params at GSP best-config (LB=8, SL=1.5,
     TP=3.0, Pyr=Y, S63 OFF per S100c)
   - 12 configs total, ~12-15 min runtime

3. Report PnL/PF/avgWin/exit-mix per config. Tell the operator which
   variant (if any) closes the gap.

4. **Success criteria:** any single config produces PnL > +$5,000 and
   PF > 1.20 in tick-level execution. If yes → ship that variant as a
   new engine (GoldScalpV2 or similar) wired into engine_init with
   `enabled=false` initially, promote after operator review.

5. **Failure criteria:** all 12 configs negative or PF < 1.0. In that
   case the trail-based exit philosophy is fundamentally
   tick-fragile, and we pivot to fixed-RR (no trail) engines.

## Important lessons / don't-repeat

1. **Always drive the production class directly when validating an
   engine.** Inline-bar-level harnesses produce paper edges that don't
   survive tick-level deployment. The S63 audit harness shape
   (`gsp_s63_audit_bt.cpp`) is the template.

2. **Recommendation systems in tools are not always right.** The
   audit's auto-verdict said "disable S63" — true but insufficient.
   S63 off gets -$17K instead of -$18K; both are losers. Read the
   numbers, don't just take the recommendation.

3. **The harness's TP_HIT count is the canary.** When tick-level
   reproduces a backtest, TP_HIT count should match within ~10%.
   GSP harness: 72 TP_HITs. Class tick-level: 0 TP_HITs. That alone
   signals exit-resolution divergence even before reading PnL.

4. **Avg-win compression is the second canary.** If WR matches but
   avgWin is half the harness's value, the exit logic is firing
   earlier than the harness assumes. This will always indicate a
   tick-vs-bar gap.

5. **The user has been burned twice on engines with no edge today.**
   Next session: before committing to ANY new engine wiring, the
   tick-level harness must show positive expectancy with realistic
   exit modeling. Paper edges have eaten a full day's effort already.

## Files modified this session — final state

```
Committed (pushed via S99h-S99j chain):
M include/GoldScalpPyramidEngine.hpp         (S99h+S99j: tr.pnl pts*lots + spreadAtEntry)
M include/QuickScalpEngine.hpp               (S99h+S99j: same)
M include/BBandScalpEngine.hpp               (S99h: tr.pnl pts*lots)
M backtest/quick_scalp_bt.cpp                (S99h: tick_value scaling)
M backtest/bband_scalp_bt.cpp                (S99h: tick_value scaling)
?? include/BBandScalpEngine.hpp              (S99i: new engine, enabled=false)
?? backtest/bband_scalp_bt.cpp               (S99i: new harness)
?? .gitignore                                (S99i: backtest/*_results.txt)

Working tree (uncommitted at session close -- commit before deploy):
?? include/GoldSessionBreakoutEngine.hpp     (S100: new engine, enabled=false)
?? backtest/gold_session_breakout_bt.cpp     (S100: new harness, S63 off)
?? backtest/gsp_s63_audit_bt.cpp             (S100b: class-direct audit harness)
M  include/engine_init.hpp                   (S100c: g_gold_scalp_pyramid.enabled=false)
?? docs/handoffs/SESSION_HANDOFF_2026-05-18b.md  (this file)
```

## Pre-deploy checklist for the S100/S100b/S100c bundle

```bash
cd ~/omega_repo

# 1. Mac canary green
cmake --build build --target OmegaBacktest -j 2>&1 | tail -5

# 2. Stage everything
git add include/GoldSessionBreakoutEngine.hpp \
        backtest/gold_session_breakout_bt.cpp \
        backtest/gsp_s63_audit_bt.cpp \
        include/engine_init.hpp \
        docs/handoffs/SESSION_HANDOFF_2026-05-18b.md

# 3. Review diff
git diff --cached --stat

# 4. Single commit (these are tightly coupled by the bar-vs-tick finding)
git commit -m "S100: bar-vs-tick gap audit + GSP disabled live

Engineering session 2026-05-18b found the validated GSP \$15K backtest is
a bar-level-harness artifact. Class-direct tick-level audit reproduces
-\$16,935 / PF 0.62 / 0 TP_HITs on same tape. Disables live GSP pending
exit-philosophy redesign. See docs/handoffs/SESSION_HANDOFF_2026-05-18b.md
for full diagnostic, lessons, and next-session goal."

# 5. Push
git push origin main

# 6. VPS deploy + verify
# (on VPS)
cd C:\\Omega
.\\OMEGA.ps1 deploy
git rev-parse origin/main
Get-ChildItem C:\\Omega\\Omega.exe | Select Name, LastWriteTime
```

## Next-session opening sequence

```bash
cd ~/omega_repo

# Read this handoff first
less docs/handoffs/SESSION_HANDOFF_2026-05-18b.md

# Confirm state
git log --oneline -10
git status
git rev-parse HEAD                          # should equal origin/main

# Then start the work described in "Recommended next-session focus"
```

End of handoff.

# POLICY: No Python for Parameter Sweeps or Cost-Sensitive Backtests

**Status:** Active
**Effective:** 2026-05-06 (S7)
**Owner:** Jo
**Supersedes:** Any prior practice of running gold / xauusd / pass-N sweeps via Python harnesses.

---

## The rule

**Parameter sweeps, cost-sensitive backtests, and any decision-grade
expectancy analysis MUST run on the native C++ harness against the real
engine code.**

Python is not permitted as the execution path for any of:

- Parameter grid sweeps (SL/TP/risk/threshold combos)
- Walk-forward / OOS expectancy reports that feed ship/retire decisions
- Any backtest whose outputs are compared against `OmegaCostGuard` calibrated costs
- Any harness that mirrors engine logic outside of the C++ source of truth

Python remains permitted for:

- Post-hoc analysis of CSV outputs the C++ harness produces
- Visualisation, charting, ad-hoc exploration of results
- Data prep and feed conversion (Dukascopy, histdata, etc.)
- One-off diagnostics that do not influence ship decisions

---

## Why

Two failure modes were demonstrated in S5 / S6 and are unacceptable for
decision-grade work:

1. **Port drift.** A Python "mirror" of an engine drifts from the C++ source
   of truth as the C++ code evolves. Sweeps then evaluate a fictional engine.
2. **Cost-basis drift.** `OmegaCostGuard` recalibrations propagate into the
   live engine but not into Python `COST_GUARD` dicts. Sweeps then bill
   stale costs and overstate edge.

Both failure modes were observed simultaneously in Pass 3's
`sweep_v3_engine_s48.py`: the harness held `slip_pts: 0.22` for XAUUSD
while the live `OmegaCostGuard` had been recalibrated to 1.10 RT. The
result was a 12-month sweep producing PFs systematically inflated relative
to live execution.

The same problem appeared in LSP-Pro: the empirical C++ verdict
(`backtest/lsp_realcost_bt.cpp`) executed 20 combos x 8-day L2 in 118 seconds
- a ~1000x speedup vs. the equivalent Python path - and used the real
engine + real cost guard. It produced an unambiguous cull verdict that
the Python sweep would not have surfaced under stale costs.

---

## Required pattern

Every new sweep is a single-file C++ binary under `backtest/` that:

1. `#include`s the real engine header(s) directly. No mirrors.
2. Calls `OmegaCostGuard` for cost numbers. No private cost dict.
3. Loops over the parameter grid in-process (no shell-script outer loop).
4. Writes a single result CSV in the existing `backtest/results/` schema.
5. Builds with `backtest/build_mac.sh` or the project CMake target.

A reference template is `backtest/lsp_realcost_bt.cpp` (S5).

---

## Enforcement

- New `*sweep*.py` files in `backtest/`, `phase2/`, or `scripts/` that
  drive parameter grids will be rejected at review.
- `run_pass*.sh` harnesses must invoke a C++ binary, not `python ...`.
- Any sweep result CSV used in a ship/retire decision must be traceable to
  a C++ binary and a commit SHA. Python-produced CSVs are advisory only.
- Existing Python sweep scripts (e.g. `phase2/donchian_postregime_sweep_v3.py`,
  `backtest/mce_sweep.py`) are grandfathered for historical reference but
  are NOT to be re-run for new decisions. Port to C++ before re-using.

---

## Migration list (open)

Tracked separately in S7 plan:

- `sweep_v3_engine_s48.py` -> C++ template (highest priority; cost-bug active)
- `phase2/donchian_postregime_sweep_v3.py` -> C++ template
- `backtest/mce_sweep.py` -> C++ template
- Any `scripts/usdjpy_asian_*sweep*.py` used in decision flow -> C++ template

---

## Rationale summary

One source of truth (the C++ engine). One cost authority (`OmegaCostGuard`).
One execution path (native C++). Native speed makes "rerun the sweep"
a tactical decision instead of an overnight commitment, which means
cost-recalibrations actually propagate into live results instead of being
deferred indefinitely.

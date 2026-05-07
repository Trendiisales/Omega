# HANDOFF — FVG Phase 0 sniff test, 3-symbol pass

End of 2026-05-02 (late session). Entry point for the next round of work.

## TL;DR

Ran the Phase 0 FVG sniff test on three symbols. **All three hard-pass
all three acceptance gates.** Component-importance ranking is nearly
identical across asset classes — `gap_size`, `gap_height`,
`displacement`, `tick_volume` carry the signal; `trend_align` and
`age_decay` are dead weight on every symbol tested. Phase 1 score
weights should be rebuilt from this empirical finding rather than the
current equal-weight composite.

## What ran

Test script: `scripts/usdjpy_xauusd_fvg_signal_test.py` (unchanged from
the start of session — the methodology spec at the top of the file is
still the source of truth for what the gates mean and why).

| Symbol  | Window                | Bars (15min) | FVGs entered | Tick file source |
|---------|-----------------------|--------------|--------------|----------------|
| XAUUSD  | 2025-09-01..2026-03-01 | 11,698      | 1,800        | Dukascopy combined |
| USDJPY  | 2025-09-01..2026-03-01 | 12,300      | 2,161        | HistData EST → UTC shifted |
| NAS     | 2025-01-01..2026-05-01 | 28,944      | 4,739        | HistData NSXUSD EST → UTC shifted |

Output trees: `fvg_phase0/{XAUUSD,USDJPY,NAS}_15min/` containing
`summary.txt`, `fvgs.csv`, `random.csv`, `fvg_distribution.png`,
`bars_*.pkl`, `run.log`.

## Gate results

```
                            XAUUSD          USDJPY          NAS
>=200 entered                PASS (1,800)    PASS (2,161)    PASS (4,739)
edge vs random >= 5pp        PASS (+33.2)    PASS (+28.7)    PASS (+32.1)
Q4-Q1 spread >= 10pp         PASS (+16.2)    PASS (+14.8)    PASS (+15.9)
```

Bounce rate 70.3–72.5% in a tight band; random control 39.3–41.6%; edge
+28.7..+33.2 pp. Q4-Q1 spread +14.8..+16.2 pp. The consistency across
three unrelated asset classes is itself a finding — it says the score
is detecting *something* structural about FVG zones, not picking up a
gold-specific or FX-specific microstructure quirk.

## Component vs outcome — the actionable finding

Read this before the headline numbers (per the original handoff
guidance): the score is being carried by four of its five components,
and the same four on every symbol.

```
Component             XAUUSD       USDJPY       NAS         spread
s_gap_size           +0.225       +0.214       +0.214       0.011
gap_height           +0.151       +0.171       +0.144       0.027
s_displacement       +0.147       +0.129       +0.136       0.018
s_tick_volume        +0.121       +0.097       +0.102       0.024
s_age_decay          −0.033       −0.066       −0.058       0.033
s_trend_align        +0.020       −0.007       +0.002       0.027
bars_until_entry    +0.020       +0.039       +0.035       0.019
atr_at_entry         +0.038       −0.031       +0.036       0.069
```

`s_trend_align` is essentially zero on all three symbols. `s_age_decay`
is small-to-slightly-negative on all three. The current score formula
weights these equally with the components that actually predict
(weights `1.0, 1.0, 1.0, 1.0, 0.5` in `FvgConfig`). Re-deriving weights
from information content, or simply dropping the two zero-signal
components, should widen the Q4-Q1 spread on all three symbols.

This is the highest-value Phase 1 follow-up.

## Per-symbol divergences

Things that varied between symbols, worth keeping in mind for engine
design:

**Session pattern.** XAUUSD and USDJPY both peak Asian (74.9% and 72.3%)
and trough Off (68.9% and 66.0%). NAS *inverts* this — Asian troughs at
67.0%, London peaks at 74.1%, NY at 73.0%, Off at 72.3%. Nasdaq's home
session is NY; Asian-hours Nasdaq is thin futures-arb traffic and
structural levels there are less reliable. Any NAS engine using FVGs
should downweight Asian-session zones.

**ATR-quartile.** XAUUSD and NAS both bounce *more* in high-vol (low-high
diff −6.7pp and −5.0pp). USDJPY bounces *less* in high-vol (+3.0pp).
USDJPY's intervention / round-number / Tokyo-fix pinning makes its
high-vol regime structurally different from levels-respecting gold and
indices.

**Score quartile monotonicity.** Strict monotone increase on XAU and
JPY (Q1<Q2<Q3<Q4). NAS has a Q3 dip (Q3=68.0% < Q2=71.1%) but Q4 still
far above the rest (+15.9pp from Q1). Sample noise or an indices-specific
quirk in mid-quality FVGs — flag for Phase 1.

## What's untracked at end of session

```
?? docs/SESSION_2026-05-02_HANDOFF_NEXT.md     # the handoff that started today
?? docs/SESSION_2026-05-02_FVG_PHASE0_3SYMBOL_HANDOFF.md  # this doc
?? phase0_ema_scalp_backtest.py                # at repo root
?? phase0_fvg_signal_test.py                   # at repo root - obsoleted by scripts/
?? scripts/prep_histdata_est_to_dukascopy_utc.py
?? scripts/run_usdjpy_phase0.sh
?? scripts/run_nas_phase0.sh
?? scripts/combine_dukascopy_monthly.py        # written for Dukascopy NAS plan, replaced by HistData
?? fvg_phase0/                                 # all three symbol output trees
```

Triage decisions to make:

1. `phase0_fvg_signal_test.py` at repo root is the OLD version of the
   sniff-test script, superseded by `scripts/usdjpy_xauusd_fvg_signal_test.py`.
   Recommend `git rm`.
2. `phase0_ema_scalp_backtest.py` at repo root — separate concern (EMA
   scalp test, not FVG). Decide independently whether to commit to
   `scripts/` or remove.
3. The new scripts in `scripts/` (prep, both wrappers, combiner) and the
   handoff docs should be committed as a single change.
4. The `fvg_phase0/` output tree contains pickled bar caches and CSVs
   totaling several MB. Either commit a results-only subset (the three
   `summary.txt` files plus distribution PNGs are ~200 KB total), or
   gitignore the whole tree and link results from this handoff doc.
   Recommend the latter — bar caches and FVG CSVs are reproducible
   from the wrappers.

## Next-session queue (in priority order)

1. **Re-derive score weights from component-importance correlations
   and re-run all three symbols.** The headline finding above — same
   ranking on three asset classes — is begging for this. Hypothesis:
   dropping `trend_align` and `age_decay` from the composite (or
   weighting them at 0) will widen the Q4-Q1 spread on all three
   symbols by a few pp. This needs a CLI override on the score weights
   (current `FvgConfig` is dataclass defaults, no flag) or a small
   driver script that constructs `FvgConfig` programmatically. Either
   approach is a *new* file, not a core code edit.

2. **Re-run XAUUSD and NAS at 1h and 4h.** Tests timeframe-robustness
   on the symbols that already passed. Per the original session-start
   handoff, this was item 4 in the queue. Both wrappers accept `--tf`
   so it's just a parameter sweep. The three gates should still pass
   if FVGs are real-structural; if they collapse at higher TF, that
   tells us something about why the 15min edge exists.

3. **EURUSD 15min.** Original queue item 3. Data already present at
   `~/Tick/EURUSD/HISTDATA_COM_ASCII_EURUSD_T*` (HistData EST format,
   same pipeline as USDJPY). One paste of the prep + FVG flow with
   `--src-root ~/Tick/EURUSD` and a different combined-CSV name. Adds
   a fourth asset class to the component-ranking comparison.

4. **Triage and commit the untracked files.** See list above.

5. **S59 shadow gate watch (the actual original priority).** Don't
   forget: the FVG sniff test is exploratory groundwork for Phase 1.
   The thing that's *currently shipping* is S59 USDJPY Asian-Open in
   shadow mode on the VPS, with a 2-week wall-clock validation. The
   start-of-session handoff
   (`docs/SESSION_2026-05-02_HANDOFF_NEXT.md`) details the gates to
   watch. Don't let FVG follow-up work crowd out checking shadow PnL
   / WR / PF / DD against the header thresholds.

## State of the repo at session end

- Branch: `feature/usdjpy-asian-open` at `bcc187b` (unchanged on tracked files).
- One file modified mid-session: `scripts/prep_histdata_est_to_dukascopy_utc.py`
  — the regex was tightened to fall back to `(N)` duplicate folders
  when only the duplicate exists (fixes the Dec 2025 NSXUSD case where
  Jo's only download was named `... (1)`). This was a script written
  earlier in the same session, not a core-code change.
- All other modifications are *new files* (no edits to engine, test
  script, or other tracked code).
- S59 promoted production engine (`include/UsdjpyAsianOpenEngine.hpp`,
  md5 `315db6eca7a4f0a88689ea7970bb0380`) — untouched this session.
- Production safety: engine still ships `shadow_mode = true` per the
  header policy. Live promotion is via `engine_init.hpp` override and
  is NOT done.

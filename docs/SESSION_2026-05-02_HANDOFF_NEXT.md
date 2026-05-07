# HANDOFF — End of 2026-05-02 / start of next session

This doc is the entry point for the *next* session. It contains everything
needed to start the next round of work without re-reading the day's
session docs.

If you do want the full context, the lineage is:
  - docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_TRAIL_FIX.md  -- trail-fix sweep
  - docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_PHASE2_FINE.md -- fine grid
  - docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_WALKFORWARD_HANDOFF.md
  - docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_WALKFORWARD.md -- walk-forward,
       exit diagnostics, exit/SL re-sweep, S59 winner promoted
  - this doc -- what to do next

## Where things stand

Current branch: `feature/usdjpy-asian-open` (off `omega-terminal`).
Last commit: `f8ca63d S59 USDJPY Asian-Open: exit/SL re-sweep + walk-forward HARD PASS`
Pushed to `origin/feature/usdjpy-asian-open` on GitHub.
Mac and sandbox both verified: production engine reproduces
$407.60 / PF 1.28 / DD $186.14 / 9 of 14 mo bit-exactly.

Production engine `include/UsdjpyAsianOpenEngine.hpp` md5:
`315db6eca7a4f0a88689ea7970bb0380` (was `d514fce…3bd4be` pre-S59).

Engine ships with `shadow_mode = true` per the header's safety policy;
live promotion is via `engine_init.hpp` override and is NOT done.

## TOP PRIORITY -- track the S59 shadow validation

S59 was promoted to the engine header today. The 2-week paper shadow
gate is the actual go-live check. Pull the branch on the VPS, build,
restart the omega service, then watch:

  - shadow PnL / WR / PF over 2 weeks
  - WR must hold >= 60% in the 8-30 pip capture zone (header comment)
  - PF must hold >= 1.15 (matching walk-forward HARD PASS threshold)
  - DD must not exceed $250 in shadow

If any of those fail by week 1, halt and reconsider before live.
If all hold for 2 weeks, propose the engine_init.hpp override change
for explicit user approval. That live-promotion change is its own
commit, separate from any subsequent backtest tooling work.

VPS deploy commands are in chat history (Mac + VPS pull / build /
restart sequences). Logs should print `shadow_mode=true` at startup.

## NEW PRIORITY -- evaluate the FVG signal across other symbols

User explored `phase0_fvg_signal_test.py` (currently sitting untracked
in repo root) and asked whether FVG scoring has value beyond XAUUSD.
The script is a Phase 0 sniff test: it asks "do FVGs predict price
reactions better than random horizontal levels in the same data?" with
explicit go/no-go acceptance gates.

**Design assessment: the test design is solid.** It includes a random-
level control group (the single thing most retail FVG analysis lacks),
explicit acceptance gates, and a quartile-bucket test that asks
whether the composite score actually carries information. Worth
running. But four methodological issues need fixing before trusting
the output (detail below).

### Goal for next session

Run the FVG sniff test on each of:
  1. XAUUSD (baseline -- script's current target)
  2. USDJPY (can we reuse this concept on the symbol we just shipped?)
  3. EURUSD (S56 territory; may give different verdict)
  4. any other symbol with sufficient tick data

Pass criterion is per-symbol: each symbol gets its own go/no-go on the
acceptance gates. A symbol that PASSES is a candidate for a Phase 1
FVG-engine prototype. A symbol that FAILS is ruled out and we never
build an FVG engine for it.

### Pre-flight obstacles to fix BEFORE running

1. **The script as pasted into chat had markdown auto-link artifacts**
   (e.g. `[pd.read](http://pd.read)_csv`, `[fv.zone](http://fv.zone)_low`).
   If the file as it sits on disk has the same corruption, it will fail
   with a SyntaxError immediately. First action of next session:

       python3 -c "import ast; ast.parse(open('phase0_fvg_signal_test.py').read())"

   If it errors, clean up the markdown injections via search/replace
   before doing anything else.

2. **Tick-format mismatch.** The script expects Dukascopy-style CSV:

       timestamp,askPrice,bidPrice
       1709258400133,2044.562,2044.265

   (millisecond epoch, decimal prices, header row, comma-separated.)

   The USDJPY tick data on this Mac is `~/Tick/USDJPY/HISTDATA_COM_ASCII_USDJPY_T*`
   -- HistData ASCII format (different schema, different timestamp
   convention). The XAUUSD data is presumably already Dukascopy-style
   if the script was built for it.

   Decision needed: either
     (a) write a one-shot HistData -> Dukascopy converter for USDJPY
         (and EURUSD, if the EURUSD ticks are also HistData-format),
     (b) source Dukascopy USDJPY/EURUSD/XAUUSD tick data directly, OR
     (c) extend the script's `load_ticks_chunked` to detect and parse
         both formats.

   (c) is the most reusable. Probably ~30 lines of code: detect format
   from the first line of the CSV, branch the parser accordingly.

3. **`PIP_SIZE = 0.10` is declared but unused** in the script's
   detection/scoring/measurement flow -- everything is ATR-normalised.
   The script SHOULD work cross-instrument as-is once the tick reader
   handles each symbol's format. Confirm by running on XAUUSD first
   and checking the output looks sensible, THEN extend to other
   symbols.

4. **Bar-cache pickle naming** -- the cache file is named
   `bars_{tf}_{start}_{end}.pkl` with no symbol in the name. Running
   on USDJPY then XAUUSD with the same date range will collide.
   Add the symbol to the cache filename, OR put each symbol's run
   in its own out-dir.

### Methodological fixes (do before trusting results)

These improve the validity of the test, in priority order:

1. **First-touch direction, not max-of-window.** Currently
   `measure_reactions` finds max favourable and max adverse moves
   within the lookforward window and calls "bounce" if favourable >
   adverse > threshold. This overstates bounce rate -- a trade that
   gaps -1.5 ATR (would have stopped you out) then rallies +2 ATR
   gets recorded as a bounce. Fix: track which threshold gets crossed
   FIRST and classify on that.

2. **Random-level overlap rejection.** Currently random midpoints
   are uniform in [-3 ATR, +3 ATR] of formation close. Some random
   levels overlap the actual FVG zone, contaminating the control.
   Cheap fix: reject random midpoints within `gap_height` of the FVG
   midpoint and re-sample.

3. **Session split.** Gold's Asian session is dead and London/NY are
   not. USDJPY's profile is the inverse. Aggregating across sessions
   hides regime variation. Add a session column (Asian/London/NY by
   UTC hour) to the per-FVG output, then split bounce-rate stats by
   session in the report.

4. **News-day separation.** Docstring claims results are split by
   ATR percentile (visible in output) but I don't see that wired up
   in the report code. NFP/CPI/FOMC bars produce the largest FVGs
   AND the worst bounce rates (they're momentum continuations
   dressed as gaps). Either filter them out, or split the bounce-rate
   stats by ATR percentile bucket in the report.

### Suggested per-symbol run sequence

After fixes 1-4 above are in place:

    # XAUUSD baseline (sanity-check the test against the symbol it was
    # designed for)
    python3 phase0_fvg_signal_test.py \
        --tick-csv ~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv \
        --out-dir  ~/fvg_phase0/XAUUSD_15min \
        --start    2025-09-01 \
        --end      2026-03-01 \
        --tf       15min

    # USDJPY (after tick-format fix)
    python3 phase0_fvg_signal_test.py \
        --tick-csv ~/Tick/duka_ticks/USDJPY_2024-03_2026-04_combined.csv \
        --out-dir  ~/fvg_phase0/USDJPY_15min \
        --start    2025-09-01 \
        --end      2026-03-01 \
        --tf       15min

    # EURUSD (after tick-format fix)
    python3 phase0_fvg_signal_test.py \
        --tick-csv ~/Tick/duka_ticks/EURUSD_2024-03_2026-04_combined.csv \
        --out-dir  ~/fvg_phase0/EURUSD_15min \
        --start    2025-09-01 \
        --end      2026-03-01 \
        --tf       15min

Run each at 15-min, 1H, and 4H timeframes -- different timeframes capture
different liquidity tiers. A symbol that fails at 15min may pass at 4H
(longer-timeframe FVGs are more institutional).

### What "passes" looks like (per symbol, per timeframe)

The script's built-in acceptance gates:
  - >= 200 entered FVGs (sample size)
  - FVG bounce rate - random bounce rate >= 5pp absolute
  - top-quartile bounce rate - bottom-quartile >= 10pp

If a symbol PASSES on all three at any timeframe, that symbol+TF combo
is a Phase 1 candidate. Worth investing in a real FVG-engine prototype
for it.

If a symbol FAILS the top-quartile vs bottom-quartile test specifically,
the SCORE has no information even if FVGs themselves do beat random --
in which case you'd build a "use any FVG" engine, not a "use top-ranked
FVGs" engine.

If a symbol FAILS the FVG-vs-random test, FVGs themselves don't carry
predictive power on that symbol and no engine should be built.

### Most informative single output

The summary's **"COMPONENT vs OUTCOME"** correlation section. It tells
you which composite-score components actually carry information:
  - if `s_displacement` correlates >0.10 with bounce, the ICT-style
    "big middle bar" signal has merit
  - if `s_trend_align` does the work, FVGs are just trend-confirmation
    levels (and you'd be better off trading the trend directly)
  - if `gap_height` (raw gap size) correlates with bounce, you're
    measuring volatility, not signal
  - if NO component correlates, the score is noise even if overall
    FVGs > random

Read this section before the headline numbers.

## Carried-over queue (in order)

These are deferred items from prior session handoffs, still relevant:

1. **USDJPY London hours (06-09 UTC) on the S59 winner.** Cheapest
   test of "is this engine architecture sound or only working on
   Asian range data." Override `SESSION_START_HOUR_UTC=6,
   SESSION_END_HOUR_UTC=9`. One full-period cell via the trail-sweep
   `cell` subcommand or exit-sweep oos subcommand.

2. **Tokyo-fix exclusion (00:50-01:00 UTC) on the S59 winner.** With
   MR=0.20 most fix-ranges should already be filtered out, but worth
   one cell to confirm.

3. **Restricted session windows {01-04, 02-04, 00-02} UTC on the S59
   winner.** First hour after Tokyo open is documented chop.

4. **Mean-reversion variant.** Lowest priority; breakout direction
   was confirmed right by the S58/S59 work.

If the S59 shadow gate gives marginal results in week 1, items 1-3
become urgent (variant of "is the S59 config really the best") --
otherwise they're nice-to-haves.

## Cleanup

Two files in repo root that are not part of this work and should be
either committed-to-`scripts/` or removed:

    phase0_ema_scalp_backtest.py     -- prior session, exploratory, untracked
    phase0_fvg_signal_test.py        -- the FVG test script we're discussing

Decision per the FVG plan: move `phase0_fvg_signal_test.py` to
`scripts/usdjpy_xauusd_fvg_signal_test.py` (or similar), apply the
methodological fixes, then commit. Decide on the EMA scalp file
separately (commit if used, `git rm` if dead).

Also: the sandbox session left two stale-lock leftovers in `.git/`.
You already cleared `.git/HEAD.lock` to unblock the pull. The other
lingering items are harmless tmp objects:

    find .git/objects -name 'tmp_obj_*' -delete
    rm -f .git/index.lock.stale

## Reminders

- **Rotate the GitHub PAT** (`ghp_9M2I…24dJPV4`). Now exposed in
  CLAUDE.md, four session docs, and the chat transcript that pushed
  S59. Generate a fresh one with only `repo` scope, then move it OUT
  of CLAUDE.md to the macOS keychain via:

      git config --global credential.helper osxkeychain

  This has been called out in four session docs and is overdue.

- **S59 live promotion** stays gated on the 2-week shadow gate. Walk-
  forward HARD PASS is necessary but NOT sufficient. Do not edit
  `engine_init.hpp` to override `shadow_mode` until shadow data
  corroborates.

- **Do NOT raise LOT_MAX above 0.20** under any circumstances pre-
  shadow. Half-Kelly is ~0.07; current 0.20 is already aggressive.

- **The MFE/MAE fields in `trail_trades_*.csv` are scaled by lot size**
  (`tr.mfe = mfe_ * size_` at engine line 785). Divide by `size`
  before converting to pips. Documented in
  `scripts/usdjpy_asian_exit_analysis.py`.

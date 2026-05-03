# XauusdFvg §7 integration + verifier — session handoff

**Date:** 2026-05-02
**Sessions:** "Resume FVG. apply §7 + verifier." → this handoff.
**Repo state:** all §7 edits committed-on-disk (no git operations performed).
**Status:** §7 wiring complete. Verifier builds and runs. Row 1 matches every
field except `score_at_entry`, which is 0.001149 below expected — one
remaining indicator-shift bug to chase.

> **2026-05-03 RESOLUTION:** the open issue at the bottom of this doc
> (score-row-1 + count gap) is closed. Verifier PASSes row-for-row within
> tolerance after four follow-up fixes (tv_mean snapshot, two verifier-only
> mitigation overrides, verifier bar-window cap, and two engine semantic
> fixes — formation-bar session label and sequential-bar TIME_STOP counter).
> See `docs/SESSION_2026-05-03_FVG_VERIFIER_PASS.md` for the full chain and
> the production behavioural deltas.

---

## What is fully done

### 1. Engine override hook (verifier-only path)

`include/XauusdFvgEngine.hpp`

- Added public `double score_cutoff_override = -1.0;` (default <0 ⇒ stock).
- `try_mitigate_pending()` now uses
  `effective_cutoff = (score_cutoff_override > 0.0) ? score_cutoff_override : SCORE_CUTOFF;`
  so the verifier can replay against the v3 train-window top-decile pool.
  Production paths (override never set) see byte-identical 0.48-gate behaviour.

### 2. Engine timestamp bugfixes (live + verifier)

`include/XauusdFvgEngine.hpp`

- `open_position()`: `m_pos.entry_ts = bar.start_ts` (was `m_last_tick_s`).
  Aligns with `entry_bar_idx` (which already references the mitigation bar)
  and matches v3's `entry_time = times[entry_idx]`. Previously off by exactly
  `BAR_SECS` (one bar later).
- `_close()`: `tr.exitTs = m_cur_bar.start_ts` (was `now_s`). Bar-aligned;
  matches v3's `exit_time = times[exit_idx]` for SL/TP/TIME_STOP.
- `m_pos.session = session_for_ts(bar.start_ts)` (was `session_for_ts(m_last_tick_s)`)
  for consistency with the new entry_ts.

These are real semantic fixes — entry_ts/exit_ts now consistently mean
"start of the bar that the trade entered/exited on" instead of "moment of
the tick that triggered the bookkeeping". Reviewed for downstream impact:
`OmegaTradeLedger.dedup_key` is unaffected (still unique per-trade);
side-channel CSV is improved (now matches trades_top.csv).

### 3. Globals + open-position registry

`include/globals.hpp`

- `#include "XauusdFvgEngine.hpp"` added next to `#include "UsdjpyAsianOpenEngine.hpp"`.
- `#include "LogXauusdFvgCsv.hpp"` added (single-TU consumption from main.cpp).
- `static omega::XauusdFvgEngine g_xauusd_fvg;` added next to
  `g_usdjpy_asian_open`.

### 4. Side-channel CSV writer

`include/LogXauusdFvgCsv.hpp` (NEW, header-only)

- Header columns aligned with `trades_top.csv` for direct field-by-field diff.
- Translates engine codes: A/L/N/O → asian/london/ny/off, B/S → long/short,
  TP_HIT/SL_HIT/TIME_STOP → tp/sl/time_stop.
- ISO-8601 UTC timestamps with `+00:00` suffix matching pandas output.
- Default landing path `logs/live_xauusd_fvg.csv`. `set_log_path()` available
  to override before the first append.

### 5. Engine init wiring

`include/engine_init.hpp` — three insertions:

- After the `g_usdjpy_asian_open` init block: `g_xauusd_fvg.shadow_mode = true`,
  `cancel_fn`, and `on_close_cb = [](const TradeRecord& tr){ handle_closed_trade(tr); omega::xauusd_fvg::log_xauusd_fvg_csv(tr, g_xauusd_fvg); }`.
- After the `UsdjpyAsianOpen` `EngineRegistry` registration: `register_engine("XauusdFvg", …)`.
- After the `UsdjpyAsianOpen` `OpenPositionRegistry` source: `register_source("XauusdFvg", …)`
  with `tick_value_multiplier("XAUUSD")`. Startup banner bumped to "5 sources".

### 6. Tick dispatch + cohort gate

`include/tick_gold.hpp` — two insertions:

- `gold_any_open` extended to OR in `g_xauusd_fvg.has_open_position()` (per
  design §7.3 + §11.6 — FVG adds itself to the gate).
- New on-tick dispatch block placed just before NBM-London position management:
  `g_xauusd_fvg.on_tick(bid, ask, now_ms_g, gold_can_enter, nullptr);`
  (`nullptr` falls back to `on_close_cb`).

### 7. Cross-platform fix in OmegaNewsBlackout.hpp

`include/OmegaNewsBlackout.hpp:424` — wrapped the `_mkgmtime` call in the
same `#ifdef _WIN32 / #else / timegm` block already used at line 180 of the
same file. Strict portability fix; Windows path byte-identical. Was blocking
Mac builds.

### 8. Build target

`CMakeLists.txt` — added `add_executable(verify_xauusd_fvg backtest/verify_xauusd_fvg.cpp)`
with `-DOMEGA_BACKTEST` and a status-message line. Self-contained: no
OpenSSL/curl/FIX/time-shim deps. No other targets touched.

### 9. Verifier infrastructure

- `scripts/dump_bars_to_csv.py` — flattens the bars `.pkl` to a CSV the C++
  verifier ingests directly (no Python binding). Run once per `.pkl`.
- `backtest/verify_xauusd_fvg.cpp` — standalone replay binary:
  - Loads bars CSV + trades_top.csv.
  - Auto-derives the cutoff from `min(score_at_entry)` of trades_top.csv.
  - Drives `omega::XauusdFvgEngine` with a synthesised tick stream.
  - Tick synthesis is direction-aware (SL-wins-on-tie ordering), ms-spaced
    so `bar.tick_count` is preserved without exceeding the bar window.
  - Diff loop: strict tolerances on price/score/atr/gap/entry_ts; loose
    tolerances (`--tol-exit-price` 5.0 USD, `--tol-exit-ts-s` 900s) on
    exit fields per the architectural caveat below.

---

## Architectural caveats baked into the verifier

Bit-exact verification against the v3 backtest is **not achievable** because:

- **v3 is bar-driven; engine is tick-driven.** v3 detects SL/TP via `bar.low <= sl`
  / `bar.high >= tp` and exits at `gross_exit = sl/tp`. Engine detects per-tick
  via `bid <= sl` / `ask >= tp` and exits at `bid/ask`. Worst-case `exit_price`
  drift = `(sl - bar.low) + half_spread`. Design doc §2 explicitly accepts this.
- **`exit_ts` may shift by one bar** when the per-tick SL trigger fires one bar
  earlier or later than v3's bar-level trigger.

The verifier's loose `exit_price` / `exit_ts` tolerances absorb these.
**Tight-tolerance fields that should match exactly:** `direction`, `entry_ts`,
`session`, `sl`, `tp`, `entry_price`, `score_at_entry`, `atr_at_entry`,
`gap_height`, `exit_reason`.

---

## Open issue — to chase next session

**Symptom:** `[FAIL] row 1 field=score_at_entry expected=0.532743 got=0.531594`
plus count mismatch (`expected=70 emitted=42`). All other tight-tolerance
fields on row 1 pass.

**Hypothesis:** the engine's `tv_mean` rolling window is off by one bar
relative to the v3 / core reference.

The engine's `update_indicators()` pushes the formation bar's tick_count
INTO the rolling deque BEFORE `try_detect_fvg()` reads `m_tv_mean`. So the
engine's `tv_mean` at formation = mean of bars `[i-19, i-18, …, i]`. v3 /
core may compute it as mean of `[i-20, i-19, …, i-1]` (excluding the
formation bar itself, since pandas `.rolling(20).mean()` evaluated at the
middle bar would include up to the middle bar, not the formation bar).

**Action items for the next session:**

1. Read `scripts/usdjpy_xauusd_fvg_signal_test.py` — find the `s_tv` /
   tick-volume score component and the rolling-mean call. Confirm exact
   semantics for the bar-index alignment.
2. If shifted: in `XauusdFvgEngine.hpp::on_bar_close()`, reorder so
   `try_mitigate_pending()` and `try_detect_fvg()` run BEFORE
   `update_indicators()`; OR add a one-bar lag to `m_tv_mean` consumption.
   Decide based on the v3 reference. The same question applies to `m_atr14`
   ordering — confirm whether v3 reads ATR at bar `i-1` or bar `i` for the
   formation at bar `i`.
3. Re-run the verifier. Expected outcome: scores match within 1e-4 and
   count matches within ±2 (edge-of-cutoff trades that bar-vs-tick exit
   timing may shift).

**If the above doesn't close the gap, secondary suspects:**

- `s_disp` (displacement) computation — `mid_body / mid_range` weighting
  factor `clamp(mid_range / atr / 2, 0, 1)` uses `atr14` at formation.
  Same bar-shift question.
- `s_gap` clamp denominator: `(2.0 - MIN_GAP_ATR)`. Verify v3 uses the
  same. Engine: `(2.0 - 0.10) = 1.9`.

---

## How to re-run

```bash
cd ~/omega_repo

# Re-flatten only if the .pkl changed:
python3 scripts/dump_bars_to_csv.py \
    fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2025-09-01_2026-03-01.pkl

# Build:
cmake --build build --target verify_xauusd_fvg --config Release

# Run:
./build/verify_xauusd_fvg \
    fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2025-09-01_2026-03-01.csv \
    fvg_pnl_backtest_v3/XAUUSD_15min_top10_be0.0_sl2.5_tp5.0/trades_top.csv

# Useful flags during diagnosis:
#   --verbose                 dump every emitted vs expected trade
#   --cutoff <val>            override the auto-derived score cutoff
#   --tol-score 1e-2          loosen score tolerance to confirm count match
#   --tol-exit-price 10       loosen exit_price for noisy bars
#   --tol-exit-ts-s 1800      loosen exit_ts to 2 bars
```

---

## Files touched this session

NEW:
- `include/LogXauusdFvgCsv.hpp`
- `scripts/dump_bars_to_csv.py`
- `backtest/verify_xauusd_fvg.cpp`
- `docs/SESSION_2026-05-02_FVG_S7_HANDOFF.md` (this file)

EDITED (with line-anchored, surgical Edits — no full-file rewrites):
- `include/XauusdFvgEngine.hpp`           — score_cutoff_override hook, entry_ts/exit_ts/session bar-alignment
- `include/globals.hpp`                   — XauusdFvg + Log header includes + g_xauusd_fvg instance
- `include/engine_init.hpp`               — init block, EngineRegistry, OpenPositionRegistry
- `include/tick_gold.hpp`                 — gold_any_open extension, on-tick dispatch
- `include/OmegaNewsBlackout.hpp`         — `_mkgmtime` → `timegm` Mac portability
- `CMakeLists.txt`                        — verify_xauusd_fvg target
- `backtest/verify_xauusd_fvg.cpp`        — removed tick-count cap, ms spacing, loose exit tolerances

#!/usr/bin/env python3
"""
fvg_reweight_v1.py
==================

Re-runs the Phase 0 FVG sniff test on XAUUSD, USDJPY, and NAS using
empirically-derived score weights, then writes a side-by-side comparison
against the baseline (equal-weight) results stored in fvg_phase0/.

NEW FILE. Does NOT modify scripts/usdjpy_xauusd_fvg_signal_test.py or any
other tracked code. Imports the existing functions and overrides only the
FvgConfig score weights via dataclasses.replace().

Loads bars from the cached pickles in fvg_phase0/<SYMBOL>_15min/, so no
tick reload, no prep step. The whole sweep should finish in seconds.

Hypothesis being tested
-----------------------
Phase 0 component-vs-outcome correlations were:
    s_gap_size      +0.214 to +0.225  (top predictor on every symbol)
    s_displacement  +0.129 to +0.147
    s_tick_volume   +0.097 to +0.121
    s_age_decay     -0.033 to -0.066  (NEGATIVE on every symbol)
    s_trend_align   -0.007 to +0.020  (zero / noise on every symbol)

The current FvgConfig weights treat all five components as roughly equal
(1.0, 1.0, 1.0, 1.0, 0.5). That gives the two dead-weight components
~30% of the composite score's variance.

This driver reweights to (1.5, 1.0, 1.0, 0.0, 0.0):
    boost s_gap_size to 1.5    (top predictor)
    keep s_displacement at 1.0
    keep s_tick_volume at 1.0
    drop s_trend_align to 0.0  (noise)
    drop s_age_decay to 0.0    (slightly negative)

If the empirical importance ranking is real, the Q4-Q1 spread should
widen on all three symbols vs the baseline.

Usage
-----
    cd ~/omega_repo
    python3 scripts/fvg_reweight_v1.py

Outputs are written to fvg_phase0_reweight/<SYMBOL>_15min/ -- the existing
fvg_phase0/ baseline tree is left untouched. A side-by-side comparison is
written to fvg_phase0_reweight/comparison_summary.txt.

Decision rule
-------------
If Q4-Q1 widens on all three symbols   -> reweight validated, proceed to
                                          one-symbol P&L backtest on USDJPY.
If Q4-Q1 widens on 1-2 symbols         -> mixed; investigate before scaling.
If Q4-Q1 narrows or flat               -> components interact non-linearly;
                                          do NOT proceed to P&L backtest.
"""

from __future__ import annotations

import re
import sys
import time
import traceback
from dataclasses import replace
from pathlib import Path

import numpy as np
import pandas as pd


# Make the existing sniff-test module importable regardless of where this
# script is invoked from.
THIS_FILE = Path(__file__).resolve()
SCRIPTS_DIR = THIS_FILE.parent
REPO_ROOT = SCRIPTS_DIR.parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

from usdjpy_xauusd_fvg_signal_test import (  # noqa: E402
    FvgConfig,
    add_indicators,
    detect_fvgs,
    measure_reactions,
    generate_random_levels,
    fvgs_to_df,
    write_summary,
    write_distribution_chart,
)


# ---------------------------------------------------------------------------
# Weights to test
# ---------------------------------------------------------------------------
# These override the FvgConfig dataclass defaults. Every other field
# (atr_period, reaction_lookforward, gap-size filters, etc.) is left at
# its baseline value so the only variable changing across runs is the
# composite-score weighting.
NEW_WEIGHTS = {
    "w_gap_size":     1.5,
    "w_displacement": 1.0,
    "w_tick_volume":  1.0,
    "w_trend_align":  0.0,
    "w_age_decay":    0.0,
}


# ---------------------------------------------------------------------------
# Symbols + windows (must match the baseline fvg_phase0/ runs so the cached
# bar pickles are reusable)
# ---------------------------------------------------------------------------
SYMBOLS = [
    {"symbol": "XAUUSD", "tf": "15min", "start": "2025-09-01", "end": "2026-03-01"},
    {"symbol": "USDJPY", "tf": "15min", "start": "2025-09-01", "end": "2026-03-01"},
    {"symbol": "NAS",    "tf": "15min", "start": "2025-01-01", "end": "2026-05-01"},
]

BASELINE_ROOT = REPO_ROOT / "fvg_phase0"
OUTPUT_ROOT = REPO_ROOT / "fvg_phase0_reweight"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def build_config(tf: str) -> FvgConfig:
    """FvgConfig with new weights applied; everything else = baseline."""
    return replace(FvgConfig(), timeframe=tf, **NEW_WEIGHTS)


def load_cached_bars(symbol: str, tf: str, start: str, end: str) -> pd.DataFrame:
    """Load the bar pickle written by the baseline Phase 0 run.

    Filename convention from usdjpy_xauusd_fvg_signal_test.main():
        bars_{symbol}_{tf}_{start}_{end}.pkl
    """
    cache = (
        BASELINE_ROOT / f"{symbol}_{tf}"
        / f"bars_{symbol}_{tf}_{start}_{end}.pkl"
    )
    if not cache.exists():
        raise FileNotFoundError(
            f"Cached bars not found at {cache}.\n"
            f"  Re-run the baseline Phase 0 wrapper for {symbol} first to "
            f"regenerate the pickle, or run the sniff test directly with "
            f"--tick-csv pointing at your combined tick file."
        )
    print(f"[load] {cache.relative_to(REPO_ROOT)}")
    return pd.read_pickle(cache)


def run_one_symbol(spec: dict) -> None:
    """Run the reweighted sniff test for a single symbol, writing the same
    output set as the baseline (fvgs.csv, random.csv, summary.txt,
    fvg_distribution.png) into fvg_phase0_reweight/<SYMBOL>_<TF>/."""
    symbol = spec["symbol"]
    tf     = spec["tf"]
    start  = spec["start"]
    end    = spec["end"]

    print()
    print("=" * 70)
    print(f" {symbol}   {start} -> {end}   tf={tf}")
    print("=" * 70)
    t0 = time.time()

    out_dir = OUTPUT_ROOT / f"{symbol}_{tf}"
    out_dir.mkdir(parents=True, exist_ok=True)

    cfg = build_config(tf)
    bars = load_cached_bars(symbol, tf, start, end)
    bars = add_indicators(bars, cfg)

    fvgs = detect_fvgs(bars, cfg)
    measure_reactions(fvgs, bars, cfg)

    rng = np.random.default_rng(seed=42)
    randoms = generate_random_levels(fvgs, bars, cfg, rng)
    measure_reactions(randoms, bars, cfg)

    fvg_df = fvgs_to_df(fvgs)
    rand_df = fvgs_to_df(randoms)
    if not fvg_df.empty:
        fvg_df.to_csv(out_dir / "fvgs.csv", index=False)
    if not rand_df.empty:
        rand_df.to_csv(out_dir / "random.csv", index=False)

    write_summary(
        fvgs, randoms, out_dir / "summary.txt",
        cfg,
        pd.Timestamp(start, tz="UTC"),
        pd.Timestamp(end, tz="UTC"),
        symbol,
    )
    write_distribution_chart(fvgs, out_dir / "fvg_distribution.png")

    print(f"[done] {symbol} in {time.time() - t0:.1f}s -> {out_dir}")


# ---------------------------------------------------------------------------
# Comparison: parse both baseline and reweighted summary.txt files and
# write a side-by-side report
# ---------------------------------------------------------------------------
SUMMARY_PATTERNS = {
    # name        regex                                                       cast
    "entered":  (r"FVGs ever entered:\s+([\d,]+)",                            "int"),
    "bounce":   (r"All FVGs:\s+([\d.]+)%",                                    "float"),
    "random":   (r"Random control:\s+([\d.]+)%",                              "float"),
    "edge":     (r"EDGE vs random:\s+([+-]?[\d.]+)\s*pp",                     "float"),
    "q1":       (r"Q1_low\s*:\s*bounce=([\d.]+)%",                            "float"),
    "q2":       (r"Q2\s*:\s*bounce=([\d.]+)%",                                "float"),
    "q3":       (r"Q3\s*:\s*bounce=([\d.]+)%",                                "float"),
    "q4":       (r"Q4_high\s*:\s*bounce=([\d.]+)%",                           "float"),
    "q4_q1":    (r"Top quartile - bottom quartile:\s+([+-]?[\d.]+)\s*pp",     "float"),
}


def parse_summary(path: Path) -> dict:
    if not path.exists():
        return {}
    text = path.read_text()
    out: dict = {}
    for key, (pat, cast) in SUMMARY_PATTERNS.items():
        m = re.search(pat, text)
        if not m:
            continue
        raw = m.group(1).replace(",", "")
        out[key] = int(raw) if cast == "int" else float(raw)
    return out


def write_comparison() -> None:
    out_path = OUTPUT_ROOT / "comparison_summary.txt"

    lines: list[str] = []
    lines.append("FVG Phase 0 - Reweight v1 vs Baseline")
    lines.append("=" * 80)
    lines.append("")
    lines.append("Reweighted FvgConfig (vs baseline 1.0 / 1.0 / 1.0 / 1.0 / 0.5):")
    for k, v in NEW_WEIGHTS.items():
        lines.append(f"  {k:<18} {v}")
    lines.append("")
    lines.append("Hypothesis: drop noise (s_trend_align) and negative-correlation")
    lines.append("            component (s_age_decay), boost top predictor (s_gap_size).")
    lines.append("            Q4-Q1 spread should widen on all 3 symbols if the")
    lines.append("            empirical importance ranking translates to better")
    lines.append("            discrimination by the composite score.")
    lines.append("")
    lines.append(f"{'Symbol':<8} {'Metric':<20} {'Baseline':>12} {'Reweight':>12} {'Delta':>10}")
    lines.append("-" * 80)

    measured = 0
    widened  = 0

    rows_to_compare = [
        ("FVGs entered",     "entered", "int"),
        ("bounce rate %",    "bounce",  "float"),
        ("random rate %",    "random",  "float"),
        ("edge vs random pp", "edge",   "float"),
        ("Q1 bounce %",      "q1",      "float"),
        ("Q4 bounce %",      "q4",      "float"),
        ("Q4-Q1 spread pp",  "q4_q1",   "float"),
    ]

    for spec in SYMBOLS:
        sym = spec["symbol"]
        tf  = spec["tf"]
        base = parse_summary(BASELINE_ROOT / f"{sym}_{tf}" / "summary.txt")
        rw   = parse_summary(OUTPUT_ROOT   / f"{sym}_{tf}" / "summary.txt")

        for i, (label, key, cast) in enumerate(rows_to_compare):
            sym_col = sym if i == 0 else ""
            b = base.get(key)
            r = rw.get(key)
            if b is None or r is None:
                b_s = "n/a" if b is None else (str(b) if cast == "int" else f"{b:.2f}")
                r_s = "n/a" if r is None else (str(r) if cast == "int" else f"{r:.2f}")
                d_s = "n/a"
            else:
                b_s = str(b) if cast == "int" else f"{b:.2f}"
                r_s = str(r) if cast == "int" else f"{r:.2f}"
                d_s = f"{r - b:+}" if cast == "int" else f"{r - b:+.2f}"
            lines.append(f"{sym_col:<8} {label:<20} {b_s:>12} {r_s:>12} {d_s:>10}")

            if key == "q4_q1" and isinstance(b, (int, float)) and isinstance(r, (int, float)):
                measured += 1
                if r > b:
                    widened += 1

        lines.append("")

    lines.append("-" * 80)
    lines.append("HYPOTHESIS RESULT")
    lines.append("-" * 80)
    if measured == 0:
        lines.append("Could not measure Q4-Q1 spread on any symbol -- check the parser")
        lines.append("output and the actual summary.txt formats.")
    elif widened == measured:
        lines.append(f"Q4-Q1 spread WIDENED on all {measured} symbols.")
        lines.append("Reweight is empirically validated.")
        lines.append("")
        lines.append("Next step: build a one-symbol P&L backtest on USDJPY using Q4")
        lines.append("entries on the reweighted score. Apply realistic execution")
        lines.append("(spread, slippage, defensible stop methodology). Decision bar:")
        lines.append("profit factor > 1.4, max drawdown < 15%, Sharpe > 1.0.")
    elif widened > 0:
        lines.append(f"Q4-Q1 spread widened on {widened}/{measured} symbols.")
        lines.append("Mixed result. Investigate the non-improving symbol(s) before")
        lines.append("scaling to other instruments. Possible causes: per-symbol")
        lines.append("microstructure, component interactions, or wrong functional form")
        lines.append("for one of the dropped components.")
    else:
        lines.append(f"Q4-Q1 spread did NOT widen on any of the {measured} symbols.")
        lines.append("The empirical correlations did not translate to better")
        lines.append("discrimination. Likely cause: components interact non-linearly,")
        lines.append("or the dropped components were carrying signal that linear")
        lines.append("correlations missed. Do NOT proceed to a P&L backtest with this")
        lines.append("weight vector.")
    lines.append("")

    text = "\n".join(lines) + "\n"
    out_path.write_text(text)
    print()
    print(text)
    print(f"Comparison written to: {out_path}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def main() -> int:
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)

    print("FVG Reweight v1 - Driver")
    print(f"  repo root:  {REPO_ROOT}")
    print(f"  baseline:   {BASELINE_ROOT}")
    print(f"  output:     {OUTPUT_ROOT}")
    print(f"  weights:    {NEW_WEIGHTS}")

    failures = 0
    for spec in SYMBOLS:
        try:
            run_one_symbol(spec)
        except Exception as e:
            failures += 1
            print(f"\n[ERROR] {spec['symbol']}: {type(e).__name__}: {e}")
            traceback.print_exc()
            print("Continuing with remaining symbols.")

    write_comparison()

    if failures:
        print(f"\nCompleted with {failures} failure(s).")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

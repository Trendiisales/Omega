#!/usr/bin/env python3
"""
fvg_pnl_backtest_v2.py
======================

Successor to fvg_pnl_backtest_v1.py with three new diagnostic levers:

  1. WIDER SL/TP GRID
     v1's grid (0.5/1.5, 1.0/2.0, 1.5/3.0) showed a clear monotonic
     improvement as stops widened. v2 extends to 2.0/4.0 and 2.5/5.0 so
     we can see whether the trend keeps climbing or plateaus.

  2. ABSOLUTE GAP-SIZE FILTER (--min-gap-pips)
     v1 used the relative `min_gap_atr` filter that ships with
     FvgConfig. That kept tiny gaps that nominally pass the ATR ratio
     but are still smaller than the round-trip cost in absolute pips.
     v2 adds an absolute floor: only take FVGs whose `gap_height` (in
     price units) is >= min_gap_pips * pip_size. Default 0 = off.

  3. AUTO-RESAMPLE TO HIGHER TIMEFRAMES
     v1 required a per-TF bar pickle written by the baseline Phase 0
     wrapper. For 1h and 4h evaluation that meant a full tick-reload.
     v2 falls back to the cached 15min pickle and resamples in-process
     to the requested TF, then caches the result so subsequent runs use
     it directly.

NEW FILE. Does NOT modify any tracked code (including v1). Imports the
existing detection / scoring / reaction / random-control functions and
overrides only the FvgConfig score weights via dataclasses.replace(),
matching the reweight v1 driver.

Methodology unchanged from v1 except for the three additions above.
Pre-registered acceptance gates unchanged:

    n_trades >= 100
    Q4 PF >= 1.4
    Q4 max DD < 15%
    Q4 Sharpe >= 1.0
    Q4 still PF >= 1.0 at 2x cost
    Q4 PF strictly greater than All-FVG and Q1 PF

Usage
-----
    cd ~/omega_repo

    # Option A: USDJPY 15m with min-gap-pips=5 (friction-test)
    python3 scripts/fvg_pnl_backtest_v2.py --min-gap-pips 5

    # Option B: higher TFs (auto-resample from existing 15min pickle)
    python3 scripts/fvg_pnl_backtest_v2.py --tf 1h
    python3 scripts/fvg_pnl_backtest_v2.py --tf 4h

    # Option C: alternate symbols (different cost-to-edge profile)
    python3 scripts/fvg_pnl_backtest_v2.py --symbol XAUUSD
    python3 scripts/fvg_pnl_backtest_v2.py --symbol NAS \\
        --start 2025-01-01 --end 2026-05-01

Outputs land in fvg_pnl_backtest_v2/<SYMBOL>_<TF>/.
"""

from __future__ import annotations

import argparse
import math
import sys
import time
import traceback
from dataclasses import dataclass, replace
from pathlib import Path
from typing import List, Optional, Tuple

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


# Make the existing sniff-test module importable regardless of where this
# script is invoked from.
THIS_FILE = Path(__file__).resolve()
SCRIPTS_DIR = THIS_FILE.parent
REPO_ROOT = SCRIPTS_DIR.parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

from usdjpy_xauusd_fvg_signal_test import (  # noqa: E402
    Fvg,
    FvgConfig,
    add_indicators,
    detect_fvgs,
    measure_reactions,
    generate_random_levels,
)


# ---------------------------------------------------------------------------
# Reweighted score config (must match scripts/fvg_reweight_v1.py exactly so
# the Q4 cutoff is computed on the same score distribution that produced
# the validated 6.6-8.0 pp Q4-Q1 widening on USDJPY).
# ---------------------------------------------------------------------------
NEW_WEIGHTS = {
    "w_gap_size":     1.5,
    "w_displacement": 1.0,
    "w_tick_volume":  1.0,
    "w_trend_align":  0.0,
    "w_age_decay":    0.0,
}


# Per-symbol pip size in price units.
PIP_SIZE = {
    "USDJPY": 0.01,
    "EURUSD": 0.0001, "GBPUSD": 0.0001, "AUDUSD": 0.0001,
    "USDCAD": 0.0001, "USDCHF": 0.0001, "NZDUSD": 0.0001,
    "EURJPY": 0.01, "GBPJPY": 0.01, "AUDJPY": 0.01,
    "EURGBP": 0.0001, "EURAUD": 0.0001, "CHFJPY": 0.01,
    "CADJPY": 0.01, "AUDNZD": 0.0001,
    "XAUUSD": 0.10, "XAGUSD": 0.005,
    "NAS":    1.0,    # index point
}


# Bars per year for Sharpe annualization. FX/CFD trades roughly 24/5 so
# we approximate as 252 trading days * 24 hours per day. Per-TF table.
BARS_PER_YEAR_BY_TF = {
    "15min": 252 * 24 * 4,
    "1h":    252 * 24,
    "4h":    252 * 6,
    "1d":    252,
}


# ---------------------------------------------------------------------------
# Configs
# ---------------------------------------------------------------------------
@dataclass
class CostsConfig:
    """Execution cost model (charged on BOTH entry and exit)."""
    slippage_pips: float = 0.5
    pip_size: float = 0.01
    half_spread_floor: float = 0.0
    cost_multiplier: float = 1.0


@dataclass
class RiskConfig:
    """Stop / target / sizing / horizon."""
    sl_atr_mult: float = 1.0
    tp_atr_mult: float = 2.0
    time_stop_bars: int = 30
    risk_per_trade_pct: float = 0.005
    starting_equity: float = 100_000.0


@dataclass
class Trade:
    """Single trade record."""
    fvg_formed_idx: int
    direction: str
    entry_idx: int
    entry_time: pd.Timestamp
    entry_price: float
    sl: float
    tp: float
    score_at_entry: float
    atr_at_entry: float
    session: str
    gap_height: float
    exit_idx: int
    exit_time: pd.Timestamp
    exit_price: float
    exit_reason: str
    pnl_pips: float
    pnl_R: float
    pnl_dollars: float
    equity_after: float
    bars_held: int


# ---------------------------------------------------------------------------
# Bar resampling (15min -> any larger TF)
# ---------------------------------------------------------------------------
def resample_bars(bars_15m: pd.DataFrame, target_tf: str) -> pd.DataFrame:
    """Resample 15-minute OHLC bars to the requested timeframe.

    Aggregations:
        open         -> first
        high         -> max
        low          -> min
        close        -> last
        tick_count   -> sum
        spread_mean  -> tick-weighted average

    The index must be a tz-aware DatetimeIndex. Empty buckets are dropped.
    """
    if not isinstance(bars_15m.index, pd.DatetimeIndex):
        raise TypeError("resample_bars expects a DatetimeIndex on bars_15m")
    grp = bars_15m.resample(target_tf, label="left", closed="left")
    out = pd.DataFrame({
        "open":       grp["open"].first(),
        "high":       grp["high"].max(),
        "low":        grp["low"].min(),
        "close":      grp["close"].last(),
        "tick_count": grp["tick_count"].sum(),
    })
    spread_x_tc = (bars_15m["spread_mean"] * bars_15m["tick_count"]) \
        .resample(target_tf, label="left", closed="left").sum()
    tc_sum = bars_15m["tick_count"] \
        .resample(target_tf, label="left", closed="left").sum()
    out["spread_mean"] = (spread_x_tc / tc_sum).where(tc_sum > 0)
    out = out.dropna(subset=["open"])
    return out


def load_or_resample_bars(symbol: str, tf: str, start: str, end: str) -> pd.DataFrame:
    """Try to load the bar pickle for the requested TF. If missing and TF
    is not 15min, fall back to resampling from the 15min pickle and cache
    the result for next time.
    """
    target_dir = REPO_ROOT / "fvg_phase0" / f"{symbol}_{tf}"
    target_pkl = target_dir / f"bars_{symbol}_{tf}_{start}_{end}.pkl"
    if target_pkl.exists():
        print(f"[load] {target_pkl.relative_to(REPO_ROOT)}")
        return pd.read_pickle(target_pkl)

    if tf == "15min":
        raise FileNotFoundError(
            f"Cached 15min bars not found at {target_pkl}.\n"
            f"  Run the baseline Phase 0 wrapper first to regenerate the pickle:\n"
            f"  python3 scripts/usdjpy_xauusd_fvg_signal_test.py --symbol {symbol} "
            f"--tick-csv <path> --out-dir fvg_phase0/{symbol}_{tf} "
            f"--start {start} --end {end} --tf {tf}"
        )

    # Fallback: load 15min pickle and resample in process.
    src_dir = REPO_ROOT / "fvg_phase0" / f"{symbol}_15min"
    src_pkl = src_dir / f"bars_{symbol}_15min_{start}_{end}.pkl"
    if not src_pkl.exists():
        raise FileNotFoundError(
            f"Neither {target_pkl} nor 15min source {src_pkl} exists.\n"
            f"  Run the baseline Phase 0 wrapper for 15min first."
        )
    print(f"[load] {src_pkl.relative_to(REPO_ROOT)} (resampling -> {tf})")
    bars_15m = pd.read_pickle(src_pkl)
    bars = resample_bars(bars_15m, tf)
    print(f"[resample] {len(bars_15m):,} 15min bars -> {len(bars):,} {tf} bars")

    # Cache the resampled pickle so subsequent runs skip the resample step.
    target_dir.mkdir(parents=True, exist_ok=True)
    bars.to_pickle(target_pkl)
    print(f"[cache] wrote {target_pkl.relative_to(REPO_ROOT)}")
    return bars


# ---------------------------------------------------------------------------
# Quartile cutoff on entered FVGs
# ---------------------------------------------------------------------------
def compute_score_quartiles(entered_fvgs: List[Fvg]) -> Tuple[float, float, float]:
    """Return (q1_cutoff, q3_cutoff, median). Top quartile = score >= q3."""
    scores = np.array(
        [fv.score_at_entry for fv in entered_fvgs if fv.score_at_entry is not None],
        dtype=float,
    )
    if scores.size < 4:
        raise RuntimeError(
            f"Only {scores.size} entered FVGs with valid scores - cannot bucket "
            f"into quartiles. Widen the date range, relax filters, or lower "
            f"--min-gap-pips."
        )
    return (
        float(np.quantile(scores, 0.25)),
        float(np.quantile(scores, 0.75)),
        float(np.quantile(scores, 0.50)),
    )


# ---------------------------------------------------------------------------
# Single-trade simulator
# ---------------------------------------------------------------------------
def simulate_trade(
    fv: Fvg,
    bars: pd.DataFrame,
    h_arr: np.ndarray,
    l_arr: np.ndarray,
    o_arr: np.ndarray,
    spread_arr: np.ndarray,
    times: pd.DatetimeIndex,
    risk_cfg: RiskConfig,
    cost_cfg: CostsConfig,
) -> Optional[Trade]:
    """Simulate one trade for a single FVG using the locked rules."""
    if fv.entry_idx is None or fv.atr_at_entry is None:
        return None
    if not math.isfinite(fv.atr_at_entry) or fv.atr_at_entry <= 0:
        return None

    entry_idx = int(fv.entry_idx)
    n = len(bars)
    if entry_idx >= n - 1:
        return None

    atr_e = float(fv.atr_at_entry)
    direction = "long" if fv.direction == "bull" else "short"

    bar_spread = spread_arr[entry_idx]
    if not (math.isfinite(bar_spread) and bar_spread > 0):
        bar_spread = 0.0
    half_spread = max(bar_spread / 2.0, cost_cfg.half_spread_floor)
    slip = cost_cfg.slippage_pips * cost_cfg.pip_size
    side_cost = (half_spread + slip) * cost_cfg.cost_multiplier

    if direction == "long":
        gross_entry = float(fv.zone_high)
        entry_price = gross_entry + side_cost
        sl = gross_entry - risk_cfg.sl_atr_mult * atr_e
        tp = gross_entry + risk_cfg.tp_atr_mult * atr_e
    else:
        gross_entry = float(fv.zone_low)
        entry_price = gross_entry - side_cost
        sl = gross_entry + risk_cfg.sl_atr_mult * atr_e
        tp = gross_entry - risk_cfg.tp_atr_mult * atr_e

    risk_per_unit = risk_cfg.sl_atr_mult * atr_e
    if risk_per_unit <= 0:
        return None

    last = min(n - 1, entry_idx + risk_cfg.time_stop_bars)
    exit_idx = -1
    exit_reason = ""
    gross_exit = float("nan")

    for k in range(entry_idx + 1, last + 1):
        bar_h = h_arr[k]
        bar_l = l_arr[k]
        if not (math.isfinite(bar_h) and math.isfinite(bar_l)):
            continue
        if direction == "long":
            sl_hit = bar_l <= sl
            tp_hit = bar_h >= tp
        else:
            sl_hit = bar_h >= sl
            tp_hit = bar_l <= tp

        if sl_hit and tp_hit:
            exit_idx, exit_reason, gross_exit = k, "sl", sl
            break
        if sl_hit:
            exit_idx, exit_reason, gross_exit = k, "sl", sl
            break
        if tp_hit:
            exit_idx, exit_reason, gross_exit = k, "tp", tp
            break

    if exit_idx == -1:
        time_stop_idx = entry_idx + risk_cfg.time_stop_bars
        if time_stop_idx + 1 < n:
            exit_idx = time_stop_idx + 1
            exit_reason = "time_stop"
            gross_exit = float(o_arr[exit_idx])
        elif time_stop_idx < n:
            exit_idx = n - 1
            exit_reason = "end_of_data"
            gross_exit = float(bars["close"].values[exit_idx])
        else:
            return None

    if not math.isfinite(gross_exit):
        return None

    if direction == "long":
        exit_price = gross_exit - side_cost
        gross_pnl_price = gross_exit - gross_entry
    else:
        exit_price = gross_exit + side_cost
        gross_pnl_price = gross_entry - gross_exit

    net_pnl_price = gross_pnl_price - 2.0 * side_cost
    pnl_pips = net_pnl_price / cost_cfg.pip_size
    pnl_R = net_pnl_price / risk_per_unit

    risk_dollars = risk_cfg.risk_per_trade_pct * risk_cfg.starting_equity
    pnl_dollars = pnl_R * risk_dollars

    return Trade(
        fvg_formed_idx=int(fv.formed_idx),
        direction=direction,
        entry_idx=entry_idx,
        entry_time=times[entry_idx],
        entry_price=float(entry_price),
        sl=float(sl),
        tp=float(tp),
        score_at_entry=float(fv.score_at_entry) if fv.score_at_entry is not None else float("nan"),
        atr_at_entry=atr_e,
        session=str(getattr(fv, "session", "unknown")),
        gap_height=float(fv.gap_height),
        exit_idx=exit_idx,
        exit_time=times[exit_idx],
        exit_price=float(exit_price),
        exit_reason=exit_reason,
        pnl_pips=float(pnl_pips),
        pnl_R=float(pnl_R),
        pnl_dollars=float(pnl_dollars),
        equity_after=float(risk_cfg.starting_equity + pnl_dollars),
        bars_held=int(exit_idx - entry_idx),
    )


# ---------------------------------------------------------------------------
# Backtest runner
# ---------------------------------------------------------------------------
def run_backtest(
    fvgs: List[Fvg],
    bars: pd.DataFrame,
    risk_cfg: RiskConfig,
    cost_cfg: CostsConfig,
    score_filter: Optional[Tuple[float, float]] = None,
    label: str = "run",
) -> Tuple[List[Trade], dict]:
    h_arr = bars["high"].values
    l_arr = bars["low"].values
    o_arr = bars["open"].values
    spread_arr = (bars["spread_mean"].values
                  if "spread_mean" in bars.columns
                  else np.zeros(len(bars)))
    times = bars.index

    candidates = [
        fv for fv in fvgs
        if fv.entry_idx is not None and fv.reaction != "no_entry"
    ]
    candidates.sort(key=lambda fv: int(fv.entry_idx))

    if score_filter is not None:
        lo, hi = score_filter
        candidates = [
            fv for fv in candidates
            if fv.score_at_entry is not None
            and lo <= float(fv.score_at_entry) <= hi
        ]

    trades: List[Trade] = []
    next_open_after = -1
    skipped_overlap = 0
    skipped_unfillable = 0
    equity = risk_cfg.starting_equity

    for fv in candidates:
        if int(fv.entry_idx) < next_open_after:
            skipped_overlap += 1
            continue
        tr = simulate_trade(fv, bars, h_arr, l_arr, o_arr, spread_arr, times,
                            risk_cfg, cost_cfg)
        if tr is None:
            skipped_unfillable += 1
            continue
        equity += tr.pnl_dollars
        tr.equity_after = equity
        trades.append(tr)
        next_open_after = tr.exit_idx + 1

    diag = {
        "label": label,
        "candidates": len(candidates),
        "trades_taken": len(trades),
        "skipped_overlap": skipped_overlap,
        "skipped_unfillable": skipped_unfillable,
    }
    print(f"[run] {label}: candidates={diag['candidates']} "
          f"trades={diag['trades_taken']} "
          f"overlap_skip={diag['skipped_overlap']} "
          f"unfillable_skip={diag['skipped_unfillable']}")
    return trades, diag


# ---------------------------------------------------------------------------
# Stats
# ---------------------------------------------------------------------------
def compute_stats(
    trades: List[Trade],
    risk_cfg: RiskConfig,
    bars_per_year: float,
) -> dict:
    n = len(trades)
    if n == 0:
        return {
            "n": 0, "win_rate": 0.0, "avg_R": 0.0, "expectancy_R": 0.0,
            "profit_factor": 0.0, "total_R": 0.0, "total_pips": 0.0,
            "total_dollars": 0.0, "max_dd_R": 0.0, "max_dd_dollars": 0.0,
            "max_dd_pct": 0.0, "sharpe": 0.0, "longest_loss_streak": 0,
            "avg_bars_held": 0.0, "n_tp": 0, "n_sl": 0, "n_time": 0,
            "n_long": 0, "n_short": 0,
        }

    R = np.array([t.pnl_R for t in trades], dtype=float)
    pips = np.array([t.pnl_pips for t in trades], dtype=float)
    usd = np.array([t.pnl_dollars for t in trades], dtype=float)
    bars_held = np.array([t.bars_held for t in trades], dtype=float)

    wins = R > 0
    losses = R < 0

    win_rate = float(wins.mean())
    avg_R = float(R.mean())
    total_R = float(R.sum())
    total_pips = float(pips.sum())
    total_dollars = float(usd.sum())

    pos_sum = float(R[wins].sum()) if wins.any() else 0.0
    neg_sum = float(-R[losses].sum()) if losses.any() else 0.0
    profit_factor = (pos_sum / neg_sum) if neg_sum > 0 \
        else (float("inf") if pos_sum > 0 else 0.0)

    eq_R = np.cumsum(R)
    peak_R = np.maximum.accumulate(eq_R)
    max_dd_R = float((peak_R - eq_R).max()) if eq_R.size else 0.0

    eq_usd = risk_cfg.starting_equity + np.cumsum(usd)
    peak_usd = np.maximum.accumulate(eq_usd)
    max_dd_dollars = float((peak_usd - eq_usd).max()) if eq_usd.size else 0.0
    max_dd_pct = float(max_dd_dollars / risk_cfg.starting_equity * 100.0)

    avg_held = float(bars_held.mean()) if bars_held.size else 0.0
    if avg_held > 0:
        trades_per_year = bars_per_year / max(1.0, avg_held)
    else:
        trades_per_year = 1.0
    if R.std(ddof=1) > 0 and n > 1:
        sharpe = float((avg_R / R.std(ddof=1)) * math.sqrt(trades_per_year))
    else:
        sharpe = 0.0

    longest_loss_streak = 0
    cur = 0
    for r in R:
        if r < 0:
            cur += 1
            longest_loss_streak = max(longest_loss_streak, cur)
        else:
            cur = 0

    return {
        "n": n,
        "win_rate": win_rate,
        "avg_R": avg_R,
        "expectancy_R": avg_R,
        "profit_factor": profit_factor,
        "total_R": total_R,
        "total_pips": total_pips,
        "total_dollars": total_dollars,
        "max_dd_R": max_dd_R,
        "max_dd_dollars": max_dd_dollars,
        "max_dd_pct": max_dd_pct,
        "sharpe": sharpe,
        "longest_loss_streak": longest_loss_streak,
        "avg_bars_held": avg_held,
        "n_tp": int(sum(1 for t in trades if t.exit_reason == "tp")),
        "n_sl": int(sum(1 for t in trades if t.exit_reason == "sl")),
        "n_time": int(sum(1 for t in trades if t.exit_reason in ("time_stop", "end_of_data"))),
        "n_long": int(sum(1 for t in trades if t.direction == "long")),
        "n_short": int(sum(1 for t in trades if t.direction == "short")),
    }


def trades_to_df(trades: List[Trade]) -> pd.DataFrame:
    if not trades:
        return pd.DataFrame()
    rows = []
    for t in trades:
        rows.append({
            "fvg_formed_idx": t.fvg_formed_idx,
            "direction": t.direction,
            "entry_idx": t.entry_idx,
            "entry_time": t.entry_time,
            "entry_price": t.entry_price,
            "sl": t.sl,
            "tp": t.tp,
            "score_at_entry": t.score_at_entry,
            "atr_at_entry": t.atr_at_entry,
            "gap_height": t.gap_height,
            "session": t.session,
            "exit_idx": t.exit_idx,
            "exit_time": t.exit_time,
            "exit_price": t.exit_price,
            "exit_reason": t.exit_reason,
            "pnl_pips": t.pnl_pips,
            "pnl_R": t.pnl_R,
            "pnl_dollars": t.pnl_dollars,
            "equity_after": t.equity_after,
            "bars_held": t.bars_held,
        })
    return pd.DataFrame(rows)


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
def fmt_stats_row(label: str, st: dict) -> str:
    return (
        f"  {label:<14} n={st['n']:>4}  "
        f"win={st['win_rate']*100:5.1f}%  "
        f"avgR={st['avg_R']:+.3f}  "
        f"PF={st['profit_factor']:5.2f}  "
        f"DD={st['max_dd_pct']:5.2f}%  "
        f"Sharpe={st['sharpe']:6.2f}  "
        f"$={st['total_dollars']:+,.0f}"
    )


def write_summary(
    out_path: Path,
    symbol: str, tf: str, start: str, end: str,
    risk_cfg: RiskConfig, cost_cfg: CostsConfig,
    min_gap_pips: float,
    score_q1: float, score_q3: float, score_med: float,
    n_total_fvgs: int, n_after_filter: int, n_entered: int,
    stats_q4: dict, diag_q4: dict,
    stats_q1: dict, diag_q1: dict,
    stats_all: dict, diag_all: dict,
    stats_rand: dict, diag_rand: dict,
    cost_grid: List[Tuple[float, dict]],
    sl_tp_grid: List[Tuple[float, float, dict]],
) -> str:
    L: List[str] = []
    L.append("=" * 78)
    L.append(f"FVG P&L Backtest v2 - {symbol} - {tf}")
    L.append("=" * 78)
    L.append(f"Window:           {start} -> {end}")
    L.append(f"Reweighted score: {NEW_WEIGHTS}")
    L.append(f"Risk:             SL={risk_cfg.sl_atr_mult}xATR  TP={risk_cfg.tp_atr_mult}xATR  "
             f"time_stop={risk_cfg.time_stop_bars} bars  "
             f"risk={risk_cfg.risk_per_trade_pct*100:.2f}%/trade  "
             f"start_eq=${risk_cfg.starting_equity:,.0f}")
    L.append(f"Costs:            slippage={cost_cfg.slippage_pips} pips/side "
             f"(pip={cost_cfg.pip_size})  cost_mult={cost_cfg.cost_multiplier:.1f}x")
    L.append(f"Min gap pips:     {min_gap_pips} (absolute floor; 0 = off)")
    L.append("")
    L.append(f"Detected FVGs:    {n_total_fvgs:,}")
    L.append(f"After gap filter: {n_after_filter:,}")
    L.append(f"Entered FVGs:     {n_entered:,}")
    L.append(f"Score quartile cutoffs (entered): Q1={score_q1:.3f}  "
             f"median={score_med:.3f}  Q3={score_q3:.3f}")
    L.append("")

    L.append("PRIMARY (Q4-only) AND BASELINES")
    L.append("-" * 78)
    L.append(fmt_stats_row("Q4 (primary)", stats_q4))
    L.append(fmt_stats_row("Q1 (sanity)",  stats_q1))
    L.append(fmt_stats_row("All FVGs",      stats_all))
    L.append(fmt_stats_row("All random",   stats_rand))
    L.append("")
    L.append("Diagnostics (cand / taken / overlap / unfillable):")
    for d in (diag_q4, diag_q1, diag_all, diag_rand):
        L.append(f"  {d['label']:<14} {d['candidates']:>5} / {d['trades_taken']:>5} "
                 f"/ {d['skipped_overlap']:>5} / {d['skipped_unfillable']:>5}")
    L.append("")

    L.append("Q4 EXIT-REASON BREAKDOWN")
    L.append("-" * 78)
    L.append(f"  TP    : {stats_q4['n_tp']:>4}   "
             f"SL    : {stats_q4['n_sl']:>4}   "
             f"TIME  : {stats_q4['n_time']:>4}   "
             f"LONG  : {stats_q4['n_long']:>4}   "
             f"SHORT : {stats_q4['n_short']:>4}")
    L.append(f"  Avg bars held       : {stats_q4['avg_bars_held']:.1f}")
    L.append(f"  Longest loss streak : {stats_q4['longest_loss_streak']}")
    L.append("")

    L.append("COST STRESS TEST (Q4 at varying cost multipliers)")
    L.append("-" * 78)
    for mult, st in cost_grid:
        L.append(fmt_stats_row(f"cost x{mult:.1f}", st))
    L.append("")

    L.append("SL / TP GRID (Q4)")
    L.append("-" * 78)
    for sl, tp, st in sl_tp_grid:
        L.append(fmt_stats_row(f"SL{sl:.1f}/TP{tp:.1f}", st))
    L.append("")

    L.append("ACCEPTANCE GATES (Phase 1 -> production-track validation)")
    L.append("-" * 78)
    gate_n   = stats_q4["n"] >= 100
    gate_pf  = stats_q4["profit_factor"] >= 1.4
    gate_dd  = stats_q4["max_dd_pct"] < 15.0
    gate_shp = stats_q4["sharpe"] >= 1.0
    pf_2x = next((st["profit_factor"] for m, st in cost_grid if abs(m - 2.0) < 1e-6),
                 float("nan"))
    gate_cost = math.isfinite(pf_2x) and pf_2x >= 1.0
    gate_edge = (stats_q4["profit_factor"] > stats_all["profit_factor"]
                 and stats_q4["profit_factor"] > stats_q1["profit_factor"])
    gates = [
        ("n_trades >= 100",                gate_n,   f"{stats_q4['n']}"),
        ("Q4 PF >= 1.4",                   gate_pf,  f"{stats_q4['profit_factor']:.2f}"),
        ("Q4 max DD < 15%",                gate_dd,  f"{stats_q4['max_dd_pct']:.2f}%"),
        ("Q4 Sharpe >= 1.0",               gate_shp, f"{stats_q4['sharpe']:.2f}"),
        ("Q4 still PF >= 1.0 at 2x cost",  gate_cost, f"{pf_2x:.2f}"),
        ("Q4 PF > All AND Q1 PF",          gate_edge,
            f"Q4={stats_q4['profit_factor']:.2f} "
            f"All={stats_all['profit_factor']:.2f} "
            f"Q1={stats_q1['profit_factor']:.2f}"),
    ]
    for gname, ok, val in gates:
        tag = "PASS" if ok else "FAIL"
        L.append(f"  {gname:<32} {tag}  ({val})")
    all_pass = all(ok for _, ok, _ in gates)
    L.append("")
    if all_pass:
        L.append("VERDICT: ACCEPT. Reweighted-Q4 structurally profitable in this window")
        L.append("         with realistic costs and stops. Next steps:")
        L.append("           1. Walk-forward split (train Q3 cutoff on first half,")
        L.append("              evaluate on second half).")
        L.append("           2. Replicate on the OTHER symbols at this cost/risk profile.")
        L.append("           3. Promote to shadow on VPS in parallel with S59.")
    else:
        L.append("VERDICT: REJECT for this configuration. Compare against the other")
        L.append("         configurations in fvg_pnl_backtest_v2/ to see which lever")
        L.append("         (TF, symbol, gap-pips floor) closes the gap.")
    L.append("=" * 78)

    text = "\n".join(L) + "\n"
    out_path.write_text(text)
    return text


def plot_equity_curve(
    trades_q4: List[Trade], trades_q1: List[Trade], trades_all: List[Trade],
    trades_rand: List[Trade], starting_equity: float, out_path: Path,
) -> None:
    if not (trades_q4 or trades_q1 or trades_all or trades_rand):
        return
    fig, ax = plt.subplots(2, 1, figsize=(11, 9))

    def curve_usd(trs: List[Trade]) -> Tuple[np.ndarray, np.ndarray]:
        if not trs:
            return np.array([]), np.array([starting_equity])
        x = np.arange(len(trs) + 1)
        y = np.concatenate([[starting_equity], np.array([t.equity_after for t in trs])])
        return x, y

    def curve_R(trs: List[Trade]) -> Tuple[np.ndarray, np.ndarray]:
        if not trs:
            return np.array([]), np.array([0.0])
        x = np.arange(len(trs) + 1)
        y = np.concatenate([[0.0], np.cumsum(np.array([t.pnl_R for t in trs]))])
        return x, y

    series = [
        ("Q4 (primary)", trades_q4,   "seagreen", 2.5),
        ("Q1 (sanity)",  trades_q1,   "indianred", 1.5),
        ("All FVGs",      trades_all,  "steelblue", 1.5),
        ("All random",   trades_rand, "gray",      1.2),
    ]
    for label, trs, color, lw in series:
        x, y = curve_usd(trs)
        if y.size > 1:
            ax[0].plot(x, y, color=color, lw=lw, label=f"{label} (n={len(trs)})")
        x, y = curve_R(trs)
        if y.size > 1:
            ax[1].plot(x, y, color=color, lw=lw, label=f"{label} (n={len(trs)})")

    ax[0].axhline(starting_equity, color="black", ls=":", lw=0.8)
    ax[0].set_xlabel("trade #")
    ax[0].set_ylabel("equity ($)")
    ax[0].set_title("Equity curve - dollars")
    ax[0].grid(alpha=0.3)
    ax[0].legend(loc="best")

    ax[1].axhline(0.0, color="black", ls=":", lw=0.8)
    ax[1].set_xlabel("trade #")
    ax[1].set_ylabel("cumulative R")
    ax[1].set_title("Equity curve - R-multiples (sizing-independent)")
    ax[1].grid(alpha=0.3)
    ax[1].legend(loc="best")

    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------
def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--symbol", default="USDJPY")
    ap.add_argument("--tf", default="15min",
                    help="Target timeframe (15min/1h/4h). If pickle missing for "
                         "non-15min TFs, the 15min pickle is auto-resampled.")
    ap.add_argument("--start", default="2025-09-01")
    ap.add_argument("--end", default="2026-03-01")
    ap.add_argument("--sl-atr", type=float, default=1.0)
    ap.add_argument("--tp-atr", type=float, default=2.0)
    ap.add_argument("--time-stop", type=int, default=30)
    ap.add_argument("--slippage-pips", type=float, default=0.5)
    ap.add_argument("--risk-pct", type=float, default=0.005)
    ap.add_argument("--starting-equity", type=float, default=100_000.0)
    ap.add_argument("--min-gap-pips", type=float, default=0.0,
                    help="Absolute minimum gap height (pips). Default 0 = off.")
    ap.add_argument("--out-dir", type=Path,
                    default=REPO_ROOT / "fvg_pnl_backtest_v2")
    return ap.parse_args()


def main() -> int:
    args = parse_args()
    symbol = args.symbol.upper()
    tf = args.tf

    pip = PIP_SIZE.get(symbol)
    if pip is None:
        print(f"[warn] {symbol} not in PIP_SIZE table; defaulting to 0.0001. "
              f"Edit fvg_pnl_backtest_v2.py PIP_SIZE if this is wrong.")
        pip = 0.0001

    bars_per_year = BARS_PER_YEAR_BY_TF.get(tf)
    if bars_per_year is None:
        print(f"[warn] no bars/year entry for tf={tf}; using 252*24 = 6048")
        bars_per_year = 252 * 24

    out_dir = args.out_dir / f"{symbol}_{tf}"
    out_dir.mkdir(parents=True, exist_ok=True)

    cfg = replace(FvgConfig(), timeframe=tf, **NEW_WEIGHTS)

    risk_cfg = RiskConfig(
        sl_atr_mult=args.sl_atr, tp_atr_mult=args.tp_atr,
        time_stop_bars=args.time_stop,
        risk_per_trade_pct=args.risk_pct,
        starting_equity=args.starting_equity,
    )
    cost_cfg = CostsConfig(slippage_pips=args.slippage_pips, pip_size=pip)
    min_gap_price = args.min_gap_pips * pip

    print()
    print("=" * 70)
    print(f" FVG P&L Backtest v2   {symbol} {tf}   {args.start} -> {args.end}")
    print("=" * 70)
    print(f"  reweighted weights:  {NEW_WEIGHTS}")
    print(f"  risk:                SL={risk_cfg.sl_atr_mult}xATR "
          f"TP={risk_cfg.tp_atr_mult}xATR "
          f"time_stop={risk_cfg.time_stop_bars}")
    print(f"  costs:               slip={cost_cfg.slippage_pips}pips/side "
          f"pip={cost_cfg.pip_size}")
    print(f"  min_gap_pips:        {args.min_gap_pips} "
          f"(price floor {min_gap_price})")

    t0 = time.time()
    bars = load_or_resample_bars(symbol, tf, args.start, args.end)
    bars = add_indicators(bars, cfg)
    print(f"[bars] {len(bars):,} {tf} bars + indicators in {time.time()-t0:.1f}s")

    fvgs_all = detect_fvgs(bars, cfg)
    measure_reactions(fvgs_all, bars, cfg)
    n_total = len(fvgs_all)

    rng = np.random.default_rng(seed=42)
    randoms_all = generate_random_levels(fvgs_all, bars, cfg, rng)
    measure_reactions(randoms_all, bars, cfg)

    # Apply absolute gap-size filter (in price units). Random levels share
    # gap_height with their source FVG, so filtering on gap_height keeps
    # the random/FVG comparison apples-to-apples.
    if min_gap_price > 0:
        fvgs = [fv for fv in fvgs_all if fv.gap_height >= min_gap_price]
        randoms = [fv for fv in randoms_all if fv.gap_height >= min_gap_price]
        print(f"[filter] min_gap_pips={args.min_gap_pips} -> "
              f"{len(fvgs):,}/{n_total:,} FVGs kept "
              f"({len(fvgs)/max(n_total,1)*100:.1f}%)")
    else:
        fvgs = fvgs_all
        randoms = randoms_all

    n_after_filter = len(fvgs)

    entered = [fv for fv in fvgs
               if fv.entry_idx is not None and fv.reaction != "no_entry"]
    n_entered = len(entered)
    print(f"[fvg] detected={n_total:,} after_filter={n_after_filter:,} "
          f"entered={n_entered:,}")

    score_q1, score_q3, score_med = compute_score_quartiles(entered)
    print(f"[score] cutoffs: Q1<={score_q1:.3f}  median={score_med:.3f}  "
          f"Q4>={score_q3:.3f}")

    INF, NINF = float("inf"), float("-inf")

    # Primary + sanity baselines
    trades_q4,  diag_q4  = run_backtest(fvgs, bars, risk_cfg, cost_cfg,
                                        (score_q3, INF), label="Q4")
    trades_q1,  diag_q1  = run_backtest(fvgs, bars, risk_cfg, cost_cfg,
                                        (NINF, score_q1), label="Q1")
    trades_all, diag_all = run_backtest(fvgs, bars, risk_cfg, cost_cfg,
                                        (NINF, INF), label="All")
    trades_rd,  diag_rd  = run_backtest(randoms, bars, risk_cfg, cost_cfg,
                                        score_filter=None, label="Rand")

    stats_q4  = compute_stats(trades_q4,  risk_cfg, bars_per_year)
    stats_q1  = compute_stats(trades_q1,  risk_cfg, bars_per_year)
    stats_all = compute_stats(trades_all, risk_cfg, bars_per_year)
    stats_rd  = compute_stats(trades_rd,  risk_cfg, bars_per_year)

    # Cost stress (Q4 only)
    cost_grid: List[Tuple[float, dict]] = []
    for mult in (0.0, 1.0, 2.0):
        cc = CostsConfig(slippage_pips=args.slippage_pips, pip_size=pip,
                         cost_multiplier=mult)
        trs, _ = run_backtest(fvgs, bars, risk_cfg, cc,
                              (score_q3, INF), label=f"Q4_cost_x{mult:.1f}")
        cost_grid.append((mult, compute_stats(trs, risk_cfg, bars_per_year)))

    # Wider SL/TP grid (Q4 only)
    sl_tp_grid: List[Tuple[float, float, dict]] = []
    for sl, tp in [(0.5, 1.5), (1.0, 2.0), (1.5, 3.0), (2.0, 4.0), (2.5, 5.0)]:
        rc = RiskConfig(
            sl_atr_mult=sl, tp_atr_mult=tp,
            time_stop_bars=risk_cfg.time_stop_bars,
            risk_per_trade_pct=risk_cfg.risk_per_trade_pct,
            starting_equity=risk_cfg.starting_equity,
        )
        trs, _ = run_backtest(fvgs, bars, rc, cost_cfg,
                              (score_q3, INF), label=f"Q4_SL{sl}_TP{tp}")
        sl_tp_grid.append((sl, tp, compute_stats(trs, rc, bars_per_year)))

    # Persist trade ledgers
    trades_to_df(trades_q4 ).to_csv(out_dir / "trades_q4.csv",  index=False)
    trades_to_df(trades_q1 ).to_csv(out_dir / "trades_q1.csv",  index=False)
    trades_to_df(trades_all).to_csv(out_dir / "trades_all.csv", index=False)
    trades_to_df(trades_rd ).to_csv(out_dir / "trades_random.csv", index=False)

    pd.DataFrame([
        {"cost_multiplier": m, **st} for m, st in cost_grid
    ]).to_csv(out_dir / "grid_costs.csv", index=False)
    pd.DataFrame([
        {"sl_atr": sl, "tp_atr": tp, **st} for sl, tp, st in sl_tp_grid
    ]).to_csv(out_dir / "grid_sl_tp.csv", index=False)

    plot_equity_curve(trades_q4, trades_q1, trades_all, trades_rd,
                      risk_cfg.starting_equity, out_dir / "equity_curve.png")

    summary_text = write_summary(
        out_dir / "summary.txt",
        symbol, tf, args.start, args.end,
        risk_cfg, cost_cfg, args.min_gap_pips,
        score_q1, score_q3, score_med,
        n_total, n_after_filter, n_entered,
        stats_q4, diag_q4,
        stats_q1, diag_q1,
        stats_all, diag_all,
        stats_rd, diag_rd,
        cost_grid, sl_tp_grid,
    )
    print()
    print(summary_text)
    print(f"All outputs written to: {out_dir}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except FileNotFoundError as e:
        print(f"\n[error] {e}", file=sys.stderr)
        sys.exit(2)
    except Exception:
        traceback.print_exc()
        sys.exit(1)

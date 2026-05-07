#!/usr/bin/env python3
"""
fvg_pnl_backtest_v3.py
======================

Successor to fvg_pnl_backtest_v2.py with two new levers tuned at the
XAUUSD 15m result that came closest to clearing the acceptance gates
(PF 1.14, Sharpe 3.96, DD 10.4%, 4 of 6 gates):

  1. TIGHTER QUANTILE FILTER (--top-pct)
     v1/v2 used Q4 = top quartile (top 25%). v3 generalizes to any
     percentile. --top-pct 0.10 means top decile. The "Q1 sanity"
     baseline mirrors at the same percentile from the bottom.

  2. BREAKEVEN STOP MOVE (--breakeven-after-atr)
     Once price moves favorably by N x atr_at_entry from the gross
     entry, SL slides to the gross entry price. A subsequent stop
     gets you out at gross_entry, which after both costs equals
     -2 x side_cost (a small loss equal to round-trip friction
     instead of the full -1.0R). TP and time-stop logic unchanged.
     N=0 disables; default 1.0 (slide once price reaches +1 ATR).

  3. WALK-FORWARD SPLIT (--train-end)   [added 2026-05-02]
     If --train-end DATE is supplied, v3 detects FVGs over the full
     --start / --end window, computes the score cutoffs ONLY from
     FVGs whose entry_time < DATE, then trades and gates ONLY FVGs
     whose entry_time >= DATE. The acceptance-gate evaluator auto-
     relaxes to the pre-registered walk-forward thresholds (n>=50,
     PF>=1.2, PF>All, cost-stress 2x PF>=1.0). Cutoffs are thus
     selected on in-sample data and applied unchanged to out-of-
     sample data, with no look-ahead. If --train-end is not
     supplied, behavior is bit-identical to prior v3 (single-window
     run, six-gate evaluator).

NEW FILE. Does NOT modify any tracked code (including v1, v2). Imports
the existing detection / scoring / reaction / random-control functions
and overrides only the FvgConfig score weights via dataclasses.replace().

Usage (XAUUSD-targeted, the closest-to-passing configuration)
-------------------------------------------------------------
    cd ~/omega_repo

    # Single-window in-sample run (the accepted v3 #5 config):
    python3 scripts/fvg_pnl_backtest_v3.py --symbol XAUUSD --tf 15min \\
        --top-pct 0.10 --breakeven-after-atr 0.0 \\
        --sl-atr 2.5 --tp-atr 5.0 --time-stop 60

    # Walk-forward TEST run (cutoffs from train half, gates on test half):
    python3 scripts/fvg_pnl_backtest_v3.py --symbol XAUUSD --tf 15min \\
        --start 2025-09-01 --end 2026-03-01 --train-end 2025-12-01 \\
        --top-pct 0.10 --breakeven-after-atr 0.0 \\
        --sl-atr 2.5 --tp-atr 5.0 --time-stop 60

Output landing
--------------
fvg_pnl_backtest_v3/<SYMBOL>_<TF>_top{pct}_be{atr}[_sl{x}_tp{y}][_wf{train_end}]/
    summary.txt
    trades_top.csv / trades_bot.csv / trades_all.csv / trades_random.csv
    grid_costs.csv / grid_sl_tp.csv / grid_breakeven.csv
    equity_curve.png

Pre-registered acceptance gates: six-gate set in single-window mode,
four-gate walk-forward set when --train-end is supplied.
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


NEW_WEIGHTS = {
    "w_gap_size":     1.5,
    "w_displacement": 1.0,
    "w_tick_volume":  1.0,
    "w_trend_align":  0.0,
    "w_age_decay":    0.0,
}


PIP_SIZE = {
    "USDJPY": 0.01,
    "EURUSD": 0.0001, "GBPUSD": 0.0001, "AUDUSD": 0.0001,
    "USDCAD": 0.0001, "USDCHF": 0.0001, "NZDUSD": 0.0001,
    "EURJPY": 0.01, "GBPJPY": 0.01, "AUDJPY": 0.01,
    "EURGBP": 0.0001, "EURAUD": 0.0001, "CHFJPY": 0.01,
    "CADJPY": 0.01, "AUDNZD": 0.0001,
    "XAUUSD": 0.10, "XAGUSD": 0.005,
    "NAS":    1.0,
}


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
    slippage_pips: float = 0.5
    pip_size: float = 0.01
    half_spread_floor: float = 0.0
    cost_multiplier: float = 1.0


@dataclass
class RiskConfig:
    sl_atr_mult: float = 1.0
    tp_atr_mult: float = 2.0
    time_stop_bars: int = 30
    risk_per_trade_pct: float = 0.005
    starting_equity: float = 100_000.0
    breakeven_after_atr: float = 1.0   # 0 = off; otherwise slide SL to gross_entry
                                       # once max favorable move >= N * atr_at_entry


@dataclass
class Trade:
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
    exit_reason: str           # "tp" / "sl" / "be_sl" / "time_stop" / "end_of_data"
    breakeven_armed: bool
    pnl_pips: float
    pnl_R: float
    pnl_dollars: float
    equity_after: float
    bars_held: int


# ---------------------------------------------------------------------------
# Bar resampling (15min -> any larger TF), same as v2
# ---------------------------------------------------------------------------
def resample_bars(bars_15m: pd.DataFrame, target_tf: str) -> pd.DataFrame:
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
    target_dir = REPO_ROOT / "fvg_phase0" / f"{symbol}_{tf}"
    target_pkl = target_dir / f"bars_{symbol}_{tf}_{start}_{end}.pkl"
    if target_pkl.exists():
        print(f"[load] {target_pkl.relative_to(REPO_ROOT)}")
        return pd.read_pickle(target_pkl)
    if tf == "15min":
        raise FileNotFoundError(
            f"Cached 15min bars not found at {target_pkl}.\n"
            f"  Run scripts/usdjpy_xauusd_fvg_signal_test.py first for {symbol}."
        )
    src_pkl = REPO_ROOT / "fvg_phase0" / f"{symbol}_15min" \
              / f"bars_{symbol}_15min_{start}_{end}.pkl"
    if not src_pkl.exists():
        raise FileNotFoundError(
            f"Neither {target_pkl} nor 15min source {src_pkl} exists."
        )
    print(f"[load] {src_pkl.relative_to(REPO_ROOT)} (resampling -> {tf})")
    bars_15m = pd.read_pickle(src_pkl)
    bars = resample_bars(bars_15m, tf)
    print(f"[resample] {len(bars_15m):,} 15min bars -> {len(bars):,} {tf} bars")
    target_dir.mkdir(parents=True, exist_ok=True)
    bars.to_pickle(target_pkl)
    print(f"[cache] wrote {target_pkl.relative_to(REPO_ROOT)}")
    return bars


# ---------------------------------------------------------------------------
# Score-percentile cutoffs (generalizes v1/v2's quartile cutoffs)
# ---------------------------------------------------------------------------
def compute_score_cutoffs(
    entered_fvgs: List[Fvg], top_pct: float
) -> Tuple[float, float, float]:
    """Return (bot_cutoff, top_cutoff, median).
    Top pool = score >= top_cutoff (top top_pct).
    Bot pool = score <= bot_cutoff (bottom top_pct, mirrored).
    """
    if not 0 < top_pct < 0.5:
        raise ValueError(f"top_pct must be in (0, 0.5); got {top_pct}")
    scores = np.array(
        [fv.score_at_entry for fv in entered_fvgs if fv.score_at_entry is not None],
        dtype=float,
    )
    if scores.size < 10:
        raise RuntimeError(
            f"Only {scores.size} entered FVGs with valid scores - cannot bucket. "
            f"Widen the date range or relax filters."
        )
    bot = float(np.quantile(scores, top_pct))
    top = float(np.quantile(scores, 1.0 - top_pct))
    median = float(np.quantile(scores, 0.5))
    return bot, top, median


# ---------------------------------------------------------------------------
# Single-trade simulator (now with breakeven SL slide)
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

    # Breakeven trigger: when MAX favorable move (in ATR units from gross_entry)
    # crosses risk_cfg.breakeven_after_atr, slide SL to gross_entry. Done once.
    be_active = risk_cfg.breakeven_after_atr > 0.0
    be_threshold = risk_cfg.breakeven_after_atr * atr_e
    be_armed = False

    last = min(n - 1, entry_idx + risk_cfg.time_stop_bars)
    exit_idx = -1
    exit_reason = ""
    gross_exit = float("nan")

    for k in range(entry_idx + 1, last + 1):
        bar_h = h_arr[k]
        bar_l = l_arr[k]
        if not (math.isfinite(bar_h) and math.isfinite(bar_l)):
            continue

        # Update best favorable excursion (used for breakeven trigger).
        # We measure favorable from gross_entry, not entry_price, so the
        # threshold is in pure ATR units regardless of cost.
        if direction == "long":
            fav_excursion = bar_h - gross_entry
        else:
            fav_excursion = gross_entry - bar_l

        # Arm breakeven if reached threshold this bar.
        if be_active and not be_armed and fav_excursion >= be_threshold:
            be_armed = True
            if direction == "long":
                sl = gross_entry  # slide SL up to entry
            else:
                sl = gross_entry  # slide SL down to entry

        if direction == "long":
            sl_hit = bar_l <= sl
            tp_hit = bar_h >= tp
        else:
            sl_hit = bar_h >= sl
            tp_hit = bar_l <= tp

        if sl_hit and tp_hit:
            exit_idx = k
            exit_reason = "be_sl" if be_armed and abs(sl - gross_entry) < 1e-12 else "sl"
            gross_exit = sl
            break
        if sl_hit:
            exit_idx = k
            exit_reason = "be_sl" if be_armed and abs(sl - gross_entry) < 1e-12 else "sl"
            gross_exit = sl
            break
        if tp_hit:
            exit_idx = k
            exit_reason = "tp"
            gross_exit = tp
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
        breakeven_armed=bool(be_armed),
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
    trades: List[Trade], risk_cfg: RiskConfig, bars_per_year: float,
) -> dict:
    n = len(trades)
    if n == 0:
        return {
            "n": 0, "win_rate": 0.0, "avg_R": 0.0, "expectancy_R": 0.0,
            "profit_factor": 0.0, "total_R": 0.0, "total_pips": 0.0,
            "total_dollars": 0.0, "max_dd_R": 0.0, "max_dd_dollars": 0.0,
            "max_dd_pct": 0.0, "sharpe": 0.0, "longest_loss_streak": 0,
            "avg_bars_held": 0.0, "n_tp": 0, "n_sl": 0, "n_be_sl": 0,
            "n_time": 0, "n_long": 0, "n_short": 0, "be_armed_pct": 0.0,
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
        "n_tp":    int(sum(1 for t in trades if t.exit_reason == "tp")),
        "n_sl":    int(sum(1 for t in trades if t.exit_reason == "sl")),
        "n_be_sl": int(sum(1 for t in trades if t.exit_reason == "be_sl")),
        "n_time":  int(sum(1 for t in trades if t.exit_reason in ("time_stop", "end_of_data"))),
        "n_long":  int(sum(1 for t in trades if t.direction == "long")),
        "n_short": int(sum(1 for t in trades if t.direction == "short")),
        "be_armed_pct": float(sum(1 for t in trades if t.breakeven_armed) / n * 100.0),
    }


def trades_to_df(trades: List[Trade]) -> pd.DataFrame:
    if not trades:
        return pd.DataFrame()
    return pd.DataFrame([{
        "fvg_formed_idx": t.fvg_formed_idx,
        "direction": t.direction,
        "entry_idx": t.entry_idx,
        "entry_time": t.entry_time,
        "entry_price": t.entry_price,
        "sl": t.sl, "tp": t.tp,
        "score_at_entry": t.score_at_entry,
        "atr_at_entry": t.atr_at_entry,
        "gap_height": t.gap_height,
        "session": t.session,
        "exit_idx": t.exit_idx,
        "exit_time": t.exit_time,
        "exit_price": t.exit_price,
        "exit_reason": t.exit_reason,
        "breakeven_armed": t.breakeven_armed,
        "pnl_pips": t.pnl_pips, "pnl_R": t.pnl_R,
        "pnl_dollars": t.pnl_dollars,
        "equity_after": t.equity_after,
        "bars_held": t.bars_held,
    } for t in trades])


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
    risk_cfg: RiskConfig, cost_cfg: CostsConfig, top_pct: float,
    score_bot: float, score_top: float, score_med: float,
    n_total: int, n_entered: int,
    stats_top: dict, diag_top: dict,
    stats_bot: dict, diag_bot: dict,
    stats_all: dict, diag_all: dict,
    stats_rand: dict, diag_rand: dict,
    cost_grid: List[Tuple[float, dict]],
    sl_tp_grid: List[Tuple[float, float, dict]],
    be_grid: List[Tuple[float, dict]],
    train_end_ts: Optional[pd.Timestamp] = None,
    n_entered_train: Optional[int] = None,
    n_entered_test: Optional[int] = None,
) -> str:
    is_wf = train_end_ts is not None
    L: List[str] = []
    L.append("=" * 78)
    title_suffix = (
        f"   WALK-FORWARD (train_end={train_end_ts.date()})" if is_wf else ""
    )
    L.append(f"FVG P&L Backtest v3 - {symbol} - {tf}{title_suffix}")
    L.append("=" * 78)
    if is_wf:
        L.append(f"Full window:      {start} -> {end}  (used for FVG detection only)")
        L.append(f"Train window:     {start} -> {train_end_ts.date()}  "
                 f"(score cutoffs computed here ONLY)")
        L.append(f"Test window:      {train_end_ts.date()} -> {end}  "
                 f"(trades + acceptance gates evaluated here ONLY)")
    else:
        L.append(f"Window:           {start} -> {end}")
    L.append(f"Reweighted score: {NEW_WEIGHTS}")
    L.append(f"Top pool:         top {top_pct*100:.1f}% of entered FVGs by score")
    L.append(f"Risk:             SL={risk_cfg.sl_atr_mult}xATR  "
             f"TP={risk_cfg.tp_atr_mult}xATR  "
             f"BE={risk_cfg.breakeven_after_atr}xATR  "
             f"time_stop={risk_cfg.time_stop_bars} bars  "
             f"risk={risk_cfg.risk_per_trade_pct*100:.2f}%/trade  "
             f"start_eq=${risk_cfg.starting_equity:,.0f}")
    L.append(f"Costs:            slippage={cost_cfg.slippage_pips} pips/side "
             f"(pip={cost_cfg.pip_size})  cost_mult={cost_cfg.cost_multiplier:.1f}x")
    L.append("")
    L.append(f"Detected FVGs:    {n_total:,}")
    if is_wf:
        L.append(f"Entered FVGs:     {n_entered:,} total  "
                 f"({n_entered_train:,} train / {n_entered_test:,} test)")
        L.append(f"Score cutoffs:    bot<={score_bot:.3f}  median={score_med:.3f}  "
                 f"top>={score_top:.3f}  (from TRAIN window only)")
    else:
        L.append(f"Entered FVGs:     {n_entered:,}")
        L.append(f"Score cutoffs:    bot<={score_bot:.3f}  median={score_med:.3f}  "
                 f"top>={score_top:.3f}")
    L.append("")

    primary_label = "TEST-WINDOW" if is_wf else "PRIMARY"
    L.append(f"{primary_label} (TOP-pool) AND BASELINES")
    L.append("-" * 78)
    L.append(fmt_stats_row(f"Top {top_pct*100:.0f}%", stats_top))
    L.append(fmt_stats_row(f"Bot {top_pct*100:.0f}%", stats_bot))
    L.append(fmt_stats_row("All FVGs",                stats_all))
    L.append(fmt_stats_row("All random",              stats_rand))
    L.append("")
    L.append("Diagnostics (cand / taken / overlap / unfillable):")
    for d in (diag_top, diag_bot, diag_all, diag_rand):
        L.append(f"  {d['label']:<14} {d['candidates']:>5} / {d['trades_taken']:>5} "
                 f"/ {d['skipped_overlap']:>5} / {d['skipped_unfillable']:>5}")
    L.append("")

    L.append("TOP-pool EXIT-REASON BREAKDOWN")
    L.append("-" * 78)
    L.append(f"  TP : {stats_top['n_tp']:>4}   "
             f"SL : {stats_top['n_sl']:>4}   "
             f"BE_SL : {stats_top['n_be_sl']:>4}   "
             f"TIME : {stats_top['n_time']:>4}   "
             f"LONG : {stats_top['n_long']:>4}   "
             f"SHORT : {stats_top['n_short']:>4}")
    L.append(f"  BE armed : {stats_top['be_armed_pct']:.1f}% of trades")
    L.append(f"  Avg bars held       : {stats_top['avg_bars_held']:.1f}")
    L.append(f"  Longest loss streak : {stats_top['longest_loss_streak']}")
    L.append("")

    L.append("COST STRESS TEST (Top pool at varying cost multipliers)")
    L.append("-" * 78)
    for mult, st in cost_grid:
        L.append(fmt_stats_row(f"cost x{mult:.1f}", st))
    L.append("")

    L.append("BREAKEVEN GRID (Top pool, default SL/TP)")
    L.append("-" * 78)
    for be, st in be_grid:
        be_label = "BE off" if be == 0.0 else f"BE +{be:.1f}xATR"
        L.append(fmt_stats_row(be_label, st))
    L.append("")

    L.append("SL / TP GRID (Top pool, current BE setting)")
    L.append("-" * 78)
    for sl, tp, st in sl_tp_grid:
        L.append(fmt_stats_row(f"SL{sl:.1f}/TP{tp:.1f}", st))
    L.append("")

    # Acceptance gates (the set switches between single-window and walk-forward).
    pf_2x = next((st["profit_factor"] for m, st in cost_grid if abs(m - 2.0) < 1e-6),
                 float("nan"))
    if is_wf:
        L.append("ACCEPTANCE GATES (Walk-forward / OOS - relaxed thresholds)")
        L.append("-" * 78)
        gate_n    = stats_top["n"] >= 50
        gate_pf   = stats_top["profit_factor"] >= 1.2
        gate_cost = math.isfinite(pf_2x) and pf_2x >= 1.0
        gate_edge = stats_top["profit_factor"] > stats_all["profit_factor"]
        gates = [
            ("WF: test n_trades >= 50",            gate_n,
                f"{stats_top['n']}"),
            ("WF: test Top PF >= 1.2",             gate_pf,
                f"{stats_top['profit_factor']:.2f}"),
            ("WF: test Top PF >= 1.0 at 2x cost",  gate_cost,
                f"{pf_2x:.2f}"),
            ("WF: test Top PF > All",              gate_edge,
                f"Top={stats_top['profit_factor']:.2f} "
                f"All={stats_all['profit_factor']:.2f}"),
        ]
    else:
        L.append("ACCEPTANCE GATES (Phase 1 -> production-track validation)")
        L.append("-" * 78)
        gate_n   = stats_top["n"] >= 100
        gate_pf  = stats_top["profit_factor"] >= 1.4
        gate_dd  = stats_top["max_dd_pct"] < 15.0
        gate_shp = stats_top["sharpe"] >= 1.0
        gate_cost = math.isfinite(pf_2x) and pf_2x >= 1.0
        gate_edge = (stats_top["profit_factor"] > stats_all["profit_factor"]
                     and stats_top["profit_factor"] > stats_bot["profit_factor"])
        gates = [
            ("n_trades >= 100",                gate_n,    f"{stats_top['n']}"),
            ("Top PF >= 1.4",                  gate_pf,   f"{stats_top['profit_factor']:.2f}"),
            ("Top max DD < 15%",               gate_dd,   f"{stats_top['max_dd_pct']:.2f}%"),
            ("Top Sharpe >= 1.0",              gate_shp,  f"{stats_top['sharpe']:.2f}"),
            ("Top still PF >= 1.0 at 2x cost", gate_cost, f"{pf_2x:.2f}"),
            ("Top PF > All AND Bot",           gate_edge,
                f"Top={stats_top['profit_factor']:.2f} "
                f"All={stats_all['profit_factor']:.2f} "
                f"Bot={stats_bot['profit_factor']:.2f}"),
        ]
    for gname, ok, val in gates:
        tag = "PASS" if ok else "FAIL"
        L.append(f"  {gname:<36} {tag}  ({val})")
    all_pass = all(ok for _, ok, _ in gates)
    L.append("")
    if all_pass:
        if is_wf:
            L.append("VERDICT: WALK-FORWARD ACCEPT. Test-window result clears the relaxed")
            L.append("         OOS gates with cutoffs frozen on the train half. Edge")
            L.append("         survives the in-sample / out-of-sample split.")
            L.append("         Next steps:")
            L.append("           1. Replicate the accepted config on NAS 15m (no")
            L.append("              further re-tuning).")
            L.append("           2. Promote XAUUSD to VPS shadow ALONGSIDE (not")
            L.append("              displacing) the S59 USDJPY Asian-Open shadow.")
        else:
            L.append("VERDICT: ACCEPT. Top-pool reweighted FVG strategy clears all gates")
            L.append("         in this window. Next steps:")
            L.append("           1. Walk-forward split (train cutoffs on first half,")
            L.append("              evaluate on second half).")
            L.append("           2. Replicate on at least one other symbol at this same")
            L.append("              risk/cost profile.")
            L.append("           3. Promote to VPS shadow alongside (NOT displacing) S59.")
    else:
        if is_wf:
            L.append("VERDICT: WALK-FORWARD REJECT. The IS gate-pass did NOT carry over")
            L.append("         to the test window. The accepted v3 config is overfit to")
            L.append("         lever-tuning on this data. Strategy goes back to the")
            L.append("         drawing board - do NOT promote to VPS shadow.")
        else:
            L.append("VERDICT: REJECT for this configuration. Check the breakeven and")
            L.append("         SL/TP grids - they show where the surface peaks.")
    L.append("=" * 78)

    text = "\n".join(L) + "\n"
    out_path.write_text(text)
    return text


def plot_equity_curve(
    trades_top: List[Trade], trades_bot: List[Trade], trades_all: List[Trade],
    trades_rand: List[Trade], starting_equity: float, out_path: Path,
) -> None:
    if not (trades_top or trades_bot or trades_all or trades_rand):
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
        ("Top (primary)", trades_top,  "seagreen", 2.5),
        ("Bot (sanity)",  trades_bot,  "indianred", 1.5),
        ("All FVGs",       trades_all,  "steelblue", 1.5),
        ("All random",    trades_rand, "gray",      1.2),
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
    ax[1].set_title("Equity curve - R-multiples")
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
    ap.add_argument("--symbol", default="XAUUSD")
    ap.add_argument("--tf", default="15min")
    ap.add_argument("--start", default="2025-09-01")
    ap.add_argument("--end", default="2026-03-01")
    ap.add_argument("--sl-atr", type=float, default=1.0)
    ap.add_argument("--tp-atr", type=float, default=2.0)
    ap.add_argument("--time-stop", type=int, default=30)
    ap.add_argument("--slippage-pips", type=float, default=0.5)
    ap.add_argument("--risk-pct", type=float, default=0.005)
    ap.add_argument("--starting-equity", type=float, default=100_000.0)
    ap.add_argument("--top-pct", type=float, default=0.10,
                    help="Top score percentile to trade (e.g. 0.10 = top decile). "
                         "Mirrors at the bottom for the sanity baseline.")
    ap.add_argument("--breakeven-after-atr", type=float, default=1.0,
                    help="Slide SL to gross_entry once price moves favorably by "
                         "this many ATRs. 0 = off.")
    ap.add_argument("--train-end", type=str, default=None,
                    help="Walk-forward split. If supplied, FVGs are detected "
                         "across the full --start/--end window, but score "
                         "cutoffs are computed ONLY from FVGs whose entry_time "
                         "< TRAIN_END, and trades + acceptance gates run ONLY "
                         "on FVGs whose entry_time >= TRAIN_END. Acceptance "
                         "gates auto-relax to the pre-registered walk-forward "
                         "thresholds (n>=50, PF>=1.2, PF>All, cost-stress 2x "
                         "PF>=1.0). Format: YYYY-MM-DD. Unset = single-window run.")
    ap.add_argument("--out-dir", type=Path,
                    default=REPO_ROOT / "fvg_pnl_backtest_v3")
    return ap.parse_args()


def main() -> int:
    args = parse_args()
    symbol = args.symbol.upper()
    tf = args.tf

    pip = PIP_SIZE.get(symbol)
    if pip is None:
        print(f"[warn] {symbol} not in PIP_SIZE table; defaulting to 0.0001.")
        pip = 0.0001

    bars_per_year = BARS_PER_YEAR_BY_TF.get(tf, 252 * 24)

    # --- Walk-forward arg parsing & validation --------------------------------
    train_end_ts: Optional[pd.Timestamp] = None
    if args.train_end is not None:
        try:
            train_end_ts = pd.Timestamp(args.train_end)
        except Exception as e:
            raise SystemExit(
                f"[error] --train-end '{args.train_end}' is not a parseable "
                f"date: {e}"
            )
        start_ts = pd.Timestamp(args.start)
        end_ts = pd.Timestamp(args.end)
        if not (start_ts < train_end_ts < end_ts):
            raise SystemExit(
                f"[error] --train-end ({train_end_ts.date()}) must lie strictly "
                f"between --start ({start_ts.date()}) and --end ({end_ts.date()})."
            )

    # Output subdir naming. SL/TP suffix only when non-default so existing
    # default-SL/TP run folders keep their names. Walk-forward runs additionally
    # get a _wf{train_end} suffix so they can never overwrite a single-window run.
    sl_tp_suffix = ""
    if not (abs(args.sl_atr - 1.0) < 1e-9 and abs(args.tp_atr - 2.0) < 1e-9):
        sl_tp_suffix = f"_sl{args.sl_atr:.1f}_tp{args.tp_atr:.1f}"
    wf_suffix = (
        f"_wf{train_end_ts.date().isoformat()}"
        if train_end_ts is not None else ""
    )
    out_subdir = (
        f"{symbol}_{tf}"
        f"_top{int(round(args.top_pct*100))}"
        f"_be{args.breakeven_after_atr:.1f}"
        f"{sl_tp_suffix}"
        f"{wf_suffix}"
    )
    out_dir = args.out_dir / out_subdir
    out_dir.mkdir(parents=True, exist_ok=True)

    cfg = replace(FvgConfig(), timeframe=tf, **NEW_WEIGHTS)

    risk_cfg = RiskConfig(
        sl_atr_mult=args.sl_atr, tp_atr_mult=args.tp_atr,
        time_stop_bars=args.time_stop,
        risk_per_trade_pct=args.risk_pct,
        starting_equity=args.starting_equity,
        breakeven_after_atr=args.breakeven_after_atr,
    )
    cost_cfg = CostsConfig(slippage_pips=args.slippage_pips, pip_size=pip)

    print()
    print("=" * 70)
    banner_mode = (
        f"   WALK-FORWARD train_end={train_end_ts.date()}"
        if train_end_ts is not None else ""
    )
    print(f" FVG P&L Backtest v3   {symbol} {tf}   "
          f"{args.start} -> {args.end}{banner_mode}")
    print("=" * 70)
    print(f"  reweighted weights:  {NEW_WEIGHTS}")
    print(f"  top_pct:             {args.top_pct} "
          f"(top {args.top_pct*100:.0f}% / bot {args.top_pct*100:.0f}%)")
    print(f"  breakeven_after_atr: {args.breakeven_after_atr}")
    print(f"  risk:                SL={risk_cfg.sl_atr_mult}xATR "
          f"TP={risk_cfg.tp_atr_mult}xATR "
          f"time_stop={risk_cfg.time_stop_bars}")
    print(f"  costs:               slip={cost_cfg.slippage_pips}pips/side "
          f"pip={cost_cfg.pip_size}")
    if train_end_ts is not None:
        print(f"  walk-forward:        cutoffs from {args.start} -> "
              f"{train_end_ts.date()}  /  trades+gates on "
              f"{train_end_ts.date()} -> {args.end}")

    t0 = time.time()
    bars = load_or_resample_bars(symbol, tf, args.start, args.end)
    bars = add_indicators(bars, cfg)
    print(f"[bars] {len(bars):,} {tf} bars + indicators in {time.time()-t0:.1f}s")

    fvgs = detect_fvgs(bars, cfg)
    measure_reactions(fvgs, bars, cfg)
    n_total = len(fvgs)

    rng = np.random.default_rng(seed=42)
    randoms = generate_random_levels(fvgs, bars, cfg, rng)
    measure_reactions(randoms, bars, cfg)

    entered = [fv for fv in fvgs
               if fv.entry_idx is not None and fv.reaction != "no_entry"]
    n_entered = len(entered)
    print(f"[fvg] detected={n_total:,} entered={n_entered:,}")

    # --- Walk-forward partition: cutoffs from train, trades on test -----------
    n_entered_train: Optional[int] = None
    n_entered_test: Optional[int] = None
    if train_end_ts is not None:
        bar_index = bars.index
        # Align train_end_ts's tz to the bar index's tz before any comparison.
        # The bar pickle is stored tz-aware (UTC); pd.Timestamp("YYYY-MM-DD")
        # parses tz-naive; pandas refuses to compare the two without help.
        bar_tz = bar_index.tz
        if bar_tz is not None and train_end_ts.tzinfo is None:
            train_end_cmp = train_end_ts.tz_localize(bar_tz)
        elif bar_tz is None and train_end_ts.tzinfo is not None:
            train_end_cmp = train_end_ts.tz_localize(None)
        else:
            train_end_cmp = train_end_ts

        entered_train = [
            fv for fv in entered
            if bar_index[fv.entry_idx] < train_end_cmp
        ]
        entered_test = [
            fv for fv in entered
            if bar_index[fv.entry_idx] >= train_end_cmp
        ]
        n_entered_train = len(entered_train)
        n_entered_test = len(entered_test)
        print(f"[walk-forward] train_end={train_end_ts.date()}  "
              f"train_entered={n_entered_train}  "
              f"test_entered={n_entered_test}")
        if n_entered_train < 10:
            raise SystemExit(
                f"[error] Only {n_entered_train} entered FVGs in train window "
                f"({args.start} -> {train_end_ts.date()}); "
                f"compute_score_cutoffs requires >= 10. Widen the train window."
            )
        score_bot, score_top, score_med = compute_score_cutoffs(
            entered_train, args.top_pct
        )
        # Restrict trades and random control to the TEST half. Detection,
        # scoring and reaction measurement already ran on the full window so
        # FVGs that straddle the boundary still see correct context bars.
        fvgs_for_trade = [
            fv for fv in fvgs
            if fv.entry_idx is not None
            and bar_index[fv.entry_idx] >= train_end_cmp
        ]
        randoms_for_trade = [
            fv for fv in randoms
            if fv.entry_idx is not None
            and bar_index[fv.entry_idx] >= train_end_cmp
        ]
    else:
        score_bot, score_top, score_med = compute_score_cutoffs(
            entered, args.top_pct
        )
        fvgs_for_trade = fvgs
        randoms_for_trade = randoms

    cutoff_origin = (
        "  (computed from TRAIN window only)"
        if train_end_ts is not None else ""
    )
    print(f"[score] cutoffs: bot<={score_bot:.3f}  median={score_med:.3f}  "
          f"top>={score_top:.3f}{cutoff_origin}")

    INF, NINF = float("inf"), float("-inf")

    trades_top, diag_top = run_backtest(fvgs_for_trade, bars, risk_cfg, cost_cfg,
                                        (score_top, INF), label="Top")
    trades_bot, diag_bot = run_backtest(fvgs_for_trade, bars, risk_cfg, cost_cfg,
                                        (NINF, score_bot), label="Bot")
    trades_all, diag_all = run_backtest(fvgs_for_trade, bars, risk_cfg, cost_cfg,
                                        (NINF, INF), label="All")
    trades_rd,  diag_rd  = run_backtest(randoms_for_trade, bars, risk_cfg, cost_cfg,
                                        score_filter=None, label="Rand")

    stats_top  = compute_stats(trades_top,  risk_cfg, bars_per_year)
    stats_bot  = compute_stats(trades_bot,  risk_cfg, bars_per_year)
    stats_all  = compute_stats(trades_all,  risk_cfg, bars_per_year)
    stats_rand = compute_stats(trades_rd,   risk_cfg, bars_per_year)

    cost_grid: List[Tuple[float, dict]] = []
    for mult in (0.0, 1.0, 2.0):
        cc = CostsConfig(slippage_pips=args.slippage_pips, pip_size=pip,
                         cost_multiplier=mult)
        trs, _ = run_backtest(fvgs_for_trade, bars, risk_cfg, cc,
                              (score_top, INF), label=f"Top_cost_x{mult:.1f}")
        cost_grid.append((mult, compute_stats(trs, risk_cfg, bars_per_year)))

    be_grid: List[Tuple[float, dict]] = []
    for be in (0.0, 1.0, 1.5):
        rc = RiskConfig(
            sl_atr_mult=risk_cfg.sl_atr_mult, tp_atr_mult=risk_cfg.tp_atr_mult,
            time_stop_bars=risk_cfg.time_stop_bars,
            risk_per_trade_pct=risk_cfg.risk_per_trade_pct,
            starting_equity=risk_cfg.starting_equity,
            breakeven_after_atr=be,
        )
        trs, _ = run_backtest(fvgs_for_trade, bars, rc, cost_cfg,
                              (score_top, INF), label=f"Top_BE{be}")
        be_grid.append((be, compute_stats(trs, rc, bars_per_year)))

    sl_tp_grid: List[Tuple[float, float, dict]] = []
    for sl, tp in [(0.5, 1.5), (1.0, 2.0), (1.5, 3.0), (2.0, 4.0), (2.5, 5.0)]:
        rc = RiskConfig(
            sl_atr_mult=sl, tp_atr_mult=tp,
            time_stop_bars=risk_cfg.time_stop_bars,
            risk_per_trade_pct=risk_cfg.risk_per_trade_pct,
            starting_equity=risk_cfg.starting_equity,
            breakeven_after_atr=risk_cfg.breakeven_after_atr,
        )
        trs, _ = run_backtest(fvgs_for_trade, bars, rc, cost_cfg,
                              (score_top, INF), label=f"Top_SL{sl}_TP{tp}")
        sl_tp_grid.append((sl, tp, compute_stats(trs, rc, bars_per_year)))

    trades_to_df(trades_top).to_csv(out_dir / "trades_top.csv", index=False)
    trades_to_df(trades_bot).to_csv(out_dir / "trades_bot.csv", index=False)
    trades_to_df(trades_all).to_csv(out_dir / "trades_all.csv", index=False)
    trades_to_df(trades_rd ).to_csv(out_dir / "trades_random.csv", index=False)

    pd.DataFrame([{"cost_multiplier": m, **st} for m, st in cost_grid]) \
        .to_csv(out_dir / "grid_costs.csv", index=False)
    pd.DataFrame([{"be_atr": be, **st} for be, st in be_grid]) \
        .to_csv(out_dir / "grid_breakeven.csv", index=False)
    pd.DataFrame([{"sl_atr": sl, "tp_atr": tp, **st}
                  for sl, tp, st in sl_tp_grid]) \
        .to_csv(out_dir / "grid_sl_tp.csv", index=False)

    plot_equity_curve(trades_top, trades_bot, trades_all, trades_rd,
                      risk_cfg.starting_equity, out_dir / "equity_curve.png")

    summary_text = write_summary(
        out_dir / "summary.txt",
        symbol, tf, args.start, args.end,
        risk_cfg, cost_cfg, args.top_pct,
        score_bot, score_top, score_med,
        n_total, n_entered,
        stats_top, diag_top,
        stats_bot, diag_bot,
        stats_all, diag_all,
        stats_rand, diag_rd,
        cost_grid, sl_tp_grid, be_grid,
        train_end_ts=train_end_ts,
        n_entered_train=n_entered_train,
        n_entered_test=n_entered_test,
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

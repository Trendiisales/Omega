#!/usr/bin/env python3
"""
phase0_ema_scalp_backtest.py
=============================
Standalone Phase 0 viability test for the EMA(7)/EMA(17) dual-line scalping
strategy on XAUUSD tick data.

This is a THROWAWAY directional sniff-test. It does NOT pretend to be a
production-grade backtest. Goal: decide whether the idea has any legs at
all on gold ticks before investing C++ engine work.

INPUT FORMAT (Dukascopy-style, comma-separated, header):
    timestamp,askPrice,bidPrice
    1709258400133,2044.562,2044.265
    ...
    timestamp is integer milliseconds since Unix epoch.

USAGE:
    python3 phase0_ema_scalp_backtest.py \
        --tick-csv ~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv \
        --out-dir  ~/phase0_results \
        --start    2025-09-01 \
        --end      2026-03-01

OUTPUTS (written to --out-dir):
    trades.csv         per-trade log
    equity.png         cumulative PnL chart
    summary.txt        headline statistics
    m5_bars_*.pkl      cached reconstructed M5 bars (re-used across runs)

DEPENDENCIES:
    pandas, numpy, matplotlib   (no Omega code, no live data calls)

DESIGN NOTES (read before complaining about realism):
    * Mid-price OHLC reconstruction. Spread is captured as the per-bar mean
      of (ask - bid) and charged twice per round-trip (entry + exit). This
      is closer to reality than a fixed "0.3 pip" assumption but still an
      approximation; real fills depend on which side the order rests against.
    * Next-bar-open execution. The video says "enter at close of signal
      candle." Realistic execution on a closed M5 bar means the next tick,
      which we approximate as the next bar's open. This adds one bar of
      delay vs. the video's literal rules. I think this is the honest move.
    * Stop = low of signal candle - 1 pip buffer (longs); high + 1 pip (shorts).
    * Target = 3R from entry (the video's headline RR claim).
    * Single position at a time. No pyramiding. No re-entry within the same
      M5 bar after a stop.
    * Session filter: London + NY only (08:00 - 21:00 UTC). Asia killed.
    * News blackout is intentionally NOT implemented in v1 — added complexity
      without clear directional impact for a sniff test. If the strategy
      survives the cost model, news filter is the obvious Phase 1 addition.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


# =============================================================================
# Configuration
# =============================================================================

@dataclass
class StrategyConfig:
    # EMA periods (M5 bars)
    ema_fast: int = 7
    ema_slow: int = 17

    # ATR period for slope proxy and candle size gates
    atr_period: int = 14

    # Slope / separation gate: |EMA_fast - EMA_slow| >= slope_atr_mult * ATR
    slope_atr_mult: float = 0.5

    # Number of bars EMA spread must be widening before signal accepted
    slope_rising_bars: int = 3

    # HTF (H1) trend gate
    htf_ema_period: int = 50

    # Session window (UTC hours, inclusive start, exclusive end)
    session_start_hour: int = 8
    session_end_hour: int = 21

    # Candle classification thresholds
    pin_wick_to_body: float = 2.0      # wick must be >= 2x body
    pin_body_position: float = 0.33    # body must be in upper/lower 33%
    pin_min_range_atr: float = 0.8     # range >= 0.8 * ATR

    momentum_body_to_range: float = 0.7  # body >= 70% of range
    momentum_min_range_atr: float = 1.5  # range >= 1.5 * ATR

    # Risk / reward
    rr_target: float = 3.0
    stop_buffer_pips: float = 1.0      # extra pips beyond signal candle low/high

    # Cost model
    spread_charge_mode: str = "bar_mean"  # "bar_mean" or "bar_max"
    extra_slippage_pips: float = 0.5      # added to each side of round-trip


@dataclass
class Trade:
    entry_time: pd.Timestamp
    exit_time: pd.Timestamp
    direction: str          # "long" or "short"
    entry_price: float
    exit_price: float
    stop_price: float
    target_price: float
    candle_type: str        # "pin" / "momentum" / "engulfing"
    outcome: str            # "tp" / "sl" / "eod"
    gross_pnl_pips: float   # before costs
    cost_pips: float        # spread + slippage charged
    net_pnl_pips: float
    bars_held: int


# Gold tick size: 1 pip = 0.10 (i.e. 2044.562 -> 2044.662 is 1 pip)
PIP_SIZE = 0.10


# =============================================================================
# Stage 1: Tick -> M5 OHLC reconstruction
# =============================================================================

def load_ticks_chunked(
    tick_csv: Path,
    start: Optional[pd.Timestamp],
    end: Optional[pd.Timestamp],
    chunksize: int = 5_000_000,
) -> pd.DataFrame:
    """Stream the tick CSV, filter by date range, return concatenated frame.

    Memory note: 154M ticks at 3 float64 cols ~ 3.7 GB. We filter aggressively
    by date during streaming so a 6-month slice stays well under 1 GB.
    """
    print(f"[load] streaming {tick_csv} chunksize={chunksize:,}")
    if start is not None:
        start_ms = int(start.timestamp() * 1000)
    else:
        start_ms = None
    if end is not None:
        end_ms = int(end.timestamp() * 1000)
    else:
        end_ms = None

    parts: List[pd.DataFrame] = []
    rows_seen = 0
    rows_kept = 0
    t0 = time.time()

    reader = pd.read_csv(
        tick_csv,
        chunksize=chunksize,
        dtype={"timestamp": np.int64, "askPrice": np.float64, "bidPrice": np.float64},
    )
    for i, chunk in enumerate(reader):
        rows_seen += len(chunk)
        if start_ms is not None:
            chunk = chunk[chunk["timestamp"] >= start_ms]
        if end_ms is not None:
            chunk = chunk[chunk["timestamp"] < end_ms]
        if len(chunk) == 0:
            # Skip-ahead optimization: if we've passed the end window, stop
            if end_ms is not None and rows_seen > 0:
                # Cheap heuristic: peek at last row of chunk before filter
                pass
            continue
        rows_kept += len(chunk)
        parts.append(chunk)
        if i % 5 == 0:
            elapsed = time.time() - t0
            print(
                f"[load]   chunk {i}: seen={rows_seen:,} kept={rows_kept:,} "
                f"elapsed={elapsed:.1f}s"
            )

    if not parts:
        raise RuntimeError("no ticks fell within the requested date range")

    df = pd.concat(parts, ignore_index=True)
    elapsed = time.time() - t0
    print(f"[load] done: {len(df):,} ticks in {elapsed:.1f}s")
    return df


def ticks_to_m5_bars(ticks: pd.DataFrame) -> pd.DataFrame:
    """Build M5 OHLC bars from ticks using mid-price.

    Returns DataFrame indexed by bar-open timestamp (UTC) with columns:
        open, high, low, close, spread_mean, spread_max, tick_count
    """
    print(f"[bars] reconstructing M5 OHLC from {len(ticks):,} ticks")
    t0 = time.time()

    ticks = ticks.copy()
    ticks["mid"] = (ticks["askPrice"] + ticks["bidPrice"]) / 2.0
    ticks["spread"] = ticks["askPrice"] - ticks["bidPrice"]
    ticks["dt"] = pd.to_datetime(ticks["timestamp"], unit="ms", utc=True)
    ticks = ticks.set_index("dt")

    grp = ticks.resample("5min", label="left", closed="left")
    bars = pd.DataFrame({
        "open":         grp["mid"].first(),
        "high":         grp["mid"].max(),
        "low":          grp["mid"].min(),
        "close":        grp["mid"].last(),
        "spread_mean":  grp["spread"].mean(),
        "spread_max":   grp["spread"].max(),
        "tick_count":   grp["mid"].count(),
    })
    # Drop empty bars (gaps, weekends).
    bars = bars.dropna(subset=["open"])
    bars = bars[bars["tick_count"] > 0]
    elapsed = time.time() - t0
    print(f"[bars] {len(bars):,} M5 bars in {elapsed:.1f}s")
    return bars


def build_h1_bars(m5_bars: pd.DataFrame) -> pd.DataFrame:
    """Aggregate M5 bars up to H1 for the higher-timeframe trend gate."""
    print("[bars] aggregating M5 -> H1 for HTF gate")
    grp = m5_bars.resample("1h", label="left", closed="left")
    h1 = pd.DataFrame({
        "open":  grp["open"].first(),
        "high":  grp["high"].max(),
        "low":   grp["low"].min(),
        "close": grp["close"].last(),
    }).dropna(subset=["open"])
    return h1


# =============================================================================
# Stage 2: Indicators
# =============================================================================

def ema(series: pd.Series, period: int) -> pd.Series:
    """Standard EMA. adjust=False matches TradingView/most platforms."""
    return series.ewm(span=period, adjust=False).mean()


def atr(bars: pd.DataFrame, period: int) -> pd.Series:
    """Wilder's True Range -> ATR. Uses RMA (alpha = 1/period)."""
    high = bars["high"]
    low = bars["low"]
    close = bars["close"]
    prev_close = close.shift(1)
    tr = pd.concat([
        (high - low).abs(),
        (high - prev_close).abs(),
        (low - prev_close).abs(),
    ], axis=1).max(axis=1)
    return tr.ewm(alpha=1.0 / period, adjust=False).mean()


def add_indicators(m5: pd.DataFrame, h1: pd.DataFrame, cfg: StrategyConfig) -> pd.DataFrame:
    """Add all indicator columns to the M5 frame."""
    print("[ind] computing EMAs, ATR, slope, HTF alignment")
    m5 = m5.copy()
    m5["ema_fast"] = ema(m5["close"], cfg.ema_fast)
    m5["ema_slow"] = ema(m5["close"], cfg.ema_slow)
    m5["ema_diff"] = m5["ema_fast"] - m5["ema_slow"]
    m5["atr"] = atr(m5, cfg.atr_period)

    # Slope rising/falling: ema_diff strictly monotonic over N bars
    n = cfg.slope_rising_bars
    diffs_up = m5["ema_diff"].diff().rolling(n).apply(
        lambda x: float((x > 0).all()), raw=False
    )
    diffs_dn = m5["ema_diff"].diff().rolling(n).apply(
        lambda x: float((x < 0).all()), raw=False
    )
    m5["slope_rising"] = (diffs_up == 1.0).astype(int)
    m5["slope_falling"] = (diffs_dn == 1.0).astype(int)

    # HTF EMA(50) on H1 close
    h1 = h1.copy()
    h1["htf_ema"] = ema(h1["close"], cfg.htf_ema_period)
    h1["htf_bull"] = (h1["close"] > h1["htf_ema"]).astype(int)
    h1["htf_bear"] = (h1["close"] < h1["htf_ema"]).astype(int)

    # Forward-fill the H1 gate onto M5 bars (a M5 bar inherits the most recent
    # COMPLETED H1 bar's gate value — shift(1) avoids lookahead).
    h1_gate = h1[["htf_bull", "htf_bear"]].shift(1)
    m5 = m5.join(h1_gate.reindex(m5.index, method="ffill"))
    m5["htf_bull"] = m5["htf_bull"].fillna(0).astype(int)
    m5["htf_bear"] = m5["htf_bear"].fillna(0).astype(int)
    return m5


# =============================================================================
# Stage 3: Candle classification
# =============================================================================

def classify_candles(m5: pd.DataFrame, cfg: StrategyConfig) -> pd.DataFrame:
    """Tag each bar with the strongest signal type it matches (or 'none').

    A single bar can in principle match multiple types; we pick the most
    selective: engulfing > momentum > pin (momentum is rarer and more
    informative than pin in practice).
    """
    print("[ind] classifying candle types (pin/momentum/engulfing)")
    m5 = m5.copy()
    o, h, l, c = m5["open"], m5["high"], m5["low"], m5["close"]
    body = (c - o).abs()
    rng = (h - l).replace(0, np.nan)
    upper_wick = h - np.maximum(o, c)
    lower_wick = np.minimum(o, c) - l
    body_top = np.maximum(o, c)
    body_bot = np.minimum(o, c)

    # --- Pin bar -----------------------------------------------------------
    bull_pin = (
        (lower_wick >= cfg.pin_wick_to_body * body)
        & ((body_bot - l) / rng >= (1.0 - cfg.pin_body_position))  # body in upper third
        & (rng >= cfg.pin_min_range_atr * m5["atr"])
        & (body > 0)
    )
    bear_pin = (
        (upper_wick >= cfg.pin_wick_to_body * body)
        & ((h - body_top) / rng >= (1.0 - cfg.pin_body_position))  # body in lower third
        & (rng >= cfg.pin_min_range_atr * m5["atr"])
        & (body > 0)
    )

    # --- Momentum candle ---------------------------------------------------
    bull_mom = (
        (c > o)
        & (body / rng >= cfg.momentum_body_to_range)
        & (rng >= cfg.momentum_min_range_atr * m5["atr"])
    )
    bear_mom = (
        (c < o)
        & (body / rng >= cfg.momentum_body_to_range)
        & (rng >= cfg.momentum_min_range_atr * m5["atr"])
    )

    # --- Engulfing ---------------------------------------------------------
    prev_o = o.shift(1)
    prev_c = c.shift(1)
    prev_body_top = np.maximum(prev_o, prev_c)
    prev_body_bot = np.minimum(prev_o, prev_c)
    bull_eng = (
        (c > o) & (prev_c < prev_o)
        & (c >= prev_body_top) & (o <= prev_body_bot)
    )
    bear_eng = (
        (c < o) & (prev_c > prev_o)
        & (o >= prev_body_top) & (c <= prev_body_bot)
    )

    # Resolve priority: engulfing > momentum > pin
    sig = pd.Series("none", index=m5.index, dtype=object)
    sig = sig.mask(bull_pin, "bull_pin")
    sig = sig.mask(bear_pin, "bear_pin")
    sig = sig.mask(bull_mom, "bull_momentum")
    sig = sig.mask(bear_mom, "bear_momentum")
    sig = sig.mask(bull_eng, "bull_engulfing")
    sig = sig.mask(bear_eng, "bear_engulfing")
    m5["candle_signal"] = sig
    return m5


# =============================================================================
# Stage 4: Signal generation
# =============================================================================

def in_session(ts: pd.Timestamp, cfg: StrategyConfig) -> bool:
    """UTC hour gate. Skips weekends too."""
    if ts.weekday() >= 5:
        return False
    return cfg.session_start_hour <= ts.hour < cfg.session_end_hour


def generate_signals(m5: pd.DataFrame, cfg: StrategyConfig) -> pd.DataFrame:
    """Tag each bar with its tradable signal direction (or 'none').

    A signal is tradable if ALL of these hold on the closed bar:
        * candle_signal is bull_* or bear_*
        * EMAs aligned with direction
        * |ema_diff| >= slope_atr_mult * ATR
        * slope rising/falling for slope_rising_bars
        * HTF gate matches direction
        * bar opens within the trading session
    """
    print("[sig] generating tradable signals")
    long_ok = (
        m5["candle_signal"].str.startswith("bull_", na=False)
        & (m5["ema_fast"] > m5["ema_slow"])
        & (m5["ema_diff"].abs() >= cfg.slope_atr_mult * m5["atr"])
        & (m5["slope_rising"] == 1)
        & (m5["htf_bull"] == 1)
    )
    short_ok = (
        m5["candle_signal"].str.startswith("bear_", na=False)
        & (m5["ema_fast"] < m5["ema_slow"])
        & (m5["ema_diff"].abs() >= cfg.slope_atr_mult * m5["atr"])
        & (m5["slope_falling"] == 1)
        & (m5["htf_bear"] == 1)
    )

    # Session filter
    in_sess = pd.Series(
        [in_session(ts, cfg) for ts in m5.index],
        index=m5.index,
    )

    sig = pd.Series("none", index=m5.index, dtype=object)
    sig = sig.mask(long_ok & in_sess, "long")
    sig = sig.mask(short_ok & in_sess, "short")
    m5["signal"] = sig
    return m5


# =============================================================================
# Stage 5: Trade simulation
# =============================================================================

def simulate(m5: pd.DataFrame, cfg: StrategyConfig) -> List[Trade]:
    """Single-position bar-by-bar simulation.

    Execution model:
        * signal fires on bar close at index i
        * entry at bar i+1 open (mid price)
        * stop and target placed; checked against bar i+1 onward
        * if both target and stop sit inside a single bar, conservative
          assumption: stop fills first (worst case for us)
        * forced exit at end of session day (21:00 UTC) if still open
    """
    print(f"[sim] simulating across {len(m5):,} M5 bars")
    trades: List[Trade] = []
    n = len(m5)
    bars_o = m5["open"].values
    bars_h = m5["high"].values
    bars_l = m5["low"].values
    bars_c = m5["close"].values
    spreads_mean = m5["spread_mean"].values
    spreads_max = m5["spread_max"].values
    signals = m5["signal"].values
    candle_types = m5["candle_signal"].values
    times = m5.index
    bar_dates = m5.index.normalize()

    i = 0
    last_progress = time.time()
    while i < n - 1:
        if signals[i] == "none":
            i += 1
            continue

        direction = signals[i]
        sig_idx = i
        # Stop placement uses signal candle's low/high
        if direction == "long":
            stop_price = bars_l[sig_idx] - cfg.stop_buffer_pips * PIP_SIZE
        else:
            stop_price = bars_h[sig_idx] + cfg.stop_buffer_pips * PIP_SIZE

        # Entry at next bar's open
        entry_idx = sig_idx + 1
        if entry_idx >= n:
            break
        entry_price = bars_o[entry_idx]
        risk = abs(entry_price - stop_price)
        if risk < PIP_SIZE * 2:  # degenerate stop, skip
            i = entry_idx
            continue
        if direction == "long":
            target_price = entry_price + cfg.rr_target * risk
        else:
            target_price = entry_price - cfg.rr_target * risk

        # Walk forward bar by bar from entry_idx, looking for stop or target hit
        outcome = "eod"
        exit_idx = entry_idx
        exit_price = bars_c[entry_idx]
        for j in range(entry_idx, n):
            hi = bars_h[j]
            lo = bars_l[j]
            stop_hit = (lo <= stop_price) if direction == "long" else (hi >= stop_price)
            target_hit = (hi >= target_price) if direction == "long" else (lo <= target_price)
            if stop_hit and target_hit:
                # Conservative: assume stop fills first
                outcome = "sl"
                exit_idx = j
                exit_price = stop_price
                break
            if stop_hit:
                outcome = "sl"
                exit_idx = j
                exit_price = stop_price
                break
            if target_hit:
                outcome = "tp"
                exit_idx = j
                exit_price = target_price
                break
            # End-of-session forced exit on transition to non-session bar of same day
            ts = times[j]
            if not in_session(ts, cfg):
                outcome = "eod"
                exit_idx = j
                exit_price = bars_c[j]
                break

        # PnL calc in pips (gold: 1 pip = 0.10)
        if direction == "long":
            gross_pips = (exit_price - entry_price) / PIP_SIZE
        else:
            gross_pips = (entry_price - exit_price) / PIP_SIZE

        # Cost: spread on entry + spread on exit + slippage on each side
        if cfg.spread_charge_mode == "bar_max":
            entry_spread = spreads_max[entry_idx] / PIP_SIZE
            exit_spread = spreads_max[exit_idx] / PIP_SIZE
        else:
            entry_spread = spreads_mean[entry_idx] / PIP_SIZE
            exit_spread = spreads_mean[exit_idx] / PIP_SIZE
        cost_pips = entry_spread + exit_spread + 2 * cfg.extra_slippage_pips
        net_pips = gross_pips - cost_pips

        trades.append(Trade(
            entry_time=times[entry_idx],
            exit_time=times[exit_idx],
            direction=direction,
            entry_price=entry_price,
            exit_price=exit_price,
            stop_price=stop_price,
            target_price=target_price,
            candle_type=candle_types[sig_idx],
            outcome=outcome,
            gross_pnl_pips=gross_pips,
            cost_pips=cost_pips,
            net_pnl_pips=net_pips,
            bars_held=exit_idx - entry_idx,
        ))

        # Cooldown: skip past exit bar so next signal must be in fresh territory
        i = exit_idx + 1

        if time.time() - last_progress > 5.0:
            print(f"[sim]   bar {i:,}/{n:,}  trades={len(trades)}")
            last_progress = time.time()

    print(f"[sim] done: {len(trades)} trades")
    return trades


# =============================================================================
# Stage 6: Reporting
# =============================================================================

def trades_to_df(trades: List[Trade]) -> pd.DataFrame:
    if not trades:
        return pd.DataFrame()
    return pd.DataFrame([t.__dict__ for t in trades])


def write_summary(
    trades: List[Trade],
    out_path: Path,
    cfg: StrategyConfig,
    start: pd.Timestamp,
    end: pd.Timestamp,
) -> str:
    if not trades:
        text = "No trades generated.\n"
        out_path.write_text(text)
        return text

    df = trades_to_df(trades)
    n = len(df)
    wins = (df["net_pnl_pips"] > 0).sum()
    losses = (df["net_pnl_pips"] <= 0).sum()
    win_rate = wins / n if n > 0 else 0.0

    gross_win = df.loc[df["net_pnl_pips"] > 0, "net_pnl_pips"].sum()
    gross_loss = -df.loc[df["net_pnl_pips"] <= 0, "net_pnl_pips"].sum()
    pf = (gross_win / gross_loss) if gross_loss > 0 else float("inf")

    avg_win = df.loc[df["net_pnl_pips"] > 0, "net_pnl_pips"].mean() if wins else 0.0
    avg_loss = df.loc[df["net_pnl_pips"] <= 0, "net_pnl_pips"].mean() if losses else 0.0
    expectancy = df["net_pnl_pips"].mean()

    cum = df["net_pnl_pips"].cumsum()
    peak = cum.cummax()
    dd = peak - cum
    max_dd = dd.max()
    total_pips = df["net_pnl_pips"].sum()

    by_dir = df.groupby("direction")["net_pnl_pips"].agg(["count", "sum", "mean"])
    by_outcome = df.groupby("outcome")["net_pnl_pips"].agg(["count", "sum", "mean"])
    by_candle = df.groupby("candle_type")["net_pnl_pips"].agg(["count", "sum", "mean"])

    lines = [
        "=" * 70,
        "Phase 0 EMA(7)/EMA(17) Scalp - XAUUSD - Backtest Summary",
        "=" * 70,
        f"Window:                {start.date()} -> {end.date()}",
        f"Strategy config:       fast={cfg.ema_fast} slow={cfg.ema_slow} "
            f"slope_atr_mult={cfg.slope_atr_mult} rr={cfg.rr_target}",
        f"Cost model:            spread={cfg.spread_charge_mode} "
            f"extra_slip={cfg.extra_slippage_pips} pips/side",
        "",
        "OVERALL",
        f"  Total trades:        {n}",
        f"  Wins / losses:       {wins} / {losses}",
        f"  Win rate:            {win_rate:.1%}",
        f"  Profit factor:       {pf:.2f}",
        f"  Total net pips:      {total_pips:.1f}",
        f"  Expectancy/trade:    {expectancy:.2f} pips",
        f"  Avg win / avg loss:  {avg_win:.1f} / {avg_loss:.1f} pips",
        f"  Max drawdown:        {max_dd:.1f} pips",
        "",
        "BY DIRECTION",
        by_dir.to_string(),
        "",
        "BY OUTCOME",
        by_outcome.to_string(),
        "",
        "BY CANDLE TYPE",
        by_candle.to_string(),
        "",
        "ACCEPTANCE GATES (Phase 0 -> Phase 1)",
        f"  >= 200 trades       : {'PASS' if n >= 200 else 'FAIL'}  ({n})",
        f"  PF >= 1.20          : {'PASS' if pf >= 1.20 else 'FAIL'}  ({pf:.2f})",
        f"  Max DD < 30% of profit: "
            f"{'PASS' if (total_pips > 0 and max_dd / max(total_pips, 1) < 0.30) else 'FAIL'}",
        "=" * 70,
    ]
    text = "\n".join(lines) + "\n"
    out_path.write_text(text)
    return text


def write_equity_chart(trades: List[Trade], out_path: Path) -> None:
    if not trades:
        return
    df = trades_to_df(trades)
    df = df.sort_values("exit_time").reset_index(drop=True)
    df["cum_pips"] = df["net_pnl_pips"].cumsum()

    fig, ax = plt.subplots(2, 1, figsize=(12, 7), sharex=True)
    ax[0].plot(df["exit_time"], df["cum_pips"], linewidth=1.0)
    ax[0].set_ylabel("Cumulative net PnL (pips)")
    ax[0].set_title("Phase 0 EMA(7)/EMA(17) Scalp - Equity Curve")
    ax[0].grid(alpha=0.3)

    peak = df["cum_pips"].cummax()
    dd = peak - df["cum_pips"]
    ax[1].fill_between(df["exit_time"], 0, -dd, alpha=0.4, color="red")
    ax[1].set_ylabel("Drawdown (pips)")
    ax[1].set_xlabel("Exit time")
    ax[1].grid(alpha=0.3)

    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)


# =============================================================================
# Main
# =============================================================================

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tick-csv", required=True, type=Path,
                    help="Path to XAUUSD tick CSV (Dukascopy-style header)")
    ap.add_argument("--out-dir", required=True, type=Path,
                    help="Directory for outputs (created if missing)")
    ap.add_argument("--start", required=True, type=str,
                    help="Start date YYYY-MM-DD (UTC)")
    ap.add_argument("--end", required=True, type=str,
                    help="End date YYYY-MM-DD (UTC, exclusive)")
    ap.add_argument("--use-bar-cache", action="store_true",
                    help="Reuse cached M5 bars from previous run if present")
    args = ap.parse_args()

    cfg = StrategyConfig()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    start = pd.Timestamp(args.start, tz="UTC")
    end = pd.Timestamp(args.end, tz="UTC")

    bar_cache = args.out_dir / f"m5_bars_{args.start}_{args.end}.pkl"
    h1_cache = args.out_dir / f"h1_bars_{args.start}_{args.end}.pkl"

    if args.use_bar_cache and bar_cache.exists() and h1_cache.exists():
        print(f"[cache] loading cached bars from {bar_cache}")
        m5 = pd.read_pickle(bar_cache)
        h1 = pd.read_pickle(h1_cache)
    else:
        ticks = load_ticks_chunked(args.tick_csv, start, end)
        m5 = ticks_to_m5_bars(ticks)
        h1 = build_h1_bars(m5)
        # Cache for fast iteration on subsequent runs (pickle: no extra deps)
        m5.to_pickle(bar_cache)
        h1.to_pickle(h1_cache)
        print(f"[cache] wrote {bar_cache.name} and {h1_cache.name}")
        del ticks  # free memory

    m5 = add_indicators(m5, h1, cfg)
    m5 = classify_candles(m5, cfg)
    m5 = generate_signals(m5, cfg)

    trades = simulate(m5, cfg)
    trades_df = trades_to_df(trades)
    if not trades_df.empty:
        trades_df.to_csv(args.out_dir / "trades.csv", index=False)

    summary = write_summary(trades, args.out_dir / "summary.txt", cfg, start, end)
    write_equity_chart(trades, args.out_dir / "equity.png")
    print()
    print(summary)
    print(f"Outputs written to: {args.out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

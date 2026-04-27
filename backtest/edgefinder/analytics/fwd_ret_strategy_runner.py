"""
backtest/edgefinder/analytics/fwd_ret_strategy_runner.py
========================================================

C6 thread #1C Step 2 — paper-trade simulator that strips the bracket
framework and replaces it with explicit external stop-losses + a hard
60-minute time-stop.

Context
-------
The bracket framework (TP +200 / SL -100 at horizon 60min for bracket 5)
showed in C6 #1A that it almost never fires — 0 TP / 66 SL / 77,934 MtM
out of 78,000 trades across the strict OOS survivors. The realised pnl
reported by paper_trade_topk is therefore essentially the directional
drift over the bracket horizon, with no real risk control.

This module re-walks the 7 STRONG OOS survivors (output of C6 #1B v2 with
the C6 #1C verdict-priority fix) under two parallel SL frameworks:

    fixed_sl   : symmetric stop at -30 pts in the trade direction
    atr_sl     : symmetric stop at -1.5 * ATR(15-min, N=14) in the
                 trade direction (volatility-adaptive)

Both frameworks share a 60-minute time-stop. Exit reason is whichever
fires first. Realised pnl_pts is computed from minute-bar OHLC with
first-touch granularity.

Pipeline
--------
1. Load tick CSV with polars, build mid-price (ask+bid)/2.
2. Resample to 1-minute and 15-minute OHLC bars. Cache to parquet.
3. Compute Wilder ATR(15min, N=14). Cache to parquet.
4. Load paper_trade_topk_trades_classified.csv. Filter to partition=='oos'
   and inner-join with the v2 STRONG list (drift_stability_v2_per_edge.csv,
   verdict_v2 == 'STRONG').
5. For each surviving trade, look up entry mid-price (1m bar close at
   ts_close), look up ATR-as-of (last 15m bar with bar_close <= ts_close),
   walk 60 1-minute bars forward checking SL touch.
6. Tag each trade with subperiod (pre / post) using 2026-04-02 UTC midnight.
7. Compute per-edge metrics (split by sl_mode x subperiod), portfolio
   metrics (deduped on ts_close+side+pid), equity curves, and a markdown
   report.

Read-only
---------
This module does NOT modify any production code, the panel binary, the
catalogue, the OOS sentinel, the v1 / v2 outputs, or the paper_trade_topk
outputs. It writes only:

    fwd_ret_journal.csv
    fwd_ret_per_edge.csv
    fwd_ret_portfolio.csv
    fwd_ret_equity_curves.csv
    fwd_ret_strategy_runner.md
    cache/bars_1m.parquet            (auto-built from tick CSV)
    cache/bars_15m.parquet           (auto-built from tick CSV)
    cache/atr_15m_14.parquet         (auto-built from 15m bars)
    cache/cache_meta.json            (sidecar; tracks tick CSV mtime)

CLI
---
    python -m backtest.edgefinder.analytics.fwd_ret_strategy_runner \\
        [--trades-csv PATH]
        [--strong-csv PATH]
        [--tick-csv PATH]
        [--cache-dir PATH]
        [--out-dir PATH]
        [--fixed-sl-pts FLOAT]   default 30.0
        [--atr-mult FLOAT]       default 1.5
        [--atr-bar-min INT]      default 15
        [--atr-period INT]       default 14
        [--horizon-min INT]      default 60
        [--tariff-cutoff STR]    default 2026-04-02 (YYYY-MM-DD UTC midnight)
        [--rebuild-cache]        force rebuild of all parquet caches

SL evaluation conventions
-------------------------
- entry_price is the close of the 1-minute bar at ts_close (which is the
  bar-close timestamp recorded by paper_trade_topk).
- SL price for LONG = entry_price - SL_pts; SHORT = entry_price + SL_pts.
- SL is evaluated on the 60 1-minute bars STRICTLY AFTER ts_close
  (entry-bar excluded; you can't have stopped out before entering).
- LONG SL hit if any subsequent bar.low <= SL_price; exit at SL_price
  on that bar (pessimistic-fill convention).
- SHORT SL hit if any subsequent bar.high >= SL_price; exit at SL_price.
- If no SL hit within 60 bars, exit at the 60th bar's close.
- pnl_pts = (exit_price - entry_price) * side_sign  (LONG +1, SHORT -1).

Portfolio dedup convention
--------------------------
Per-edge metrics are computed faithfully against all 7 STRONG rows. For
the portfolio aggregate, trade rows that share (ts_close, side, pid)
are counted once — the bracket_id distinction collapses once the bracket
framework is stripped (e.g. pid 70823 b3 vs b4 produce identical trades
under external SL). Different pids that happen to fire on the same bar
remain separate portfolio contributions (additive).
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import numpy as np
import pandas as pd
import polars as pl


# -----------------------------------------------------------------------------
# Defaults
# -----------------------------------------------------------------------------
DEFAULT_TRADES_CSV = (
    'backtest/edgefinder/output/paper_trade/paper_trade_topk_trades_classified.csv'
)
DEFAULT_STRONG_CSV = (
    'backtest/edgefinder/output/paper_trade/drift_stability_v2_per_edge.csv'
)
DEFAULT_TICK_CSV = '~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv'
DEFAULT_CACHE_DIR = 'backtest/edgefinder/output/paper_trade/cache'
DEFAULT_OUT_DIR = 'backtest/edgefinder/output/paper_trade'
DEFAULT_FIXED_SL_PTS = 30.0
DEFAULT_ATR_MULT = 1.5
DEFAULT_ATR_BAR_MIN = 15
DEFAULT_ATR_PERIOD = 14
DEFAULT_HORIZON_MIN = 60
DEFAULT_TARIFF_CUTOFF = '2026-04-02'

SL_MODES = ('fixed_sl', 'atr_sl')
SUBPERIODS = ('pre', 'post')

EXIT_REASONS = ('SL_HIT', 'TIME_STOP', 'NO_DATA')


def _ts() -> str:
    return time.strftime('%H:%M:%S')


def _log(msg: str) -> None:
    print(f"[{_ts()}] {msg}", flush=True)


# -----------------------------------------------------------------------------
# Cache management
# -----------------------------------------------------------------------------
def _cache_meta_path(cache_dir: Path) -> Path:
    return cache_dir / 'cache_meta.json'


def _read_cache_meta(cache_dir: Path) -> dict:
    p = _cache_meta_path(cache_dir)
    if not p.is_file():
        return {}
    try:
        return json.loads(p.read_text())
    except (json.JSONDecodeError, OSError):
        return {}


def _write_cache_meta(cache_dir: Path, meta: dict) -> None:
    cache_dir.mkdir(parents=True, exist_ok=True)
    _cache_meta_path(cache_dir).write_text(json.dumps(meta, indent=2, sort_keys=True))


def _tick_csv_signature(tick_csv: Path) -> dict:
    st = tick_csv.stat()
    return {
        'tick_csv_path': str(tick_csv),
        'tick_csv_size': int(st.st_size),
        'tick_csv_mtime_ns': int(st.st_mtime_ns),
    }


def _cache_is_valid(cache_dir: Path, tick_csv: Path, required_files: list[str]) -> bool:
    meta = _read_cache_meta(cache_dir)
    sig = _tick_csv_signature(tick_csv)
    for k, v in sig.items():
        if meta.get(k) != v:
            return False
    for f in required_files:
        if not (cache_dir / f).is_file():
            return False
    return True


# -----------------------------------------------------------------------------
# Tick -> bar pipeline (polars)
# -----------------------------------------------------------------------------
def build_minute_bars(
    tick_csv: Path,
    bar_min: int,
) -> pl.DataFrame:
    """
    Stream tick CSV with polars lazy API. Schema:
        timestamp (epoch milliseconds, UTC), askPrice, bidPrice
    Compute mid = (ask + bid) / 2.
    Group by N-minute floor of timestamp -> OHLC bars on mid.

    Returns a polars DataFrame with columns:
        ts_open    (datetime[ms, UTC])  start of bar
        ts_close   (datetime[ms, UTC])  end of bar = ts_open + bar_min minutes
        open, high, low, close          (Float64)
        n_ticks                         (UInt32)
    """
    _log(f"streaming tick CSV: {tick_csv}")
    lf = pl.scan_csv(
        tick_csv,
        schema_overrides={
            'timestamp': pl.Int64,
            'askPrice': pl.Float64,
            'bidPrice': pl.Float64,
        },
    )

    # Cast ms to datetime UTC and compute mid.
    lf = lf.with_columns(
        pl.from_epoch('timestamp', time_unit='ms').dt.replace_time_zone('UTC').alias('ts'),
        ((pl.col('askPrice') + pl.col('bidPrice')) / 2.0).alias('mid'),
    )

    # Floor to N-minute bar opens. group_by_dynamic produces OHLC.
    lf = (
        lf.sort('ts')
          .group_by_dynamic(
              index_column='ts',
              every=f'{bar_min}m',
              period=f'{bar_min}m',
              closed='left',
              label='left',
          )
          .agg(
              pl.col('mid').first().alias('open'),
              pl.col('mid').max().alias('high'),
              pl.col('mid').min().alias('low'),
              pl.col('mid').last().alias('close'),
              pl.len().cast(pl.UInt32).alias('n_ticks'),
          )
          .rename({'ts': 'ts_open'})
    )

    df = lf.collect(streaming=True)

    # ts_close = ts_open + bar_min minutes (the convention used by
    # paper_trade_topk for "bar's ts_close").
    df = df.with_columns(
        (pl.col('ts_open') + pl.duration(minutes=bar_min)).alias('ts_close'),
    )

    # Reorder: ts_open, ts_close, then OHLC, then n_ticks
    df = df.select(['ts_open', 'ts_close', 'open', 'high', 'low', 'close', 'n_ticks'])
    _log(f"  built {len(df):,} {bar_min}-minute bars "
         f"({df['ts_open'][0]} ... {df['ts_open'][-1]})")
    return df


def compute_wilder_atr(
    bars: pl.DataFrame,
    period: int,
) -> pl.DataFrame:
    """
    Compute Wilder's ATR on a polars OHLC bar frame.

    True range:
        TR_i = max(high_i - low_i,
                   |high_i - close_{i-1}|,
                   |low_i  - close_{i-1}|)

    Wilder smoothing (RMA):
        ATR_period = mean(TR[0:period])
        ATR_i = (ATR_{i-1} * (period - 1) + TR_i) / period   for i > period

    Returns a polars DataFrame with columns:
        ts_close, atr   (atr is null for the first `period` bars)
    """
    if period < 1:
        raise ValueError(f"period must be >= 1, got {period}")

    # Move to pandas for the iterative RMA — polars rolling_mean is the
    # SMA, not RMA. RMA on N=14 over a few hundred thousand bars in pandas
    # is trivial.
    pdf = bars.to_pandas()
    n = len(pdf)
    if n < period + 1:
        atr = np.full(n, np.nan)
    else:
        high = pdf['high'].to_numpy(dtype=np.float64)
        low = pdf['low'].to_numpy(dtype=np.float64)
        close = pdf['close'].to_numpy(dtype=np.float64)
        prev_close = np.empty(n, dtype=np.float64)
        prev_close[0] = np.nan
        prev_close[1:] = close[:-1]
        tr = np.maximum.reduce([
            high - low,
            np.abs(high - prev_close),
            np.abs(low - prev_close),
        ])
        atr = np.full(n, np.nan, dtype=np.float64)
        # Seed with simple mean of first `period` TRs (TR[0] is high-low only
        # because prev_close is nan; that's the standard Wilder seed).
        tr[0] = high[0] - low[0]
        atr[period - 1] = tr[:period].mean()
        for i in range(period, n):
            atr[i] = (atr[i - 1] * (period - 1) + tr[i]) / period

    return pl.DataFrame({
        'ts_close': bars['ts_close'],
        'atr': atr,
    })


def ensure_caches(
    tick_csv: Path,
    cache_dir: Path,
    atr_bar_min: int,
    atr_period: int,
    rebuild: bool,
) -> dict[str, Path]:
    """
    Build (or load from cache) the 1m bars, atr-bar bars, and ATR series.

    Returns a dict with keys 'bars_1m', 'bars_atr', 'atr', mapping to
    their parquet paths. Loading is the caller's responsibility.
    """
    cache_dir.mkdir(parents=True, exist_ok=True)
    bars_1m_path = cache_dir / 'bars_1m.parquet'
    bars_atr_path = cache_dir / f'bars_{atr_bar_min}m.parquet'
    atr_path = cache_dir / f'atr_{atr_bar_min}m_{atr_period}.parquet'

    needed_files = [bars_1m_path.name, bars_atr_path.name, atr_path.name]
    if not rebuild and _cache_is_valid(cache_dir, tick_csv, needed_files):
        _log(f"cache valid — reusing {cache_dir}")
        return {'bars_1m': bars_1m_path, 'bars_atr': bars_atr_path, 'atr': atr_path}

    _log(f"rebuilding caches (rebuild={rebuild})")
    t0 = time.time()
    bars_1m = build_minute_bars(tick_csv, bar_min=1)
    bars_1m.write_parquet(bars_1m_path)
    _log(f"wrote {bars_1m_path} ({len(bars_1m):,} rows, {time.time() - t0:.1f}s)")

    if atr_bar_min == 1:
        bars_atr = bars_1m
    else:
        t0 = time.time()
        bars_atr = build_minute_bars(tick_csv, bar_min=atr_bar_min)
        _log(f"  built {atr_bar_min}m bars ({time.time() - t0:.1f}s)")
    bars_atr.write_parquet(bars_atr_path)
    _log(f"wrote {bars_atr_path} ({len(bars_atr):,} rows)")

    t0 = time.time()
    atr = compute_wilder_atr(bars_atr, period=atr_period)
    atr.write_parquet(atr_path)
    _log(f"wrote {atr_path} ({len(atr):,} rows, "
         f"non-null ATR rows: {int(atr['atr'].is_not_null().sum())}, "
         f"{time.time() - t0:.1f}s)")

    meta = _tick_csv_signature(tick_csv)
    meta['atr_bar_min'] = atr_bar_min
    meta['atr_period'] = atr_period
    _write_cache_meta(cache_dir, meta)

    return {'bars_1m': bars_1m_path, 'bars_atr': bars_atr_path, 'atr': atr_path}


# -----------------------------------------------------------------------------
# Trade simulation
# -----------------------------------------------------------------------------
@dataclass
class SimResult:
    exit_reason: str       # SL_HIT / TIME_STOP / NO_DATA
    exit_idx: int          # index into bars_1m of the exit bar (-1 if NO_DATA)
    exit_price: float      # NaN if NO_DATA
    pnl_pts: float         # NaN if NO_DATA


def _simulate_one(
    bars_close: np.ndarray,
    bars_high: np.ndarray,
    bars_low: np.ndarray,
    entry_idx: int,
    entry_price: float,
    side_sign: int,
    sl_pts: float,
    horizon_bars: int,
    n_bars: int,
) -> SimResult:
    """
    Walk minute bars forward from entry_idx + 1 (entry-bar excluded) for
    up to horizon_bars steps. Return exit details.

    side_sign: +1 LONG, -1 SHORT
    sl_pts:    stop distance in points (positive)
    """
    if not np.isfinite(entry_price) or sl_pts <= 0 or not np.isfinite(sl_pts):
        return SimResult(exit_reason='NO_DATA', exit_idx=-1,
                         exit_price=float('nan'), pnl_pts=float('nan'))

    # SL price: LONG stops on price falling, SHORT on price rising.
    if side_sign > 0:
        sl_price = entry_price - sl_pts
    else:
        sl_price = entry_price + sl_pts

    last_idx = entry_idx + horizon_bars
    if last_idx >= n_bars:
        # Truncate to available bars.
        last_idx = n_bars - 1
    if last_idx <= entry_idx:
        return SimResult(exit_reason='NO_DATA', exit_idx=-1,
                         exit_price=float('nan'), pnl_pts=float('nan'))

    # Walk bars [entry_idx+1, last_idx] inclusive checking SL touch.
    for i in range(entry_idx + 1, last_idx + 1):
        if side_sign > 0:
            if bars_low[i] <= sl_price:
                exit_price = sl_price
                pnl = (exit_price - entry_price) * side_sign
                return SimResult(exit_reason='SL_HIT', exit_idx=i,
                                 exit_price=exit_price, pnl_pts=pnl)
        else:
            if bars_high[i] >= sl_price:
                exit_price = sl_price
                pnl = (exit_price - entry_price) * side_sign
                return SimResult(exit_reason='SL_HIT', exit_idx=i,
                                 exit_price=exit_price, pnl_pts=pnl)

    # No SL — time-stop at last_idx close.
    exit_price = float(bars_close[last_idx])
    pnl = (exit_price - entry_price) * side_sign
    return SimResult(exit_reason='TIME_STOP', exit_idx=last_idx,
                     exit_price=exit_price, pnl_pts=pnl)


def simulate_trades(
    trades: pd.DataFrame,
    bars_1m: pd.DataFrame,
    atr: pd.DataFrame,
    fixed_sl_pts: float,
    atr_mult: float,
    horizon_min: int,
) -> pd.DataFrame:
    """
    For each input trade, run two parallel simulations (fixed_sl, atr_sl).
    Returns a long-format frame: 2 rows per input trade.

    Required columns in trades:
        ts_close (UTC datetime), pid, side, bracket_id, rank, edge_label

    Required columns in bars_1m:
        ts_close (UTC datetime), open, high, low, close

    Required columns in atr:
        ts_close (UTC datetime), atr
    """
    # Build numpy arrays for fast indexing.
    bars_ts_close = bars_1m['ts_close'].to_numpy()  # datetime64[ns, UTC]
    bars_open = bars_1m['open'].to_numpy(dtype=np.float64)
    bars_high = bars_1m['high'].to_numpy(dtype=np.float64)
    bars_low = bars_1m['low'].to_numpy(dtype=np.float64)
    bars_close = bars_1m['close'].to_numpy(dtype=np.float64)
    n_bars = len(bars_ts_close)

    atr_ts_close = atr['ts_close'].to_numpy()
    atr_values = atr['atr'].to_numpy(dtype=np.float64)
    n_atr = len(atr_ts_close)

    # Sort both arrays once for searchsorted lookups.
    if not np.all(bars_ts_close[:-1] <= bars_ts_close[1:]):
        order = np.argsort(bars_ts_close)
        bars_ts_close = bars_ts_close[order]
        bars_open = bars_open[order]
        bars_high = bars_high[order]
        bars_low = bars_low[order]
        bars_close = bars_close[order]
    if not np.all(atr_ts_close[:-1] <= atr_ts_close[1:]):
        order = np.argsort(atr_ts_close)
        atr_ts_close = atr_ts_close[order]
        atr_values = atr_values[order]

    horizon_bars = horizon_min  # 1m bars
    rows = []
    n_trades = len(trades)
    log_every = max(1, n_trades // 10)

    for ti, t in enumerate(trades.itertuples(index=False)):
        if ti % log_every == 0:
            _log(f"  simulating trade {ti+1:,}/{n_trades:,}")
        ts_close = pd.Timestamp(t.ts_close)
        if ts_close.tz is None:
            ts_close = ts_close.tz_localize('UTC')
        else:
            ts_close = ts_close.tz_convert('UTC')
        ts_close_np = np.datetime64(ts_close.tz_convert('UTC').tz_localize(None), 'ns')

        # Locate entry bar: bars_ts_close == ts_close exactly.
        idx = int(np.searchsorted(bars_ts_close, ts_close_np, side='left'))
        if idx >= n_bars or bars_ts_close[idx] != ts_close_np:
            # No exact match — skip this trade as NO_DATA.
            for sl_mode, sl_pts in (
                ('fixed_sl', fixed_sl_pts),
                ('atr_sl', float('nan')),
            ):
                rows.append(_no_data_row(t, sl_mode, sl_pts))
            continue
        entry_idx = idx
        entry_price = float(bars_close[entry_idx])

        # ATR lookup: largest atr_ts_close <= ts_close_np.
        a_idx = int(np.searchsorted(atr_ts_close, ts_close_np, side='right')) - 1
        if a_idx < 0 or not np.isfinite(atr_values[a_idx]):
            atr_at_entry = float('nan')
        else:
            atr_at_entry = float(atr_values[a_idx])

        side_sign = +1 if str(t.side) == 'LONG' else -1

        # Fixed SL
        sim_fixed = _simulate_one(
            bars_close=bars_close,
            bars_high=bars_high,
            bars_low=bars_low,
            entry_idx=entry_idx,
            entry_price=entry_price,
            side_sign=side_sign,
            sl_pts=fixed_sl_pts,
            horizon_bars=horizon_bars,
            n_bars=n_bars,
        )
        rows.append(_journal_row(
            t, sl_mode='fixed_sl', sl_pts=fixed_sl_pts,
            entry_idx=entry_idx, entry_price=entry_price,
            atr_at_entry=atr_at_entry,
            sim=sim_fixed,
            bars_ts_close=bars_ts_close,
            side_sign=side_sign,
        ))

        # ATR SL
        atr_sl_pts = atr_mult * atr_at_entry if np.isfinite(atr_at_entry) else float('nan')
        if not np.isfinite(atr_sl_pts) or atr_sl_pts <= 0:
            sim_atr = SimResult(exit_reason='NO_DATA', exit_idx=-1,
                                exit_price=float('nan'), pnl_pts=float('nan'))
        else:
            sim_atr = _simulate_one(
                bars_close=bars_close,
                bars_high=bars_high,
                bars_low=bars_low,
                entry_idx=entry_idx,
                entry_price=entry_price,
                side_sign=side_sign,
                sl_pts=atr_sl_pts,
                horizon_bars=horizon_bars,
                n_bars=n_bars,
            )
        rows.append(_journal_row(
            t, sl_mode='atr_sl', sl_pts=atr_sl_pts,
            entry_idx=entry_idx, entry_price=entry_price,
            atr_at_entry=atr_at_entry,
            sim=sim_atr,
            bars_ts_close=bars_ts_close,
            side_sign=side_sign,
        ))

    out = pd.DataFrame(rows)
    return out


def _no_data_row(t, sl_mode: str, sl_pts: float) -> dict:
    return {
        'ts_close':       pd.Timestamp(t.ts_close),
        'rank':           int(t.rank),
        'pid':            int(t.pid),
        'side':           str(t.side),
        'bracket_id':     int(t.bracket_id),
        'edge_label':     str(t.edge_label),
        'sl_mode':        sl_mode,
        'sl_pts':         float(sl_pts),
        'entry_price':    float('nan'),
        'atr_at_entry':   float('nan'),
        'exit_idx':       -1,
        'ts_exit':        pd.NaT,
        'exit_price':     float('nan'),
        'exit_reason':    'NO_DATA',
        'holding_bars':   -1,
        'pnl_pts':        float('nan'),
        'subperiod':      None,    # filled in later
    }


def _journal_row(
    t,
    sl_mode: str,
    sl_pts: float,
    entry_idx: int,
    entry_price: float,
    atr_at_entry: float,
    sim: SimResult,
    bars_ts_close: np.ndarray,
    side_sign: int,
) -> dict:
    if sim.exit_reason == 'NO_DATA' or sim.exit_idx < 0:
        ts_exit = pd.NaT
        holding = -1
    else:
        ts_exit_np = bars_ts_close[sim.exit_idx]
        ts_exit = pd.Timestamp(ts_exit_np).tz_localize('UTC')
        holding = int(sim.exit_idx - entry_idx)
    return {
        'ts_close':       pd.Timestamp(t.ts_close),
        'rank':           int(t.rank),
        'pid':            int(t.pid),
        'side':           str(t.side),
        'bracket_id':     int(t.bracket_id),
        'edge_label':     str(t.edge_label),
        'sl_mode':        sl_mode,
        'sl_pts':         float(sl_pts),
        'entry_price':    float(entry_price),
        'atr_at_entry':   float(atr_at_entry),
        'exit_idx':       int(sim.exit_idx),
        'ts_exit':        ts_exit,
        'exit_price':     float(sim.exit_price),
        'exit_reason':    sim.exit_reason,
        'holding_bars':   holding,
        'pnl_pts':        float(sim.pnl_pts),
        'subperiod':      None,
    }


# -----------------------------------------------------------------------------
# Tagging & metrics
# -----------------------------------------------------------------------------
def tag_subperiod(journal: pd.DataFrame, tariff_cutoff_iso: str) -> pd.DataFrame:
    """Tag each journal row with subperiod ('pre' or 'post') based on ts_close."""
    cutoff = pd.Timestamp(tariff_cutoff_iso, tz='UTC')
    ts = pd.to_datetime(journal['ts_close'], utc=True, errors='coerce')
    journal = journal.copy()
    journal['subperiod'] = np.where(ts < cutoff, 'pre', 'post')
    return journal


def _metrics_block(pnl: pd.Series) -> dict:
    """Compute n / hit / mean / std / sharpe / pf / sum / max_dd on a pnl_pts series."""
    pnl = pnl.dropna()
    n = int(len(pnl))
    if n == 0:
        return {
            'n': 0, 'hit_rate': float('nan'),
            'mean_pnl_pts': float('nan'), 'std_pnl_pts': float('nan'),
            'sharpe': float('nan'), 'profit_factor': float('nan'),
            'sum_pnl_pts': 0.0, 'max_dd_pts': 0.0,
        }
    pnl_arr = pnl.to_numpy(dtype=np.float64)
    hit = float((pnl_arr > 0).mean())
    mean = float(pnl_arr.mean())
    std = float(pnl_arr.std(ddof=1)) if n >= 2 else 0.0
    sharpe = mean / std if std > 0 else float('nan')
    gains = pnl_arr[pnl_arr > 0].sum()
    losses = -pnl_arr[pnl_arr < 0].sum()
    if losses > 0:
        pf = float(gains / losses)
    elif gains > 0:
        pf = float('inf')
    else:
        pf = float('nan')
    cum = pnl_arr.cumsum()
    running_max = np.maximum.accumulate(cum)
    dd = cum - running_max  # <= 0
    max_dd = float(dd.min()) if len(dd) else 0.0
    return {
        'n': n, 'hit_rate': hit,
        'mean_pnl_pts': mean, 'std_pnl_pts': std,
        'sharpe': sharpe, 'profit_factor': pf,
        'sum_pnl_pts': float(pnl_arr.sum()), 'max_dd_pts': max_dd,
    }


def _exit_breakdown(reasons: pd.Series) -> dict:
    out = {}
    for r in EXIT_REASONS:
        out[f'n_{r}'] = int((reasons == r).sum())
    return out


def per_edge_metrics(journal: pd.DataFrame) -> pd.DataFrame:
    """
    Per-edge metrics, broken down by sl_mode and subperiod.
    Edge identity = (rank, pid, side, bracket_id, edge_label).
    """
    keys = ['rank', 'pid', 'side', 'bracket_id', 'edge_label', 'sl_mode']
    rows = []
    for sub in list(SUBPERIODS) + ['ALL']:
        sub_mask = journal['subperiod'] == sub if sub != 'ALL' else slice(None)
        sub_df = journal[sub_mask] if sub != 'ALL' else journal
        if sub_df.empty:
            continue
        for key, grp in sub_df.groupby(keys, sort=False):
            sorted_grp = grp.sort_values('ts_close')
            m = _metrics_block(sorted_grp['pnl_pts'])
            m.update(_exit_breakdown(sorted_grp['exit_reason']))
            row = dict(zip(keys, key))
            row['subperiod'] = sub
            row.update(m)
            rows.append(row)
    return pd.DataFrame(rows)


def portfolio_metrics(journal: pd.DataFrame) -> pd.DataFrame:
    """
    Portfolio dedup convention: one contribution per (ts_close, side, pid)
    per sl_mode. bracket_id collapses (b3==b4 for pid 70823 produce identical
    trades under external SL). Different pids stay separate (additive).
    """
    rows = []
    dedup_keys = ['ts_close', 'side', 'pid', 'sl_mode']
    for sub in list(SUBPERIODS) + ['ALL']:
        sub_df = journal if sub == 'ALL' else journal[journal['subperiod'] == sub]
        if sub_df.empty:
            continue
        deduped = (
            sub_df.sort_values(['ts_close', 'rank', 'bracket_id'])
                  .drop_duplicates(subset=dedup_keys, keep='first')
        )
        for sl_mode, grp in deduped.groupby('sl_mode', sort=False):
            sorted_grp = grp.sort_values('ts_close')
            m = _metrics_block(sorted_grp['pnl_pts'])
            m.update(_exit_breakdown(sorted_grp['exit_reason']))
            m['n_unique_after_dedup'] = int(len(sorted_grp))
            row = {'sl_mode': sl_mode, 'subperiod': sub}
            row.update(m)
            rows.append(row)
    return pd.DataFrame(rows)


def equity_curves(journal: pd.DataFrame) -> pd.DataFrame:
    """
    Long-format equity curves: edge_label x sl_mode x subperiod.
    Plus 'PORTFOLIO' edge_label using the dedup rule.
    """
    rows = []
    keys_per_edge = ['edge_label', 'sl_mode', 'subperiod']
    for key, grp in journal.groupby(keys_per_edge, sort=False):
        s = grp.sort_values('ts_close')
        cum = s['pnl_pts'].fillna(0.0).cumsum().to_numpy()
        for ts, c, p in zip(s['ts_close'], cum, s['pnl_pts'].fillna(0.0).to_numpy()):
            rows.append({
                'edge_label': key[0],
                'sl_mode':    key[1],
                'subperiod':  key[2],
                'ts_close':   ts,
                'pnl_pts':    float(p),
                'cum_pnl_pts': float(c),
            })

    # Portfolio variant — dedup on (ts_close, side, pid, sl_mode).
    dedup_keys = ['ts_close', 'side', 'pid', 'sl_mode']
    deduped = (
        journal.sort_values(['ts_close', 'rank', 'bracket_id'])
               .drop_duplicates(subset=dedup_keys, keep='first')
    )
    for (sl_mode, sub), grp in deduped.groupby(['sl_mode', 'subperiod'], sort=False):
        s = grp.sort_values('ts_close')
        cum = s['pnl_pts'].fillna(0.0).cumsum().to_numpy()
        for ts, c, p in zip(s['ts_close'], cum, s['pnl_pts'].fillna(0.0).to_numpy()):
            rows.append({
                'edge_label': 'PORTFOLIO',
                'sl_mode':    sl_mode,
                'subperiod':  sub,
                'ts_close':   ts,
                'pnl_pts':    float(p),
                'cum_pnl_pts': float(c),
            })

    return pd.DataFrame(rows)


# -----------------------------------------------------------------------------
# Markdown report
# -----------------------------------------------------------------------------
def _fmt_signed(x, w: int = 9, prec: int = 2) -> str:
    if x is None or (isinstance(x, float) and not np.isfinite(x)):
        return f"{'nan':>{w}}"
    return f"{float(x):>+{w}.{prec}f}"


def _fmt_int(x, w: int = 5) -> str:
    if x is None or (isinstance(x, float) and not np.isfinite(x)):
        return f"{'-':>{w}}"
    try:
        return f"{int(x):>{w}}"
    except (ValueError, TypeError):
        return f"{'-':>{w}}"


def _fmt_pct(x, w: int = 6) -> str:
    if x is None or (isinstance(x, float) and not np.isfinite(x)):
        return f"{'nan':>{w}}"
    return f"{100.0 * float(x):>{w-1}.1f}%"


def write_md_report(
    journal: pd.DataFrame,
    per_edge: pd.DataFrame,
    portfolio: pd.DataFrame,
    out_md_path: Path,
    args_summary: dict,
) -> None:
    lines = []
    lines.append("# C6 #1C Step 2 — Forward-Return Strategy Runner")
    lines.append("")
    lines.append("Paper-trade simulator for STRONG OOS survivors with the bracket "
                 "framework stripped and replaced by external SL + 60-min time-stop. "
                 "Two SL modes evaluated side-by-side: fixed -30pts and -1.5×ATR(15m).")
    lines.append("")
    lines.append("## Parameters")
    lines.append("")
    for k, v in args_summary.items():
        lines.append(f"- **{k}**: `{v}`")
    lines.append("")

    lines.append("## Trade counts by sl_mode × subperiod × exit_reason")
    lines.append("")
    lines.append("| sl_mode | subperiod | n | SL_HIT | TIME_STOP | NO_DATA |")
    lines.append("|:---|:---|---:|---:|---:|---:|")
    for sl_mode in SL_MODES:
        for sub in list(SUBPERIODS) + ['ALL']:
            sub_df = journal[journal['sl_mode'] == sl_mode]
            if sub != 'ALL':
                sub_df = sub_df[sub_df['subperiod'] == sub]
            if sub_df.empty:
                continue
            n = len(sub_df)
            counts = sub_df['exit_reason'].value_counts()
            lines.append(
                f"| {sl_mode} | {sub} | {n} "
                f"| {int(counts.get('SL_HIT', 0))} "
                f"| {int(counts.get('TIME_STOP', 0))} "
                f"| {int(counts.get('NO_DATA', 0))} |"
            )
    lines.append("")

    # Per-edge tables, one per sl_mode, with ALL/pre/post rows interleaved
    lines.append("## Per-edge metrics")
    lines.append("")
    lines.append("Faithful to the 7 STRONG rows (no auto-collapse). pid 70823 b3 "
                 "and b4 are reported separately even though they produce identical "
                 "trades under external SL — the dedup happens in the portfolio table.")
    lines.append("")
    for sl_mode in SL_MODES:
        sub = per_edge[per_edge['sl_mode'] == sl_mode]
        if sub.empty:
            continue
        lines.append(f"### sl_mode = {sl_mode}")
        lines.append("")
        lines.append("| edge_label | subperiod | n | hit | E[pnl] | sharpe | "
                     "pf | sum pts | max DD | SL | TS | ND |")
        lines.append("|:---|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        # Order: by edge_label then subperiod (ALL, pre, post)
        sub_order = {'ALL': 0, 'pre': 1, 'post': 2}
        sub_sorted = sub.copy()
        sub_sorted['_so'] = sub_sorted['subperiod'].map(sub_order).fillna(99)
        sub_sorted = sub_sorted.sort_values(['edge_label', '_so'])
        for _, r in sub_sorted.iterrows():
            pf = r['profit_factor']
            pf_s = "inf" if pf == float('inf') else _fmt_signed(pf, 6, 2)
            lines.append(
                f"| {r['edge_label']} | {r['subperiod']} "
                f"| {_fmt_int(r['n'], 4)} "
                f"| {_fmt_pct(r['hit_rate'])} "
                f"| {_fmt_signed(r['mean_pnl_pts'], 7, 3)} "
                f"| {_fmt_signed(r['sharpe'], 7, 3)} "
                f"| {pf_s} "
                f"| {_fmt_signed(r['sum_pnl_pts'], 9, 1)} "
                f"| {_fmt_signed(r['max_dd_pts'], 8, 1)} "
                f"| {_fmt_int(r.get('n_SL_HIT'), 4)} "
                f"| {_fmt_int(r.get('n_TIME_STOP'), 4)} "
                f"| {_fmt_int(r.get('n_NO_DATA'), 3)} |"
            )
        lines.append("")

    # Portfolio table
    lines.append("## Portfolio metrics (deduped on ts_close+side+pid)")
    lines.append("")
    lines.append("| sl_mode | subperiod | n_unique | hit | E[pnl] | sharpe | "
                 "pf | sum pts | max DD | SL | TS | ND |")
    lines.append("|:---|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for sl_mode in SL_MODES:
        for sub in list(SUBPERIODS) + ['ALL']:
            row = portfolio[(portfolio['sl_mode'] == sl_mode) &
                             (portfolio['subperiod'] == sub)]
            if row.empty:
                continue
            r = row.iloc[0]
            pf = r['profit_factor']
            pf_s = "inf" if pf == float('inf') else _fmt_signed(pf, 6, 2)
            lines.append(
                f"| {sl_mode} | {sub} "
                f"| {_fmt_int(r['n_unique_after_dedup'], 4)} "
                f"| {_fmt_pct(r['hit_rate'])} "
                f"| {_fmt_signed(r['mean_pnl_pts'], 7, 3)} "
                f"| {_fmt_signed(r['sharpe'], 7, 3)} "
                f"| {pf_s} "
                f"| {_fmt_signed(r['sum_pnl_pts'], 9, 1)} "
                f"| {_fmt_signed(r['max_dd_pts'], 8, 1)} "
                f"| {_fmt_int(r.get('n_SL_HIT'), 4)} "
                f"| {_fmt_int(r.get('n_TIME_STOP'), 4)} "
                f"| {_fmt_int(r.get('n_NO_DATA'), 3)} |"
            )
    lines.append("")

    out_md_path.write_text('\n'.join(lines))


# -----------------------------------------------------------------------------
# Strong-list filter
# -----------------------------------------------------------------------------
def filter_to_strong_oos(
    trades: pd.DataFrame,
    strong_csv: Path,
) -> pd.DataFrame:
    """
    Inner-join trades with the v2 STRONG list on (rank, pid, side, bracket_id),
    keep partition=='oos'.
    """
    strong = pd.read_csv(strong_csv)
    if 'verdict_v2' not in strong.columns:
        raise ValueError(
            f"strong CSV {strong_csv} missing verdict_v2 column — wrong file?"
        )
    strong = strong[strong['verdict_v2'] == 'STRONG'][
        ['rank', 'pid', 'side', 'bracket_id']
    ].drop_duplicates()
    _log(f"  STRONG edges: {len(strong)}")
    if strong.empty:
        return trades.iloc[0:0].copy()

    oos = trades[trades['partition'] == 'oos'].copy()
    _log(f"  OOS rows in trades CSV: {len(oos)}")

    merged = oos.merge(
        strong,
        on=['rank', 'pid', 'side', 'bracket_id'],
        how='inner',
    )
    _log(f"  OOS rows joined to STRONG: {len(merged)}")
    return merged


# -----------------------------------------------------------------------------
# CLI
# -----------------------------------------------------------------------------
def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=(
            "C6 #1C Step 2: paper-trade simulator with external SL and time-stop. "
            "Reads STRONG OOS trades from paper_trade_topk output, simulates each "
            "under fixed -30pts SL and -1.5*ATR(15m) SL, splits results by "
            "pre/post tariff, writes per-trade journal + per-edge + portfolio "
            "metrics + equity curves + markdown report. Read-only with respect "
            "to all production code and the OOS sentinel."
        )
    )
    p.add_argument('--trades-csv', default=DEFAULT_TRADES_CSV)
    p.add_argument('--strong-csv', default=DEFAULT_STRONG_CSV)
    p.add_argument('--tick-csv', default=DEFAULT_TICK_CSV)
    p.add_argument('--cache-dir', default=DEFAULT_CACHE_DIR)
    p.add_argument('--out-dir', default=DEFAULT_OUT_DIR)
    p.add_argument('--fixed-sl-pts', type=float, default=DEFAULT_FIXED_SL_PTS)
    p.add_argument('--atr-mult', type=float, default=DEFAULT_ATR_MULT)
    p.add_argument('--atr-bar-min', type=int, default=DEFAULT_ATR_BAR_MIN)
    p.add_argument('--atr-period', type=int, default=DEFAULT_ATR_PERIOD)
    p.add_argument('--horizon-min', type=int, default=DEFAULT_HORIZON_MIN)
    p.add_argument('--tariff-cutoff', default=DEFAULT_TARIFF_CUTOFF)
    p.add_argument('--rebuild-cache', action='store_true',
                   help="force unconditional rebuild of all parquet caches")
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)

    trades_csv = Path(args.trades_csv)
    strong_csv = Path(args.strong_csv)
    tick_csv = Path(args.tick_csv).expanduser()
    cache_dir = Path(args.cache_dir)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if not trades_csv.is_file():
        print(f"ERROR: trades CSV not found: {trades_csv}", file=sys.stderr)
        return 1
    if not strong_csv.is_file():
        print(f"ERROR: STRONG CSV not found: {strong_csv}", file=sys.stderr)
        return 1
    if not tick_csv.is_file():
        print(f"ERROR: tick CSV not found: {tick_csv}", file=sys.stderr)
        return 1

    _log(f"args: trades={trades_csv} strong={strong_csv} tick={tick_csv}")
    _log(f"      fixed_sl={args.fixed_sl_pts} atr_mult={args.atr_mult} "
         f"atr_bar_min={args.atr_bar_min} atr_period={args.atr_period} "
         f"horizon_min={args.horizon_min} tariff_cutoff={args.tariff_cutoff}")

    # 1. Caches (bars + ATR)
    cache_paths = ensure_caches(
        tick_csv=tick_csv,
        cache_dir=cache_dir,
        atr_bar_min=args.atr_bar_min,
        atr_period=args.atr_period,
        rebuild=args.rebuild_cache,
    )

    _log(f"loading 1m bars from {cache_paths['bars_1m']}")
    bars_1m = pl.read_parquet(cache_paths['bars_1m']).to_pandas()
    _log(f"  {len(bars_1m):,} 1m bars "
         f"({bars_1m['ts_close'].iloc[0]} ... {bars_1m['ts_close'].iloc[-1]})")

    _log(f"loading ATR from {cache_paths['atr']}")
    atr = pl.read_parquet(cache_paths['atr']).to_pandas()
    _log(f"  {len(atr):,} ATR rows, non-null: {int(atr['atr'].notna().sum()):,}")

    # 2. Load trades and filter to STRONG OOS
    _log(f"loading trades CSV: {trades_csv}")
    trades = pd.read_csv(trades_csv)
    required = {'ts_close', 'partition', 'rank', 'pid', 'side',
                'bracket_id', 'edge_label'}
    missing = required - set(trades.columns)
    if missing:
        print(f"ERROR: trades CSV missing columns: {sorted(missing)}", file=sys.stderr)
        return 2
    trades['ts_close'] = pd.to_datetime(trades['ts_close'], utc=True, errors='coerce')
    if trades['ts_close'].isna().any():
        n_bad = int(trades['ts_close'].isna().sum())
        print(f"ERROR: {n_bad} ts_close values failed to parse as UTC", file=sys.stderr)
        return 2

    target = filter_to_strong_oos(trades, strong_csv)
    if target.empty:
        print("ERROR: no STRONG OOS trades to simulate", file=sys.stderr)
        return 3

    # 3. Simulate
    _log(f"simulating {len(target)} STRONG OOS trades x 2 SL modes "
         f"= {2 * len(target)} simulator runs")
    journal = simulate_trades(
        trades=target,
        bars_1m=bars_1m,
        atr=atr,
        fixed_sl_pts=args.fixed_sl_pts,
        atr_mult=args.atr_mult,
        horizon_min=args.horizon_min,
    )
    journal = tag_subperiod(journal, args.tariff_cutoff)
    _log(f"journal: {len(journal)} rows")

    # 4. Metrics
    _log(f"computing per-edge metrics")
    per_edge = per_edge_metrics(journal)
    _log(f"  {len(per_edge)} per-edge x sl_mode x subperiod rows")

    _log(f"computing portfolio metrics")
    port = portfolio_metrics(journal)
    _log(f"  {len(port)} portfolio rows")

    _log(f"computing equity curves")
    eq = equity_curves(journal)
    _log(f"  {len(eq)} equity-curve rows")

    # 5. Write outputs
    journal_csv = out_dir / 'fwd_ret_journal.csv'
    journal.to_csv(journal_csv, index=False)
    _log(f"wrote {journal_csv}")

    per_edge_csv = out_dir / 'fwd_ret_per_edge.csv'
    per_edge.to_csv(per_edge_csv, index=False)
    _log(f"wrote {per_edge_csv}")

    port_csv = out_dir / 'fwd_ret_portfolio.csv'
    port.to_csv(port_csv, index=False)
    _log(f"wrote {port_csv}")

    eq_csv = out_dir / 'fwd_ret_equity_curves.csv'
    eq.to_csv(eq_csv, index=False)
    _log(f"wrote {eq_csv}")

    args_summary = {
        'trades_csv': str(trades_csv),
        'strong_csv': str(strong_csv),
        'tick_csv': str(tick_csv),
        'fixed_sl_pts': args.fixed_sl_pts,
        'atr_mult': args.atr_mult,
        'atr_bar_min': args.atr_bar_min,
        'atr_period': args.atr_period,
        'horizon_min': args.horizon_min,
        'tariff_cutoff': args.tariff_cutoff,
        'n_input_trades': int(len(target)),
        'n_journal_rows': int(len(journal)),
    }
    md_path = out_dir / 'fwd_ret_strategy_runner.md'
    write_md_report(
        journal=journal,
        per_edge=per_edge,
        portfolio=port,
        out_md_path=md_path,
        args_summary=args_summary,
    )
    _log(f"wrote {md_path}")

    # 6. Console summary
    _log("=" * 60)
    _log("portfolio summary (deduped, ALL subperiods, by sl_mode):")
    for sl_mode in SL_MODES:
        row = port[(port['sl_mode'] == sl_mode) & (port['subperiod'] == 'ALL')]
        if row.empty:
            continue
        r = row.iloc[0]
        _log(f"  {sl_mode:8s}  n={int(r['n_unique_after_dedup']):4d}  "
             f"mean={r['mean_pnl_pts']:+.3f}  sharpe={r['sharpe']:+.3f}  "
             f"sum={r['sum_pnl_pts']:+.1f}  maxDD={r['max_dd_pts']:+.1f}")

    return 0


if __name__ == '__main__':
    sys.exit(main())

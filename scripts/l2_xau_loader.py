"""Load L2 XAUUSD tick CSVs, build OHLCV bars with bid/ask + feature aggregation."""
from __future__ import annotations
import glob
import os
import numpy as np
import pandas as pd

TICK_DIR = "/Users/jo/Downloads"
TICK_GLOB = "l2_ticks_XAUUSD_2026-05-*.csv"
CACHE_DIR = "/Users/jo/omega_repo/outputs/cache"

DTYPES = {
    "ts_ms": "int64",
    "mid": "float64",
    "bid": "float64",
    "ask": "float64",
    "l2_imb": "float32",
    "l2_bid_vol": "float32",
    "l2_ask_vol": "float32",
    "depth_bid_levels": "int16",
    "depth_ask_levels": "int16",
    "depth_events_total": "int32",
    "watchdog_dead": "int8",
    "vol_ratio": "float32",
    "regime": "int8",
    "vpin": "float32",
    "has_pos": "int8",
    "micro_edge": "float32",
    "ewm_drift": "float32",
}


def load_ticks(force: bool = False) -> pd.DataFrame:
    cache = os.path.join(CACHE_DIR, "ticks.parquet")
    if os.path.exists(cache) and not force:
        return pd.read_parquet(cache)
    files = sorted(glob.glob(os.path.join(TICK_DIR, TICK_GLOB)))
    if not files:
        raise FileNotFoundError(f"No ticks in {TICK_DIR}/{TICK_GLOB}")
    parts = []
    for f in files:
        df = pd.read_csv(f, dtype=DTYPES)
        parts.append(df)
    df = pd.concat(parts, ignore_index=True)
    df = df.sort_values("ts_ms", kind="mergesort").reset_index(drop=True)
    df["ts"] = pd.to_datetime(df["ts_ms"], unit="ms", utc=True)
    # drop watchdog_dead ticks (feed stale)
    df = df[df["watchdog_dead"] == 0].reset_index(drop=True)
    # drop crossed / zero spreads
    df = df[(df["ask"] > df["bid"]) & (df["bid"] > 0)].reset_index(drop=True)
    os.makedirs(CACHE_DIR, exist_ok=True)
    df.to_parquet(cache, compression="zstd")
    return df


def build_bars(ticks: pd.DataFrame, seconds: int) -> pd.DataFrame:
    """OHLCV bars on mid, plus mean bid/ask + last-feature snapshot."""
    t = ticks.set_index("ts")
    rule = f"{seconds}s"
    o = t["mid"].resample(rule).agg(["first", "max", "min", "last"]).rename(
        columns={"first": "open", "max": "high", "min": "low", "last": "close"}
    )
    o["bid"] = t["bid"].resample(rule).last()
    o["ask"] = t["ask"].resample(rule).last()
    o["spread"] = (t["ask"] - t["bid"]).resample(rule).mean()
    o["tick_count"] = t["mid"].resample(rule).count()
    # feature aggregation
    o["l2_imb"] = t["l2_imb"].resample(rule).mean()
    o["vpin"] = t["vpin"].resample(rule).mean()
    o["regime"] = t["regime"].resample(rule).last()
    o["vol_ratio"] = t["vol_ratio"].resample(rule).mean()
    o["ewm_drift"] = t["ewm_drift"].resample(rule).last()
    o["micro_edge"] = t["micro_edge"].resample(rule).mean()
    o["depth_events"] = t["depth_events_total"].resample(rule).sum()
    bars = o.dropna(subset=["close"]).copy()
    bars["ret"] = bars["close"].pct_change().fillna(0.0)
    bars["lret"] = np.log(bars["close"]).diff().fillna(0.0)
    bars["atr"] = (bars["high"] - bars["low"]).rolling(14, min_periods=1).mean()
    bars["rv"] = bars["lret"].rolling(20, min_periods=5).std() * np.sqrt(20)
    bars["hour"] = bars.index.hour
    # session tags: Asia 0-6 UTC, London 7-12, NY overlap 13-16, NY late 17-20
    h = bars["hour"]
    bars["session"] = np.where(
        h < 7, "asia",
        np.where(h < 13, "london",
                 np.where(h < 17, "ny_overlap", "ny_late"))
    )
    return bars


def get_bars(seconds: int, force: bool = False) -> pd.DataFrame:
    cache = os.path.join(CACHE_DIR, f"bars_{seconds}s.parquet")
    if os.path.exists(cache) and not force:
        return pd.read_parquet(cache)
    ticks = load_ticks(force=force)
    bars = build_bars(ticks, seconds)
    bars.to_parquet(cache, compression="zstd")
    return bars


if __name__ == "__main__":
    import sys
    force = "--force" in sys.argv
    ticks = load_ticks(force=force)
    print(f"ticks: {len(ticks):,} rows, {ticks['ts'].min()} -> {ticks['ts'].max()}")
    for sec in (300, 900, 3600):
        b = get_bars(sec, force=force)
        print(f"bars {sec}s: {len(b):,}, spread mean {b['spread'].mean():.4f}")

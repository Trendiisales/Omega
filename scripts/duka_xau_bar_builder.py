"""Chunked loader for 2y XAUUSD Duka tick file -> bars cache.
Format: timestamp,askPrice,bidPrice  (timestamp ms)."""
from __future__ import annotations
import os
import numpy as np
import pandas as pd

TICK_PATH = "/Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv"
CACHE_DIR = "/Users/jo/omega_repo/outputs/cache"


def build_bars_long(seconds: int, force: bool = False) -> pd.DataFrame:
    cache = os.path.join(CACHE_DIR, f"bars_long_{seconds}s.parquet")
    if os.path.exists(cache) and not force:
        return pd.read_parquet(cache)

    os.makedirs(CACHE_DIR, exist_ok=True)
    rule = f"{seconds}s"
    aggs = []
    CHUNK = 5_000_000
    print(f"streaming {TICK_PATH} in chunks of {CHUNK:,}...")
    for i, chunk in enumerate(pd.read_csv(
        TICK_PATH, chunksize=CHUNK,
        dtype={"timestamp": "int64", "askPrice": "float64", "bidPrice": "float64"},
    )):
        chunk = chunk[(chunk["askPrice"] > chunk["bidPrice"]) & (chunk["bidPrice"] > 0)]
        chunk["mid"] = (chunk["askPrice"] + chunk["bidPrice"]) * 0.5
        chunk["spread"] = chunk["askPrice"] - chunk["bidPrice"]
        chunk["ts"] = pd.to_datetime(chunk["timestamp"], unit="ms", utc=True)
        chunk = chunk.set_index("ts")
        ohlc = chunk["mid"].resample(rule).agg(["first", "max", "min", "last"]).rename(
            columns={"first": "open", "max": "high", "min": "low", "last": "close"}
        )
        ohlc["bid"] = chunk["bidPrice"].resample(rule).last()
        ohlc["ask"] = chunk["askPrice"].resample(rule).last()
        ohlc["spread"] = chunk["spread"].resample(rule).mean()
        ohlc["tick_count"] = chunk["mid"].resample(rule).count()
        ohlc = ohlc.dropna(subset=["close"])
        aggs.append(ohlc)
        print(f"  chunk {i+1}: {len(chunk):,} ticks -> {len(ohlc):,} bars")

    bars = pd.concat(aggs)
    # merge overlapping bar boundaries (chunks may span the same bar)
    bars = bars.groupby(level=0).agg({
        "open": "first", "high": "max", "low": "min", "close": "last",
        "bid": "last", "ask": "last", "spread": "mean", "tick_count": "sum",
    })
    bars = bars.sort_index()
    bars["ret"] = bars["close"].pct_change().fillna(0.0)
    bars["lret"] = np.log(bars["close"]).diff().fillna(0.0)
    bars["atr"] = (bars["high"] - bars["low"]).rolling(14, min_periods=1).mean()
    bars["rv"] = bars["lret"].rolling(20, min_periods=5).std() * np.sqrt(20)
    bars["hour"] = bars.index.hour
    h = bars["hour"]
    bars["session"] = np.where(
        h < 7, "asia",
        np.where(h < 13, "london",
                 np.where(h < 17, "ny_overlap", "ny_late"))
    )
    # placeholder feature cols for sleeves that read them (long history has no L2)
    bars["l2_imb"] = 0.5
    bars["vpin"] = 0.10
    bars["regime"] = 0
    bars["ewm_drift"] = 0.0
    bars["vol_ratio"] = 0.5
    bars.to_parquet(cache, compression="zstd")
    print(f"wrote {cache}: {len(bars):,} bars")
    return bars


if __name__ == "__main__":
    import sys
    sec = int(sys.argv[1]) if len(sys.argv) > 1 else 900
    b = build_bars_long(sec, force="--force" in sys.argv)
    print(f"bars {sec}s: {len(b):,}, range {b.index[0]} -> {b.index[-1]}")
    print(f"spread mean {b['spread'].mean():.4f}")

"""Build warmup_XAUUSD_H1.csv from the Duka tick file.

Output format: bar_start_ms,open,high,low,close
Matches what XauTrendFollow1hEngine::warmup_from_csv() expects.
"""
from __future__ import annotations
import os
import pandas as pd

TICK = os.path.expanduser("~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv")
OUT = "/Users/jo/omega_repo/phase1/signal_discovery/warmup_XAUUSD_H1.csv"


def main():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    chunks = []
    print(f"streaming {TICK}...")
    for i, chunk in enumerate(pd.read_csv(
        TICK, chunksize=5_000_000,
        dtype={"timestamp": "int64", "askPrice": "float64", "bidPrice": "float64"},
    )):
        chunk = chunk[(chunk["askPrice"] > chunk["bidPrice"]) & (chunk["bidPrice"] > 0)]
        chunk["mid"] = (chunk["askPrice"] + chunk["bidPrice"]) * 0.5
        chunk["ts"] = pd.to_datetime(chunk["timestamp"], unit="ms", utc=True)
        chunk = chunk.set_index("ts")
        o = chunk["mid"].resample("3600s").agg(["first", "max", "min", "last"]).rename(
            columns={"first": "open", "max": "high", "min": "low", "last": "close"}
        )
        o = o.dropna(subset=["close"])
        chunks.append(o)
        print(f"  chunk {i+1}: {len(chunk):,} ticks -> {len(o):,} H1 bars")

    bars = pd.concat(chunks)
    bars = bars.groupby(level=0).agg({
        "open": "first", "high": "max", "low": "min", "close": "last",
    }).sort_index()
    bars["bar_start_ms"] = (bars.index.astype("int64") // 1_000_000)
    out = bars[["bar_start_ms", "open", "high", "low", "close"]]
    out.to_csv(OUT, index=False)
    print(f"wrote {OUT}: {len(out):,} H1 bars  ({bars.index[0]} -> {bars.index[-1]})")


if __name__ == "__main__":
    main()

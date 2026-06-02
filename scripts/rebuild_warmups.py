#!/usr/bin/env python3
"""Rebuild engine warm-seed CSVs from captured l2_ticks_*.csv so engine boxes
seed at CURRENT price instead of a frozen repo snapshot.

Background (2026-06-01): the XauStraddle engines warm-seed their breakout box from
phase1/signal_discovery/warmup_XAUUSD_M{15,30}.csv -- a manually-built, git-committed
OHLC snapshot. When gold drifted ~230pts away from the snapshot, the stale box sat
far off-market and produced phantom 0-second fills (see the gap/arm guards in
XauStraddleM30Engine). The raw data was there all along: the live binary writes
C:\\Omega\\logs\\l2_ticks_<SYM>_YYYY-MM-DD.csv every tick. This script resamples
those ticks (mid price) into the warmup bar files, so a deploy/restart seeds fresh.

Wired into OMEGA.ps1 deploy AFTER git reset, BEFORE Start-Service, so the fresh
files override the git snapshot for the run. Safe to run by hand too.

l2_ticks schema:  ts_ms,mid,bid,ask,l2_imb,...   (mid = column 1)
warmup schema  :  bar_start_ms,open,high,low,close   (ts in ms; seed_from_csv skips header)

Usage:
  py scripts/rebuild_warmups.py [--logs DIR] [--out DIR] [--days N]
Defaults target the VPS layout (C:\\Omega\\logs, C:\\Omega\\phase1\\signal_discovery).
Falls back to repo-relative paths when those don't exist (local dev / Mac mirror).
"""
from __future__ import annotations
import argparse
import glob
import os
import sys

import pandas as pd

# (symbol, timeframe_minutes, output_filename, keep_last_bars)
TARGETS = [
    ("XAUUSD", 15, "warmup_XAUUSD_M15.csv", 6000),
    ("XAUUSD", 30, "warmup_XAUUSD_M30.csv", 4000),
    ("ESTX50",  5, "warmup_ESTX50_M5.csv",  4000),
    # Index straddle cells. Symbols with live l2_ticks (NAS100/US500/USTEC) auto-
    # refresh here; GER40/UK100 have no capture yet -> skip -> keep the bundled
    # warmup (the straddle gap+arm guards make a stale seed box safe regardless).
    ("GER40",  15, "warmup_GER40_M15.csv",  4000),
    ("GER40",  30, "warmup_GER40_M30.csv",  3500),
    ("NAS100", 15, "warmup_NAS100_M15.csv", 6000),
    ("NAS100", 30, "warmup_NAS100_M30.csv", 4000),
    ("UK100",  30, "warmup_UK100_M30.csv",  3500),
    ("UK100", 240, "warmup_UK100_M240.csv", 2000),
]


def _default_dirs():
    win_logs = r"C:\Omega\logs"
    win_out = r"C:\Omega\phase1\signal_discovery"
    if os.path.isdir(win_logs):
        return win_logs, win_out
    # local / Mac fallbacks
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for logs in (os.path.expanduser("~/Tick/l2_xau_vps"), os.path.join(here, "logs")):
        if os.path.isdir(logs):
            return logs, os.path.join(here, "phase1", "signal_discovery")
    return win_logs, os.path.join(here, "phase1", "signal_discovery")


def _load_ticks(logs_dir: str, symbol: str, days: int) -> pd.DataFrame | None:
    paths = sorted(glob.glob(os.path.join(logs_dir, f"l2_ticks_{symbol}_*.csv")))
    if not paths:
        return None
    paths = paths[-days:] if days > 0 else paths
    frames = []
    for p in paths:
        try:
            df = pd.read_csv(p, usecols=["ts_ms", "mid"],
                             dtype={"ts_ms": "int64", "mid": "float64"})
        except Exception as e:  # malformed / partial day -- skip, don't abort
            print(f"  [skip] {os.path.basename(p)}: {e}")
            continue
        df = df[(df["mid"] > 0) & (df["ts_ms"] > 0)]
        if len(df):
            frames.append(df)
    if not frames:
        return None
    return pd.concat(frames, ignore_index=True)


def _resample(df: pd.DataFrame, tf_min: int) -> pd.DataFrame:
    df = df.drop_duplicates(subset="ts_ms").sort_values("ts_ms")
    idx = pd.to_datetime(df["ts_ms"], unit="ms", utc=True)
    s = pd.Series(df["mid"].values, index=idx)
    bars = s.resample(f"{tf_min}min").agg(["first", "max", "min", "last"]).dropna()
    bars.columns = ["open", "high", "low", "close"]
    bars["bar_start_ms"] = (bars.index.astype("int64") // 1_000_000)
    return bars[["bar_start_ms", "open", "high", "low", "close"]]


def main() -> int:
    dl, do = _default_dirs()
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs", default=dl, help="dir holding l2_ticks_*.csv")
    ap.add_argument("--out", default=do, help="warmup output dir (phase1/signal_discovery)")
    ap.add_argument("--days", type=int, default=30, help="most-recent N daily files per symbol")
    args = ap.parse_args()

    print(f"[rebuild_warmups] logs={args.logs} out={args.out} days={args.days}")
    os.makedirs(args.out, exist_ok=True)
    rebuilt, skipped = 0, 0
    for symbol, tf_min, out_name, keep in TARGETS:
        ticks = _load_ticks(args.logs, symbol, args.days)
        if ticks is None or len(ticks) < 100:
            print(f"  [skip] {out_name}: no l2_ticks for {symbol} (engine keeps git snapshot)")
            skipped += 1
            continue
        bars = _resample(ticks, tf_min)
        if len(bars) < 50:
            print(f"  [skip] {out_name}: only {len(bars)} bars resampled")
            skipped += 1
            continue
        if keep > 0:
            bars = bars.tail(keep)
        out_path = os.path.join(args.out, out_name)
        tmp = out_path + ".tmp"
        bars.to_csv(tmp, index=False)
        os.replace(tmp, out_path)  # atomic
        last_px = float(bars["close"].iloc[-1])
        print(f"  [ok]   {out_name}: {len(bars)} M{tf_min} bars, "
              f"last close={last_px:.2f} ({bars['bar_start_ms'].iloc[-1]})")
        rebuilt += 1
    print(f"[rebuild_warmups] done: {rebuilt} rebuilt, {skipped} skipped")
    return 0  # never fail the deploy -- a skip just keeps the git snapshot


if __name__ == "__main__":
    sys.exit(main())

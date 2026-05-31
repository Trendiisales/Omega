#!/usr/bin/env python3
"""Fetch the VIX term-structure ratio (VIX/VIX3M) and write it where the live
IndexSeasonalEngine reads it: data/vix_term_ratio.txt  ("epoch_sec,ratio").

WHY: the broker FIX feed carries VIX.F (level) but NOT VIX3M, so the engine's
term-structure gate (skip seasonal entry when ratio>=1.05; Sharpe 0.69->0.80,
maxDD halved) needs an external source for VIX3M. This script supplies it.

DEPLOY (VPS, operator):
  Schedule daily after the US close (e.g. 21:30 UTC) via Windows Task Scheduler:
      python C:\\Omega\\tools\\fetch_vix_ratio.py
  It writes C:\\Omega\\data\\vix_term_ratio.txt. The engine reads the last line,
  ignores it if older than vix_max_age_days (4) -> degrades to the proven ungated
  edge, so a missed run or dead network never stops trading.

Requires: pip install yfinance pandas   (same dep as vix-engine/vix_term.py).
"""
import os, sys, time

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "vix_term_ratio.txt")

def main():
    try:
        import yfinance as yf
    except Exception as e:
        print(f"[fetch_vix_ratio] yfinance missing: {e}", file=sys.stderr); sys.exit(1)
    d = yf.download(["^VIX", "^VIX3M"], period="10d", interval="1d", progress=False, auto_adjust=False)
    closes = d["Close"].dropna()
    if closes.empty:
        print("[fetch_vix_ratio] empty download", file=sys.stderr); sys.exit(1)
    last = closes.iloc[-1]
    vix   = float(last["^VIX"])
    vix3m = float(last["^VIX3M"])
    if vix <= 0 or vix3m <= 0:
        print("[fetch_vix_ratio] bad values", file=sys.stderr); sys.exit(1)
    ratio = vix / vix3m
    # timestamp = the bar's date at UTC midnight (epoch sec)
    ts = int(closes.index[-1].timestamp())
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as f:
        f.write(f"{ts},{ratio:.5f}\n")
    print(f"[fetch_vix_ratio] wrote {OUT}: ts={ts} VIX={vix:.2f} VIX3M={vix3m:.2f} ratio={ratio:.4f}")

if __name__ == "__main__":
    main()

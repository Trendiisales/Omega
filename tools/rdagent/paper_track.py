#!/usr/bin/env python3
"""
Paper-trade tracker + INDEPENDENT verification of the reversal sleeve.

Deliberately a clean, separate implementation from reversal_sweep.py — if this
simulator reproduces the ~0.72 Sharpe, that's real confirmation (no shared bug).

Simulates the tuned book daily (L3 reversal, hold 5d, quintile long/short,
market-neutral), marks to market, and writes an equity curve + live stats.
  --verify : backtest the whole window, print stats (the re-verification)
  default  : same sim, also appends a paper ledger CSV for forward tracking

    python paper_track.py --close-csv ~/Omega/data/rdagent/sp500_close.csv --verify
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import pandas as pd

L, HOLD, FRAC, CLIP, MIN_PX = 3, 5, 0.20, 0.20, 5.0


def simulate(close: pd.DataFrame, cost_bps: float) -> pd.Series:
    fwd = close.shift(-1) / close - 1.0
    rev = -(close / close.shift(L) - 1.0)            # reversal: biggest losers rank high
    cost = cost_bps / 1e4
    dates = list(close.index)
    longs = shorts = []
    rets = {}
    for i, d in enumerate(dates):
        if d not in fwd.index:
            continue
        liquid = close.loc[d][close.loc[d] >= MIN_PX].index
        sig = rev.loc[d].reindex(liquid).dropna()
        fr = fwd.loc[d].reindex(sig.index).dropna().clip(-CLIP, CLIP)
        sig = sig.reindex(fr.index)
        if len(sig) < 40:
            continue
        turn = 0.0
        if i % HOLD == 0:
            k = max(1, int(len(sig) * FRAC))
            longs = list(sig.sort_values(ascending=False).head(k).index)   # losers -> long
            shorts = list(sig.sort_values().head(k).index)                 # winners -> short
            turn = 1.0
        L_ok = [s for s in longs if s in fr.index]
        S_ok = [s for s in shorts if s in fr.index]
        if not L_ok or not S_ok:
            continue
        rets[d] = fr[L_ok].mean() - fr[S_ok].mean() - turn * cost
    return pd.Series(rets)


def stats(r: pd.Series) -> dict:
    r = r.dropna()
    eq = (1 + r).cumprod()
    dd = float((eq / eq.cummax() - 1).min())
    return {"days": len(r), "ann_return": round(float(r.mean() * 252), 3),
            "sharpe": round(float(r.mean() / r.std() * np.sqrt(252)), 2),
            "max_drawdown": round(dd, 3), "hit_rate": round(float((r > 0).mean()), 3),
            "final_equity": round(float(eq.iloc[-1]), 3)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--close-csv", default=str(Path.home() / "Omega" / "data" / "rdagent" / "sp500_close.csv"))
    ap.add_argument("--cost-bps", type=float, default=3.0)
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--out", default=str(Path.home() / "Omega" / "data" / "rdagent" / "paper_ledger.csv"))
    a = ap.parse_args()
    close = pd.read_csv(a.close_csv, index_col=0, parse_dates=True).sort_index()
    r = simulate(close, a.cost_bps)
    s = stats(r)
    print(f"\nIndependent verification · L{L}/H{HOLD}/quintile · {a.cost_bps}bps · {close.shape[1]} names")
    print(f"  {s['days']} days {r.index.min().date()}..{r.index.max().date()}")
    print(f"  Sharpe {s['sharpe']} | ann {s['ann_return']*100:.1f}% | maxDD {s['max_drawdown']*100:.1f}% | "
          f"hit {s['hit_rate']*100:.0f}% | equity x{s['final_equity']}")
    eq = (1 + r).cumprod()
    led = pd.DataFrame({"daily_return": r, "equity": eq})
    led.to_csv(a.out)
    print(f"  ledger -> {a.out}")
    if not a.verify:
        print("  (forward mode: re-run daily after the book refresh to extend the ledger)")


if __name__ == "__main__":
    main()

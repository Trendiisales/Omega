#!/usr/bin/env python3
"""
THE CORRECT SOLUTION — protected beta core + market-neutral alpha overlay.

Lesson from regime_fix_bt: timing the index can only cut drawdown, never beat
buy-hold risk-adjusted. To beat it you must add return that is UNCORRELATED to the
market. The reversal sleeve (long losers / short winners, market-neutral, beta~0)
is exactly that. Blend it onto a vol-targeted long-beta core.

  core (beta)  : vol-targeted long index, HALF size in bear, never short (the fixed book)
  overlay(alpha): rev_3 / hold-5 / quintile long-short, dollar-neutral (the 0.37 sleeve)
  blend        : w*core + (1-w)*overlay, swept for best Sharpe

Compares to buy_hold over 2020-2026, overall + per-year. Wins = higher Sharpe AND
lower maxDD than buy_hold.

    python regime_combo_bt.py --close-csv /tmp/sp500_long_close.csv
"""
from __future__ import annotations

import argparse

import numpy as np
import pandas as pd


def stats(r):
    r = r.dropna()
    eq = (1 + r).cumprod()
    return (round(float(r.mean()*252), 3), round(float(r.mean()/r.std()*np.sqrt(252)), 2),
            round(float((eq/eq.cummax()-1).min()), 3))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--close-csv", default="/tmp/sp500_long_close.csv")
    ap.add_argument("--target-vol", type=float, default=0.10)
    ap.add_argument("--cost-bps", type=float, default=5.0)
    a = ap.parse_args()
    close = pd.read_csv(a.close_csv, index_col=0, parse_dates=True).sort_index()
    ret1 = close.pct_change().clip(-0.20, 0.20)
    idx = close.mean(axis=1)
    ir = idx.pct_change().clip(-0.20, 0.20)
    sma200, ret5, vol20 = idx.rolling(200).mean(), idx.pct_change(5), ir.rolling(20).std()
    lev = (a.target_vol/np.sqrt(252) / vol20).clip(0, 2.0)
    cost = a.cost_bps/1e4
    L, H, FRAC = 3, 5, 0.20
    rev = -(close / close.shift(L) - 1)
    fwd = close.shift(-1)/close - 1
    dates = list(close.index)

    core, alpha = {}, {}
    pl = pr = []
    pprev = 0.0
    for i in range(1, len(dates)):
        d, p = dates[i], dates[i-1]
        if sma200.loc[p] != sma200.loc[p] or vol20.loc[p] != vol20.loc[p]:
            continue
        crash = bool(ret5.loc[p] < -0.05) if ret5.loc[p] == ret5.loc[p] else False
        bull = bool(idx.loc[p] >= sma200.loc[p]) and not crash
        r = float(ir.loc[d]) if ir.loc[d] == ir.loc[d] else 0.0
        lv = float(lev.loc[p]) * (1.0 if bull else 0.5)         # core: vol-target, half in bear, no short
        core[d] = lv*r - abs(lv-pprev)*cost; pprev = lv
        # alpha: market-neutral reversal, rebalance every H days
        if i % H == 1:
            liquid = close.loc[p][close.loc[p] >= 5].index
            sg = rev.loc[p].reindex(liquid).dropna()
            k = max(1, int(len(sg)*FRAC))
            pl = list(sg.sort_values(ascending=False).head(k).index)
            pr = list(sg.sort_values().head(k).index)
        if pl and pr:
            fl = fwd.loc[p].reindex(pl).dropna() if p in fwd.index else pd.Series(dtype=float)
            # mark today's realized on held book
            rl = ret1.loc[d].reindex(pl).dropna().mean()
            rs = ret1.loc[d].reindex(pr).dropna().mean()
            alpha[d] = float(rl - rs) - (cost if i % H == 1 else 0)

    core = pd.Series(core); alpha = pd.Series(alpha).reindex(core.index).fillna(0)
    bh = pd.Series({d: float(ir.loc[d]) for d in core.index})

    print(f"\nTHE CORRECT SOLUTION · {close.shape[1]} names · {core.index.min().date()}..{core.index.max().date()} · {a.cost_bps}bps\n")
    print(f"{'leg':<22}  ann    Sharpe  maxDD")
    for nm, s in [("buy_hold (beta)", bh), ("core: vol-target+nobear-short", core), ("overlay: reversal MN", alpha)]:
        an, sh, dd = stats(s)
        print(f"{nm:<22} {an!s:>5}   {sh!s:>5}  {dd!s:>6}")
    bh_sh = stats(bh)[1]
    print(f"\n{'blend w*core+(1-w)*alpha':<22}  ann    Sharpe  maxDD")
    best = None
    for w in [0.3, 0.4, 0.5, 0.6, 0.7]:
        bl = w*core + (1-w)*alpha
        an, sh, dd = stats(bl)
        flag = "  <-- beats B&H Sharpe" if sh > bh_sh else ""
        print(f"  w={w:<19} {an!s:>5}   {sh!s:>5}  {dd!s:>6}{flag}")
        if best is None or sh > best[1]:
            best = (w, sh, bl)

    print("\nPER-YEAR ann return (best blend vs buy_hold):")
    bl = best[2]
    yrs = sorted({d.year for d in core.index})
    print(f"{'':<10} " + " ".join(f"{y:>6}" for y in yrs))
    for nm, s in [("buy_hold", bh), (f"blend w={best[0]}", bl)]:
        print(f"{nm:<10} " + " ".join(f"{s[[d for d in s.index if d.year==y]].sum()*100:>5.0f}%" for y in yrs))


if __name__ == "__main__":
    main()

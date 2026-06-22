#!/usr/bin/env python3
"""
Long-period (2020-now) test of the REGIME-GATING CONCEPT on daily data.

The deployed flat-by-close book needs intraday bars (only since mid-2024), so this
uses DAILY close-to-close as a proxy to answer the real question: does long-in-bull
/ short-in-bear survive the 2020 COVID crash and the 2022 bear, or was the 1.40 a
2026 artifact? Regime = equal-weight index vs 200d SMA (+ 5d-crash override).

Strategies compared (winsorized ±20%, price>$5):
  buy_hold        long the index every day (benchmark)
  regime_longflat long index in BULL, CASH in BEAR (protection only)
  regime_timing   long index in BULL, SHORT index in BEAR (full regime switch)
  regime_topk     long top-K 20d-momentum in BULL, short index in BEAR (mirrors deployed)

Reports overall + per-year.

    python regime_longer_bt.py --close-csv /tmp/sp500_long_close.csv --topk 20 --cost-bps 5
"""
from __future__ import annotations

import argparse

import numpy as np
import pandas as pd


def metrics(r: pd.Series) -> dict:
    r = r.dropna()
    if len(r) < 20 or r.std() == 0:
        return {}
    eq = (1 + r).cumprod()
    return {"ann": round(float(r.mean()*252), 3),
            "sharpe": round(float(r.mean()/r.std()*np.sqrt(252)), 2),
            "maxdd": round(float((eq/eq.cummax()-1).min()), 3)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--close-csv", default="/tmp/sp500_long_close.csv")
    ap.add_argument("--topk", type=int, default=20)
    ap.add_argument("--cost-bps", type=float, default=5.0)
    a = ap.parse_args()
    close = pd.read_csv(a.close_csv, index_col=0, parse_dates=True).sort_index()
    ret1 = close.pct_change().clip(-0.20, 0.20)
    idx = close.mean(axis=1)
    idx_ret = idx.pct_change().clip(-0.20, 0.20)
    sma200 = idx.rolling(200).mean()
    ret5 = idx.pct_change(5)
    mom20 = close / close.shift(20) - 1
    cost = a.cost_bps / 1e4

    dates = [d for d in close.index if d == d]
    bh, lf, tm, tk = {}, {}, {}, {}
    prev_top = set()
    for i in range(1, len(dates)):
        d, p = dates[i], dates[i-1]
        if sma200.loc[p] != sma200.loc[p]:
            continue
        crash = bool(ret5.loc[p] < -0.05) if ret5.loc[p] == ret5.loc[p] else False
        bull = bool(idx.loc[p] >= sma200.loc[p]) and not crash
        ir = float(idx_ret.loc[d]) if idx_ret.loc[d] == idx_ret.loc[d] else 0.0
        bh[d] = ir
        lf[d] = ir if bull else 0.0
        tm[d] = ir if bull else -ir - cost
        if bull:
            liquid = close.loc[p][close.loc[p] >= 5].index
            top = list(mom20.loc[p].reindex(liquid).dropna().sort_values(ascending=False).head(a.topk).index)
            r = float(ret1.loc[d].reindex(top).dropna().mean())
            turn = len(set(top) ^ prev_top) / max(2*len(top), 1)
            tk[d] = r - turn * cost
            prev_top = set(top)
        else:
            tk[d] = -ir - cost
            prev_top = set()

    S = {"buy_hold": pd.Series(bh), "regime_longflat": pd.Series(lf),
         "regime_timing": pd.Series(tm), "regime_topk": pd.Series(tk)}
    span = f"{pd.Series(bh).index.min().date()}..{pd.Series(bh).index.max().date()}"
    print(f"\nLong-period regime test · {close.shape[1]} names · {span} · {a.cost_bps}bps\n")
    print(f"{'strategy':<16}  ann    Sharpe  maxDD")
    for k, r in S.items():
        m = metrics(r)
        print(f"{k:<16} {m.get('ann')!s:>5}   {m.get('sharpe')!s:>5}  {m.get('maxdd')!s:>6}")

    print("\nPER-YEAR ann return (does 2020 crash / 2022 bear break it?):")
    yrs = sorted({d.year for d in S['buy_hold'].index})
    print(f"{'strategy':<16} " + " ".join(f"{y:>7}" for y in yrs))
    for k, r in S.items():
        cells = []
        for y in yrs:
            ry = r[[d for d in r.index if d.year == y]]
            cells.append(f"{ry.sum()*100:>6.0f}%" if len(ry) else "    -  ")
        print(f"{k:<16} " + " ".join(cells))


if __name__ == "__main__":
    main()

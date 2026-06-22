#!/usr/bin/env python3
"""
Full lever sweep on the post-2020 reversal sleeve — find the most accurate config.

Levers tested:
  signal  : raw reversal (-ret_L)  vs  RESIDUAL reversal (-idiosyncratic return,
            i.e. return minus beta*market over a rolling window — isolates true
            mean-reversion from beta-bounce; the biggest known improvement)
  weight  : equal  vs  inverse-vol (risk-parity, down-weights wild names)
  L       : reversal lookback (2,3,5,10)
  H       : hold days (3,5,10,20)  — the turnover/cost lever
  frac    : selectivity (0.1 decile, 0.2 quintile)
  + skip-recent-day variant (avoid 1-day bid-ask bounce contamination)

Reports top configs by NET Sharpe (after cost) on 2021-2026, with IC/beta/both-regime.

    python reversal_sweep.py --close-csv /tmp/sp500_recent_close.csv --cost-bps 3
"""
from __future__ import annotations

import argparse
import itertools

import numpy as np
import pandas as pd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--close-csv", default="/tmp/sp500_recent_close.csv")
    ap.add_argument("--provider", default=None)
    ap.add_argument("--region", default="us")
    ap.add_argument("--market", default="sp500")
    ap.add_argument("--cost-bps", type=float, default=3.0)
    ap.add_argument("--clip", type=float, default=0.20)
    ap.add_argument("--min-price", type=float, default=5.0)
    ap.add_argument("--quick", action="store_true")
    a = ap.parse_args()
    if a.provider:
        import validate_factors as vf
        close = vf._close_qlib(a.provider, a.region, a.market)
    else:
        close = pd.read_csv(a.close_csv, index_col=0, parse_dates=True).sort_index()
    ret1 = close.pct_change()
    mkt = ret1.mean(axis=1)
    vol20 = ret1.rolling(20).std()
    # rolling beta per name vs equal-weight market (60d)
    cov = ret1.rolling(60).cov(mkt)
    var = mkt.rolling(60).var()
    beta = cov.div(var, axis=0)
    resid1 = ret1.sub(beta.mul(mkt, axis=0))        # daily idiosyncratic return
    cost = a.cost_bps / 1e4
    dates = list(close.index)
    fwd = close.shift(-1) / close - 1.0

    def signal(kind, L, skip):
        if kind == "raw":
            base = close.shift(skip) / close.shift(L) - 1.0
            return -base
        # residual: cumulative idiosyncratic return over [skip..L]
        return -(resid1.shift(skip).rolling(L - skip).sum())

    def run(kind, wt, L, H, frac, skip):
        sig = signal(kind, L, skip)
        liquid_mask = close >= a.min_price
        spread, ics = {}, {}
        longs = shorts = []
        wl = ws = None
        for i, d in enumerate(dates):
            if d not in fwd.index:
                continue
            row = sig.loc[d].where(liquid_mask.loc[d]).dropna()
            fr = fwd.loc[d].reindex(row.index).dropna().clip(-a.clip, a.clip)
            row = row.reindex(fr.index)
            if len(row) < 40:
                continue
            if i % H == 0:
                k = max(1, int(len(row) * frac))
                longs = list(row.sort_values(ascending=False).head(k).index)
                shorts = list(row.sort_values().head(k).index)
                if wt == "invvol":
                    v = vol20.loc[d]
                    wl = (1 / v.reindex(longs)).replace([np.inf, np.nan], 0); wl /= wl.sum() or 1
                    ws = (1 / v.reindex(shorts)).replace([np.inf, np.nan], 0); ws /= ws.sum() or 1
                else:
                    wl = pd.Series(1 / len(longs), index=longs); ws = pd.Series(1 / len(shorts), index=shorts)
                turn = 1.0 if i == 0 else min(1.0, 1.0)  # full rebalance every H
            else:
                turn = 0.0
            L_ok = [s for s in longs if s in fr.index]
            S_ok = [s for s in shorts if s in fr.index]
            if not L_ok or not S_ok:
                continue
            r = (fr[L_ok] * wl.reindex(L_ok)).sum() - (fr[S_ok] * ws.reindex(S_ok)).sum()
            spread[d] = r - turn * cost
        s = pd.Series(spread)
        if len(s) < 100:
            return None
        m = mkt.reindex(s.index)
        beta_s = float(np.polyfit(m.fillna(0), s, 1)[0]) if m.std() > 0 else 0.0
        up, dn = s[m > 0], s[m < 0]
        sh = float(s.mean() / s.std() * np.sqrt(252)) if s.std() else 0.0
        return {"sig": kind, "wt": wt, "L": L, "H": H, "frac": frac, "skip": skip,
                "net_sharpe": round(sh, 2), "ann": round(float(s.mean() * 252), 3),
                "beta": round(beta_s, 2), "dn_bps": round(float(dn.mean() * 1e4), 1),
                "up_bps": round(float(up.mean() * 1e4), 1)}

    rows = []
    if a.quick:  # focused: winning-config family only (raw/equal, skip 0 vs 1)
        grid = itertools.product(["raw"], ["equal"], [3, 5], [5, 10], [0.2], [0, 1])
    else:
        grid = itertools.product(["raw", "resid"], ["equal", "invvol"], [2, 3, 5, 10], [3, 5, 10, 20], [0.1, 0.2], [0, 1])
    for kind, wt, L, H, frac, skip in grid:
        if skip >= L:
            continue
        r = run(kind, wt, L, H, frac, skip)
        if r:
            rows.append(r)
    df = pd.DataFrame(rows).sort_values("net_sharpe", ascending=False)
    print(f"\nPost-2020 reversal lever sweep · {close.shape[1]} names · {a.cost_bps}bps · {len(rows)} configs\n")
    print("TOP 12 by net Sharpe (after cost):")
    print(df.head(12).to_string(index=False))
    print("\nbaseline (raw/equal/L5/H20/0.1) for reference:")
    print(df[(df.sig=="raw")&(df.wt=="equal")&(df.L==5)&(df.H==20)&(df.frac==0.1)&(df.skip==0)].to_string(index=False))


if __name__ == "__main__":
    main()

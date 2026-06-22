#!/usr/bin/env python3
"""
Turnover-reduction search on the ONE proven-real signal: CSI300 short-term reversal.

rev_L (= -L-day return) has real, neutral predictive IC but dies on cost because
daily rebalancing of a fast signal is high-turnover. Fix: hold H days (rebalance
every H), and trade only the extreme fraction. Search (L, H, frac) for the config
whose NET-of-cost OOS Sharpe is positive — i.e. a cost-robust version of the edge.

    python reversal_hold_bt.py --provider ~/.qlib/qlib_data/cn_data --market csi300 --cost-bps 5
"""
from __future__ import annotations

import argparse

import numpy as np
import pandas as pd
from scipy.stats import spearmanr

import validate_factors as vf


def bt(close, fwd, mkt, L, H, frac, cost, dates, clip=0.20, min_price=5.0):
    spread, ics = {}, {}
    longs = shorts = []
    for i, d in enumerate(dates):
        fac = -(close.loc[d] / close.loc[:d].iloc[-1 - L] - 1) if len(close.loc[:d]) > L else None
        if fac is None:
            continue
        # data hygiene: drop illiquid/penny names, winsorize fwd returns (kill split/glitch tails)
        liquid = close.loc[d][close.loc[d] >= min_price].index
        fac = fac[fac.index.isin(liquid)]
        fr = fwd.loc[d].reindex(liquid).dropna().clip(-clip, clip)
        if i % H == 0:  # rebalance only every H days
            row = fac.dropna()
            common = row.index.intersection(fr.index)
            if len(common) < 10:
                continue
            row = row[common]
            k = max(1, int(len(common) * frac))
            new_l = list(row.sort_values(ascending=False).head(k).index)
            new_s = list(row.sort_values().head(k).index)
            turn = (len(set(new_l) ^ set(longs)) + len(set(new_s) ^ set(shorts))) / (2 * k)
            longs, shorts = new_l, new_s
        else:
            turn = 0.0
        L_ok = [s for s in longs if s in fr.index]
        S_ok = [s for s in shorts if s in fr.index]
        if not L_ok or not S_ok:
            continue
        spread[d] = fr[L_ok].mean() - fr[S_ok].mean() - turn * cost
        row2 = fac.dropna()
        c2 = row2.index.intersection(fr.index)
        ic = spearmanr(row2[c2].values, fr[c2].values).correlation
        if ic == ic:
            ics[d] = ic
    s = pd.Series(spread)
    if len(s) < 30:
        return None
    sh = float(s.mean() / s.std() * np.sqrt(252)) if s.std() > 0 else 0.0
    return {"L": L, "H": H, "frac": frac, "net_sharpe": round(sh, 2),
            "ann": round(float(s.mean() * 252), 3), "mean_IC": round(float(pd.Series(ics).mean()), 4),
            "n": len(s)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--provider", default="~/.qlib/qlib_data/cn_data")
    ap.add_argument("--region", default="cn")
    ap.add_argument("--market", default="csi300")
    ap.add_argument("--close-csv", default=None, help="date x ticker close matrix CSV (overrides qlib)")
    ap.add_argument("--cost-bps", type=float, default=5.0)
    ap.add_argument("--oos-frac", type=float, default=0.4)
    a = ap.parse_args()
    if a.close_csv:
        close = pd.read_csv(a.close_csv, index_col=0, parse_dates=True).sort_index()
    else:
        close = vf._close_qlib(a.provider, a.region, a.market)
    fwd = close.shift(-1) / close - 1.0
    mkt = close.mean(axis=1).pct_change().shift(-1)
    dates = [d for d in close.index if d in fwd.index]
    oos = dates[int(len(dates) * (1 - a.oos_frac)):]
    cost = a.cost_bps / 1e4
    print(f"\nCSI300 reversal turnover search · OOS {oos[0].date()}..{oos[-1].date()} · {a.cost_bps}bps net\n")
    rows = []
    for L in (3, 5, 10, 20):
        for H in (1, 5, 10, 20):
            for frac in (0.1, 0.2, 0.33):
                r = bt(close, fwd, mkt, L, H, frac, cost, oos)
                if r:
                    rows.append(r)
    df = pd.DataFrame(rows).sort_values("net_sharpe", ascending=False)
    print("TOP 8 net-of-cost configs:")
    print(df.head(8).to_string(index=False))
    print("\nL=reversal lookback, H=hold days, frac=extreme fraction traded. net_sharpe>0 after cost = cost-robust edge.")


if __name__ == "__main__":
    main()

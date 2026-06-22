#!/usr/bin/env python3
"""
THE FIX — regime/risk approaches that survive 2020-2026 (vs the failed short-bear).

Diagnosis from regime_longer_bt: shorting the bear is the killer (cash-bear +7.8%
Sharpe 0.67 vs short-bear -3.8% Sharpe -0.21). Cure: never short; size by volatility.

Candidates (daily, winsorized ±20%, no lookahead — exposure uses PRIOR-day signals):
  buy_hold      long index always (benchmark, Sharpe ~0.90)
  cash_bear     long index in BULL, CASH in BEAR (no short)
  vol_target    long index scaled to a constant 10% vol target (no regime gate)
  vt_trend      vol-targeted long in BULL, CASH in BEAR
  vt_softbear   vol-targeted long always, HALF size in BEAR (defensive, never short)

Wins = higher Sharpe + lower maxDD than buy_hold. Reports overall + per-year.

    python regime_fix_bt.py --close-csv /tmp/sp500_long_close.csv --target-vol 0.10
"""
from __future__ import annotations

import argparse

import numpy as np
import pandas as pd


def stats(r):
    r = r.dropna()
    if len(r) < 20 or r.std() == 0:
        return {}
    eq = (1 + r).cumprod()
    return {"ann": round(float(r.mean()*252), 3), "sharpe": round(float(r.mean()/r.std()*np.sqrt(252)), 2),
            "maxdd": round(float((eq/eq.cummax()-1).min()), 3)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--close-csv", default="/tmp/sp500_long_close.csv")
    ap.add_argument("--target-vol", type=float, default=0.10)
    ap.add_argument("--cost-bps", type=float, default=5.0)
    a = ap.parse_args()
    close = pd.read_csv(a.close_csv, index_col=0, parse_dates=True).sort_index()
    idx = close.mean(axis=1)
    ir = idx.pct_change().clip(-0.20, 0.20)
    sma200 = idx.rolling(200).mean()
    ret5 = idx.pct_change(5)
    vol20 = ir.rolling(20).std()
    tgt_d = a.target_vol / np.sqrt(252)
    lev = (tgt_d / vol20).clip(0, 2.0)               # inverse-vol leverage, capped 2x
    cost = a.cost_bps / 1e4
    dates = list(close.index)

    bh, cb, vt, vtr, vts = {}, {}, {}, {}, {}
    prev_lev_t = prev_lev_r = prev_lev_s = 0.0
    for i in range(1, len(dates)):
        d, p = dates[i], dates[i-1]
        if sma200.loc[p] != sma200.loc[p] or vol20.loc[p] != vol20.loc[p]:
            continue
        crash = bool(ret5.loc[p] < -0.05) if ret5.loc[p] == ret5.loc[p] else False
        bull = bool(idx.loc[p] >= sma200.loc[p]) and not crash
        r = float(ir.loc[d]) if ir.loc[d] == ir.loc[d] else 0.0
        lv = float(lev.loc[p])
        bh[d] = r
        cb[d] = r if bull else 0.0
        vt[d] = lv * r - abs(lv - prev_lev_t) * cost; prev_lev_t = lv
        lr = lv if bull else 0.0
        vtr[d] = lr * r - abs(lr - prev_lev_r) * cost; prev_lev_r = lr
        ls = lv if bull else lv * 0.5
        vts[d] = ls * r - abs(ls - prev_lev_s) * cost; prev_lev_s = ls

    S = {"buy_hold": pd.Series(bh), "cash_bear": pd.Series(cb), "vol_target": pd.Series(vt),
         "vt_trend": pd.Series(vtr), "vt_softbear": pd.Series(vts)}
    span = f"{S['buy_hold'].index.min().date()}..{S['buy_hold'].index.max().date()}"
    print(f"\nTHE FIX · {close.shape[1]} names · {span} · target-vol {a.target_vol:.0%} · {a.cost_bps}bps\n")
    print(f"{'strategy':<14}  ann    Sharpe  maxDD   (buy_hold = benchmark)")
    bh_sh = stats(S['buy_hold'])['sharpe']
    for k, r in S.items():
        m = stats(r)
        flag = " <-- beats B&H" if (m.get('sharpe') or 0) > bh_sh else ""
        print(f"{k:<14} {m.get('ann')!s:>5}   {m.get('sharpe')!s:>5}  {m.get('maxdd')!s:>6}{flag}")

    print("\nPER-YEAR ann return:")
    yrs = sorted({d.year for d in S['buy_hold'].index})
    print(f"{'strategy':<14} " + " ".join(f"{y:>6}" for y in yrs))
    for k, r in S.items():
        cells = [f"{r[[d for d in r.index if d.year==y]].sum()*100:>5.0f}%" for y in yrs]
        print(f"{k:<14} " + " ".join(cells))


if __name__ == "__main__":
    main()

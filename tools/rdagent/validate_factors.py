#!/usr/bin/env python3
"""
Market-neutral factor validation (BACKTEST_TRUTH protocol).

Builds standard cross-sectional factors, forms a dollar-neutral long-short book
(long top tertile, short bottom tertile, daily), and validates each on the
OUT-OF-SAMPLE tail with the checks that separate real alpha from bull-beta:

  - OOS Sharpe of the long-short SPREAD (not the long basket)
  - IC (daily rank corr of factor vs next-day return) + IC info-ratio
  - market beta of the spread  (|beta|~0 = genuinely neutral, not closet-long)
  - up-day vs DOWN-day spread return (both-regime: must work when market falls)
  - net of cost (long-short rebalances daily -> high turnover)

A factor passes only if OOS net Sharpe>0.5 AND |beta|<0.3 AND down-day spread>0.
Reports for the bigcap universe; pass --provider cn_data to cross-check on CSI300
(300 names = real statistical power vs 30 megacaps).

    python validate_factors.py --bars /tmp/omega_bars_real --oos-frac 0.4 --cost-bps 5
"""
from __future__ import annotations

import argparse
import glob
import os
import re
from pathlib import Path

import numpy as np
import pandas as pd
from scipy.stats import spearmanr


def _close_qlib(provider: str, region: str, market: str) -> pd.DataFrame:
    import qlib
    from qlib.data import D
    qlib.init(provider_uri=str(Path(provider).expanduser()), region=region, kernels=1)
    insts = D.list_instruments(D.instruments(market), as_list=True)
    df = D.features(insts, ["$close"], freq="day")["$close"].unstack("instrument")
    return df.sort_index()


def _close_matrix(bars_dir: str) -> pd.DataFrame:
    cols = {}
    for f in sorted(glob.glob(os.path.join(bars_dir, "*.csv"))):
        sym = re.sub(r"_(d1|15m|5m|30m)$", "", Path(f).stem, flags=re.I)
        df = pd.read_csv(f).rename(columns=str.lower)
        idx = pd.to_datetime(df["ts"], unit="s") if "ts" in df else pd.to_datetime(df["date"])
        cols[sym] = pd.Series(df["close"].values, index=idx)
    return pd.DataFrame(cols).sort_index()


def _factors(close: pd.DataFrame) -> dict[str, pd.DataFrame]:
    ret1 = close.pct_change()
    return {
        "mom_60":  close / close.shift(60) - 1,
        "mom_20":  close / close.shift(20) - 1,
        "rev_5":  -(close / close.shift(5) - 1),
        "rev_1":  -ret1,
        "lowvol": -ret1.rolling(20).std(),
    }


def _zscore(df: pd.DataFrame) -> pd.DataFrame:
    return df.sub(df.mean(axis=1), axis=0).div(df.std(axis=1).replace(0, np.nan), axis=0)


def _eval(fac: pd.DataFrame, fwd: pd.DataFrame, mkt: pd.Series, cost: float, dates):
    """Dollar-neutral tertile long-short; return per-day spread + IC over `dates`."""
    spread, ics, prev_l, prev_s = {}, {}, set(), set()
    for d in dates:
        row = fac.loc[d].dropna()
        fr = fwd.loc[d].dropna()
        common = row.index.intersection(fr.index)
        if len(common) < 9:
            continue
        row, fr = row[common], fr[common]
        k = max(1, len(common) // 3)
        longs = list(row.sort_values(ascending=False).head(k).index)
        shorts = list(row.sort_values().head(k).index)
        turn = (len(set(longs) ^ prev_l) + len(set(shorts) ^ prev_s)) / (2 * k)
        spread[d] = fr[longs].mean() - fr[shorts].mean() - turn * cost
        ic = spearmanr(row.values, fr.values).correlation
        if ic == ic:
            ics[d] = ic
        prev_l, prev_s = set(longs), set(shorts)
    s = pd.Series(spread)
    ic = pd.Series(ics)
    if len(s) < 20:
        return None
    m = mkt.reindex(s.index)
    beta = float(np.polyfit(m.fillna(0), s, 1)[0]) if m.std() > 0 else 0.0
    up, dn = s[m > 0], s[m < 0]
    sharpe = float(s.mean() / s.std() * np.sqrt(252)) if s.std() > 0 else 0.0
    return {
        "oos_sharpe": round(sharpe, 2),
        "ann_return": round(float(s.mean() * 252), 3),
        "mean_IC": round(float(ic.mean()), 4),
        "IC_IR": round(float(ic.mean() / ic.std() * np.sqrt(252)), 2) if ic.std() > 0 else None,
        "mkt_beta": round(beta, 2),
        "downday_bps": round(float(dn.mean() * 1e4), 1) if len(dn) else None,
        "upday_bps": round(float(up.mean() * 1e4), 1) if len(up) else None,
        "n_days": len(s),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bars", default="/tmp/omega_bars_real")
    ap.add_argument("--provider", default=None, help="qlib provider dir (e.g. ~/.qlib/qlib_data/cn_data)")
    ap.add_argument("--region", default="cn")
    ap.add_argument("--market", default="csi300")
    ap.add_argument("--oos-frac", type=float, default=0.4)
    ap.add_argument("--cost-bps", type=float, default=5.0)
    a = ap.parse_args()
    close = _close_qlib(a.provider, a.region, a.market) if a.provider else _close_matrix(a.bars)
    fwd = close.shift(-1) / close - 1.0
    mkt = close.mean(axis=1).pct_change().shift(-1)  # next-day equal-weight market move
    facs = _factors(close)
    facs["combo"] = sum(_zscore(f) for f in facs.values()) / len(facs)
    cost = a.cost_bps / 1e4

    all_dates = [d for d in close.index if d in fwd.index]
    split = all_dates[int(len(all_dates) * (1 - a.oos_frac))]
    oos = [d for d in all_dates if d >= split]
    print(f"\nMarket-neutral factor validation · {close.shape[1]} names · OOS from {split.date()} ({len(oos)} days) · {a.cost_bps}bps")
    print("PASS = OOS net Sharpe>0.5 AND |beta|<0.3 AND down-day spread>0\n")
    rows = []
    for name, f in facs.items():
        r = _eval(f, fwd, mkt, cost, oos)
        if r:
            r["factor"] = name
            r["PASS"] = (r["oos_sharpe"] > 0.5 and abs(r["mkt_beta"]) < 0.3
                         and (r["downday_bps"] or -1) > 0)
            rows.append(r)
    df = pd.DataFrame(rows).set_index("factor")[
        ["oos_sharpe", "ann_return", "mean_IC", "IC_IR", "mkt_beta", "downday_bps", "upday_bps", "n_days", "PASS"]]
    print(df.to_string())
    print("\nmkt_beta ~0 = neutral (not closet-long) · downday_bps>0 = works when market falls")


if __name__ == "__main__":
    main()

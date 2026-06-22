#!/usr/bin/env python3
"""
Flat-by-close + regime gates + long/short, with up-day / DOWN-DAY decomposition.

All methods are flat-by-close (enter open, exit close, ZERO overnight risk). On top:
  long_flat            long top-K only (baseline)
  long_hivol_gate      long top-K, but sit in CASH on top-quartile-vol days
  long_bear_gate       long top-K, but sit in CASH when market < 50d SMA (bear)
  longshort_flat       long top-K / short bottom-K, market-neutral (can profit in a bear)
  longshort_hivol_gate market-neutral + skip top-quartile-vol days

Reports ann/Sharpe/maxDD/win + the mean return specifically on DOWN-MARKET days
(does it bleed or profit when the market falls?). Sample is mostly bull, so the
down-day column — not the headline — is the bear read.

    python bear_regime_bt.py --m15 /tmp/omega_15m --mlruns /tmp/omega_factors/mlruns \
        --topk 5 --cost-bps 5
"""
from __future__ import annotations

import argparse

import numpy as np
import pandas as pd

import intraday_bt as ib


def _market(sess):
    idx = pd.DataFrame({s: sess[s]["s_close"] for s in sess}).mean(axis=1).sort_index()
    ret = idx.pct_change()
    return pd.DataFrame({
        "idx": idx, "ret": ret,
        "sma50": idx.rolling(50).mean(),
        "vol20": ret.rolling(20).std(),
        "intraday": pd.DataFrame({s: sess[s]["intraday"] for s in sess}).mean(axis=1),
    })


def run(method, sess, sig, mkt, topk, cost_bps):
    cost = cost_bps / 1e4
    hivol_thr = mkt["vol20"].quantile(0.75)
    syms = [s for s in sig.columns if s in sess]
    dates = sorted(set(sig.index) & set.union(*[set(sess[s].index) for s in syms]) & set(mkt.index))
    rets, dmask = {}, {}
    prev = None
    for d in dates:
        if prev is None or prev not in sig.index:
            prev = d
            continue
        # regime flags (known at prior close)
        bear = bool(mkt.loc[prev, "idx"] < mkt.loc[prev, "sma50"]) if mkt.loc[prev, "sma50"] == mkt.loc[prev, "sma50"] else False
        hivol = bool(mkt.loc[prev, "vol20"] > hivol_thr) if mkt.loc[prev, "vol20"] == mkt.loc[prev, "vol20"] else False
        gated = (method == "long_hivol_gate" and hivol) or (method == "long_bear_gate" and bear) \
            or (method == "longshort_hivol_gate" and hivol)

        row = sig.loc[prev].dropna()
        radj = {s: row[s] / sess[s].loc[prev, "vol20"] for s in row.index
                if s in sess and d in sess[s].index and prev in sess[s].index
                and sess[s].loc[prev, "vol20"] == sess[s].loc[prev, "vol20"] and sess[s].loc[prev, "vol20"] > 0}
        ranked = [s for s, _ in sorted(radj.items(), key=lambda x: x[1], reverse=True)]
        prev = d
        if len(ranked) < 2 * topk if method.startswith("longshort") else len(ranked) < topk:
            continue
        dmask[d] = mkt.loc[d, "intraday"] < 0  # market fell intraday today
        if gated:
            rets[d] = 0.0  # in cash
            continue
        longs = ranked[:topk]
        if method.startswith("longshort"):
            shorts = ranked[-topk:]
            r = float(np.mean([sess[s].loc[d, "intraday"] for s in longs])) \
                - float(np.mean([sess[s].loc[d, "intraday"] for s in shorts]))
            r -= 2 * cost  # both legs round-trip daily
        else:
            r = float(np.mean([sess[s].loc[d, "intraday"] for s in longs])) - cost
        rets[d] = r
    r = pd.Series(rets)
    m = ib._metrics(r)
    dm = pd.Series(dmask).reindex(r.index).fillna(False)
    down = r[dm.values]
    m["downday_mean_bps"] = round(float(down.mean() * 1e4), 1) if len(down) else None
    m["n_downdays"] = int(len(down))
    m["pct_in_cash"] = round(float((r == 0).mean()), 2)
    return {"method": method, **m}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--m15", default="/tmp/omega_15m")
    ap.add_argument("--mlruns", default="/tmp/omega_factors/mlruns")
    ap.add_argument("--topk", type=int, default=5)
    ap.add_argument("--cost-bps", type=float, default=5.0)
    a = ap.parse_args()
    sess = ib._sessions(a.m15)
    sig = ib._signal(a.mlruns)
    mkt = _market(sess)
    nd = int((mkt["idx"] < mkt["sma50"]).sum())
    print(f"\nSample: {len(mkt)} days, {nd} below-50dSMA (bear-ish), worst day {mkt['ret'].min()*100:.1f}% — mostly bull, read down-day col for bear")
    methods = ["long_flat", "long_hivol_gate", "long_bear_gate", "longshort_flat", "longshort_hivol_gate"]
    rows = [run(m, sess, sig, mkt, a.topk, a.cost_bps) for m in methods]
    df = pd.DataFrame([r for r in rows if r.get("ann_return") is not None]).set_index("method")
    print(f"\nFlat-by-close · top-{a.topk} · {a.cost_bps}bps · ZERO overnight risk\n")
    print(df.to_string())
    print("\ndownday_mean_bps = avg daily return on days the market FELL (the bear read; >0 = profits when market drops)")


if __name__ == "__main__":
    main()

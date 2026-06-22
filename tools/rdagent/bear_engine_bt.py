#!/usr/bin/env python3
"""
BEAR ENGINE — arms only in a bear/crash, idle in a calm bull. Flat-by-close.

Arming rule (known at prior close):
  bear  = index < 50d SMA
  crash = index 5-session return < -crash_thr (fires even inside a bull)
  armed = bear OR crash   (else: idle, 0% — sits in cash)

When armed it SHORTS the weakest-ranked names (lowest risk-adj signal — the ones
that fall hardest), intraday, flat by close. Also tests an index-short variant and
an always-short baseline so we can see whether shorting only-in-bear actually helps
or just bleeds. Sample is bull-heavy, so read the down-day column + %armed, not the
headline. Memory prior: outright index shorts are usually dead; relative-value less so.

    python bear_engine_bt.py --topk 5 --cost-bps 5 --crash-thr 0.05
"""
from __future__ import annotations

import argparse

import numpy as np
import pandas as pd

import bear_regime_bt as br
import intraday_bt as ib


def run(method, sess, sig, mkt, topk, cost_bps, crash_thr):
    cost = cost_bps / 1e4
    ret5 = mkt["idx"].pct_change(5)
    syms = [s for s in sig.columns if s in sess]
    dates = sorted(set(sig.index) & set.union(*[set(sess[s].index) for s in syms]) & set(mkt.index))
    rets, dmask, armed_mask = {}, {}, {}
    prev = None
    for d in dates:
        if prev is None or prev not in sig.index:
            prev = d
            continue
        sma = mkt.loc[prev, "sma50"]
        bear = bool(mkt.loc[prev, "idx"] < sma) if sma == sma else False
        crash = bool(ret5.loc[prev] < -crash_thr) if ret5.loc[prev] == ret5.loc[prev] else False
        strict_bear = bear and (bool(mkt.loc[prev, "ret"] < 0) if mkt.loc[prev, "ret"] == mkt.loc[prev, "ret"] else False)
        if method.startswith("crash_only"):
            armed = crash
        elif method.startswith("strict"):
            armed = strict_bear or crash
        else:
            armed = bear or crash
        row = sig.loc[prev].dropna()
        radj = {s: row[s] / sess[s].loc[prev, "vol20"] for s in row.index
                if s in sess and d in sess[s].index and prev in sess[s].index
                and sess[s].loc[prev, "vol20"] == sess[s].loc[prev, "vol20"] and sess[s].loc[prev, "vol20"] > 0}
        ranked = [s for s, _ in sorted(radj.items(), key=lambda x: x[1], reverse=True)]
        prev = d
        if len(ranked) < topk:
            continue
        dmask[d] = mkt.loc[d, "intraday"] < 0
        armed_mask[d] = armed
        weak = ranked[-topk:]                         # lowest score = short candidates
        if method in ("bear_idxshort", "crash_only_idxshort", "strict_idxshort"):
            r = (0.0 if not armed else -float(mkt.loc[d, "intraday"]) - cost)
        elif method == "bear_xshort":
            r = (0.0 if not armed else -float(np.mean([sess[s].loc[d, "intraday"] for s in weak])) - cost)
        elif method == "always_xshort":
            r = -float(np.mean([sess[s].loc[d, "intraday"] for s in weak])) - cost
        else:
            r = 0.0
        rets[d] = r
    r = pd.Series(rets)
    m = ib._metrics(r)
    dm = pd.Series(dmask).reindex(r.index).fillna(False)
    am = pd.Series(armed_mask).reindex(r.index).fillna(False)
    m["downday_mean_bps"] = round(float(r[dm.values].mean() * 1e4), 1) if dm.any() else None
    m["armed_mean_bps"] = round(float(r[am.values].mean() * 1e4), 1) if am.any() else None
    m["pct_armed"] = round(float(am.mean()), 2)
    return {"method": method, **m}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--m15", default="/tmp/omega_15m")
    ap.add_argument("--mlruns", default="/tmp/omega_factors/mlruns")
    ap.add_argument("--topk", type=int, default=5)
    ap.add_argument("--cost-bps", type=float, default=5.0)
    ap.add_argument("--crash-thr", type=float, default=0.05)
    a = ap.parse_args()
    sess = ib._sessions(a.m15)
    sig = ib._signal(a.mlruns)
    mkt = br._market(sess)
    print(f"\nBear engine · top-{a.topk} · {a.cost_bps}bps · crash<-{a.crash_thr:.0%}/5d · flat-by-close, no overnight")
    print(f"sample worst day {mkt['ret'].min()*100:.1f}%, worst 5d {mkt['idx'].pct_change(5).min()*100:.1f}%\n")
    rows = [run(m, sess, sig, mkt, a.topk, a.cost_bps, a.crash_thr)
            for m in ["bear_idxshort", "strict_idxshort", "crash_only_idxshort", "always_xshort"]]
    df = pd.DataFrame([r for r in rows if r.get("ann_return") is not None]).set_index("method")
    print(df.to_string())
    print("\narmed_mean_bps = avg return on days the engine was ARMED (the real test: >0 = it profits when it fires)")
    print("downday_mean_bps = avg return on market-down days · pct_armed = fraction of days it traded")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
Overnight-risk test: how much of the bigcap signal's return is OVERNIGHT (the part
you're exposed to in a crash) vs INTRADAY (the part you can capture flat-by-close)?

Decomposes each day's close-to-close move into:
  overnight = open / prev_close - 1   (gap risk — held while you sleep)
  intraday  = close / open - 1        (captured if you enter at open, flat at close)

Then backtests three ways to trade the top-K signal:
  overnight_hold   close-to-close (current) — full overnight exposure
  flat_by_close    enter open, exit close, FLAT overnight — no gap risk
  flat_intraday_TP enter open, bank +TP% intraday if hit, else exit at close, flat overnight

Reports ann return / Sharpe / maxDD per method + the overnight-vs-intraday split.

    python intraday_bt.py --m15 /tmp/omega_15m --mlruns /tmp/omega_factors/mlruns \
        --topk 5 --cost-bps 10 --tp 0.03
"""
from __future__ import annotations

import argparse
import glob
import os
import re
from pathlib import Path

import numpy as np
import pandas as pd


def _sessions(m15_dir: str) -> dict[str, pd.DataFrame]:
    """Per symbol: daily session frame (s_open, s_close, s_high, s_low, prev_close)."""
    out = {}
    for f in sorted(glob.glob(os.path.join(m15_dir, "*.csv"))):
        sym = re.sub(r"_(15m|5m|30m)$", "", Path(f).stem, flags=re.I)
        df = pd.read_csv(f).rename(columns=str.lower)
        df["dt"] = pd.to_datetime(df["ts"], unit="s")
        df["day"] = df["dt"].dt.date
        g = df.groupby("day")
        sess = pd.DataFrame({
            "s_open": g["open"].first(),
            "s_close": g["close"].last(),
            "s_high": g["high"].max(),
            "s_low": g["low"].min(),
        })
        sess.index = pd.to_datetime(sess.index)
        sess["prev_close"] = sess["s_close"].shift(1)
        sess["overnight"] = sess["s_open"] / sess["prev_close"] - 1
        sess["intraday"] = sess["s_close"] / sess["s_open"] - 1
        sess["c2c"] = sess["s_close"] / sess["prev_close"] - 1
        sess["vol20"] = sess["c2c"].rolling(20).std()
        out[sym] = sess.dropna(subset=["prev_close"])
    return out


def _signal(mlruns: str) -> pd.DataFrame:
    p = max(glob.glob(os.path.join(mlruns, "**", "pred.pkl"), recursive=True), key=os.path.getmtime)
    s = pd.read_pickle(p)
    return s[s.columns[0]].unstack("instrument")


def _metrics(rets: pd.Series) -> dict:
    rets = rets.dropna()
    if len(rets) < 5 or rets.std() == 0:
        return {}
    eq = (1 + rets).cumprod()
    dd = float((eq / eq.cummax() - 1).min())
    return {"ann_return": round(float(rets.mean() * 252), 3),
            "sharpe": round(float(rets.mean() / rets.std() * np.sqrt(252)), 2),
            "max_dd": round(dd, 3),
            "win_rate": round(float((rets > 0).mean()), 2)}


def run(method: str, sess, sig, topk: int, cost_bps: float, tp: float) -> dict:
    cost = cost_bps / 1e4
    syms = [s for s in sig.columns if s in sess]
    dates = sorted(set(sig.index) & set.union(*[set(sess[s].index) for s in syms]))
    prev_picks: set = set()
    rets = {}
    prev_date = None
    for d in dates:
        if prev_date is None:
            prev_date = d
            continue
        row = sig.loc[prev_date].dropna() if prev_date in sig.index else None
        if row is None:
            prev_date = d
            continue
        radj = {}
        for s in row.index:
            if s in sess and d in sess[s].index and prev_date in sess[s].index:
                v = sess[s].loc[prev_date, "vol20"]
                if v and v == v and v > 0:
                    radj[s] = row[s] / v
        picks = [s for s, _ in sorted(radj.items(), key=lambda x: x[1], reverse=True)][:topk]
        if not picks:
            prev_date = d
            continue
        per = []
        for s in picks:
            r = sess[s].loc[d]
            if method == "overnight_hold":
                per.append(r["c2c"])
            elif method == "flat_by_close":
                per.append(r["intraday"])
            elif method == "flat_intraday_TP":
                hit = r["s_high"] >= r["s_open"] * (1 + tp)
                per.append(tp if hit else r["intraday"])
        gross = float(np.mean(per))
        # cost: flat strategies trade fully every day (enter+exit); overnight only on turnover
        if method == "overnight_hold":
            turn = len(set(picks) ^ prev_picks) / max(len(picks), 1)
            c = turn * cost
        else:
            c = cost  # full round-trip daily
        rets[d] = gross - c
        prev_picks = set(picks)
        prev_date = d
    return {"method": method, **_metrics(pd.Series(rets))}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--m15", default="/tmp/omega_15m")
    ap.add_argument("--mlruns", default="/tmp/omega_factors/mlruns")
    ap.add_argument("--topk", type=int, default=5)
    ap.add_argument("--cost-bps", type=float, default=10.0)
    ap.add_argument("--tp", type=float, default=0.03)
    a = ap.parse_args()
    sess = _sessions(a.m15)
    sig = _signal(a.mlruns)

    # ---- decomposition: where does the top-K return live? ----
    syms = [s for s in sig.columns if s in sess]
    dates = sorted(set(sig.index) & set.union(*[set(sess[s].index) for s in syms]))
    on_tot, id_tot = [], []
    prev = None
    for d in dates:
        if prev is None or prev not in sig.index:
            prev = d
            continue
        row = sig.loc[prev].dropna()
        radj = {s: row[s] / sess[s].loc[prev, "vol20"] for s in row.index
                if s in sess and d in sess[s].index and prev in sess[s].index
                and sess[s].loc[prev, "vol20"] == sess[s].loc[prev, "vol20"] and sess[s].loc[prev, "vol20"] > 0}
        picks = [s for s, _ in sorted(radj.items(), key=lambda x: x[1], reverse=True)][:a.topk]
        if picks:
            on_tot.append(float(np.mean([sess[s].loc[d, "overnight"] for s in picks])))
            id_tot.append(float(np.mean([sess[s].loc[d, "intraday"] for s in picks])))
        prev = d
    on, idy = np.array(on_tot), np.array(id_tot)
    print(f"\nReturn decomposition · top-{a.topk} basket · {len(on)} days")
    print(f"  overnight (gap-risk, held asleep) : {on.sum()*100:+.0f}% total, Sharpe {on.mean()/on.std()*np.sqrt(252):.2f}")
    print(f"  intraday  (capturable flat-by-close): {idy.sum()*100:+.0f}% total, Sharpe {idy.mean()/idy.std()*np.sqrt(252):.2f}")
    print(f"  worst single overnight gap on a held name's basket: {on.min()*100:.1f}%")

    rows = [run(m, sess, sig, a.topk, a.cost_bps, a.tp)
            for m in ["overnight_hold", "flat_by_close", "flat_intraday_TP"]]
    df = pd.DataFrame([r for r in rows if r.get("ann_return") is not None]).set_index("method")
    print(f"\nStrategy · top-{a.topk} · {a.cost_bps}bps · intraday TP +{a.tp:.0%}\n")
    print(df.to_string())
    print("\nflat_by_close / flat_intraday_TP hold NOTHING overnight — overnight gap risk = 0")


if __name__ == "__main__":
    main()

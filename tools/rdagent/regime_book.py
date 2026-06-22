#!/usr/bin/env python3
"""
REGIME BOOK — the deployed daily engine. Reads the regime, runs ONLY the right
engine, emits flat-by-close orders, and appends an immutable shadow ledger.

  regime (prior close, no lookahead): BULL if index >= 50d SMA and no 5d-crash, else BEAR.
  BULL day -> BULL engine: LONG top-K by signal, enter open / exit close, flat overnight.
  BEAR day -> BEAR engine: SHORT the index (SPY), enter open / exit close, flat overnight.

Flat-by-close means each day is independent (nothing held overnight) so the ledger
is naturally append-only: one immutable row per new trading day.

    python regime_book.py --m15 /tmp/omega_15m --mlruns /tmp/omega_factors/mlruns \
        --topk 5 --capital 100000 [--mode shadow]
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
from pathlib import Path

import numpy as np
import pandas as pd

import bear_regime_bt as br
import intraday_bt as ib

DATA = Path.home() / "Omega" / "data" / "rdagent"
LEDGER = DATA / "regime_ledger.csv"
BOOK = DATA / "regime_book.json"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--m15", default="/tmp/omega_15m")
    ap.add_argument("--mlruns", default="/tmp/omega_factors/mlruns")
    ap.add_argument("--topk", type=int, default=5)
    ap.add_argument("--cost-bps", type=float, default=5.0)
    ap.add_argument("--capital", type=float, default=100000.0)
    ap.add_argument("--mode", choices=["shadow", "live"], default="shadow")
    a = ap.parse_args()
    sess = ib._sessions(a.m15)
    sig = ib._signal(a.mlruns)
    mkt = br._market(sess)
    cost = a.cost_bps / 1e4
    ret5 = mkt["idx"].pct_change(5)
    syms = [s for s in sig.columns if s in sess]
    dates = sorted(set(sig.index) & set.union(*[set(sess[s].index) for s in syms]) & set(mkt.index))

    last = None
    if LEDGER.exists():
        seen = [r["date"] for r in csv.DictReader(LEDGER.open())]
        last = pd.Timestamp(seen[-1]) if seen else None
    else:
        LEDGER.write_text("date,regime,engine,ret,equity\n")

    eq = 1.0
    if last is not None:
        rows0 = list(csv.DictReader(LEDGER.open()))
        if rows0:
            eq = float(rows0[-1]["equity"])

    new_rows = []
    today = {"regime": None, "engine": None, "basket": [], "orders": []}
    prev = None
    for d in dates:
        if prev is None or prev not in sig.index:
            prev = d
            continue
        sma = mkt.loc[prev, "sma50"]
        if sma != sma:
            prev = d
            continue
        crash = bool(ret5.loc[prev] < -0.04) if ret5.loc[prev] == ret5.loc[prev] else False
        is_bull = bool(mkt.loc[prev, "idx"] >= sma) and not crash
        reg = "BULL" if is_bull else "BEAR"
        row = sig.loc[prev].dropna()
        radj = {s: row[s] / sess[s].loc[prev, "vol20"] for s in row.index
                if s in sess and d in sess[s].index and prev in sess[s].index
                and sess[s].loc[prev, "vol20"] == sess[s].loc[prev, "vol20"] and sess[s].loc[prev, "vol20"] > 0}
        ranked = [s for s, _ in sorted(radj.items(), key=lambda x: x[1], reverse=True)]
        prev = d
        if len(ranked) < a.topk:
            continue
        if reg == "BULL":
            r = float(np.mean([sess[s].loc[d, "intraday"] for s in ranked[:a.topk]])) - cost
            eng, basket = "BULL-long-topK", ranked[:a.topk]
        else:
            r = -float(mkt.loc[d, "intraday"]) - cost
            eng, basket = "BEAR-short-index", ["SPY"]
        if last is None or d > last:
            eq *= (1 + r)
            new_rows.append((str(d.date()), reg, eng, round(r, 6), round(eq, 5)))
        today = {"regime": reg, "engine": eng, "basket": basket}

    with LEDGER.open("a", newline="") as f:
        w = csv.writer(f)
        for r in new_rows:
            w.writerow(r)

    # today's flat-by-close orders for the active engine
    cap = a.capital
    orders = []
    if today["regime"] == "BULL":
        names = today["basket"]
        per = cap / max(len(names), 1)
        for s in names:
            p = float(sess[s]["s_close"].iloc[-1])
            orders.append({"symbol": s, "side": "BUY", "shares": round(per / p), "price": round(p, 2),
                           "exit": "SELL at close"})
    elif today["regime"] == "BEAR":
        orders.append({"symbol": "SPY", "side": "SHORT", "shares": "~index notional",
                       "note": "short the index ETF, exit at close", "exit": "COVER at close",
                       "sized_to": cap})
    today["orders"] = orders
    BOOK.write_text(json.dumps({"generated_at": dt.datetime.now().astimezone().isoformat(timespec="seconds"), **today}, indent=2))

    # summary
    led = pd.read_csv(LEDGER)
    rr = led["ret"]
    e = led["equity"]
    sh = float(rr.mean() / rr.std() * np.sqrt(252)) if rr.std() else 0.0
    dd = float((e / e.cummax() - 1).min())
    print(f"REGIME BOOK ledger · {len(led)} days · {(led['regime']=='BULL').sum()} bull / {(led['regime']=='BEAR').sum()} bear")
    print(f"  equity x{e.iloc[-1]:.3f} | Sharpe {sh:.2f} | maxDD {dd*100:.1f}% | appended {len(new_rows)} new row(s)")
    print(f"  TODAY: {today['regime']} -> {today['engine']}")
    for o in orders[:6]:
        print(f"    {o.get('side'):>6} {o['symbol']:<6} {o.get('shares')} @ {o.get('price','')}  ({o['exit']})")
    print(f"  book -> {BOOK} · ledger -> {LEDGER}")


if __name__ == "__main__":
    main()

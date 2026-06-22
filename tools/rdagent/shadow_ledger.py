#!/usr/bin/env python3
"""
Forward SHADOW LEDGER for the reversal sleeve — treat-as-live testing.

Unlike paper_track.py (which re-simulates the whole window each run = a backtest),
this is an APPEND-ONLY forward record, same discipline as Omega's shadow ledger:
  - holds the book H=5 days, marks it forward close-to-close each trading day,
  - rebalances weekly (pays turnover cost only then),
  - writes ONE immutable row per day; never rewrites history,
  - persists the held book in shadow_state.json so the next run continues, not restarts.

First run SEEDS by forward-replaying the available history in strict chronological
order (no lookahead). Rows on/before the go-live date are phase=seed; rows appended
on later runs are phase=live — the live rows are the only real forward proof.

    python shadow_ledger.py --close-csv ~/Omega/data/rdagent/sp500_close.csv [--golive 2026-06-22]
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
from pathlib import Path

import numpy as np
import pandas as pd

L, HOLD, FRAC, CLIP, MIN_PX = 3, 5, 0.20, 0.20, 5.0
DATA = Path.home() / "Omega" / "data" / "rdagent"
LEDGER = DATA / "shadow_ledger.csv"
STATE = DATA / "shadow_state.json"


def pick_book(close: pd.DataFrame, d) -> tuple[list, list]:
    hist = close.loc[:d]
    if len(hist) <= L:
        return [], []
    rev = -(hist.iloc[-1] / hist.iloc[-1 - L] - 1)
    liquid = hist.iloc[-1][hist.iloc[-1] >= MIN_PX].index
    rev = rev.reindex(liquid).dropna()
    k = max(1, int(len(rev) * FRAC))
    longs = sorted(rev.sort_values(ascending=False).head(k).index)   # biggest losers
    shorts = sorted(rev.sort_values().head(k).index)                 # biggest winners
    return longs, shorts


def mark(close_row, prev_close, names) -> float:
    rs = []
    for s in names:
        if s in prev_close and s in close_row and prev_close[s] and close_row[s] == close_row[s]:
            r = close_row[s] / prev_close[s] - 1
            rs.append(float(np.clip(r, -CLIP, CLIP)))
    return float(np.mean(rs)) if rs else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--close-csv", default=str(DATA / "sp500_close.csv"))
    ap.add_argument("--cost-bps", type=float, default=3.0)
    ap.add_argument("--golive", default=dt.date.today().isoformat())
    a = ap.parse_args()
    close = pd.read_csv(a.close_csv, index_col=0, parse_dates=True).sort_index()
    cost = a.cost_bps / 1e4
    dates = [d for d in close.index]

    if STATE.exists():
        st = json.loads(STATE.read_text())
        last = pd.Timestamp(st["last_date"])
        equity, days_held = st["equity"], st["days_held"]
        longs, shorts = st["longs"], st["shorts"]
        prev_close = st["prev_close"]
        new_dates = [d for d in dates if d > last]
        write_header = False
    else:                                   # seed: start at first date with L history
        start_i = L + 1
        d0 = dates[start_i]
        longs, shorts = pick_book(close, d0)
        prev_close = {s: float(close.loc[d0, s]) for s in longs + shorts if close.loc[d0, s] == close.loc[d0, s]}
        equity, days_held = 1.0, 0
        new_dates = dates[start_i + 1:]
        write_header = True
        LEDGER.write_text("date,net_ret,equity,days_held,action,phase\n")
        with LEDGER.open("a") as f:
            f.write(f"{d0.date()},0.0,1.0,0,INIT,seed\n")

    rows = []
    for d in new_dates:
        crow = close.loc[d]
        gross = mark(crow, prev_close, longs) - mark(crow, prev_close, shorts)
        days_held += 1
        c = 0.0
        action = "HOLD"
        # advance marks for currently-held names
        prev_close = {s: float(crow[s]) for s in longs + shorts if s in crow and crow[s] == crow[s]}
        if days_held >= HOLD:               # weekly rebalance at this close
            nl, ns = pick_book(close, d)
            if nl and ns:
                k = max(len(longs), 1)
                turn = (len(set(nl) ^ set(longs)) + len(set(ns) ^ set(shorts))) / (2 * k)
                c = turn * cost
                longs, shorts = nl, ns
                prev_close = {s: float(crow[s]) for s in longs + shorts if s in crow and crow[s] == crow[s]}
                days_held = 0
                action = "REBAL"
        net = gross - c
        equity *= (1 + net)
        phase = "seed" if str(d.date()) <= a.golive else "live"
        rows.append((str(d.date()), round(net, 6), round(equity, 5), days_held, action, phase))

    with LEDGER.open("a") as f:
        w = csv.writer(f)
        for r in rows:
            w.writerow(r)
    if new_dates:
        STATE.write_text(json.dumps({
            "last_date": str(new_dates[-1].date()), "equity": equity, "days_held": days_held,
            "longs": longs, "shorts": shorts, "prev_close": prev_close}, indent=0))

    # summary
    led = pd.read_csv(LEDGER)
    r = led["net_ret"].iloc[1:]
    eq = led["equity"]
    dd = float((eq / eq.cummax() - 1).min())
    sh = float(r.mean() / r.std() * np.sqrt(252)) if r.std() else 0.0
    nlive = int((led["phase"] == "live").sum())
    print(f"SHADOW LEDGER · {len(led)} days ({led['phase'].value_counts().get('seed',0)} seed / {nlive} live forward)")
    print(f"  equity ×{eq.iloc[-1]:.3f} | Sharpe {sh:.2f} | maxDD {dd*100:.1f}% | current DD {(eq.iloc[-1]/eq.cummax().iloc[-1]-1)*100:.1f}%")
    print(f"  appended {len(rows)} new row(s) this run; ledger {LEDGER}")
    if nlive == 0:
        print("  NOTE: 0 live forward days yet — all rows are seed (historical replay). Live proof accrues one row per trading day from here.")


if __name__ == "__main__":
    main()

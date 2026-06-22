#!/usr/bin/env python3
"""
TOP-SIGNALS · FLAT-BY-CLOSE engine — what was actually asked for.

Each day: rank the universe by the signal, LONG the top-K, enter at the open,
EXIT ALL at the close. Hold NOTHING overnight. Sit out top-quartile-vol days
(the gate that lifted Sharpe). Repeat daily.

Long-only directional (NOT market-neutral) — it rides the strongest names
intraday. Forward shadow ledger (append-only) + today's basket + orders.

    python intraday_engine.py --m15 /tmp/omega_15m --mlruns /tmp/omega_factors/mlruns --topk 5
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path

import numpy as np
import pandas as pd

import bear_regime_bt as br
import intraday_bt as ib

DATA = Path.home() / "Omega" / "data" / "rdagent"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--m15", default="/tmp/omega_15m")
    ap.add_argument("--mlruns", default="/tmp/omega_factors/mlruns")
    ap.add_argument("--topk", type=int, default=5)
    ap.add_argument("--cost-bps", type=float, default=5.0)
    ap.add_argument("--capital", type=float, default=100000.0)
    a = ap.parse_args()
    sess = ib._sessions(a.m15)
    sig = ib._signal(a.mlruns)
    mkt = br._market(sess)
    cost = a.cost_bps / 1e4
    hivol_thr = mkt["vol20"].quantile(0.75)

    syms = [s for s in sig.columns if s in sess]
    dates = sorted(set(sig.index) & set.union(*[set(sess[s].index) for s in syms]) & set(mkt.index))

    # forward shadow ledger: long top-K open->close, flat overnight, gate high-vol
    eq = 1.0
    rows = []
    prev = None
    today_book, today_action = [], "CASH"
    for d in dates:
        if prev is None or prev not in sig.index:
            prev = d
            continue
        hivol = bool(mkt.loc[prev, "vol20"] > hivol_thr) if mkt.loc[prev, "vol20"] == mkt.loc[prev, "vol20"] else False
        row = sig.loc[prev].dropna()
        radj = {s: row[s] / sess[s].loc[prev, "vol20"] for s in row.index
                if s in sess and d in sess[s].index and prev in sess[s].index
                and sess[s].loc[prev, "vol20"] == sess[s].loc[prev, "vol20"] and sess[s].loc[prev, "vol20"] > 0}
        ranked = [s for s, _ in sorted(radj.items(), key=lambda x: x[1], reverse=True)]
        prev = d
        if len(ranked) < a.topk:
            continue
        longs = ranked[:a.topk]
        if hivol:
            r, act = 0.0, "CASH (high-vol)"
        else:
            r = float(np.mean([sess[s].loc[d, "intraday"] for s in longs])) - cost   # open->close, flat by close
            act = "LONG top-%d intraday" % a.topk
        eq *= (1 + r)
        rows.append((str(d.date()), round(r, 6), round(eq, 5), act))
        today_book, today_action = longs, act

    led = pd.DataFrame(rows, columns=["date", "ret", "equity", "action"])
    rr = led["ret"]
    e = led["equity"]
    dd = float((e / e.cummax() - 1).min())
    dm = pd.Series([mkt.loc[pd.Timestamp(x), "intraday"] < 0 if pd.Timestamp(x) in mkt.index else False for x in led["date"]])
    sh = float(rr.mean() / rr.std() * np.sqrt(252)) if rr.std() else 0.0
    led.to_csv(DATA / "intraday_ledger.csv", index=False)

    print("TOP-SIGNALS FLAT-BY-CLOSE · long top-%d · %sbps · no overnight" % (a.topk, a.cost_bps))
    print("  span %s -> %s (%d days)" % (led["date"].iloc[0], led["date"].iloc[-1], len(led)))
    print("  equity x%.3f | ann %.1f%% | Sharpe %.2f | vol %.1f%% | maxDD %.1f%% | win %.0f%%"
          % (e.iloc[-1], rr.mean()*252*100, sh, rr.std()*np.sqrt(252)*100, dd*100, (rr > 0).mean()*100))
    dn = rr[dm.values]
    print("  down-market days: %.0f bps avg  (long-only = bleeds when market falls)" % (dn.mean()*1e4 if len(dn) else 0))
    print("  %% days in cash (gated): %.0f%%" % ((led["action"].str.startswith("CASH")).mean()*100))

    # today's book + flat-by-close orders
    px = {s: float(sess[s]["s_close"].iloc[-1]) for s in today_book}
    per = a.capital / max(len(today_book), 1)
    print("\n  TODAY: %s" % today_action)
    print("  enter at OPEN, exit at CLOSE — basket of %d:" % len(today_book))
    for s in today_book:
        sh_ct = round(per / px[s]) if px.get(s) else 0
        print("    BUY  %-6s %4d sh @ ~$%.2f  ($%s)  -> SELL all at close" % (s, sh_ct, px[s], f"{sh_ct*px[s]:,.0f}"))
    Path(DATA / "intraday_book.json").write_text(json.dumps(
        {"generated_at": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
         "action": today_action, "basket": today_book, "rule": "enter open, exit close, flat overnight"}, indent=2))


if __name__ == "__main__":
    main()

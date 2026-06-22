#!/usr/bin/env python3
"""
Execution engine for the reversal sleeve — LIVE-IN-SHADOW (treat-as-live).

Turns today's book (reversal_sleeve.json) into actual sized orders: dollar-neutral,
book split 50/50 long/short, equal-weight, shares = (leg_notional / N) / price.
Diffs against current positions and emits the precise BUY / SELL / SHORT / COVER
orders with share counts + notional.

  --mode shadow (default): logs the orders it WOULD place to shadow_orders.csv,
        marks them against last close. NO broker connection. This is the live test.
  --mode live: connects to IBKR via ib_insync and places them. Requires --i-confirm,
        a running IB gateway, and is gated OFF by default. Nothing trades without it.

    python execute.py --book ~/Omega/data/rdagent/reversal_sleeve.json \
        --close-csv ~/Omega/data/rdagent/sp500_close.csv --capital 100000 --mode shadow
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
from pathlib import Path

import pandas as pd

DATA = Path.home() / "Omega" / "data" / "rdagent"
POS = DATA / "shadow_positions.json"        # current shadow holdings {sym: shares (+long/-short)}
ORDERS = DATA / "shadow_orders.csv"


def target_shares(names, leg_notional, prices, sign):
    n = len(names)
    if n == 0:
        return {}
    per = leg_notional / n
    out = {}
    for s in names:
        p = prices.get(s)
        if p and p > 0:
            out[s] = sign * round(per / p)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--book", default=str(DATA / "reversal_sleeve.json"))
    ap.add_argument("--close-csv", default=str(DATA / "sp500_close.csv"))
    ap.add_argument("--capital", type=float, default=100000.0, help="gross book $ (split 50/50 L/S)")
    ap.add_argument("--mode", choices=["shadow", "live"], default="shadow")
    ap.add_argument("--i-confirm", action="store_true", help="required for --mode live")
    a = ap.parse_args()

    book = json.loads(Path(a.book).read_text())
    close = pd.read_csv(a.close_csv, index_col=0, parse_dates=True).sort_index()
    last_close = close.iloc[-1]
    prices = {s: float(last_close[s]) for s in last_close.index if last_close[s] == last_close[s]}

    leg = a.capital / 2.0
    tgt = {**target_shares(book["longs"], leg, prices, +1),
           **target_shares(book["shorts"], leg, prices, -1)}
    cur = json.loads(POS.read_text()) if POS.exists() else {}

    orders = []
    for s in sorted(set(tgt) | set(cur)):
        t, c = tgt.get(s, 0), cur.get(s, 0)
        d = t - c
        if d == 0:
            continue
        side = ("BUY" if d > 0 else "SELL") if c >= 0 and t >= 0 else \
               ("SELL/SHORT" if d < 0 else "COVER/BUY")
        # clearer side labels for crossing zero
        if c == 0:
            side = "BUY" if d > 0 else "SHORT"
        elif t == 0:
            side = "SELL" if c > 0 else "COVER"
        elif (c > 0) == (d > 0):
            side = "ADD-LONG" if d > 0 else ("REDUCE-LONG" if t > 0 else "FLIP-SHORT")
        else:
            side = "REDUCE/REVERSE"
        orders.append({"symbol": s, "side": side, "shares": int(d),
                       "price": round(prices.get(s, 0.0), 2),
                       "notional": round(abs(d) * prices.get(s, 0.0))})

    ts = dt.datetime.now().astimezone().isoformat(timespec="seconds")
    gross = sum(abs(o["shares"]) * o["price"] for o in orders)
    long_ct = sum(1 for v in tgt.values() if v > 0)
    short_ct = sum(1 for v in tgt.values() if v < 0)
    print(f"[{a.mode.upper()}] {ts} · capital ${a.capital:,.0f} · target {long_ct}L/{short_ct}S · {len(orders)} orders · ${gross:,.0f} traded")
    for o in orders[:12]:
        print(f"  {o['side']:>14} {o['symbol']:<6} {o['shares']:+5d} @ ${o['price']:.2f}  (${o['notional']:,})")
    if len(orders) > 12:
        print(f"  … +{len(orders)-12} more")

    if a.mode == "shadow":
        new = ORDERS.exists()
        with ORDERS.open("a", newline="") as f:
            w = csv.writer(f)
            if not new:
                w.writerow(["ts", "as_of", "symbol", "side", "shares", "price", "notional"])
            for o in orders:
                w.writerow([ts, book.get("as_of"), o["symbol"], o["side"], o["shares"], o["price"], o["notional"]])
        POS.write_text(json.dumps(tgt, indent=0))   # shadow fills assumed at last close
        print(f"  -> shadow fills booked; positions {len(tgt)} names; orders -> {ORDERS}")
        print("  (LIVE-IN-SHADOW: these are the exact orders that would hit IBKR. Flip --mode live to send.)")
        return

    # live path — heavily gated
    if not a.i_confirm:
        raise SystemExit("REFUSED: --mode live requires --i-confirm AND a running IB gateway. Aborting (nothing sent).")
    try:
        from ib_insync import IB, Stock, MarketOrder
    except ImportError:
        raise SystemExit("ib_insync not installed. `pip install ib_insync` + start IB Gateway, then retry.")
    ib = IB()
    ib.connect("127.0.0.1", 4001, clientId=17)      # IBKR gateway; operator must have it up
    for o in orders:
        contract = Stock(o["symbol"], "SMART", "USD")
        order = MarketOrder("BUY" if o["shares"] > 0 else "SELL", abs(o["shares"]))
        ib.placeOrder(contract, order)
        print(f"  SENT {o['side']} {o['symbol']} {o['shares']:+d}")
    ib.disconnect()
    print("  LIVE orders submitted to IBKR.")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
Paper executor for the RD-Agent DISCOVERY buy basket (the top-K names shown on
the :7799 panel). Operator mandate 2026-06-22: "treat paper as real -- trade the
basket I'm looking at." Long-only, equal-weight, daily rebalance.

Reads ~/Omega/data/rdagent/latest.json (signal.basket, the model's BUY names),
equal-weights the top-K long-only, diffs against the current paper book, and emits
the BUY/SELL orders + an updated book + a ledger + a one-line result the GUI shows.

  --mode shadow (DEFAULT): logs the orders it WOULD place, marks them against the
        latest close. NO broker connection. This IS the paper test -- treat as real.
  --mode live: connects to IBKR via ib_insync and places them. Requires --i-confirm,
        a running IB gateway, and is gated OFF by default. Nothing trades without it.
        (Real money also requires the factors to clear Omega faithful tick BT first --
        these are bar-replay grade; --mode live is a hard manual step, never scheduled.)

  python execute_basket.py --topk 5 --capital 100000 --mode shadow
"""
from __future__ import annotations
import argparse, csv, json, sys
import datetime as dt
from pathlib import Path

DATA = Path.home() / "Omega" / "data" / "rdagent"
LATEST = DATA / "latest.json"
CLOSE_CANDIDATES = [DATA / "sp500_long_close.csv", DATA / "sp500_close.csv"]
POS = DATA / "factor_basket_positions.json"   # {sym: shares} long-only
ORDERS = DATA / "factor_basket_orders.csv"
LEDGER = DATA / "factor_basket_ledger.csv"
RESULT = DATA / "factor_basket_result.json"    # one-line summary the GUI reads


def latest_closes(path: Path) -> dict[str, float]:
    """Last non-empty close per instrument from a wide date,SYM,SYM,... csv."""
    closes: dict[str, float] = {}
    with open(path, newline="") as fh:
        r = csv.reader(fh)
        hdr = next(r)
        syms = hdr[1:]
        for row in r:
            for i, sym in enumerate(syms, start=1):
                if i < len(row) and row[i] not in ("", "nan", "NaN"):
                    try: closes[sym] = float(row[i])
                    except ValueError: pass
    return closes


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--topk", type=int, default=5)
    ap.add_argument("--capital", type=float, default=100000.0)
    ap.add_argument("--mode", choices=["shadow", "live"], default="shadow")
    ap.add_argument("--i-confirm", action="store_true", help="required for --mode live")
    a = ap.parse_args()

    if not LATEST.exists():
        print(json.dumps({"error": "no latest.json -- run export_signals.py first"})); return 1
    meta = json.loads(LATEST.read_text())
    basket = meta.get("signal", {}).get("basket", [])
    buys = [b for b in basket if b.get("action") == "BUY" and b.get("instrument")]
    buys = sorted(buys, key=lambda b: b.get("rank", 1e9))[: a.topk]
    if not buys:
        print(json.dumps({"error": "no BUY names in today's basket"})); return 1

    close_path = next((p for p in CLOSE_CANDIDATES if p.exists()), None)
    if not close_path:
        print(json.dumps({"error": "no close csv found"})); return 1
    closes = latest_closes(close_path)

    leg = a.capital / len(buys)                       # equal-weight dollar per name
    target: dict[str, int] = {}
    skipped = []
    for b in buys:
        sym = b["instrument"]; px = closes.get(sym)
        if not px or px <= 0: skipped.append(sym); continue
        target[sym] = int(leg // px)                  # whole shares, long-only

    cur = json.loads(POS.read_text()) if POS.exists() else {}
    syms = sorted(set(cur) | set(target))
    now = dt.datetime.now().astimezone().isoformat(timespec="seconds")
    asof = meta.get("signal", {}).get("date", "")
    orders = []
    for sym in syms:
        delta = target.get(sym, 0) - int(cur.get(sym, 0))
        if delta == 0: continue
        px = closes.get(sym, 0.0)
        side = "BUY" if delta > 0 else "SELL"
        orders.append((now, asof, sym, side, delta, round(px, 2), round(abs(delta) * px, 0)))

    if a.mode == "live":
        if not a.i_confirm:
            print(json.dumps({"error": "--mode live requires --i-confirm + IB gateway (gated off)"})); return 2
        print(json.dumps({"error": "live broker path not wired here -- use the audited Omega live path"})); return 2

    # ---- shadow: persist book + orders + ledger + GUI result ----
    POS.write_text(json.dumps(target, indent=0))
    new_orders = not ORDERS.exists()
    with open(ORDERS, "a", newline="") as fh:
        w = csv.writer(fh)
        if new_orders: w.writerow(["ts", "as_of", "instrument", "side", "shares", "price", "notional"])
        for o in orders: w.writerow(o)
    deployed = sum(t * closes.get(s, 0) for s, t in target.items())
    with open(LEDGER, "a", newline="") as fh:
        w = csv.writer(fh)
        if fh.tell() == 0: w.writerow(["ts", "as_of", "n_names", "deployed_usd", "names"])
        w.writerow([now, asof, len(target), round(deployed, 0), "|".join(target)])
    result = {
        "ts": now, "as_of": asof, "mode": "shadow", "capital": a.capital,
        "n_names": len(target), "deployed_usd": round(deployed, 0),
        "orders": [{"sym": o[2], "side": o[3], "shares": o[4], "price": o[5], "notional": o[6]} for o in orders],
        "book": target, "skipped_no_price": skipped,
    }
    RESULT.write_text(json.dumps(result, indent=2))
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())

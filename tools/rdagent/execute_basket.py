#!/usr/bin/env python3
"""
Paper executor for the RD-Agent DISCOVERY buy basket (the top-K names shown on
the :7799 panel). Operator mandate 2026-06-22: "treat paper as real -- trade the
basket I'm looking at." Long-only, equal-weight, daily rebalance.

Reads ~/Omega/data/rdagent/latest.json (signal.basket, the model's BUY names),
equal-weights the top-K long-only, diffs against the current paper book, and emits
the BUY/SELL orders + an updated book + a ledger + a one-line result the GUI shows.
Legs are sized from CURRENT EQUITY (cash + marked book), not --capital, and every
BUY is floored at available cash (S-2026-07-14: capital-based sizing drove the paper
cash book negative after losses). --capital still seeds the book and anchors P&L.

REAL-COST CAPTURE (S-2026-06-30): the paper book is run "as if live" so the ledger
reflects real trading costs. Every fill is charged:
  - IBKR Pro Fixed equity commission: $0.005/share, min $1.00/order, max 1% notional.
  - Slippage: 5 bps of notional per fill (close-marked fills cross the spread).
A cash account (starts at --capital) is debited/credited per fill net of cost, and
the ledger records cash, mark-to-market equity, this-rebalance cost, and cumulative
cost. equity - capital = total paper P&L NET of real costs. Cost params are tunable
via --commission-per-share / --commission-min / --commission-max-pct / --slippage-bps.

  --mode shadow (DEFAULT): logs the orders it WOULD place, marks them against the
        latest close, charges real costs to the paper cash book. NO broker connection.
        This IS the paper test -- treat as real.
  --mode live: connects to IBKR via ib_insync and places them. Requires --i-confirm,
        a running IB gateway, and is gated OFF by default. Nothing trades without it.
        (Real money also requires the factors to clear Omega faithful tick BT first --
        these are bar-replay grade; --mode live is a hard manual step, never scheduled.)

  python execute_basket.py --topk 5 --capital 10000 --mode shadow
"""
from __future__ import annotations
import argparse, csv, json, os, sys
import datetime as dt
from pathlib import Path

# RDA_DATA_DIR: test-only override so the book can be exercised against a COPY of
# the state (never set in cron/prod -- default is the live path, behavior unchanged).
DATA = Path(os.environ.get("RDA_DATA_DIR") or (Path.home() / "Omega" / "data" / "rdagent"))
LATEST = DATA / "latest.json"
CLOSE_CANDIDATES = [DATA / "sp500_long_close.csv", DATA / "sp500_close.csv"]
POS = DATA / "factor_basket_positions.json"   # {sym: shares} long-only
STATE = DATA / "factor_basket_state.json"     # {cash_usd, cost_cum_usd} paper cash book
ORDERS = DATA / "factor_basket_orders.csv"
LEDGER = DATA / "factor_basket_ledger.csv"
RESULT = DATA / "factor_basket_result.json"    # one-line summary the GUI reads

ORDERS_HEADER = ["ts", "as_of", "instrument", "side", "shares", "price", "notional", "cost"]
LEDGER_HEADER = ["ts", "as_of", "n_names", "deployed_usd", "names",
                 "cash_usd", "equity_usd", "cost_usd", "cost_cum_usd"]

STALE_DAYS = 5  # S-2026-06-25: a close > this many days behind the file's NEWEST date is DROPPED.


def latest_closes(path: Path) -> dict[str, float]:
    """Most-recent close per instrument, FRESHNESS-GATED.

    S-2026-06-25 STALE-PRICE GUARD: previously this took the last non-empty cell per
    column ignoring date, so dead columns (INTC/NFLX/ADBE/DELL/AAPL stopped populating
    mid-2024) returned 2-YEAR-OLD prices -- and main() would FILL the paper basket at
    them. Now a close is only returned if it is within STALE_DAYS of the file's newest
    date; stale names fall out of the dict -> main() skips them (the `skipped` list)
    rather than booking a fill at a ghost price.
    """
    last_date: dict[str, str] = {}
    last_val: dict[str, float] = {}
    newest = ""
    with open(path, newline="") as fh:
        r = csv.reader(fh)
        syms = next(r)[1:]
        for row in r:
            if not row:
                continue
            d = row[0]
            if d > newest:
                newest = d
            for i, sym in enumerate(syms, start=1):
                if i < len(row) and row[i] not in ("", "nan", "NaN"):
                    try: last_val[sym] = float(row[i]); last_date[sym] = d
                    except ValueError: pass
    try: newest_d = dt.date.fromisoformat(newest)
    except ValueError: newest_d = None
    closes: dict[str, float] = {}
    for sym, v in last_val.items():
        fresh = True
        if newest_d is not None:
            try: fresh = (newest_d - dt.date.fromisoformat(last_date[sym])).days <= STALE_DAYS
            except ValueError: fresh = True
        if fresh: closes[sym] = v
    return closes


def fill_cost(shares: int, notional: float, per_share: float, comm_min: float,
              comm_max_pct: float, slippage_bps: float) -> float:
    """Real per-fill cost: IBKR Pro Fixed commission + slippage. shares/notional are
    absolute magnitudes for one order leg."""
    if notional <= 0:
        return 0.0
    comm = per_share * abs(shares)
    comm = max(comm, comm_min)
    comm = min(comm, comm_max_pct * notional)   # IBKR caps fixed commission at 1% of trade value
    slip = notional * slippage_bps / 1e4
    return round(comm + slip, 2)


def migrate_csv(path: Path, new_header: list[str]) -> None:
    """One-time schema upgrade: if an existing file's header predates the cost columns,
    rewrite it with the new header and pad historical rows with trailing blanks (the
    new columns are appended at the end, so old data stays aligned)."""
    if not path.exists():
        return
    rows = list(csv.reader(open(path, newline="")))
    if not rows or rows[0] == new_header:
        return
    body = rows[1:]
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(new_header)
        for r in body:
            w.writerow(r + [""] * (new_header_pad(new_header, r)))


def new_header_pad(new_header: list[str], row: list[str]) -> int:
    return max(0, len(new_header) - len(row))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--topk", type=int, default=5)
    ap.add_argument("--capital", type=float, default=10000.0)
    ap.add_argument("--mode", choices=["shadow", "live"], default="shadow")
    ap.add_argument("--i-confirm", action="store_true", help="required for --mode live")
    ap.add_argument("--flatten", action="store_true",
                    help="PANIC KILL-ALL: liquidate every open position to cash (target book = empty). "
                         "Ignores today's basket. Same shadow/live gating as a normal run.")
    # real-cost knobs (IBKR Pro Fixed equity + slippage); defaults are the operator-chosen model
    ap.add_argument("--commission-per-share", type=float, default=0.005)
    ap.add_argument("--commission-min", type=float, default=1.00)
    ap.add_argument("--commission-max-pct", type=float, default=0.01)
    ap.add_argument("--slippage-bps", type=float, default=5.0)
    a = ap.parse_args()

    # PANIC KILL-ALL: liquidate to an empty book, ignore today's basket. Still
    # needs close prices to value the SELLs; latest.json is NOT required.
    if a.flatten:
        meta = json.loads(LATEST.read_text()) if LATEST.exists() else {}
    else:
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

    cur = json.loads(POS.read_text()) if POS.exists() else {}

    # paper cash book -- init from current state, or seed so equity == capital at first run
    # (loaded BEFORE sizing: legs are sized from current equity, see below)
    if STATE.exists():
        st = json.loads(STATE.read_text())
        cash = float(st.get("cash_usd", a.capital))
        cost_cum = float(st.get("cost_cum_usd", 0.0))
    else:
        cur_mv = sum(int(cur.get(s, 0)) * closes.get(s, 0.0) for s in cur)
        cash = a.capital - cur_mv          # so cash + current market value == capital
        cost_cum = 0.0

    target: dict[str, int] = {}
    skipped = []
    sizing_equity = 0.0
    if not a.flatten:
        # S-2026-07-14 SIZING FIX: legs are sized from CURRENT EQUITY (cash + marked
        # value of the open book), NOT the fixed --capital placeholder. Sizing off
        # initial capital oversized the buy legs after losses and drove paper cash
        # NEGATIVE (-$97 observed 2026-07-14 04:14). Held names without a fresh close
        # mark at 0 here -- conservative, and consistent with how deployed/equity
        # mark them below.
        held_mv = sum(int(cur.get(s, 0)) * closes.get(s, 0.0) for s in cur)
        sizing_equity = max(0.0, cash + held_mv)
        leg = sizing_equity / len(buys)               # equal-weight dollar per name
        for b in buys:
            sym = b["instrument"]; px = closes.get(sym)
            if not px or px <= 0: skipped.append(sym); continue
            target[sym] = int(leg // px)              # whole shares, long-only

    syms = sorted(set(cur) | set(target))
    now = dt.datetime.now().astimezone().isoformat(timespec="seconds")
    asof = meta.get("signal", {}).get("date", "")

    orders = []
    cost_today = 0.0
    capped = []
    # S-2026-07-14 CASH FLOOR: fill SELLs before BUYs so sale proceeds fund the buys,
    # then trim any BUY whose notional+cost exceeds available cash. A sizing decision
    # can never drive the paper cash book negative again.
    deltas = {s: target.get(s, 0) - int(cur.get(s, 0)) for s in syms}
    for sym in sorted(syms, key=lambda s: (deltas[s] > 0, s)):
        delta = deltas[sym]
        if delta == 0: continue
        px = closes.get(sym, 0.0)
        if px <= 0:                       # no fresh price -> never sell/buy at $0; keep the slot
            target[sym] = int(cur.get(sym, 0)); skipped.append(sym); continue
        if delta > 0:                     # cash floor: shrink the buy to what cash affords
            want = delta
            while delta > 0:
                n = delta * px
                c = fill_cost(delta, n, a.commission_per_share, a.commission_min,
                              a.commission_max_pct, a.slippage_bps)
                if n + c <= cash: break
                delta -= 1
            if delta < want:
                capped.append({"sym": sym, "want": want, "filled": delta})
                target[sym] = int(cur.get(sym, 0)) + delta
                if target[sym] <= 0: target.pop(sym, None)
                if delta == 0: continue
        side = "BUY" if delta > 0 else "SELL"
        notional = abs(delta) * px
        cost = fill_cost(delta, notional, a.commission_per_share, a.commission_min,
                         a.commission_max_pct, a.slippage_bps)
        cost_today += cost
        if delta > 0:   # BUY: pay notional + cost
            cash -= (notional + cost)
        else:           # SELL: receive notional - cost
            cash += (notional - cost)
        orders.append((now, asof, sym, side, delta, round(px, 2), round(notional, 0), round(cost, 2)))

    cost_cum += cost_today

    if a.mode == "live":
        if not a.i_confirm:
            print(json.dumps({"error": "--mode live requires --i-confirm + IB gateway (gated off)"})); return 2
        print(json.dumps({"error": "live broker path not wired here -- use the audited Omega live path"})); return 2

    # ---- shadow: persist book + cash + orders + ledger + GUI result ----
    deployed = sum(t * closes.get(s, 0) for s, t in target.items())
    equity = cash + deployed
    POS.write_text(json.dumps(target, indent=0))
    STATE.write_text(json.dumps({"cash_usd": round(cash, 2), "cost_cum_usd": round(cost_cum, 2),
                                 "capital": a.capital, "updated": now}, indent=0))

    migrate_csv(ORDERS, ORDERS_HEADER)
    new_orders = not ORDERS.exists()
    with open(ORDERS, "a", newline="") as fh:
        w = csv.writer(fh)
        if new_orders: w.writerow(ORDERS_HEADER)
        for o in orders: w.writerow(o)

    migrate_csv(LEDGER, LEDGER_HEADER)
    new_ledger = not LEDGER.exists()
    with open(LEDGER, "a", newline="") as fh:
        w = csv.writer(fh)
        if new_ledger or fh.tell() == 0: w.writerow(LEDGER_HEADER)
        w.writerow([now, asof, len(target), round(deployed, 0), "|".join(target),
                    round(cash, 0), round(equity, 0), round(cost_today, 2), round(cost_cum, 2)])

    # S-2026-07-14f: per-name open-position P&L for the desk STOCK BASKET panel
    # (operator "we need a pnl column in here"). Avg cost replayed from the full
    # ORDERS history (cost-inclusive; sells reduce basis proportionally), marked
    # at the freshness-gated close. Source of truth = the same csv the fills wrote.
    positions = []
    try:
        basis: dict[str, list[float]] = {}   # sym -> [shares, cost_usd]
        if ORDERS.exists():
            with open(ORDERS) as fh:
                for row in csv.DictReader(fh):
                    s = row.get("instrument", "")
                    try:
                        sh = int(float(row["shares"])); px = float(row["price"]); c = float(row["cost"])
                    except (KeyError, ValueError):
                        continue
                    b = basis.setdefault(s, [0.0, 0.0])
                    if sh > 0:
                        b[0] += sh; b[1] += sh * px + c
                    elif sh < 0 and b[0] > 0:
                        frac = min(1.0, -sh / b[0])
                        b[0] += sh; b[1] *= (1.0 - frac)
        for s, sh in target.items():
            last = closes.get(s, 0.0)
            b = basis.get(s)
            avg = (b[1] / b[0]) if b and b[0] > 0 else 0.0
            pnl = (last - avg) * sh if avg > 0 and last > 0 else 0.0
            positions.append({"sym": s, "shares": sh, "avg_cost": round(avg, 2),
                              "last": round(last, 2), "pnl_usd": round(pnl, 2)})
    except Exception as e:                       # P&L detail must never break the book write
        positions = [{"error": str(e)}]

    result = {
        "ts": now, "as_of": asof, "mode": "shadow", "flatten": a.flatten, "capital": a.capital,
        "n_names": len(target), "deployed_usd": round(deployed, 0),
        "cash_usd": round(cash, 0), "equity_usd": round(equity, 0),
        "pnl_usd": round(equity - a.capital, 0),
        "cost_today_usd": round(cost_today, 2), "cost_cum_usd": round(cost_cum, 2),
        "orders": [{"sym": o[2], "side": o[3], "shares": o[4], "price": o[5],
                    "notional": o[6], "cost": o[7]} for o in orders],
        "book": target, "positions": positions, "skipped_no_price": skipped,
        "sizing_equity_usd": round(sizing_equity, 0), "buys_cash_capped": capped,
    }
    RESULT.write_text(json.dumps(result, indent=2))
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())

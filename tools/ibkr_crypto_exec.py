#!/usr/bin/env python3
# =============================================================================
# ibkr_crypto_exec.py -- REAL order execution for IBKR crypto on the gateway.
# (S-2026-07-02, follows the probe run that proved MBT/MET/BRR/SOL/XRP futures
#  + PAXOS spot all permissioned on the paper account DUO485909.)
#
# PURPOSE
#   The crypto book currently self-simulates fills. Port 4002 is the PAPER
#   gateway -- broker-simulated fills, real order lifecycle, real margin, zero
#   money at risk. This module places REAL (paper) orders there so the book's
#   shadow record upgrades from "we guessed the fill" to "IBKR's simulator
#   filled us". The ~/Crypto book can import place_order()/flatten() or shell
#   out to the CLI.
#
# SAFETY
#   - Default port 4002 (paper). Port 4001 (LIVE) is REFUSED unless
#     --live-i-mean-it is passed. There is no way to hit live by accident.
#   - --dry runs a whatIf margin preview instead of transmitting.
#   - Futures resolve via ContFuture (front month, auto-roll on re-resolve);
#     PAXOS spot orders are sized in USD via cashQty (IBKR requirement --
#     error 10289 if you size spot in coins).
#
# CLI
#   python3 tools/ibkr_crypto_exec.py --positions
#   python3 tools/ibkr_crypto_exec.py --symbol MBT --side BUY  --qty 1 --dry   # margin preview
#   python3 tools/ibkr_crypto_exec.py --symbol MBT --side BUY  --qty 1         # paper market order
#   python3 tools/ibkr_crypto_exec.py --symbol MET --side SELL --qty 1 --limit 1580.0
#   python3 tools/ibkr_crypto_exec.py --symbol BTC-SPOT --side BUY --usd 500   # spot sized in USD
#   python3 tools/ibkr_crypto_exec.py --flatten MBT                            # close position
# =============================================================================
import argparse, asyncio, json, sys

try:
    asyncio.get_event_loop()
except RuntimeError:
    asyncio.set_event_loop(asyncio.new_event_loop())
try:
    from ib_async import IB, Crypto, ContFuture, MarketOrder, LimitOrder
except Exception:
    try:
        from ib_insync import IB, Crypto, ContFuture, MarketOrder, LimitOrder
    except Exception as e:
        sys.exit(f"Neither ib_async nor ib_insync importable ({e}). pip3 install ib_async")

# canonical -> contract spec. Futures verified by the 2026-07-02 probe
# (QUALIFY=YES, orders accepted on DUO485909). SOL fut = 500 SOL multiplier
# (~$37k notional) -- too big for the current book; spot SOL is the sized route.
FUTURES = {
    "MBT": ("CME", "Micro Bitcoin future (0.1 BTC)"),
    "MET": ("CME", "Micro Ether future (0.1 ETH)"),
    "BRR": ("CME", "Bitcoin future (5 BTC -- BIG)"),
    "SOL": ("CME", "Solana future (500 SOL -- BIG, ~$37k notional)"),
    "XRP": ("CME", "XRP future (50k XRP)"),
}
SPOT = {  # sized in USD via cashQty
    "BTC-SPOT": "BTC", "ETH-SPOT": "ETH", "SOL-SPOT": "SOL",
    "LTC-SPOT": "LTC", "BCH-SPOT": "BCH",
}

def resolve(ib, symbol):
    s = symbol.upper()
    if s in FUTURES:
        exch, _ = FUTURES[s]
        det = ib.reqContractDetails(ContFuture(s, exch, "USD"))
        if not det: sys.exit(f"[RESOLVE-FAIL] {s}: no contract details (permission/listing?)")
        return det[0].contract, "FUT"
    if s in SPOT:
        det = ib.reqContractDetails(Crypto(SPOT[s], "PAXOS", "USD"))
        if not det: sys.exit(f"[RESOLVE-FAIL] {s}: no PAXOS contract")
        return det[0].contract, "SPOT"
    sys.exit(f"[RESOLVE-FAIL] unknown symbol {s}. Futures: {list(FUTURES)}  Spot: {list(SPOT)}")

def trade_result(tr):
    st = tr.orderStatus
    return {"status": st.status, "filled": st.filled, "avg_px": st.avgFillPrice,
            "contract": tr.contract.localSymbol,
            "log": [l.message for l in tr.log if l.message][-3:]}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4002, help="4002 paper (default). 4001 live REFUSED without --live-i-mean-it")
    ap.add_argument("--client-id", type=int, default=96)
    ap.add_argument("--symbol", help=f"futures {list(FUTURES)} or spot {list(SPOT)}")
    ap.add_argument("--side", choices=["BUY", "SELL"])
    ap.add_argument("--qty", type=float, help="contracts (futures)")
    ap.add_argument("--usd", type=float, help="cash quantity in USD (spot only)")
    ap.add_argument("--limit", type=float, help="limit price (default: market)")
    ap.add_argument("--dry", action="store_true", help="whatIf margin preview, no transmit")
    ap.add_argument("--positions", action="store_true", help="list open positions and exit")
    ap.add_argument("--flatten", metavar="SYM", help="market-close the position in SYM")
    ap.add_argument("--live-i-mean-it", action="store_true")
    a = ap.parse_args()

    if a.port == 4001 and not a.live_i_mean_it:
        sys.exit("[REFUSED] 4001 is the LIVE gateway. Pass --live-i-mean-it if you truly intend live orders.")

    ib = IB()
    ib.connect(a.host, a.port, clientId=a.client_id, timeout=20)
    mode = "PAPER" if a.port == 4002 else "LIVE"
    print(f"[EXEC] gw={a.host}:{a.port} ({mode}) accounts={ib.managedAccounts()}")

    if a.positions:
        for p in ib.positions():
            print(json.dumps({"sym": p.contract.localSymbol, "secType": p.contract.secType,
                              "pos": p.position, "avgCost": p.avgCost}))
        ib.disconnect(); return

    if a.flatten:
        c, kind = resolve(ib, a.flatten)
        pos = [p for p in ib.positions() if p.contract.conId == c.conId or
               p.contract.symbol == c.symbol]
        if not pos:
            print(f"[FLATTEN] no open position in {a.flatten}"); ib.disconnect(); return
        p = pos[0]
        side = "SELL" if p.position > 0 else "BUY"
        o = MarketOrder(side, abs(p.position)); o.tif = "DAY"
        # ib.positions() returns contracts WITHOUT exchange set -> IBKR rejects the
        # close with Warning 321 "Missing order exchange" (hit live 2026-07-02 on the
        # first MBT flatten). Route on the held contract but with the exchange filled.
        p.contract.exchange = p.contract.exchange or c.exchange or "CME"
        tr = ib.placeOrder(p.contract, o)
        while not tr.isDone(): ib.sleep(0.25)
        print("[FLATTEN]", json.dumps(trade_result(tr))); ib.disconnect(); return

    if not (a.symbol and a.side):
        sys.exit("need --symbol and --side (or --positions / --flatten)")
    c, kind = resolve(ib, a.symbol)
    if kind == "SPOT":
        if not a.usd: sys.exit("spot orders are sized in USD: pass --usd (IBKR cashQty rule)")
        o = MarketOrder(a.side, 0)
        o.cashQty = a.usd
        o.tif = "IOC"
    else:
        if not a.qty: sys.exit("futures orders need --qty (contracts)")
        o = (LimitOrder(a.side, a.qty, a.limit) if a.limit else MarketOrder(a.side, a.qty))
        o.tif = "DAY"

    if a.dry:
        st = ib.whatIfOrder(c, o)
        print("[DRY] margin preview:", json.dumps({
            "contract": c.localSymbol,
            "initMargin": getattr(st, "initMarginChange", None),
            "maintMargin": getattr(st, "maintMarginChange", None),
            "commission": getattr(st, "commission", None),
            "warning": getattr(st, "warningText", "") or ""}))
        ib.disconnect(); return

    tr = ib.placeOrder(c, o)
    # market orders on paper fill in ~1-3s; wait to terminal (cap 20s)
    for _ in range(80):
        if tr.isDone(): break
        ib.sleep(0.25)
    print("[ORDER]", json.dumps(trade_result(tr)))
    ib.disconnect()

if __name__ == "__main__":
    main()

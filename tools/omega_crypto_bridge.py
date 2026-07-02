#!/usr/bin/env python3
# =============================================================================
# omega_crypto_bridge.py -- persistent-session order bridge the ~/Crypto book
# imports to place REAL (paper) orders through IBKR instead of self-simulating.
# (S-2026-07-02. Sits on top of the probe/exec proof: MBT/MET filled live on
#  paper DUO485909, ~$2 commission = the measured 2-8bps ladder venue.)
#
# WHY A BRIDGE, NOT THE CLI:
#   The CLI (ibkr_crypto_exec.py) reconnects per call (~2s handshake) -- fine for
#   manual use, wasteful for an automated book. This holds ONE connection open
#   and auto-reconnects on drop, so the book calls submit()/flatten() with no
#   per-order handshake. Reuses the CLI's resolve()/trade_result() so contract
#   handling stays in ONE place.
#
# TWO WAYS THE BOOK USES IT:
#   (A) Python book (stall_accountant.py) -- import directly:
#         from omega_crypto_bridge import CryptoBridge
#         bx = CryptoBridge(port=4002)             # 4002 paper; 4001 refused w/o live_ok
#         fill = bx.submit("MBT", "BUY", qty=1)    # -> {"status","filled","avg_px",...}
#         bx.flatten("MBT")
#   (B) C++ engine (ibkrcrypto_engine) -- run as a JSON-line server, one request
#       per line on stdin, one JSON reply per line on stdout:
#         python3 tools/omega_crypto_bridge.py --serve --port 4002
#         > {"op":"submit","symbol":"MBT","side":"BUY","qty":1}
#         < {"status":"Filled","filled":1.0,"avg_px":60370.0,"contract":"MBTN6"}
#         ops: submit {symbol,side,qty|usd,limit?} | flatten {symbol} | positions {} | ping {}
#
# SAFETY: port 4002 (paper) default; 4001 (LIVE) refused unless live_ok=True /
# --live-i-mean-it. Never routes live by accident.
# =============================================================================
import argparse, asyncio, json, sys

try:
    asyncio.get_event_loop()
except RuntimeError:
    asyncio.set_event_loop(asyncio.new_event_loop())
try:
    from ib_async import IB, MarketOrder, LimitOrder
except Exception:
    from ib_insync import IB, MarketOrder, LimitOrder

# reuse the single source of truth for contract resolution + result shaping
from ibkr_crypto_exec import resolve, trade_result  # same tools/ dir


class CryptoBridge:
    def __init__(self, host="127.0.0.1", port=4002, client_id=95, live_ok=False):
        if port == 4001 and not live_ok:
            raise RuntimeError("port 4001 is LIVE; pass live_ok=True to intend live orders")
        self.host, self.port, self.client_id = host, port, client_id
        self.ib = IB()
        self._connect()

    def _connect(self):
        if not self.ib.isConnected():
            self.ib.connect(self.host, self.port, clientId=self.client_id, timeout=20)
        return self.ib.isConnected()

    def _ensure(self):
        # auto-reconnect: gateways drop connections on their nightly restart
        for _ in range(3):
            if self._connect():
                return
            self.ib.sleep(1.0)
        raise RuntimeError(f"cannot reach gateway {self.host}:{self.port}")

    def submit(self, symbol, side, qty=None, usd=None, limit=None):
        """Market/limit order. Futures -> qty (contracts). Spot -> usd (cashQty)."""
        self._ensure()
        c, kind = resolve(self.ib, symbol)
        if kind == "SPOT":
            if not usd:
                raise ValueError("spot orders are sized in USD (cashQty); pass usd=")
            o = MarketOrder(side, 0); o.cashQty = float(usd); o.tif = "IOC"
        else:
            if not qty:
                raise ValueError("futures orders need qty= (contracts)")
            o = (LimitOrder(side, qty, limit) if limit else MarketOrder(side, qty))
            o.tif = "DAY"
        tr = self.ib.placeOrder(c, o)
        for _ in range(80):
            if tr.isDone():
                break
            self.ib.sleep(0.25)
        return trade_result(tr)

    def flatten(self, symbol):
        self._ensure()
        c, _ = resolve(self.ib, symbol)
        pos = [p for p in self.ib.positions()
               if p.contract.conId == c.conId or p.contract.symbol == c.symbol]
        if not pos:
            return {"status": "flat", "note": f"no open position in {symbol}"}
        p = pos[0]
        side = "SELL" if p.position > 0 else "BUY"
        o = MarketOrder(side, abs(p.position)); o.tif = "DAY"
        p.contract.exchange = p.contract.exchange or c.exchange or "CME"  # Warning-321 fix
        tr = self.ib.placeOrder(p.contract, o)
        for _ in range(80):
            if tr.isDone():
                break
            self.ib.sleep(0.25)
        return trade_result(tr)

    def positions(self):
        self._ensure()
        return [{"sym": p.contract.localSymbol, "secType": p.contract.secType,
                 "pos": p.position, "avgCost": p.avgCost} for p in self.ib.positions()]

    def close(self):
        if self.ib.isConnected():
            self.ib.disconnect()


def _serve(bx):
    """One JSON request per stdin line -> one JSON reply per stdout line."""
    sys.stdout.write(json.dumps({"ready": True, "port": bx.port}) + "\n"); sys.stdout.flush()
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
            op = req.get("op")
            if op == "submit":
                r = bx.submit(req["symbol"], req["side"], req.get("qty"),
                              req.get("usd"), req.get("limit"))
            elif op == "flatten":
                r = bx.flatten(req["symbol"])
            elif op == "positions":
                r = bx.positions()
            elif op == "ping":
                r = {"pong": True, "connected": bx.ib.isConnected()}
            else:
                r = {"error": f"unknown op {op!r}"}
        except Exception as e:
            r = {"error": str(e)}
        sys.stdout.write(json.dumps(r) + "\n"); sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4002)
    ap.add_argument("--client-id", type=int, default=95)
    ap.add_argument("--serve", action="store_true", help="JSON-line server on stdin/stdout")
    ap.add_argument("--live-i-mean-it", action="store_true")
    ap.add_argument("--selftest", action="store_true", help="connect, print positions, disconnect")
    a = ap.parse_args()
    bx = CryptoBridge(a.host, a.port, a.client_id, live_ok=a.live_i_mean_it)
    try:
        if a.serve:
            _serve(bx)
        elif a.selftest:
            print(json.dumps({"connected": bx.ib.isConnected(),
                              "accounts": bx.ib.managedAccounts(),
                              "positions": bx.positions()}, indent=2))
        else:
            print("connected; use --serve or --selftest, or import CryptoBridge")
    finally:
        bx.close()


if __name__ == "__main__":
    main()

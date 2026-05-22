#!/usr/bin/env python3
# IBKR TWS L2 DOM -> CSV recorder.
# Record-only mode: subscribes reqMktDepth on a set of symbols, writes
# one CSV row per book update. Schema matches existing Omega L2 files
# (data/l2_ticks_XAUUSD_*.csv) so XauScalper backtester can read it.
#
# No order side. Read-only.
#
# Usage:
#   /tmp/ibvenv/bin/python3 tools/ibkr_dom_bridge.py \
#       --port 7497 --client-id 42 --symbols XAUUSD \
#       --out-dir /tmp/ibkr_l2 --duration-sec 600

import argparse
import csv
import json
import os
import signal
import socket
import sys
import threading
import time
from datetime import datetime, timezone

from ib_async import IB, Contract, Future


def now_ms() -> int:
    return int(time.time() * 1000)


# Index futures map -- Blackbull symbol -> IBKR Future contract spec.
# All futures (real institutional L2, continuous front-month resolved at
# runtime by resolve_front_month()).
# USTEC is treated as an alias of NAS100 (both price off NQ); subscribe once,
# dispatch both Blackbull symbols off the same feed downstream.
# Notes on Eurex symbols: external Eurex codes are FDAX / FESX, but IBKR's
# internal trading-class symbols are plain DAX / ESTX50. Wrong code returns
# "Error 200: No security definition" rather than an ambiguous-contract list.
INDEX_FUTURES = {
    "US500":  dict(symbol="ES",     exchange="CME",   currency="USD"),
    "NAS100": dict(symbol="NQ",     exchange="CME",   currency="USD"),
    "USTEC":  dict(symbol="NQ",     exchange="CME",   currency="USD"),  # alias of NAS100
    "DJ30":   dict(symbol="YM",     exchange="CBOT",  currency="USD"),
    "GER40":  dict(symbol="DAX",    exchange="EUREX", currency="EUR"),
    "ESTX50": dict(symbol="ESTX50", exchange="EUREX", currency="EUR"),
    "UK100":  dict(symbol="Z",      exchange="ICEEU", currency="GBP"),
}


def resolve_front_month(ib, contract):
    """For Future contracts without an explicit expiry, query all matching
    contract details, filter to the nearest non-expired expiry, and return
    that fully-qualified contract.  Non-future contracts pass through.
    """
    if getattr(contract, "secType", "") != "FUT":
        return contract
    if getattr(contract, "lastTradeDateOrContractMonth", "") not in ("", None):
        return contract  # caller already pinned an expiry
    details = ib.reqContractDetails(contract)
    if not details:
        raise ValueError(
            f"No contract details returned for {contract.symbol}/"
            f"{contract.exchange} -- check market data subscription or "
            f"symbol/exchange code"
        )
    today = datetime.now(timezone.utc).date()
    candidates = []
    for d in details:
        c = d.contract
        # IBKR returns either "YYYYMMDD" or "YYYYMMDD HH:MM:SS TZ" or "YYYYMM"
        raw = (c.lastTradeDateOrContractMonth or "").split(" ")[0]
        try:
            if len(raw) == 8:
                exp = datetime.strptime(raw, "%Y%m%d").date()
            elif len(raw) == 6:
                exp = datetime.strptime(raw + "01", "%Y%m%d").date()
            else:
                continue
        except ValueError:
            continue
        if exp >= today:
            candidates.append((exp, c))
    if not candidates:
        raise ValueError(
            f"All contracts for {contract.symbol}/{contract.exchange} are "
            f"in the past -- IBKR has not listed forward expirations"
        )
    candidates.sort(key=lambda x: x[0])
    chosen = candidates[0][1]
    print(
        f"front-month {contract.symbol}/{contract.exchange}: "
        f"{chosen.localSymbol} expiry={chosen.lastTradeDateOrContractMonth.split(' ')[0]} "
        f"conId={chosen.conId}",
        flush=True,
    )
    return chosen


def make_contract(sym: str) -> Contract:
    # Accept both "US500.F" (Blackbull suffix) and "US500".
    u = sym.upper()
    if u.endswith(".F"):
        u = u[:-2]
    # XAU on IBKR has multiple variants. Try CMDTY/SMART/USD first (matches
    # the TWS screenshot). Fall back to METAL/SMART if CMDTY not subscribed.
    if u == "XAUUSD":
        return Contract(symbol="XAUUSD", secType="CMDTY", exchange="SMART", currency="USD")
    if u == "XAGUSD":
        return Contract(symbol="XAGUSD", secType="CMDTY", exchange="SMART", currency="USD")
    # Index futures (CME/CBOT/EUREX/ICEEU). Empty lastTradeDateOrContractMonth
    # -- ib.qualifyContracts() at the call site resolves to the front-month
    # contract automatically, so no manual quarterly-roll logic needed here.
    if u in INDEX_FUTURES:
        m = INDEX_FUTURES[u]
        return Future(
            symbol=m["symbol"],
            exchange=m["exchange"],
            currency=m["currency"],
            lastTradeDateOrContractMonth="",
        )
    # FX pairs
    if len(u) == 6 and u.isalpha():
        base, quote = u[:3], u[3:]
        return Contract(symbol=base, secType="CASH", exchange="IDEALPRO", currency=quote)
    raise ValueError(f"Unknown symbol mapping: {sym}")


class TcpBroadcaster:
    """Accept multiple clients on host:port, broadcast newline-delimited JSON.

    Lossy: if a client buffer fills, message is dropped for that client.
    Never blocks the producer thread.
    """

    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self.clients: list[socket.socket] = []
        self.lock = threading.Lock()
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind((host, port))
        self.srv.listen(8)
        self.srv.settimeout(0.5)
        self.stop_flag = False
        self.accept_thread = threading.Thread(target=self._accept_loop, daemon=True)
        self.accept_thread.start()
        print(f"tcp-broadcaster listening on {host}:{port}", flush=True)

    def _accept_loop(self):
        while not self.stop_flag:
            try:
                conn, addr = self.srv.accept()
                conn.setblocking(False)
                with self.lock:
                    self.clients.append(conn)
                print(f"tcp client connected from {addr}", flush=True)
            except socket.timeout:
                continue
            except Exception as e:
                if not self.stop_flag:
                    print(f"tcp accept error: {e}", flush=True)

    def send(self, payload_bytes: bytes):
        if not self.clients:
            return
        dead = []
        with self.lock:
            for c in self.clients:
                try:
                    c.send(payload_bytes)
                except (BlockingIOError, OSError):
                    dead.append(c)
            for c in dead:
                try:
                    c.close()
                except Exception:
                    pass
                self.clients.remove(c)

    def stop(self):
        self.stop_flag = True
        try:
            self.srv.close()
        except Exception:
            pass
        with self.lock:
            for c in self.clients:
                try:
                    c.close()
                except Exception:
                    pass
            self.clients.clear()


class DomRecorder:
    def __init__(self, ib: IB, contract: Contract, out_path: str,
                 max_levels: int = 5, broadcaster: TcpBroadcaster | None = None):
        self.ib = ib
        self.contract = contract
        self.sym = contract.symbol
        self.max_levels = max_levels
        self.out_path = out_path
        self.broadcaster = broadcaster
        new_file = not os.path.exists(out_path) or os.path.getsize(out_path) == 0
        self.fh = open(out_path, "a", newline="", buffering=1)
        self.w = csv.writer(self.fh)
        if new_file:
            self.w.writerow([
                "ts_ms", "mid", "bid", "ask",
                "l2_imb", "l2_bid_vol", "l2_ask_vol",
                "depth_bid_levels", "depth_ask_levels",
                "depth_events_total",
            ])
        self.events = 0
        self.last_log = 0.0
        self.ticker = None

    def start(self):
        self.ticker = self.ib.reqMktDepth(
            self.contract, numRows=self.max_levels, isSmartDepth=True
        )
        self.ticker.updateEvent += self._on_update

    def stop(self):
        if self.ticker is not None:
            try:
                self.ib.cancelMktDepth(self.contract, isSmartDepth=True)
            except Exception:
                pass
        try:
            self.fh.close()
        except Exception:
            pass

    def _on_update(self, _ticker):
        t = self.ticker
        bids = t.domBids[: self.max_levels] if t.domBids else []
        asks = t.domAsks[: self.max_levels] if t.domAsks else []
        if not bids or not asks:
            return
        # Filter out level rows with zero / NaN size (IBKR sometimes emits
        # a level skeleton before the first size update).
        bid_vol = sum((b.size or 0.0) for b in bids)
        ask_vol = sum((a.size or 0.0) for a in asks)
        total = bid_vol + ask_vol
        if total <= 0.0:
            return
        imb = bid_vol / total
        bid_px = bids[0].price
        ask_px = asks[0].price
        if not bid_px or not ask_px or ask_px <= bid_px:
            return
        mid = (bid_px + ask_px) / 2.0
        self.events += 1
        ts = now_ms()
        self.w.writerow([
            ts,
            f"{mid:.4f}", f"{bid_px:.4f}", f"{ask_px:.4f}",
            f"{imb:.4f}", f"{bid_vol:.2f}", f"{ask_vol:.2f}",
            len(bids), len(asks), self.events,
        ])
        if self.broadcaster is not None:
            # Compact JSON, newline-delimited. Short keys to keep msg small.
            # bp/bs/ap/as = per-level price/size arrays (top of book first).
            bp = ",".join(f"{b.price:.4f}" for b in bids)
            bs_ = ",".join(f"{(b.size or 0.0):.2f}" for b in bids)
            ap = ",".join(f"{a.price:.4f}" for a in asks)
            as_ = ",".join(f"{(a.size or 0.0):.2f}" for a in asks)
            msg = (
                '{"ts":%d,"s":"%s","b":%.4f,"a":%.4f,'
                '"bv":%.2f,"av":%.2f,"i":%.4f,"bl":%d,"al":%d,'
                '"bp":[%s],"bs":[%s],"ap":[%s],"as":[%s]}\n'
                % (ts, self.sym, bid_px, ask_px, bid_vol, ask_vol, imb,
                   len(bids), len(asks), bp, bs_, ap, as_)
            )
            self.broadcaster.send(msg.encode("ascii"))
        now = time.monotonic()
        if now - self.last_log >= 5.0:
            self.last_log = now
            print(f"[{self.sym}] events={self.events} "
                  f"bid={bid_px} ask={ask_px} imb={imb:.3f} "
                  f"bid_vol={bid_vol:.0f} ask_vol={ask_vol:.0f}",
                  flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7497, help="7497=paper, 7496=live")
    ap.add_argument("--client-id", type=int, default=42)
    ap.add_argument("--symbols", default="XAUUSD",
                    help="comma-separated, e.g. XAUUSD,XAGUSD,EURUSD")
    ap.add_argument("--out-dir", default="/tmp/ibkr_l2")
    ap.add_argument("--duration-sec", type=int, default=0,
                    help="0 = run until Ctrl-C")
    ap.add_argument("--max-levels", type=int, default=5)
    ap.add_argument("--tcp-port", type=int, default=0,
                    help="if >0, broadcast newline-JSON to this localhost port")
    ap.add_argument("--tcp-host", default="127.0.0.1")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    syms = [s.strip().upper() for s in args.symbols.split(",") if s.strip()]
    if not syms:
        print("no symbols", file=sys.stderr)
        sys.exit(2)

    ib = IB()
    print(f"connecting to {args.host}:{args.port} clientId={args.client_id}", flush=True)
    try:
        ib.connect(args.host, args.port, clientId=args.client_id, timeout=10)
    except Exception as e:
        print(f"CONNECT FAILED: {e}", file=sys.stderr)
        print("Check: TWS running? File > Global Config > API > Settings:", file=sys.stderr)
        print("  - 'Enable ActiveX and Socket Clients' checked", file=sys.stderr)
        print(f"  - Socket port = {args.port}", file=sys.stderr)
        print(f"  - Trusted IPs includes {args.host}", file=sys.stderr)
        sys.exit(3)
    print(f"connected: server={ib.client.serverVersion()} time={ib.reqCurrentTime()}",
          flush=True)

    broadcaster: TcpBroadcaster | None = None
    if args.tcp_port > 0:
        broadcaster = TcpBroadcaster(args.tcp_host, args.tcp_port)

    recorders = []
    for sym in syms:
        try:
            contract = make_contract(sym)
            # For Future contracts left ambiguous (no expiry), pick the
            # nearest forward expiration. Pass-through for non-futures.
            contract = resolve_front_month(ib, contract)
            ib.qualifyContracts(contract)
            today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
            out_path = os.path.join(args.out_dir, f"ibkr_l2_{sym}_{today}.csv")
            rec = DomRecorder(ib, contract, out_path,
                              max_levels=args.max_levels,
                              broadcaster=broadcaster)
            rec.start()
            recorders.append(rec)
            # For futures, log the resolved front-month expiry so the operator
            # can spot when a roll is due (typically 8 trading days before
            # lastTradeDateOrContractMonth for E-mini / Eurex quarterly cycle).
            expiry = getattr(contract, "lastTradeDateOrContractMonth", "") or ""
            sectype = getattr(contract, "secType", "") or ""
            tag = f" [{sectype} expiry={expiry}]" if expiry else ""
            print(f"subscribed {sym} -> {out_path}{tag}", flush=True)
        except Exception as e:
            print(f"FAILED {sym}: {e}", file=sys.stderr)

    if not recorders:
        ib.disconnect()
        sys.exit(4)

    stop = {"now": False}

    def _sigint(_sig, _frm):
        stop["now"] = True

    signal.signal(signal.SIGINT, _sigint)
    signal.signal(signal.SIGTERM, _sigint)

    deadline = time.monotonic() + args.duration_sec if args.duration_sec > 0 else None
    print("recording... Ctrl-C to stop", flush=True)
    try:
        while not stop["now"]:
            ib.sleep(0.5)
            if deadline is not None and time.monotonic() >= deadline:
                break
    finally:
        for r in recorders:
            print(f"[{r.sym}] total_events={r.events}", flush=True)
            r.stop()
        if broadcaster is not None:
            broadcaster.stop()
        ib.disconnect()
        print("disconnected", flush=True)


if __name__ == "__main__":
    main()

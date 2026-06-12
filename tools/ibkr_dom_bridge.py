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
    "US500":   dict(symbol="ES",     exchange="CME",       currency="USD"),
    "NAS100":  dict(symbol="NQ",     exchange="CME",       currency="USD"),
    "USTEC":   dict(symbol="NQ",     exchange="CME",       currency="USD"),  # alias of NAS100
    "DJ30":    dict(symbol="YM",     exchange="CBOT",      currency="USD"),
    "GER40":   dict(symbol="DAX",    exchange="EUREX",     currency="EUR"),
    "ESTX50":  dict(symbol="ESTX50", exchange="EUREX",     currency="EUR"),
    "UK100":   dict(symbol="Z",      exchange="ICEEU",     currency="GBP"),
    # Energies -- NYMEX-listed crude / natural gas; Brent on ICEEU.
    # USOIL = WTI light sweet (CL), UKBRENT = Brent (COIL on ICEEUSOFT).
    "USOIL":   dict(symbol="CL",     exchange="NYMEX",     currency="USD"),
    "UKBRENT": dict(symbol="COIL",   exchange="IPE",       currency="USD"),
    "NGAS":    dict(symbol="NG",     exchange="NYMEX",     currency="USD"),
    # Vol + Dollar -- CFE / ICE-US.
    "VIX":     dict(symbol="VX",     exchange="CFE",       currency="USD"),
    "DX":      dict(symbol="DX",     exchange="NYBOT",     currency="USD"),
    # 2026-05-26: MGC micro gold (CME COMEX). Wired ahead of CME L2 sub
    # activation at month-start (2026-06-01). Until the subscription
    # turns on, this contract resolution will fail with error 354 "no
    # market data permissions for MGC" -- do NOT include MGC in --symbols
    # until then. Once subscribed: --symbols XAUUSD,MGC.
    "MGC":     dict(symbol="MGC",    exchange="COMEX",     currency="USD"),
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
        self.out_dir = os.path.dirname(out_path) or "."
        self.broadcaster = broadcaster
        self.cur_date = datetime.now(timezone.utc).strftime("%Y-%m-%d")
        self.fh = None
        self.w = None
        self._open_for_date(self.cur_date)
        self.events = 0
        self.last_log = 0.0
        self.ticker = None

    def _open_for_date(self, date_str):
        path = os.path.join(self.out_dir,
                            f"ibkr_l2_{self.sym}_{date_str}.csv")
        new_file = not os.path.exists(path) or os.path.getsize(path) == 0
        if self.fh is not None:
            try:
                self.fh.close()
            except Exception:
                pass
        self.fh = open(path, "a", newline="", buffering=1)
        self.w = csv.writer(self.fh)
        if new_file:
            self.w.writerow([
                "ts_ms", "mid", "bid", "ask",
                "l2_imb", "l2_bid_vol", "l2_ask_vol",
                "depth_bid_levels", "depth_ask_levels",
                "depth_events_total",
            ])
        self.out_path = path
        # 2026-06-12 iceberg groundwork: SECOND file with the full per-level
        # book (5 levels x price,size per side, one row per book update).
        # The aggregate file above is untouched (existing consumers safe);
        # this is the data an iceberg/reload detector needs offline. ~40MB/day.
        lpath = os.path.join(self.out_dir,
                             f"ibkr_l2levels_{self.sym}_{date_str}.csv")
        lnew = not os.path.exists(lpath) or os.path.getsize(lpath) == 0
        if getattr(self, "lfh", None) is not None:
            try:
                self.lfh.close()
            except Exception:
                pass
        self.lfh = open(lpath, "a", newline="", buffering=1)
        self.lw = csv.writer(self.lfh)
        if lnew:
            hdr = ["ts_ms"]
            for i in range(1, self.max_levels + 1):
                hdr += [f"b{i}p", f"b{i}s"]
            for i in range(1, self.max_levels + 1):
                hdr += [f"a{i}p", f"a{i}s"]
            self.lw.writerow(hdr)

    def _maybe_rotate(self):
        d = datetime.now(timezone.utc).strftime("%Y-%m-%d")
        if d != self.cur_date:
            print(f"[{self.sym}] UTC midnight rollover {self.cur_date} -> {d} "
                  f"events_prev_day={self.events}", flush=True)
            self.cur_date = d
            self.events = 0
            self._open_for_date(d)

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
        try:
            self.lfh.close()
        except Exception:
            pass


class TradesRecorder:
    """2026-06-12 iceberg groundwork: tick-by-tick trade prints (AllLast).

    Iceberg/reload detection needs executions at a price level to compare
    against displayed size -- the DOM alone can't see hidden liquidity get
    consumed. Futures only (MGC): spot CFD XAUUSD has no centralized tape.
    Writes ibkr_trades_{sym}_{date}.csv: ts_ms,price,size,exch,spec.
    """
    def __init__(self, ib: IB, contract: Contract, out_dir: str):
        self.ib = ib
        self.contract = contract
        self.sym = contract.symbol
        self.out_dir = out_dir
        self.cur_date = datetime.now(timezone.utc).strftime("%Y-%m-%d")
        self.fh = None
        self.w = None
        self.n = 0
        self._open_for_date(self.cur_date)
        self.ticker = None

    def _open_for_date(self, date_str):
        path = os.path.join(self.out_dir, f"ibkr_trades_{self.sym}_{date_str}.csv")
        new_file = not os.path.exists(path) or os.path.getsize(path) == 0
        if self.fh is not None:
            try:
                self.fh.close()
            except Exception:
                pass
        self.fh = open(path, "a", newline="", buffering=1)
        self.w = csv.writer(self.fh)
        if new_file:
            self.w.writerow(["ts_ms", "price", "size", "exch", "spec"])

    def start(self):
        self.ticker = self.ib.reqTickByTickData(self.contract, "AllLast", 0, False)
        self.ticker.updateEvent += self._on_update

    def stop(self):
        try:
            self.ib.cancelTickByTickData(self.contract, "AllLast")
        except Exception:
            pass
        try:
            self.fh.close()
        except Exception:
            pass

    def _on_update(self, ticker):
        d = datetime.now(timezone.utc).strftime("%Y-%m-%d")
        if d != self.cur_date:
            self.cur_date = d
            self._open_for_date(d)
        for t in (ticker.tickByTicks or []):
            try:
                ts = int(t.time.timestamp() * 1000) if hasattr(t.time, "timestamp") else now_ms()
                self.w.writerow([ts, f"{t.price:.4f}", f"{t.size:.2f}",
                                 getattr(t, "exchange", ""),
                                 getattr(t, "specialConditions", "")])
                self.n += 1
            except Exception:
                continue
        # ib_async leaves processed ticks in the list; clear so we don't rewrite
        try:
            ticker.tickByTicks.clear()
        except Exception:
            pass

    def _on_update(self, _ticker):
        self._maybe_rotate()
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
        # per-level row (iceberg groundwork; pads short books with 0,0)
        lrow = [ts]
        for i in range(self.max_levels):
            if i < len(bids):
                lrow += [f"{bids[i].price:.4f}", f"{(bids[i].size or 0.0):.2f}"]
            else:
                lrow += ["0", "0"]
        for i in range(self.max_levels):
            if i < len(asks):
                lrow += [f"{asks[i].price:.4f}", f"{(asks[i].size or 0.0):.2f}"]
            else:
                lrow += ["0", "0"]
        self.lw.writerow(lrow)
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

    # TcpBroadcaster (the server Omega's IbkrDomConsumer connects to) is created
    # ONCE and kept alive across IB reconnects, so Omega never sees the feed port
    # drop just because the IB session bounced.
    broadcaster: TcpBroadcaster | None = None
    if args.tcp_port > 0:
        broadcaster = TcpBroadcaster(args.tcp_host, args.tcp_port)

    stop = {"now": False}

    def _sigint(_sig, _frm):
        stop["now"] = True

    signal.signal(signal.SIGINT, _sigint)
    signal.signal(signal.SIGTERM, _sigint)

    deadline = time.monotonic() + args.duration_sec if args.duration_sec > 0 else None
    RECONNECT_SEC = 15

    def subscribe_all():
        recs = []
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
                recs.append(rec)
                # 2026-06-12 iceberg groundwork: trade prints for futures only
                # (centralized tape exists; spot CFD has none).
                if (getattr(contract, "secType", "") or "") == "FUT":
                    tr = TradesRecorder(ib, contract, args.out_dir)
                    tr.start()
                    recs.append(tr)
                    print(f"trade-prints ON {sym} (AllLast)", flush=True)
                # For futures, log the resolved front-month expiry so the operator
                # can spot when a roll is due (typically 8 trading days before
                # lastTradeDateOrContractMonth for E-mini / Eurex quarterly cycle).
                expiry = getattr(contract, "lastTradeDateOrContractMonth", "") or ""
                sectype = getattr(contract, "secType", "") or ""
                tag = f" [{sectype} expiry={expiry}]" if expiry else ""
                print(f"subscribed {sym} -> {out_path}{tag}", flush=True)
            except Exception as e:
                print(f"FAILED {sym}: {e}", file=sys.stderr)
        return recs

    # ---- supervise loop: (re)connect + (re)subscribe until stop/deadline ------
    # A gateway daily-restart or IBKR data-farm reset drops the IB connection.
    # The bridge previously connected ONCE then idled forever (no reconnect), so
    # a single drop turned it into a zombie -- alive but producing no DOM, which
    # caused multi-hour data gaps (e.g. 04:16 -> next manual restart, ~18h). Now
    # it detects the disconnect via ib.isConnected() and reconnects on its own
    # within RECONNECT_SEC, re-subscribing all symbols. The freshness watchdog is
    # now only a last-resort backstop.
    try:
        while not stop["now"]:
            if not ib.isConnected():
                try:
                    print(f"connecting to {args.host}:{args.port} clientId={args.client_id}",
                          flush=True)
                    ib.connect(args.host, args.port, clientId=args.client_id, timeout=10)
                    print(f"connected: server={ib.client.serverVersion()} "
                          f"time={ib.reqCurrentTime()}", flush=True)
                except Exception as e:
                    print(f"CONNECT FAILED: {e} -- retry in {RECONNECT_SEC}s "
                          f"(gateway up? port {args.port}? API enabled? trusted IP {args.host}?)",
                          file=sys.stderr)
                    time.sleep(RECONNECT_SEC)
                    continue

            recorders = subscribe_all()
            if not recorders:
                print(f"no recorders subscribed -- retry in {RECONNECT_SEC}s", file=sys.stderr)
                try:
                    ib.disconnect()
                except Exception:
                    pass
                time.sleep(RECONNECT_SEC)
                continue

            print("recording... Ctrl-C to stop", flush=True)
            # pump the event loop until the session drops, stop, or deadline
            while not stop["now"] and ib.isConnected():
                ib.sleep(0.5)
                if deadline is not None and time.monotonic() >= deadline:
                    stop["now"] = True
                    break

            for r in recorders:
                print(f"[{r.sym}] total_events={r.events}", flush=True)
                try:
                    r.stop()
                except Exception:
                    pass

            if not stop["now"]:
                print(f"DISCONNECTED -- reconnecting in {RECONNECT_SEC}s", file=sys.stderr)
                try:
                    ib.disconnect()
                except Exception:
                    pass
                time.sleep(RECONNECT_SEC)
    finally:
        if broadcaster is not None:
            broadcaster.stop()
        try:
            ib.disconnect()
        except Exception:
            pass
        print("disconnected", flush=True)


if __name__ == "__main__":
    main()

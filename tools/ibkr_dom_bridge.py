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
    "BRENT":   dict(symbol="COIL",   exchange="IPE",       currency="USD"),  # alias of UKBRENT (BlackBull ext symbol is "BRENT")
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
    # 2026-07-09: M2K micro E-mini Russell 2000 (CME). L1-only (not in DEPTH_SYMBOLS),
    # no depth-slot cost -- feeds the index mimic ladder SHADOW book (M2K validated
    # W24/thr1.5 + BE-entry0.08 = +76.5% WF both halves; live-wire this session). CME
    # bundle already entitled (same as ES/NQ); if not, resolve fails 354 -> book stays
    # quiet (safe, shadow). Only add M2K to --symbols after confirming the CME sub.
    "M2K":     dict(symbol="M2K",    exchange="CME",       currency="USD"),
}


# S-2026-07-09 FEED MIGRATION: the ONLY symbols that use the capped reqMktDepth
# path (IBKR limits concurrent reqMktDepth to 3 streams on this account tier).
# EVERY other non-CASH symbol on --symbols routes to L1 (reqMktData top-of-book),
# which carries NO depth-slot cost -- the same rationale that lets the FX majors
# coexist with the 3 depth streams. This is what moves US500/GER40/UK100/ESTX50/
# USOIL/XAGUSD/VIX/DX/NGAS/UKBRENT off BlackBull FIX onto IBKR without freeing a
# depth slot. MGC is kept here so a future re-add lands on the depth path (the
# Aurora footprint feed needs its book), not on L1.
# S-2026-07-21: XAUUSD REMOVED from the depth set. COMEX metal L2 depth is not
# entitled on this account (reqMktDepth returns nothing -> the whole XAUUSD feed
# starved: no ibkr_l1/l2_XAUUSD, GUI PR panel stuck "waiting for m5 bars"), while
# plain L1 (reqMktData top-of-book) returns live gold bid/ask fine (probed
# 4067.13/4067.49). Gold depth was only consumed by MicroScalperGold, culled this
# session, so XAUUSD now rides L1 like XAGUSD/the indices -- no depth-slot cost.
DEPTH_SYMBOLS = {"DJ30", "NAS100", "MGC"}


# S-2026-07-10 BIGCAP STOCK L1: the mimic ladder companion's LIVE-CONFIRMATION gate
# (include/StockDayMoverLadderCompanion.hpp) needs a real-time top-of-book quote per bigcap
# name to confirm "actively trading + rising" before it opens a pending +thr window. Each of
# these tickers maps to a US-equity STK/SMART/USD contract subscribed as L1 (reqMktData, NOT
# reqMktDepth) -> NO depth-slot cost, so they coexist with the 3-stream DEPTH cap exactly like
# the FX/index L1 lines. US-equity real-time L1 entitlement was VERIFIED live on this account
# 2026-07-10 (NVDA/AVGO/SMCI/DELL returned real-time bid/ask, mdType=1, no error 354).
#
# This is the CAPABILITY MAP (make_contract can resolve any of these). The bridge only actually
# subscribes what is on --symbols. LINE-BUDGET CAUTION: ~45 extra L1 lines may exceed the IBKR
# simultaneous-line budget on this tier -- add only the ACTIVE roster (names currently armed /
# pending, e.g. NVDA,AVGO,SMCI,DELL,NBIS,CRDO plus the x2 elites) to --symbols, not all 45.
# The full 45-name roster mirrors engine_init.hpp BIGCAP_LAD (39 + the S-2026-07-08c adds).
STOCKS = {
    "NVDA", "AMD", "AVGO", "MU", "MRVL", "SMCI", "ARM", "PLTR", "TSLA", "META", "NFLX", "CRWD",
    "SHOP", "COIN", "MSTR", "SNOW", "NOW", "PANW", "UBER", "ABNB", "DELL", "ORCL", "QCOM", "INTC",
    "AMZN", "GOOGL", "MSFT", "AAPL", "CRM", "ADBE", "IONQ", "RGTI", "QBTS", "ASTS", "RKLB", "NBIS",
    "CRWV", "ALAB", "CRDO", "WDC", "STX", "DD", "TPR", "BMY", "SWKS",
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
    # ROLL OFFSET (2026-06-17): the NEAREST non-expired contract is NOT the
    # liquid one near expiry -- trade volume rolls to the next contract ~a week
    # out while the expiring one still QUOTES (depth floods, tape goes to ~0).
    # That starved the Aurora footprint feed: NQ June (exp in 3d) had volume in
    # Sept; MGC June rolling to Aug. If the front expires within ROLL_DAYS and a
    # next contract exists, record the next one (the liquid month). Recording a
    # liquid next-month slightly early is harmless; recording a dead front month
    # loses the whole tape. Pin an explicit expiry upstream to override.
    ROLL_DAYS = 10
    idx = 0
    days_to_exp = (candidates[0][0] - today).days
    if days_to_exp <= ROLL_DAYS and len(candidates) > 1:
        idx = 1
        print(
            f"[roll] {contract.symbol}/{contract.exchange}: front "
            f"{candidates[0][1].localSymbol} expires in {days_to_exp}d "
            f"(<= {ROLL_DAYS}) -> rolling to liquid next "
            f"{candidates[1][1].localSymbol}",
            flush=True,
        )
    chosen = candidates[idx][1]
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
    # S-2026-07-10 BIGCAP STOCK L1: US-equity top-of-book for the mimic ladder live-
    # confirmation gate. STK/SMART/USD -> resolve_front_month passes through (not FUT),
    # subscribe_all routes it to L1Recorder (non-CASH, not in DEPTH_SYMBOLS) broadcasting
    # contract.symbol == the ticker, which omega_main on_book forwards to on_live_tick.
    # Checked BEFORE the FX branch so a 6-char alpha ticker never mis-parses as an FX pair.
    if u in STOCKS:
        return Contract(symbol=u, secType="STK", exchange="SMART", currency="USD")
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
        # Roll-break watchdog (2026-07-03): monotonic timestamp of the last
        # written depth event. A futures contract that has ROLLED keeps the IB
        # session connected but goes silent (the expiring/expired month stops
        # quoting). The supervise loop only re-resolves on DISCONNECT, so a roll
        # left the bridge pinned to a dead contract -- MGC went header-only for a
        # full day (07-02). check_rolls() below uses this to detect the silence.
        self.last_event_mono = time.monotonic()
        self.is_fut = (getattr(contract, "secType", "") or "") == "FUT"

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
        self.last_event_mono = time.monotonic()
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
        self.last_event_mono = time.monotonic()
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

class L1Recorder:
    """Top-of-book (L1) recorder for FX majors -- S-2026-07-06.

    FX on IDEALPRO needs only best bid/ask; reqMktData (L1) carries NO
    depth-slot cost, so FX coexists with the 3-stream reqMktDepth cap that pins
    the depth recorders at XAUUSD,DJ30,NAS100. Broadcasts newline-JSON in the
    SAME schema IbkrDomConsumer.parse_line() reads (b/a top-of-book; bv/av/imb
    synthetic 0.5 since L1 has no book depth). The broadcast symbol `s` is the
    full 6-char pair (e.g. EURUSD), NOT contract.symbol ("EUR"), so the C++
    consumer's lookup() resolves it to the right FX slot.
    """
    def __init__(self, ib: IB, contract: Contract, bcast_sym: str,
                 out_dir: str, broadcaster: "TcpBroadcaster | None" = None):
        self.ib = ib
        self.contract = contract
        self.sym = bcast_sym            # broadcast/CSV symbol = the pair (EURUSD)
        self.out_dir = out_dir
        self.broadcaster = broadcaster
        self.cur_date = datetime.now(timezone.utc).strftime("%Y-%m-%d")
        self.fh = None
        self.w = None
        self.events = 0
        self.last_log = 0.0
        self.last_bid = 0.0
        self.last_ask = 0.0
        self.last_event_mono = time.monotonic()
        self.is_fut = False
        self.ticker = None
        self._open_for_date(self.cur_date)

    def _open_for_date(self, date_str):
        path = os.path.join(self.out_dir, f"ibkr_l1_{self.sym}_{date_str}.csv")
        new_file = not os.path.exists(path) or os.path.getsize(path) == 0
        if self.fh is not None:
            try: self.fh.close()
            except Exception: pass
        self.fh = open(path, "a", newline="", buffering=1)
        self.w = csv.writer(self.fh)
        if new_file:
            self.w.writerow(["ts_ms", "bid", "ask"])

    def _maybe_rotate(self):
        d = datetime.now(timezone.utc).strftime("%Y-%m-%d")
        if d != self.cur_date:
            self.cur_date = d
            self.events = 0
            self._open_for_date(d)

    def start(self):
        self.last_event_mono = time.monotonic()
        # snapshot=False -> streaming; regulatory/genericTicks empty = plain quote.
        self.ticker = self.ib.reqMktData(self.contract, "", False, False)
        self.ticker.updateEvent += self._on_update

    def stop(self):
        if self.ticker is not None:
            try: self.ib.cancelMktData(self.contract)
            except Exception: pass
        try: self.fh.close()
        except Exception: pass

    def _on_update(self, _ticker):
        self._maybe_rotate()
        t = self.ticker
        bid, ask = t.bid, t.ask
        # ib_async uses NaN for absent sides; NaN != NaN filters them out.
        if not (bid == bid and ask == ask):
            return
        if not bid or not ask or ask <= bid:
            return
        self.last_bid, self.last_ask = bid, ask
        self.events += 1
        self.last_event_mono = time.monotonic()
        ts = now_ms()
        self.w.writerow([ts, f"{bid:.5f}", f"{ask:.5f}"])
        if self.broadcaster is not None:
            # Synthetic depth fields (L1 has no book): bv/av/imb = balanced,
            # bl/al = 1. No per-level arrays -> parse_line treats them optional.
            msg = (
                '{"ts":%d,"s":"%s","b":%.5f,"a":%.5f,'
                '"bv":1.00,"av":1.00,"i":0.5000,"bl":1,"al":1}\n'
                % (ts, self.sym, bid, ask)
            )
            self.broadcaster.send(msg.encode("ascii"))
        now = time.monotonic()
        if now - self.last_log >= 5.0:
            self.last_log = now
            print(f"[{self.sym}] L1 events={self.events} bid={bid} ask={ask}",
                  flush=True)


# Roll-break watchdog threshold: a live futures contract emits depth events
# near-continuously during market hours; sustained silence means the contract
# has expired/rolled. We only ACT on the silence if re-resolving the front month
# now returns a DIFFERENT conId -- so genuine quiet windows (CME daily halt,
# weekend close) never trigger a needless resubscribe; only an actual roll does.
FUT_ROLL_STALE_SEC = 30 * 60


def check_rolls(ib, recorders):
    """Return True if any FUT recorder has gone silent past the threshold AND
    the current front month now resolves to a different contract (a real roll),
    signalling the caller to tear down and resubscribe (which re-resolves)."""
    now = time.monotonic()
    for r in recorders:
        if not getattr(r, "is_fut", False):
            continue
        if now - getattr(r, "last_event_mono", now) < FUT_ROLL_STALE_SEC:
            continue
        try:
            fresh = resolve_front_month(ib, make_contract(r.sym))
            cur_id = getattr(r.contract, "conId", 0)
            new_id = getattr(fresh, "conId", 0)
            if new_id and new_id != cur_id:
                print(
                    f"[roll-watchdog] {r.sym} silent "
                    f"{int(now - r.last_event_mono)}s and front month rolled "
                    f"{cur_id} -> {new_id} ({getattr(fresh,'localSymbol','?')}) "
                    f"-- forcing resubscribe",
                    flush=True,
                )
                return True
            # Same contract, just quiet: reset baseline so we don't re-resolve
            # every pump tick during a legitimate halt/weekend.
            r.last_event_mono = now
        except Exception as e:
            print(f"[roll-watchdog] {r.sym} re-resolve failed: {e}",
                  file=sys.stderr)
    return False


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

    # SINGLE-INSTANCE GUARD (S-2026-07-09b): a DUPLICATE bridge started with the same --client-id
    # fights the first over the IBKR Gateway connection (IBKR allows one socket per client-id) ->
    # both flap "disconnected, reconnecting in 2s" and the consumer's L1 (USDCAD) goes stale. The
    # feed tcp-port is the natural single-instance lock: pre-flight bind it; if it's already taken,
    # another bridge already owns the feed -> EXIT NOW, before connecting to Gateway with a
    # colliding client-id. Self-healing: a crashed prior bridge frees the port so a fresh launch
    # wins. (The real TcpBroadcaster re-binds the port a few lines below.)
    # HELD lock socket on a dedicated per-client-id port. bind() is atomic, so of TWO bridges
    # launched in the same instant (the venv-launcher double-spawn we hit) exactly ONE wins the
    # bind; the other gets EADDRINUSE -> exits BEFORE touching Gateway. The winner HOLDS the
    # socket for its whole lifetime (never closed), so no close-race window. Self-healing: a dead
    # bridge's lock frees so a fresh launch wins. Per-client-id (19000+cid) so bridges with
    # different client-ids can still coexist.
    _lock_port = 19000 + int(args.client_id)
    _lock_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    _lock_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 0)   # do NOT allow a 2nd bind
    try:
        _lock_sock.bind(("127.0.0.1", _lock_port))
        _lock_sock.listen(1)
    except OSError:
        print(f"[GUARD] single-instance lock :{_lock_port} held -- another ibkr_dom_bridge "
              f"(client-id {args.client_id}) is already running; exiting", file=sys.stderr)
        sys.exit(0)
    globals()["_SINGLETON_LOCK"] = _lock_sock   # keep a ref so it is never GC'd/closed while alive

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
    dup = {"hit": False}

    def _sigint(_sig, _frm):
        stop["now"] = True

    signal.signal(signal.SIGINT, _sigint)

    # DUPLICATE-EXIT (S-2026-07-09b): IBKR Gateway allows one socket per clientId. If a second
    # ibkr_dom_bridge with the same --client-id starts (task double-fire / venv launcher / stale
    # respawn), Gateway sends error 326 ("client id is already in use"). Without this the loser
    # just reconnect-loops forever and both flap, starving USDCAD L1. Exit cleanly on 326 so the
    # FIRST bridge keeps the feed uncontested -- topology-independent, complements the port guard.
    def _on_ib_err(reqId, code, msg, contract=None):
        if code == 326 or "already in use" in (str(msg) or "").lower():
            dup["hit"] = True
            stop["now"] = True
            print(f"[GUARD] IBKR error {code} (client id {args.client_id} already in use) -- "
                  f"another bridge owns the feed; exiting", file=sys.stderr)
    try:
        ib.errorEvent += _on_ib_err
    except Exception:
        pass
    signal.signal(signal.SIGTERM, _sigint)

    deadline = time.monotonic() + args.duration_sec if args.duration_sec > 0 else None
    RECONNECT_SEC = 15

    def subscribe_all():
        recs = []
        # Live market data (type 1). FX L1 quotes need this; harmless for depth.
        try:
            ib.reqMarketDataType(1)
        except Exception:
            pass
        for sym in syms:
            try:
                contract = make_contract(sym)
                # For Future contracts left ambiguous (no expiry), pick the
                # nearest forward expiration. Pass-through for non-futures.
                contract = resolve_front_month(ib, contract)
                ib.qualifyContracts(contract)
                today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
                # FX majors (CASH/IDEALPRO) -> L1 (reqMktData). No depth-slot cost,
                # so they coexist with the 3-stream reqMktDepth cap. S-2026-07-06.
                if (getattr(contract, "secType", "") or "") == "CASH":
                    l1 = L1Recorder(ib, contract, sym, args.out_dir,
                                    broadcaster=broadcaster)
                    l1.start()
                    recs.append(l1)
                    print(f"subscribed {sym} -> L1 (reqMktData IDEALPRO top-of-book)",
                          flush=True)
                    continue
                # S-2026-07-09 FEED MIGRATION: any non-CASH symbol that is NOT one of
                # the 3 capped depth streams (DEPTH_SYMBOLS) rides L1 (reqMktData top-
                # of-book), NOT reqMktDepth -- no depth-slot cost, so it never touches
                # the 3-stream cap. Broadcast symbol = contract.symbol (ES/DAX/Z/CL/VX/
                # DX/NG/COIL/XAGUSD/ESTX50); IbkrDomConsumer.lookup() aliases each to its
                # Omega slot and omega_main on_book remaps it to the engine tick symbol.
                norm = sym.upper()
                if norm.endswith(".F"):
                    norm = norm[:-2]
                if norm not in DEPTH_SYMBOLS:
                    l1 = L1Recorder(ib, contract, contract.symbol, args.out_dir,
                                    broadcaster=broadcaster)
                    l1.start()
                    recs.append(l1)
                    print(f"subscribed {sym} -> L1 (reqMktData {contract.symbol}; "
                          f"index/commodity, no depth-slot cost)", flush=True)
                    continue
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
            # pump the event loop until the session drops, stop, deadline, OR a
            # futures roll is detected (check_rolls breaks out -> resubscribe).
            next_roll_check = time.monotonic() + 60.0
            while not stop["now"] and ib.isConnected():
                ib.sleep(0.5)
                if deadline is not None and time.monotonic() >= deadline:
                    stop["now"] = True
                    break
                if time.monotonic() >= next_roll_check:
                    next_roll_check = time.monotonic() + 60.0
                    if check_rolls(ib, recorders):
                        break  # tear down + resubscribe re-resolves front month

            for r in recorders:
                # DomRecorder has .events, TradesRecorder has .n -- fall back so
                # the shutdown summary never AttributeErrors on a mixed list.
                print(f"[{r.sym}] total_events={getattr(r, 'events', getattr(r, 'n', 0))}", flush=True)
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

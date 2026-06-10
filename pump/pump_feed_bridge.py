#!/usr/bin/env python3
"""
pump_feed_bridge — IBKR -> stdout feed for the C++ pump_shadow runner.

DATA ONLY. No trading logic, no orders. This is the thin Python pipe the C++
engine needs because the IBKR API is Python (same pattern as every Omega IBKR
feed: ibkr_dom_bridge.py, OmegaMgcLiveBars). All decisions live in the C++
PumpScalpManager downstream.

What it does, every 5s:
  - (every 30s) scan IBKR for top % gainers; subscribe streaming quotes for names
    already up >= PREFILTER_PCT from the day open (cap MAX_SYMBOLS).
  - on first subscribe, SEED today's 5/10/15m history so the engine is warm.
  - emit a price tick and roll 5/10/15m bars from the streaming last price.

Feed protocol (stdout, line-buffered):
  B,SYM,TF_SEC,o,h,l,c,v,ts_ms     one CLOSED timeframe bar
  P,SYM,px,ts_ms                   one price tick

Run:  python pump_feed_bridge.py | pump_shadow.exe --gate 100
"""
import sys, time, math, socket, threading, json
from http.server import BaseHTTPRequestHandler, HTTPServer
from ib_async import IB, Stock, ScannerSubscription, util

IB_HOST, IB_PORT, IB_CID = "127.0.0.1", 4002, 33   # paper; distinct clientId
PREFILTER_PCT = 40.0      # only subscribe names already this far up from open (engine gate is 100)
MAX_SYMBOLS   = 12
TFS           = [300, 600, 900]
SCAN_EVERY    = 30
TICK_EVERY    = 5
SERVE_PORT    = 7782      # TCP server for the in-Omega PumpFeedConsumer (--serve)

# In server mode, feed lines stream to the connected Omega consumer; else stdout
# (standalone pump_shadow.exe). A background thread owns the accept loop.
_conn = None
_serve = False
_candidates = {}          # sym -> dict(px, day_open, up, ts) for the scanner web page
SCANNER_HTTP_PORT = 7783  # "separate address": http://<vps>:7783 shows live pump candidates


class _ScanHandler(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        rows = sorted(_candidates.values(), key=lambda c: -c["up"])
        if self.path.startswith("/api"):
            body = json.dumps(rows).encode(); ctype = "application/json"
        else:
            # styled to match the Omega dashboard SCREENER panel (dark/mono/teal)
            def row(c):
                arrow = '<span style="color:#3ddc97">&#9650;</span>'   # up-mover
                gate = '<span style="color:#3ddc97">TRADE</span>' if c["up"] >= 100 else \
                       '<span style="color:#6b6b6b">watch</span>'
                return (f"<tr><td style='color:#d8d8d8'>{c['sym']}</td>"
                        f"<td>{c['px']:.3f}</td><td>{arrow}</td>"
                        f"<td style='color:#e6a23c'>+{c['up']:.0f}%</td>"
                        f"<td>{c['day_open']:.3f}</td><td>{gate}</td></tr>")
            trs = "".join(row(c) for c in rows) or \
                  "<tr><td colspan=6 style='color:#6b6b6b'>no live movers — waiting for a pump</td></tr>"
            n_trade = sum(1 for c in rows if c["up"] >= 100)
            body = (
                "<html><head><title>PUMP SCANNER</title><meta http-equiv=refresh content=5>"
                "<style>"
                "body{background:#0a0a0a;color:#c8c8c8;font:13px/1.5 'SF Mono',Menlo,monospace;margin:0;padding:18px}"
                ".panel{border:1px solid #1d1d1d;border-radius:6px;padding:14px 16px;max-width:720px}"
                ".hdr{color:#9aa0a6;letter-spacing:.5px;font-size:12px;margin-bottom:10px}"
                "table{border-collapse:collapse;width:100%}"
                "th{text-align:left;color:#5fd3e0;font-weight:normal;padding:4px 16px 8px 0;border-bottom:1px solid #1d1d1d}"
                "td{padding:5px 16px 5px 0;white-space:nowrap}"
                ".foot{color:#6b6b6b;font-size:11px;margin-top:12px}"
                "</style></head><body><div class=panel>"
                f"<div class=hdr>PUMP SCANNER &middot; {len(rows)} live movers &middot; {n_trade} at gate (&ge;100%)</div>"
                "<table><tr><th>symbol</th><th>price</th><th>trend</th><th>up from open</th>"
                "<th>day open</th><th>status</th></tr>"
                f"{trs}</table>"
                "<div class=foot>up = % above today's open &middot; TRADE = past the 100% gate the 5/10/15m "
                "engines act on &middot; live pump trades show in the dashboard RUNNING TRADES panel</div>"
                "</div></body></html>").encode()
            ctype = "text/html"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Access-Control-Allow-Origin", "*")   # so the dashboard could embed it later
        self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)


def _http_thread(port):
    try:
        HTTPServer(("0.0.0.0", port), _ScanHandler).serve_forever()
    except Exception as e:
        sys.stderr.write(f"[pump_bridge] scanner http error: {e}\n")


def _serve_thread(port):
    global _conn
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port)); srv.listen(1)
    sys.stderr.write(f"[pump_bridge] serving on 127.0.0.1:{port}\n"); sys.stderr.flush()
    while True:
        c, _ = srv.accept()
        sys.stderr.write("[pump_bridge] consumer connected\n"); sys.stderr.flush()
        _conn = c


def emit(s: str):
    global _conn
    if _serve:
        if _conn is not None:
            try:
                _conn.sendall((s + "\n").encode())
            except Exception:
                try: _conn.close()
                except Exception: pass
                _conn = None        # accept loop will pick up the next consumer
    else:
        sys.stdout.write(s + "\n")
        sys.stdout.flush()


class Sub:
    """One subscribed symbol: streaming ticker + per-TF bar aggregation state."""
    def __init__(self, ticker):
        self.ticker = ticker
        self.day_open = 0.0
        self.last_vol = None
        self.bars = {tf: None for tf in TFS}   # tf -> [bucket_ms, o,h,l,c,v]

    def roll(self, sym, px, vol, ts_ms):
        if px is None or px <= 0 or px != px:
            return
        emit(f"P,{sym},{px},{ts_ms}")
        dv = 0.0
        if vol is not None and self.last_vol is not None and vol >= self.last_vol:
            dv = vol - self.last_vol
        if vol is not None:
            self.last_vol = vol
        for tf in TFS:
            bkt = (ts_ms // (tf * 1000)) * (tf * 1000)
            b = self.bars[tf]
            if b is None:
                self.bars[tf] = [bkt, px, px, px, px, dv]
            elif bkt != b[0]:
                emit(f"B,{sym},{tf},{b[1]},{b[2]},{b[3]},{b[4]},{b[5]},{b[0]}")   # closed bar
                self.bars[tf] = [bkt, px, px, px, px, dv]
            else:
                b[2] = max(b[2], px); b[3] = min(b[3], px); b[4] = px; b[5] += dv


def seed(ib, sym):
    """Replay today's 5/10/15m history as B lines so the engine warms instantly.
    Returns (day_open, day_high) from the 5m bars for a reliable prefilter."""
    c = Stock(sym, "SMART", "USD")
    try:
        q = ib.qualifyContracts(c)
        if q: c = q[0]
    except Exception:
        return None
    day_open = day_high = None
    for tf in TFS:
        bs = "5 mins" if tf == 300 else ("10 mins" if tf == 600 else "15 mins")
        try:
            bars = ib.reqHistoricalData(c, endDateTime="", durationStr="1 D",
                                        barSizeSetting=bs, whatToShow="TRADES",
                                        useRTH=False, formatDate=1)
        except Exception:
            continue
        for b in bars:
            ts = int(b.date.timestamp() * 1000)
            emit(f"S,{sym},{tf},{b.open},{b.high},{b.low},{b.close},{b.volume},{ts}")  # S=seed: warm only
            if tf == 300:
                if day_open is None and b.open > 0:
                    day_open = b.open
                day_high = b.high if day_high is None else max(day_high, b.high)
    return (day_open, day_high) if day_open else None


def main():
    ib = IB()
    ib.connect(IB_HOST, IB_PORT, clientId=IB_CID, timeout=15)
    try:
        ib.reqMarketDataType(3)   # delayed ok in paper if no live entitlement
    except Exception:
        pass
    subs = {}           # sym -> Sub
    last_scan = 0.0
    emit("# pump_feed_bridge up")

    while True:
        now = time.time()
        # ---- periodic scan + subscribe ----
        if now - last_scan >= SCAN_EVERY:
            last_scan = now
            try:
                sc = ScannerSubscription(instrument="STK", locationCode="STK.US.MAJOR",
                                         scanCode="TOP_PERC_GAIN")
                sc.abovePrice = 0.3
                rows = ib.reqScannerData(sc)
                for item in rows[:40]:
                    if len(subs) >= MAX_SYMBOLS:
                        break
                    sym = str(item.contractDetails.contract.symbol).upper()
                    if sym in subs:
                        continue
                    c = Stock(sym, "SMART", "USD")
                    try:
                        q = ib.qualifyContracts(c)
                        if q: c = q[0]
                    except Exception:
                        continue
                    # use today's real 5m bars (not the cold ticker) to judge the move
                    sd = seed(ib, sym)
                    if not sd:
                        continue
                    d_open, d_high = sd
                    move = (d_high / d_open - 1.0) * 100.0 if d_open else 0.0
                    if move < PREFILTER_PCT:                       # not a real mover -> skip
                        continue
                    tk = ib.reqMktData(c, "", False, False)
                    ib.sleep(0.5)
                    s = Sub(tk); s.day_open = d_open
                    subs[sym] = s
                    emit(f"# subscribe {sym} day {d_open:.3f}->{d_high:.3f} up={move:.0f}%")
            except Exception as e:
                emit(f"# scan error: {e}")

        # ---- emit ticks + roll bars + scanner candidate for all subs ----
        ts_ms = int(now * 1000)
        for sym, s in list(subs.items()):
            tk = s.ticker
            px = tk.last or tk.marketPrice() or tk.close
            vol = getattr(tk, "volume", None)
            try:
                s.roll(sym, float(px) if px else None, float(vol) if vol else None, ts_ms)
                if px and s.day_open:                              # C = scanner candidate (feed + web page)
                    up = (float(px) / s.day_open - 1.0) * 100.0
                    emit(f"C,{sym},{float(px)},{s.day_open},{up:.1f},{ts_ms}")
                    _candidates[sym] = {"sym": sym, "px": float(px), "day_open": s.day_open,
                                        "up": up, "ts": ts_ms}
            except Exception:
                pass

        ib.sleep(TICK_EVERY)


if __name__ == "__main__":
    if "--serve" in sys.argv:
        _serve = True
        i = sys.argv.index("--serve")
        port = int(sys.argv[i+1]) if i+1 < len(sys.argv) and sys.argv[i+1].isdigit() else SERVE_PORT
        threading.Thread(target=_serve_thread, args=(port,), daemon=True).start()
    threading.Thread(target=_http_thread, args=(SCANNER_HTTP_PORT,), daemon=True).start()  # scanner web page
    try:
        main()
    except KeyboardInterrupt:
        pass

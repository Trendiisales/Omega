#!/usr/bin/env python3
"""
bigcap_feed_bridge (IBKR) — real-time feed for g_bigcap_momo (OMEGA_BIGCAP_BRIDGE=1).

Swapped from Yahoo polling to IBKR ib_async (real-time, no ~15min delay) — same
machinery as pump_feed_bridge.py, but a BIG-CAP universe + 5m bars:
  * IBKR scanner TOP_PERC_GAIN, filtered to non-penny (abovePrice>=10) liquid
    LARGE/MID-cap (marketCapAbove) names already up intraday.
  * subscribe each mover: seed today's 5m history, then stream live ticks ->
    roll 5m bars + price + day-mover candidate.
  * emit the SAME B/S/P/C protocol the PumpFeedConsumer reads (port 7784).

Validated edge (bigcap_scalp_sweep.py): scalp only names already up >= 5% on the
session (engine day_gate_pct), ride continuation, 3% trail. This bridge surfaces
the candidates; the engine applies the 5% gate + 3% trail + $20B large/mega-cap floor.

Run (on the VPS, IB Gateway/TWS up):  python3 pump/bigcap_feed_bridge.py --serve
Dev without IBKR: use bigcap_feed_bridge_yahoo.py instead.
"""
import sys, time, socket, threading, json, os
# ── visibility: this bridge runs under pythonw (no console) via ScheduledTask
#    OmegaBigCapBridge, so sys.stdout/stderr are None and every diagnostic was
#    silently dropped -- that is why "no stderr" + the zero-trades gap could not
#    be explained. Tee ALL output to a real logfile (line-buffered). (2026-06-14)
_LOG_PATH = os.environ.get("OMEGA_BIGCAP_LOG", r"C:\Omega\logs\bigcap_bridge.log")
try:
    _logf = open(_LOG_PATH, "a", buffering=1, encoding="utf-8")
    sys.stdout = _logf
    sys.stderr = _logf
except Exception:
    pass
from http.server import BaseHTTPRequestHandler, HTTPServer
from ib_async import IB, Stock, ScannerSubscription

# ── config (big-cap) ─────────────────────────────────────────────────────────
IB_HOST = "127.0.0.1"
IB_PORT = int(os.environ.get("OMEGA_IBKR_PORT", "4001"))  # 4001 = live gateway default (2026-07-18 fresh LIVE login reverted the 2026-06-30 4002 move). env-overridable.
IB_CID  = 34                                              # clientId distinct from pump(33)
SERVE_PORT    = int(os.environ.get("OMEGA_BIGCAP_BRIDGE_PORT", "7784"))
PREFILTER_PCT = 3.0          # subscribe names already >=3% up (engine gates at 5%)
MARKETCAP_MIN = 20000.0      # S-2026-06-23 $500B->$20B (=20000 musd) for BigCapMomoCons (bridge engine):
                             # deep faithful sweep (bigcap_momo_faithful.cpp, 5m 60d, 509-name) — the $500B
                             # floor was near-dormant (megas too calm to clear gate); the $20-50B band carries
                             # the momentum (2026 semis AMD/INTC/LRCX/AMAT now >=$500B + the $20-300B movers).
                             # Robust plateau $20-100B both-halves+ & survives +30bps. The breadth>=2 gate
                             # (g_bigcap_momo.min_breadth) handles the chop/bear the wider universe admits.
                             # NOTE (S-2026-06-24 corrected): the in-process IBKR engine is ALSO $20B now --
                             # engine_init.hpp:4166 bc.market_cap_above_musd=20000 + BigCapMomoIbkr.hpp class
                             # default=20000. Both engines aligned at $20B. (Old comment claimed the IBKR
                             # engine "keeps $500B" -- stale/wrong; it was actually $100M default until the
                             # 06-24 hardening, which is how the AEHR/SHAZ/PBLS small-caps leaked in pre-fix.)
                             # S-2026-06-20c (prior): $500B mega-cap-only (now superseded -> $20B both engines).
                             # big/mid-cap only -- UNITS = MILLIONS USD (TWS scanner
                             # convention). 2000 = $2B. BUG 2026-06-13: was 2.0e9 =
                             # "$2 quadrillion" -> scanner returned 0 rows since ship.
PRICE_MIN     = 10.0         # not a penny stock
MAX_SYMBOLS   = 45   # S-2026-07-10: 30->45 to cover the full ladder universe (BIGCAP_LAD). The
                     # tail 15 (incl WDC/STX/DD/TPR/BMY/SWKS) were dropped at 30 -> the in-binary
                     # daily-close writer never got their mids -> those columns froze. 45 IBKR L1
                     # lines is well within a standard paper allocation, but VERIFY at the next US
                     # open (13:30 UTC) that no IBKR line-limit error (Error 101/10197) appears in
                     # bigcap_bridge.log; if it does, tier the subscription instead of a flat 45.
TFS           = [300]        # 5m bars only (engine tf_sec=300)
SCAN_EVERY    = 30           # seconds between scanner passes
# tick/roll cadence = the EXIT-reaction clock. The engine's trailing/hard stop is
# checked on every P line (on_price), so this poll interval IS the giveback-control
# latency. 5s was too slow for protecting profit on a fast mover. 2s default; with
# real-time data (OMEGA_BIGCAP_MKTDATA=1) this gives ~2s exit reaction. NOTE: with
# delayed data (mdtype=3) the PRICE is ~15min stale regardless of poll speed -- the
# real near-real-time unlock is the live market-data entitlement, not this number.
TICK_EVERY    = float(os.environ.get("OMEGA_BIGCAP_TICK", "2"))

# ── scanner web page (2026-06-12: takes over :7783 from the retired micro-cap
#    pump scanner). Shows ONLY what the live engine can act on, using the SAME
#    gates engine_init ships for g_bigcap_momo: day-move >= GATE_PCT, price >=
#    PRICE_MIN, day $vol >= DVOL_MIN. status TRADE = all gates pass.
SCANNER_HTTP_PORT = int(os.environ.get("OMEGA_BIGCAP_SCANNER_PORT", "7783"))
GATE_PCT  = 4.0              # engine day_gate_pct (S-2026-06-13a: 5 -> 4)
# 2026-06-14: SYNC TO ENGINE. engine_init g_bigcap_momo.min_dvol_usd = 0 since the
# 06-13k fix (liquidity enforced upstream by the $2B-cap scanner, not a per-bar
# dvol gate). The page kept DVOL_MIN=$100M, so its TRADE/watch status used a
# STRICTER gate than the engine AND the delayed-feed dvol reads ~0 -> the page
# could NEVER show TRADE even on names the engine would act on. Set 0 to match.
DVOL_MIN  = float(os.environ.get("OMEGA_BIGCAP_DVOL_MIN", "0"))
# market-data type: 1=live (needs a funded IBKR real-time US-equity subscription),
# 3=delayed (paper default). Delayed = laggy prices + unreliable volume; flip to 1
# once your IBKR account's market-data entitlement is active. (2026-06-14)
MKT_DATA_TYPE = int(os.environ.get("OMEGA_BIGCAP_MKTDATA", "1"))  # S-2026-06-23 3->1: IBKR US-equity RT data sub now ACTIVE (NASDAQ+NYSE+Network B); delayed(3) no longer needed
_candidates = {}             # sym -> dict(sym,name,px,day_open,up,dvol,ts)
_names = {}                  # sym -> company longName (IBKR contractDetails)

# ── S-2026-06-25 STALE-DATA GUARDS (operator-mandated: the engine must NEVER act
#    on stale/frozen/delayed/non-RTH prices). A name is emitted (P/C) ONLY when its
#    tick is LIVE this pass; a non-live name is SUPPRESSED entirely so the consumer's
#    120s watchdog flattens any open pos and no new candidate can fire. Root cause this
#    closes: overnight the feed froze (tk.last stuck) yet the bridge kept emitting
#    "tradeable=24 top=DHI +18.8%" off a stale day_open -> garbage candidates.
STALE_TICK_MS = int(os.environ.get("OMEGA_BIGCAP_STALE_MS", "15000"))   # no IBKR tick update in >Xms = stale
REQUIRE_RTH   = os.environ.get("OMEGA_BIGCAP_REQUIRE_RTH", "1") == "1"  # only emit during US regular session
RTH_OPEN_MIN  = int(os.environ.get("OMEGA_BIGCAP_RTH_OPEN_MIN",  str(13*60+30)))  # 13:30 UTC = 09:30 ET
RTH_CLOSE_MIN = int(os.environ.get("OMEGA_BIGCAP_RTH_CLOSE_MIN", str(20*60)))     # 20:00 UTC = 16:00 ET
LOCK_PORT     = int(os.environ.get("OMEGA_BIGCAP_LOCK_PORT", "7785"))   # G1 singleton lock port
HEALTH_FLAG   = os.path.join(os.environ.get("OMEGA_LOG_DIR", r"C:\Omega\logs"), "bigcap_feed_unhealthy.flag")
_lock_sock = None

def _in_rth(now_ms):
    tm = time.gmtime(now_ms/1000.0)
    if tm.tm_wday >= 5: return False                       # Sat/Sun: US equities closed
    mins = tm.tm_hour*60 + tm.tm_min
    return RTH_OPEN_MIN <= mins < RTH_CLOSE_MIN

def _tick_live(tk, now_ms):
    """True only if this ticker's data is live + fresh this pass. Returns (bool, reason)."""
    mdt = getattr(tk, "marketDataType", None)
    if mdt not in (None, 1):                               # 2=frozen 3=delayed 4=delayed-frozen
        return False, "mdtype%s" % mdt
    if REQUIRE_RTH and not _in_rth(now_ms):
        return False, "non-RTH"
    tkt = getattr(tk, "time", None)                        # ib_async: datetime of last update
    if tkt is not None:
        try:
            age = now_ms - int(tkt.timestamp()*1000)
            if age > STALE_TICK_MS:
                return False, "age%dms" % age
        except Exception:
            pass
    return True, "live"

def _acquire_singleton_lock():
    """G1: bind a lock port so a 2nd bridge instance can't fight over :7783/:7784.
    Only the real interpreter runs main(); a venv-stub parent never reaches here."""
    global _lock_sock
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(("127.0.0.1", LOCK_PORT))                   # NO SO_REUSEADDR -> a 2nd bind fails
        s.listen(1)
        _lock_sock = s                                     # held for process lifetime
        return True
    except OSError as e:
        print(f"[BIGCAP-FATAL] another bridge already holds lock :{LOCK_PORT} ({e}) "
              f"-- exiting to avoid a double feed", flush=True)
        return False

def _set_health(unhealthy, reason=""):
    try:
        if unhealthy:
            with open(HEALTH_FLAG, "w") as f:
                f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {reason}\n")
        elif os.path.exists(HEALTH_FLAG):
            os.remove(HEALTH_FLAG)
    except Exception:
        pass

# ── TCP server -> in-Omega PumpFeedConsumer (one consumer) ───────────────────
_conn=None; _lock=threading.Lock(); _need_reseed=False
_starve_hb=0   # consecutive heartbeats with consumer=N while names tradeable (S-2026-06-17 alert)
def emit(s):
    global _conn
    with _lock:
        if _conn:
            try: _conn.sendall((s+"\n").encode())
            except Exception:
                try: _conn.close()
                except Exception: pass
                _conn=None
        else:
            print(s, flush=True)

def _serve_thread(port):
    global _conn,_need_reseed
    srv=socket.socket(socket.AF_INET,socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
    srv.bind(("127.0.0.1",port)); srv.listen(1)
    print(f"# bigcap bridge serving 127.0.0.1:{port}", file=sys.stderr)
    while True:
        c,_=srv.accept()
        with _lock: _conn=c; _need_reseed=True
        print("# consumer connected -> reseed", file=sys.stderr)

class _ScanHandler(BaseHTTPRequestHandler):
    """BIGCAP MOMO scanner page -- replaces the retired micro-cap PUMP SCANNER
    on this port. Engine-faithful: gate logic mirrors g_bigcap_momo settings.
    LIQ column = day dollar-volume; the iceberg/absorption column lights up once
    the MGC-validated detector ships (per-symbol equity depth is subscribed only
    for the symbol in an active trade -- IBKR depth-line budget)."""
    def log_message(self, *a): pass

    def do_GET(self):
        rows = sorted(_candidates.values(), key=lambda c: -c["up"])
        if self.path.startswith("/api"):
            body = json.dumps(rows).encode(); ctype = "application/json"
        else:
            def gate(c):
                return c["up"] >= GATE_PCT and c["px"] >= PRICE_MIN and c["dvol"] >= DVOL_MIN
            def row(c):
                ok = gate(c)
                st = '<span style="color:#3ddc97">TRADE</span>' if ok else \
                     '<span style="color:#6b6b6b">watch</span>'
                liq = c["dvol"] / 1e6
                liq_s = (f'<span style="color:#3ddc97">{liq:,.0f}M</span>' if c["dvol"] >= DVOL_MIN
                         else f'<span style="color:#6b6b6b">{liq:,.0f}M</span>')
                nm = (c.get("name") or "").replace("&","&amp;").replace("<","&lt;").replace(">","&gt;")
                return (f"<tr><td style='color:#d8d8d8'>{c['sym']}</td>"
                        f"<td style='color:#9aa0a6'>{nm}</td>"
                        f"<td>{c['px']:.2f}</td>"
                        f"<td style='color:#e6a23c'>+{c['up']:.1f}%</td>"
                        f"<td>{c['day_open']:.2f}</td><td>{liq_s}</td><td>{st}</td></tr>")
            trs = "".join(row(c) for c in rows) or \
                  "<tr><td colspan=7 style='color:#6b6b6b'>no big-cap movers ≥ prefilter — scanning</td></tr>"
            n_trade = sum(1 for c in rows if gate(c))
            # NOTE: no %-formatting here -- the JS contains literal '%' (e.g.
            # +'%</td>') which blows up printf-style formatting and killed the
            # HTML route with a silent ValueError. Token replacement instead.
            script = (
                "<script>\n"
                "const GATE=@GATE@, PMIN=@PMIN@, DVMIN=@DVMIN@;\n"
                "async function rf(){try{\n"
                " const r=await fetch('/api',{cache:'no-store'}); const d=await r.json();\n"
                " d.sort((a,b)=>b.up-a.up);\n"
                " const esc=s=>String(s).replace(/[&<>]/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[m]));\n"
                " let h='';\n"
                " for(const c of d){\n"
                "  const ok=c.up>=GATE&&c.px>=PMIN&&c.dvol>=DVMIN;\n"
                "  const st=ok?'<span style=\"color:#3ddc97\">TRADE</span>':'<span style=\"color:#6b6b6b\">watch</span>';\n"
                "  const lm=(c.dvol/1e6).toLocaleString(undefined,{maximumFractionDigits:0});\n"
                "  const liq=c.dvol>=DVMIN?('<span style=\"color:#3ddc97\">'+lm+'M</span>'):('<span style=\"color:#6b6b6b\">'+lm+'M</span>');\n"
                "  h+='<tr><td style=\"color:#d8d8d8\">'+esc(c.sym)+'</td>'\n"
                "    +'<td style=\"color:#9aa0a6\">'+esc(c.name||'')+'</td>'\n"
                "    +'<td>'+c.px.toFixed(2)+'</td>'\n"
                "    +'<td style=\"color:#e6a23c\">+'+c.up.toFixed(1)+'%</td>'\n"
                "    +'<td>'+c.day_open.toFixed(2)+'</td><td>'+liq+'</td><td>'+st+'</td></tr>';\n"
                " }\n"
                " if(!h) h='<tr><td colspan=7 style=\"color:#6b6b6b\">no big-cap movers \\u2265 prefilter \\u2014 scanning</td></tr>';\n"
                " document.getElementById('rows').innerHTML=h;\n"
                " const ng=d.filter(c=>c.up>=GATE&&c.px>=PMIN&&c.dvol>=DVMIN).length;\n"
                " document.getElementById('hdr').textContent='BIGCAP MOMO \\u00b7 '+d.length+' movers \\u00b7 '"
                "+ng+' tradeable (\\u2265@GPCT@% + $@DVM@M liq)';\n"
                "}catch(e){}}\n"
                "setInterval(rf,5000); rf();\n"
                "</script>")
            script = (script.replace("@GATE@", f"{GATE_PCT:.1f}")
                            .replace("@PMIN@", f"{PRICE_MIN:.1f}")
                            .replace("@DVMIN@", f"{DVOL_MIN:.0f}")
                            .replace("@GPCT@", f"{GATE_PCT:.0f}")
                            .replace("@DVM@", f"{DVOL_MIN/1e6:.0f}"))
            body = (
                "<html><head><meta charset=utf-8><title>BIGCAP MOMO</title>"
                "<style>"
                "body{background:#0a0a0a;color:#c8c8c8;font:13px/1.5 'SF Mono',Menlo,monospace;margin:0;padding:18px}"
                ".panel{border:1px solid #1d1d1d;border-radius:6px;padding:14px 16px;max-width:920px}"
                ".brand{display:flex;align-items:center;gap:10px;margin-bottom:12px}"
                ".brand .t{color:#5fd3e0;letter-spacing:3px;font-weight:bold;font-size:14px}"
                ".brand .v{color:#3a5b62;letter-spacing:2px;font-size:10px;text-transform:uppercase}"
                ".hdr{color:#9aa0a6;letter-spacing:.5px;font-size:12px;margin-bottom:10px}"
                "table{border-collapse:collapse;width:100%}"
                "th{text-align:left;color:#5fd3e0;font-weight:normal;padding:4px 16px 8px 0;border-bottom:1px solid #1d1d1d}"
                "td{padding:5px 16px 5px 0;white-space:nowrap}"
                ".foot{color:#6b6b6b;font-size:11px;margin-top:12px}"
                "</style></head><body><div class=panel>"
                "<div class=brand><span class=t>OMEGA</span><span class=v>bigcap momo</span></div>"
                f"<div class=hdr id=hdr>BIGCAP MOMO &middot; {len(rows)} movers &middot; {n_trade} tradeable "
                f"(&ge;{GATE_PCT:.0f}% + ${DVOL_MIN/1e6:.0f}M liq)</div>"
                "<table><tr><th>symbol</th><th>name</th><th>price</th><th>up from open</th>"
                "<th>day open</th><th>liq ($vol)</th><th>status</th></tr>"
                f"<tbody id=rows>{trs}</tbody></table>"
                "<div class=foot>engine settings: gate &ge;4% from open &middot; price &ge;$10 &middot; "
                "no dvol gate (liquidity via $2B-cap scanner) &middot; no-TP runner, 5% trail off peak + 6% hard "
                "&middot; SHADOW (paper-record) &middot; shadow trades show in the dashboard RUNNING TRADES panel "
                "&middot; iceberg/absorption column arrives once the MGC-validated detector ships</div>"
                f"</div>{script}</body></html>").encode()
            ctype = "text/html; charset=utf-8"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)


def _http_thread(port):
    try:
        HTTPServer(("0.0.0.0", port), _ScanHandler).serve_forever()
    except Exception as e:
        sys.stderr.write(f"[bigcap_bridge] scanner http error on :{port}: {e}\n")


# ── per-symbol 5m bar roller (mirrors pump bridge Sub) ───────────────────────
class Sub:
    def __init__(self, ticker):
        self.ticker=ticker; self.day_open=0.0; self.last_vol=None
        self.bars={tf:None for tf in TFS}
        self.last_bar_dvol=0.0   # close*vol of last COMPLETED 5m bar (engine liq gate unit)
        self.stale_why=""        # S-2026-06-25: non-empty when this name was suppressed as stale this pass
    def roll(self, sym, px, vol, ts_ms):
        if px is None or px<=0 or px!=px: return
        emit(f"P,{sym},{px},{ts_ms}")
        dv=0.0
        if vol is not None and self.last_vol is not None and vol>=self.last_vol: dv=vol-self.last_vol
        if vol is not None: self.last_vol=vol
        for tf in TFS:
            bkt=(ts_ms//(tf*1000))*(tf*1000); b=self.bars[tf]
            if b is None: self.bars[tf]=[bkt,px,px,px,px,dv]
            elif bkt!=b[0]:
                emit(f"B,{sym},{tf},{b[1]},{b[2]},{b[3]},{b[4]},{b[5]},{b[0]}")
                self.last_bar_dvol=b[4]*b[5]   # SAME math as the engine's bar-close*vol liq gate
                self.bars[tf]=[bkt,px,px,px,px,dv]
            else:
                b[2]=max(b[2],px); b[3]=min(b[3],px); b[4]=px; b[5]+=dv

def fetch_seed(ib, sym):
    """seed today's 5m history; return (day_open, day_high, lines, contract) or None."""
    c=Stock(sym,"SMART","USD")
    try:
        q=ib.qualifyContracts(c)
        if q: c=q[0]
    except Exception: return None
    day_open=day_high=None; lines=[]
    for tf in TFS:
        try:
            bars=ib.reqHistoricalData(c, endDateTime="", durationStr="1 D",
                                      barSizeSetting="5 mins", whatToShow="TRADES",
                                      useRTH=False, formatDate=1)
        except Exception: continue
        for b in bars:
            ts=int(b.date.timestamp()*1000)
            lines.append(f"S,{sym},{tf},{b.open},{b.high},{b.low},{b.close},{b.volume},{ts}")
            if day_open is None and b.open>0: day_open=b.open
            day_high=b.high if day_high is None else max(day_high,b.high)
    return (day_open,day_high,lines,c) if day_open else None

def subscribe_symbol(ib, subs, sym, min_move):
    if sym in subs or len(subs)>=MAX_SYMBOLS: return False
    sd=fetch_seed(ib,sym)
    if not sd: return False
    d_open,d_high,lines,c=sd
    move=(d_high/d_open-1.0)*100.0 if d_open else 0.0
    if move<min_move: return False
    if sym not in _names:               # scanner didn't supply longName -> resolve directly
        try:
            cds=ib.reqContractDetails(c)
            nm=(cds[0].longName or "").strip() if cds else ""
            if nm: _names[sym]=nm
        except Exception: pass
    emit(f"R,{sym}")
    for ln in lines: emit(ln)
    tk=ib.reqMktData(c,"",False,False); ib.sleep(0.5)
    s=Sub(tk); s.day_open=d_open; subs[sym]=s
    emit(f"# subscribe {sym} day {d_open:.2f}->{d_high:.2f} up={move:.0f}%")
    return True

def main():
    if not _acquire_singleton_lock():       # G1: refuse to start a 2nd bridge (no :7783/:7784 port fight)
        sys.exit(1)
    threading.Thread(target=_serve_thread, args=(SERVE_PORT,), daemon=True).start()
    threading.Thread(target=_http_thread, args=(SCANNER_HTTP_PORT,), daemon=True).start()
    # S-2026-06-23: suppress the BENIGN "Error 162 ... scanner subscription cancelled" spam.
    # reqScannerData (every 30s) subscribes-then-cancels by design -> Error 162 each pass =
    # ~36MB/day of log noise (the scan still works). Drop ONLY that line; keep all real errors.
    import logging as _lg
    class _DropScanCancel(_lg.Filter):
        def filter(self, r):
            m=r.getMessage()
            return not ("162" in m and "scanner subscription cancelled" in m.lower())
    _lg.getLogger("ib_async").addFilter(_DropScanCancel())
    _lg.getLogger("ib_async.wrapper").addFilter(_DropScanCancel())
    ib=IB(); ib.connect(IB_HOST, IB_PORT, clientId=IB_CID, timeout=15)
    try: ib.reqMarketDataType(MKT_DATA_TYPE)   # 3=delayed(paper) 1=live(entitled)
    except Exception: pass
    subs={}; last_scan=0.0
    print(f"[BIGCAP-BRIDGE] up | ib={IB_HOST}:{IB_PORT} cid={IB_CID} "
          f"mdtype={MKT_DATA_TYPE} gate={GATE_PCT}% dvol_min={DVOL_MIN:.0f} "
          f"serve=:{SERVE_PORT} scanner=:{SCANNER_HTTP_PORT}", flush=True)
    emit("# bigcap_feed_bridge (IBKR) up")
    global _need_reseed
    while True:
        now=time.time()
        # consumer (re)connected -> replay today's history so Omega recovers day_open
        if _need_reseed and subs:
            _need_reseed=False
            for sym,s in list(subs.items()):
                sd=fetch_seed(ib,sym)
                if sd:
                    emit(f"R,{sym}")
                    for ln in sd[2]: emit(ln)
                    if sd[0]>0: s.day_open=sd[0]
        elif _need_reseed:
            _need_reseed=False
        # periodic scanner: big-cap top-gainers
        if now-last_scan>=SCAN_EVERY:
            last_scan=now
            try:
                sc=ScannerSubscription(instrument="STK", locationCode="STK.US.MAJOR",
                                       scanCode="TOP_PERC_GAIN")
                sc.abovePrice=PRICE_MIN
                sc.marketCapAbove=MARKETCAP_MIN        # BIG/MID-cap only (the not-penny filter)
                rows=ib.reqScannerData(sc)
                for item in rows[:40]:
                    if len(subs)>=MAX_SYMBOLS: break
                    cd=item.contractDetails
                    sym=str(cd.contract.symbol).upper()
                    nm=(getattr(cd,"longName","") or "").strip()
                    if nm: _names[sym]=nm
                    subscribe_symbol(ib, subs, sym, min_move=PREFILTER_PCT)
            except Exception as e:
                emit(f"# scan error: {e}")
            # heartbeat -> logfile: streaming health + why-no-trade visibility
            _top = max(_candidates.values(), key=lambda c: c["up"], default=None)
            _arm = sum(1 for c in _candidates.values()
                       if c["up"]>=GATE_PCT and c["px"]>=PRICE_MIN and c["dvol"]>=DVOL_MIN)
            _stale = sum(1 for s in subs.values() if getattr(s,"stale_why",""))   # G2: names suppressed as stale
            _rth   = _in_rth(int(now*1000))
            # health flag for preflight/monitor (G4): UNHEALTHY only if, DURING RTH, names are
            # subscribed but EVERY one is stale (= real feed death). Idle overnight is NOT unhealthy.
            _unhealthy = bool(_rth and len(subs)>0 and _stale==len(subs))
            _set_health(_unhealthy, f"RTH all-stale subs={len(subs)}")
            print(f"[HB {time.strftime('%H:%M:%S')}] subs={len(subs)} cand={len(_candidates)} "
                  f"tradeable={_arm} stale={_stale} rth={'Y' if _rth else 'N'} "
                  f"consumer={'Y' if _conn else 'N'} mdtype={MKT_DATA_TYPE} "
                  f"top={(_top['sym']+' +'+format(_top['up'],'.1f')+'%') if _top else '-'}",
                  flush=True)
            # S-2026-06-17: LOUD escalation — bridge streaming movers but NO Omega consumer
            #   attached while names are tradeable = the engine is starved (zero trades, silent).
            #   Greppable [BIGCAP-ALERT] + sentinel file the watchdog/operator can poll.
            global _starve_hb
            if (not _conn) and _arm > 0:
                _starve_hb += 1
                if _starve_hb >= 2:   # ~1 min of orphaned feed with live candidates
                    print(f"[BIGCAP-ALERT] consumer=N for {_starve_hb} HBs while tradeable={_arm} "
                          f"-- Omega NOT consuming :{SERVE_PORT}; check OMEGA_BIGCAP_BRIDGE=1 + single bridge proc",
                          flush=True)
                    try:
                        with open(os.path.join(os.environ.get("OMEGA_LOG_DIR","C:\\Omega\\logs"),
                                               "bigcap_consumer_down.flag"),"w") as _f:
                            _f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} consumer=N tradeable={_arm}\n")
                    except Exception: pass
            else:
                _starve_hb = 0
        # stream ticks + roll bars + candidate
        ts_ms=int(now*1000)
        for k in [k for k,v in _candidates.items() if ts_ms-v["ts"]>600_000]:
            _candidates.pop(k,None)   # prune names that stopped updating (10 min)
        for sym,s in list(subs.items()):
            tk=s.ticker
            px=tk.last or tk.marketPrice() or tk.close
            vol=getattr(tk,"volume",None)
            live, why = _tick_live(tk, ts_ms)            # G2: only feed LIVE+fresh+RTH ticks
            if not live:
                s.stale_why = why
                _candidates.pop(sym, None)               # stale -> drop candidate (no entry; consumer watchdog flattens)
                continue
            s.stale_why = ""
            try:
                s.roll(sym, float(px) if px else None, float(vol) if vol else None, ts_ms)
                if px and s.day_open:
                    up=(float(px)/s.day_open-1.0)*100.0
                    emit(f"C,{sym},{float(px)},{s.day_open},{up:.1f},{ts_ms}")
                    _candidates[sym]={"sym":sym,"name":_names.get(sym,""),
                                      "px":float(px),"day_open":s.day_open,
                                      "up":up,"dvol":s.last_bar_dvol,"ts":ts_ms}
            except Exception: pass
        ib.sleep(TICK_EVERY)

if __name__=="__main__":
    main()

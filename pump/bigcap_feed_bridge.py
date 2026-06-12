#!/usr/bin/env python3
"""
bigcap_feed_bridge — feeds g_bigcap_momo (OMEGA_BIGCAP_BRIDGE=1) the SAME B/S/P/C
protocol as pump_feed_bridge, but scans NAS/SPX big-caps for intraday DAY-MOVERS
(the validated bigcap_scalp_sweep edge: names already up >= DAY_GATE on the session,
liquid, then continuation-scalp with a 3% trail).

Source: Yahoo /v8 chart range=1d interval=5m per symbol (one call = day-open + 5m
bars + latest price). Same scan approach we validated with. Free Yahoo intraday is
~15min delayed -> fine for SHADOW observation; swap to IBKR for live speed later.

Protocol to the in-Omega consumer (TCP server, one consumer):
  C,SYM,px,day_open,up_pct,ts_ms     candidate (day-mover crossed the gate)
  B,SYM,TF_SEC,o,h,l,c,v,ts_ms       one CLOSED 5m bar
  P,SYM,px,ts_ms                     latest price tick (drives the trail exit)
Run:  OMEGA_BIGCAP_BRIDGE=1 in Omega; python3 pump/bigcap_feed_bridge.py
"""
import sys, time, json, socket, threading, urllib.request, concurrent.futures as cf

UNIVERSE_FILES = ["pump/bigcap_universe.txt", "bigcap_universe.txt", "/tmp/bigcap_universe.txt"]  # NAS100+SP500 (bundled); rebuilt by largecap_mover_scan.py
SERVE_PORT    = int(__import__("os").environ.get("OMEGA_BIGCAP_BRIDGE_PORT", "7784"))
TF_SEC        = 300          # 5m bars (matches engine tf_sec)
DAY_GATE      = 5.0          # candidate when up >= this% on the session (engine day_gate_pct)
PRICE_MIN     = 10.0
MIN_DVOL_USD  = 100e6        # liquid big/mid-cap
POLL_SEC      = 60           # min seconds between full universe passes
UA = {"User-Agent": "Mozilla/5.0"}

_conn=None; _lock=threading.Lock()
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
            print(s, flush=True)   # stdout when no consumer (debug)

def accept_loop(port):
    global _conn
    srv=socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port)); srv.listen(1)
    print(f"# bigcap bridge serving on 127.0.0.1:{port}", file=sys.stderr)
    while True:
        c,_=srv.accept()
        with _lock: _conn=c
        print("# consumer connected", file=sys.stderr)

def get(u,t=12): return urllib.request.urlopen(urllib.request.Request(u,headers=UA),timeout=t).read().decode("utf-8","replace")

def universe():
    for path in UNIVERSE_FILES:
        try:
            u=[s.strip() for s in open(path) if s.strip()]
            if u: return u
        except Exception: continue
    # fallback: a core liquid set so the bridge runs even without the scan file
    return "AAPL MSFT NVDA AMZN GOOGL META TSLA AMD AVGO NFLX CRM ADBE INTC MU PLTR".split()

def scan_symbol(sym):
    """one chart call -> (candidate_line, [bar_lines], price_line) or None if not a mover."""
    try:
        j=json.loads(get(f"https://query1.finance.yahoo.com/v8/finance/chart/{sym}?range=1d&interval=5m"))
        res=j["chart"]["result"][0]; meta=res["meta"]; ts=res["timestamp"]; q=res["indicators"]["quote"][0]
        o,h,l,c,v=q["open"],q["high"],q["low"],q["close"],q["volume"]
    except Exception: return None
    # session open = first valid bar open
    day_open=None
    for i in range(len(ts)):
        if o[i] is not None and o[i]>0: day_open=o[i]; break
    if not day_open: return None
    px=meta.get("regularMarketPrice") or c[-1]
    if px is None or px<PRICE_MIN: return None
    up=(px/day_open-1)*100
    if up<DAY_GATE: return None                      # not a day-mover -> skip
    # liquidity: cumulative $-volume today
    dvol=sum((c[i] or 0)*(v[i] or 0) for i in range(len(ts)))
    if dvol<MIN_DVOL_USD: return None
    now_ms=int(ts[-1])*1000
    cand=f"C,{sym},{px:.4f},{day_open:.4f},{up:.1f},{now_ms}"
    bars=[]
    for i in range(len(ts)):
        if None in (o[i],h[i],l[i],c[i],v[i]): continue
        bars.append(f"B,{sym},{TF_SEC},{o[i]:.4f},{h[i]:.4f},{l[i]:.4f},{c[i]:.4f},{v[i]:.0f},{int(ts[i])*1000}")
    price=f"P,{sym},{px:.4f},{now_ms}"
    return (cand,bars,price)

def main():
    threading.Thread(target=accept_loop, args=(SERVE_PORT,), daemon=True).start()
    syms=universe()
    print(f"# bigcap bridge: {len(syms)} NAS/SPX syms, gate {DAY_GATE}% price>={PRICE_MIN} $vol>={MIN_DVOL_USD/1e6:.0f}M tf={TF_SEC}s", file=sys.stderr)
    while True:
        t0=time.time(); movers=0
        with cf.ThreadPoolExecutor(max_workers=12) as ex:
            for r in ex.map(scan_symbol, syms):
                if not r: continue
                movers+=1
                cand,bars,price=r
                emit(cand)
                for b in bars[-30:]: emit(b)   # recent bars (engine warms its own window)
                emit(price)
        emit(f"# pass done movers={movers} in {time.time()-t0:.0f}s")
        dt=POLL_SEC-(time.time()-t0)
        if dt>0: time.sleep(dt)

if __name__=="__main__":
    main()

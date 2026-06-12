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
the candidates; the engine applies the 5% gate + 3% trail + $100M liquidity floor.

Run (on the VPS, IB Gateway/TWS up):  python3 pump/bigcap_feed_bridge.py --serve
Dev without IBKR: use bigcap_feed_bridge_yahoo.py instead.
"""
import sys, time, socket, threading, json, os
from ib_async import IB, Stock, ScannerSubscription

# ── config (big-cap) ─────────────────────────────────────────────────────────
IB_HOST, IB_PORT, IB_CID = "127.0.0.1", 4002, 34   # paper gateway; clientId distinct from pump(33)
SERVE_PORT    = int(os.environ.get("OMEGA_BIGCAP_BRIDGE_PORT", "7784"))
PREFILTER_PCT = 3.0          # subscribe names already >=3% up (engine gates at 5%)
MARKETCAP_MIN = 2.0e9        # big/mid-cap only (scanner filter)
PRICE_MIN     = 10.0         # not a penny stock
MAX_SYMBOLS   = 30
TFS           = [300]        # 5m bars only (engine tf_sec=300)
SCAN_EVERY    = 30           # seconds between scanner passes
TICK_EVERY    = 5            # seconds between tick/roll passes

# ── TCP server -> in-Omega PumpFeedConsumer (one consumer) ───────────────────
_conn=None; _lock=threading.Lock(); _need_reseed=False
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

# ── per-symbol 5m bar roller (mirrors pump bridge Sub) ───────────────────────
class Sub:
    def __init__(self, ticker):
        self.ticker=ticker; self.day_open=0.0; self.last_vol=None
        self.bars={tf:None for tf in TFS}
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
    emit(f"R,{sym}")
    for ln in lines: emit(ln)
    tk=ib.reqMktData(c,"",False,False); ib.sleep(0.5)
    s=Sub(tk); s.day_open=d_open; subs[sym]=s
    emit(f"# subscribe {sym} day {d_open:.2f}->{d_high:.2f} up={move:.0f}%")
    return True

def main():
    threading.Thread(target=_serve_thread, args=(SERVE_PORT,), daemon=True).start()
    ib=IB(); ib.connect(IB_HOST, IB_PORT, clientId=IB_CID, timeout=15)
    try: ib.reqMarketDataType(3)   # delayed ok in paper without live entitlement
    except Exception: pass
    subs={}; last_scan=0.0
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
                    sym=str(item.contractDetails.contract.symbol).upper()
                    subscribe_symbol(ib, subs, sym, min_move=PREFILTER_PCT)
            except Exception as e:
                emit(f"# scan error: {e}")
        # stream ticks + roll bars + candidate
        ts_ms=int(now*1000)
        for sym,s in list(subs.items()):
            tk=s.ticker
            px=tk.last or tk.marketPrice() or tk.close
            vol=getattr(tk,"volume",None)
            try:
                s.roll(sym, float(px) if px else None, float(vol) if vol else None, ts_ms)
                if px and s.day_open:
                    up=(float(px)/s.day_open-1.0)*100.0
                    emit(f"C,{sym},{float(px)},{s.day_open},{up:.1f},{ts_ms}")
            except Exception: pass
        ib.sleep(TICK_EVERY)

if __name__=="__main__":
    main()

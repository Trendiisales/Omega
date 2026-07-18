#!/usr/bin/env python3
"""FIX #1 (reliable): refresh sp500_long_close.csv from IBKR instead of throttled yfinance.

Root cause it fixes: the yfinance pull in blend_daily.sh is chronically throttled (~75%/attempt),
so ~230 of 536 names never refresh and their columns froze at the 2024 build -> the GUI showed
"-" (stale guard) and the basket lost half its prices. IBKR (reqHistoricalData) has no such throttle.

PROHIBITED against the PRODUCTION exec gateway (operator rule 2026-07-14: a bulk
historical pull through omega-new:4002 killed the gateway -- live orders dead).
Point it ONLY at a data-only IBKR session (separate gateway/TWS login), and set
RDA_IBKR_DATA_ONLY=1 to attest that. Without the env var the script refuses to run.
    RDA_IBKR_DATA_ONLY=1 python3 refresh_close_ibkr.py --tickers bigcap
    RDA_IBKR_DATA_ONLY=1 python3 refresh_close_ibkr.py --tickers full   # overnight, IBKR pacing
Merges (extend, not replace) into sp500_long_close.csv. READ-ONLY on IBKR (no orders).

S-2026-07-14q: pull() rewritten -- the old version used ONE reqId (9000) for every
request, never cancelled on timeout, and appended every historicalData callback
regardless of reqId. A timed-out symbol kept streaming bars into the NEXT symbol's
buffer: ADBE's column became CRM's series for 251 rows (CRM precedes ADBE in BIGCAP)
and the GUI booked/marked ADBE at CRM's price. Now: unique reqId per request,
callbacks filtered by reqId, cancelHistoricalData on timeout, and close_csv_guard
refuses to write a frame with aliased columns.
"""
import sys, os, time, argparse, datetime as dt
if os.environ.get('RDA_IBKR_DATA_ONLY')!='1':
    print("REFUSING: bulk historical pulls through the PRODUCTION exec gateway are "
          "prohibited (operator rule 2026-07-14 -- one killed omega-new:4002, live orders dead).\n"
          "Point at a DATA-ONLY IBKR session and set RDA_IBKR_DATA_ONLY=1 to attest.")
    sys.exit(4)
import pandas as pd
from ibapi.client import EClient
from ibapi.wrapper import EWrapper
from ibapi.contract import Contract

DATA = os.path.expanduser("~/Omega/data/rdagent")
CLOSE = f"{DATA}/sp500_long_close.csv"
# S-2026-07-10: 39 -> 45 to match the ladder universe (engine_init BIGCAP_LAD). The final
# 6 (WDC STX DD TPR BMY SWKS) were added to the engine on 07-08c but never to this producer,
# so they shipped with bars=0 and could never trade -- caught by the new content-based
# stock guard (feeds_selftest.py vps_stock_book_health). This historical-pull producer
# (IBKR reqHistoricalData, NOT streaming) has no market-data-line limit, so it can cover
# all 45 where the live bridge (MAX_SYMBOLS=30) drops the tail.
BIGCAP = "NVDA AMD AVGO MU MRVL SMCI ARM PLTR TSLA META NFLX CRWD SHOP COIN MSTR SNOW NOW PANW UBER ABNB DELL ORCL QCOM INTC AMZN GOOGL MSFT AAPL CRM ADBE IONQ RGTI QBTS ASTS RKLB NBIS CRWV ALAB CRDO WDC STX DD TPR BMY SWKS".split()

class App(EWrapper, EClient):
    def __init__(self):
        EClient.__init__(self, self)
        self.ready=False; self.bars=[]; self.done=False; self.err=False
        self.cur_req=-1   # S-2026-07-14q: only callbacks for THIS reqId are accepted
    def error(self, reqId, code, msg, *a):
        if code in (162,200,354,420):
            if reqId==self.cur_req: self.err=True
        elif code not in (2104,2106,2158,2107,2103,2100,2150,2174,165,2119,366,2108): print('IBKR err',code,str(msg)[:50])
    def nextValidId(self, oid): self.ready=True
    def historicalData(self, reqId, bar):
        if reqId==self.cur_req: self.bars.append(bar)   # a late prior request can no longer bleed in
    def historicalDataEnd(self, reqId, s, e):
        if reqId==self.cur_req: self.done=True

_req_seq=[9000]
def pull(app, sym, dur):
    c=Contract(); c.symbol=sym; c.secType='STK'; c.exchange='SMART'; c.currency='USD'
    _req_seq[0]+=1; rid=_req_seq[0]                       # unique reqId per request
    app.bars=[]; app.done=False; app.err=False; app.cur_req=rid
    app.reqHistoricalData(rid, c, '', dur, '1 day', 'ADJUSTED_LAST', 1, 1, False, [])
    t0=time.time()
    while not app.done and not app.err and time.time()-t0<18: time.sleep(0.1)
    if not app.done:
        # timeout/err: cancel so the stream can't keep delivering, and stop accepting rid
        try: app.cancelHistoricalData(rid)
        except Exception: pass
        print(f"  {sym}: TIMEOUT/err -- cancelled req {rid}, column skipped this run")
    app.cur_req=-1
    out={}
    for b in app.bars:
        d=str(b.date)[:10]
        if '-' not in d: d=f"{d[:4]}-{d[4:6]}-{d[6:8]}"
        out[d]=b.close
    return out if app.done else {}

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--tickers',default='bigcap'); ap.add_argument('--port',type=int,default=4001)
    ap.add_argument('--cid',type=int,default=1402); ap.add_argument('--dur',default='1 Y'); a=ap.parse_args()
    if a.tickers=='full' and os.path.exists(f"{DATA}/sp500_tickers.txt"):
        syms=open(f"{DATA}/sp500_tickers.txt").read().split()
    else:
        syms=BIGCAP
    app=App(); app.connect('127.0.0.1', a.port, clientId=a.cid)
    import threading; threading.Thread(target=app.run, daemon=True).start()
    t0=time.time()
    while not app.ready and time.time()-t0<15: time.sleep(0.1)
    if not app.ready: print("no IBKR handshake — is the gateway up + tunnel open?"); return 1
    cols={}; ok=0
    for i,s in enumerate(syms):
        d=pull(app, s, a.dur)
        if d: cols[s]=pd.Series(d); ok+=1
        if (i+1)%20==0: print(f"  {i+1}/{len(syms)} pulled ({ok} ok)", flush=True)
        time.sleep(11)   # IBKR pacing: ~54 req/10min
    app.disconnect()
    if not cols: print("nothing pulled"); return 1
    new=pd.DataFrame(cols); new.index=pd.to_datetime(new.index); new=new.sort_index()
    if os.path.exists(CLOSE):
        old=pd.read_csv(CLOSE, index_col=0)
        old.index=pd.to_datetime(old.index)
        # BUG FIX S-2026-07-07: the previous merge assigned new[c] into a frame
        # still on OLD's index -- pandas reindexes the series on assignment, so
        # dates newer than the file NEVER entered and the CSV froze at its last
        # row (06-29) while every pull "succeeded". Union the indices FIRST,
        # then let fresh IBKR values win on overlap.
        merged=old.reindex(old.index.union(new.index)).sort_index()
        for c in new.columns:
            merged[c]=new[c].combine_first(merged[c]) if c in merged.columns else new[c]
        out=merged
    else:
        out=new
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from close_csv_guard import assert_no_aliased_columns
    assert_no_aliased_columns(out, " refresh_close_ibkr")   # S-2026-07-14q: abort, don't write aliased columns
    out.to_csv(CLOSE, date_format="%Y-%m-%d")
    print(f"refreshed {ok}/{len(syms)} names from IBKR -> {CLOSE} (through {out.index.max().date()})")
    return 0

if __name__=='__main__':
    sys.exit(main())

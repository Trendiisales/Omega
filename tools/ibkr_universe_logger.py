#!/usr/bin/env python3
"""Daily forward point-in-time universe logger (read-only).

Fixes the survivorship gap: the IBKR scanner only scans NOW, so backfilling
today's top-N history is survivorship-biased. Running THIS daily (cron) appends
the day's high-ADR scan-union to a dated log -> a true point-in-time universe
accumulates going forward, bias-free, for honest forward backtests + the live
watchlist. Read-only: reqScannerSubscription only, unique clientId, no orders.

Cron (NZ example): run ~07:00 ET after US open settles, via the gateway tunnel.
Output: data/universe_log.csv  (date,scanCode,rank,symbol)  -- append-only.
"""
import sys, time, os, datetime as dt
from ibapi.client import EClient
from ibapi.wrapper import EWrapper
from ibapi.scanner import ScannerSubscription

OUT = os.environ.get('UNIVERSE_LOG', '/Users/jo/Omega/data/universe_log.csv')
HOST = os.environ.get('IB_HOST', '127.0.0.1'); PORT = int(os.environ.get('IB_PORT', '4002'))
CID  = int(os.environ.get('IB_CID', '1390'))
CODES = ['TOP_PERC_GAIN','TOP_PERC_LOSE','MOST_ACTIVE','HOT_BY_VOLUME',
         'HIGH_OPT_IMP_VOLAT','HIGH_OPT_IMP_VOLAT_OVER_HIST','TOP_TRADE_COUNT']
MCAP_MUSD = float(os.environ.get('IB_MCAP_MUSD', '20000'))   # $20B floor (validated)

class App(EWrapper, EClient):
    def __init__(self): EClient.__init__(self, self); self.ready=False; self.rows=[]; self.done=False
    def error(self, reqId, code, msg, *a):
        if code not in (2104,2106,2158,2107,2103,2100,2150,2174,162,165,321): print('ERR',code,msg)
    def nextValidId(self, oid): self.ready=True
    def scannerData(self, reqId, rank, cd, *a): self.rows.append((rank, cd.contract.symbol))
    def scannerDataEnd(self, reqId): self.done=True

def main():
    app = App(); app.connect(HOST, PORT, clientId=CID)
    import threading; threading.Thread(target=app.run, daemon=True).start()
    t0=time.time()
    while not app.ready and time.time()-t0<15: time.sleep(0.1)
    if not app.ready: print('no handshake — is the gateway up + tunnel open?'); return 1
    today = dt.datetime.now(dt.timezone.utc).date().isoformat()  # UTC: Mac local NZ (+12) stamps rows a day ahead of the US session
    newfile = not os.path.exists(OUT)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    rows=[]
    for i,code in enumerate(CODES):
        app.rows=[]; app.done=False
        sub=ScannerSubscription(); sub.instrument='STK'; sub.locationCode='STK.US.MAJOR'
        sub.scanCode=code; sub.abovePrice=10; sub.marketCapAbove=MCAP_MUSD; sub.numberOfRows=50
        app.reqScannerSubscription(i+1, sub, [], [])
        t0=time.time()
        while not app.done and time.time()-t0<12: time.sleep(0.1)
        app.cancelScannerSubscription(i+1)
        for rank,sym in app.rows: rows.append((today, code, rank, sym))
        time.sleep(1)
    with open(OUT, 'a') as f:
        if newfile: f.write('date,scanCode,rank,symbol\n')
        for r in rows: f.write(','.join(str(x) for x in r)+'\n')
    uniq=len(set(s for *_,s in rows))
    print(f'{today}: logged {len(rows)} rows, {uniq} unique symbols -> {OUT}')
    app.disconnect(); return 0

if __name__=='__main__':
    sys.exit(main())

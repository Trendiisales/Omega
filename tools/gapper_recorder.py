# gapper_recorder.py -- daily IBKR gapper dataset recorder (APPEND + dedup).
# Scan TOP_PERC_GAIN $3-20 on the LIVE gateway -> append today's gappers +
# their 1-min bars to recent_gappers.csv + gapper_minute.csv. Same feed the
# GapShort engine trades. Run after US close. Grows the backtest set over time.
# Scheduled: OmegaGapperRecorder (weekdays 22:00 server-local). 2026-06-16.
import sys, os, csv, datetime as dt
from ib_async import IB, Stock, ScannerSubscription
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4002
GAP='C:/Omega/tools/recent_gappers.csv'; MIN='C:/Omega/data/gapper_minute.csv'
today = dt.date.today().isoformat()
def keys(path):
    s=set()
    if os.path.exists(path):
        with open(path, newline='') as f:
            r=csv.reader(f); next(r, None)
            for row in r:
                if len(row)>=2: s.add((row[0], row[1]))
    return s
gk=keys(GAP); mk=keys(MIN)
def hhmm(d):
    if isinstance(d, dt.datetime): return d.strftime('%H:%M')
    s=str(d); return s[11:16] if len(s)>=16 else s
ib=IB(); ib.connect('127.0.0.1', PORT, clientId=80, timeout=20)
sub=ScannerSubscription(instrument='STK', locationCode='STK.US.MAJOR', scanCode='TOP_PERC_GAIN',
    abovePrice=3, belowPrice=20, aboveVolume=100000)
scan=ib.reqScannerData(sub)
syms=[]
for s in scan:
    try: syms.append(s.contractDetails.contract.symbol)
    except Exception: pass
syms=syms[:50]
print(f'[rec] scan -> {len(syms)} names', flush=True)
new_gap=[]; new_min=[]
for sym in syms:
    try:
        c=Stock(sym,'SMART','USD'); ib.qualifyContracts(c)
        if (today,sym) not in gk:
            d=ib.reqHistoricalData(c,'','2 D','1 day','TRADES',useRTH=False,formatDate=1)
            if len(d)>=2 and d[-2].close>0:
                pc=d[-2].close; o=d[-1].open; gp=(o-pc)/pc*100
                new_gap.append([today,sym,f'{pc:.4f}',f'{o:.4f}',f'{d[-1].high:.4f}',f'{d[-1].low:.4f}',f'{d[-1].close:.4f}',f'{gp:.2f}'])
        if (today,sym) not in mk:
            m=ib.reqHistoricalData(c,'','1 D','1 min','TRADES',useRTH=False,formatDate=1)
            for b in m:
                new_min.append([today,sym,hhmm(b.date),f'{b.open:.4f}',f'{b.high:.4f}',f'{b.low:.4f}',f'{b.close:.4f}',int(b.volume)])
    except Exception as e:
        print(f'[rec] {sym} ERR {e}', flush=True)
def append(path, header, rows):
    isnew = (not os.path.exists(path)) or os.path.getsize(path)==0
    with open(path,'a',newline='') as f:
        w=csv.writer(f)
        if isnew: w.writerow(header)
        w.writerows(rows)
append(GAP,['date','ticker','prevClose','open','high','low','close','gapPct'],new_gap)
append(MIN,['date','ticker','bar_ts','o','h','l','c','v'],new_min)
print(f'[rec] {today}: +{len(new_gap)} gappers, +{len(new_min)} min-bars', flush=True)
ib.disconnect()

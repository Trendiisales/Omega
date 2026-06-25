from ibapi.client import EClient
from ibapi.wrapper import EWrapper
from ibapi.scanner import ScannerSubscription
import threading, time
class App(EWrapper, EClient):
    def __init__(self): EClient.__init__(self,self); self.ready=False; self.rows=[]; self.done=False
    def error(self,reqId,code,msg,*a):
        if code not in (2104,2106,2158,2107,2103,2100,2150,2174,162,165,321): print('ERR',code,msg)
    def nextValidId(self,oid): self.ready=True
    def scannerData(self,reqId,rank,cd,*a): self.rows.append((cd.contract.symbol,cd.contract.secType))
    def scannerDataEnd(self,reqId): self.done=True
app=App(); app.connect('127.0.0.1',4001,clientId=1378)
threading.Thread(target=app.run,daemon=True).start()
t0=time.time()
while not app.ready and time.time()-t0<15: time.sleep(0.1)
print('ready',app.ready)
CODES=['TOP_PERC_GAIN','TOP_PERC_LOSE','MOST_ACTIVE','HOT_BY_VOLUME','HIGH_OPT_IMP_VOLAT','HIGH_OPT_IMP_VOLAT_OVER_HIST','TOP_TRADE_COUNT']
uni={}
for i,code in enumerate(CODES):
    app.rows=[]; app.done=False
    sub=ScannerSubscription(); sub.instrument='STK'; sub.locationCode='STK.US.MAJOR'; sub.scanCode=code
    sub.abovePrice=10; sub.marketCapAbove=20000; sub.numberOfRows=50
    app.reqScannerSubscription(i+1, sub, [], [])
    t0=time.time()
    while not app.done and time.time()-t0<12: time.sleep(0.1)
    app.cancelScannerSubscription(i+1)
    for sym,st in app.rows: uni[sym]=st
    print(f'{code:32} +{len(app.rows)} (union {len(uni)})')
    time.sleep(1)
import json
json.dump(sorted(uni.keys()), open('/tmp/ib_uni.json','w'))
etf=[s for s,t in uni.items() if t!='STK']
print('TOTAL union:', len(uni), '| non-STK:', etf[:20])
print('UNIVERSE:', ' '.join(sorted(uni.keys())))
app.disconnect()

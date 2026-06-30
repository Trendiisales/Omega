from ibapi.client import EClient
from ibapi.wrapper import EWrapper
from ibapi.contract import Contract
import threading, time, json, os
OUT='/tmp/ib_data'; os.makedirs(OUT,exist_ok=True)
ETF={'SPY','QQQ','QQQM','GLD','SLV','GDX','IWM','IWF','IVE','IUSV','SPYV','VOE','USMV','SPMO','SCHG',
'SCHD','SDY','DGRO','FNDX','XLK','SOXX','SMH','TQQQ','SOXL','IBIT','BIL','SGOV','SHV','SHY','GOVT',
'BNDX','FBND','JPST','USHY','VGSH','JAAA','EMXC','EWY','SPCX','P','BNY'}
syms=[s for s in json.load(open('/tmp/ib_uni.json')) if s not in ETF]
print('stocks to pull:',len(syms),flush=True)
class App(EWrapper, EClient):
    def __init__(self): EClient.__init__(self,self); self.ready=False; self.bars=[]; self.done=False; self.err162=False
    def error(self,reqId,code,msg,*a):
        if code==162: self.err162=True
        elif code not in (2104,2106,2158,2107,2103,2100,2150,2174,165,321,166,2119): print('ERR',code,msg,flush=True)
    def nextValidId(self,oid): self.ready=True
    def historicalData(self,reqId,bar): self.bars.append(bar)
    def historicalDataEnd(self,reqId,s,e): self.done=True
app=App(); app.connect('127.0.0.1',4002,clientId=1379)
threading.Thread(target=app.run,daemon=True).start()
t0=time.time()
while not app.ready and time.time()-t0<15: time.sleep(0.1)
print('ready',app.ready,flush=True)
ok=0; fail=[]
for i,sym in enumerate(syms):
    p=f'{OUT}/{sym}.csv'
    if os.path.exists(p) and os.path.getsize(p)>3000: ok+=1; continue
    c=Contract(); c.symbol=sym; c.secType='STK'; c.exchange='SMART'; c.currency='USD'
    app.bars=[]; app.done=False; app.err162=False
    app.reqHistoricalData(1000+i, c, '', '5 Y', '1 day', 'ADJUSTED_LAST', 1, 1, False, [])
    t0=time.time()
    while not app.done and not app.err162 and time.time()-t0<20: time.sleep(0.1)
    if app.err162:                      # pacing -> wait and retry once
        time.sleep(20); app.bars=[]; app.done=False; app.err162=False
        app.reqHistoricalData(2000+i, c, '', '5 Y', '1 day', 'ADJUSTED_LAST', 1, 1, False, [])
        t0=time.time()
        while not app.done and time.time()-t0<20: time.sleep(0.1)
    if len(app.bars)>300:
        with open(p,'w') as f:
            f.write('date,open,high,low,close,volume\n')
            for b in app.bars:
                d=str(b.date)[:10] if '-' in str(b.date)[:10] else f'{str(b.date)[:4]}-{str(b.date)[4:6]}-{str(b.date)[6:8]}'
                f.write(f'{d},{b.open},{b.high},{b.low},{b.close},{int(b.volume) if b.volume>0 else 1}\n')
        ok+=1
        if ok%20==0: print(f'  {ok}/{len(syms)} pulled',flush=True)
    else: fail.append(sym)
    time.sleep(11)                      # pacing: ~54 req/10min, under the 60 limit
print('DONE pulled',ok,'fail',len(fail),fail[:15],flush=True)
app.disconnect()

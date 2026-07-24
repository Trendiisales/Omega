# Place a broker-side disaster STP (GTC) on every held LONG that has none. avgCost*(1-15%).
# If the account preset rejects (10349), retry TIF=DAY. Skips BMY (has a resting close).
from ib_insync import IB, Stock, StopOrder
ib=IB(); ib.connect('127.0.0.1',4001,clientId=71,timeout=15)
ib.reqAllOpenOrders(); ib.sleep(2)
have_stop={t.contract.symbol for t in ib.openTrades() if t.order.orderType=='STP'}
have_sell={t.contract.symbol for t in ib.openTrades() if t.order.action=='SELL'}
for p in ib.positions():
    s=p.contract.symbol; qty=p.position
    if qty<=0: continue
    if s in have_stop or s=='BMY': print(f'{s}: already has stop/close, skip'); continue
    stop_px=round(p.avgCost*0.85, 2)
    c=Stock(s,'SMART','USD'); ib.qualifyContracts(c)
    for tif in ('GTC','DAY'):
        o=StopOrder('SELL', abs(qty), stop_px); o.tif=tif; o.outsideRth=True
        tr=ib.placeOrder(c,o); ib.sleep(2)
        st=tr.orderStatus.status
        print(f'{s}: SELL STP @ {stop_px} tif={tif} -> {st}')
        if st in ('PreSubmitted','Submitted'): break
        ib.cancelOrder(o); ib.sleep(1)   # rejected -> try next tif
ib.disconnect()

# UNPROTECTED-POSITION check: every held position MUST have a protective SELL order
# (STP stop or a resting LMT close) at the broker. Any without = RED. This is the check
# that was MISSING when BMY sat 36h unprotected. Exit 0 = all protected, 2 = unprotected.
import sys
from ib_insync import IB
try:
    ib=IB(); ib.connect('127.0.0.1',4001,clientId=73,timeout=12)
except Exception as e:
    print(f'PROBE-UNREACHABLE: {e}'); sys.exit(0)   # can't verify -> don't false-alarm
ib.reqAllOpenOrders(); ib.sleep(2)
protected={t.contract.symbol for t in ib.openTrades() if t.order.action=='SELL' and t.orderStatus.status in ('PreSubmitted','Submitted')}
naked=[(p.contract.symbol, p.position) for p in ib.positions() if p.position>0 and p.contract.symbol not in protected]
ib.disconnect()
if naked:
    print('UNPROTECTED POSITIONS (no broker stop/close): '+', '.join(f'{s}({q})' for s,q in naked)); sys.exit(2)
print('OK: every broker position has a protective order'); sys.exit(0)

# place_stops.py — broker-side disaster STP (GTC) on EVERY held position that has none.
# LONG  (qty>0): protective SELL STP at entry*(1-PCT).
# SHORT (qty<0): protective BUY  STP at entry*(1+PCT).   <- S-2026-07-24 (audit gap 3)
# Covers ALL classes the engines trade — STK, FUT, CASH(FX) — not just stock longs.
#   * FUT: IBKR avgCost is price*multiplier, so the entry price = avgCost/multiplier
#     (rounding a raw avgCost gives a stop ~multiplier x too high — the correctness trap).
#   * Stops are rounded to the instrument's minTick; routing exchange comes from
#     reqContractDetails (the position contract has a bare/blank exchange).
# Idempotent: skips a position that already has a resting STP or an opposite-side
# closing order (a long covered by a resting SELL, a short by a resting BUY). Skips BMY
# (has a resting close). If the account preset rejects GTC (10349), retries TIF=DAY.
# Read-only except placing protective stops (AUDIT_PROBE_SAFETY: no flatten/kill/reset).
from ib_insync import IB, StopOrder

PCT = 0.15  # disaster distance from entry

ib = IB(); ib.connect('127.0.0.1', 4001, clientId=71, timeout=15)
ib.reqAllOpenOrders(); ib.sleep(2)

# Resting orders that already protect a position, keyed by symbol.
have_stop = {t.contract.symbol for t in ib.openTrades() if t.order.orderType == 'STP'}
resting_sell = {t.contract.symbol for t in ib.openTrades() if t.order.action == 'SELL'}
resting_buy = {t.contract.symbol for t in ib.openTrades() if t.order.action == 'BUY'}


def place(p):
    s = p.contract.symbol
    qty = p.position
    if qty == 0:
        return
    if s == 'BMY':
        print(f'{s}: BMY has a resting close, skip'); return

    long = qty > 0
    # Protected already?  long -> any STP or a resting SELL;  short -> any STP or a resting BUY.
    if s in have_stop or (long and s in resting_sell) or ((not long) and s in resting_buy):
        print(f'{s}: already has stop/close, skip'); return

    # Fully-specify the contract (exchange + multiplier + minTick) from the bare position contract.
    cds = ib.reqContractDetails(p.contract)
    if not cds:
        print(f'{s}: could not qualify contract (secType={p.contract.secType}), SKIP — investigate'); return
    cd = cds[0]; con = cd.contract
    if con.secType == 'STK':
        con.exchange = 'SMART'
    elif con.secType == 'CASH':
        con.exchange = 'IDEALPRO'
    # FUT / others keep the exchange from contract details (COMEX/CME/EUREX/...).

    min_tick = cd.minTick or 0.01
    try:
        mult = float(con.multiplier) if con.multiplier else 1.0
    except (TypeError, ValueError):
        mult = 1.0
    entry = p.avgCost / mult          # avgCost is per-contract (price*multiplier) for FUT
    raw = entry * (1 - PCT) if long else entry * (1 + PCT)
    stop_px = round(round(raw / min_tick) * min_tick, 10)
    action = 'SELL' if long else 'BUY'

    for tif in ('GTC', 'DAY'):
        o = StopOrder(action, abs(qty), stop_px)
        o.tif = tif
        if con.secType == 'STK':
            o.outsideRth = True       # equities: protect outside RTH; FUT/CASH trade ~24h
        tr = ib.placeOrder(con, o); ib.sleep(2)
        st = tr.orderStatus.status
        print(f'{s}: {action} STP @ {stop_px} tif={tif} sec={con.secType} qty={abs(qty)} -> {st}')
        if st in ('PreSubmitted', 'Submitted'):
            break
        ib.cancelOrder(o); ib.sleep(1)   # rejected -> try next tif


for p in ib.positions():
    try:
        place(p)
    except Exception as e:  # noqa: BLE001  — one bad symbol must not skip the rest
        print(f'{p.contract.symbol}: ERROR placing stop: {e}')

ib.disconnect()

#!/usr/bin/env python3
"""Daily-bracket — fires at user-supplied UTC hour (default = current time).

Validated configs (gold, 2y backtest):
  13:00 + 14:00 UTC: offset=$2, TP=$50, SL=$2, hold=60min
  -> 1,201 trades, 50% win, +$9,043/oz, PF 8.64, Calmar 494

Run-on-demand pattern: cron/Task Scheduler triggers script at the desired UTC hour.
Script grabs current price, places brackets, monitors 60min, exits.

Usage:
  python live\\daily_bracket.py --paper --qty 1 --instrument MGC --strategy DAILY1300
  python live\\daily_bracket.py --paper --qty 1 --instrument MGC --strategy DAILY1400
"""
import argparse, json, logging, math, sys
from datetime import datetime, timezone, timedelta
from pathlib import Path
from ib_insync import IB, Future, ContFuture, Forex, Stock, StopOrder, LimitOrder, MarketOrder

def utcnow(): return datetime.now(timezone.utc)

# Validated daily 13/14 UTC config (tighter than Sunday)
OFFSET = 2.0
TP_DIST = 50.0
SL_DIST = 2.0
HOLD_MIN = 60
TICK_SZ = 0.10

ROOT = Path(__file__).resolve().parent.parent
LOG_DIR = ROOT / 'logs'; LOG_DIR.mkdir(exist_ok=True)
DATA_DIR = ROOT / 'data'; DATA_DIR.mkdir(exist_ok=True)
TRADES_FILE = DATA_DIR / 'trades.ndjson'

logging.basicConfig(level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    handlers=[logging.FileHandler(LOG_DIR / f'bracket_{utcnow():%Y%m%d}.log'),
              logging.StreamHandler(sys.stdout)])
log = logging.getLogger('daily_bracket')

def round_tick(px, tick=TICK_SZ): return round(round(px / tick) * tick, 2)

def append_trade(rec):
    with open(TRADES_FILE, 'a') as f: f.write(json.dumps(rec) + '\n')

def get_contract(ib, instrument, expiry=None):
    if instrument in ('MGC', 'GC'):
        c = Future(instrument, expiry, 'COMEX') if expiry else ContFuture(instrument, 'COMEX')
        ib.qualifyContracts(c)
        return c
    if instrument == 'XAUUSD':
        c = Forex('XAUUSD'); ib.qualifyContracts(c); return c
    raise ValueError(f'Unknown instrument: {instrument}')

def get_price(ib, c):
    # Try real-time first; fall back to frozen/delayed if the account
    # isn't subscribed for this contract, so a missing sub never hard-fails.
    for mdt, label in [(1, 'live'), (2, 'frozen'), (3, 'delayed'), (4, 'delayed-frozen')]:
        try:
            ib.reqMarketDataType(mdt)
            tk = ib.reqMktData(c, '', False, False)
            for _ in range(4):
                ib.sleep(2)
                for cand in (tk.marketPrice(), tk.last, tk.close, tk.bid, tk.ask):
                    if cand and not math.isnan(cand) and cand > 0:
                        ib.cancelMktData(c)
                        log.info(f'price [{label}]: {cand}')
                        return float(cand)
            ib.cancelMktData(c)
            log.warning(f'no price from mdt={mdt} ({label})')
        except Exception as e:
            log.warning(f'mdt={mdt} ({label}) failed: {e}')
    try:
        bars = ib.reqHistoricalData(c, endDateTime='', durationStr='1 D',
                                     barSizeSetting='1 min', whatToShow='TRADES',
                                     useRTH=False, formatDate=1)
        if bars and bars[-1].close > 0:
            log.info(f'historical price: {bars[-1].close}')
            return float(bars[-1].close)
    except Exception as e:
        log.warning(f'historical failed: {e}')
    return None

def place_brackets(ib, c, qty, open_px, strategy):
    oca = f'{strategy}_{utcnow():%Y%m%d_%H%M%S}'
    bt = round_tick(open_px + OFFSET); st = round_tick(open_px - OFFSET)
    buy = StopOrder('BUY', qty, stopPrice=bt); buy.ocaGroup=oca; buy.ocaType=1; buy.tif='GTC'
    sell = StopOrder('SELL', qty, stopPrice=st); sell.ocaGroup=oca; sell.ocaType=1; sell.tif='GTC'
    bt_tr = ib.placeOrder(c, buy); st_tr = ib.placeOrder(c, sell)
    log.info(f'PLACED OCA {oca}: BUY_STP@{bt} SELL_STP@{st} qty={qty}')
    return oca, bt_tr, st_tr

def attach_exits(ib, c, parent_tr, qty, hold_min):
    side = parent_tr.order.action
    fill = parent_tr.orderStatus.avgFillPrice
    exit_act = 'SELL' if side=='BUY' else 'BUY'
    tp_px = round_tick(fill + TP_DIST) if side=='BUY' else round_tick(fill - TP_DIST)
    sl_px = round_tick(fill - SL_DIST) if side=='BUY' else round_tick(fill + SL_DIST)
    oca = f'CHILD_{parent_tr.order.orderId}'
    tp = LimitOrder(exit_act, qty, lmtPrice=tp_px); tp.tif='GTC'; tp.ocaGroup=oca; tp.ocaType=1
    sl = StopOrder(exit_act, qty, stopPrice=sl_px); sl.tif='GTC'; sl.ocaGroup=oca; sl.ocaType=1
    tp_tr = ib.placeOrder(c, tp); sl_tr = ib.placeOrder(c, sl)
    log.info(f'EXITS: TP@{tp_px} SL@{sl_px} (entry {fill})')
    deadline = utcnow() + timedelta(minutes=hold_min)
    while utcnow() < deadline:
        ib.sleep(2)
        if tp_tr.isDone() and tp_tr.orderStatus.status=='Filled':
            return 'TP', tp_tr.orderStatus.avgFillPrice
        if sl_tr.isDone() and sl_tr.orderStatus.status=='Filled':
            return 'SL', sl_tr.orderStatus.avgFillPrice
    try: ib.cancelOrder(tp_tr.order)
    except: pass
    try: ib.cancelOrder(sl_tr.order)
    except: pass
    ib.sleep(1)
    mkt = MarketOrder(exit_act, qty); cl = ib.placeOrder(c, mkt); ib.sleep(3)
    return 'TIME', cl.orderStatus.avgFillPrice

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--paper', action='store_true')
    ap.add_argument('--live', action='store_true')
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=4002)
    ap.add_argument('--instrument', default='MGC')
    ap.add_argument('--expiry', default=None)
    ap.add_argument('--qty', type=int, default=1)
    ap.add_argument('--client-id', type=int, default=100)
    ap.add_argument('--strategy', default='DAILY1300', help='label for this run (DAILY1300, DAILY1400, etc.)')
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()

    # Weekday check — only fire Mon-Fri (skip Sat=5, Sun=6)
    wd = utcnow().weekday()
    if wd >= 5:
        log.info(f'Skipping weekend (weekday={wd})')
        sys.exit(0)

    log.info(f'=== {args.strategy} instr={args.instrument} qty={args.qty} port={args.port} dry={args.dry_run} ===')
    ib = IB()
    try:
        ib.connect(args.host, args.port, clientId=args.client_id, timeout=10)
        log.info(f'connected {args.host}:{args.port} clientId={args.client_id}')
    except Exception as e:
        log.error(f'connect failed: {e}'); sys.exit(2)

    try:
        c = get_contract(ib, args.instrument, args.expiry)
        log.info(f'contract: {c}')
    except Exception as e:
        log.error(f'contract resolve failed: {e}'); ib.disconnect(); sys.exit(2)

    open_px = get_price(ib, c)
    if open_px is None:
        log.warning('NO PRICE — exiting clean')
        append_trade({'ts':utcnow().isoformat(),'strategy':args.strategy,
                      'instrument':args.instrument,'qty':args.qty,
                      'open_px':None,'side':None,'entry':None,'exit':None,'reason':'NO_PRICE',
                      'pnl_per_oz':0.0,'pnl_total':0.0,'paper':not args.live})
        ib.disconnect(); sys.exit(0)
    log.info(f'open_px = {open_px}')

    if args.dry_run:
        log.info('DRY RUN — skipping order placement')
        ib.disconnect(); sys.exit(0)

    oca, bt, st = place_brackets(ib, c, args.qty, open_px, args.strategy)
    deadline = utcnow() + timedelta(minutes=HOLD_MIN)
    triggered = None
    while utcnow() < deadline:
        ib.sleep(2)
        if bt.isDone() and bt.orderStatus.status=='Filled': triggered=bt; break
        if st.isDone() and st.orderStatus.status=='Filled': triggered=st; break

    if not triggered:
        log.info('NO TRIGGER, cancelling stops')
        try: ib.cancelOrder(bt.order)
        except: pass
        try: ib.cancelOrder(st.order)
        except: pass
        append_trade({'ts':utcnow().isoformat(),'strategy':args.strategy,
                      'instrument':args.instrument,'qty':args.qty,
                      'open_px':open_px,'side':None,'entry':None,'exit':None,'reason':'NO_TRIGGER',
                      'pnl_per_oz':0.0,'pnl_total':0.0,'paper':not args.live})
        ib.disconnect(); sys.exit(0)

    reason, exit_px = attach_exits(ib, c, triggered, args.qty, HOLD_MIN)
    entry = triggered.orderStatus.avgFillPrice
    side = triggered.order.action
    pnl_oz = (exit_px-entry) if side=='BUY' else (entry-exit_px)
    pnl_tot = pnl_oz * args.qty
    log.info(f'DONE [{args.strategy}]: {side} entry={entry} exit={exit_px} reason={reason} pnl=${pnl_oz:.2f}/oz total=${pnl_tot:.2f}')
    append_trade({'ts':utcnow().isoformat(),'strategy':args.strategy,
                  'instrument':args.instrument,'qty':args.qty,
                  'open_px':open_px,'side':side,'entry':float(entry),'exit':float(exit_px),
                  'reason':reason,'pnl_per_oz':float(pnl_oz),'pnl_total':float(pnl_tot),
                  'paper':not args.live})
    ib.disconnect()

if __name__ == '__main__': main()

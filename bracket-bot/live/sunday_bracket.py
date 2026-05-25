#!/usr/bin/env python3
"""Sunday-open gold bracket — live execution via ib_insync.

Validated config (2y backtest):
  offset = $5    (buy_stop @ open+$5, sell_stop @ open-$5)
  TP     = $20   (take profit at +/- $20 from entry)
  hold   = 60min (time exit if no TP)
  result: 71% win, PF 4.38, +$835/oz over 2.4y

Usage:
  source /Users/jo/omega_repo/.venv/bin/activate
  python sunday_bracket.py --paper   # paper trading (port 7497)
  python sunday_bracket.py --live    # live trading (port 7496)
  python sunday_bracket.py --qty 1   # 1 contract MGC (default)
  python sunday_bracket.py --instrument GC --qty 1   # full GC
  python sunday_bracket.py --instrument XAU --qty 0.01  # CFD via XAU symbol

Scheduling: cron entry runs this 5min before CME globex Sunday open.
"""
import argparse
import json
import logging
import sys
from datetime import datetime, timezone, timedelta
from pathlib import Path

from ib_insync import IB, Future, Forex, StopOrder, LimitOrder, MarketOrder, util

TRADES_FILE = Path(__file__).resolve().parent.parent / 'data' / 'trades.ndjson'
TRADES_FILE.parent.mkdir(exist_ok=True)

def append_trade(rec: dict):
    """Persist one trade outcome for the GUI to read."""
    with open(TRADES_FILE, 'a') as f:
        f.write(json.dumps(rec) + '\n')

# ── CONFIG (Calmar-optimal validated 2.4y) ─────────────────────────────────
OFFSET     = 3.0    # $ above/below open for stops
TP_DIST    = 50.0   # $ TP from entry
SL_DIST    = 3.0    # $ hard SL from entry (1× offset, tight)
HOLD_MIN   = 60     # minutes max hold
TICK_SZ    = 0.10   # gold tick size (MGC/GC = 0.10)

LOG_DIR = Path(__file__).resolve().parent.parent / 'logs'
LOG_DIR.mkdir(exist_ok=True)

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    handlers=[
        logging.FileHandler(LOG_DIR / f'bracket_{datetime.utcnow():%Y%m%d}.log'),
        logging.StreamHandler(sys.stdout),
    ]
)
log = logging.getLogger('bracket')


def round_tick(px, tick=TICK_SZ):
    return round(round(px / tick) * tick, 2)


def get_contract(instrument: str, expiry: str = None):
    """Build IB contract by instrument key."""
    if instrument == 'MGC':
        return Future('MGC', expiry or '20260618', 'COMEX')
    if instrument == 'GC':
        return Future('GC', expiry or '20260618', 'COMEX')
    if instrument == 'XAUUSD':
        # Spot gold via IB Forex (smallest size = 1 oz)
        from ib_insync import Forex
        return Forex('XAUUSD')
    if instrument == 'XAU':
        # Spot gold CFD via IBKR SMART (alternative)
        from ib_insync import CFD
        return CFD('XAUUSD', exchange='SMART')
    raise ValueError(f'Unknown instrument: {instrument}')


def get_current_price(ib: IB, contract) -> float:
    """Get reference price for bracket placement."""
    [ticker] = ib.reqTickers(contract)
    px = ticker.marketPrice()
    if not px or px <= 0:
        # Fall back to last
        px = ticker.last or ticker.close
    if not px or px <= 0:
        raise RuntimeError(f'No price for {contract.symbol}')
    return float(px)


def place_brackets(ib: IB, contract, qty: int, open_px: float):
    """Place buy+sell stops OCA-linked, then attach TP children on fill."""
    oca = f'GOLD_BRK_{datetime.utcnow():%Y%m%d_%H%M%S}'
    buy_trigger  = round_tick(open_px + OFFSET)
    sell_trigger = round_tick(open_px - OFFSET)

    buy  = StopOrder('BUY',  qty, stopPrice=buy_trigger)
    buy.ocaGroup = oca; buy.ocaType = 1   # 1 = cancel block + remaining qty on fill
    buy.tif = 'GTC'

    sell = StopOrder('SELL', qty, stopPrice=sell_trigger)
    sell.ocaGroup = oca; sell.ocaType = 1
    sell.tif = 'GTC'

    buy_trade  = ib.placeOrder(contract, buy)
    sell_trade = ib.placeOrder(contract, sell)

    log.info(f'PLACED OCA {oca}: BUY_STP @ {buy_trigger} / SELL_STP @ {sell_trigger}, qty={qty}')
    return oca, buy_trade, sell_trade


def attach_tp_and_time_exit(ib: IB, contract, parent_trade, qty: int, hold_min: int):
    """Once parent fills, attach a TP limit + schedule time exit."""
    parent_order = parent_trade.order
    parent_id    = parent_order.orderId
    side_filled  = parent_order.action      # BUY or SELL
    fill_px      = parent_trade.orderStatus.avgFillPrice
    log.info(f'TRIGGERED: {side_filled} filled @ {fill_px}')

    # Opposite side for exit
    exit_action = 'SELL' if side_filled == 'BUY' else 'BUY'
    if side_filled == 'BUY':
        tp_px = round_tick(fill_px + TP_DIST)
        sl_px = round_tick(fill_px - SL_DIST)
    else:
        tp_px = round_tick(fill_px - TP_DIST)
        sl_px = round_tick(fill_px + SL_DIST)

    # OCA pair: TP limit + SL stop. First fill cancels other.
    child_oca = f'CHILD_{parent_id}'
    tp = LimitOrder(exit_action, qty, lmtPrice=tp_px)
    tp.tif = 'GTC'; tp.ocaGroup = child_oca; tp.ocaType = 1

    sl = StopOrder(exit_action, qty, stopPrice=sl_px)
    sl.tif = 'GTC'; sl.ocaGroup = child_oca; sl.ocaType = 1

    tp_trade = ib.placeOrder(contract, tp)
    sl_trade = ib.placeOrder(contract, sl)
    log.info(f'ATTACHED OCA exits: TP {exit_action} LMT@{tp_px} | SL {exit_action} STP@{sl_px}  (entry {fill_px})')

    # Time-exit watcher — first child fill OR time runs out
    deadline = datetime.utcnow() + timedelta(minutes=hold_min)
    log.info(f'Time exit deadline: {deadline.isoformat()}')
    while datetime.utcnow() < deadline:
        ib.sleep(2)
        if tp_trade.isDone() and tp_trade.orderStatus.status == 'Filled':
            log.info(f'TP HIT: px={tp_trade.orderStatus.avgFillPrice}')
            return 'TP', tp_trade.orderStatus.avgFillPrice
        if sl_trade.isDone() and sl_trade.orderStatus.status == 'Filled':
            log.info(f'SL HIT: px={sl_trade.orderStatus.avgFillPrice}')
            return 'SL', sl_trade.orderStatus.avgFillPrice

    # Time exit: cancel both children, market close
    log.info(f'TIME EXIT firing: cancelling TP+SL + market close')
    try: ib.cancelOrder(tp_trade.order)
    except: pass
    try: ib.cancelOrder(sl_trade.order)
    except: pass
    ib.sleep(1)
    mkt = MarketOrder(exit_action, qty)
    close_trade = ib.placeOrder(contract, mkt)
    ib.sleep(3)
    log.info(f'TIME EXIT done: status={close_trade.orderStatus.status} avg={close_trade.orderStatus.avgFillPrice}')
    return 'TIME', close_trade.orderStatus.avgFillPrice


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--paper', action='store_true', help='paper port 7497 (default)')
    ap.add_argument('--live',  action='store_true', help='live port 7496')
    ap.add_argument('--host',  default='127.0.0.1')
    ap.add_argument('--instrument', default='XAUUSD', choices=['MGC','GC','XAU','XAUUSD'])
    ap.add_argument('--expiry', default=None)
    ap.add_argument('--qty', type=int, default=1)
    ap.add_argument('--client-id', type=int, default=42)
    args = ap.parse_args()

    port = 7496 if args.live else 7497
    log.info(f'=== SUNDAY BRACKET — instrument={args.instrument} qty={args.qty} port={port} ===')

    ib = IB()
    try:
        ib.connect(args.host, port, clientId=args.client_id)
        log.info(f'Connected to IB Gateway {args.host}:{port}')
    except Exception as e:
        log.error(f'Connect failed: {e}')
        sys.exit(2)

    contract = get_contract(args.instrument, args.expiry)
    ib.qualifyContracts(contract)
    log.info(f'Contract: {contract}')

    open_px = get_current_price(ib, contract)
    log.info(f'Reference open price: {open_px}')

    oca, buy_trade, sell_trade = place_brackets(ib, contract, args.qty, open_px)

    # Wait for either bracket to trigger (up to HOLD_MIN as no-trigger timeout)
    log.info(f'Watching for trigger (timeout {HOLD_MIN}min)...')
    trigger_deadline = datetime.utcnow() + timedelta(minutes=HOLD_MIN)
    triggered = None
    while datetime.utcnow() < trigger_deadline:
        ib.sleep(2)
        if buy_trade.isDone() and buy_trade.orderStatus.status == 'Filled':
            triggered = buy_trade; break
        if sell_trade.isDone() and sell_trade.orderStatus.status == 'Filled':
            triggered = sell_trade; break

    if not triggered:
        log.info('No trigger within window — cancelling both stops')
        ib.cancelOrder(buy_trade.order)
        ib.cancelOrder(sell_trade.order)
        append_trade({
            'ts': datetime.utcnow().isoformat(),
            'strategy': 'SUNDAY', 'instrument': args.instrument,
            'qty': args.qty,
            'open_px': open_px,
            'side': None, 'entry': None, 'exit': None,
            'reason': 'NO_TRIGGER',
            'pnl_per_oz': 0.0,
            'pnl_total': 0.0,
            'paper': not args.live,
        })
        ib.disconnect()
        sys.exit(0)

    # Attach TP + time exit
    reason, exit_px = attach_tp_and_time_exit(ib, contract, triggered, args.qty, HOLD_MIN)
    entry_px = triggered.orderStatus.avgFillPrice
    side     = triggered.order.action
    pnl_per_oz = (exit_px - entry_px) if side == 'BUY' else (entry_px - exit_px)
    pnl_total = pnl_per_oz * args.qty
    log.info(f'TRADE COMPLETE: side={side} entry={entry_px} exit={exit_px} reason={reason} pnl=${pnl_per_oz:.2f}/oz total=${pnl_total:.2f}')

    append_trade({
        'ts': datetime.utcnow().isoformat(),
        'strategy': 'SUNDAY', 'instrument': args.instrument,
        'qty': args.qty,
        'open_px': open_px,
        'side': side,
        'entry': float(entry_px),
        'exit': float(exit_px),
        'reason': reason,
        'pnl_per_oz': float(pnl_per_oz),
        'pnl_total': float(pnl_total),
        'paper': not args.live,
    })

    ib.disconnect()


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
r"""Sunday-open gold bracket — live execution via ib_insync.

Validated config (2y backtest):
  offset = $5    (buy_stop @ open+$5, sell_stop @ open-$5)
  TP     = $20   (take profit at +/- $20 from entry)
  hold   = 60min (time exit if no TP)
  result: 71% win, PF 4.38, +$835/oz over 2.4y

Usage (Windows VPS, IB Gateway paper port 4002 by default):
  & ".\.venv\Scripts\python.exe" live\sunday_bracket.py --paper
  & ".\.venv\Scripts\python.exe" live\sunday_bracket.py --live      # live port 7496
  & ".\.venv\Scripts\python.exe" live\sunday_bracket.py --instrument GC --qty 1
  & ".\.venv\Scripts\python.exe" live\sunday_bracket.py --instrument XAU --qty 0.01

Scheduling: cron entry runs this 5min before CME globex Sunday open.
"""
import argparse
import json
import logging
import math
import os
import sys
from datetime import datetime, timezone, timedelta
from pathlib import Path

from ib_insync import IB, Future, ContFuture, Forex, StopOrder, LimitOrder, MarketOrder, util

def utcnow(): return datetime.now(timezone.utc)

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
        logging.FileHandler(LOG_DIR / f'bracket_{utcnow():%Y%m%d}.log'),
        logging.StreamHandler(sys.stdout),
    ]
)
log = logging.getLogger('bracket')


def round_tick(px, tick=TICK_SZ):
    return round(round(px / tick) * tick, 2)


def get_contract(instrument: str, expiry: str = None):
    """Build IB contract by instrument key."""
    if instrument in ('MGC', 'GC'):
        return Future(instrument, expiry, 'COMEX') if expiry else ContFuture(instrument, 'COMEX')
    if instrument == 'XAUUSD':
        # Spot gold via IB Forex (smallest size = 1 oz)
        return Forex('XAUUSD')
    if instrument == 'XAU':
        # Spot gold CFD via IBKR SMART (alternative)
        from ib_insync import CFD
        return CFD('XAUUSD', exchange='SMART')
    raise ValueError(f'Unknown instrument: {instrument}')


def get_price_from_depth(ib: IB, contract, secs: int = 10):
    """L2 best-bid/ask midpoint. Uses existing IBKR L2 sub (separate from
    top-of-book sub on CME). Returns None if depth not available."""
    try:
        ib.reqMarketDataType(1)
        dom = ib.reqMktDepth(contract, numRows=1)
        for _ in range(secs):
            ib.sleep(1)
            if dom.domBids and dom.domAsks:
                bid = dom.domBids[0].price; ask = dom.domAsks[0].price
                if (bid and ask and bid > 0 and ask > 0
                        and not math.isnan(bid) and not math.isnan(ask)):
                    ib.cancelMktDepth(contract)
                    mid = (bid + ask) / 2.0
                    log.info(f'price [L2 mid]: {mid} (bid={bid} ask={ask})')
                    return float(mid)
        ib.cancelMktDepth(contract)
        log.warning('no L2 depth within timeout')
    except Exception as e:
        log.warning(f'L2 depth failed: {e}')
    return None


def get_current_price(ib: IB, contract) -> float:
    """Reference price for bracket placement.
    Order: L2 mid (real-time via existing L2 sub) → top-of-book live → frozen → delayed.
    BRACKET_DELAYED_ONLY=1: skip L2 + live mdt (no /DEEP or top-of-book sub on paper
    account); go straight to delayed. Unset once real-time sub activates."""
    delayed_only = os.environ.get('BRACKET_DELAYED_ONLY') == '1'
    if not delayed_only:
        px = get_price_from_depth(ib, contract)
        if px is not None:
            return px
    mdts = ([(3, 'delayed'), (4, 'delayed-frozen')] if delayed_only
            else [(1, 'live'), (2, 'frozen'), (3, 'delayed'), (4, 'delayed-frozen')])
    for mdt, label in mdts:
        try:
            ib.reqMarketDataType(mdt)
            [ticker] = ib.reqTickers(contract)
            px = ticker.marketPrice()
            if not px or (isinstance(px, float) and math.isnan(px)) or px <= 0:
                px = ticker.last or ticker.close
            if px and not (isinstance(px, float) and math.isnan(px)) and px > 0:
                log.info(f'price [{label}]: {px}')
                return float(px)
            log.warning(f'no price from mdt={mdt} ({label})')
        except Exception as e:
            log.warning(f'mdt={mdt} ({label}) failed: {e}')
    raise RuntimeError(f'No price for {contract.symbol} across all market-data types')


def place_brackets(ib: IB, contract, qty: int, open_px: float):
    """Place buy+sell stops OCA-linked, then attach TP children on fill."""
    oca = f'GOLD_BRK_{utcnow():%Y%m%d_%H%M%S}'
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
    deadline = utcnow() + timedelta(minutes=hold_min)
    log.info(f'Time exit deadline: {deadline.isoformat()}')
    while utcnow() < deadline:
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
    ap.add_argument('--paper', action='store_true', help='paper mode (default port 4002 = IB Gateway paper)')
    ap.add_argument('--live',  action='store_true', help='live mode (override --port for live gateway/TWS)')
    ap.add_argument('--host',  default='127.0.0.1')
    ap.add_argument('--port',  type=int, default=4002, help='IB Gateway 4002 (paper) / 4001 (live), TWS 7497/7496')
    ap.add_argument('--instrument', default='MGC', choices=['MGC','GC','XAU','XAUUSD'])
    ap.add_argument('--expiry', default=None)
    ap.add_argument('--qty', type=int, default=1)
    ap.add_argument('--client-id', type=int, default=102, help='avoid 42 (ibkr_dom_bridge) and 100 (daily_bracket)')
    ap.add_argument('--dry-run', action='store_true', help='resolve contract + fetch price, skip order placement')
    args = ap.parse_args()

    log.info(f'=== SUNDAY BRACKET — instrument={args.instrument} qty={args.qty} port={args.port} dry={args.dry_run} ===')

    ib = IB()
    try:
        ib.connect(args.host, args.port, clientId=args.client_id)
        log.info(f'Connected to IB Gateway {args.host}:{args.port}')
    except Exception as e:
        log.error(f'Connect failed: {e}')
        sys.exit(2)

    contract = get_contract(args.instrument, args.expiry)
    ib.qualifyContracts(contract)
    log.info(f'Contract: {contract}')

    open_px = get_current_price(ib, contract)
    log.info(f'Reference open price: {open_px}')

    if args.dry_run:
        log.info('DRY RUN — skipping order placement')
        ib.disconnect(); sys.exit(0)

    oca, buy_trade, sell_trade = place_brackets(ib, contract, args.qty, open_px)

    # Wait for either bracket to trigger (up to HOLD_MIN as no-trigger timeout)
    log.info(f'Watching for trigger (timeout {HOLD_MIN}min)...')
    trigger_deadline = utcnow() + timedelta(minutes=HOLD_MIN)
    triggered = None
    while utcnow() < trigger_deadline:
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
            'ts': utcnow().isoformat(),
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
        'ts': utcnow().isoformat(),
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

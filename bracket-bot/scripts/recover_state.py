#!/usr/bin/env python3
"""Recovery: inspect state files written by bracket scripts and reconcile
against IBKR open orders. Run at boot / after a crash before the next
scheduled task is allowed to fire.

  python scripts\\recover_state.py                 # report only
  python scripts\\recover_state.py --cancel-orphans   # cancel orders not in any state file

A state file lives at  data/state/<STRATEGY>.json  while a bracket is in
flight (stages: placing_brackets -> awaiting_trigger -> in_position).
A clean run clears it. A leftover file = the script died mid-cycle.
Possible outcomes:

  - parent stop never filled  -> cancel the two stops; clear state.
  - parent filled, children present -> let the children run, clear state.
  - parent filled, no children -> P0 — bracket has no exits, ALERT.

The latter is what this script is for.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from ib_insync import IB

from live._common import STATE_DIR, ensure_dirs, notify_failure


def load_states() -> list[dict]:
    ensure_dirs()
    out = []
    for p in sorted(STATE_DIR.glob('*.json')):
        try:
            d = json.loads(p.read_text())
            d['_path'] = str(p)
            out.append(d)
        except Exception as e:  # noqa: BLE001
            print(f'WARN: failed to parse {p.name}: {e}', file=sys.stderr)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=4002)
    ap.add_argument('--client-id', type=int, default=0)
    ap.add_argument('--cancel-orphans', action='store_true',
                    help='cancel any non-state-tracked open MGC/GC order')
    args = ap.parse_args()

    states = load_states()
    if not states:
        print('no stale state files')
    for s in states:
        print(f'stale state: {s["_path"]}  stage={s.get("stage")}  oca={s.get("oca")}')

    ib = IB()
    try:
        ib.connect(args.host, args.port, clientId=args.client_id, timeout=10)
    except Exception as e:  # noqa: BLE001
        print(f'connect failed: {e}', file=sys.stderr)
        return 2

    ib.reqAllOpenOrders(); ib.sleep(2)
    open_trades = ib.openTrades()
    print(f'open orders on account: {len(open_trades)}')
    for t in open_trades:
        print(f'  #{t.order.orderId} cid={t.order.clientId} {t.contract.symbol} '
              f'{t.order.action} {t.order.orderType} oca={t.order.ocaGroup or "-"} '
              f'status={t.orderStatus.status}')

    tracked_oca = {s.get('oca') for s in states if s.get('oca')}
    orphan = [t for t in open_trades
              if t.contract.symbol in ('MGC', 'GC')
              and (t.order.ocaGroup or '') not in tracked_oca]

    in_position_no_children = [
        s for s in states
        if s.get('stage') == 'in_position'
        and not any((t.order.ocaGroup or '').startswith(f'CHILD_{s.get("parent_order_id")}')
                    for t in open_trades)
    ]

    if in_position_no_children:
        msg = (f'P0: {len(in_position_no_children)} bracket(s) in position with NO child exits — '
               f'manual reconciliation required')
        print(msg, file=sys.stderr)
        notify_failure('RECOVERY', msg)

    if orphan:
        print(f'orphan orders (not tracked by any state file): {len(orphan)}')
        for t in orphan:
            print(f'  #{t.order.orderId} {t.contract.symbol} {t.order.action} '
                  f'{t.order.orderType} oca={t.order.ocaGroup or "-"}')
        if args.cancel_orphans:
            for t in orphan:
                try:
                    ib.cancelOrder(t.order)
                    print(f'  cancelled #{t.order.orderId}')
                except Exception as e:  # noqa: BLE001
                    print(f'  cancel #{t.order.orderId} failed: {e}', file=sys.stderr)

    ib.disconnect()
    return 0 if not in_position_no_children else 3


if __name__ == '__main__':
    sys.exit(main())

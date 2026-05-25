#!/usr/bin/env python3
"""Omega Sunday-bracket dashboard.

Serves on http://localhost:5050
  /                      -> dashboard HTML
  /api/trades            -> all trade records
  /api/summary           -> aggregated stats
  /api/status            -> cron next-run + connection check
"""
import json
import subprocess
from datetime import datetime, timezone, timedelta
from pathlib import Path
from flask import Flask, jsonify, send_from_directory

ROOT = Path(__file__).resolve().parent
DATA = ROOT / 'data' / 'trades.ndjson'
GUI  = ROOT / 'gui'

app = Flask(__name__, static_folder=str(GUI), static_url_path='')

def load_trades():
    if not DATA.exists(): return []
    out = []
    with open(DATA) as f:
        for line in f:
            line = line.strip()
            if not line: continue
            try: out.append(json.loads(line))
            except: pass
    return out

def next_sunday_2255_utc():
    now = datetime.now(timezone.utc)
    days_ahead = (6 - now.weekday()) % 7  # Sunday = 6
    target = now.replace(hour=22, minute=55, second=0, microsecond=0) + timedelta(days=days_ahead)
    if target <= now: target += timedelta(days=7)
    return target

@app.route('/')
def index():
    return send_from_directory(str(GUI), 'index.html')

@app.route('/api/trades')
def api_trades():
    return jsonify(load_trades())

def _summarize(trades):
    traded = [t for t in trades if t.get('side')]
    wins = [t for t in traded if t['pnl_total'] > 0]
    losses = [t for t in traded if t['pnl_total'] <= 0]
    pos = sum(t['pnl_total'] for t in wins)
    neg = abs(sum(t['pnl_total'] for t in losses))
    pf = pos/neg if neg > 0 else 99 if pos > 0 else 0
    cum = 0; peak = 0; max_dd = 0
    for t in traded:
        cum += t['pnl_total']; peak = max(peak, cum); max_dd = min(max_dd, cum - peak)
    return {
        'total_records': len(trades), 'total_trades': len(traded),
        'wins': len(wins), 'losses': len(losses),
        'win_rate': len(wins)/len(traded)*100 if traded else 0,
        'total_pnl': sum(t['pnl_total'] for t in traded),
        'avg_win': pos/len(wins) if wins else 0,
        'avg_loss': -neg/len(losses) if losses else 0,
        'profit_factor': pf, 'max_drawdown': max_dd,
        'no_trigger_count': len(trades) - len(traded),
    }

@app.route('/api/summary')
def api_summary():
    trades = load_trades()
    overall = _summarize(trades)
    by_strategy = {}
    strategies = sorted(set(t.get('strategy', 'UNKNOWN') for t in trades))
    for s in strategies:
        sub = [t for t in trades if t.get('strategy', 'UNKNOWN') == s]
        by_strategy[s] = _summarize(sub)
    overall['by_strategy'] = by_strategy
    return jsonify(overall)

@app.route('/api/cancel_all', methods=['POST'])
def api_cancel_all():
    import socket
    for port in [4002, 4001, 7497, 7496]:
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=1): pass
        except OSError:
            continue
        try:
            from ib_insync import IB
            ib = IB(); ib.connect('127.0.0.1', port, clientId=997, timeout=4)
            ib.reqGlobalCancel(); ib.sleep(2); ib.disconnect()
            return jsonify({'ok': True, 'port': port})
        except Exception as e:
            return jsonify({'ok': False, 'error': str(e)})
    return jsonify({'ok': False, 'error': 'no IB Gateway reachable'})

@app.route('/api/open_orders')
def api_open_orders():
    """Snapshot of IBKR open orders. Requires Gateway reachable."""
    import socket
    out = {'orders': [], 'error': None}
    # Probe both paper + live ports
    for port in [4002, 4001, 7497, 7496]:
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=1):
                pass
        except OSError:
            continue
        try:
            from ib_insync import IB
            ib = IB()
            ib.connect('127.0.0.1', port, clientId=998, timeout=4)
            ib.reqAllOpenOrders(); ib.sleep(2)
            for t in ib.openTrades():
                out['orders'].append({
                    'port': port,
                    'client_id': t.order.clientId,
                    'action': t.order.action,
                    'qty': t.order.totalQuantity,
                    'symbol': t.contract.symbol,
                    'aux_price': t.order.auxPrice,
                    'lmt_price': t.order.lmtPrice,
                    'oca_group': t.order.ocaGroup,
                    'status': t.orderStatus.status,
                })
            ib.disconnect()
            break
        except Exception as e:
            out['error'] = str(e)
    return jsonify(out)

@app.route('/api/status')
def api_status():
    nxt = next_sunday_2255_utc()
    seconds_until = (nxt - datetime.now(timezone.utc)).total_seconds()
    # Check IB Gateway port
    paper_up = subprocess.call(['nc','-z','127.0.0.1','7497'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0
    live_up = subprocess.call(['nc','-z','127.0.0.1','7496'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0
    return jsonify({
        'next_run_utc': nxt.isoformat(),
        'seconds_until_next_run': int(seconds_until),
        'ib_paper_listening': paper_up,
        'ib_live_listening': live_up,
        'now_utc': datetime.now(timezone.utc).isoformat(),
    })

if __name__ == '__main__':
    app.run(host='127.0.0.1', port=5050, debug=False)

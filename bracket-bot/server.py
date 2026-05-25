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
from datetime import datetime, time, timezone, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo
from flask import Flask, jsonify, send_from_directory

LDN_TZ = ZoneInfo('Europe/London')
NY_TZ  = ZoneInfo('America/New_York')
LDN_OPEN = time(8, 0)   # 08:00 London local (DST-aware)
LDN_CLOSE = time(16, 30)
NY_OPEN  = time(9, 30)  # 09:30 NY local (DST-aware)
NY_CLOSE = time(16, 0)

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

def _next_session_open(tz, open_t):
    """Next local open time (skips weekends), returned as UTC."""
    now_local = datetime.now(tz)
    target = now_local.replace(hour=open_t.hour, minute=open_t.minute, second=0, microsecond=0)
    if target <= now_local:
        target += timedelta(days=1)
    while target.weekday() >= 5:  # Sat=5, Sun=6
        target += timedelta(days=1)
    return target.astimezone(timezone.utc)

def _in_window(now_local, open_t, close_t):
    if now_local.weekday() >= 5: return False
    return open_t <= now_local.timetz().replace(tzinfo=None) <= close_t

def session_state():
    now_utc = datetime.now(timezone.utc)
    ldn_local = now_utc.astimezone(LDN_TZ)
    ny_local  = now_utc.astimezone(NY_TZ)
    ldn_open_in = _in_window(ldn_local, LDN_OPEN, LDN_CLOSE)
    ny_open_in  = _in_window(ny_local,  NY_OPEN,  NY_CLOSE)
    return {
        'london': {
            'in_session': ldn_open_in,
            'local_now': ldn_local.strftime('%H:%M %Z'),
            'next_open_utc': _next_session_open(LDN_TZ, LDN_OPEN).isoformat(),
        },
        'ny': {
            'in_session': ny_open_in,
            'local_now': ny_local.strftime('%H:%M %Z'),
            'next_open_utc': _next_session_open(NY_TZ, NY_OPEN).isoformat(),
        },
    }

def classify_session(ts_iso):
    """Tag a trade UTC timestamp as LONDON / NY / OVERLAP / OFF."""
    try:
        dt = datetime.fromisoformat(ts_iso.replace('Z', '+00:00'))
        if dt.tzinfo is None: dt = dt.replace(tzinfo=timezone.utc)
    except Exception:
        return 'OFF'
    in_ldn = _in_window(dt.astimezone(LDN_TZ), LDN_OPEN, LDN_CLOSE)
    in_ny  = _in_window(dt.astimezone(NY_TZ),  NY_OPEN,  NY_CLOSE)
    if in_ldn and in_ny: return 'OVERLAP'
    if in_ldn: return 'LONDON'
    if in_ny:  return 'NY'
    return 'OFF'

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
    by_session = {}
    for tag in ('LONDON', 'NY', 'OVERLAP', 'OFF'):
        sub = [t for t in trades if classify_session(t.get('ts', '')) == tag]
        by_session[tag] = _summarize(sub)
    overall['by_session'] = by_session
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

@app.route('/api/health')
def api_health():
    """Return latest healthcheck snapshot. Path overridable via
    OMEGA_HEALTH_STATUS env var. If file missing or stale, surface that."""
    import os
    candidates = []
    if os.environ.get('OMEGA_HEALTH_STATUS'):
        candidates.append(Path(os.environ['OMEGA_HEALTH_STATUS']))
    candidates += [
        Path(r'C:\Omega\logs\health\status.json'),
        ROOT.parent / 'logs' / 'health' / 'status.json',
        ROOT / 'logs' / 'health' / 'status.json',
    ]
    for p in candidates:
        if p.exists():
            try:
                payload = json.loads(p.read_text(encoding='utf-8'))
            except Exception as e:
                return jsonify({'overall': 'FAIL', 'error': f'parse {p}: {e}',
                                'checks': [], 'fail_count': 1, 'warn_count': 0})
            # Stale-snapshot guard: if the healthcheck hasn't run in 10 min,
            # treat that itself as a FAIL — silent monitoring failure is the
            # exact thing we're trying to prevent.
            try:
                ts = datetime.fromisoformat(payload['ts'].replace('Z', '+00:00'))
                age_min = (datetime.now(timezone.utc) - ts).total_seconds() / 60.0
                if age_min > 10:
                    payload['overall'] = 'FAIL'
                    payload.setdefault('checks', []).insert(0, {
                        'name': 'healthcheck.snapshot_fresh', 'severity': 'FAIL',
                        'status': 'stale',
                        'detail': f'status.json is {age_min:.1f}m old (limit 10m). Healthcheck task not running?',
                        'ts': payload['ts']})
                    payload['fail_count'] = payload.get('fail_count', 0) + 1
            except Exception: pass
            payload['source_path'] = str(p)
            return jsonify(payload)
    return jsonify({'overall': 'FAIL',
                    'error': 'status.json not found in any known location',
                    'checks': [{'name': 'healthcheck.snapshot_present',
                                'severity': 'FAIL', 'status': 'missing',
                                'detail': 'Run tools/healthcheck.ps1 on the VPS.'}],
                    'fail_count': 1, 'warn_count': 0,
                    'searched': [str(p) for p in candidates]})

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
        'sessions': session_state(),
    })

if __name__ == '__main__':
    app.run(host='127.0.0.1', port=5050, debug=False)

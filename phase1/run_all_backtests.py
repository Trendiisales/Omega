#!/usr/bin/env python3
"""
phase1/run_all_backtests.py — Driver for all (strategy, tf, direction) cells.

Uses sim_lib.py to run each combination. Saves trades_net/<combo>.parquet
with full GROSS/NET/STRESS columns.

Cell metric:
  family A  (EMA pullback, Donchian)        — sim_family_a
  family B  (RSI revert, Bollinger)         — sim_family_b
  family C  (TS momentum)                    — sim_family_c
  family D  (Asian break)                    — sim_family_d
"""
import os, sys, time, math
sys.path.insert(0, '/Users/jo/omega_repo/phase1')
import sim_lib
from sim_lib import (TickReader, sim_family_a, sim_family_b, sim_family_c,
                     sim_family_d, apply_costs, TF_MS, PHASE0, PHASE1)

import pyarrow.parquet as pq
import pyarrow as pa

SIGDIR = os.path.join(PHASE1, 'signals')
TRADEDIR = os.path.join(PHASE1, 'trades_net')
os.makedirs(TRADEDIR, exist_ok=True)

# =============================================================================
# Cell definitions: (strategy, tf, direction) -> (family, params)
# =============================================================================
TIMEFRAMES = ['M15', 'H1', 'H2', 'H4', 'H6', 'D1']

# RSI/Bollinger need exit-indicator functions and bar lookups
def make_rsi_exit_long(rsi_arr):
    def fn(bar, direction):
        # Exit when RSI crosses 50 (back to neutral)
        idx = bar.get('_idx')
        if idx is None or rsi_arr[idx] is None: return False
        return rsi_arr[idx] >= 50
    return fn

def make_rsi_exit_short(rsi_arr):
    def fn(bar, direction):
        idx = bar.get('_idx')
        if idx is None or rsi_arr[idx] is None: return False
        return rsi_arr[idx] <= 50
    return fn

def make_bb_exit_long(bb_mid):
    def fn(bar, direction):
        idx = bar.get('_idx')
        if idx is None or bb_mid[idx] is None: return False
        return bar['close'] >= bb_mid[idx]
    return fn

def make_bb_exit_short(bb_mid):
    def fn(bar, direction):
        idx = bar.get('_idx')
        if idx is None or bb_mid[idx] is None: return False
        return bar['close'] <= bb_mid[idx]
    return fn

# RSI / Bollinger computation (mirror of phase 1.1)
RSI_PERIOD = 14
BB_PERIOD = 20
BB_SIGMA = 2.0

def compute_rsi_array(bars):
    rsis = [None] * len(bars)
    if len(bars) < RSI_PERIOD + 1: return rsis
    gains = []; losses = []
    for i in range(1, RSI_PERIOD + 1):
        ch = bars[i]['close'] - bars[i-1]['close']
        gains.append(max(ch, 0)); losses.append(max(-ch, 0))
    avg_gain = sum(gains)/RSI_PERIOD
    avg_loss = sum(losses)/RSI_PERIOD
    if avg_loss == 0: rsis[RSI_PERIOD] = 100
    else: rsis[RSI_PERIOD] = 100 - 100/(1 + avg_gain/avg_loss)
    for i in range(RSI_PERIOD+1, len(bars)):
        ch = bars[i]['close'] - bars[i-1]['close']
        g = max(ch,0); l = max(-ch,0)
        avg_gain = (avg_gain*(RSI_PERIOD-1) + g)/RSI_PERIOD
        avg_loss = (avg_loss*(RSI_PERIOD-1) + l)/RSI_PERIOD
        if avg_loss == 0: rsis[i] = 100
        else: rsis[i] = 100 - 100/(1 + avg_gain/avg_loss)
    return rsis

def compute_bb_arrays(bars):
    n = len(bars)
    mid = [None]*n; up = [None]*n; lo = [None]*n
    if n < BB_PERIOD: return mid, up, lo
    for i in range(BB_PERIOD-1, n):
        win = [bars[j]['close'] for j in range(i-BB_PERIOD+1, i+1)]
        m = sum(win)/BB_PERIOD
        var = sum((x-m)**2 for x in win)/BB_PERIOD
        sd = math.sqrt(var)
        mid[i]=m; up[i]=m+BB_SIGMA*sd; lo[i]=m-BB_SIGMA*sd
    return mid, up, lo


# =============================================================================
# Run one cell
# =============================================================================
def run_cell(strategy, tf, direction, reader, bars, m15_bars, m15_starts):
    sig_path = os.path.join(SIGDIR, f'{strategy}_{tf}_{direction}.parquet')
    if not os.path.isfile(sig_path):
        return None, 'NO_SIG_FILE'
    signals = pq.read_table(sig_path).to_pylist()
    if not signals:
        return [], 'EMPTY'

    # Index bars for fast lookup of bar by signal_bar_idx
    # (signal already has signal_bar_idx and we need _bar_lookup_func for B)

    trades = []
    if strategy == 'ema_pullback':
        for s in signals:
            tr = sim_family_a(reader, s, direction, tf, sl_atr=1.0, tp_r=2.5,
                              max_hold_bars=30)
            if tr: trades.append(tr)
    elif strategy == 'donchian':
        for s in signals:
            tr = sim_family_a(reader, s, direction, tf, sl_atr=1.0, tp_r=2.5,
                              max_hold_bars=30)
            if tr: trades.append(tr)
    elif strategy == 'tsmom':
        for s in signals:
            tr = sim_family_c(reader, s, direction, tf, hold_bars=12,
                              hard_sl_atr=3.0)
            if tr: trades.append(tr)
    elif strategy == 'rsi_revert':
        rsis = compute_rsi_array(bars)
        # attach _idx to bars for the indicator function
        for i, b in enumerate(bars): b['_idx'] = i
        exit_fn = make_rsi_exit_long(rsis) if direction == 'long' else make_rsi_exit_short(rsis)
        bar_lookup = lambda: bars
        for s in signals:
            s['_bar_lookup_func'] = bar_lookup
            tr = sim_family_b(reader, s, direction, tf, hard_sl_atr=1.5,
                              max_hold_bars=20, exit_indicator_func=exit_fn)
            if tr: trades.append(tr)
    elif strategy == 'bollinger':
        mid, up, lo = compute_bb_arrays(bars)
        for i, b in enumerate(bars): b['_idx'] = i
        exit_fn = make_bb_exit_long(mid) if direction == 'long' else make_bb_exit_short(mid)
        bar_lookup = lambda: bars
        for s in signals:
            s['_bar_lookup_func'] = bar_lookup
            tr = sim_family_b(reader, s, direction, tf, hard_sl_atr=1.5,
                              max_hold_bars=20, exit_indicator_func=exit_fn)
            if tr: trades.append(tr)
    elif strategy == 'asian_break':
        for s in signals:
            tr = sim_family_d(reader, s, direction, tf, hard_sl_atr=1.0,
                              ny_close_hour=21)
            if tr: trades.append(tr)
    else:
        return None, 'UNKNOWN_STRAT'

    if trades:
        apply_costs(trades, reader, m15_bars, m15_starts)

    return trades, 'OK'


# =============================================================================
# Main
# =============================================================================
def main():
    print('Loading M15 bars (for cost lookup)...')
    m15_bars = pq.read_table(os.path.join(PHASE0, 'bars_M15_final.parquet')).to_pylist()
    m15_starts = [b['bar_start_ms'] for b in m15_bars]
    print(f'  {len(m15_bars)} M15 bars')

    print('Opening tick reader...')
    reader = TickReader()

    cells = []  # list of (strategy, tf, direction)
    strategies_main = ['ema_pullback', 'tsmom', 'donchian', 'rsi_revert', 'bollinger']
    for strat in strategies_main:
        for tf in TIMEFRAMES:
            for d in ['long', 'short']:
                cells.append((strat, tf, d))
    # Asian break only on M15/H1/H2
    for tf in ['M15', 'H1', 'H2']:
        for d in ['long', 'short']:
            cells.append(('asian_break', tf, d))

    print(f'Total cells: {len(cells)}')
    print()

    bar_cache = {}

    t_total = time.time()
    summary_rows = []
    for cell_i, (strat, tf, d) in enumerate(cells):
        t0 = time.time()
        print(f'[{cell_i+1}/{len(cells)}] {strat:>14}_{tf}_{d:<5} ', end='', flush=True)
        if tf not in bar_cache:
            bar_cache[tf] = pq.read_table(
                os.path.join(PHASE0, f'bars_{tf}_final.parquet')).to_pylist()
        trades, status = run_cell(strat, tf, d, reader, bar_cache[tf],
                                   m15_bars, m15_starts)
        elapsed = time.time() - t0
        if status != 'OK':
            print(f'  [{status}]  ({elapsed:.1f}s)')
            summary_rows.append({'strategy': strat, 'tf': tf, 'direction': d,
                                 'n': 0, 'status': status})
            continue
        if not trades:
            print(f'  no trades  ({elapsed:.1f}s)')
            summary_rows.append({'strategy': strat, 'tf': tf, 'direction': d,
                                 'n': 0, 'status': 'NO_TRADES'})
            continue
        # Save
        out_path = os.path.join(TRADEDIR, f'{strat}_{tf}_{d}_net.parquet')
        # Strip any non-serializable fields from signals that leaked into trades
        for tr in trades:
            tr.pop('_bar_lookup_func', None)
        pq.write_table(pa.Table.from_pylist(trades), out_path)
        # Aggregate
        n = len(trades)
        wins = sum(1 for t in trades if t['pnl_pts_net'] > 0)
        tot_g = sum(t['pnl_pts'] for t in trades)
        tot_n = sum(t['pnl_pts_net'] for t in trades)
        tot_s = sum(t['pnl_pts_stress'] for t in trades)
        pos_n = sum(t['pnl_pts_net'] for t in trades if t['pnl_pts_net']>0)
        neg_n = abs(sum(t['pnl_pts_net'] for t in trades if t['pnl_pts_net']<0))
        pf_n  = pos_n/neg_n if neg_n>0 else float('inf')
        print(f'  n={n:>4}  wr={100*wins/n:>4.1f}%  '
              f'gross={tot_g:>+8.1f}  net={tot_n:>+8.1f}  stress={tot_s:>+8.1f}  '
              f'pf_net={pf_n:.2f}  ({elapsed:.1f}s)')
        summary_rows.append({
            'strategy': strat, 'tf': tf, 'direction': d, 'status': 'OK',
            'n': n, 'wr_net': 100*wins/n,
            'gross': tot_g, 'net': tot_n, 'stress': tot_s,
            'pf_net': pf_n if pf_n != float('inf') else 999,
            'avg_trade_net': tot_n/n,
        })
    reader.close()

    # Save summary
    pq.write_table(pa.Table.from_pylist(summary_rows),
                   os.path.join(PHASE1, 'master_summary.parquet'))
    print()
    print(f'Total runtime: {(time.time()-t_total)/60:.1f} min')
    print(f'Master summary: {PHASE1}/master_summary.parquet')

if __name__ == '__main__':
    main()

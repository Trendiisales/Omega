#!/usr/bin/env python3
"""
phase1/sim_lib.py — Backtest simulator library.

Four exit families:
  A. ATR TP/SL with intrabar tick-walk     (EMA pullback, Donchian)
  B. Indicator exit + hard ATR SL          (RSI revert, Bollinger)
  C. Time exit + hard ATR SL               (TS momentum)
  D. Range exit + hard ATR SL              (Asian break)

For TF >= H1: full intrabar tick-walk
For M15:      bar-OHLC fill simulation (worst-case SL-before-TP ordering)

Cost model: spread is already baked into entry_px (ask for long, bid for
short) and exit_px (bid for long, ask for short). Only commission ($0.05
round-trip) is subtracted to get NET. Stress applies p99 spread on top.
"""
import os
from datetime import datetime, timezone
from bisect import bisect_right

import pyarrow.parquet as pq
import pyarrow as pa

PATH = '/Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv'
PHASE0 = '/Users/jo/omega_repo/phase0'
PHASE1 = '/Users/jo/omega_repo/phase1'

COMMISSION_RT = 0.05
TF_MS = {'M15': 15*60*1000, 'H1': 60*60*1000, 'H2': 2*60*60*1000,
         'H4': 4*60*60*1000, 'H6': 6*60*60*1000, 'D1': 24*60*60*1000}


class TickReader:
    """Random-access tick stream using the M15-byte-offset index."""
    GRANULE_MS = 15 * 60 * 1000

    def __init__(self, path=PATH, index_path=None):
        self.f = open(path, 'rb')
        if index_path is None:
            index_path = os.path.join(PHASE1, 'tick_index.parquet')
        idx_table = pq.read_table(index_path)
        self.index = dict(zip(idx_table['bar_start_ms'].to_pylist(),
                              idx_table['byte_offset'].to_pylist()))
        self.sorted_keys = sorted(self.index.keys())

    def seek_to_ms(self, ts_ms):
        bucket = (ts_ms // self.GRANULE_MS) * self.GRANULE_MS
        idx = bisect_right(self.sorted_keys, bucket) - 1
        if idx < 0: idx = 0
        bk = self.sorted_keys[idx]
        self.f.seek(self.index[bk])

    def iter_from(self, start_ms, max_ms=None):
        self.seek_to_ms(start_ms)
        for line in self.f:
            try:
                parts = line.rstrip().split(b',')
                ts = int(parts[0])
                ask = float(parts[1])
                bid = float(parts[2])
            except (ValueError, IndexError):
                continue
            if ts < start_ms: continue
            if max_ms is not None and ts > max_ms: return
            yield ts, ask, bid

    def find_quote(self, exit_ts):
        bucket = (exit_ts // self.GRANULE_MS) * self.GRANULE_MS
        idx = bisect_right(self.sorted_keys, bucket) - 1
        if idx < 0: return None, None
        self.f.seek(self.index[self.sorted_keys[idx]])
        for line in self.f:
            try:
                parts = line.rstrip().split(b',')
                ts = int(parts[0])
                ask = float(parts[1])
                bid = float(parts[2])
            except (ValueError, IndexError):
                continue
            if ts == exit_ts: return ask, bid
            if ts > exit_ts:  return None, None
        return None, None

    def close(self):
        self.f.close()


def _entry_fill(reader, next_bar_ms, direction):
    """First tick at or after next_bar_ms. Returns (entry_ts, entry_px,
    entry_ask, entry_bid)."""
    for ts, ask, bid in reader.iter_from(next_bar_ms):
        return (ts, ask if direction == 'long' else bid, ask, bid)
    return None


# =============================================================================
# Family A: ATR TP/SL (EMA pullback, Donchian)
# =============================================================================
def sim_family_a(reader, signal, direction, tf, sl_atr=1.0, tp_r=2.5,
                 max_hold_bars=30):
    """Standard ATR-based TP/SL with worst-case intrabar ordering."""
    tf_ms = TF_MS[tf]
    atr = signal['signal_atr14']
    if atr is None or atr <= 0: return None

    next_bar_ms = signal['signal_bar_ms'] + tf_ms
    entry_info = _entry_fill(reader, next_bar_ms, direction)
    if entry_info is None: return None
    entry_ts, entry_px, entry_ask, entry_bid = entry_info

    if direction == 'long':
        sl = entry_px - sl_atr * atr
        tp = entry_px + tp_r * sl_atr * atr
    else:
        sl = entry_px + sl_atr * atr
        tp = entry_px - tp_r * sl_atr * atr

    timeout_ts = entry_ts + max_hold_bars * tf_ms
    exit_ts = exit_px = exit_reason = None

    for ts, ask, bid in reader.iter_from(entry_ts + 1, max_ms=timeout_ts):
        if direction == 'long':
            if bid <= sl:
                exit_ts, exit_px, exit_reason = ts, bid, 'SL_HIT'; break
            if bid >= tp:
                exit_ts, exit_px, exit_reason = ts, bid, 'TP_HIT'; break
        else:
            if ask >= sl:
                exit_ts, exit_px, exit_reason = ts, ask, 'SL_HIT'; break
            if ask <= tp:
                exit_ts, exit_px, exit_reason = ts, ask, 'TP_HIT'; break
    if exit_ts is None:
        # Timed out — find first tick after timeout
        for ts, ask, bid in reader.iter_from(timeout_ts):
            exit_ts, exit_px = ts, (bid if direction=='long' else ask)
            exit_reason = 'TIMEOUT'
            break
        if exit_ts is None: return None

    pnl = exit_px - entry_px if direction == 'long' else entry_px - exit_px
    return _build_trade(signal, direction, entry_ts, entry_px, entry_ask, entry_bid,
                        sl, tp, exit_ts, exit_px, exit_reason, pnl, atr, sl_atr,
                        family='A')


# =============================================================================
# Family B: indicator exit + hard ATR SL (RSI revert, Bollinger)
# =============================================================================
def sim_family_b(reader, signal, direction, tf, hard_sl_atr=1.5,
                 max_hold_bars=20, exit_indicator_func=None):
    """exit_indicator_func(bar) -> True to exit. Hard SL is intrabar."""
    tf_ms = TF_MS[tf]
    atr = signal['signal_atr14']
    if atr is None or atr <= 0: return None

    next_bar_ms = signal['signal_bar_ms'] + tf_ms
    entry_info = _entry_fill(reader, next_bar_ms, direction)
    if entry_info is None: return None
    entry_ts, entry_px, entry_ask, entry_bid = entry_info

    if direction == 'long':
        sl = entry_px - hard_sl_atr * atr
    else:
        sl = entry_px + hard_sl_atr * atr
    tp = None  # no fixed TP in family B

    timeout_ts = entry_ts + max_hold_bars * tf_ms

    # We need to walk bar-by-bar to check the indicator exit, but ticks for SL.
    # Strategy: iterate through bars from the entry bar forward. For each bar,
    # walk its ticks for SL hit. If SL not hit and bar closes with indicator
    # exit signal, exit at next bar's open.
    exit_ts = exit_px = exit_reason = None
    bar_iter = signal.get('_bar_lookup_func')  # supplied by caller
    if bar_iter is None:
        # Fallback: just hard-SL or timeout
        for ts, ask, bid in reader.iter_from(entry_ts + 1, max_ms=timeout_ts):
            if direction == 'long' and bid <= sl:
                exit_ts, exit_px, exit_reason = ts, bid, 'SL_HIT'; break
            if direction == 'short' and ask >= sl:
                exit_ts, exit_px, exit_reason = ts, ask, 'SL_HIT'; break
        if exit_ts is None:
            for ts, ask, bid in reader.iter_from(timeout_ts):
                exit_ts, exit_px = ts, (bid if direction=='long' else ask)
                exit_reason = 'TIMEOUT'; break
            if exit_ts is None: return None
    else:
        # Walk bars forward; for each bar, check ticks for SL, then bar-close
        # for indicator
        bars = bar_iter()
        sig_idx = signal['signal_bar_idx']
        for bar_offset in range(1, max_hold_bars + 1):
            b_idx = sig_idx + bar_offset
            if b_idx >= len(bars): break
            b = bars[b_idx]
            bar_start = b['bar_start_ms']
            bar_end = bar_start + tf_ms
            # Walk ticks of this bar
            for ts, ask, bid in reader.iter_from(bar_start, max_ms=bar_end-1):
                if direction == 'long' and bid <= sl:
                    exit_ts, exit_px, exit_reason = ts, bid, 'SL_HIT'; break
                if direction == 'short' and ask >= sl:
                    exit_ts, exit_px, exit_reason = ts, ask, 'SL_HIT'; break
            if exit_ts is not None: break
            # Bar closed without SL: check indicator exit
            if exit_indicator_func(b, direction):
                # Exit at next bar open (or at this bar's last tick if no next)
                if b_idx + 1 < len(bars):
                    nb = bars[b_idx + 1]
                    for ts, ask, bid in reader.iter_from(nb['bar_start_ms']):
                        exit_ts, exit_px = ts, (bid if direction=='long' else ask)
                        exit_reason = 'INDICATOR'; break
                    if exit_ts is not None: break
        if exit_ts is None:
            # Timeout
            for ts, ask, bid in reader.iter_from(timeout_ts):
                exit_ts, exit_px = ts, (bid if direction=='long' else ask)
                exit_reason = 'TIMEOUT'; break
            if exit_ts is None: return None

    pnl = exit_px - entry_px if direction == 'long' else entry_px - exit_px
    return _build_trade(signal, direction, entry_ts, entry_px, entry_ask, entry_bid,
                        sl, tp, exit_ts, exit_px, exit_reason, pnl, atr, hard_sl_atr,
                        family='B')


# =============================================================================
# Family C: Time exit + hard ATR SL (TS momentum)
# =============================================================================
def sim_family_c(reader, signal, direction, tf, hold_bars=12, hard_sl_atr=3.0):
    """Hold N bars. Hard SL is intrabar."""
    tf_ms = TF_MS[tf]
    atr = signal['signal_atr14']
    if atr is None or atr <= 0: return None

    next_bar_ms = signal['signal_bar_ms'] + tf_ms
    entry_info = _entry_fill(reader, next_bar_ms, direction)
    if entry_info is None: return None
    entry_ts, entry_px, entry_ask, entry_bid = entry_info

    if direction == 'long':
        sl = entry_px - hard_sl_atr * atr
    else:
        sl = entry_px + hard_sl_atr * atr
    timeout_ts = entry_ts + hold_bars * tf_ms

    exit_ts = exit_px = exit_reason = None
    for ts, ask, bid in reader.iter_from(entry_ts + 1, max_ms=timeout_ts):
        if direction == 'long' and bid <= sl:
            exit_ts, exit_px, exit_reason = ts, bid, 'SL_HIT'; break
        if direction == 'short' and ask >= sl:
            exit_ts, exit_px, exit_reason = ts, ask, 'SL_HIT'; break
    if exit_ts is None:
        for ts, ask, bid in reader.iter_from(timeout_ts):
            exit_ts, exit_px = ts, (bid if direction=='long' else ask)
            exit_reason = 'TIME_EXIT'; break
        if exit_ts is None: return None

    pnl = exit_px - entry_px if direction == 'long' else entry_px - exit_px
    return _build_trade(signal, direction, entry_ts, entry_px, entry_ask, entry_bid,
                        sl, None, exit_ts, exit_px, exit_reason, pnl, atr, hard_sl_atr,
                        family='C')


# =============================================================================
# Family D: Range exit + hard ATR SL (Asian break)
# =============================================================================
def sim_family_d(reader, signal, direction, tf, hard_sl_atr=1.0,
                 ny_close_hour=21):
    """Exit at end of NY session (21:00 UTC) or hard SL."""
    tf_ms = TF_MS[tf]
    atr = signal['signal_atr14']
    if atr is None or atr <= 0: return None

    next_bar_ms = signal['signal_bar_ms'] + tf_ms
    entry_info = _entry_fill(reader, next_bar_ms, direction)
    if entry_info is None: return None
    entry_ts, entry_px, entry_ask, entry_bid = entry_info

    if direction == 'long':
        sl = entry_px - hard_sl_atr * atr
    else:
        sl = entry_px + hard_sl_atr * atr

    # NY close timestamp = same calendar day at 21:00 UTC, or next day if past
    entry_dt = datetime.fromtimestamp(entry_ts/1000, tz=timezone.utc)
    if entry_dt.hour >= ny_close_hour:
        # roll to next day
        ny_close_dt = entry_dt.replace(hour=ny_close_hour, minute=0, second=0,
                                       microsecond=0)
        from datetime import timedelta
        ny_close_dt += timedelta(days=1)
    else:
        ny_close_dt = entry_dt.replace(hour=ny_close_hour, minute=0, second=0,
                                       microsecond=0)
    ny_close_ms = int(ny_close_dt.timestamp() * 1000)

    exit_ts = exit_px = exit_reason = None
    for ts, ask, bid in reader.iter_from(entry_ts + 1, max_ms=ny_close_ms):
        if direction == 'long' and bid <= sl:
            exit_ts, exit_px, exit_reason = ts, bid, 'SL_HIT'; break
        if direction == 'short' and ask >= sl:
            exit_ts, exit_px, exit_reason = ts, ask, 'SL_HIT'; break
    if exit_ts is None:
        for ts, ask, bid in reader.iter_from(ny_close_ms):
            exit_ts, exit_px = ts, (bid if direction=='long' else ask)
            exit_reason = 'NY_CLOSE'; break
        if exit_ts is None: return None

    pnl = exit_px - entry_px if direction == 'long' else entry_px - exit_px
    return _build_trade(signal, direction, entry_ts, entry_px, entry_ask, entry_bid,
                        sl, None, exit_ts, exit_px, exit_reason, pnl, atr, hard_sl_atr,
                        family='D')


# =============================================================================
# Common: build trade record + apply costs
# =============================================================================
def _build_trade(signal, direction, entry_ts, entry_px, entry_ask, entry_bid,
                 sl, tp, exit_ts, exit_px, exit_reason, pnl, atr, sl_mult,
                 family):
    return {
        'signal_bar_ms': signal['signal_bar_ms'],
        'signal_bar_iso': signal['signal_bar_iso'],
        'family': family,
        'direction': direction,
        'entry_ts': entry_ts,
        'entry_iso': datetime.fromtimestamp(entry_ts/1000, tz=timezone.utc).isoformat(),
        'entry_px': entry_px,
        'entry_ask_at_fill': entry_ask,
        'entry_bid_at_fill': entry_bid,
        'sl': sl,
        'tp': tp,
        'atr14': atr,
        'sl_mult': sl_mult,
        'exit_ts': exit_ts,
        'exit_iso': datetime.fromtimestamp(exit_ts/1000, tz=timezone.utc).isoformat(),
        'exit_px': exit_px,
        'exit_reason': exit_reason,
        'hold_sec': (exit_ts - entry_ts) / 1000.0,
        'pnl_pts': pnl,
        'r_unit': abs(entry_px - sl),
        'r_multiple': pnl / abs(entry_px - sl) if abs(entry_px - sl) > 0 else 0,
        'trend': signal.get('trend'),
        'vol_relative': signal.get('vol_relative'),
        'vol_absolute': signal.get('vol_absolute'),
        'session': signal.get('session'),
        'spike': signal.get('spike'),
    }


def apply_costs(trades, reader, m15_bars, m15_starts, commission=COMMISSION_RT):
    """Annotate trades with NET and STRESS pnl. Mutates in place."""
    for t in trades:
        # Actual entry spread (we have ask+bid at entry)
        t['entry_spread_actual'] = t['entry_ask_at_fill'] - t['entry_bid_at_fill']
        # Actual exit spread
        ea, eb = reader.find_quote(t['exit_ts'])
        if ea is None:
            mb_idx = bisect_right(m15_starts, t['exit_ts']) - 1
            est = m15_bars[mb_idx]['sp_med'] if mb_idx >= 0 else 0.5
            t['exit_spread_actual'] = est
        else:
            t['exit_spread_actual'] = ea - eb
        # M15 sp_p99 lookups
        ent_idx = bisect_right(m15_starts, t['entry_ts']) - 1
        ext_idx = bisect_right(m15_starts, t['exit_ts']) - 1
        sp_p99_ent = m15_bars[ent_idx]['sp_p99'] if ent_idx >= 0 else t['entry_spread_actual']
        sp_p99_ext = m15_bars[ext_idx]['sp_p99'] if ext_idx >= 0 else t['exit_spread_actual']
        t['sp_p99_entry'] = sp_p99_ent
        t['sp_p99_exit']  = sp_p99_ext
        # NET / STRESS
        t['pnl_pts_net'] = t['pnl_pts'] - commission
        extra = max(0, sp_p99_ent - t['entry_spread_actual']) + \
                max(0, sp_p99_ext - t['exit_spread_actual'])
        t['extra_spread_at_p99'] = extra
        t['pnl_pts_stress'] = t['pnl_pts'] - commission - extra
        ru = t['r_unit']
        t['r_multiple_net']    = t['pnl_pts_net'] / ru if ru > 0 else 0
        t['r_multiple_stress'] = t['pnl_pts_stress'] / ru if ru > 0 else 0

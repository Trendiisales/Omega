#!/usr/bin/env python3
"""
phase1/build_donchian_H1_long_retuned.py

Builds a Donchian H1 long trade ledger using the RETUNED params from the
v3 post-regime sweep:

    sweep verdict:        (period=20, sl_atr=3.0, tp_r=5.0)   [sweep vocab]
    sim_family_a equivalent: sl_atr=3.0, tp_r=1.6667          [TP = 5.0 ATR]

The signal file is unchanged (period=20, cooldown=5 — already byte-exact
validated, 509 signals at phase1/signals/donchian_H1_long.parquet).
Only the EXIT geometry changes:

    canonical:     SL = 1.0 ATR    TP = 2.5 ATR   (sl_atr=1.0, tp_r=2.5)
    retuned:       SL = 3.0 ATR    TP = 5.0 ATR   (sl_atr=3.0, tp_r=1.6667)

Why tp_r=1.6667 and not 5.0:
    sim_family_a defines  tp = entry + tp_r * sl_atr * atr
    For TP = 5.0 ATR with sl_atr = 3.0:  tp_r = 5.0 / 3.0 = 1.6667

This script reuses the canonical pipeline:
    sim_family_a()  → tick-walk fill, intrabar SL/TP
    apply_costs()   → commission + spread → pnl_pts_net

Output:
    phase1/trades_net/donchian_H1_long_3.0_5.0_net.parquet

The portfolio sim consumes pnl_pts_net, so the new ledger is directly
A/B-comparable to the canonical donchian_H1_long_net.parquet.

Rules followed:
    - No core-code modification (sim_lib.py untouched)
    - Full file, no patches
    - Same simulator, same cost model, same signal file as canonical
    - Output filename does NOT overwrite canonical
"""
from __future__ import annotations
import os
import sys
import time

sys.path.insert(0, '/Users/jo/omega_repo/phase1')

import pyarrow.parquet as pq
import pyarrow as pa

from sim_lib import (TickReader, sim_family_a, apply_costs,
                     PHASE0, PHASE1)

# -----------------------------------------------------------------------------
# Config — locked from CHOSEN.md verdict
# -----------------------------------------------------------------------------
SIGNAL_PATH    = os.path.join(PHASE1, 'signals', 'donchian_H1_long.parquet')
OUT_PATH       = os.path.join(PHASE1, 'trades_net',
                              'donchian_H1_long_3.0_5.0_net.parquet')
M15_BARS_PATH  = os.path.join(PHASE0, 'bars_M15_final.parquet')

# sim_family_a vocab — see header for derivation
SL_ATR         = 3.0
TP_R           = 5.0 / 3.0       # = 1.6666666... so that TP = 5.0 ATR
MAX_HOLD_BARS  = 30              # canonical default in run_all_backtests.py
TF             = 'H1'
DIRECTION      = 'long'

# Sanity: confirm the geometry
assert abs(TP_R * SL_ATR - 5.0) < 1e-9, \
    f"TP geometry wrong: tp_r({TP_R}) * sl_atr({SL_ATR}) = {TP_R*SL_ATR}, expected 5.0"


def main() -> None:
    print("=" * 72)
    print("Donchian H1 long — RETUNED ledger build")
    print("=" * 72)
    print(f"  signal file    : {SIGNAL_PATH}")
    print(f"  output         : {OUT_PATH}")
    print(f"  sl_atr         : {SL_ATR}        (SL = {SL_ATR:.1f} ATR below entry)")
    print(f"  tp_r           : {TP_R:.6f}  (TP = {TP_R*SL_ATR:.1f} ATR above entry)")
    print(f"  max_hold_bars  : {MAX_HOLD_BARS}")
    print(f"  timeframe      : {TF}")
    print(f"  direction      : {DIRECTION}")
    print()

    if not os.path.isfile(SIGNAL_PATH):
        sys.exit(f"FATAL: signal file missing: {SIGNAL_PATH}")
    if not os.path.isfile(M15_BARS_PATH):
        sys.exit(f"FATAL: M15 bars missing: {M15_BARS_PATH}")

    print("Loading signals...")
    signals = pq.read_table(SIGNAL_PATH).to_pylist()
    print(f"  {len(signals)} signals")

    print("Loading M15 bars (for cost lookup)...")
    m15_bars = pq.read_table(M15_BARS_PATH).to_pylist()
    m15_starts = [b['bar_start_ms'] for b in m15_bars]
    print(f"  {len(m15_bars)} M15 bars")

    print("Opening tick reader...")
    reader = TickReader()

    print("Backtesting (tick-walk intrabar)...")
    t0 = time.time()
    trades = []
    skipped = 0
    for i, s in enumerate(signals):
        tr = sim_family_a(reader, s, DIRECTION, TF,
                          sl_atr=SL_ATR, tp_r=TP_R,
                          max_hold_bars=MAX_HOLD_BARS)
        if tr:
            trades.append(tr)
        else:
            skipped += 1
        if (i + 1) % 50 == 0 or (i + 1) == len(signals):
            print(f"  [{i+1:>4}/{len(signals)}]  trades={len(trades):>4}  "
                  f"skipped={skipped:>3}  ({time.time()-t0:.1f}s)")

    if not trades:
        reader.close()
        sys.exit("FATAL: no trades produced")

    print()
    print("Applying costs (commission + spread)...")
    apply_costs(trades, reader, m15_bars, m15_starts)
    reader.close()

    # Strip non-serializable fields (mirror of run_all_backtests.py)
    for tr in trades:
        tr.pop('_bar_lookup_func', None)

    print(f"Writing {len(trades)} trades to {OUT_PATH}...")
    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    pq.write_table(pa.Table.from_pylist(trades), OUT_PATH)

    # Summary
    n = len(trades)
    wins = sum(1 for t in trades if t['pnl_pts_net'] > 0)
    tot_g = sum(t['pnl_pts'] for t in trades)
    tot_n = sum(t['pnl_pts_net'] for t in trades)
    tot_s = sum(t['pnl_pts_stress'] for t in trades)
    pos_n = sum(t['pnl_pts_net'] for t in trades if t['pnl_pts_net'] > 0)
    neg_n = abs(sum(t['pnl_pts_net'] for t in trades if t['pnl_pts_net'] < 0))
    pf_n = (pos_n / neg_n) if neg_n > 0 else float('inf')

    print()
    print("=" * 72)
    print("Summary")
    print("=" * 72)
    print(f"  trades         : {n}")
    print(f"  skipped        : {skipped}  (no atr14 / no entry fill)")
    print(f"  win rate       : {100*wins/n:.1f}%")
    print(f"  gross pnl_pts  : {tot_g:+.2f}")
    print(f"  net   pnl_pts  : {tot_n:+.2f}")
    print(f"  stress pnl_pts : {tot_s:+.2f}")
    pf_str = f"{pf_n:.3f}" if pf_n != float('inf') else "inf"
    print(f"  pf (net)       : {pf_str}")
    print(f"  avg trade net  : {tot_n/n:+.4f}")
    print()
    print(f"  output         : {OUT_PATH}")
    print()
    print("Next: re-run phase2/portfolio_C1_C2.py to A/B compare baseline vs retuned.")


if __name__ == '__main__':
    try:
        main()
    except Exception as e:
        import traceback
        traceback.print_exc()
        print(f"\nFATAL: {type(e).__name__}: {e}", file=sys.stderr)
        sys.exit(1)

#!/usr/bin/env python3
"""Splice the two certified MGC 30m sources into one continuous series.

Sources (both individually CERTIFIED CLEAN by backtest/data_integrity_gate.py):
  /Users/jo/Tick/mgc_30m_hist.csv        2024-06-03 .. 2026-06-03  (23,616 rows)
  /Users/jo/Omega/data/mgc_30m_hist.csv  2026-05-13 .. 2026-07-13  (1,957 rows, nightly refresh)

Overlap 2026-05-13..2026-06-03 (666 bars): close diff median 0.3pt, max 4.0pt
(~0.09% — two IBKR CONTFUT pulls at different times). Newer file wins on overlap.
Output MUST be re-gated (data_integrity_gate.py) before any backtest uses it.
Verdict 2026-07-14: CERTIFIED CLEAN (24,907 rows, 2024-06-03..2026-07-13).
"""
import csv, sys, datetime

OLD = '/Users/jo/Tick/mgc_30m_hist.csv'
NEW = '/Users/jo/Omega/data/mgc_30m_hist.csv'
OUT = '/Users/jo/Omega/backtest/data/mgc_30m_spliced_2024_2026.csv'

def load(p):
    d = {}
    with open(p) as f:
        r = csv.reader(f); next(r)
        for row in r:
            d[int(row[0])] = tuple(float(x) for x in row[1:5])
    return d

def main():
    a, b = load(OLD), load(NEW)
    merged = {}
    merged.update(a); merged.update(b)   # newer file wins on overlap
    ts_sorted = sorted(merged)
    with open(OUT, 'w') as f:
        f.write('ts,o,h,l,c\n')
        for t in ts_sorted:
            o, h, l, c = merged[t]
            f.write(f'{t},{o},{h},{l},{c}\n')
    lo = datetime.datetime.utcfromtimestamp(ts_sorted[0])
    hi = datetime.datetime.utcfromtimestamp(ts_sorted[-1])
    print(f'wrote {OUT}: {len(ts_sorted)} rows  {lo} .. {hi}')
    print('NOW RUN: python3 backtest/data_integrity_gate.py', OUT)

if __name__ == '__main__':
    main()

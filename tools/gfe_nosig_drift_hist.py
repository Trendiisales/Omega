#!/usr/bin/env python3
"""
gfe_nosig_drift_hist.py  --  Count |drift| distribution across GFE-NOSIG lines.

Purpose: quantify how many NOSIG rejections would have been entries under
different drift threshold values, so the threshold change is evidence-based.

Usage on VPS:
    python tools\\gfe_nosig_drift_hist.py --log C:\\Omega\\logs\\tmp_21\\omega_2026-04-21.log

Optional --window-start / --window-end to restrict to the down-move window
(HH:MM:SS UTC).
"""

import argparse
import re
import sys


RE_NOSIG = re.compile(
    r'^(?P<ts>\d{2}:\d{2}:\d{2}) \[GFE-NOSIG\] drift=(?P<drift>-?[\d.]+)'
    r'.*?atr=(?P<atr>-?[\d.]+)'
    r'.*?eff_thresh=(?P<eff>-?[\d.]+)'
    r'.*?window=(?P<win>\d+)/(?P<winmax>\d+)'
)


def ts_to_sec(ts):
    h, m, s = ts.split(':')
    return int(h) * 3600 + int(m) * 60 + int(s)


def run(log_path, w_start, w_end):
    start_sec = ts_to_sec(w_start) if w_start else None
    end_sec = ts_to_sec(w_end) if w_end else None

    total = 0
    in_window = 0
    d_buckets = {
        '>=0.3': 0,
        '>=0.5': 0,
        '>=0.8': 0,
        '>=1.0': 0,
        '>=1.2': 0,
        '>=1.5': 0,
        '>=2.0': 0,
    }
    max_drift_seen = 0.0
    atr_seen = []

    with open(log_path, 'r', encoding='utf-8', errors='replace') as fh:
        for raw in fh:
            m = RE_NOSIG.match(raw.rstrip('\n'))
            if not m:
                continue
            ts_sec = ts_to_sec(m.group('ts'))
            if start_sec is not None and ts_sec < start_sec:
                continue
            if end_sec is not None and ts_sec > end_sec:
                continue

            total += 1
            d = abs(float(m.group('drift')))
            atr = float(m.group('atr'))
            atr_seen.append(atr)
            if d > max_drift_seen:
                max_drift_seen = d
            if d >= 0.3:
                d_buckets['>=0.3'] += 1
            if d >= 0.5:
                d_buckets['>=0.5'] += 1
            if d >= 0.8:
                d_buckets['>=0.8'] += 1
            if d >= 1.0:
                d_buckets['>=1.0'] += 1
            if d >= 1.2:
                d_buckets['>=1.2'] += 1
            if d >= 1.5:
                d_buckets['>=1.5'] += 1
            if d >= 2.0:
                d_buckets['>=2.0'] += 1

    print("Log:      {}".format(log_path))
    if start_sec is not None or end_sec is not None:
        print("Window:   {} -> {}".format(w_start or '(start)', w_end or '(end)'))
    print("Total GFE-NOSIG samples: {}".format(total))
    if total == 0:
        print("No samples matched.")
        return 0
    print("Max |drift| seen:        {:.3f}".format(max_drift_seen))
    if atr_seen:
        atr_mean = sum(atr_seen) / len(atr_seen)
        print("Mean ATR across samples: {:.3f}".format(atr_mean))
    print("")
    print("|drift| >= threshold distribution:")
    for k in ['>=0.3', '>=0.5', '>=0.8', '>=1.0', '>=1.2', '>=1.5', '>=2.0']:
        v = d_buckets[k]
        pct = 100.0 * v / total
        print("  {:<6} {:>5} ({:.1f}%)".format(k, v, pct))
    print("")
    print("Interpretation:")
    print("  Each bucket = how many NOSIG rejections had |drift| above that value.")
    print("  A strong directional move should see a heavy tail at higher buckets.")
    print("  If >=0.8 is high but >=1.5 is low, the 1.5 threshold is filtering out")
    print("  the move itself, not just noise.")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--log', required=True)
    ap.add_argument('--window-start', default=None,
                    help='HH:MM:SS UTC start of window (optional)')
    ap.add_argument('--window-end', default=None,
                    help='HH:MM:SS UTC end of window (optional)')
    args = ap.parse_args()
    sys.exit(run(args.log, args.window_start, args.window_end))


if __name__ == '__main__':
    main()

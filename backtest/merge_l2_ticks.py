#!/usr/bin/env python3
"""
merge_l2_ticks.py -- Merge price tick CSV with L2 DOM data.

Input:
  ~/Tick/2yr_XAUUSD_tick.csv    -- price ticks (ts, bid, ask)
  ~/Tick/l2_data/l2_ticks_*.csv -- L2 DOM data (ts_ms, l2_imb, depth_bid, depth_ask, etc.)

Output:
  ~/Tick/merged_ticks.csv -- price + L2, forward-filled L2 on price ticks

Usage:
  python3 merge_l2_ticks.py
  python3 merge_l2_ticks.py --price ~/Tick/2yr_XAUUSD_tick.csv --l2dir ~/Tick/l2_data/
"""

import sys, os, csv, glob, argparse
from datetime import datetime

def ymdhms_to_ms(date_s, time_s):
    y=int(date_s[:4]); mo=int(date_s[4:6]); dy=int(date_s[6:8])
    h=int(time_s[:2]); mi=int(time_s[3:5]); se=int(time_s[6:8])
    if mo<=2: y-=1; mo+=12
    days=365*y+y//4-y//100+y//400+(153*mo+8)//5+dy-719469
    return (days*86400+h*3600+mi*60+se)*1000

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--price', default=os.path.expanduser('~/Tick/2yr_XAUUSD_tick.csv'))
    parser.add_argument('--l2dir', default=os.path.expanduser('~/Tick/l2_data/'))
    parser.add_argument('--out',   default=os.path.expanduser('~/Tick/merged_ticks.csv'))
    args = parser.parse_args()

    # Load L2 data into sorted list
    print(f"Loading L2 data from {args.l2dir}...")
    l2_files = sorted(glob.glob(os.path.join(args.l2dir, 'l2_ticks_*.csv')))
    if not l2_files:
        print(f"No L2 files found in {args.l2dir}")
        print("Copy from VPS first:")
        print("  scp trader@185.167.119.59:\"C:/Omega/logs/l2_ticks_*.csv\" ~/Tick/l2_data/")
        sys.exit(1)

    # L2 data: list of (ts_ms, imb, bid_lvls, ask_lvls, ewm_drift)
    l2_data = []
    for fpath in l2_files:
        print(f"  {os.path.basename(fpath)}")
        with open(fpath) as f:
            reader = csv.DictReader(f)
            for row in reader:
                try:
                    ts  = int(row['ts_ms'])
                    imb = float(row['l2_imb'])
                    bl  = int(row['depth_bid_levels'])
                    al  = int(row['depth_ask_levels'])
                    drift = float(row.get('ewm_drift', 0))
                    l2_data.append((ts, imb, bl, al, drift))
                except: continue

    l2_data.sort(key=lambda x: x[0])
    print(f"  {len(l2_data):,} L2 rows loaded")

    if not l2_data:
        print("No L2 data loaded")
        sys.exit(1)

    l2_start = datetime.utcfromtimestamp(l2_data[0][0]/1000).strftime('%Y-%m-%d')
    l2_end   = datetime.utcfromtimestamp(l2_data[-1][0]/1000).strftime('%Y-%m-%d')
    print(f"  L2 range: {l2_start} to {l2_end}")

    # Merge price ticks with L2 -- forward fill L2 on each price tick
    print(f"\nMerging with price data from {args.price}...")
    l2_start_ms = l2_data[0][0]
    l2_end_ms   = l2_data[-1][0]

    l2_idx = 0
    l2_len = len(l2_data)
    cur_imb = 0.5; cur_bl = 0; cur_al = 0; cur_drift = 0.0

    written = 0; skipped = 0

    with open(args.price) as fin, open(args.out, 'w', newline='') as fout:
        writer = csv.writer(fout)
        writer.writerow(['ts_ms','bid','ask','l2_imb','depth_bid','depth_ask','ewm_drift'])

        for line in fin:
            line = line.strip()
            if not line: continue
            parts = line.split(',')
            if len(parts) < 4: continue
            try:
                bid = float(parts[2]); ask = float(parts[3])
                ts  = ymdhms_to_ms(parts[0], parts[1])
            except: continue

            # Only output rows within L2 date range
            if ts < l2_start_ms or ts > l2_end_ms:
                skipped += 1
                continue

            # Advance L2 pointer to best match for this timestamp
            while l2_idx < l2_len - 1 and l2_data[l2_idx+1][0] <= ts:
                l2_idx += 1
            if l2_idx < l2_len:
                cur_imb, cur_bl, cur_al, cur_drift = l2_data[l2_idx][1:]

            writer.writerow([ts, f'{bid:.3f}', f'{ask:.3f}',
                             f'{cur_imb:.4f}', cur_bl, cur_al, f'{cur_drift:.4f}'])
            written += 1
            if written % 5_000_000 == 0:
                print(f"  {written:,} rows written...")

    print(f"\nDone. {written:,} rows written to {args.out}")
    print(f"Skipped {skipped:,} rows outside L2 date range")

if __name__ == '__main__':
    main()

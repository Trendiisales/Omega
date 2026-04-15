#!/usr/bin/env python3
"""
Deep analysis of the imb>0.70 signal -- the only bucket with real edge.
Find exactly what conditions produce imb>0.70 and what the optimal entry looks like.
"""

import sys, os, csv, glob, math, collections, datetime

L2_DIR = os.path.expanduser("~/Tick/l2_data/")
SPREAD = 0.25

files = sorted(glob.glob(os.path.join(L2_DIR, 'l2_ticks_*.csv')))
# Skip the split part files -- use the merged one
files = [f for f in files if 'part' not in f]

print(f"Loading {len(files)} files...")
rows = []
for fpath in files:
    with open(fpath) as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                rows.append({
                    'ts':    int(row['ts_ms']),
                    'bid':   float(row['bid']),
                    'ask':   float(row['ask']),
                    'imb':   float(row['l2_imb']),
                    'drift': float(row.get('ewm_drift', 0)),
                    'bl':    int(row['depth_bid_levels']),
                    'al':    int(row['depth_ask_levels']),
                    'vpin':  float(row.get('vpin', 0)),
                    'vol':   float(row.get('vol_ratio', 1)),
                })
            except: continue

rows.sort(key=lambda x: x['ts'])
n = len(rows)
print(f"Loaded {n:,} rows")

# -- What produces imb > 0.70? ------------------------------------------------
print("\n=== IMB > 0.70 breakdown ===")
high_imb = [r for r in rows if r['imb'] > 0.70]
print(f"Count: {len(high_imb)} ({len(high_imb)/n*100:.3f}% of all rows)")

# Bid/ask level distribution
bl_dist = collections.Counter(r['bl'] for r in high_imb)
al_dist = collections.Counter(r['al'] for r in high_imb)
print(f"Bid levels: {dict(sorted(bl_dist.items()))}")
print(f"Ask levels: {dict(sorted(al_dist.items()))}")

# Drift distribution
drifts = [r['drift'] for r in high_imb]
print(f"Drift: min={min(drifts):.2f} max={max(drifts):.2f} mean={sum(drifts)/len(drifts):.2f}")

# Hour distribution
by_hour = collections.Counter(r['ts']//1000//3600%24 for r in high_imb)
print(f"By UTC hour: {dict(sorted(by_hour.items()))}")

# -- Forward return for imb>0.70 at various horizons --------------------------
print("\n=== IMB > 0.70 forward returns ===")
for hz_s in [1, 5, 10, 30, 60, 120, 300]:
    hz_ms = hz_s * 1000
    pnls = []
    j = 0
    for r in high_imb:
        while j < n-1 and rows[j]['ts'] < r['ts'] + hz_ms:
            j += 1
        if abs(rows[j]['ts'] - (r['ts'] + hz_ms)) > hz_ms:
            continue
        fwd = (rows[j]['bid'] + rows[j]['ask'])/2 - (r['bid'] + r['ask'])/2
        # Direction: imb>0.70 = more bids = LONG signal
        pnls.append(fwd)
    if not pnls: continue
    wins = sum(1 for p in pnls if p > SPREAD)
    mean = sum(pnls)/len(pnls)
    ev = mean - SPREAD
    std = math.sqrt(sum((p-mean)**2 for p in pnls)/len(pnls))
    t = mean / (std/math.sqrt(len(pnls))) if std > 0 else 0
    print(f"  {hz_s:>4}s: n={len(pnls):>4} mean={mean:>+7.4f} EV={ev:>+7.4f} WR={wins/len(pnls)*100:>5.1f}% t={t:>+6.2f}")

# -- LOW imb < 0.30 (SHORT signal) -------------------------------------------
low_imb = [r for r in rows if r['imb'] < 0.30]
print(f"\n=== IMB < 0.30 (SHORT) count: {len(low_imb)} ===")
if low_imb:
    for hz_s in [30, 60, 120, 300]:
        hz_ms = hz_s * 1000
        pnls = []
        j = 0
        for r in low_imb:
            while j < n-1 and rows[j]['ts'] < r['ts'] + hz_ms:
                j += 1
            if abs(rows[j]['ts'] - (r['ts'] + hz_ms)) > hz_ms:
                continue
            fwd = (rows[j]['bid'] + rows[j]['ask'])/2 - (r['bid'] + r['ask'])/2
            pnls.append(-fwd)  # SHORT: negative return = win
        if not pnls: continue
        wins = sum(1 for p in pnls if p > SPREAD)
        mean = sum(pnls)/len(pnls)
        ev = mean - SPREAD
        print(f"  {hz_s:>4}s: n={len(pnls):>4} mean={mean:>+7.4f} EV={ev:>+7.4f} WR={wins/len(pnls)*100:>5.1f}%")

# -- Optimal entry: imb>0.70 AND drift confirms direction ---------------------
print("\n=== IMB > 0.70 + DRIFT confirmation ===")
confirmed = [r for r in high_imb if r['drift'] > 0]
against   = [r for r in high_imb if r['drift'] <= 0]
print(f"With drift>0 (confirmed LONG): {len(confirmed)}")
print(f"Against drift (drift<=0):      {len(against)}")

for label, subset in [("CONFIRMED (imb>0.70 + drift>0)", confirmed),
                       ("AGAINST   (imb>0.70 + drift<=0)", against)]:
    if not subset: continue
    hz_ms = 60000
    pnls = []
    j = 0
    for r in subset:
        while j < n-1 and rows[j]['ts'] < r['ts'] + hz_ms:
            j += 1
        if abs(rows[j]['ts'] - (r['ts'] + hz_ms)) > hz_ms:
            continue
        fwd = (rows[j]['bid'] + rows[j]['ask'])/2 - (r['bid'] + r['ask'])/2
        pnls.append(fwd)
    if not pnls: continue
    mean = sum(pnls)/len(pnls)
    wins = sum(1 for p in pnls if p > SPREAD)
    print(f"  {label}: n={len(pnls)} mean={mean:>+7.4f} EV={mean-SPREAD:>+7.4f} WR={wins/len(pnls)*100:.1f}%")

# -- Show actual imb>0.70 events with context ---------------------------------
print("\n=== Sample of imb>0.70 events ===")
for r in high_imb[:20]:
    dt = datetime.datetime.utcfromtimestamp(r['ts']/1000)
    print(f"  {dt.strftime('%m-%d %H:%M:%S')} bid={r['bid']:.2f} ask={r['ask']:.2f} "
          f"imb={r['imb']:.4f} bl={r['bl']} al={r['al']} drift={r['drift']:>+.3f}")


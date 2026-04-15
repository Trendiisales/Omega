#!/usr/bin/env python3
"""
analyse_l2.py -- What does real L2 imbalance actually predict?

Read the real L2 CSV files from the VPS and test:
1. Does l2_imbalance > 0.6 predict upward price movement?
2. Does l2_imbalance < 0.4 predict downward price movement?
3. What is the actual predictive horizon?
4. What EV after spread?

This is the only untested signal -- everything else has been proven to lose.
"""

import sys, os, csv, glob, math, collections

L2_DIR   = os.path.expanduser("~/Tick/l2_data/")
SPREAD   = 0.25
HORIZONS = [1, 5, 10, 30, 60, 120, 300]  # seconds

class Stat:
    def __init__(self, l): self.l=l; self.n=0; self.s=0.0; self.s2=0.0; self.w=0
    def add(self, r):
        self.n+=1; self.s+=r; self.s2+=r*r
        if r > SPREAD: self.w+=1
    def mean(self): return self.s/self.n if self.n else 0
    def std(self):
        if self.n<2: return 0
        return math.sqrt(max(self.s2/self.n - (self.s/self.n)**2, 0))
    def t(self): s=self.std(); return self.mean()/(s/math.sqrt(self.n)) if s and self.n>1 else 0
    def ev(self): return self.mean() - SPREAD
    def wr(self): return self.w/self.n*100 if self.n else 0

files = sorted(glob.glob(os.path.join(L2_DIR, 'l2_ticks_*.csv')))
if not files:
    print(f"No L2 files in {L2_DIR}")
    print("Run: scp trader@185.167.119.59:\"C:/Omega/logs/l2_ticks_*.csv\" ~/Tick/l2_data/")
    sys.exit(1)

print(f"Loading {len(files)} L2 files...")
rows = []
for fpath in files:
    print(f"  {os.path.basename(fpath)}")
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
                })
            except: continue

rows.sort(key=lambda x: x['ts'])
print(f"Loaded {len(rows):,} rows")

if len(rows) < 1000:
    print("Not enough data")
    sys.exit(1)

ts_start = rows[0]['ts']/1000
ts_end   = rows[-1]['ts']/1000
print(f"Period: {__import__('datetime').datetime.utcfromtimestamp(ts_start)} to "
      f"{__import__('datetime').datetime.utcfromtimestamp(ts_end)}")

# Build forward return index
print("Computing forward returns...")
n = len(rows)

# For each horizon, bucket imbalance and measure directional return
for hz in HORIZONS:
    bkts = {}
    hz_ms = hz * 1000
    j = 0
    for i, row in enumerate(rows):
        # Advance j to find row at ts + hz
        while j < n-1 and rows[j]['ts'] < row['ts'] + hz_ms:
            j += 1
        if rows[j]['ts'] < row['ts'] + hz_ms/2:
            continue
        fwd = (rows[j]['bid'] + rows[j]['ask'])/2 - (row['bid'] + row['ask'])/2

        imb = row['imb']
        # Bucket imbalance
        if   imb > 0.70: k = "imb>0.70 LONG"
        elif imb > 0.60: k = "imb 0.60-0.70 LONG"
        elif imb > 0.55: k = "imb 0.55-0.60 LONG"
        elif imb < 0.30: k = "imb<0.30 SHORT"
        elif imb < 0.40: k = "imb 0.30-0.40 SHORT"
        elif imb < 0.45: k = "imb 0.40-0.45 SHORT"
        else:             k = "imb 0.45-0.55 NEUTRAL"

        if k not in bkts: bkts[k] = Stat(k)
        # Return in signal direction
        if "LONG" in k:   bkts[k].add(fwd)
        elif "SHORT" in k: bkts[k].add(-fwd)
        else:              bkts[k].add(abs(fwd))

    print(f"\n{'─'*65}")
    print(f"L2 IMBALANCE vs Forward Return [{hz}s]")
    print(f"{'─'*65}")
    print(f"  {'Bucket':<28} {'N':>7} {'Mean':>7} {'EV':>7} {'WR%':>6} {'t':>7}")
    stats = sorted(bkts.values(), key=lambda s: s.ev(), reverse=True)
    for s in stats:
        if s.n < 10: continue
        mk = " ★" if abs(s.t()) > 2.0 else ""
        print(f"  {s.l:<28} {s.n:>7} {s.mean():>+7.4f} {s.ev():>+7.4f} {s.wr():>5.1f}% {s.t():>+7.2f}{mk}")

# Also test: does high imbalance + inside daily range predict better?
print(f"\n{'─'*65}")
print(f"L2 IMBALANCE + DRIFT COMBO [30s]")
print(f"{'─'*65}")
bkts2 = {}
hz_ms = 30000
j = 0
for i, row in enumerate(rows):
    while j < n-1 and rows[j]['ts'] < row['ts'] + hz_ms:
        j += 1
    if rows[j]['ts'] < row['ts'] + hz_ms/2: continue
    fwd = (rows[j]['bid']+rows[j]['ask'])/2 - (row['bid']+row['ask'])/2

    imb = row['imb']; drift = row['drift']
    if   imb > 0.60 and drift > 0.5:  k = "imb>0.6 + drift+ LONG (confluence)"
    elif imb < 0.40 and drift < -0.5: k = "imb<0.4 + drift- SHORT (confluence)"
    elif imb > 0.60 and drift < -0.5: k = "imb>0.6 vs drift- (conflict LONG)"
    elif imb < 0.40 and drift > 0.5:  k = "imb<0.4 vs drift+ (conflict SHORT)"
    elif imb > 0.60:                   k = "imb>0.6 only LONG"
    elif imb < 0.40:                   k = "imb<0.4 only SHORT"
    else: continue
    if k not in bkts2: bkts2[k] = Stat(k)
    if "SHORT" in k: bkts2[k].add(-fwd)
    else:             bkts2[k].add(fwd)

stats2 = sorted(bkts2.values(), key=lambda s: s.ev(), reverse=True)
print(f"  {'Bucket':<42} {'N':>7} {'Mean':>7} {'EV':>7} {'WR%':>6} {'t':>7}")
for s in stats2:
    if s.n < 10: continue
    mk = " ★" if abs(s.t()) > 2.0 else ""
    print(f"  {s.l:<42} {s.n:>7} {s.mean():>+7.4f} {s.ev():>+7.4f} {s.wr():>5.1f}% {s.t():>+7.2f}{mk}")


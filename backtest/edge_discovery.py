#!/usr/bin/env python3
"""
Edge discovery on 2yr XAUUSD tick data.
Methodology:
1. Compute a set of candidate features at each bar close
2. For each feature, measure forward return over N periods
3. Find which features have statistically significant predictive power
4. Show exact threshold values and expected value per trade

Features tested:
- EWM drift (various halflife windows: 10s, 30s, 120s, 300s)
- Drift momentum (is drift accelerating or decelerating)
- RSI level + direction
- ATR regime (low/med/high vol)
- Session (UTC hour)
- VWAP distance (above/below by how much)
- Price vs recent high/low
- Bar body ratio + direction

Forward return windows: 30s, 60s, 120s, 300s
"""

import sys, csv, math, datetime, collections
from dataclasses import dataclass, field
from typing import List

DATA_FILE = "/Users/jo/Tick/2yr_XAUUSD_tick.csv"
if len(sys.argv) > 1:
    DATA_FILE = sys.argv[1]

# ── Config ────────────────────────────────────────────────────────────────────
BAR_SEC      = 60       # M1 bars
FORWARD_SECS = [30, 60, 120, 300]   # forward return horizons
MIN_TRADES   = 50       # minimum sample size for a bucket
SPREAD_EST   = 0.30     # estimated spread in pts (cost per side)

# ── EWM helper ────────────────────────────────────────────────────────────────
def make_ewm(halflife_s):
    """Returns stateful EWM updater. halflife in seconds."""
    state = {"v": None, "ts": None}
    def update(val, ts_s):
        if state["v"] is None:
            state["v"] = val; state["ts"] = ts_s; return val
        dt = max(ts_s - state["ts"], 0.001)
        a = 1.0 - math.exp(-dt * 0.693147 / halflife_s)
        state["v"] = a * val + (1-a) * state["v"]
        state["ts"] = ts_s
        return state["v"]
    return update

# ── Bar builder ───────────────────────────────────────────────────────────────
@dataclass
class Bar:
    ts: int = 0
    o: float = 0; h: float = 0; l: float = 0; c: float = 0
    ticks: int = 0

    def update(self, mid):
        if self.ticks == 0: self.o = self.h = self.l = mid
        if mid > self.h: self.h = mid
        if mid < self.l: self.l = mid
        self.c = mid; self.ticks += 1

# ── Feature snapshot at bar close ─────────────────────────────────────────────
@dataclass
class Snapshot:
    ts:         int   = 0
    mid:        float = 0.0
    # drift at multiple timescales
    drift_10s:  float = 0.0
    drift_30s:  float = 0.0
    drift_120s: float = 0.0
    drift_300s: float = 0.0
    # drift momentum: is drift growing or shrinking
    drift_accel: float = 0.0   # drift_30s - drift_30s_prev
    # RSI
    rsi:        float = 50.0
    rsi_slope:  float = 0.0
    # ATR
    atr:        float = 0.0
    # VWAP
    vwap_dist:  float = 0.0   # mid - vwap (pts)
    # bar shape
    bar_body:   float = 0.0   # close - open
    bar_range:  float = 0.0
    bar_ratio:  float = 0.0   # |body| / range
    # session
    utc_hour:   int   = 0
    # price vs recent range
    dist_from_high: float = 0.0   # mid - 20bar high (negative = below)
    dist_from_low:  float = 0.0   # mid - 20bar low (positive = above)
    # forward returns (filled in after)
    fwd_30:  float = 0.0
    fwd_60:  float = 0.0
    fwd_120: float = 0.0
    fwd_300: float = 0.0

# ── Stats for a bucket ────────────────────────────────────────────────────────
@dataclass
class Bucket:
    label:  str
    n:      int   = 0
    sum_r:  float = 0.0
    sum_r2: float = 0.0
    wins:   int   = 0

    def add(self, r):
        self.n += 1; self.sum_r += r; self.sum_r2 += r*r
        if r > SPREAD_EST: self.wins += 1

    def mean(self):  return self.sum_r / self.n if self.n else 0
    def std(self):
        if self.n < 2: return 0
        v = self.sum_r2/self.n - (self.sum_r/self.n)**2
        return math.sqrt(max(v,0))
    def t_stat(self):
        s = self.std(); return (self.mean() / (s/math.sqrt(self.n))) if s and self.n>1 else 0
    def wr(self):    return self.wins/self.n*100 if self.n else 0
    def ev(self):    return self.mean() - SPREAD_EST   # expected value after cost

def print_buckets(title, buckets_list, horizon_label, min_n=MIN_TRADES):
    print(f"\n{'─'*70}")
    print(f"{title}  [{horizon_label}]")
    print(f"{'─'*70}")
    print(f"  {'Bucket':<30} {'N':>6} {'Mean':>7} {'EV':>7} {'WR%':>6} {'t':>6}")
    print(f"  {'─'*30} {'─'*6} {'─'*7} {'─'*7} {'─'*6} {'─'*6}")
    buckets_list.sort(key=lambda b: b.ev(), reverse=True)
    for b in buckets_list:
        if b.n < min_n: continue
        marker = " ★" if abs(b.t_stat()) > 2.0 and b.n >= min_n else ""
        print(f"  {b.label:<30} {b.n:>6} {b.mean():>+7.3f} {b.ev():>+7.3f} {b.wr():>5.1f}% {b.t_stat():>+6.2f}{marker}")

# ── Main pass ─────────────────────────────────────────────────────────────────
print(f"Loading {DATA_FILE} ...")

ewm10  = make_ewm(10)
ewm30  = make_ewm(30)
ewm120 = make_ewm(120)
ewm300 = make_ewm(300)

rsi_gains  = collections.deque(maxlen=14)
rsi_losses = collections.deque(maxlen=14)
rsi_prev_mid = None
rsi_cur = 50.0
rsi_prev = 50.0

# VWAP
vpv = vvol = 0.0; vday = -1; vwap = 0.0

# ATR
atr = 0.0; atr_alpha = 2/21

# Bar state
cur_bar = Bar(); cur_bar_min = -1

# 20-bar range
recent_highs = collections.deque(maxlen=20)
recent_lows  = collections.deque(maxlen=20)

# Snapshots
snaps: List[Snapshot] = []

prev_drift30 = 0.0
last_ts = 0

n_rows = 0
with open(DATA_FILE) as f:
    for line in f:
        n_rows += 1
        if n_rows % 20_000_000 == 0:
            print(f"  {n_rows//1_000_000}M rows, {len(snaps)} snapshots...")
        line = line.strip()
        if not line: continue
        parts = line.split(',')
        if len(parts) < 4: continue
        try:
            date_s = parts[0]; time_s = parts[1]
            bid = float(parts[2]); ask = float(parts[3])
        except: continue
        if bid <= 0 or ask <= bid: continue

        # Parse timestamp
        try:
            y=int(date_s[:4]); mo=int(date_s[4:6]); d=int(date_s[6:8])
            h=int(time_s[:2]); mi=int(time_s[3:5]); s=int(time_s[6:8])
            if mo<=2: y-=1; mo+=12
            days=365*y+y//4-y//100+y//400+(153*mo+8)//5+d-719469
            ts = (days*86400+h*3600+mi*60+s)
        except: continue

        mid = (bid+ask)*0.5

        # Gap reset
        if last_ts > 0 and (ts - last_ts) > 3600:
            ewm10 = make_ewm(10); ewm30 = make_ewm(30)
            ewm120 = make_ewm(120); ewm300 = make_ewm(300)
            vpv = vvol = 0.0; vday = -1
            cur_bar = Bar(); cur_bar_min = -1
            prev_drift30 = 0.0
        last_ts = ts

        # EWM drift
        e10  = ewm10(mid, ts)
        e30  = ewm30(mid, ts)
        e120 = ewm120(mid, ts)
        e300 = ewm300(mid, ts)
        d10  = mid - e10
        d30  = mid - e30
        d120 = mid - e120
        d300 = mid - e300

        # RSI
        if rsi_prev_mid is not None:
            chg = mid - rsi_prev_mid
            rsi_gains.append(chg if chg>0 else 0)
            rsi_losses.append(-chg if chg<0 else 0)
            if len(rsi_gains)==14:
                ag=sum(rsi_gains)/14; al=sum(rsi_losses)/14
                rsi_prev=rsi_cur
                rsi_cur = 100.0 if al<1e-9 else 100.0-100.0/(1+ag/al)
        rsi_prev_mid = mid

        # VWAP
        day = ts//86400
        if day != vday: vpv=vvol=0.0; vday=day
        vpv+=mid; vvol+=1; vwap=vpv/vvol

        # Bar
        bar_min = ts//BAR_SEC
        if bar_min != cur_bar_min:
            if cur_bar.ticks > 0 and len(rsi_gains)==14:
                # Completed bar -- take snapshot
                s = Snapshot()
                s.ts        = cur_bar_min * BAR_SEC
                s.mid       = cur_bar.c
                s.drift_10s = d10
                s.drift_30s = d30
                s.drift_120s= d120
                s.drift_300s= d300
                s.drift_accel= d30 - prev_drift30
                s.rsi       = rsi_cur
                s.rsi_slope = rsi_cur - rsi_prev
                s.atr       = atr
                s.vwap_dist = cur_bar.c - vwap
                s.bar_body  = cur_bar.c - cur_bar.o
                s.bar_range = cur_bar.h - cur_bar.l
                s.bar_ratio = abs(s.bar_body)/s.bar_range if s.bar_range>0 else 0
                s.utc_hour  = (cur_bar_min*60//3600)%24
                if recent_highs:
                    s.dist_from_high = cur_bar.c - max(recent_highs)
                    s.dist_from_low  = cur_bar.c - min(recent_lows)
                snaps.append(s)

                # Update recent range
                recent_highs.append(cur_bar.h)
                recent_lows.append(cur_bar.l)

                # Update ATR
                rng = cur_bar.h - cur_bar.l
                atr = atr_alpha*rng + (1-atr_alpha)*atr if atr>0 else rng

                prev_drift30 = d30

            cur_bar = Bar(); cur_bar.ts = bar_min*BAR_SEC; cur_bar_min = bar_min
        cur_bar.update(mid)

print(f"Loaded {n_rows:,} rows -> {len(snaps):,} bar snapshots")

# ── Fill forward returns ───────────────────────────────────────────────────────
print("Computing forward returns...")
for i, s in enumerate(snaps):
    # Find bars at +30s, +60s, +120s, +300s
    for j, fwd_s in enumerate(FORWARD_SECS):
        target_ts = s.ts + fwd_s
        # Binary search
        lo, hi = i+1, min(i+50, len(snaps)-1)
        best = None
        while lo <= hi:
            mid_idx = (lo+hi)//2
            if snaps[mid_idx].ts <= target_ts:
                best = snaps[mid_idx]
                lo = mid_idx+1
            else:
                hi = mid_idx-1
        if best:
            r = best.mid - s.mid
            if   j==0: s.fwd_30  = r
            elif j==1: s.fwd_60  = r
            elif j==2: s.fwd_120 = r
            elif j==3: s.fwd_300 = r

print(f"Forward returns done. Analysing {len(snaps):,} snapshots...")

# ── Feature analysis ──────────────────────────────────────────────────────────
# For each feature, bucket and compute mean/t-stat of fwd return

def analyse(horizon_attr, horizon_label):
    def fwd(s): return getattr(s, horizon_attr)

    # ── 1. DRIFT 30s buckets ──────────────────────────────────────────────────
    buckets = {}
    thresholds = [-3,-2,-1.5,-1,-0.5,0,0.5,1,1.5,2,3]
    def drift_bucket(v):
        if v < -3: return "drift30 < -3.0"
        if v < -2: return "drift30 -3 to -2"
        if v < -1.5:return "drift30 -2 to -1.5"
        if v < -1: return "drift30 -1.5 to -1"
        if v < -0.5:return "drift30 -1 to -0.5"
        if v <  0: return "drift30 -0.5 to 0"
        if v <  0.5:return "drift30 0 to 0.5"
        if v <  1: return "drift30 0.5 to 1"
        if v <  1.5:return "drift30 1 to 1.5"
        if v <  2: return "drift30 1.5 to 2"
        if v <  3: return "drift30 2 to 3"
        return "drift30 > 3.0"
    for s in snaps:
        k = drift_bucket(s.drift_30s)
        if k not in buckets: buckets[k] = Bucket(k)
        buckets[k].add(fwd(s))
    print_buckets("DRIFT 30s vs Forward Return", list(buckets.values()), horizon_label)

    # ── 2. DRIFT ACCELERATION ─────────────────────────────────────────────────
    abuckets = {}
    def accel_bucket(v):
        if v < -0.5: return "accel < -0.5 (drift falling fast)"
        if v < -0.2: return "accel -0.5 to -0.2 (drift falling)"
        if v < -0.05:return "accel -0.2 to -0.05 (drift slowing)"
        if v <  0.05:return "accel -0.05 to 0.05 (drift flat)"
        if v <  0.2: return "accel 0.05 to 0.2 (drift rising)"
        if v <  0.5: return "accel 0.2 to 0.5 (drift rising fast)"
        return "accel > 0.5 (drift spike)"
    for s in snaps:
        k = accel_bucket(s.drift_accel)
        if k not in abuckets: abuckets[k] = Bucket(k)
        abuckets[k].add(fwd(s))
    print_buckets("DRIFT ACCELERATION vs Forward Return", list(abuckets.values()), horizon_label)

    # ── 3. SESSION HOUR ───────────────────────────────────────────────────────
    hbuckets = {}
    for s in snaps:
        k = f"hour {s.utc_hour:02d}"
        if k not in hbuckets: hbuckets[k] = Bucket(k)
        hbuckets[k].add(abs(fwd(s)))  # absolute move = tradeable range
    print_buckets("SESSION HOUR vs Abs Move (tradeable range)", list(hbuckets.values()), horizon_label)

    # ── 4. ATR REGIME + DRIFT ─────────────────────────────────────────────────
    combos = {}
    for s in snaps:
        if s.atr <= 0: continue
        atr_bucket = "ATR<1" if s.atr<1 else "ATR 1-2" if s.atr<2 else "ATR 2-4" if s.atr<4 else "ATR>4"
        # drift aligned with trade direction
        drift_sign = "D+" if s.drift_30s > 0.3 else "D-" if s.drift_30s < -0.3 else "D~0"
        k = f"{atr_bucket} {drift_sign}"
        if k not in combos: combos[k] = Bucket(k)
        # return in drift direction (positive = drift was right)
        r = fwd(s) * (1 if s.drift_30s >= 0 else -1)
        combos[k].add(r)
    print_buckets("ATR REGIME + DRIFT vs Drift-Aligned Return", list(combos.values()), horizon_label)

    # ── 5. VWAP DISTANCE ─────────────────────────────────────────────────────
    vbuckets = {}
    for s in snaps:
        v = s.vwap_dist
        if   v < -10: k = "VWAP dist < -10 (far below)"
        elif v <  -5: k = "VWAP dist -10 to -5"
        elif v <  -2: k = "VWAP dist -5 to -2"
        elif v <  -0.5:k="VWAP dist -2 to -0.5"
        elif v <   0.5:k="VWAP dist near 0 (+/-0.5)"
        elif v <   2: k = "VWAP dist 0.5 to 2"
        elif v <   5: k = "VWAP dist 2 to 5"
        elif v <  10: k = "VWAP dist 5 to 10"
        else:          k = "VWAP dist > 10 (far above)"
        if k not in vbuckets: vbuckets[k] = Bucket(k)
        vbuckets[k].add(fwd(s))
    print_buckets("VWAP DISTANCE vs Forward Return", list(vbuckets.values()), horizon_label)

    # ── 6. COMBINED: best signal combinations ────────────────────────────────
    combos2 = {}
    for s in snaps:
        if s.atr < 1.5: continue   # skip low vol entirely
        # Drift falling (negative accel) + high positive drift = reversal setup
        if s.drift_30s > 1.0 and s.drift_accel < -0.2:
            k = "HIGH_DRIFT + FALLING_ACCEL (fade long?)"
        elif s.drift_30s < -1.0 and s.drift_accel > 0.2:
            k = "LOW_DRIFT + RISING_ACCEL (fade short?)"
        elif s.drift_30s > 0.5 and s.drift_accel > 0.2:
            k = "RISING_DRIFT + ACCEL (follow long?)"
        elif s.drift_30s < -0.5 and s.drift_accel < -0.2:
            k = "FALLING_DRIFT + ACCEL (follow short?)"
        elif abs(s.drift_30s) < 0.3:
            k = "FLAT_DRIFT (skip)"
        else:
            continue
        if k not in combos2: combos2[k] = Bucket(k)
        # Return in trade direction
        if "fade long" in k:   r = -fwd(s)   # short trade
        elif "fade short" in k: r = fwd(s)   # long trade
        elif "follow long" in k: r = fwd(s)
        elif "follow short" in k: r = -fwd(s)
        else: r = 0
        combos2[k].add(r)
    print_buckets("SIGNAL COMBINATIONS vs Trade Return", list(combos2.values()), horizon_label)

# Run analysis on 60s and 300s horizons
analyse("fwd_60",  "60s forward")
analyse("fwd_300", "300s forward")


#!/usr/bin/env python3
"""
Edge discovery pass 3 - find what actually predicts gold direction.

Key insight from pass 1: drift at 30s halflife = noise after cost.
But session hour abs move is massive (1.79pts at 13-14 UTC).
The edge EXISTS -- we just need the right predictor.

Tests:
1. Longer-horizon momentum: 5min, 15min, 30min price change
2. Session open momentum: first bar direction in London (07:00) and NY (13:00)
3. Bar-over-bar momentum: direction of last 3 consecutive bars
4. Previous session high/low break
5. Intrabar momentum: close vs open of current bar (strong bar = continuation)
"""

import sys, math, collections
from dataclasses import dataclass, field
from typing import List

DATA_FILE = sys.argv[1] if len(sys.argv) > 1 else "/Users/jo/Tick/2yr_XAUUSD_tick.csv"
SPREAD = 0.30
BAR_SEC = 60

@dataclass
class Bar:
    ts:int=0; o:float=0; h:float=0; l:float=0; c:float=0; n:int=0
    def upd(self,m):
        if self.n==0: self.o=self.h=self.l=m
        self.h=max(self.h,m); self.l=min(self.l,m); self.c=m; self.n+=1
    @property
    def body(self): return self.c - self.o
    @property
    def rng(self): return self.h - self.l

@dataclass
class Snap:
    ts:int=0; mid:float=0; hour:int=0; minute:int=0
    atr:float=0
    # Multi-timeframe momentum (price change over N bars)
    mom5:float=0    # 5-bar (5min) price change
    mom15:float=0   # 15-bar price change
    mom30:float=0   # 30-bar price change
    mom60:float=0   # 60-bar (1hr) price change
    # Bar sequence
    bar_dir:int=0       # current bar direction: +1/-1/0
    bars_same:int=0     # consecutive bars in same direction (1-5+)
    bar_body_ratio:float=0  # |body|/range of current bar
    # Session open momentum
    since_london:int=-1  # bars since London open (07:00 UTC), -1=not today yet
    since_ny:int=-1      # bars since NY open (13:00 UTC)
    london_open_dir:int=0  # direction of first bar at London open
    ny_open_dir:int=0      # direction of first bar at NY open
    # Structural
    prev_day_high:float=0
    prev_day_low:float=0
    rel_to_pdh:float=0   # mid - prev_day_high
    rel_to_pdl:float=0   # mid - prev_day_low
    # Forward
    fwd60:float=0; fwd300:float=0; fwd900:float=0

class Stat:
    def __init__(self,l): self.l=l; self.n=0; self.s=0.0; self.s2=0.0; self.w=0
    def add(self,r):
        self.n+=1; self.s+=r; self.s2+=r*r
        if r>SPREAD: self.w+=1
    def mean(self): return self.s/self.n if self.n else 0
    def std(self):
        if self.n<2: return 0
        return math.sqrt(max(self.s2/self.n-(self.s/self.n)**2, 0))
    def t(self): s=self.std(); return self.mean()/(s/math.sqrt(self.n)) if s and self.n>1 else 0
    def ev(self): return self.mean()-SPREAD
    def wr(self): return self.w/self.n*100 if self.n else 0

def table(title, stats, min_n=50):
    stats = [s for s in stats if s.n >= min_n]
    if not stats: print(f"\n{title}\n  (no data)"); return
    stats.sort(key=lambda s: s.ev(), reverse=True)
    print(f"\n{'─'*75}")
    print(f"{title}")
    print(f"{'─'*75}")
    print(f"  {'Bucket':<40} {'N':>6} {'Mean':>7} {'EV':>7} {'WR%':>6} {'t':>7}")
    for s in stats:
        mk = " ★" if abs(s.t()) > 2.5 else ""
        print(f"  {s.l:<40} {s.n:>6} {s.mean():>+7.3f} {s.ev():>+7.3f} {s.wr():>5.1f}% {s.t():>+7.2f}{mk}")

# ── Load ─────────────────────────────────────────────────────────────────────
print(f"Loading {DATA_FILE}...")
bars_hist = collections.deque(maxlen=61)
cur = Bar(); cur_min = -1
atr = 0.0
snaps: List[Snap] = []
last_ts = 0

# Day tracking
day_bars: List[Bar] = []
prev_day_high = 0.0; prev_day_low = 0.0
cur_day = -1; cur_day_high = 0.0; cur_day_low = 0.0

# Session open tracking  
london_open_bar = None  # first bar at 07:00
ny_open_bar = None      # first bar at 13:00
london_open_day = -1
ny_open_day = -1

n = 0
with open(DATA_FILE) as f:
    for line in f:
        n += 1
        if n % 20_000_000 == 0: print(f"  {n//1_000_000}M rows, {len(snaps)} snaps...")
        line = line.strip()
        if not line: continue
        p = line.split(',')
        if len(p) < 4: continue
        try:
            bid=float(p[2]); ask=float(p[3])
        except: continue
        if bid<=0 or ask<=bid: continue
        try:
            ds=p[0]; ts_s=p[1]
            y=int(ds[:4]); mo=int(ds[4:6]); dy=int(ds[6:8])
            h=int(ts_s[:2]); mi=int(ts_s[3:5]); se=int(ts_s[6:8])
            if mo<=2: y-=1; mo+=12
            days=365*y+y//4-y//100+y//400+(153*mo+8)//5+dy-719469
            ts=(days*86400+h*3600+mi*60+se)
        except: continue

        mid=(bid+ask)*0.5
        if last_ts>0 and ts-last_ts>3600:
            cur=Bar(); cur_min=-1; bars_hist.clear()
            london_open_bar=None; ny_open_bar=None
        last_ts=ts

        bm = ts // BAR_SEC
        if bm != cur_min:
            if cur.n > 0:
                b = Bar(ts=cur_min*BAR_SEC, o=cur.o, h=cur.h, l=cur.l, c=cur.c, n=cur.n)
                bar_hour = (cur_min*60//3600)%24
                bar_min_of_hr = (cur_min*60//60)%60
                bar_day = cur_min*60//86400

                # Day tracking
                if bar_day != cur_day:
                    if cur_day >= 0:
                        prev_day_high = cur_day_high
                        prev_day_low  = cur_day_low
                    cur_day = bar_day
                    cur_day_high = b.h
                    cur_day_low  = b.l
                else:
                    cur_day_high = max(cur_day_high, b.h)
                    cur_day_low  = min(cur_day_low,  b.l)

                # Session open tracking
                if bar_hour == 7 and bar_day != london_open_day:
                    london_open_bar = b
                    london_open_day = bar_day
                if bar_hour == 13 and bar_day != ny_open_day:
                    ny_open_bar = b
                    ny_open_day = bar_day

                bars_hist.append(b)
                rng = b.h - b.l
                atr = 2/21*rng + (1-2/21)*atr if atr>0 else rng

                if len(bars_hist) >= 61 and atr > 0.5 and prev_day_high > 0:
                    bl = list(bars_hist)
                    # Multi-timeframe momentum
                    mom5  = b.c - bl[-6].c  if len(bl)>=6  else 0
                    mom15 = b.c - bl[-16].c if len(bl)>=16 else 0
                    mom30 = b.c - bl[-31].c if len(bl)>=31 else 0
                    mom60 = b.c - bl[-61].c if len(bl)>=61 else 0

                    # Bar sequence
                    bdir = 1 if b.body>0.05 else (-1 if b.body<-0.05 else 0)
                    same = 0
                    if bdir != 0:
                        for pb in reversed(list(bl)[:-1]):
                            pd = 1 if pb.body>0.05 else (-1 if pb.body<-0.05 else 0)
                            if pd == bdir: same += 1
                            else: break
                        same = min(same, 5)

                    # Session
                    london_dir = 0
                    if london_open_bar:
                        london_dir = 1 if london_open_bar.body>0 else (-1 if london_open_bar.body<0 else 0)
                    ny_dir = 0
                    if ny_open_bar:
                        ny_dir = 1 if ny_open_bar.body>0 else (-1 if ny_open_bar.body<0 else 0)

                    # Bars since open
                    since_lon = -1
                    if london_open_bar and bar_hour >= 7:
                        since_lon = (b.ts - london_open_bar.ts) // BAR_SEC
                    since_ny_ = -1
                    if ny_open_bar and bar_hour >= 13:
                        since_ny_ = (b.ts - ny_open_bar.ts) // BAR_SEC

                    s = Snap(
                        ts=b.ts, mid=b.c, hour=bar_hour,
                        minute=bar_min_of_hr, atr=atr,
                        mom5=mom5, mom15=mom15, mom30=mom30, mom60=mom60,
                        bar_dir=bdir, bars_same=same,
                        bar_body_ratio=abs(b.body)/b.rng if b.rng>0 else 0,
                        since_london=since_lon, since_ny=since_ny_,
                        london_open_dir=london_dir, ny_open_dir=ny_dir,
                        prev_day_high=prev_day_high, prev_day_low=prev_day_low,
                        rel_to_pdh=b.c-prev_day_high,
                        rel_to_pdl=b.c-prev_day_low,
                    )
                    snaps.append(s)

            cur=Bar(); cur.ts=bm*BAR_SEC; cur_min=bm
        cur.upd(mid)

print(f"Loaded {n:,} rows -> {len(snaps):,} snaps")

# Forward returns
print("Computing forward returns...")
for i,s in enumerate(snaps):
    for fwd_s, attr in [(60,'fwd60'),(300,'fwd300'),(900,'fwd900')]:
        tgt = s.ts + fwd_s
        lo, hi = i+1, min(i+60, len(snaps)-1)
        best = None
        while lo <= hi:
            m = (lo+hi)//2
            if snaps[m].ts <= tgt: best=snaps[m]; lo=m+1
            else: hi=m-1
        if best: setattr(s, attr, best.mid - s.mid)

print(f"Done. Analysing {len(snaps):,} snaps...")

# ── Analysis ─────────────────────────────────────────────────────────────────

for horizon, attr in [("60s","fwd60"), ("300s","fwd300"), ("900s","fwd900")]:
    def fwd(s): return getattr(s, attr)

    # 1. Multi-timeframe momentum -- does N-bar momentum predict next move?
    for label, mom_attr, thresholds in [
        ("5-bar mom",  "mom5",  [(-3,-1.5,-0.5,0.5,1.5,3)]),
        ("15-bar mom", "mom15", [(-6,-3,-1,1,3,6)]),
        ("30-bar mom", "mom30", [(-10,-5,-2,2,5,10)]),
        ("60-bar mom", "mom60", [(-15,-8,-3,3,8,15)]),
    ]:
        thrs = thresholds[0]
        bkts = {}
        for s in snaps:
            v = getattr(s, mom_attr)
            # bucket by threshold
            if   v < thrs[0]: k=f"{label}<{thrs[0]}"
            elif v < thrs[1]: k=f"{label} {thrs[0]}-{thrs[1]}"
            elif v < thrs[2]: k=f"{label} {thrs[1]}-{thrs[2]}"
            elif v < thrs[3]: k=f"{label} {thrs[2]}-{thrs[3]}"
            elif v < thrs[4]: k=f"{label} {thrs[3]}-{thrs[4]}"
            elif v < thrs[5]: k=f"{label} {thrs[4]}-{thrs[5]}"
            else:              k=f"{label}>{thrs[5]}"
            if k not in bkts: bkts[k]=Stat(k)
            # return in momentum direction (positive = continuation)
            bkts[k].add(fwd(s) * (1 if v>=0 else -1))
        table(f"{label} CONTINUATION [{horizon}]", list(bkts.values()))

    # 2. Consecutive bars in same direction
    bkts = {}
    for s in snaps:
        if s.bar_dir == 0: continue
        k = f"{s.bars_same+1} consecutive {'UP' if s.bar_dir>0 else 'DN'} bars"
        if k not in bkts: bkts[k]=Stat(k)
        bkts[k].add(fwd(s) * s.bar_dir)  # in bar direction
    table(f"CONSECUTIVE BARS MOMENTUM [{horizon}]", list(bkts.values()))

    # 3. Session open direction -- does London/NY open bar predict rest of session?
    bkts = {}
    for s in snaps:
        if s.london_open_dir == 0: continue
        if s.hour < 7 or s.hour > 16: continue
        if s.since_london < 0 or s.since_london > 60: continue
        k = f"London open {'UP' if s.london_open_dir>0 else 'DN'}, bar {min(s.since_london//5, 11)*5}-{min(s.since_london//5+1,12)*5}min"
        if k not in bkts: bkts[k]=Stat(k)
        bkts[k].add(fwd(s) * s.london_open_dir)  # in open direction
    table(f"LONDON OPEN DIRECTION PERSISTENCE [{horizon}]", list(bkts.values()))

    bkts = {}
    for s in snaps:
        if s.ny_open_dir == 0: continue
        if s.hour < 13 or s.hour > 17: continue
        if s.since_ny < 0 or s.since_ny > 60: continue
        k = f"NY open {'UP' if s.ny_open_dir>0 else 'DN'}, {min(s.since_ny//5,11)*5}-{min(s.since_ny//5+1,12)*5}min after"
        if k not in bkts: bkts[k]=Stat(k)
        bkts[k].add(fwd(s) * s.ny_open_dir)
    table(f"NY OPEN DIRECTION PERSISTENCE [{horizon}]", list(bkts.values()))

    # 4. PDH/PDL breaks
    bkts = {}
    for s in snaps:
        if s.prev_day_high <= 0: continue
        above_pdh = s.rel_to_pdh
        below_pdl = -s.rel_to_pdl
        if   above_pdh > 5:   k="ABOVE PDH >5pts"
        elif above_pdh > 2:   k="ABOVE PDH 2-5pts"
        elif above_pdh > 0.5: k="ABOVE PDH 0.5-2pts"
        elif above_pdh > 0:   k="JUST ABOVE PDH"
        elif below_pdl > 5:   k="BELOW PDL >5pts"
        elif below_pdl > 2:   k="BELOW PDL 2-5pts"
        elif below_pdl > 0.5: k="BELOW PDL 0.5-2pts"
        elif below_pdl > 0:   k="JUST BELOW PDL"
        else:                  k="INSIDE PDH/PDL"
        if k not in bkts: bkts[k]=Stat(k)
        # For above PDH: long return. For below PDL: short return.
        if "ABOVE" in k: bkts[k].add(fwd(s))
        elif "BELOW" in k: bkts[k].add(-fwd(s))
        else: bkts[k].add(abs(fwd(s)))
    table(f"PREV DAY HIGH/LOW BREAK [{horizon}]", list(bkts.values()))

    # 5. Strong bar body -- big body bars tend to continue
    bkts = {}
    for s in snaps:
        if s.bar_dir == 0: continue
        if   s.bar_body_ratio > 0.80: k="Strong bar body >80%"
        elif s.bar_body_ratio > 0.60: k="Good bar body 60-80%"
        elif s.bar_body_ratio > 0.40: k="Medium bar body 40-60%"
        else:                          k="Weak bar body <40%"
        if k not in bkts: bkts[k]=Stat(k)
        bkts[k].add(fwd(s) * s.bar_dir)
    table(f"BAR BODY STRENGTH CONTINUATION [{horizon}]", list(bkts.values()))

    # 6. Momentum + session combined (best combo)
    bkts = {}
    for s in snaps:
        if s.atr < 1.0: continue
        # 15-bar momentum in active session
        is_active = (7<=s.hour<=16) or (0<=s.hour<=3)
        if not is_active: continue
        m = s.mom15
        if   m >  5: k="mom15>5 active session → LONG"
        elif m >  2: k="mom15 2-5 active session → LONG"
        elif m >  1: k="mom15 1-2 active session → LONG"
        elif m < -5: k="mom15<-5 active session → SHORT"
        elif m < -2: k="mom15 -5 to -2 active → SHORT"
        elif m < -1: k="mom15 -2 to -1 active → SHORT"
        else: continue
        if k not in bkts: bkts[k]=Stat(k)
        bkts[k].add(fwd(s) * (1 if m>0 else -1))
    table(f"15-BAR MOMENTUM + ACTIVE SESSION [{horizon}]", list(bkts.values()))


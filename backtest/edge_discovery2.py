#!/usr/bin/env python3
"""
Edge discovery pass 2 -- informed by pass 1.
Now testing:
1. Session breakout: enter on range breakout during high-move hours
   - Build 30-bar range before 12:00 UTC
   - Long on break above, short on break below
   - Measure forward 60s and 300s return
2. Extreme drift fade: drift < -2.0 → LONG, drift > 2.0 → SHORT
   - Only during high-ATR sessions
3. Drift < -3 deep fade: the strongest single signal
4. Combined: extreme drift fade + high session hour
"""

import sys, math, collections
from dataclasses import dataclass

DATA_FILE = sys.argv[1] if len(sys.argv) > 1 else "/Users/jo/Tick/2yr_XAUUSD_tick.csv"
SPREAD    = 0.30
BAR_SEC   = 60

def make_ewm(hl):
    s = {"v": None, "ts": None}
    def u(v, ts):
        if s["v"] is None: s["v"]=v; s["ts"]=ts; return v
        dt=max(ts-s["ts"],0.001); a=1-math.exp(-dt*0.693147/hl)
        s["v"]=a*v+(1-a)*s["v"]; s["ts"]=ts; return s["v"]
    return u

@dataclass
class Bar:
    ts:int=0;o:float=0;h:float=0;l:float=0;c:float=0;n:int=0
    def upd(self,m):
        if self.n==0:self.o=self.h=self.l=m
        self.h=max(self.h,m);self.l=min(self.l,m);self.c=m;self.n+=1

@dataclass
class Snap:
    ts:int=0; mid:float=0; hour:int=0
    d30:float=0; d120:float=0; atr:float=0
    # Range breakout features
    range_hi:float=0; range_lo:float=0  # 30-bar range before this bar
    above_range:float=0   # mid - range_hi (positive = above)
    below_range:float=0   # range_lo - mid (positive = below)
    # Forward returns
    fwd60:float=0; fwd300:float=0

class Stat:
    def __init__(self,lbl): self.l=lbl;self.n=0;self.s=0;self.s2=0;self.w=0
    def add(self,r):
        self.n+=1;self.s+=r;self.s2+=r*r
        if r>SPREAD:self.w+=1
    def mean(self): return self.s/self.n if self.n else 0
    def std(self):
        if self.n<2:return 0
        v=self.s2/self.n-(self.s/self.n)**2;return math.sqrt(max(v,0))
    def t(self): s=self.std();return (self.mean()/(s/math.sqrt(self.n))) if s and self.n>1 else 0
    def ev(self): return self.mean()-SPREAD
    def wr(self): return self.w/self.n*100 if self.n else 0

def table(title, stats, min_n=30):
    print(f"\n{'─'*72}")
    print(f"{title}")
    print(f"{'─'*72}")
    print(f"  {'Bucket':<38} {'N':>6} {'Mean':>7} {'EV':>7} {'WR%':>6} {'t':>6}")
    stats.sort(key=lambda s:s.ev(),reverse=True)
    for s in stats:
        if s.n<min_n:continue
        mk=" ★" if abs(s.t())>2 else ""
        print(f"  {s.l:<38} {s.n:>6} {s.mean():>+7.3f} {s.ev():>+7.3f} {s.wr():>5.1f}% {s.t():>+6.2f}{mk}")

print(f"Loading {DATA_FILE}...")
e30=make_ewm(30); e120=make_ewm(120)
vpv=vvol=0.0;vday=-1;vwap=0.0
atr=0.0
cur=Bar();cur_min=-1
bars=collections.deque(maxlen=31)
snaps=[]
last_ts=0
rsi_g=collections.deque(maxlen=14);rsi_l=collections.deque(maxlen=14)
rsi_pm=None;rsi_cur=50.0;rsi_prev=50.0

n=0
with open(DATA_FILE) as f:
    for line in f:
        n+=1
        if n%20_000_000==0: print(f"  {n//1_000_000}M rows, {len(snaps)} snaps...")
        line=line.strip()
        if not line:continue
        p=line.split(',')
        if len(p)<4:continue
        try:
            ds=p[0];ts_s=p[1];bid=float(p[2]);ask=float(p[3])
        except:continue
        if bid<=0 or ask<=bid:continue
        try:
            y=int(ds[:4]);mo=int(ds[4:6]);dy=int(ds[6:8])
            h=int(ts_s[:2]);mi=int(ts_s[3:5]);se=int(ts_s[6:8])
            if mo<=2:y-=1;mo+=12
            days=365*y+y//4-y//100+y//400+(153*mo+8)//5+dy-719469
            ts=(days*86400+h*3600+mi*60+se)
        except:continue

        mid=(bid+ask)*0.5
        if last_ts>0 and ts-last_ts>3600:
            e30=make_ewm(30);e120=make_ewm(120)
            vpv=vvol=0.0;vday=-1;cur=Bar();cur_min=-1;bars.clear()
        last_ts=ts

        em30=e30(mid,ts); em120=e120(mid,ts)
        d30=mid-em30; d120=mid-em120

        day=ts//86400
        if day!=vday:vpv=vvol=0.0;vday=day
        vpv+=mid;vvol+=1;vwap=vpv/vvol

        if rsi_pm:
            chg=mid-rsi_pm
            rsi_g.append(chg if chg>0 else 0)
            rsi_l.append(-chg if chg<0 else 0)
            if len(rsi_g)==14:
                ag=sum(rsi_g)/14;al=sum(rsi_l)/14
                rsi_prev=rsi_cur
                rsi_cur=100 if al<1e-9 else 100-100/(1+ag/al)
        rsi_pm=mid

        bm=ts//BAR_SEC
        if bm!=cur_min:
            if cur.n>0:
                bars.append(Bar(ts=cur_min*BAR_SEC,o=cur.o,h=cur.h,l=cur.l,c=cur.c,n=cur.n))
                rng=cur.h-cur.l
                atr=2/21*rng+(1-2/21)*atr if atr>0 else rng
                if len(bars)>=30:
                    # 30-bar lookback range
                    lookback=list(bars)[-30:]
                    rhi=max(b.h for b in lookback)
                    rlo=min(b.l for b in lookback)
                    s=Snap()
                    s.ts=cur_min*BAR_SEC
                    s.mid=cur.c
                    s.hour=(cur_min*60//3600)%24
                    s.d30=d30; s.d120=d120; s.atr=atr
                    s.range_hi=rhi; s.range_lo=rlo
                    s.above_range=cur.c-rhi
                    s.below_range=rlo-cur.c
                    snaps.append(s)
            cur=Bar();cur.ts=bm*BAR_SEC;cur_min=bm
        cur.upd(mid)

print(f"Loaded {n:,} rows -> {len(snaps):,} snaps")
print("Computing forward returns...")
for i,s in enumerate(snaps):
    for fwd_s,attr in [(60,'fwd60'),(300,'fwd300')]:
        tgt=s.ts+fwd_s
        lo,hi=i+1,min(i+50,len(snaps)-1)
        best=None
        while lo<=hi:
            m=(lo+hi)//2
            if snaps[m].ts<=tgt:best=snaps[m];lo=m+1
            else:hi=m-1
        if best:setattr(s,attr,best.mid-s.mid)

print(f"Done. {len(snaps):,} snaps. Analysing...")

# ── Analysis 1: Range breakout buckets ───────────────────────────────────────
# During high-vol session hours (12-16 UTC), does breakout of 30-bar range predict next move?

for horizon,attr in [("60s","fwd60"),("300s","fwd300")]:
    brk_stats={}
    for s in snaps:
        if s.atr < 0.8: continue
        r=getattr(s,attr)
        # Above range = potential long breakout
        # Below range = potential short breakout
        ab=s.above_range; bl=s.below_range
        if   ab > 0.5:  k=f"ABOVE range >0.5 (long brk)"
        elif ab > 0.2:  k=f"ABOVE range 0.2-0.5"
        elif bl > 0.5:  k=f"BELOW range >0.5 (short brk)"
        elif bl > 0.2:  k=f"BELOW range 0.2-0.5"
        else:           k=f"INSIDE range"
        if k not in brk_stats: brk_stats[k]=Stat(k)
        # Return in breakout direction
        trade_r = r if ab>0 else -r if bl>0 else 0
        brk_stats[k].add(trade_r)
    table(f"RANGE BREAKOUT (ATR>0.8) vs Breakout-Aligned Return [{horizon}]",
          list(brk_stats.values()))

# ── Analysis 2: Session + Range breakout ─────────────────────────────────────
for horizon,attr in [("300s","fwd300")]:
    combo={}
    for s in snaps:
        if s.atr < 1.0: continue
        r=getattr(s,attr)
        h=s.hour
        # Session buckets
        if   13<=h<=14: sess="NY_OPEN(13-14)"
        elif 12<=h<=16: sess="NY(12-16)"
        elif 7<=h<=11:  sess="LONDON(07-11)"
        elif 0<=h<=3:   sess="ASIA(00-03)"
        else:           continue

        ab=s.above_range; bl=s.below_range
        if   ab>0.5: direction="LONG_BRK"
        elif bl>0.5: direction="SHORT_BRK"
        else:        continue

        k=f"{sess} {direction}"
        if k not in combo: combo[k]=Stat(k)
        trade_r = r if "LONG" in direction else -r
        combo[k].add(trade_r)
    table(f"SESSION + RANGE BREAKOUT [{horizon}]", list(combo.values()))

# ── Analysis 3: Extreme drift fade ───────────────────────────────────────────
for horizon,attr in [("60s","fwd60"),("300s","fwd300")]:
    fade={}
    for s in snaps:
        r=getattr(s,attr)
        h=s.hour
        d=s.d30

        # Extreme drift: price has moved far from 30s EWM mean
        if d < -2.0 and s.atr > 1.0:
            k=f"FADE: drift<-2 ATR>1 → LONG"
            if k not in fade: fade[k]=Stat(k)
            fade[k].add(r)   # going LONG: positive r = win
        elif d > 2.0 and s.atr > 1.0:
            k=f"FADE: drift>+2 ATR>1 → SHORT"
            if k not in fade: fade[k]=Stat(k)
            fade[k].add(-r)  # going SHORT: negative r = win
        elif d < -3.0:
            k=f"FADE: drift<-3 (extreme) → LONG"
            if k not in fade: fade[k]=Stat(k)
            fade[k].add(r)
        elif d > 3.0:
            k=f"FADE: drift>+3 (extreme) → SHORT"
            if k not in fade: fade[k]=Stat(k)
            fade[k].add(-r)
    table(f"EXTREME DRIFT FADE [{horizon}]", list(fade.values()))

# ── Analysis 4: Best combination ─────────────────────────────────────────────
for horizon,attr in [("300s","fwd300")]:
    best={}
    for s in snaps:
        if s.atr < 1.5: continue
        r=getattr(s,attr)
        h=s.hour
        d=s.d30
        ab=s.above_range; bl=s.below_range
        sess_ok = (7<=h<=16) or (0<=h<=3)
        if not sess_ok: continue

        # A: Range breakout with drift confirmation
        if ab>0.5 and d>0.2:
            k="LONG: range_brk + drift_confirm"
            if k not in best: best[k]=Stat(k)
            best[k].add(r)
        elif bl>0.5 and d<-0.2:
            k="SHORT: range_brk + drift_confirm"
            if k not in best: best[k]=Stat(k)
            best[k].add(-r)
        # B: Range breakout against drift (fade)
        elif ab>0.5 and d<-0.2:
            k="SHORT: range_above + drift_fade"
            if k not in best: best[k]=Stat(k)
            best[k].add(-r)
        elif bl>0.5 and d>0.2:
            k="LONG: range_below + drift_fade"
            if k not in best: best[k]=Stat(k)
            best[k].add(r)
        # C: Extreme drift fade in session
        elif d<-2.0 and (7<=h<=16 or 0<=h<=3):
            k="LONG: extreme_drift_fade in session"
            if k not in best: best[k]=Stat(k)
            best[k].add(r)
        elif d>2.0 and (7<=h<=16 or 0<=h<=3):
            k="SHORT: extreme_drift_fade in session"
            if k not in best: best[k]=Stat(k)
            best[k].add(-r)

    table(f"BEST COMBINATIONS (ATR>1.5, active sessions) [{horizon}]",
          list(best.values()))


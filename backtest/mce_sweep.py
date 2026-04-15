#!/usr/bin/env python3
"""
MCE threshold sweep -- find the optimal ATR/drift/vol thresholds
that maximize profitable trades while preserving the 69% WR signal quality.

The only profitable engine in 2yr backtest is MCE at original thresholds.
We need more trades. This sweeps thresholds to find where edge degrades.

Key question: at ATR=4.5, drift=4, vol=2.0 -- does WR drop below 50%?
"""

import sys, math, collections, csv
from dataclasses import dataclass

DATA = sys.argv[1] if len(sys.argv) > 1 else "/Users/jo/Tick/2yr_XAUUSD_tick.csv"
SPREAD = 0.25
RISK   = 30.0
USD_PT = 100.0

def make_ewm(hl):
    s = {"v": None, "ts": None}
    def u(v, ts):
        if s["v"] is None: s["v"]=v; s["ts"]=ts; return v
        dt=max(ts-s["ts"],0.001); a=1-math.exp(-dt*0.693147/hl)
        s["v"]=a*v+(1-a)*s["v"]; s["ts"]=ts; return s["v"]
    return u

@dataclass
class Pos:
    active:bool=False; is_long:bool=True
    entry:float=0; sl:float=0; tp:float=0
    size:float=0; entry_ts:int=0
    mfe:float=0; mae:float=0

class Res:
    def __init__(self,label): self.label=label; self.trades=[]; self.monthly=collections.defaultdict(float)
    def add(self, pnl, ts, is_win):
        self.trades.append((pnl,ts,is_win))
        mo = f"{1970+ts//31557600:04d}-{(ts%31557600)//2592000+1:02d}"
        self.monthly[mo] += pnl
    def summary(self):
        if not self.trades: return f"{self.label:<35} {'no trades':>10}"
        pnls=[t[0] for t in self.trades]
        wins=[p for p in pnls if p>0]
        net=sum(pnls); wr=len(wins)/len(pnls)*100
        peak=eq=dd=0
        for p in pnls:
            eq+=p; peak=max(peak,eq); dd=max(dd,peak-eq)
        pos_mo=sum(1 for v in self.monthly.values() if v>0)
        return (f"{self.label:<40} n={len(pnls):>4} WR={wr:>5.1f}% "
                f"net=${net:>+9.2f} avg=${net/len(pnls):>+6.2f} "
                f"dd=${dd:>7.2f} pos_mo={pos_mo}/{len(self.monthly)}")

# Threshold combinations to test
# (atr_min, drift_min, vol_min, sl_mult, label)
configs = [
    # Original -- proven 69% WR
    (6.0, 5.0, 2.5, 1.5, "ORIGINAL atr6 d5 v2.5"),
    # Slightly lower
    (5.0, 4.5, 2.0, 1.5, "atr5 d4.5 v2.0"),
    (5.0, 4.0, 2.0, 1.5, "atr5 d4.0 v2.0"),
    (4.5, 4.0, 2.0, 1.5, "atr4.5 d4.0 v2.0"),
    (4.5, 3.5, 2.0, 1.5, "atr4.5 d3.5 v2.0"),
    (4.0, 4.0, 2.0, 1.5, "atr4.0 d4.0 v2.0"),
    (4.0, 3.5, 2.0, 1.5, "atr4.0 d3.5 v2.0"),
    (4.0, 3.0, 2.0, 1.5, "atr4.0 d3.0 v2.0"),
    (4.0, 3.0, 1.8, 1.5, "atr4.0 d3.0 v1.8"),
    (3.5, 3.0, 1.8, 1.5, "atr3.5 d3.0 v1.8"),
    # With session gate (Asia+London+early NY: 22-17 UTC, block 17-22)
    (5.0, 4.0, 2.0, 1.5, "atr5 d4.0 v2.0 SESSION"),
    (4.5, 3.5, 2.0, 1.5, "atr4.5 d3.5 v2.0 SESSION"),
    (4.0, 3.0, 1.8, 1.5, "atr4.0 d3.0 v1.8 SESSION"),
]

results = {cfg[4]: Res(cfg[4]) for cfg in configs}
positions = {cfg[4]: Pos() for cfg in configs}
last_close = {cfg[4]: 0 for cfg in configs}

ewm30 = make_ewm(30)
ewm120 = make_ewm(120)
atr = 0.0
vs = vl = 0.0  # vol ratio EWMs

# PDH/PDL
pdh = pdl = 0.0
today_hi = 0.0; today_lo = 1e9; cur_day = -1

last_ts = 0; n = 0
print(f"Loading {DATA}...")

with open(DATA) as f:
    for line in f:
        n += 1
        if n % 20_000_000 == 0: print(f"  {n//1_000_000}M rows...")
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
            ewm30=make_ewm(30); ewm120=make_ewm(120); vs=vl=0

        last_ts=ts
        dt=1.0  # approximate

        em30=ewm30(mid,ts); em120=ewm120(mid,ts)
        drift=mid-em30

        # ATR (tick EWM scaled to bar-equivalent)
        spread_now=ask-bid
        if atr==0: atr=spread_now*20
        else: atr=0.002*spread_now*20+0.998*atr

        # Vol ratio: short/long EWM of |drift|
        ab=abs(drift)
        vs=0.1*ab+0.9*vs; vl=0.01*ab+0.99*vl
        vol_ratio=vs/vl if vl>1e-9 else 1.0

        hour=(ts%86400)//3600

        # PDH/PDL
        day=ts//86400
        if day!=cur_day:
            if cur_day>=0: pdh=today_hi; pdl=(today_lo if today_lo<1e8 else 0)
            today_hi=mid; today_lo=mid; cur_day=day
        else:
            if mid>today_hi: today_hi=mid
            if mid<today_lo: today_lo=mid

        for cfg in configs:
            atr_min,drift_min,vol_min,sl_mult,label = cfg
            session_gate = "SESSION" in label
            if session_gate and (17<=hour<=21): continue  # block late NY

            pos=positions[label]; res=results[label]

            if pos.active:
                m=(mid-pos.entry) if pos.is_long else (pos.entry-mid)
                pos.mfe=max(pos.mfe,m); pos.mae=min(pos.mae,m)
                sl_hit=(pos.is_long and bid<=pos.sl) or (not pos.is_long and ask>=pos.sl)
                tp_hit=(pos.is_long and ask>=pos.tp) or (not pos.is_long and bid<=pos.tp)
                time_exit=(ts-pos.entry_ts)>=600
                if sl_hit or tp_hit or time_exit:
                    ep=pos.sl if sl_hit else (pos.tp if tp_hit else (bid if pos.is_long else ask))
                    pnl_pts=(ep-pos.entry) if pos.is_long else (pos.entry-ep)
                    pnl_usd=pnl_pts*pos.size*USD_PT - SPREAD*pos.size*USD_PT
                    res.add(pnl_usd, ts, pnl_usd>0)
                    pos.active=False; last_close[label]=ts
                continue

            if ts-last_close[label]<120: continue
            if atr<atr_min: continue
            if vol_ratio<vol_min: continue
            if abs(drift)<drift_min: continue

            # Entry: follow the drift direction (MCE logic -- ride the impulse)
            is_long=drift>0
            sl_pts=atr*sl_mult*0.05  # scale tick ATR to tradeable SL
            sl_pts=max(sl_pts, 1.5)  # minimum 1.5pt SL
            ep=ask if is_long else bid
            sl_px=ep-sl_pts if is_long else ep+sl_pts
            tp_px=ep+sl_pts*2 if is_long else ep-sl_pts*2
            sz=max(0.01,min(0.20, RISK/(sl_pts*USD_PT)))
            sz=math.floor(sz/0.001)*0.001

            positions[label]=Pos(True,is_long,ep,sl_px,tp_px,sz,ts,0,0)

print(f"\nLoaded {n:,} rows\n")
print(f"{'Config':<40} {'N':>5} {'WR%':>6} {'Net':>10} {'Avg':>7} {'DD':>8} {'POS/MO':>7}")
print("─"*85)
for cfg in configs:
    print(results[cfg[4]].summary())

# Detailed monthly for configs that show positive months
print("\n\nDETAIL -- configs with any profitable months:")
for cfg in configs:
    r=results[cfg[4]]
    if not r.trades: continue
    pos_mo=sum(1 for v in r.monthly.values() if v>0)
    if pos_mo < 3: continue
    print(f"\n{cfg[4]}:")
    cum=0
    for mo,v in sorted(r.monthly.items()):
        cum+=v
        bar='#'*int(v/30) if v>0 else '.'*int(abs(v)/30)
        print(f"  {mo}  {v:>+8.2f}  cum={cum:>+9.2f}  {bar[:40]}")


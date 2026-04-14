#!/usr/bin/env python3
"""
Edge-based engine backtest.
Built from statistically significant signals in edge_discovery3.py:

SIGNAL 1: London Open Follow
  - Detect direction of first M1 bar at 07:00 UTC
  - Enter in that direction at bars 5-10min, 15-20min, 25-30min, 35-40min after open
  - Exit after 15 bars (15min) or SL = 1.5x ATR
  - Evidence: mean +0.307-0.361 at 900s, t=2.93-3.80★

SIGNAL 2: NY Open Fade
  - Detect direction of first M1 bar at 13:00 UTC  
  - Enter AGAINST that direction at bars 5-15min after open
  - Exit after 15 bars or SL = 1.5x ATR
  - Evidence: NY UP → price falls -0.367 to -0.493 at 900s, t=-3.04 to -3.25★

SIGNAL 3: Extreme 15-bar momentum fade in active session
  - mom15 > 5pts → SHORT, mom15 < -5pts → LONG
  - Only during London/NY (07-17 UTC)
  - Evidence: mean +0.127-0.134 at 900s, t=2.84-3.23★ (but still negative EV at $0.30 spread)
  - Test with realistic spread of $0.20

Risk: $30 per trade, SL = 1.5x ATR, max lot 0.20
"""

import sys, math, collections
from dataclasses import dataclass, field
from typing import List, Optional

DATA = sys.argv[1] if len(sys.argv) > 1 else "/Users/jo/Tick/2yr_XAUUSD_tick.csv"
SPREAD_COST = 0.20   # realistic London/NY spread
COMMISSION  = 0.20   # round trip commission in pts
TOTAL_COST  = SPREAD_COST + COMMISSION
RISK_USD    = 30.0
MIN_LOT     = 0.01
MAX_LOT     = 0.20
USD_PER_PT  = 100.0

@dataclass
class Bar:
    ts:int=0;o:float=0;h:float=0;l:float=0;c:float=0;n:int=0
    def upd(self,m):
        if self.n==0:self.o=self.h=self.l=m
        self.h=max(self.h,m);self.l=min(self.l,m);self.c=m;self.n+=1
    @property
    def body(self): return self.c-self.o

@dataclass
class Trade:
    entry_ts:int=0; exit_ts:int=0
    entry:float=0; exit_px:float=0
    is_long:bool=True; size:float=0
    pnl_usd:float=0; signal:str=""
    mfe:float=0; mae:float=0

class Stats:
    def __init__(self,name):
        self.name=name; self.trades:List[Trade]=[]
    def add(self,t): self.trades.append(t)
    def report(self):
        if not self.trades: print(f"  {self.name}: no trades"); return
        pnls=[t.pnl_usd for t in self.trades]
        wins=[p for p in pnls if p>0]
        loss=[p for p in pnls if p<0]
        cum=sum(pnls)
        wr=len(wins)/len(pnls)*100
        avg=cum/len(pnls)
        avg_w=sum(wins)/len(wins) if wins else 0
        avg_l=sum(loss)/len(loss) if loss else 0
        rr=abs(avg_w/avg_l) if avg_l else 0
        # Monthly
        monthly=collections.defaultdict(float)
        for t in self.trades:
            sec=t.exit_ts
            yr=1970+(sec//31557600); mo=((sec%31557600)//2592000)+1
            monthly[f"{yr}-{mo:02d}"]+=t.pnl_usd
        pos_months=sum(1 for v in monthly.values() if v>0)
        print(f"\n{'='*60}")
        print(f"  {self.name}")
        print(f"  Trades: {len(pnls)}  WR: {wr:.1f}%  Net: ${cum:.2f}")
        print(f"  Avg: ${avg:.2f}  Avg_win: ${avg_w:.2f}  Avg_loss: ${avg_l:.2f}  R:R: {rr:.2f}")
        # Max DD
        peak=eq=dd=0
        for t in sorted(self.trades,key=lambda x:x.exit_ts):
            eq+=t.pnl_usd
            if eq>peak:peak=eq
            if peak-eq>dd:dd=peak-eq
        print(f"  Max DD: ${dd:.2f}  Pos months: {pos_months}/{len(monthly)}")
        print(f"  Monthly P&L:")
        cum2=0
        for m,v in sorted(monthly.items()):
            cum2+=v
            bar='#'*int(v/20) if v>0 else '.'*int(abs(v)/20)
            print(f"    {m}  {v:+8.2f}  cum={cum2:+8.2f}  {bar[:40]}")

all_stats = {
    'LON_FOLLOW': Stats('London Open Follow'),
    'NY_FADE':    Stats('NY Open Fade'),
    'MOM_FADE':   Stats('15-bar Momentum Fade'),
}

@dataclass
class Position:
    active:bool=False; is_long:bool=True
    entry:float=0; sl:float=0; tp:float=0
    size:float=0; entry_ts:int=0
    mfe:float=0; mae:float=0
    signal:str=""; max_hold_bars:int=15

def size_lot(sl_pts):
    if sl_pts<=0: return MIN_LOT
    s=RISK_USD/(sl_pts*USD_PER_PT)
    s=math.floor(s/0.001)*0.001
    return max(MIN_LOT,min(MAX_LOT,s))

print(f"Loading {DATA}...")

cur=Bar();cur_min=-1
bars=collections.deque(maxlen=65)
atr=0.0
pos=Position()
bars_in_pos=0

# Session open tracking
lon_open_bar:Optional[Bar]=None; lon_open_day=-1
ny_open_bar:Optional[Bar]=None;  ny_open_day=-1
lon_bars_since=0; ny_bars_since=0

last_ts=0; n=0

def close_pos(pos, exit_px, exit_ts, signal_stats, reason):
    if not pos.active: return
    pnl_pts = (exit_px-pos.entry) if pos.is_long else (pos.entry-exit_px)
    pnl_usd = pnl_pts * pos.size * USD_PER_PT - TOTAL_COST * pos.size * USD_PER_PT
    t = Trade(
        entry_ts=pos.entry_ts, exit_ts=exit_ts,
        entry=pos.entry, exit_px=exit_px,
        is_long=pos.is_long, size=pos.size,
        pnl_usd=pnl_usd, signal=pos.signal,
        mfe=pos.mfe, mae=pos.mae
    )
    signal_stats.add(t)
    pos.active=False

with open(DATA) as f:
    for line in f:
        n+=1
        if n%20_000_000==0: print(f"  {n//1_000_000}M rows...")
        line=line.strip()
        if not line: continue
        p=line.split(',')
        if len(p)<4: continue
        try:
            bid=float(p[2]); ask=float(p[3])
        except: continue
        if bid<=0 or ask<=bid: continue
        try:
            ds=p[0];ts_s=p[1]
            y=int(ds[:4]);mo=int(ds[4:6]);dy=int(ds[6:8])
            h=int(ts_s[:2]);mi=int(ts_s[3:5]);se=int(ts_s[6:8])
            if mo<=2:y-=1;mo+=12
            days=365*y+y//4-y//100+y//400+(153*mo+8)//5+dy-719469
            ts=(days*86400+h*3600+mi*60+se)
        except: continue

        mid=(bid+ask)*0.5
        if last_ts>0 and ts-last_ts>3600:
            cur=Bar();cur_min=-1
            if pos.active:
                close_pos(pos,mid,ts,all_stats[pos.signal],"GAP")
            lon_bars_since=0;ny_bars_since=0
        last_ts=ts

        bm=ts//60
        if bm!=cur_min:
            if cur.n>0:
                b=Bar(ts=cur_min*60,o=cur.o,h=cur.h,l=cur.l,c=cur.c,n=cur.n)
                bar_hour=(cur_min//60)%24
                bar_day=cur_min*60//86400
                rng=b.h-b.l
                atr=2/21*rng+(1-2/21)*atr if atr>0 else rng
                bars.append(b)

                # Track session opens
                if bar_hour==7 and bar_day!=lon_open_day:
                    lon_open_bar=b; lon_open_day=bar_day; lon_bars_since=0
                elif bar_hour>=7 and lon_open_day==bar_day:
                    lon_bars_since+=1

                if bar_hour==13 and bar_day!=ny_open_day:
                    ny_open_bar=b; ny_open_day=bar_day; ny_bars_since=0
                elif bar_hour>=13 and ny_open_day==bar_day:
                    ny_bars_since+=1

                # Manage open position
                if pos.active:
                    move=(mid-pos.entry) if pos.is_long else (pos.entry-mid)
                    if move>pos.mfe: pos.mfe=move
                    if move<pos.mae: pos.mae=move
                    bars_in_pos+=1
                    # SL
                    if (pos.is_long and bid<=pos.sl) or (not pos.is_long and ask>=pos.sl):
                        close_pos(pos,pos.sl,ts,all_stats[pos.signal],"SL")
                    # Max hold
                    elif bars_in_pos>=pos.max_hold_bars:
                        exit_px=bid if pos.is_long else ask
                        close_pos(pos,exit_px,ts,all_stats[pos.signal],"TIME")

                # Entry signals (only when flat)
                if not pos.active and atr>1.0 and len(bars)>=20:
                    # ── Signal 1: London Open Follow ─────────────────────
                    if (lon_open_bar and lon_open_day==bar_day and
                            4<=lon_bars_since<=40 and
                            lon_bars_since in [5,15,25,35] and
                            7<=bar_hour<=11):
                        lon_dir = 1 if lon_open_bar.body>0.05 else (-1 if lon_open_bar.body<-0.05 else 0)
                        if lon_dir!=0:
                            sl_pts=atr*1.5
                            ep=ask if lon_dir>0 else bid
                            sl=ep-sl_pts if lon_dir>0 else ep+sl_pts
                            pos=Position(active=True,is_long=lon_dir>0,
                                entry=ep,sl=sl,size=size_lot(sl_pts),
                                entry_ts=ts,signal='LON_FOLLOW',max_hold_bars=15)
                            bars_in_pos=0

                    # ── Signal 2: NY Open Fade ────────────────────────────
                    elif (ny_open_bar and ny_open_day==bar_day and
                            4<=ny_bars_since<=15 and
                            ny_bars_since in [5,10,15] and
                            13<=bar_hour<=15):
                        ny_dir = 1 if ny_open_bar.body>0.05 else (-1 if ny_open_bar.body<-0.05 else 0)
                        if ny_dir!=0:
                            # FADE: enter against NY open direction
                            fade_dir=-ny_dir
                            sl_pts=atr*1.5
                            ep=ask if fade_dir>0 else bid
                            sl=ep-sl_pts if fade_dir>0 else ep+sl_pts
                            pos=Position(active=True,is_long=fade_dir>0,
                                entry=ep,sl=sl,size=size_lot(sl_pts),
                                entry_ts=ts,signal='NY_FADE',max_hold_bars=15)
                            bars_in_pos=0

                    # ── Signal 3: 15-bar momentum fade ───────────────────
                    elif 7<=bar_hour<=17 and len(bars)>=16:
                        bl=list(bars)
                        mom15=b.c-bl[-16].c
                        if abs(mom15)>5.0 and atr>1.5:
                            fade_dir=-1 if mom15>0 else 1
                            sl_pts=atr*1.5
                            ep=ask if fade_dir>0 else bid
                            sl=ep-sl_pts if fade_dir>0 else ep+sl_pts
                            pos=Position(active=True,is_long=fade_dir>0,
                                entry=ep,sl=sl,size=size_lot(sl_pts),
                                entry_ts=ts,signal='MOM_FADE',max_hold_bars=15)
                            bars_in_pos=0

            cur=Bar();cur.ts=bm*60;cur_min=bm
        cur.upd(mid)

# Close any open position at end
if pos.active:
    close_pos(pos,mid,last_ts,all_stats[pos.signal],"EOD")

print(f"\nLoaded {n:,} rows")
total_trades=sum(len(s.trades) for s in all_stats.values())
total_pnl=sum(t.pnl_usd for s in all_stats.values() for t in s.trades)
print(f"Total trades: {total_trades}  Total P&L: ${total_pnl:.2f}")

for s in all_stats.values():
    s.report()

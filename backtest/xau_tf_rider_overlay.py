#!/usr/bin/env python3
# Rider overlay sim: replays the bank-and-reload companion over the REAL engine
# trades (from the faithful C++ driver) + the H4 price path.
# Engine entries/exits are 100% real; the rider is transparent price-path arithmetic.
import sys, csv, collections

H4   = sys.argv[1]            # H4 ohlc csv
TR   = sys.argv[2]            # trades csv from the C++ dump
LABEL= sys.argv[3] if len(sys.argv)>3 else ""
SPREAD = 0.20

# --- load H4 bars ---
bars=[]
with open(H4) as f:
    for i,l in enumerate(f):
        l=l.strip()
        if not l: continue
        if i==0 and not l[0].isdigit(): continue
        p=l.split(',')
        bars.append((int(float(p[0])), float(p[1]),float(p[2]),float(p[3]),float(p[4])))  # ts,o,h,l,c
bars.sort()

# --- D1 ATR (aggregate H4->D1 by UTC day, Wilder ATR14) to match engine's 2.0-ATR SL scale ---
import datetime
days=collections.OrderedDict()
for ts,o,h,lo,c in bars:
    d=datetime.datetime.utcfromtimestamp(ts).date()
    if d not in days: days[d]=[o,h,lo,c,ts]
    else:
        rec=days[d]; rec[1]=max(rec[1],h); rec[2]=min(rec[2],lo); rec[3]=c
d1=list(days.values())  # o,h,l,c,ts_first
atr=0.0; d1atr={}
prevc=None
for k,(o,h,lo,c,ts) in enumerate(d1):
    tr = (h-lo) if prevc is None else max(h-lo, abs(h-prevc), abs(lo-prevc))
    atr = tr if k==0 else (atr*13+tr)/14.0
    d1atr[ts]=atr
    prevc=c
# atr lookup: nearest D1 day start <= t
d1ts=sorted(d1atr)
def atr_at(t):
    import bisect
    i=bisect.bisect_right(d1ts,t)-1
    if i<0: i=0
    return d1atr[d1ts[i]]

# bars index for window walk
bar_ts=[b[0] for b in bars]
import bisect
def bars_in(t0,t1):
    i=bisect.bisect_left(bar_ts,t0); j=bisect.bisect_right(bar_ts,t1)
    return bars[i:j]

# --- load trades ---
trades=[]
with open(TR) as f:
    for r in csv.DictReader(f):
        trades.append(r)

def run(N, S, use_chop, maxlegs, engine_filter=None):
    # N = bank target in ATR; S = leg stop in ATR (0 = no independent stop, mirror mode)
    main_net=0.0; rider_net=0.0; legs_banked=0; legs_total=0
    half_main=[0.0,0.0]; half_rider=[0.0,0.0]
    ts_all=sorted(int(t['entry_ts']) for t in trades)
    split=ts_all[len(ts_all)//2]
    for t in trades:
        if engine_filter and engine_filter not in t['engine']: continue
        side=t['side']; e_ts=int(t['entry_ts']); x_ts=int(t['exit_ts'])
        e_px=float(t['entry_px']); x_px=float(t['exit_px']); m_usd=float(t['pnl_usd'])
        main_net+=m_usd
        h = 0 if e_ts<split else 1
        half_main[h]+=m_usd
        A=atr_at(e_ts)
        if A<=0: A= max(1.0, abs(e_px-x_px))  # fallback
        bank=N*A; stop=S*A
        path=bars_in(e_ts, x_ts)
        if not path:
            # no intrabar path: last leg = main move
            rp=(e_px-x_px) if side=="SHORT" else (x_px-e_px)
            rp-=SPREAD; rider_net+=rp; half_rider[h]+=rp; legs_total+=1
            continue
        leg_open=e_px; legged=0; r=0.0; reloads=0
        for (bts,bo,bh,bl,bc) in path:
            # favorable bank check (SL-first: check stop first if S>0)
            if side=="SHORT":
                fav = bl <= leg_open - bank
                adv = (S>0) and (bh >= leg_open + stop)
            else:
                fav = bh >= leg_open + bank
                adv = (S>0) and (bl <= leg_open - stop)
            if adv and not fav:
                r += -stop - SPREAD; legged+=1
                # re-arm at this bar close if room + not chop
                if reloads<maxlegs-1 and (not use_chop or not is_chop(bts,A)):
                    leg_open=bc; reloads+=1
                else:
                    leg_open=None; break
                continue
            if fav:
                r += bank - SPREAD; legged+=1; legs_banked+=1
                if reloads<maxlegs-1 and (not use_chop or not is_chop(bts,A)):
                    leg_open=bc; reloads+=1
                else:
                    leg_open=None; break
        # close final open leg at main exit px
        if leg_open is not None:
            rp=(leg_open - x_px) if side=="SHORT" else (x_px - leg_open)
            r += rp - SPREAD; legged+=1
        rider_net+=r; half_rider[h]+=r; legs_total+=legged
    return dict(main=main_net, rider=rider_net, combined=main_net+rider_net,
                legs=legs_total, banked=legs_banked,
                hm=half_main, hr=half_rider)

# simple chop: D1 vol-range expansion proxy unavailable on H4-only; use ATR-relative
# placeholder (off in V1). Hook kept for the sweep.
def is_chop(t,A): return False

print(f"=== RIDER SWEEP {LABEL} ===  (USD; combined = engine + rider; both-halves H1/H2)")
print(f"{'cfg':<22} {'engine':>9} {'rider':>9} {'combined':>9}  {'legs':>5} {'bank':>5}  {'H1c':>8} {'H2c':>8}")
base=run(2,0,False,1,None)
for (N,S,chop,ml,tag) in [(2,0,False,1,'mirror N2'),(2.5,0,False,1,'mirror N2.5'),
                          (3,0,False,1,'mirror N3'),(2,1,False,5,'stop N2 S1 x5'),
                          (2.5,1,False,5,'stop N2.5 S1 x5'),(3,1.5,False,5,'stop N3 S1.5 x5')]:
    r=run(N,S,chop,ml,None)
    hm=r['hm']; hr=r['hr']
    print(f"{tag:<22} {r['main']:>+9.0f} {r['rider']:>+9.0f} {r['combined']:>+9.0f}  {r['legs']:>5} {r['banked']:>5}  {hm[0]+hr[0]:>+8.0f} {hm[1]+hr[1]:>+8.0f}")
# momentum cell alone (the live-loss cell)
print("-- Momentum cell only (the live -$42 cell) --")
for (N,S,ml,tag) in [(2,0,1,'mirror N2'),(2.5,0,1,'mirror N2.5'),(2,1,5,'stop N2 S1 x5')]:
    r=run(N,S,False,ml,'Momentum')
    print(f"{tag:<22} engine={r['main']:>+7.0f} rider={r['rider']:>+7.0f} combined={r['combined']:>+7.0f} legs={r['legs']} bank={r['banked']}")

#!/usr/bin/env python3
# Generic rider overlay: replays the bank-and-reload companion over REAL engine
# trades (faithful C++ dump) using each trade's OWN atr_at_entry as the bank
# unit (atr_mode=self) and the bar CSV as the intrabar price path. Engine
# entries/exits are 100% real; the rider is transparent price-path arithmetic.
# Same banking logic as backtest/xau_tf_rider_overlay.py (mirror = no leg stop).
#
# RUN: rider_overlay_generic.py <bars_csv> <trades_csv> <label>
import sys, csv, bisect
BARS=sys.argv[1]; TR=sys.argv[2]; LABEL=sys.argv[3] if len(sys.argv)>3 else ""
SPREAD=0.20
bars=[]
with open(BARS) as f:
    for i,l in enumerate(f):
        l=l.strip()
        if not l: continue
        if i==0 and not l[0].isdigit(): continue
        p=l.split(',')
        bars.append((int(float(p[0])),float(p[1]),float(p[2]),float(p[3]),float(p[4])))  # ts,o,h,l,c
bars.sort(); bar_ts=[b[0] for b in bars]
def bars_in(t0,t1):
    i=bisect.bisect_left(bar_ts,t0); j=bisect.bisect_right(bar_ts,t1); return bars[i:j]
trades=[]
with open(TR) as f:
    for r in csv.DictReader(f): trades.append(r)
if not trades: print("no trades"); sys.exit(0)
ts_all=sorted(int(t['entry_ts']) for t in trades); split=ts_all[len(ts_all)//2]
def run(N,S,maxlegs):
    main_net=rider_net=0.0; legs=banked=0; hm=[0.0,0.0]; hr=[0.0,0.0]
    for t in trades:
        side=t['side']; e_ts=int(t['entry_ts']); x_ts=int(t['exit_ts'])
        e_px=float(t['entry_px']); x_px=float(t['exit_px']); m=float(t['pnl_usd'])
        A=float(t['atr_at_entry']);
        if A<=0: A=max(0.5,abs(e_px-x_px))
        h=0 if e_ts<split else 1
        main_net+=m; hm[h]+=m
        bank=N*A; stop=S*A; path=bars_in(e_ts,x_ts)
        if not path:
            rp=((e_px-x_px) if side=="SHORT" else (x_px-e_px))-SPREAD
            rider_net+=rp; hr[h]+=rp; legs+=1; continue
        leg_open=e_px; r=0.0; reloads=0
        for (bts,bo,bh,bl,bc) in path:
            if side=="SHORT":
                fav=bl<=leg_open-bank; adv=(S>0) and (bh>=leg_open+stop)
            else:
                fav=bh>=leg_open+bank; adv=(S>0) and (bl<=leg_open-stop)
            if adv and not fav:
                r+=-stop-SPREAD; legs+=1
                if reloads<maxlegs-1: leg_open=bc; reloads+=1
                else: leg_open=None; break
                continue
            if fav:
                r+=bank-SPREAD; legs+=1; banked+=1
                if reloads<maxlegs-1: leg_open=bc; reloads+=1
                else: leg_open=None; break
        if leg_open is not None:
            rp=((leg_open-x_px) if side=="SHORT" else (x_px-leg_open))-SPREAD
            r+=rp; legs+=1
        rider_net+=r; hr[h]+=r
    return dict(main=main_net,rider=rider_net,comb=main_net+rider_net,legs=legs,banked=banked,hm=hm,hr=hr)
print(f"=== RIDER OVERLAY {LABEL} ===  n_trades={len(trades)}  (USD; combined=engine+rider; both-halves)")
print(f"{'cfg':<20} {'engine':>8} {'rider':>8} {'combined':>9} {'legs':>5} {'bank':>5} {'H1c':>8} {'H2c':>8}")
for (N,S,ml,tag) in [(2,0,1,'mirror N2'),(2.5,0,1,'mirror N2.5'),(3,0,1,'mirror N3'),
                     (2,1,5,'stop N2 S1 x5'),(2.5,1,5,'stop N2.5 S1 x5')]:
    r=run(N,S,ml); hm=r['hm']; hr=r['hr']
    print(f"{tag:<20} {r['main']:>+8.0f} {r['rider']:>+8.0f} {r['comb']:>+9.0f} {r['legs']:>5} {r['banked']:>5} {hm[0]+hr[0]:>+8.0f} {hm[1]+hr[1]:>+8.0f}")

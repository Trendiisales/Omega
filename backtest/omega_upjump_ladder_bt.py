#!/usr/bin/env python3
"""OMEGA upjump/no-floor LADDER companion research (operator order 3, S-2026-07-07x).

Extends the crypto/BIGCAP no-floor ladder pattern to the Omega real-engine
instruments (indices / gold / FX) on H1 bars, replacing the retired BE-floor
family. SEPARATE INDEPENDENT book, judged STANDALONE (never vs-WIDE).

Mechanism = StockDayMoverLadderCompanion structure, thr-scaled (BIGCAP ratios
to its 3%-day-mover threshold):
  detector : close rises >= thr% off the min low of the last W H1 bars
  TIGHT    : arm +0.17*thr, trail giveback 0.67*thr (abs % of entry)
  WIDE     : arm +2.7*thr,  trail 50% of MFE (g50)
  STACKED  : arms +{0.67,1.33,2.0}*thr, each g50
  cap      : 5 clips/window, reclip on further +1.67*thr
  LOSS_CUT : 5*thr per clip
Fills at H1 closes for entries/arms; trail/LC evaluated intrabar via H1 low
(conservative SL-first: low before high within a bar). Costs debited per clip
RT in bp of entry. Controls: 2x cost, WF halves, both regime files where they
exist, RANDOM-WINDOW control (same window count, random starts; the detector
must beat it).

Usage: python3 backtest/omega_upjump_ladder_bt.py
"""
import csv, math, random, os, sys

TICK = "/Users/jo/Tick"

# (label, path, rt_cost_bp, regime)  -- H1 csv: ts,o,h,l,c[,extra]
FILES = [
    ("XAU_bull",   f"{TICK}/2yr_XAUUSD_tick_fresh.h1.csv",  4.0, "bull"),
    ("XAU_bear",   f"{TICK}/XAUUSD_2022_2023.h1.csv",       4.0, "bear"),
    ("GER40_bull", f"{TICK}/GRXEUR_merged.h1.csv",          2.0, "bull"),
    ("GER40_bear", f"{TICK}/DAX2022_merged.h1.csv",         2.0, "bear"),
    ("NAS_bear",   f"{TICK}/NAS2022_bear_h1.csv",           2.0, "bear"),
    ("EURUSD",     f"{TICK}/EURUSD_merged.h1.csv",          2.0, "mixed"),
    ("GBPUSD",     f"{TICK}/GBPUSD_befloor_h1.csv",         2.0, "mixed"),
    ("AUDUSD",     f"{TICK}/AUDUSD_befloor_h1.csv",         2.0, "mixed"),
    ("NZDUSD",     f"{TICK}/NZDUSD_befloor_h1.csv",         2.5, "mixed"),
    ("USDJPY",     f"{TICK}/USDJPY_befloor_h1.csv",         2.0, "mixed"),
    ("USDCAD",     f"{TICK}/USDCAD_befloor_h1.csv",         2.0, "mixed"),
]

def load(path):
    out=[]
    with open(path) as f:
        for ln in f:
            p=ln.strip().split(',')
            if len(p)<5 or not p[0][:1].isdigit(): continue
            try: out.append((int(float(p[0])),float(p[1]),float(p[2]),float(p[3]),float(p[4])))
            except ValueError: continue
    return out

class Clip:
    __slots__=("entry","peak","armed","kind","g50","arm_px","trail_abs")
    def __init__(self,entry,kind,arm_pct,trail_abs_pct=None):
        self.entry=entry; self.peak=entry; self.armed=False; self.kind=kind
        self.arm_px=entry*(1+arm_pct/100.0)
        self.g50 = trail_abs_pct is None
        self.trail_abs = entry*(trail_abs_pct/100.0) if trail_abs_pct is not None else 0.0

def run(bars, W, thr, cost_bp, seed_windows=None):
    """Returns (list of clip returns in %, window_starts). seed_windows: use these
    start indices instead of the detector (random control)."""
    a_t, s_t          = 0.17*thr, 0.67*thr
    a_w               = 2.7*thr
    stacked           = [0.67*thr, 1.33*thr, 2.0*thr]
    reclip_pct        = 1.67*thr
    lc_pct            = 5.0*thr
    CAP               = 5
    rets=[]; win_starts=[]
    open_clips=[]; win_active=False; win_end=0; nclips=0; last_reclip_px=0.0
    seed=set(seed_windows or ())
    for i in range(W, len(bars)):
        ts,o,h,l,c = bars[i]
        # ---- manage open clips intrabar: low first (SL-first), then high, then close
        still=[]
        for cl in open_clips:
            closed=False
            for px in (l, h, c):
                if closed: break
                if not cl.armed:
                    if px <= cl.entry*(1-lc_pct/100.0):
                        rets.append(-(lc_pct)-cost_bp/100.0); closed=True; break
                    if px >= cl.arm_px: cl.armed=True; cl.peak=max(cl.peak,px)
                else:
                    cl.peak=max(cl.peak,px)
                    if cl.g50:
                        gain=cl.peak-cl.entry
                        stop=cl.entry+0.5*gain
                    else:
                        stop=cl.peak-cl.trail_abs
                    if px <= stop:
                        rets.append((stop-cl.entry)/cl.entry*100.0 - cost_bp/100.0); closed=True; break
            if not closed: still.append(cl)
        open_clips=still
        # ---- window lifecycle
        if win_active and i>=win_end:
            # window over: flatten remaining clips at close (honest EOW flush)
            for cl in open_clips:
                rets.append((c-cl.entry)/cl.entry*100.0 - cost_bp/100.0)
            open_clips=[]; win_active=False
        # ---- detector / seeded windows
        trigger=False
        if seed_windows is None:
            wl=min(b[3] for b in bars[i-W:i])
            if wl>0 and (c-wl)/wl*100.0 >= thr: trigger=True
        else:
            trigger = i in seed
        if trigger and not win_active:
            win_active=True; win_end=i+W; nclips=0; last_reclip_px=c; win_starts.append(i)
        if win_active and nclips<CAP:
            if nclips==0:
                open_clips.append(Clip(c,"T",a_t,s_t))
                open_clips.append(Clip(c,"W",a_w))
                for sa in stacked: open_clips.append(Clip(c,"S",sa))
                nclips=1; last_reclip_px=c
            elif c >= last_reclip_px*(1+reclip_pct/100.0):
                open_clips.append(Clip(c,"W",a_w))
                nclips+=1; last_reclip_px=c
    for cl in open_clips:
        rets.append((bars[-1][4]-cl.entry)/cl.entry*100.0 - cost_bp/100.0)
    return rets, win_starts

def stats(rets):
    if not rets: return dict(n=0,net=0,pf=0,dd=0,h1=0,h2=0,wf=False)
    n=len(rets); net=sum(rets)
    gw=sum(r for r in rets if r>0); gl=-sum(r for r in rets if r<0)
    pf=gw/gl if gl>0 else 99.9
    cur=peak=dd=0.0
    for r in rets:
        cur+=r; peak=max(peak,cur); dd=max(dd,peak-cur)
    mid=n//2; h1=sum(rets[:mid]); h2=sum(rets[mid:])
    return dict(n=n,net=net,pf=pf,dd=dd,h1=h1,h2=h2,wf=(net>0 and h1>0 and h2>0))

def main():
    random.seed(20260707)
    Ws=[24,48,96]
    print(f"{'file':11s} {'W':>3s} {'thr':>4s} | {'n':>5s} {'net%':>8s} {'PF':>5s} {'DD%':>7s} {'WF':>3s} | {'2x n/net/PF':>18s} | {'rand net%':>9s}")
    for label,path,cost,regime in FILES:
        if not os.path.exists(path): print(f"{label}: MISSING {path}"); continue
        bars=load(path)
        if len(bars)<500: print(f"{label}: thin ({len(bars)})"); continue
        thrs=[0.5,1.0,1.5] if bars[0][4]<10 else ([0.5,1.0,1.5] if bars[0][4]<200 else [1.0,2.0,3.0])
        for W in Ws:
            for thr in thrs:
                rets,wins=run(bars,W,thr,cost)
                s=stats(rets)
                if s['n']==0: continue
                r2,_=run(bars,W,thr,cost*2.0)
                s2=stats(r2)
                # random-window control (same count, avg of 5 seeds)
                rnd_nets=[]
                for sd in range(5):
                    random.seed(1000+sd)
                    cand=random.sample(range(W,len(bars)),min(len(wins),len(bars)-W)) if wins else []
                    rr,_=run(bars,W,thr,cost,seed_windows=cand)
                    rnd_nets.append(stats(rr)['net'])
                rnd=sum(rnd_nets)/len(rnd_nets) if rnd_nets else 0.0
                print(f"{label:11s} {W:3d} {thr:4.1f} | {s['n']:5d} {s['net']:+8.1f} {s['pf']:5.2f} {s['dd']:7.1f} "
                      f"{'✓' if s['wf'] else '✗':>3s} | {s2['n']:5d}/{s2['net']:+7.1f}/{s2['pf']:4.2f} | {rnd:+9.1f}")
    print("\nnet% = sum of clip returns in % of clip notional (1 unit/clip), costs debited per clip RT.")

if __name__=='__main__':
    main()

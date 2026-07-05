#!/usr/bin/env python3
"""GOLD long+short BE-floor companion — faithful mirror of the crypto BE-floor
mechanism (/tmp/be_bptrail.py), two flavors:

  LONG  flavor: up-detector (Wh/+thr) -> BE-floor long clip. Stays FLAT until price
                rises >= be_bp above ref (covers RT cost) -> opens there (net starts 0)
                -> stop = max(entry, hwm*(1-gb/1e4)) sits AT-or-above entry, trails UP
                -> exit_px >= entry ALWAYS -> net_bp >= 0 EVERY clip. Reclip from exit.
  SHORT flavor: down-detector (Wh/-thr) -> BE-floor short clip. MIRROR: stays FLAT until
                price falls >= be_bp below ref -> opens short -> stop = min(entry,
                lwm*(1+gb/1e4)) sits AT-or-below entry, trails DOWN -> exit_px <= entry
                ALWAYS -> short net_bp >= 0 EVERY clip. Reclip from exit.

Both = x2 tiers (tight banker gb / wide runner gb), BE-floor covered, neg MUST be 0 by
construction. Gold CAN short (no 200DMA bull-gate; the SHORT flavor IS the bear-capture).

STANDALONE additive companion (feedback-companion-independent-engine) — never judged vs
parent/WIDE. Real gold RT cost (project-ibkr-cost-basis): 2*0.00015*px commission (3bp)
+ spread (~2-3bp) => be_bp floor = GOLD_RT_BP. 1bp = 0.01% of notional.
"""
import csv, sys, os

SMA_BARS = 200
GOLD_RT_BP = 6.0   # gold round-trip cost floor (3bp comm + ~3bp spread), conservative

# ── data ──────────────────────────────────────────────────────────────────
def load(path):
    ts=[];o=[];h=[];l=[];c=[]
    with open(path) as f:
        r=csv.reader(f); next(r)
        for row in r:
            ts.append(int(row[0])); o.append(float(row[1]))
            h.append(float(row[2])); l.append(float(row[3])); c.append(float(row[4]))
    N=len(ts); sma=[None]*N; run=0.0
    for i in range(N):
        run+=c[i]
        if i>=SMA_BARS: run-=c[i-SMA_BARS]
        if i>=SMA_BARS-1: sma[i]=run/SMA_BARS
    return ts,o,h,l,c,N,sma

# ── detectors (parent windows) ────────────────────────────────────────────
def parent_long(bars,W,thr):
    """UP detector: enter on Wh/+thr push, window closes on the -thr reversal."""
    ts,o,h,l,c,N,sma=bars; trades=[];pos=False;ent=None
    for i in range(W,N):
        j=c[i]/c[i-W]-1.0
        if not pos and j>=thr:
            ei=i+1
            if ei>=N: continue
            pos=True; ent=(ei,o[ei])
        elif pos and j<=-thr:
            xi=i+1
            if xi>=N: xi=N-1
            trades.append((ent[0],xi,ent[1])); pos=False;ent=None
    if pos: trades.append((ent[0],N-1,ent[1]))
    return trades

def parent_short(bars,W,thr):
    """DOWN detector: enter on Wh/-thr push, window closes on the +thr reversal (mirror)."""
    ts,o,h,l,c,N,sma=bars; trades=[];pos=False;ent=None
    for i in range(W,N):
        j=c[i]/c[i-W]-1.0
        if not pos and j<=-thr:
            ei=i+1
            if ei>=N: continue
            pos=True; ent=(ei,o[ei])
        elif pos and j>=thr:
            xi=i+1
            if xi>=N: xi=N-1
            trades.append((ent[0],xi,ent[1])); pos=False;ent=None
    if pos: trades.append((ent[0],N-1,ent[1]))
    return trades

# ── BE-floor clip books ───────────────────────────────────────────────────
def leg_book_long(bars,trades,be_bp,gb_bp):
    ts,o,h,l,c,N,sma=bars; rows=[]
    for ei,xi,epx in trades:
        bull=(sma[ei] is not None and c[ei]>sma[ei])
        ref=o[ei]; entry=None; hwm=None
        for i in range(ei,xi):
            cur=c[i]
            if entry is None:
                if (cur/ref-1.0)*1e4>=be_bp: entry=cur; hwm=cur   # cover BE, then open
                continue
            if cur>hwm: hwm=cur
            stop=max(entry, hwm*(1.0-gb_bp/1e4))                  # BE-floor: stop >= entry
            if cur<=stop:
                g=max(0.0,(stop/entry-1.0)*1e4); rows.append((ts[i],g/100.0,bull))
                ref=stop; entry=None; hwm=None                    # reclip from exit
        if entry is not None:
            last=c[xi-1] if xi-1>=ei else o[ei]
            rows.append((ts[xi],max(0.0,(last/entry-1.0)*1e4)/100.0,bull))
    return rows

def leg_book_short(bars,trades,be_bp,gb_bp):
    ts,o,h,l,c,N,sma=bars; rows=[]
    for ei,xi,epx in trades:
        bull=(sma[ei] is not None and c[ei]>sma[ei])
        ref=o[ei]; entry=None; lwm=None
        for i in range(ei,xi):
            cur=c[i]
            if entry is None:
                if (1.0-cur/ref)*1e4>=be_bp: entry=cur; lwm=cur   # price fell be_bp -> open short
                continue
            if cur<lwm: lwm=cur
            stop=min(entry, lwm*(1.0+gb_bp/1e4))                  # BE-floor: stop <= entry
            if cur>=stop:
                g=max(0.0,(entry/stop-1.0)*1e4); rows.append((ts[i],g/100.0,bull))
                ref=stop; entry=None; lwm=None                    # reclip from exit
        if entry is not None:
            last=c[xi-1] if xi-1>=ei else o[ei]
            rows.append((ts[xi],max(0.0,(entry/last-1.0)*1e4)/100.0,bull))
    return rows

def x2(book,bars,trades,tgb,wgb):
    return book(bars,trades,GOLD_RT_BP,tgb)+book(bars,trades,GOLD_RT_BP,wgb)

def met(r):
    if not r: return None
    net=sum(x[1] for x in r); neg=sum(1 for x in r if x[1]<-1e-9)
    wins=sum(1 for x in r if x[1]>1e-9)
    return dict(n=len(r),net=net,neg=neg,wins=wins)
def sl(r,lo): return met([x for x in r if x[0]>=lo])
def half_lo(r,hi): return met([x for x in r if x[0]<hi])   # first half [start,hi)

TIGHT=[20,40,80]      # bp giveback (banker)  — operator spec primary = 20
WIDE =[150,300,600]   # bp giveback (runner)  — operator spec primary = 150

FILES=[("2yr_bull",  "/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv"),
       ("2022_2023", "/Users/jo/Tick/XAUUSD_2022_2023.h1.csv")]

def run(label,path,W,thr):
    bars=load(path)
    tl=parent_long(bars,W,thr); ts_=parent_short(bars,W,thr)
    half=(bars[0][0]+bars[0][-1])//2   # WF split at median ts
    print(f"\n########## {label}  ({os.path.basename(path)})  W={W}h thr={thr*100:+.1f}%  RT={GOLD_RT_BP:.0f}bp")
    print(f"  LONG events={len(tl)}   SHORT events={len(ts_)}")
    for flavor,book,trades in [("LONG",leg_book_long,tl),("SHORT",leg_book_short,ts_)]:
        print(f"  --- {flavor} flavor  (x2: tight banker / wide runner) ---")
        print(f"    {'T/W bp':10s} {'clips':>6s} {'neg':>4s} {'wins':>5s} {'FULL_%':>8s} {'WF-H1_%':>8s} {'WF-H2_%':>8s}")
        res=[]
        for tg in TIGHT:
            for wg in WIDE:
                r=x2(book,bars,trades,tg,wg); m=met(r)
                if not m: continue
                res.append((tg,wg,m,half_lo(r,half),sl(r,half)))
        res.sort(key=lambda x:-(x[2]["net"]))
        for tg,wg,m,h1,h2 in res:
            star=" *SPEC" if (tg==20 and wg==150) else ""
            h1n=f"{h1['net']:+8.1f}" if h1 else "     n/a"
            h2n=f"{h2['net']:+8.1f}" if h2 else "     n/a"
            print(f"    {tg:>3d}/{wg:<4d}  {m['n']:6d} {m['neg']:4d} {m['wins']:5d} {m['net']:+8.1f} {h1n} {h2n}{star}")

def main():
    W=int(sys.argv[1]) if len(sys.argv)>1 else 2
    thr=float(sys.argv[2]) if len(sys.argv)>2 else 0.01
    print(f"GOLD BE-FLOOR long+short companion  |  operator spec = W=2h thr=+/-1% x2(20/150) BE-floor")
    print(f"neg MUST be 0 for every row (BE-floor: exit >= entry long / <= entry short by construction).")
    print(f"Net in bp (1bp = 0.01% notional). STANDALONE additive — not vs parent/WIDE.")
    for label,path in FILES:
        run(label,path,W,thr)

if __name__=="__main__": main()

#!/usr/bin/env python3
"""FX AuPos/AuNeg BE-floor companion — VIABILITY RESEARCH (faithful port of the gold
mechanism in backtest/gold_befloor_ls.py, adapted to forex).

Same STANDALONE additive companion (feedback-companion-independent-engine): a 2h (W=2 H1)
jump detector arms an up-window (LONG/AuPos) or down-window (SHORT/AuNeg); inside the window
a BE-floor leg stays FLAT until price clears +be_bp from ref (covers RT cost), opens THERE,
stop sits at/above entry (long) or at/below entry (short) and trails only the favourable way
=> exit >= entry (long) / <= entry (short) ALWAYS => net_bp >= 0 on EVERY clip BY CONSTRUCTION.
neg MUST be 0. Judge standalone (net>0 both WF halves), never vs a parent / vs WIDE.

FX adaptations (handoff): thr LOWER than gold's 1% (majors rarely move 1% in 2h; sweep
0.2-1.0%), be_bp SMALLER than gold's 6bp (major RT spread+comm ~1-3bp), giveback tiers
scaled down. Net in % of notional (1% ~ $1000 / std lot on a USD-quote major).

  python3 fx_befloor_ls.py [file.csv ...]     # default: all majors it can find
"""
import csv, sys, os, glob

W = 2                                  # detector window (H1 bars) = 2h
THRS = [0.002, 0.003, 0.004, 0.005, 0.007, 0.010]   # 2h jump arm thresholds
BE_BPS = [1.0, 2.0, 3.0]               # RT cost floor to sweep (bp; 1bp=0.01% notional)
TIGHT = [5, 10, 20]                    # banker giveback (bp)
WIDE  = [40, 80, 150]                  # runner giveback (bp)


def load(path):
    ts=[];o=[];h=[];l=[];c=[]
    with open(path) as f:
        r=csv.reader(f)
        first=next(r)
        # tolerate header or headerless
        if first and first[0].lstrip("-").isdigit():
            rows=[first]
        else:
            rows=[]
        for row in r: rows.append(row)
        for row in rows:
            if len(row)<5 or not row[0].lstrip("-").isdigit(): continue
            ts.append(int(row[0])); o.append(float(row[1]))
            h.append(float(row[2])); l.append(float(row[3])); c.append(float(row[4]))
    return ts,o,h,l,c,len(ts)


def parent(bars,W,thr,up):
    ts,o,h,l,c,N=bars; trades=[];pos=False;ent=None
    for i in range(W,N):
        j=c[i]/c[i-W]-1.0
        enter = (j>=thr) if up else (j<=-thr)
        exit_ = (j<=-thr) if up else (j>=thr)
        if not pos and enter:
            ei=i+1
            if ei>=N: continue
            pos=True; ent=(ei,o[ei])
        elif pos and exit_:
            xi=i+1
            if xi>=N: xi=N-1
            trades.append((ent[0],xi,ent[1])); pos=False;ent=None
    if pos: trades.append((ent[0],N-1,ent[1]))
    return trades


def leg_book(bars,trades,be_bp,gb_bp,is_long):
    ts,o,h,l,c,N=bars; rows=[]
    for ei,xi,epx in trades:
        ref=o[ei]; entry=None; wm=None
        for i in range(ei,xi):
            cur=c[i]
            if entry is None:
                cond = ((cur/ref-1.0)*1e4>=be_bp) if is_long else ((1.0-cur/ref)*1e4>=be_bp)
                if cond: entry=cur; wm=cur
                continue
            if is_long:
                if cur>wm: wm=cur
                stop=max(entry, wm*(1.0-gb_bp/1e4))
                if cur<=stop:
                    g=max(0.0,(stop/entry-1.0)*1e4); rows.append((ts[i],g/100.0))
                    ref=stop; entry=None; wm=None
            else:
                if cur<wm: wm=cur
                stop=min(entry, wm*(1.0+gb_bp/1e4))
                if cur>=stop:
                    g=max(0.0,(entry/stop-1.0)*1e4); rows.append((ts[i],g/100.0))
                    ref=stop; entry=None; wm=None
        if entry is not None:
            last=c[xi-1] if xi-1>=ei else o[ei]
            g = (last/entry-1.0)*1e4 if is_long else (entry/last-1.0)*1e4
            rows.append((ts[xi],max(0.0,g)/100.0))
    return rows


def x2(bars,trades,be,tg,wg,is_long):
    return leg_book(bars,trades,be,tg,is_long)+leg_book(bars,trades,be,wg,is_long)

# Real RT bid-ask cost per pair (median spread, bp) from fx_tick_to_h1.py — deducted
# per clip: every clip is one open+close round trip => pays ~1 full spread. HONEST net.
RT_COST = {"EURUSD":0.7, "AUDUSD":1.61, "NZDUSD":1.95, "USDCAD":0.90, "EURGBP":1.07}
_COST = 0.0   # set per-file in run()

def met(r):
    if not r: return None
    gross=sum(x[1] for x in r)
    net=gross - len(r)*_COST/100.0            # deduct real RT spread per clip
    return dict(n=len(r), gross=gross, net=net,
                neg=sum(1 for x in r if x[1]<-1e-9), wins=sum(1 for x in r if x[1]>1e-9))
def sl(r,lo): return met([x for x in r if x[0]>=lo])
def half_lo(r,hi): return met([x for x in r if x[0]<hi])


def run(path):
    global _COST
    bars=load(path); N=bars[5]
    if N<300:
        print(f"  SKIP {os.path.basename(path)} — only {N} bars"); return
    base=os.path.basename(path)
    _COST=next((v for k,v in RT_COST.items() if k in base), 1.5)   # per-pair real spread; 1.5 default
    half=(bars[0][0]+bars[0][-1])//2
    span_d=(bars[0][-1]-bars[0][0])/86400.0
    print(f"\n########## {base}  bars={N}  span={span_d:.0f}d  W={W}h  RT_cost={_COST:.2f}bp/clip")
    print(f"  net = GROSS minus real RT spread per clip. Report ONLY cost-adjusted net (both WF halves must be +).")
    print(f"{'thr%':>5s} {'be':>3s} {'flav':>5s} {'BEST T/W':>9s} {'clips':>6s} {'neg':>4s} {'wins':>5s} "
          f"{'GROSS%':>8s} {'NET%':>8s} {'WF-H1%':>8s} {'WF-H2%':>8s} {'~$/lot':>8s}")
    for thr in THRS:
        tl=parent(bars,W,thr,True); tsr=parent(bars,W,thr,False)
        for flav,trades,is_long in [("AuPos",tl,True),("AuNeg",tsr,False)]:
            for be in BE_BPS:
                best=None
                for tg in TIGHT:
                    for wg in WIDE:
                        r=x2(bars,trades,be,tg,wg,is_long); m=met(r)
                        if not m: continue
                        cand=(tg,wg,m,half_lo(r,half),sl(r,half))
                        if best is None or m["net"]>best[2]["net"]: best=cand
                if not best: continue
                tg,wg,m,h1,h2=best
                h1n=f"{h1['net']:+8.2f}" if h1 else "     n/a"
                h2n=f"{h2['net']:+8.2f}" if h2 else "     n/a"
                usd=m["net"]*1000.0/2.0   # net% * $1000/1%/lot, /2 because x2 stacks two tiers
                print(f"{thr*100:5.2f} {be:3.0f} {flav:>5s} {tg:>4d}/{wg:<4d} {m['n']:6d} {m['neg']:4d} "
                      f"{m['wins']:5d} {m['gross']:+8.2f} {m['net']:+8.2f} {h1n} {h2n} {usd:+8.0f}")


def main():
    args=sys.argv[1:]
    if not args:
        cands=["/Users/jo/Tick/EURUSD_merged.h1.csv"]
        cands+=sorted(glob.glob("/Users/jo/Tick/*_befloor_h1.csv"))  # aggregated majors, if built
        args=[p for p in cands if os.path.exists(p)]
    print("FX BE-FLOOR AuPos/AuNeg companion — STANDALONE additive, neg MUST=0 (BE-floor by construction).")
    print("Net in % of notional (1% ~ $1000/std lot). Best T/W tier shown per (thr,be,flavor).")
    for p in args: run(p)


if __name__=="__main__": main()

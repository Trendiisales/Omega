#!/usr/bin/env python3
# (S-2026-06-24) Faithful daily turtle BT on REAL index data (we HAVE it: /Users/jo/Tick
# SPX/DJ30/NDX/UK100 daily 2016-2026, incl 2018+2020+2022 bears). Tests whether the
# not-BEAR regime gate (that HELPED BigCapMomo momentum) helps or HURTS the index turtle.
# PRIOR (NasTurtleD1Engine.hpp header): an ema100 trend filter was TESTED + REJECTED -- it
# destroys 2022-bear protection (blocks the bear-bounce breakouts that ARE the turtle's edge).
# not-BEAR is the same class of filter -> expect HURT. Confirm with BACKTEST_TRUTH.
# Config mirrors NasTurtleD1Engine: Donch20 entry / Donch10 exit / ATR14 sl1.5 tp5.0, long-only.
import csv, math, sys

FILES = {
    "SPX":  "/Users/jo/Tick/SPX_daily_2016_2026.csv",
    "DJ30": "/Users/jo/Tick/DJ30_daily_2016_2026.csv",
    "NDX":  "/Users/jo/Tick/NDX_daily_2016_2026.csv",
    "UK100":"/Users/jo/Tick/UK100_daily_2016_2026.csv",
}
N_IN=20; N_OUT=10; ATR_N=14; SL_ATR=1.5; TP_ATR=5.0; COST=0.0003  # index CFD ~3bps

def load(p):
    O=[];H=[];L=[];C=[];T=[]
    with open(p) as fh:
        for row in csv.reader(fh):
            if len(row)<5: continue
            try:
                T.append(int(float(row[0]))); O.append(float(row[1])); H.append(float(row[2]))
                L.append(float(row[3])); C.append(float(row[4]))
            except: pass
    return T,O,H,L,C

def year(ts):
    # epoch sec -> year (approx, UTC)
    return 1970 + int(ts//31557600)

def regime(C, t, SMA=200, SLOPE=20):
    if t < SMA+SLOPE: return "WARMUP"
    sma=sum(C[t-SMA+1:t+1])/SMA
    smap=sum(C[t-SMA-SLOPE+1:t-SLOPE+1])/SMA
    c=C[t]
    if   c>sma and sma>smap: return "BULL"
    elif c<sma and sma<smap: return "BEAR"
    return "NEUTRAL"

def run(T,O,H,L,C, gate):
    n=len(C); inpos=False; entry=peak=0; trades=[]; etime=0
    # ATR (Wilder-ish simple)
    for t in range(max(N_IN,ATR_N)+1, n):
        atr=sum(max(H[k]-L[k], abs(H[k]-C[k-1]), abs(L[k]-C[k-1])) for k in range(t-ATR_N,t))/ATR_N
        atrp=atr/C[t] if C[t] else 0
        if inpos:
            don_lo=min(L[t-N_OUT:t])
            sl=entry*(1-SL_ATR*atrp_e); tp=entry*(1+TP_ATR*atrp_e)
            ex=None
            if C[t]<=don_lo: ex=C[t]
            elif L[t]<=sl: ex=sl
            elif H[t]>=tp: ex=tp
            if ex is not None:
                r=ex/entry-1-2*COST; trades.append((r, year(T[etime]), regime(C,etime,))); inpos=False
        if not inpos:
            don_hi=max(H[t-N_IN:t])
            if C[t]>=don_hi:
                reg = regime(C,t)
                ok = True
                if gate=="notbear": ok = (reg!="BEAR")
                if gate=="bull":    ok = (reg=="BULL")
                if ok and reg!="WARMUP":
                    inpos=True; entry=C[t]; atrp_e=atrp; etime=t
    if inpos:
        r=C[-1]/entry-1-2*COST; trades.append((r, year(T[etime]), regime(C,etime)))
    return trades

def stats(ts):
    if not ts: return "n=0"
    rs=[x[0] for x in ts]; w=[r for r in rs if r>0]; l=[-r for r in rs if r<0]
    pf=sum(w)/sum(l) if l else float('inf')
    net=sum(rs)*100; wr=100*len(w)/len(rs)
    return f"n={len(rs):<3} WR={wr:3.0f}% PF={pf:4.2f} net={net:+6.1f}%"

if __name__=="__main__":
    for sym,path in FILES.items():
        try: T,O,H,L,C=load(path)
        except Exception as e: print(sym,"load err",e); continue
        print(f"\n=== {sym}  ({len(C)} daily bars) ===")
        for gate in ("none","notbear","bull"):
            tr=run(T,O,H,L,C,gate)
            bear22=[x for x in tr if x[1]==2022]
            print(f"  gate={gate:<8} ALL {stats(tr)}   | 2022 {stats(bear22)}")

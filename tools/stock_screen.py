#!/usr/bin/env python3
# stock_screen.py — "how we pick stocks" for the FvgContinuation engine.
# Ranks a universe by the traits that make the NQ continuation edge work:
# liquidity (real DOL pools + clean fills), intraday range (room to reach the
# DOL), trend/momentum (continuation works on trenders), and price floor.
# Reads data/stocks/<SYM>_d1.csv (ts,o,h,l,c,v). Prints a ranked, flagged table.
#   python tools/stock_screen.py [data/stocks]
import sys, os, glob, math

DDIR = sys.argv[1] if len(sys.argv) > 1 else "data/stocks"

# ---- thresholds (the selection screen) -------------------------------------
MIN_ADV_USD = 200e6     # >$200M avg daily $-volume: liquid, real liquidity pools
MIN_ATRPCT  = 2.0       # >=2% daily ATR%: enough range to reach a DOL intraday
MIN_PRICE   = 20.0      # >$20: avoid sub-$ noise
MIN_MOM_6M  = 0.0       # 6-month return >= 0: trender, not a faller
# (earnings filter is a RUNTIME gate -- skip +/-2 days -- not screenable here)

def load(p):
    rows=[]
    with open(p) as f:
        f.readline()
        for ln in f:
            k=ln.split(",")
            if len(k)<6: continue
            try: rows.append((int(k[0]),float(k[1]),float(k[2]),float(k[3]),float(k[4]),float(k[5])))
            except: pass
    return rows

def sma(v,n): return sum(v[-n:])/n if len(v)>=n else None
def atr(rows,n=14):
    if len(rows)<n+1: return None
    trs=[]
    for i in range(len(rows)-n,len(rows)):
        h,l,pc=rows[i][2],rows[i][3],rows[i-1][4]
        trs.append(max(h-l,abs(h-pc),abs(l-pc)))
    return sum(trs)/n

results=[]
for p in sorted(glob.glob(f"{DDIR}/*_d1.csv")):
    sym=os.path.basename(p).replace("_d1.csv","")
    r=load(p)
    if len(r)<200: continue
    closes=[x[4] for x in r]; vols=[x[5] for x in r]
    px=closes[-1]
    adv=sum(closes[i]*vols[i] for i in range(len(r)-20,len(r)))/20
    a=atr(r); atrpct=100*a/px if a else 0
    hi52=max(x[2] for x in r[-252:]); dist52=100*(px/hi52-1)
    mom6=100*(px/closes[-126]-1) if len(closes)>=126 else 0
    s50,s200=sma(closes,50),sma(closes,200)
    trend = (s50 and s200 and px>s50>s200)
    # composite score (higher = better continuation candidate)
    score = (math.log10(max(adv,1))*1.0) + (atrpct*0.8) + (mom6*0.05) + (10 if trend else 0) + (dist52*0.10)
    tradeable = (adv>=MIN_ADV_USD and atrpct>=MIN_ATRPCT and px>=MIN_PRICE and mom6>=MIN_MOM_6M and trend)
    results.append((sym,px,adv,atrpct,mom6,dist52,trend,score,tradeable))

results.sort(key=lambda x:-x[7])
print(f"{'SYM':6} {'px':>8} {'ADV$M':>8} {'ATR%':>5} {'mom6m%':>7} {'d52h%':>6} {'trend':>5} {'score':>6} PICK")
print("-"*64)
for sym,px,adv,atrpct,mom6,d52,tr,sc,tradeable in results:
    print(f"{sym:6} {px:8.2f} {adv/1e6:8.0f} {atrpct:5.1f} {mom6:+7.1f} {d52:+6.1f} {str(tr):>5} {sc:6.1f} {'  YES' if tradeable else '   no'}")
picks=[r[0] for r in results if r[8]]
print("\nTRADEABLE BASKET (passes screen):", ",".join(picks) if picks else "(none)")
print("Then: backtest each via fvg_core on its 15m bars; only both-halves+/cost-robust names -> shadow.")

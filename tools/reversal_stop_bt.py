# Reversal safe-stop edge-impact: ride-wide vs ride-wide + CONFIRMED-REVERSAL stop.
# Reversal-stop fires ONLY when: trade WAS armed (peak>=GATE), then reversed THROUGH entry to a loss,
# AND regime confirms sustained-against (close<SMA200 AND SMA200 falling = sustained bear, for a long).
# This is NOT a pullback clip -> it only acts on a dead, reversed trend. Proves: does it keep net + cut DD?
import datetime
TICK="/Users/jo/Tick"; COST=2.0/1e4
def load(p):
    out=[]
    for ln in open(p):
        x=ln.strip().split(",")
        if len(x)<5 or not x[1].replace('.','',1).isdigit(): continue
        try: ts=int(float(x[0]))
        except: continue
        out.append((datetime.date.fromtimestamp(ts),float(x[1]),float(x[2]),float(x[3]),float(x[4])))
    return out
def sma(a,i,n):
    if i<n: return None
    return sum(a[i-n:i])/n
def run(sym, reversal_stop):
    b=load(f"{TICK}/{sym}_daily_2016_2026.csv"); n=len(b)
    H=[x[2] for x in b]; L=[x[3] for x in b]; C=[x[4] for x in b]
    GATE=0.03  # "armed" = was up >=3%
    pos=None; trades=[]; eq=0.0; peak_eq=0.0; maxdd=0.0
    for i in range(200,n):
        d,o,h,l,c=b[i]
        dch_hi=max(H[i-20:i]); dch_lo=min(L[i-10:i])
        s200=sma(C,i,200); s200_prev=sma(C,i-20,200)
        sustained_bear = (s200 is not None and s200_prev is not None and c<s200 and s200<s200_prev)
        if pos is None:
            if c>dch_hi and i+1<n: pos=[b[i+1][1],i,c]  # entry, idx, peak_close
        else:
            epx,eidx,pk=pos; pk=max(pk,c); pos[2]=pk
            armed = (pk-epx)/epx >= GATE
            reversed_to_loss = c < epx
            ex=None; why=None
            # CONFIRMED REVERSAL STOP (only if enabled): armed + reversed-through-to-loss + sustained bear
            if reversal_stop and armed and reversed_to_loss and sustained_bear:
                ex=b[i+1][1] if i+1<n else c; why="REVERSAL_STOP"
            elif c<dch_lo:                                  # normal structural exit (ride-wide)
                ex=b[i+1][1] if i+1<n else c; why="DONCH_EXIT"
            if ex is not None:
                r=(ex-epx)/epx-COST; trades.append((d,r,why)); eq+=r
                peak_eq=max(peak_eq,eq); maxdd=max(maxdd,peak_eq-eq); pos=None
    if not trades: return None
    rets=[t[1] for t in trades]; gw=sum(x for x in rets if x>0); gl=-sum(x for x in rets if x<0)
    pf=gw/gl if gl>0 else 99
    rs=sum(1 for t in trades if t[2]=="REVERSAL_STOP")
    return (len(trades),round(pf,2),round(eq*100,1),round(maxdd*100,1),rs)
print(f"{'engine':14} {'mode':16} {'n':>3} {'PF':>5} {'net%':>7} {'maxDD%':>7} {'rev-stops':>9}")
for sym in ["SPX","NDX","DJ30"]:
    a=run(sym,False); c=run(sym,True)
    print(f"{sym+' turtle':14} {'ride-wide':16} {a[0]:>3} {a[1]:>5} {a[2]:>7} {a[3]:>7} {'-':>9}")
    print(f"{sym+' turtle':14} {'+reversal-stop':16} {c[0]:>3} {c[1]:>5} {c[2]:>7} {c[3]:>7} {c[4]:>9}")

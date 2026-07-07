import datetime
def load(p):
    rows=[]
    for ln in open(p):
        x=ln.strip().split(',')
        if len(x)<5: continue
        try: ts=int(x[0]); o,h,l,c=map(float,x[1:5])
        except: continue
        rows.append((ts,o,h,l,c))
    return rows
B=load("/Users/jo/Tick/NSXUSD_2022_2026.h1.csv")
S=load("/Users/jo/Tick/SPXUSD_2022_2026.h1.csv")
COST=8  # bp rt NAS CFD ~2pt at 25000
def dt(ts): return datetime.datetime.utcfromtimestamp(ts)

def daily_cash(rows):
    """cash day = 13:30..20:00 UTC approx -> use 14..20 H1 bars; return dict date->(open,close,ret)"""
    days={}
    for ts,o,h,l,c in rows:
        d=dt(ts); 
        if 13<=d.hour<=20:
            k=d.date()
            if k not in days: days[k]=[o,c]
            else: days[k][1]=c
    out={}; prev=None; keys=sorted(days)
    for k in keys:
        o,c=days[k]
        pc=days[prev][1] if prev else None
        out[k]=(o,c,100*(c/pc-1) if pc else 0); prev=k
    return out

def test_overnight(rows,thr,exit_h):
    """short at last cash bar close on ret<=-thr day; cover at exit_h UTC next day"""
    days=daily_cash(rows)
    idx={}; 
    for i,(ts,o,h,l,c) in enumerate(rows): idx[ts]=i
    pnl=[]; byy={}
    for k,(o,c,ret) in days.items():
        if ret>-thr: continue
        # find bar at 20:00 that date, then bar at exit_h next day
        e=x=None
        for ts,o2,h2,l2,c2 in rows:
            d=dt(ts)
            if d.date()==k and d.hour==20: e=c2
            if e and ((d.date()-k).days>=1 and d.hour==exit_h):
                x=c2; break
        if e and x:
            p=10000*(e-x)/e-COST; pnl.append((k,p)); byy.setdefault(k.year,[]).append(p)
    if not pnl: return
    v=[p for _,p in pnl]; n=len(v); h1=v[:n//2]; h2=v[n//2:]
    print(f"  overnight-short thr={thr} exit={exit_h:02d}UTC: n={n} tot={sum(v):+6.0f} mean={sum(v)/n:+5.1f} WR={100*sum(1 for x in v if x>0)/n:.0f}% h1={sum(h1):+5.0f} h2={sum(h2):+5.0f} " + " ".join(f"{y}:{sum(w):+4.0f}" for y,w in sorted(byy.items())))

def test_bouncefail(rows,thr):
    """crash day (cash ret<=-thr): next 48h, wait bounce >=1.5% off post-crash low, then short close<prior-3-bar-low; SL=bounce high+0.5*rng, TP=post-crash low; timeout 24 bars"""
    days=daily_cash(rows); N=len(rows)
    pnl=[]; byy={}
    tsidx=[r[0] for r in rows]
    for k,(o,c,ret) in days.items():
        if ret>-thr: continue
        # start scanning from 20:00 that day
        start=None
        for i,(ts,*_) in enumerate(rows):
            d=dt(ts)
            if d.date()==k and d.hour==20: start=i; break
        if start is None: continue
        lo=rows[start][4]; lo_i=start; armed=False; hb=0
        done=False
        for i in range(start+1,min(start+72,N-1)):
            ts,o2,h2,l2,c2=rows[i]
            if l2<lo: lo=l2; lo_i=i; armed=False; hb=0
            bounce=100*(c2/lo-1)
            if bounce>=1.5: armed=True; hb=max(hb,h2)
            if armed:
                p3l=min(r[3] for r in rows[i-3:i])
                if c2<p3l:  # bounce-fail short
                    e=c2; sl=hb; tp=lo
                    for j in range(i+1,min(i+24,N)):
                        _,o3,h3,l3,c3=rows[j]
                        if h3>=sl: p=10000*(e-sl)/e-COST; break
                        if l3<=tp: p=10000*(e-tp)/e-COST; break
                    else: p=10000*(e-rows[min(i+24,N-1)][4])/e-COST
                    pnl.append((k,p)); byy.setdefault(k.year,[]).append(p); done=True
                    break
        # one trade per crash day
    if not pnl: return
    v=[p for _,p in pnl]; n=len(v); h1=v[:n//2]; h2=v[n//2:]
    print(f"  bounce-fail-short thr={thr}: n={n} tot={sum(v):+6.0f} mean={sum(v)/n:+5.1f} WR={100*sum(1 for x in v if x>0)/n:.0f}% h1={sum(h1):+5.0f} h2={sum(h2):+5.0f} " + " ".join(f"{y}:{sum(w):+4.0f}" for y,w in sorted(byy.items())))

print("NAS100 2022-2026(H1):")
for thr in (2.0,2.5,3.0):
    test_overnight(B,thr,1); test_overnight(B,thr,4); test_overnight(B,thr,7)
for thr in (2.0,2.5):
    test_bouncefail(B,thr)
print("US500:")
for thr in (2.0,2.5):
    test_overnight(S,thr,4); test_bouncefail(S,thr)

import datetime, glob
# USDJPY M1 histdata (EST fixed -> UTC +5h). Build UTC daily closes at 21:00 UTC boundary (cash close).
def load_jpy():
    bars={}
    for f in sorted(glob.glob("/Users/jo/Tick/usdjpy_m1/DAT_ASCII_USDJPY_M1_*.csv")):
        for ln in open(f):
            p=ln.strip().split(';')
            if len(p)<5: continue
            dts=p[0]
            try:
                dt=datetime.datetime.strptime(dts,"%Y%m%d %H%M%S")+datetime.timedelta(hours=5)
                c=float(p[4])
            except: continue
            bars[dt]=c
    return bars
def load_daily(p):
    rows=[]
    for ln in open(p):
        x=ln.strip().split(',')
        if len(x)<5: continue
        try: ts=int(x[0]); c=float(x[4])
        except: continue
        rows.append((datetime.datetime.utcfromtimestamp(ts).date(),c))
    return rows
print("loading...")
J=load_jpy()
jd=sorted(J)
ndx=load_daily("/Users/jo/Tick/NDX_daily_2016_2026.csv")
# USDJPY px at (date, 21:00 UTC) = last M1 <= that moment within 2h
import bisect
def jpy_at(d,hh):
    t=datetime.datetime(d.year,d.month,d.day,hh)
    i=bisect.bisect_right(jd,t)
    if i==0: return None
    x=jd[i-1]
    if (t-x).total_seconds()>7200: return None
    return J[x]
COST=1.5  # bp rt USDJPY CFD (spread ~0.3-0.5 pip + comm)
res={}
for hold in (1,2,3):
    res[hold]=[]
for i in range(1,len(ndx)):
    d,c=ndx[i]; pc=ndx[i-1][1]
    ret=100*(c/pc-1)
    if ret<=-2.0:
        e=jpy_at(d,21)
        if e is None: continue
        for hold in (1,2,3):
            xd=d
            n=0; px=None; tries=0
            dd=d
            while n<hold and tries<7:
                dd=dd+datetime.timedelta(days=1); tries+=1
                v=jpy_at(dd,21)
                if v is not None: n+=1; px=v
            if px: res[hold].append((d,10000*(e-px)/e-COST))
for hold in (1,2,3):
    v=[x for _,x in res[hold]]
    if not v: continue
    N=len(v); h1=v[:N//2]; h2=v[N//2:]
    byy={}
    for (d,p) in res[hold]: byy.setdefault(d.year,[]).append(p)
    print(f"USDJPY SHORT @cash-close hold{hold}d after NDX<=-2%: n={N} tot={sum(v):+6.0f}bp mean={sum(v)/N:+5.1f} WR={100*sum(1 for x in v if x>0)/N:.0f}% h1={sum(h1):+5.0f} h2={sum(h2):+5.0f}")
    print("   ", {y:f"{sum(w):+.0f}" for y,w in sorted(byy.items())})

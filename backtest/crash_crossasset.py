import datetime
def load_daily(p):
    out={}
    prev=None
    for ln in open(p):
        x=ln.strip().split(',')
        if len(x)<5: continue
        try: ts=int(x[0]); o,h,l,c=map(float,x[1:5])
        except: continue
        d=datetime.datetime.utcfromtimestamp(ts).date()
        out[d]=(o,h,l,c)
    return out
def xau_daily_from_m1(p):
    days={}
    for ln in open(p):
        x=ln.strip().split(',')
        if len(x)<5: continue
        try: ts=int(x[0]); c=float(x[4])
        except: continue
        d=datetime.datetime.utcfromtimestamp(ts)
        k=d.date()
        days.setdefault(k,[None,0,1e9,None])
        v=days[k]
        if v[0] is None: v[0]=float(x[1])
        v[1]=max(v[1],float(x[2])); v[2]=min(v[2],float(x[3])); v[3]=c
    return {k:tuple(v) for k,v in days.items()}
ndx=load_daily("/Users/jo/Tick/NDX_daily_2016_2026.csv")
xau=load_daily("/Users/jo/Tick/2yr_XAUUSD_daily.csv")
xau22=xau_daily_from_m1("/Users/jo/Tick/xau_m1_2022bear.csv")
xau24=xau_daily_from_m1("/Users/jo/Tick/xau_m1_2024_2026.csv")
# merge (m1-built preferred where daily missing)
XA=dict(xau24); XA.update(xau22); XA.update(xau)
print("XAU days:",len(XA),min(XA),max(XA))
def fwd(sym_map, d, n):
    ks=sorted(sym_map)
    if d not in sym_map: return None
    i=ks.index(d)
    if i+n>=len(ks): return None
    return 10000*(sym_map[ks[i+n]][3]/sym_map[d][3]-1)
ks=sorted(ndx)
res={1:[],2:[]}
byy={}
for i in range(1,len(ks)):
    d=ks[i]; pc=ndx[ks[i-1]][3]; c=ndx[d][3]
    ret=100*(c/pc-1)
    if ret<=-2.0 and d in XA:
        for n in (1,2):
            f=fwd(XA,d,n)
            if f is not None:
                res[n].append((d,f))
                if n==1: byy.setdefault(d.year,[]).append(f)
for n in (1,2):
    v=[x for _,x in res[n]]
    if not v: continue
    N=len(v); h1=v[:N//2]; h2=v[N//2:]
    print(f"XAU LONG next{n}d after NDX<=-2%: n={N} tot={sum(v):+6.0f}bp mean={sum(v)/N:+5.1f} WR={100*sum(1 for x in v if x>0)/N:.0f}% h1={sum(h1):+5.0f} h2={sum(h2):+5.0f}")
print("  by year(next1d):", {y:f"{sum(w):+.0f}" for y,w in sorted(byy.items())})
# same-day co-move (does gold dump WITH equities on crash day? liquidation risk)
sd=[]
for i in range(1,len(ks)):
    d=ks[i]; pc=ndx[ks[i-1]][3]; c=ndx[d][3]
    if 100*(c/pc-1)<=-2.0 and d in XA:
        ksx=sorted(XA); j=ksx.index(d)
        if j>0: sd.append(10000*(XA[d][3]/XA[ksx[j-1]][3]-1))
print(f"XAU same-day move on crash days: n={len(sd)} mean={sum(sd)/len(sd):+5.1f}bp  neg_share={100*sum(1 for x in sd if x<0)/len(sd):.0f}%")

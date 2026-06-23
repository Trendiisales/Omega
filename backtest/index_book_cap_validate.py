#!/usr/bin/env python3
# Validate the IndexBookBudget net-long concurrent-leg cap: does capping the
# correlated long cluster (Tue/Fri seasonal + last3/first3 TOM, x5 indices) cut
# book drawdown for an acceptable frequency/return cost? Day-by-day sim, per-leg
# fixed-unit risk (each active long leg risks 1 return-unit = its day's % move).
# Faithful: long on the day the engine holds; close-to-close daily return.
import datetime
from collections import defaultdict
TICK="/Users/jo/Tick"
F={"SPX":"SPX_daily_2016_2026.csv","NDX":"NDX_daily_2016_2026.csv","DJ30":"DJ30_daily_2016_2026.csv",
   "GER40":"GER40_daily_2016_2026.csv","UK100":"UK100_daily_2016_2026.csv"}
def load(p):
    rows=[]
    for ln in open(f"{TICK}/{p}"):
        a=ln.split(',')
        if len(a)<5: continue
        try: ts=int(float(a[0])); o,h,l,c=map(float,a[1:5])
        except: continue
        if c<=0: continue
        rows.append((datetime.datetime.utcfromtimestamp(ts).date(),o,h,l,c))
    rows.sort(); seen={}
    for r in rows: seen[r[0]]=r
    return [seen[k] for k in sorted(seen)]
DATA={k:load(v) for k,v in F.items()}
ALLDAYS=sorted({d for rows in DATA.values() for (d,_,_,_,_) in rows})

# per-index: map date->(prev_close,close) and trading-day-of-month index
def build(rows):
    ret={}; tdom={}; months=defaultdict(list)
    for i,(d,o,h,l,c) in enumerate(rows):
        if i>0: ret[d]=(c/rows[i-1][4]-1.0)
        months[(d.year,d.month)].append(d)
    keys=sorted(months); idxmap={}
    for mi,mk in enumerate(keys):
        days=months[mk]; nlast=len(keys)>mi
        for j,d in enumerate(days):
            from_end=len(days)-1-j
            idxmap[d]=(j, from_end)  # (0-based from start, from end)
    return ret, idxmap, {d:i for i,(d,_,_,_,_) in enumerate(rows)}, rows
PER={k:build(v) for k,v in DATA.items()}

def seasonal_long(rows, idx_of, d):
    # long on the day held: Tue-close->Wed-close => long Wed; Fri-close->Mon-close => long Mon
    i=idx_of.get(d)
    if i is None or i==0: return False
    prev=rows[i-1][0]
    return prev.weekday()==1 or prev.weekday()==4   # Mon=0..: prev Tue(1)->hold Wed ; prev Fri(4)->hold Mon

def tom_long(idxmap, d, lastn=3, firstn=3):
    v=idxmap.get(d)
    if not v: return False
    j,from_end=v
    return (from_end < lastn) or (j < firstn)

def run(cap):
    daily=defaultdict(float); legcount=defaultdict(int)
    for k,(ret,idxmap,idx_of,rows) in PER.items():
        for d in ALLDAYS:
            if d not in ret: continue
            legs=0
            if seasonal_long(rows, idx_of, d): legs+=1
            if tom_long(idxmap, d): legs+=1   # TOM leg (overlap = both, counted as up to 2 legs/idx)
            for _ in range(legs):
                # register an intended long leg for this index/day
                legcount[d]+=1
    # second pass: apply cap per day (keep up to `cap` net-long legs), equal unit risk
    book=[]; eq=0; pk=0; mdd=0; tot=0; ndays_active=0; legdays=0; capped_legdays=0
    for d in ALLDAYS:
        intended=[]  # (idx, ret) for each active long leg
        for k,(ret,idxmap,idx_of,rows) in PER.items():
            if d not in ret: continue
            n=0
            if seasonal_long(rows, idx_of, d): n+=1
            if tom_long(idxmap, d): n+=1
            for _ in range(n): intended.append(ret[d])
        legdays+=len(intended)
        active=intended if cap is None else intended[:cap]
        capped_legdays+=len(active)
        if active:
            ndays_active+=1
            pnl=sum(active)        # fixed 1-unit risk per leg
            tot+=pnl; eq+=pnl; pk=max(pk,eq); mdd=max(mdd,pk-eq)
    return dict(tot=tot*100, mdd=mdd*100, legdays=legdays, capped=capped_legdays,
                active_days=ndays_active)

print("IndexBookBudget cap validation — seasonal(Tue/Fri) + TOM(last3/first3) long book, 5 idx, 2016-26")
print("per-leg fixed 1-unit risk; book PnL/DD in return-units (%)\n")
print(f"{'cap (net-long legs)':<22}{'totRet%':>9}{'maxDD%':>9}{'ret/DD':>8}{'leg-days kept':>15}")
base=None
for cap in [None, 10, 8, 6, 4, 3, 2]:
    r=run(cap)
    rd=r['tot']/r['mdd'] if r['mdd']>0 else 0
    keep=f"{r['capped']}/{r['legdays']} ({100*r['capped']/r['legdays']:.0f}%)"
    label="UNCAPPED" if cap is None else f"cap={cap}"
    print(f"{label:<22}{r['tot']:>9.1f}{r['mdd']:>9.2f}{rd:>8.2f}{keep:>15}")

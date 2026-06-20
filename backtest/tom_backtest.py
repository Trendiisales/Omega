#!/usr/bin/env python3
# Turn-of-Month (TOM) seasonality on the index basket — a flows/liquidity quant staple
# NOT in the book (we only have day-of-week IndexSeasonal). Long index over the TOM window
# (last LASTN trading days of month + first FIRSTN of next), flat otherwise. Faithful daily:
# enter at close of the day BEFORE the window opens -> fill next OPEN; exit at OPEN after the
# window. 2bp RT cost. Per-index + equal-weight book; split 2022 bear vs bull. cross-regime check.
import datetime
from collections import defaultdict
TICK="/Users/jo/Tick"
F={"SPX":"SPX_daily_2016_2026.csv","NDX":"NDX_daily_2016_2026.csv","DJ30":"DJ30_daily_2016_2026.csv",
   "GER40":"GER40_daily_2016_2026.csv","UK100":"UK100_daily_2016_2026.csv"}
COST=2.0/1e4
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

def stats(tr):
    if not tr: return None
    rs=[r for _,r in tr]; w=[r for r in rs if r>0]; l=[-r for r in rs if r<0]
    gp=sum(w); gl=sum(l); eq=0;pk=0;mdd=0
    for r in rs: eq+=r;pk=max(pk,eq);mdd=max(mdd,pk-eq)
    return dict(n=len(rs),wr=len(w)/len(rs),pf=gp/gl if gl>0 else 9.9,tot=sum(rs),mdd=mdd)

def tom_trades(rows, lastn, firstn):
    # mark each row's trading-day-of-month index (1..) and days-from-month-end
    n=len(rows); months=defaultdict(list)
    for i,(d,o,h,l,c) in enumerate(rows): months[(d.year,d.month)].append(i)
    in_win=[False]*n
    keys=sorted(months)
    for mi,mk in enumerate(keys):
        idxs=months[mk]
        # last LASTN of this month
        for i in idxs[-lastn:]: in_win[i]=True
        # first FIRSTN of NEXT month
        if mi+1<len(keys):
            for i in months[keys[mi+1]][:firstn]: in_win[i]=True
    # build contiguous windows -> one trade each: enter open of first in-win day, exit open after last
    trades=[]; i=1
    while i<n-1:
        if in_win[i] and not in_win[i-1]:
            j=i
            while j<n and in_win[j]: j+=1
            ei=i; xi=min(j, n-1)               # enter open[i], exit open[j] (first out-day)
            if xi>ei:
                a=rows[ei][1]; b=rows[xi][1]    # open-to-open (faithful, no same-bar)
                if a>0:
                    trades.append((rows[ei][0], (b/a-1.0)-COST))
            i=j
        else: i+=1
    return trades

def yr_split(tr):
    bull=[t for t in tr if t[0].year!=2022]; bear=[t for t in tr if t[0].year==2022]
    return bull,bear
def fmt(s): return "  --" if not s else f"n={s['n']:4d} WR{s['wr']*100:3.0f}% PF{s['pf']:4.2f} tot{s['tot']*100:+6.1f}% DD{s['mdd']*100:4.1f}%"

for lastn,firstn in [(1,3),(2,3),(1,4),(3,3)]:
    print(f"\n===== TOM window: last {lastn} + first {firstn} trading days =====")
    book=[]
    for s in DATA:
        tr=tom_trades(DATA[s],lastn,firstn); book+=tr
        a=stats(tr); h=len(tr)//2; s1=stats(tr[:h]); s2=stats(tr[h:]); bull,bear=yr_split(tr)
        ok = s1 and s2 and stats(bull) and stats(bear) and s1['pf']>1 and s2['pf']>1 and stats(bull)['pf']>1 and stats(bear)['pf']>1
        print(f"  {s:6s} {fmt(a)}  H1pf{(s1 or {}).get('pf',0):.2f} H2pf{(s2 or {}).get('pf',0):.2f} BULLpf{(stats(bull) or {}).get('pf',0):.2f} BEARpf{(stats(bear) or {}).get('pf',0):.2f} {'PASS' if ok else ''}")
    book.sort()
    a=stats(book); h=len(book)//2; s1=stats(book[:h]); s2=stats(book[h:]); bull,bear=yr_split(book)
    # trades/week over span
    span_days=(book[-1][0]-book[0][0]).days if len(book)>1 else 1
    tpw=len(book)/(span_days/7.0) if span_days>0 else 0
    print(f"  BOOK   {fmt(a)}  {tpw:.1f} t/wk  H1pf{s1['pf']:.2f} H2pf{s2['pf']:.2f} BULLpf{stats(bull)['pf']:.2f} BEARpf{stats(bear)['pf']:.2f}")

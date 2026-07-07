#!/usr/bin/env python3
# edge_screen.py — quick faithful-lite screens for 4 untapped-edge candidates.
# Stats: n, net bp, avg, PF, WR, both-halves, 2022 bear, 2020 crash, maxDD. Costs per test noted.
import csv, datetime, statistics

IDX = {
 "SPX":"/Users/jo/Tick/SPX_daily_2016_2026.csv",
 "NDX":"/Users/jo/Tick/NDX_daily_2016_2026.csv",
 "DJ30":"/Users/jo/Tick/DJ30_daily_2016_2026.csv",
 "GER40":"/Users/jo/Tick/GER40_daily_2016_2026.csv",
 "UK100":"/Users/jo/Tick/UK100_daily_2016_2026.csv",
}
def load_epoch(p):
    out=[]
    for r in csv.reader(open(p)):
        if len(r)<5: continue
        try: t=int(r[0]); o,h,l,c=map(float,r[1:5])
        except: continue
        out.append((datetime.datetime.utcfromtimestamp(t).date(),o,h,l,c))
    return out
def load_gc(p):
    out=[]
    for r in csv.reader(open(p)):
        try: d=datetime.date.fromisoformat(r[0]); o,h,l,c=map(float,r[1:5])
        except: continue
        if o<=0: continue
        out.append((d,o,h,l,c))
    return out

class St:
    def __init__(s): s.n=0;s.s=0;s.w=0;s.wp=0;s.lp=0;s.eq=0;s.pk=0;s.dd=0
    def add(s,bp):
        s.n+=1;s.s+=bp
        if bp>0: s.w+=1;s.wp+=bp
        else: s.lp+=bp
        s.eq+=bp;s.pk=max(s.pk,s.eq);s.dd=max(s.dd,s.pk-s.eq)
    def pf(s): return s.wp/-s.lp if s.lp<0 else (99 if s.wp>0 else 0)
    def row(s,tag):
        if s.n==0: return f"{tag:28s} n=0"
        return (f"{tag:28s} n={s.n:4d} net={s.s:7.0f} avg={s.s/s.n:6.1f} PF={s.pf():5.2f} "
                f"WR={100*s.w/s.n:3.0f}%% DD={s.dd:6.0f}")

def run(trades, tag, mid=datetime.date(2021,6,1)):
    # trades: list of (date, bp)
    a=St();bear=St();crash=St();bull=St();h1=St();h2=St()
    for d,bp in trades:
        a.add(bp)
        if d.year==2022: bear.add(bp)
        elif d.year==2020: crash.add(bp)
        else: bull.add(bp)
        (h1 if d<mid else h2).add(bp)
    print(a.row(tag) + f" | bear={bear.s:+6.0f}(PF{bear.pf():.2f} n{bear.n}) crash={crash.s:+5.0f} bull={bull.s:+7.0f} | h1={h1.s:+6.0f} h2={h2.s:+6.0f}")

print("="*110)
print("TEST 0 — overnight gap sanity: mean |open/prevclose-1| bp per symbol (if ~0, 24h-CFD bars -> overnight untestable)")
data={k:load_epoch(p) for k,p in IDX.items()}
for k,v in data.items():
    gaps=[abs(v[i][1]/v[i-1][4]-1)*10000 for i in range(1,len(v)) if v[i-1][4]>0]
    print(f"  {k:6s} mean|gap|={statistics.mean(gaps):6.1f}bp median={statistics.median(gaps):6.1f}bp")

print("="*110)
print("TEST 1 — OVERNIGHT PREMIUM: long prev close -> today open, gate prevclose>SMA50. cost 3bp RT")
COST=3.0
for k,v in data.items():
    closes=[b[4] for b in v]
    sma=[0.0]*len(v); run50=0.0
    for i,c in enumerate(closes):
        run50+=c
        if i>=50: run50-=closes[i-50]
        if i>=49: sma[i]=run50/50
    tr=[]
    for i in range(1,len(v)):
        if sma[i-1]>0 and closes[i-1]>sma[i-1] and closes[i-1]>0:
            tr.append((v[i][0],(v[i][1]/closes[i-1]-1)*10000-COST))
    run(tr,f"overnight {k} gated50")

print("="*110)
print("TEST 2 — OPEX WEEK (3rd-Friday week): long prior-Fri close -> opex-Fri close. cost 3bp RT")
def third_friday(y,m):
    d=datetime.date(y,m,1); fr=0
    while True:
        if d.weekday()==4:
            fr+=1
            if fr==3: return d
        d+=datetime.timedelta(days=1)
for k,v in data.items():
    bydate={b[0]:b[4] for b in v}
    dates=sorted(bydate)
    tr=[]
    y0,y1=dates[0].year,dates[-1].year
    for y in range(y0,y1+1):
        for m in range(1,13):
            opx=third_friday(y,m)
            ent=opx-datetime.timedelta(days=7)
            # snap to nearest available trading dates
            e=[d for d in dates if ent<=d<ent+datetime.timedelta(days=4)]
            x=[d for d in dates if opx-datetime.timedelta(days=2)<d<=opx]
            if not e or not x: continue
            pe,px=bydate[e[0]],bydate[x[-1]]
            if pe>0: tr.append((x[-1],(px/pe-1)*10000-3.0))
    run(tr,f"opexweek {k}")

print("="*110)
print("TEST 3 — 5-INDEX BREADTH GATE: breadth = count(close>SMA20)>=4 -> long NDX next close-to-close. cost 3bp")
# align by date
sets={k:{b[0]:b[4] for b in v} for k,v in data.items()}
common=sorted(set.intersection(*[set(s) for s in sets.values()]))
sma20={k:{} for k in IDX}
for k in IDX:
    cl=[sets[k][d] for d in common]
    r=0.0
    for i,d in enumerate(common):
        r+=cl[i]
        if i>=20: r-=cl[i-20]
        if i>=19: sma20[k][d]=r/20
tr=[]
for i in range(1,len(common)-1):
    d=common[i]; dn=common[i+1]
    if d not in sma20["SPX"]: continue
    br=sum(1 for k in IDX if d in sma20[k] and sets[k][d]>sma20[k][d])
    if br>=4:
        c0,c1=sets["NDX"][d],sets["NDX"][dn]
        tr.append((dn,(c1/c0-1)*10000-3.0))
run(tr,"breadth>=4 long NDX c2c")

print("="*110)
print("TEST 4 — GOLD FOMC DRIFT (GC futures 2019-2026, FOMC table from IndexFomcEngine): cost 1bp RT")
gc=load_gc("/Users/jo/Tick/mid_freq_research/gcf_daily.csv")
fomc=[20190130,20190320,20190501,20190619,20190731,20190918,20191030,20191211,
20200129,20200318,20200429,20200610,20200729,20200916,20201105,20201216,
20210127,20210317,20210428,20210616,20210728,20210922,20211103,20211215,
20220126,20220316,20220504,20220615,20220727,20220921,20221102,20221214,
20230201,20230322,20230503,20230614,20230726,20230920,20231101,20231213,
20240131,20240320,20240501,20240612,20240731,20240918,20241107,20241218,
20250129,20250319,20250507,20250618,20250730,20250917,20251029,20251210,
20260128,20260318,20260429,20260617]
fd={datetime.date(x//10000,x//100%100,x%100) for x in fomc}
dates=[b[0] for b in gc]; cl={b[0]:b[4] for b in gc}
idx={d:i for i,d in enumerate(dates)}
for label,ent_off,ex_off in [("pre-drift: c[-2]->c[fomc]",-2,0),("day-of: c[-1]->c[fomc]",-1,0),("post: c[fomc]->c[+2]",0,2)]:
    tr=[]
    for f in sorted(fd):
        # find fomc trading day (or next)
        dd=f
        while dd not in idx and dd<f+datetime.timedelta(days=5): dd+=datetime.timedelta(days=1)
        if dd not in idx: continue
        i=idx[dd]
        ie,ix=i+ent_off,i+ex_off
        if ie<0 or ix>=len(dates): continue
        pe,px=cl[dates[ie]],cl[dates[ix]]
        if pe>0: tr.append((dates[ix],(px/pe-1)*10000-1.0))
    run(tr,f"goldFOMC {label}")

#!/usr/bin/env python3
# INDEX analog of the BlendBook fix, on real index CFD data 2016-2026 (incl 2018/2020/2022 bears).
# Answers the operator: do the 3 BlendBook lessons reproduce on the INDEX book, and can we build
# the same kind of solution (protected-beta core + uncorrelated MN alpha) for indices?
#
#   KILL-TEST (IndexBearShortEngine): long-bull-basket vs cash-bear vs short-basket-bear vs short-weakest-bear
#   CORE       : vol-targeted long basket, HALF size in bear, NEVER short
#   ALPHA      : validated ALL-WEATHER index relval (bull long-only-mom + neutral mom-L/S + bear/chop MR-L/S),
#                breadth-gated, marked DAILY close-to-close, equal-capital across the 3 sub-engines
#   BLEND      : w*core + (1-w)*alpha ; compare to basket buy-hold
# Faithful: signal at close[t] -> position effective close[t+1] (no lookahead). Cost 2bp RT/leg on rebalance.
import math, datetime
from collections import Counter

TICK="/Users/jo/Tick"
FILES={"SPX":f"{TICK}/SPX_daily_2016_2026.csv","NDX":f"{TICK}/NDX_daily_2016_2026.csv",
       "DJ30":f"{TICK}/DJ30_daily_2016_2026.csv","GER40":f"{TICK}/GER40_daily_2016_2026.csv",
       "UK100":f"{TICK}/UK100_daily_2016_2026.csv"}
COST=2.0/1e4

def load(p):
    rows=[]
    for line in open(p):
        a=line.strip().split(',')
        if len(a)<5: continue
        try: ts=int(float(a[0])); o,h,l,c=map(float,a[1:5])
        except: continue
        if c<=0: continue
        rows.append((datetime.datetime.utcfromtimestamp(ts).date(),o,h,l,c))
    rows.sort(); seen={}
    for r in rows: seen[r[0]]=r
    return [seen[k] for k in sorted(seen)]
DATA={k:load(v) for k,v in FILES.items()}
common=set.intersection(*[set(d for d,*_ in DATA[s]) for s in DATA])
DATES=sorted(common); N=len(DATES)
SYMS=list(DATA.keys())
CL={s:[ {d:c for d,_,_,_,c in DATA[s]}[d] for d in DATES] for s in SYMS}
# daily close-to-close return per sym
RET={s:[0.0]+[CL[s][i]/CL[s][i-1]-1 for i in range(1,N)] for s in SYMS}
def bret(i): return sum(RET[s][i] for s in SYMS)/len(SYMS)   # equal-weight basket daily return
BR=[bret(i) for i in range(N)]

def sma(xs,n,i): return None if i+1<n else sum(xs[i-n+1:i+1])/n
def regime(i):
    if i<205: return "CHOP"
    cnt=0
    for s in SYMS:
        c=CL[s][i]; sm=sma(CL[s],200,i); sm20=sma(CL[s],200,i-20)
        if sm is None or sm20 is None: continue
        if c>sm and sm>sm20: cnt+=1
    if cnt>=4: return "BULL"
    if cnt<=1: return "BEAR"
    return "CHOP"
REG=[regime(i) for i in range(N)]

# basket SMA200 + 5d crash for the simple long/flat/short timing test
BCL=[sum(CL[s][i] for s in SYMS)/len(SYMS) for i in range(N)]
def bsma(i): return sma(BCL,200,i)
def bcrash(i): return i>=5 and (BCL[i]/BCL[i-5]-1)<-0.05

def ann_sh_dd(daily):
    xs=[r for r in daily if r==r]
    if len(xs)<20: return (float('nan'),)*3
    m=sum(xs)/len(xs); sd=math.sqrt(sum((x-m)**2 for x in xs)/(len(xs)-1)) or 1e-12
    eq=1.0;pk=1.0;mdd=0.0
    for r in xs:
        eq*=(1+r); pk=max(pk,eq); mdd=min(mdd,eq/pk-1)
    return (round(m*252,3), round(m/sd*math.sqrt(252),2), round(mdd,3))

def peryear(daily):
    yr={}
    for i in range(1,N):
        y=DATES[i].year; yr.setdefault(y,0.0); yr[y]+=daily[i]
    return yr

# ---- daily L/S sub-engine: rebalance every `hold` days when gated; mark close-to-close ----
def sub_daily(signal_fn, hold, gate):
    out=[0.0]*N
    legs=[]; since=hold
    for i in range(206,N):
        # mark today's return on yesterday's legs (position effective the day after signal)
        if legs:
            r=sum(d*RET[s][i] for s,d in legs)/len(legs)
            out[i]=r
        since+=1
        if since>=hold:
            if gate(i-1):                      # decide at prior close, effective today already counted next loop
                nl=signal_fn(i-1)
                if nl is not None:
                    # cost on turnover (legs changed)
                    changed=len(set((s,d) for s,d in nl) ^ set((s,d) for s,d in legs))
                    if i<N: out[i]-=COST*changed/max(1,len(nl))
                    legs=nl; since=0
            else:
                legs=[]; since=0
    return out

def mom_legs(i,lb,longonly=False):
    sc={s:CL[s][i]/CL[s][i-lb]-1 for s in SYMS if CL[s][i-lb]>0}
    if len(sc)<len(SYMS): return None
    r=sorted(sc,key=lambda s:sc[s])
    if longonly: return [(r[-1],+1)]
    return [(r[-1],+1),(r[0],-1)]
def mr_legs(i,lb):
    sc={s:CL[s][i]/CL[s][i-lb]-1 for s in SYMS if CL[s][i-lb]>0}
    if len(sc)<len(SYMS): return None
    r=sorted(sc,key=lambda s:sc[s])
    return [(r[0],+1),(r[-1],-1)]

g_bull=lambda i:REG[i]=="BULL"; g_notchop=lambda i:REG[i]!="CHOP"; g_nonbull=lambda i:REG[i]!="BULL"
e1=sub_daily(lambda i:mom_legs(i,120,longonly=True),20,g_bull)      # BULL long-only momentum
e2=sub_daily(lambda i:mom_legs(i,120),20,g_notchop)                 # NEUTRAL mom L/S
e3=sub_daily(lambda i:mr_legs(i,3),3,g_nonbull)                     # BEAR/CHOP MR L/S
ALPHA=[ (e1[i]+e2[i]+e3[i])/3.0 for i in range(N)]                  # equal-capital across 3 sub-engines

# ---- protected-beta CORE: vol-target long basket, half in bear, never short ----
def vol20(i):
    if i<21: return None
    xs=BR[i-19:i+1]; m=sum(xs)/20
    return math.sqrt(sum((x-m)**2 for x in xs)/19)
CORE=[0.0]*N; prev_lv=0.0
for i in range(206,N):
    v=vol20(i-1); sm=bsma(i-1)
    if v is None or v<=0 or sm is None: continue
    bull = BCL[i-1]>=sm and not bcrash(i-1)
    lv=min(0.10/math.sqrt(252)/v,2.0)*(1.0 if bull else 0.5)
    CORE[i]=lv*BR[i]-abs(lv-prev_lv)*COST; prev_lv=lv

# ---- KILL-TEST: the IndexBearShortEngine structure ----
BH=BR[:]                                              # buy & hold basket
LF=[0.0]*N; TM=[0.0]*N; TW=[0.0]*N
for i in range(206,N):
    sm=bsma(i-1)
    if sm is None: continue
    bull=BCL[i-1]>=sm and not bcrash(i-1)
    LF[i]=BR[i] if bull else 0.0                      # long-bull / CASH-bear
    TM[i]=BR[i] if bull else (-BR[i]-COST)            # long-bull / SHORT-BASKET-bear  (= IndexBearShort idx-short)
    if bull: TW[i]=BR[i]
    else:                                             # long-bull / SHORT-WEAKEST-bear (= IndexBearShort xshort)
        sc={s:CL[s][i-1]/CL[s][i-61]-1 for s in SYMS if i>=61 and CL[s][i-61]>0}
        if len(sc)==len(SYMS):
            weak=min(sc,key=lambda s:sc[s]); TW[i]=-RET[weak][i]-COST
        else: TW[i]=0.0

def line(nm,d):
    a,sh,dd=ann_sh_dd(d); print(f"{nm:<30} ann {a!s:>6}  Sharpe {sh!s:>5}  maxDD {dd!s:>6}")
print(f"# index panel {N}d {DATES[0]}..{DATES[-1]}  regimes {dict(Counter(REG))}\n")
print("===== KILL-TEST: IndexBearShortEngine structure (does shorting the bear help?) =====")
for nm,d in [("buy_hold basket",BH),("long-bull / CASH-bear",LF),
             ("long-bull / SHORT-basket-bear",TM),("long-bull / SHORT-weakest-bear",TW)]:
    line(nm,d)
print("\n===== INDEX BLENDBOOK: protected-beta core + index-relval alpha =====")
for nm,d in [("buy_hold basket (beta)",BH),("CORE (vol-tgt, half-bear, no short)",CORE),
             ("ALPHA (ALL-WEATHER relval, MN)",ALPHA),
             ("  e1 bull long-mom",e1),("  e2 neutral mom L/S",e2),("  e3 bear/chop MR L/S",e3)]:
    line(nm,d)

# correlations
def corr(a,b):
    idx=[i for i in range(N) if a[i]==a[i] and b[i]==b[i] and (a[i]!=0 or b[i]!=0)]
    if len(idx)<20: return float('nan')
    xa=[a[i] for i in idx]; xb=[b[i] for i in idx]
    ma=sum(xa)/len(xa); mb=sum(xb)/len(xb)
    cov=sum((xa[k]-ma)*(xb[k]-mb) for k in range(len(idx)))
    va=sum((x-ma)**2 for x in xa); vb=sum((x-mb)**2 for x in xb)
    return cov/math.sqrt(va*vb) if va>0 and vb>0 else float('nan')
print(f"\nCORR  alpha<->core {corr(ALPHA,CORE):+.2f}   alpha<->buyhold {corr(ALPHA,BH):+.2f}")

# PURE market-neutral alpha: drop the beta-carrying e1 (bull long-only mom), keep e2+e3 only
ALPHA_MN=[(e2[i]+e3[i])/2.0 for i in range(N)]
# cash-bear core: long-bull / flat-bear (the simplest kill-test winner) as an alternate beta core
CORE_CB=LF[:]
print("\n===== PURE-MN ALPHA (drop beta leg e1) + alt cores =====")
line("ALPHA_MN (e2+e3, pure MN)",ALPHA_MN)
line("CORE_CB (long-bull/cash-bear)",CORE_CB)
print(f"CORR  alpha_mn<->core_voltgt {corr(ALPHA_MN,CORE):+.2f}   alpha_mn<->core_cb {corr(ALPHA_MN,CORE_CB):+.2f}   alpha_mn<->buyhold {corr(ALPHA_MN,BH):+.2f}")

bh_sh=ann_sh_dd(BH)[1]
print("\n===== BLEND w*core + (1-w)*alpha vs basket buy-hold (B&H Sharpe %.2f) =====" % bh_sh)
best=None
combos=[("voltgt+allweather",CORE,ALPHA),("voltgt+pureMN",CORE,ALPHA_MN),
        ("cashbear+pureMN",CORE_CB,ALPHA_MN),("cashbear+allweather",CORE_CB,ALPHA)]
for cname,C,A in combos:
    print(f"-- {cname} --")
    for w in (0.3,0.4,0.5,0.6,0.7):
        bl=[w*C[i]+(1-w)*A[i] for i in range(N)]
        a,sh,dd=ann_sh_dd(bl)
        flag="  <-- beats B&H Sharpe" if sh>bh_sh else ""
        print(f"   w={w:<4} ann {a!s:>6}  Sharpe {sh!s:>5}  maxDD {dd!s:>6}{flag}")
        if best is None or sh>best[1]: best=(f"{cname} w={w}",sh,bl)
print(f"\nBEST blend: {best[0]}  Sharpe {best[1]}")
best=(best[0],best[1],best[2])

print("\n===== PER-YEAR ann return =====")
yrs=sorted({d.year for d in DATES})
print(f"{'':<26}"+" ".join(f"{y:>6}" for y in yrs))
for nm,d in [("buy_hold",BH),("CORE_CB cash-bear",CORE_CB),("ALPHA_MN pure",ALPHA_MN),
             ("SHORT-bear (kill-test)",TM),(f"BEST {best[0]}",best[2])]:
    py=peryear(d)
    print(f"{nm:<26}"+" ".join(f"{py.get(y,0)*100:>5.0f}%" for y in yrs))

#!/usr/bin/env python3
# Cross-sectional (relative-value) edges across the index basket -- GAP not currently traded by Omega.
# Omega trades each index leg DIRECTIONALLY (turtle/MR/seasonal) but never RANKS them cross-sectionally.
# Three classic quant staples mined here, faithfully:
#   1. XS Momentum   : rank by trailing return; long winners / short losers (Jegadeesh-Titman / AQR)
#   2. XS Mean-Rev   : rank by SHORT trailing return; long losers / short winners (weekly reversal)
#   3. Coint spread  : log-ratio z-score pairs stat-arb (Engle-Granger style, rolling)
# Faithful: signal on close of day t -> fill at NEXT day's OPEN. Cost ~1bp/side index (2bp RT).
# Walk-forward halves + bull/bear regime split reported for every variant.
import csv, math, datetime, itertools
from collections import defaultdict

TICK="/Users/jo/Tick"
FILES={
  "SPX":f"{TICK}/SPX_daily_2016_2026.csv",
  "NDX":f"{TICK}/NDX_daily_2016_2026.csv",
  "DJ30":f"{TICK}/DJ30_daily_2016_2026.csv",
  "GER40":f"{TICK}/GER40_daily_2016_2026.csv",
  "UK100":f"{TICK}/UK100_daily_2016_2026.csv",
}
COST_BPS_RT=2.0          # 1bp/side round trip on a CFD index
SLIP_BPS=0.0

def load(path):
    rows=[]
    with open(path) as f:
        for line in f:
            p=line.strip().split(',')
            if len(p)<5: continue
            try:
                ts=int(float(p[0])); o,h,l,c=map(float,p[1:5])
            except: continue
            if c<=0: continue
            d=datetime.datetime.utcfromtimestamp(ts).date()
            rows.append((d,o,h,l,c))
    rows.sort(key=lambda r:r[0])
    seen={}
    for r in rows: seen[r[0]]=r
    return [seen[k] for k in sorted(seen)]

DATA={k:load(v) for k,v in FILES.items()}
# build common date axis (intersection of all symbols -> aligned panel)
common=set.intersection(*[set(d for d,*_ in DATA[s]) for s in DATA])
DATES=sorted(common)
# panel[sym] = list of (o,h,l,c) aligned to DATES
PANEL={}
for s in DATA:
    m={d:(o,h,l,c) for d,o,h,l,c in DATA[s]}
    PANEL[s]=[m[d] for d in DATES]
SYMS=list(DATA.keys())
N=len(DATES)
print(f"# panel: {N} aligned days {DATES[0]}..{DATES[-1]} x {len(SYMS)} syms {SYMS}")

# S-2026-06-23: proper risk-off bear definition = SPX below its 200d SMA (captures
# 2018-Q4 + 2020 crash + 2022, not just the 2022 calendar year) -> a MEANINGFUL bear
# sample instead of n=4-5. Precompute the bear-date set from the SPX panel.
_spx_close = {DATES[i]: PANEL["SPX"][i][3] for i in range(N)}
_bear_dates = set()
_sc = [PANEL["SPX"][i][3] for i in range(N)]
for i in range(N):
    if i >= 200:
        sma200 = sum(_sc[i-200:i]) / 200.0
        if _sc[i] < sma200:
            _bear_dates.add(DATES[i])
print(f"# bear days (SPX<200dMA): {len(_bear_dates)}/{N} ({100*len(_bear_dates)/N:.0f}%)")
def regime_of(date):
    return "BEAR" if date in _bear_dates else "BULL"

def stats(trades):
    # trades: list of (date, ret_frac)
    if not trades: return None
    rs=[r for _,r in trades]
    wins=[r for r in rs if r>0]; losses=[-r for r in rs if r<0]
    gp=sum(wins); gl=sum(losses)
    pf=gp/gl if gl>0 else float('inf')
    wr=len(wins)/len(rs)
    tot=sum(rs)
    # equity curve maxDD in return units
    eq=0.0; peak=0.0; mdd=0.0
    for r in rs:
        eq+=r; peak=max(peak,eq); mdd=max(mdd,peak-eq)
    sharpe=(sum(rs)/len(rs))/(stdev(rs)+1e-12)*math.sqrt(252/ (max(1,(trades[-1][0]-trades[0][0]).days)/len(rs)) ) if len(rs)>2 else 0
    return dict(n=len(rs),wr=wr,pf=pf,tot=tot,mdd=mdd,retdd=(tot/mdd if mdd>0 else float('inf')))

def stdev(xs):
    if len(xs)<2: return 0.0
    m=sum(xs)/len(xs); return math.sqrt(sum((x-m)**2 for x in xs)/(len(xs)-1))

def report(name, trades):
    if not trades:
        print(f"{name:42s}  NO TRADES"); return
    half=len(trades)//2
    h1=trades[:half]; h2=trades[half:]
    bull=[t for t in trades if regime_of(t[0])=="BULL"]
    bear=[t for t in trades if regime_of(t[0])=="BEAR"]
    bear_old=[t for t in bear if t[0].year<2025]
    bear_recent=[t for t in bear if t[0].year>=2025]   # 2025 spring + 2026 Q1 -- OOS bears
    a=stats(trades);
    def fmt(s):
        if not s: return " n=0"
        return f"n={s['n']:4d} WR{s['wr']*100:4.0f}% PF{s['pf']:5.2f} tot{s['tot']*100:+7.1f}% DD{s['mdd']*100:5.1f}% r/dd{s['retdd']:5.1f}"
    s1=stats(h1); s2=stats(h2); sb=stats(bull); sr=stats(bear)
    print(f"{name:42s} {fmt(a)}")
    print(f"{'':42s}  H1[{fmt(s1)}]")
    print(f"{'':42s}  H2[{fmt(s2)}]")
    print(f"{'':42s}  BULL[{fmt(sb)}]")
    print(f"{'':42s}  BEAR[{fmt(sr)}]")
    print(f"{'':42s}  BEAR_old(<=24)[{fmt(stats(bear_old))}]")
    print(f"{'':42s}  BEAR_recent(25-26 OOS)[{fmt(stats(bear_recent))}]")
    # PASS gate: both halves PF>1 AND both regimes PF>1 (faithful BACKTEST_TRUTH)
    ok = all(x and x['pf']>1.0 and x['tot']>0 for x in [s1,s2,sb,sr])
    print(f"{'':42s}  >>> {'PASS both-halves+both-regimes' if ok else 'FAIL (not robust)'}")
    return ok

# ---------- 1. CROSS-SECTIONAL MOMENTUM / MEAN-REV ----------
def xs_rank(lookback=60, hold=20, topk=1, side='mom', longonly=False, skip=0):
    # at close of day i: score = trailing return over [i-lookback-skip, i-skip]
    # mom: long highest score, short lowest. meanrev: invert.
    closes={s:[b[3] for b in PANEL[s]] for s in SYMS}
    opens ={s:[b[0] for b in PANEL[s]] for s in SYMS}
    trades=[]
    i=lookback+skip+1
    while i < N-hold-1:
        scores={}
        for s in SYMS:
            c0=closes[s][i-lookback-skip]; c1=closes[s][i-skip]
            if c0>0: scores[s]=c1/c0-1.0
        if len(scores)<len(SYMS): i+=1; continue
        ranked=sorted(scores, key=lambda s:scores[s])
        if side=='mom':
            longs=ranked[-topk:]; shorts=ranked[:topk]
        else:
            longs=ranked[:topk]; shorts=ranked[-topk:]
        legs=[(s,+1) for s in longs]
        if not longonly: legs+=[(s,-1) for s in shorts]
        # enter NEXT open (i+1), exit open at i+1+hold
        ei=i+1; xi=i+1+hold
        if xi>=N: break
        pnl=0.0; nlegs=0
        for s,dir in legs:
            ein=opens[s][ei]; exo=opens[s][xi]
            if ein<=0: continue
            r=dir*(exo/ein-1.0) - (COST_BPS_RT+SLIP_BPS)/1e4
            pnl+=r; nlegs+=1
        if nlegs>0:
            trades.append((DATES[ei], pnl/nlegs))
        i+=hold  # non-overlapping
    return trades

# ---------- 2. COINTEGRATION SPREAD (rolling z of log-ratio) ----------
def coint_pair(a,b, win=60, z_in=2.0, z_out=0.5, maxhold=30):
    ca=[x[3] for x in PANEL[a]]; cb=[x[3] for x in PANEL[b]]
    oa=[x[0] for x in PANEL[a]]; ob=[x[0] for x in PANEL[b]]
    trades=[]; pos=None  # (dir, entry_idx)
    i=win+1
    while i<N-1:
        seg=[math.log(ca[k]/cb[k]) for k in range(i-win+1,i+1)]
        m=sum(seg)/win; sd=stdev(seg)
        if sd<=0: i+=1; continue
        z=(seg[-1]-m)/sd
        if pos is None:
            if z> z_in: pos=(-1,i)   # spread high -> short A long B
            elif z<-z_in: pos=(+1,i) # spread low  -> long A short B
        else:
            dir,ent=pos
            held=i-ent
            if abs(z)<z_out or held>=maxhold:
                # exit at next open; entry was at open after signal day ent
                ei=ent+1; xi=i+1
                if xi<N:
                    # leg A dir, leg B -dir (dollar-neutral approx, equal weight)
                    ra=dir*(oa[xi]/oa[ei]-1.0)
                    rb=-dir*(ob[xi]/ob[ei]-1.0)
                    r=(ra+rb)/2 - 2*(COST_BPS_RT)/1e4
                    trades.append((DATES[ei], r))
                pos=None
        i+=1
    return trades

print("\n================= 1. CROSS-SECTIONAL MOMENTUM =================")
best=[]
for lb in (20,60,120,250):
    for hold in (5,10,20):
        ok=report(f"XS-MOM lb{lb} hold{hold} top1 L/S", xs_rank(lb,hold,1,'mom'))
        if ok: best.append(('XS-MOM',lb,hold))
print("\n--- long-only top1 momentum ---")
for lb in (60,120,250):
    report(f"XS-MOM lb{lb} hold20 top1 LONG-ONLY", xs_rank(lb,20,1,'mom',longonly=True))

print("\n================= 2. CROSS-SECTIONAL MEAN-REVERSION =================")
for lb in (3,5,10,20):
    for hold in (3,5,10,20):
        ok=report(f"XS-MR lb{lb} hold{hold} top1 L/S", xs_rank(lb,hold,1,'meanrev'))
        if ok: best.append(('XS-MR',lb,hold))

print("\n================= 3. COINTEGRATION SPREAD (pairs) =================")
pairs=list(itertools.combinations(SYMS,2))
for a,b in pairs:
    for win in (40,60,120):
        ok=report(f"COINT {a}/{b} win{win} z2.0", coint_pair(a,b,win,2.0,0.5,30))
        if ok: best.append(('COINT',a,b,win))

print("\n================= SURVIVORS (both-halves + both-regimes PF>1) =================")
for b in best: print("  ", b)

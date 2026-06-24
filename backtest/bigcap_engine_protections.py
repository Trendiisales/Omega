#!/usr/bin/env python3
# (S-2026-06-24) Two faithful-er daily cross-sectional tests on 527 S&P names
# (data/rdagent/sp500_long_close.csv, 2019-06..2026-06, real fixed-capital equity):
#   #1 TURTLE trend-long (Donchian55 break -> Donchian20-low exit) x regime gate modes
#   #2 cross-sectional MEAN-REVERSION (RSI2<lo buy -> SMA5/RSI2-hi/time exit) x gate modes
# Tests whether the not-BEAR gate relaxation helps a real index-trend engine (#1) and
# what gate (if any) a cross-sectional equity-MR engine wants (#2). Daily proxy; exits
# are engine-native here (turtle Donchian, MR RSI2) not the BigCapMomo ATR-trail.
import csv, math

F="data/rdagent/sp500_long_close.csv"
CAP=100_000.0; RISK=0.05; COST=0.0020

def load(p):
    with open(p) as fh:
        r=csv.reader(fh); hdr=next(r); rows=[x for x in r]
    dates=[x[0] for x in rows]; names=hdr[1:]
    C=[[ (float(v) if v not in('','nan','NaN') else None) for v in row[1:]] for row in rows]
    return dates,names,C

def regimes(C):
    T=len(C);N=len(C[0]);base=[None]*N
    for i in range(N):
        for t in range(T):
            if C[t][i] is not None: base[i]=C[t][i];break
    idx=[]
    for t in range(T):
        v=[C[t][i]/base[i] for i in range(N) if C[t][i] is not None and base[i]]
        idx.append(sum(v)/len(v) if v else (idx[-1] if idx else 1.0))
    reg=[]
    for t in range(T):
        if t<200: reg.append("WARMUP");continue
        sma=sum(idx[t-199:t+1])/200; smap=sum(idx[t-219:t-19])/200 if t>=219 else sma
        c=idx[t]
        reg.append("BULL" if (c>sma and sma>smap) else "BEAR" if (c<sma and sma<smap) else "NEUTRAL")
    return reg

def gate_ok(mode,reg_t):
    if mode=="none": return True
    if mode=="bull": return reg_t=="BULL"
    if mode=="notbear": return reg_t!="BEAR"
    return True

def equity_stats(eq):
    pk=eq[0];mdd=0
    for e in eq:
        pk=max(pk,e);mdd=max(mdd,(pk-e)/pk)
    yrs=len(eq)/252; tot=eq[-1]/CAP-1
    cagr=(eq[-1]/CAP)**(1/yrs)-1 if yrs>0 and eq[-1]>0 else -1
    rs=[eq[k]/eq[k-1]-1 for k in range(1,len(eq))]
    mu=sum(rs)/len(rs); sd=(sum((r-mu)**2 for r in rs)/len(rs))**.5
    sh=mu/sd*math.sqrt(252) if sd>0 else 0
    return dict(tot=tot*100,cagr=cagr*100,mdd=mdd*100,sharpe=sh,calmar=(cagr/mdd if mdd>0 else 0))

# ---------- #1 TURTLE (Donchian55 breakout long, Donchian20-low exit) ----------
def turtle(dates,names,C,reg,mode,N_IN=55,N_OUT=20,MAXOPEN=20):
    T=len(C);N=len(C[0]);cash=CAP;pos={};eq=[]; nt=0; by={r:[0.0,0] for r in("BULL","NEUTRAL","BEAR")}
    for t in range(1,T):
        for i in list(pos.keys()):
            c=C[t][i]
            if c is None: continue
            lo=max(0,t-N_OUT+1); w=[C[k][i] for k in range(lo,t+1) if C[k][i] is not None]
            if w and c<=min(w[:-1] or w):                      # Donchian-low exit
                cash+=pos[i]["sh"]*c*(1-COST)
                r=c/pos[i]["entry"]-1; er=pos[i]["reg"]; by[er][0]+=r; by[er][1]+=1; del pos[i]
        if reg[t]!="WARMUP" and gate_ok(mode,reg[t]):
            for i in range(N):
                if i in pos or len(pos)>=MAXOPEN: continue
                c=C[t][i]
                if c is None: continue
                lo=max(0,t-N_IN+1); w=[C[k][i] for k in range(lo,t+1) if C[k][i] is not None]
                if len(w)<N_IN//2 or c<max(w[:-1] or w): continue   # 55d high breakout
                d=CAP*RISK
                if cash<d: break
                cash-=d*(1+COST); pos[i]=dict(entry=c,sh=d/c,reg=reg[t]); nt+=1
        mtm=cash+sum(p["sh"]*C[t][i] for i,p in pos.items() if C[t][i] is not None); eq.append(mtm)
    for i,p in pos.items():
        if C[-1][i] is not None: cash+=p["sh"]*C[-1][i]*(1-COST)
    s=equity_stats(eq); s["nt"]=nt; s["by"]=by; return s

# ---------- #2 cross-sectional MR (RSI2<lo buy, SMA5/RSI2-hi/time exit) ----------
def rsi2(series):
    if len(series)<3: return 50.0
    g=l=0.0
    for k in range(1,len(series)):
        d=series[k]-series[k-1]
        if d>0:g+=d
        else:l-=d
    g/=(len(series)-1);l/=(len(series)-1)
    rs=g/l if l>0 else 999
    return 100-100/(1+rs)
def mr(dates,names,C,reg,mode,LO=5,HI=70,TIME=5,STOP=0.08,MAXOPEN=20,LOWN=10,PERDAY=6):
    T=len(C);N=len(C[0]);cash=CAP;pos={};eq=[];nt=0; by={r:[0.0,0] for r in("BULL","NEUTRAL","BEAR")}
    for t in range(3,T):
        for i in list(pos.keys()):
            c=C[t][i]
            if c is None: continue
            p=pos[i];p["held"]+=1
            sma5=[C[k][i] for k in range(max(0,t-4),t+1) if C[k][i] is not None]
            sma5=sum(sma5)/len(sma5) if sma5 else c
            r2=rsi2([C[k][i] for k in range(max(0,t-2),t+1) if C[k][i] is not None])
            ex=None
            if c<=p["entry"]*(1-STOP): ex=c
            elif c>sma5 or r2>HI: ex=c                          # MR target
            elif p["held"]>=TIME: ex=c
            if ex is not None:
                cash+=p["sh"]*ex*(1-COST); rr=ex/p["entry"]-1; er=p["reg"]; by[er][0]+=rr; by[er][1]+=1; del pos[i]
        if reg[t]!="WARMUP" and gate_ok(mode,reg[t]):
            added=0
            for i in range(N):
                if i in pos or len(pos)>=MAXOPEN or added>=PERDAY: continue   # cap entries/day = selectivity
                seg=[C[k][i] for k in range(max(0,t-2),t+1) if C[k][i] is not None]
                if len(seg)<3: continue
                if rsi2(seg)>=LO: continue                      # deeply oversold (RSI2<5)
                lo=max(0,t-LOWN+1); w=[C[k][i] for k in range(lo,t+1) if C[k][i] is not None]
                if not w or C[t][i]>min(w): continue            # AND at a new 10d low (extremity)
                d=CAP*RISK
                if cash<d: break
                cash-=d*(1+COST); pos[i]=dict(entry=C[t][i],sh=d/C[t][i],held=0,reg=reg[t]); nt+=1; added+=1
        mtm=cash+sum(p["sh"]*C[t][i] for i,p in pos.items() if C[t][i] is not None); eq.append(mtm)
    for i,p in pos.items():
        if C[-1][i] is not None: cash+=p["sh"]*C[-1][i]*(1-COST)
    s=equity_stats(eq); s["nt"]=nt; s["by"]=by; return s

def line(lbl,s):
    by=s.get("by",{})
    bstr=" ".join(f"{r[0]}:{(by[r[0]][0]*100):+.0f}%/n{by[r[0]][1]}" for r in [("BULL",),("NEUTRAL",),("BEAR",)])
    print(f"  {lbl:<26} tot={s['tot']:+7.1f}% CAGR={s['cagr']:+5.1f}% maxDD={s['mdd']:4.1f}% "
          f"Sh={s['sharpe']:4.2f} Cal={s['calmar']:4.2f} nt={s['nt']:<4} [{bstr}]")

if __name__=="__main__":
    dates,names,C=load(F); reg=regimes(C)
    print(f"data {len(names)} names {dates[0]}..{dates[-1]}  ($100k fixed-capital)\n")
    print("#1 TURTLE trend-long (Donchian55/20) -- regime-gate modes (per-regime = REALIZED trade-ret sum):")
    for m in ("none","bull","notbear"): line(f"gate={m}", turtle(dates,names,C,reg,m))
    print("\n#2 cross-sectional MEAN-REVERSION (RSI2<10) -- regime-gate modes:")
    for m in ("none","bull","notbear"): line(f"gate={m}", mr(dates,names,C,reg,m))

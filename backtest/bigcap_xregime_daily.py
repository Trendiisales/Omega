#!/usr/bin/env python3
# Tier-1 cross-regime test for the BigCapMomo thesis + protection-transfer study.
# (S-2026-06-24). Daily PROXY of the thesis across REAL bears (2020-covid + 2022).
# Validates the gate/breadth/exit chassis, NOT 5m microstructure (that's Tier 2).
#
# v2: real fixed-capital equity accounting (real maxDD/CAGR), gate modes, protection
# ablation, and a cross-engine transfer test (does the same chassis help mean-reversion?).
import csv, math

F = "data/rdagent/sp500_long_close.csv"
CAP        = 100_000.0
RISK_FRAC  = 0.05     # each position = 5% of capital (20 concurrent = fully invested)
GATE       = 0.03     # momo: up-day return to ignite
DROP       = -0.05    # mr  : down-day return to buy weakness
NEWHIGH_N  = 20
BREADTH    = 2
ATR_N      = 14
ATR_MULT   = 5.0
BE_ARM     = 0.02
BE_FLOOR   = 0.01
MAXHOLD    = 10
COST       = 0.0020   # per side

def load(path):
    with open(path) as fh:
        r = csv.reader(fh); hdr = next(r); rows = [row for row in r]
    dates = [row[0] for row in rows]; names = hdr[1:]
    C = []
    for row in rows:
        rec = []
        for x in row[1:]:
            try: rec.append(float(x))
            except: rec.append(None)
        C.append(rec)
    return dates, names, C

def market_regime(C):
    T=len(C); N=len(C[0]); base=[None]*N
    for i in range(N):
        for t in range(T):
            if C[t][i] is not None: base[i]=C[t][i]; break
    idx=[]
    for t in range(T):
        vals=[C[t][i]/base[i] for i in range(N) if C[t][i] is not None and base[i]]
        idx.append(sum(vals)/len(vals) if vals else (idx[-1] if idx else 1.0))
    reg=[]
    for t in range(T):
        if t<200: reg.append("WARMUP"); continue
        sma=sum(idx[t-199:t+1])/200.0
        smap=sum(idx[t-219:t-19])/200.0 if t>=219 else sma
        c=idx[t]
        if   c>sma and sma>smap: reg.append("BULL")
        elif c<sma and sma<smap: reg.append("BEAR")
        else: reg.append("NEUTRAL")
    return reg

def run(dates, names, C, reg, *, gate="bull", side="momo",
        breadth=True, trail=True, be=True, skip_if_profit=True):
    T=len(C); N=len(C[0])
    cash=CAP; pos={}; eq_curve=[]
    def gate_ok(t):
        if gate=="none": return True
        if gate=="bull": return reg[t]=="BULL"
        if gate=="notbear": return reg[t]!="BEAR"
        return True
    for t in range(1, T):
        # manage
        for i in list(pos.keys()):
            c=C[t][i]
            if c is None: continue
            p=pos[i]; p["held"]+=1
            if c>p["peak"]: p["peak"]=c
            exit_px=None
            if trail:
                stop=p["peak"]-ATR_MULT*p["atr"]
                if be and p["peak"]>=p["entry"]*(1+BE_ARM):
                    stop=max(stop, p["entry"]*(1+BE_FLOOR))
                if c<=stop: exit_px=min(c,stop)
            else:
                stop=p["entry"]*(1-0.05)                       # plain 5% fixed stop
                if be and p["peak"]>=p["entry"]*(1+BE_ARM):
                    stop=max(stop, p["entry"]*(1+BE_FLOOR))
                if c<=stop: exit_px=min(c,stop)
            in_profit = c>p["entry"]
            if exit_px is None and p["held"]>=MAXHOLD:
                if not (skip_if_profit and in_profit): exit_px=c
            if exit_px is not None:
                cash += p["shares"]*exit_px*(1-COST)
                del pos[i]
        # entry
        if reg[t]!="WARMUP" and gate_ok(t):
            ig=[]
            for i in range(N):
                if i in pos: continue
                c,cp=C[t][i],C[t-1][i]
                if c is None or cp is None or cp<=0: continue
                ret=c/cp-1
                if side=="momo":
                    if ret<GATE: continue
                    lo=max(0,t-NEWHIGH_N+1)
                    w=[C[k][i] for k in range(lo,t+1) if C[k][i] is not None]
                    if not w or c<max(w): continue
                else:  # mr: buy weakness (down-day below threshold + near 20d low)
                    if ret>DROP: continue
                    lo=max(0,t-NEWHIGH_N+1)
                    w=[C[k][i] for k in range(lo,t+1) if C[k][i] is not None]
                    if not w or c>min(w): continue
                ig.append(i)
            if not breadth or len(ig)>=BREADTH:
                for i in ig:
                    dollars=CAP*RISK_FRAC
                    if cash<dollars: break
                    difs=[abs(C[k][i]-C[k-1][i]) for k in range(max(1,t-ATR_N+1),t+1)
                          if C[k][i] is not None and C[k-1][i] is not None]
                    if not difs: continue
                    e=C[t][i]; cash-=dollars*(1+COST)
                    pos[i]=dict(entry=e,peak=e,held=0,shares=dollars/e,
                                atr=sum(difs)/len(difs))
        # mark-to-market equity
        mtm=cash+sum(p["shares"]*C[t][i] for i,p in pos.items() if C[t][i] is not None)
        eq_curve.append(mtm)
    # liquidate
    for i,p in pos.items():
        if C[-1][i] is not None: cash+=p["shares"]*C[-1][i]*(1-COST)
    final=cash
    # stats on equity curve
    pk=eq_curve[0]; mdd=0
    for e in eq_curve:
        pk=max(pk,e); mdd=max(mdd,(pk-e)/pk)
    yrs=len(eq_curve)/252.0
    tot=final/CAP-1
    cagr=(final/CAP)**(1/yrs)-1 if yrs>0 and final>0 else -1
    rets=[eq_curve[k]/eq_curve[k-1]-1 for k in range(1,len(eq_curve))]
    mu=sum(rets)/len(rets); sd=(sum((r-mu)**2 for r in rets)/len(rets))**0.5
    sharpe=(mu/sd*math.sqrt(252)) if sd>0 else 0
    return dict(tot=tot*100, cagr=cagr*100, mdd=mdd*100, sharpe=sharpe,
                calmar=(cagr/mdd if mdd>0 else 0))

def line(label, s):
    print(f"  {label:<34} tot={s['tot']:+7.1f}%  CAGR={s['cagr']:+6.1f}%  "
          f"maxDD={s['mdd']:5.1f}%  Sharpe={s['sharpe']:4.2f}  Calmar={s['calmar']:4.2f}")

if __name__=="__main__":
    dates,names,C=load(F); reg=market_regime(C)
    print(f"data {len(names)} names {dates[0]}..{dates[-1]}  (real fixed-capital equity, ${CAP:,.0f})\n")
    print("A+B) MOMENTUM engine -- regime-gate modes (real maxDD):")
    line("no gate",            run(dates,names,C,reg,gate="none"))
    line("BULL-only (production)", run(dates,names,C,reg,gate="bull"))
    line("not-BEAR (relaxed: +NEUTRAL)", run(dates,names,C,reg,gate="notbear"))
    print("\n   Protection ablation (BULL-only gate, toggle one off):")
    line("full chassis",        run(dates,names,C,reg,gate="bull"))
    line("-breadth",            run(dates,names,C,reg,gate="bull",breadth=False))
    line("-ATR-trail (5% stop)",run(dates,names,C,reg,gate="bull",trail=False))
    line("-BE-ratchet",         run(dates,names,C,reg,gate="bull",be=False))
    line("-ride (cut winners@maxhold)", run(dates,names,C,reg,gate="bull",skip_if_profit=False))
    print("\nC) TRANSFER: same chassis on a MEAN-REVERSION engine (buy weakness):")
    line("MR no gate",          run(dates,names,C,reg,gate="none",side="mr"))
    line("MR + BULL gate",      run(dates,names,C,reg,gate="bull",side="mr"))
    line("MR + not-BEAR gate",  run(dates,names,C,reg,gate="notbear",side="mr"))
    line("MR no gate, no trail (MR-native exit)", run(dates,names,C,reg,gate="none",side="mr",trail=False))

#!/usr/bin/env python3
"""Short mirror of the Luke engine for BEAR regimes. Inside-day breakDOWN / pullback
to a falling EMA on LAG names (9<21<50), gated to a confirmed downtrend (market<200MA).
Tests whether shorting high-ADR names in the regime the long engine sits out adds a
profitable, additive use. Faithful short accounting."""
import sys, glob, os
import numpy as np, pandas as pd
import luke_bt as L
import luke_matrix as M

def gen_short(d, adr_min, stop_buf, max_stopw, min_stopw, touch_buf, modeA, modeC):
    sig=[]; h,l,c=d['high'].values,d['low'].values,d['close'].values
    e9,e21,e50=d['ema9'].values,d['ema21'].values,d['ema50'].values
    sl=d['ema21_slope'].values; adr=d['adr'].values
    for i in range(55,len(d)-1):
        if np.isnan(e50[i]) or np.isnan(adr[i]) or adr[i]<adr_min: continue
        lag = e9[i]<e21[i]<e50[i]; down = sl[i]<0
        cand=None
        # C-short: inside-day in downtrend -> break BELOW inside-day low
        if modeC and lag and down and c[i]<e21[i]:
            in1=h[i]<h[i-1] and l[i]>l[i-1]
            in2=in1 and h[i-1]<h[i-2] and l[i-1]>l[i-2]
            if in1 or in2: cand=('Cs',l[i],h[i]*(1+stop_buf))
        # A-short: pullback UP to a falling 9/21 EMA, close back below
        if cand is None and modeA and lag and down:
            near21=h[i]>=e21[i]*(1-touch_buf) and c[i]<e21[i]
            near9 =h[i]>=e9[i]*(1-touch_buf)  and c[i]<e9[i]
            if near21 or near9: cand=('As',l[i],max(h[i],e21[i] if near21 else e9[i])*(1+stop_buf))
        if cand is None: continue
        setup,trig,stop=cand
        if stop<=trig: continue
        sw=(stop-trig)/trig
        if sw<min_stopw or sw>max_stopw: continue
        sig.append(dict(i=i,date=d['date'].iloc[i],setup=setup,trig=trig,stop=stop,adr=adr[i]))
    return sig

def sim_short(univ, reg, P):
    alldates=sorted(set().union(*[set(d['date']) for d in univ.values()]))
    idx={t:{dt:k for k,dt in enumerate(d['date'])} for t,d in univ.items()}
    sigmap={}
    for t,d in univ.items():
        for s in gen_short(d,P['adr_min'],P['stop_buf'],P['max_stopw'],P['min_stopw'],P['touch_buf'],P['mA'],P['mC']):
            sigmap.setdefault(s['date'],[]).append((t,s))
    eq=cash=P['equity0']; cost=P['cost_bps']/1e4; slip=P['stop_slip_bps']/1e4
    pos={}; trades=[]; curve=[]
    for di,dt in enumerate(alldates):
        for t in list(pos.keys()):
            k=idx[t].get(dt)
            if k is None: continue
            d=univ[t]; o,hi,lo,cl,e9=d['open'][k],d['high'][k],d['low'][k],d['close'][k],d['ema9'][k]
            p=pos[t]
            # stop ABOVE (gap aware)
            if hi>=p['stop']:
                fill=(max(o,p['stop']) if o>p['stop'] else p['stop'])*(1+slip)
                p['pnl']+=p['sh']*(p['entry']-fill)-p['sh']*fill*cost; cash+=p['pnl']-p.get('booked',0);
                trades.append(_fin(p,'stop')); del pos[t]; continue
            # cover on close ABOVE 9EMA (ride the downtrend until it turns up)
            if not np.isnan(e9) and cl>e9:
                p['pnl']+=p['sh']*(p['entry']-cl)-p['sh']*cl*cost
                trades.append(_fin(p,'ema')); del pos[t]
        # mark-to-market (short unrealized = sh*(entry-close))
        mtm=cash+sum(p['sh']*(p['entry']-univ[t]['close'][idx[t][dt]]) for t,p in pos.items() if dt in idx[t])
        curve.append(mtm)
        prev=sigmap.get(alldates[di-1]) if di>0 else None
        if prev and len(pos)<P['max_concurrent']:
            r=reg.get(dt,'bull') if reg else 'bull'
            # SHORT only in BEAR regime (the gate the long engine uses to SIT OUT)
            if not P['bear_gate'] or r=='bear':
                cands=[(t,s,idx[t][dt]) for t,s in prev if t not in pos and dt in idx[t]]
                cands.sort(key=lambda x:(x[1]['stop']-x[1]['trig'])/x[1]['trig'])
                for t,s,k in cands:
                    if len(pos)>=P['max_concurrent']: break
                    d=univ[t]
                    if d['low'][k]>s['trig']: continue          # never broke down
                    fill=(d['open'][k] if d['open'][k]<s['trig'] else s['trig'])*(1-cost)
                    sw=s['stop']-fill
                    if sw<=0: continue
                    sh=int(eq*P['risk_pct']/sw);
                    if sh<=0: continue
                    pos[t]=dict(ticker=t,entry=fill,sh=sh,stop=s['stop'],setup=s['setup'],reg=r,
                                risk=eq*P['risk_pct'],pnl=0.0,date_in=dt)
        eq=curve[-1]
    for t,p in list(pos.items()):
        p['pnl']+=p['sh']*(p['entry']-univ[t]['close'].iloc[-1]); trades.append(_fin(p,'eod'))
    return trades, curve

def _fin(p,why): return dict(ticker=p['ticker'],setup=p['setup'],reg=p['reg'],date_in=p['date_in'],
                             pnl=p['pnl'],R=p['pnl']/p['risk'] if p['risk'] else 0)

def met(trades,curve,f=None):
    ts=[t for t in trades if f is None or f(t)]
    if not ts: return None
    pnl=np.array([t['pnl'] for t in ts]); w=pnl[pnl>0]; lo=pnl[pnl<0]
    eq=np.array(curve); peak=np.maximum.accumulate(eq)
    return dict(n=len(ts),pf=round(w.sum()/abs(lo.sum()),2) if lo.sum() else 9.9,
        wr=round(len(w)/len(ts)*100,1),ret=round((eq[-1]/eq[0]-1)*100,1),
        dd=round(((peak-eq)/peak).max()*100,1) if len(eq) else 0,net=round(pnl.sum()))

def run(pattern,label,**ov):
    u=M.load_glob(pattern)
    if not u: print(label,'(no data)'); return
    for d in u.values(): L.add_avwap(d)
    reg=M.build_regime_dict(u)
    P=dict(adr_min=3.0,stop_buf=0.003,max_stopw=0.06,min_stopw=0.005,touch_buf=0.015,
           mA=True,mC=True,cost_bps=6.0,stop_slip_bps=10.0,max_concurrent=5,risk_pct=0.01,
           bear_gate=True,equity0=100000.0); P.update(ov)
    tr,cv=sim_short(u,reg,P)
    a=met(tr,cv);
    if not a: print(f'  {label:32} (no shorts)'); return
    b=met(tr,cv,lambda t:str(t['date_in'])[:4]=='2022')
    print(f"  {label:32} n={a['n']:4} PF={a['pf']:5} ret={a['ret']:7}% DD={a['dd']:5} WR={a['wr']:5} | CY2022={b['pf'] if b else None}")

if __name__=='__main__':
    print('=== SHORT MIRROR (bear-gated) — does shorting high-ADR names in bear add a use? ===')
    print('-- CRYPTO 2019-2026 (real 2022 -65% bear) --')
    run('/tmp/luke_crypto/*.csv','crypto short C+A bear-gated',cost_bps=6,stop_slip_bps=10,adr_min=4.0)
    run('/tmp/luke_crypto/*.csv','crypto short C-only bear-gated',mA=False,cost_bps=6,stop_slip_bps=10,adr_min=4.0)
    print('-- INDEX 10yr 2016-2026 (2018/2020/2022 bears) --')
    run('/tmp/luke_idx10/*.csv','idx short C+A bear-gated',adr_min=0.8,max_stopw=0.04,cost_bps=2,stop_slip_bps=3)
    print('-- STOCKS 2024-2026 (limited bear) --')
    run('/tmp/luke_data/*.csv','stocks short C+A bear-gated',adr_min=4.0,cost_bps=2,stop_slip_bps=5)

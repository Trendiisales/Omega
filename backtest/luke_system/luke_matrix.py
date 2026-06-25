#!/usr/bin/env python3
"""TF-aware comprehensive runner: symbols x timeframes x entries x exits.
Regime is computed on a DAILY resample of the breadth index so the bull/bear
gate is consistent regardless of bar timeframe."""
import sys, json, glob, os
import numpy as np, pandas as pd
import luke_bt as L

def daily_regime(univ):
    """Equal-weight breadth index resampled to DAILY, close>SMA200 = bull."""
    acc={}
    for d in univ.values():
        s=d.set_index('date')['close']; s=s/s.iloc[0]
        day=s.resample('1D').last().dropna()
        for dt,v in day.items(): acc.setdefault(dt.normalize(),[]).append(v)
    idx=pd.Series({dt:np.mean(v) for dt,v in acc.items()}).sort_index()
    sma=idx.rolling(200).mean()
    reg=pd.Series(np.where(idx>sma,'bull','bear'),index=idx.index)
    # map every (possibly intraday) timestamp to its day's regime
    return reg

def regime_lookup(reg_daily):
    d={k.normalize():v for k,v in reg_daily.items()}
    def f(ts):
        return d.get(pd.Timestamp(ts).normalize(),'bull')
    return d  # we build a per-date dict below instead

def build_regime_dict(univ):
    reg=daily_regime(univ)
    dmap={k.normalize():v for k,v in reg.items()}
    # expand to every timestamp present in the data
    out={}
    for d in univ.values():
        for ts in d['date']:
            out[ts]=dmap.get(pd.Timestamp(ts).normalize(),'bull')
    return out

BASE=dict(entry_modes=['A','C'],touch_buf=0.015,base_buf=0.06,stop_buf=0.003,
    avwap_band=0.02,cluster_band=0.02,cluster_min=2,min_stopw=0.001,
    max_stopw=0.06,risk_pct=0.01,max_pos_pct=0.35,max_concurrent=5,
    cost_bps=6.0,stop_slip_bps=10.0,partial_frac=0.15,partial_mult=3.0,
    partial_step=2.0,max_partials=3,be_after_partial=True,trail_ema=True,
    time_stop=0,regime_gate=False,equity0=100000.0,adr_min=3.0)

def load_glob(pattern):
    u={}
    for p in sorted(glob.glob(pattern)):
        if os.path.basename(p).startswith('_'): continue
        d=L.load_one(p)
        if d is not None: u[os.path.basename(p)[:-4]]=d
    return u

def run(pattern, P, label):
    u=load_glob(pattern)
    if not u: return None
    for d in u.values(): L.add_avwap(d)
    reg=build_regime_dict(u)
    pp=dict(BASE); pp.update(P)
    tr,eqc=L.simulate(u,reg,pp)
    def m(f=None): return L.metrics(tr,eqc,f)
    a=m()
    if not a: return None
    out=dict(label=label,n=a['n'],pf=a['pf'],ret=a['ret'],dd=a['maxDD'],wr=a['wr'],avgR=a['avgR'],
        bull=(m(lambda t:t['reg']=='bull') or {}).get('pf'),
        bear=(m(lambda t:t['reg']=='bear') or {}).get('pf'),
        cy2022=(m(lambda t:str(t['date_in'])[:4]=='2022') or {}).get('pf'),
        A=(m(lambda t:t['setup']=='A') or {}).get('pf'),
        C=(m(lambda t:t['setup']=='C') or {}).get('pf'))
    return out

def pr(o):
    if not o: print('  (no trades)'); return
    print(f"  {o['label']:34} n={o['n']:5} PF={o['pf']:5} ret={o['ret']:8}% DD={o['dd']:5} WR={o['wr']:5} "
          f"bull={o['bull']} bear={o['bear']} CY22={o['cy2022']} | A={o['A']} C={o['C']}")

if __name__=='__main__':
    pass

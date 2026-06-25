#!/usr/bin/env python3
"""Lever sweep over the Luke-system engine. Prepares universe once, runs the grid,
ranks by robustness (both-regime + both-half positive), prints leaderboard."""
import itertools, json, sys, copy
import numpy as np
import luke_bt as L

BASE=dict(entry_modes=['A','B','C'],touch_buf=0.015,adr_min=4.0,base_buf=0.06,
    stop_buf=0.003,avwap_band=0.02,cluster_band=0.02,cluster_min=2,
    min_stopw=0.005,max_stopw=0.12,risk_pct=0.005,max_pos_pct=0.35,
    max_concurrent=8,cost_bps=2.0,stop_slip_bps=5.0,partial_frac=0.15,
    partial_mult=3.0,partial_step=2.0,max_partials=3,be_after_partial=True,
    trail_ema=False,time_stop=0,regime_gate=False,equity0=100000.0)

print('loading universe...',file=sys.stderr)
UNIV=L.load_universe(); REG=L.load_regime(UNIV)
for d in UNIV.values(): L.add_avwap(d)
print(f'universe={len(UNIV)} regime={"on" if REG else "NONE"}',file=sys.stderr)

def split_metrics(trades, eqc):
    """overall + bull/bear + first/second chronological half."""
    out={}
    out['ALL']=L.metrics(trades,eqc)
    out['bull']=L.metrics(trades,eqc,lambda t:t['reg']=='bull')
    out['bear']=L.metrics(trades,eqc,lambda t:t['reg']=='bear')
    ts=sorted(trades,key=lambda t:t['date_in'])
    if len(ts)>=8:
        mid=ts[len(ts)//2]['date_in']
        out['h1']=L.metrics([t for t in ts if t['date_in']<mid],eqc)
        out['h2']=L.metrics([t for t in ts if t['date_in']>=mid],eqc)
    return out

def run(P):
    pp=dict(BASE); pp.update(P)
    tr,eqc=L.simulate(UNIV,REG,pp)
    return split_metrics(tr,eqc), len(tr)

def robust_score(m):
    """positive PF in ALL/bull/bear/h1/h2 -> robust. Score = min PF across regimes*halves."""
    keys=['ALL','bull','bear','h1','h2']
    pfs=[]
    for k in keys:
        if m.get(k) is None: return -1, False
        pfs.append(m[k]['pf'])
    allpos=all(p>1.0 for p in pfs)
    return round(min(pfs),2), allpos

if __name__=='__main__':
    mode=sys.argv[1] if len(sys.argv)>1 else 'modes'
    results=[]
    if mode=='modes':
        grid=[['A'],['B'],['C'],['A','C'],['B','C'],['A','B'],['A','B','C']]
        for em in grid:
            m,n=run({'entry_modes':em})
            results.append((','.join(em),m,n))
        print('=== ENTRY MODE COMPARISON (base levers) ===')
        for name,m,n in results:
            a=m['ALL']; sc,rob=robust_score(m)
            print(f"{name:8} n={n:4} ALL_PF={a['pf']:5} WR={a['wr']:4} avgR={a['avgR']:5} "
                  f"ret={a['ret']:6} DD={a['maxDD']:5} | "
                  f"bull={m['bull']['pf'] if m['bull'] else 'NA'} "
                  f"bear={m['bear']['pf'] if m['bear'] else 'NA'} "
                  f"minPF={sc} robust={rob}")
    elif mode=='full':
        grid=dict(
            risk_pct=[0.005,0.0075,0.01],
            adr_min=[3.0,4.0,5.0,6.0],
            max_pos_pct=[0.25,0.35,0.50],
            max_concurrent=[5,8,12],
            max_stopw=[0.06,0.08,0.12],
            be_after_partial=[True,False],
            trail_ema=[True,False],
            regime_gate=[True,False],
        )
        keys=list(grid); combos=list(itertools.product(*[grid[k] for k in keys]))
        print(f'running {len(combos)} configs on entry_modes=A,C ...',file=sys.stderr)
        rows=[]
        for c in combos:
            P={'entry_modes':['A','C']}; P.update(dict(zip(keys,c)))
            m,n=run(P)
            sc,rob=robust_score(m)
            a=m['ALL']
            rows.append(dict(P={k:P[k] for k in keys},n=n,ret=a['ret'],dd=a['maxDD'],
                pf=a['pf'],minpf=sc,robust=rob,
                bull=m['bull']['pf'] if m['bull'] else 0,
                bear=m['bear']['pf'] if m['bear'] else 0,
                rar=round(a['ret']/a['maxDD'],2) if a['maxDD']>0 else 0))
        rob=[r for r in rows if r['robust']]
        print(f"\n{len(rob)}/{len(rows)} configs robust (PF>1 across ALL/bull/bear/h1/h2)\n")
        def show(title,lst):
            print(f"=== {title} ===")
            for r in lst:
                p=r['P']
                print(f"ret={r['ret']:6} DD={r['dd']:5} RAR={r['rar']:5} PF={r['pf']:4} "
                      f"minPF={r['minpf']:4} bull={r['bull']:4} bear={r['bear']:4} n={r['n']:4} | "
                      f"risk={p['risk_pct']} adr={p['adr_min']} pos={p['max_pos_pct']} "
                      f"conc={p['max_concurrent']} stopw={p['max_stopw']} be={int(p['be_after_partial'])} "
                      f"trail={int(p['trail_ema'])} gate={int(p['regime_gate'])}")
        show("TOP 12 ROBUST by RETURN", sorted(rob,key=lambda r:-r['ret'])[:12])
        show("TOP 8 ROBUST by RISK-ADJ (ret/DD)", sorted(rob,key=lambda r:-r['rar'])[:8])
        show("TOP 5 ALL by RETURN (robust or not)", sorted(rows,key=lambda r:-r['ret'])[:5])
        json.dump(rows,open('/tmp/luke_sweep_results.json','w'),default=str)
        print('\nsaved /tmp/luke_sweep_results.json')
